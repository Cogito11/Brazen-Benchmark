#pragma once
#include "BenchmarkRunner.h"
#include "TestRegistry.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace brazen {

// Describes one queued unit of work: run the given test prototype in the
// given mode, for durationSeconds, with an optional thread count override
// for multi-core runs (0 = auto-detect, the default).
//
// The prototype is a fully-configured IBenchmarkTest instance, not
// just a TestRegistry index, so a caller that needs non-default
// settings (e.g. the RAM test's buffer size) can construct exactly the
// instance it wants and hand it over directly; BenchmarkRunner clones it
// per worker thread via IBenchmarkTest::Clone() the same way it always
// has. Enqueue(testIndex, ...) below is a convenience overload for the
// common case of "just run this registered test with its defaults".
//
// durationSeconds is captured here, at enqueue time, rather than read
// from a shared field on BenchmarkManager at the moment a job starts
// running. That distinction matters once different categories can have
// different durations (e.g. a CPU run configured for 8s, a RAM run
// configured for 15s): if duration lived in one shared mutable field,
// enqueueing a second batch with a different duration before the first
// batch finished draining would silently change the duration of
// already-queued-but-not-yet-started jobs out from under them.
struct QueuedRun {
    std::shared_ptr<IBenchmarkTest> prototype;
    RunMode mode;
    double durationSeconds;
    unsigned threadCountOverride = 0;
};

// Owns a single background worker thread that drains a queue of
// QueuedRun requests one at a time, so the UI thread never blocks.
// The UI polls GetResults()/IsBusy()/CurrentlyRunning() each frame.
// Thread-safety: all public methods are safe to call from the UI thread
// while the worker thread is running.
class BenchmarkManager {
public:
    BenchmarkManager() : m_stop(false) {
        m_worker = std::thread([this] { WorkerLoop(); });
    }

    ~BenchmarkManager() {
        m_stop.store(true);
        m_cancelCurrent.store(true);
        m_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    // Enqueue a fully-configured test instance directly (e.g. a
    // RamBandwidthTest constructed with a non-default buffer size).
    void Enqueue(std::shared_ptr<IBenchmarkTest> prototype, RunMode mode,
                 double durationSeconds, unsigned threadCountOverride = 0) {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queue.push_back({std::move(prototype), mode, durationSeconds, threadCountOverride});
        }
        m_cv.notify_all();
    }

    // Convenience overload: run a registered test with its default
    // configuration (what TestRegistry's factory produces).
    void Enqueue(size_t testIndex, RunMode mode, double durationSeconds, unsigned threadCountOverride = 0) {
        Enqueue(std::shared_ptr<IBenchmarkTest>(TestRegistry::Instance().Create(testIndex)),
                mode, durationSeconds, threadCountOverride);
    }

    void EnqueueAll(RunMode mode, double durationSeconds) {
        auto& reg = TestRegistry::Instance();
        for (size_t i = 0; i < reg.Count(); ++i) {
            if (reg.Samples()[i]->IsAvailable())
                Enqueue(i, mode, durationSeconds);
        }
    }

    void CancelAll() {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
        m_cancelCurrent.store(true);
    }

    bool IsBusy() const {
        return m_busy.load(std::memory_order_relaxed);
    }

    // Name of the test currently running, empty if idle.
    std::string CurrentlyRunning() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_currentTestName;
    }

    // Snapshot of the currently-running job's timing, for a countdown
    // display. elapsedSeconds is computed here (now - job start) rather
    // than pushed incrementally from the worker thread, since
    // BenchmarkRunner's timed loop runs to completion without
    // checkpointing progress back out. This way the UI thread can
    // still show a live countdown just by polling wall-clock time
    // against a start point recorded once, right before the blocking
    // run call begins.
    struct RunProgress {
        bool busy = false;
        std::string testName;
        double elapsedSeconds = 0.0;
        double durationSeconds = 0.0;
    };

    RunProgress GetCurrentProgress() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        RunProgress p;
        p.busy = !m_currentTestName.empty();
        if (p.busy) {
            p.testName = m_currentTestName;
            p.durationSeconds = m_currentJobDuration;
            p.elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_currentJobStart).count();
        }
        return p;
    }

    size_t QueueSize() const {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_queue.size();
    }

    // Pops and returns all results produced since the last call.
    std::vector<BenchmarkResult> DrainResults() {
        std::lock_guard<std::mutex> lock(m_resultsMutex);
        std::vector<BenchmarkResult> out;
        out.swap(m_results);
        return out;
    }

    // Pops and returns log lines produced since the last call (test
    // start/finish messages, etc.), for a console-style UI panel.
    std::vector<std::string> DrainLog() {
        std::lock_guard<std::mutex> lock(m_logMutex);
        std::vector<std::string> out;
        out.swap(m_log);
        return out;
    }

private:
    void WorkerLoop() {
        while (!m_stop.load()) {
            QueuedRun job;
            bool haveJob = false;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_cv.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
                if (m_stop.load()) break;
                if (!m_queue.empty()) {
                    job = m_queue.front();
                    m_queue.pop_front();
                    haveJob = true;
                    // Reset the cancel flag while still holding the same
                    // lock CancelAll() takes to set it. This matters:
                    // resetting it after releasing the lock left a window
                    // where a CancelAll() call landing between "job
                    // popped" and "flag reset" would have its cancel
                    // request silently overwritten by this reset, so a
                    // "Cancel All" click could fail to actually cancel
                    // the run that was just about to start. Doing both
                    // under the same lock makes the two operations
                    // strictly ordered: either CancelAll() fully
                    // happens-before this reset (and the reset correctly
                    // wins, since the cancelled job was already gone from
                    // the queue by the time CancelAll ran) or fully
                    // happens-after it (and CancelAll's true correctly
                    // sticks for the job that's about to run).
                    m_cancelCurrent.store(false);
                }
            }
            if (!haveJob || !job.prototype) continue;

            IBenchmarkTest& test = *job.prototype;

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_currentTestName = test.GetName();
                m_currentJobDuration = job.durationSeconds;
                m_currentJobStart = std::chrono::steady_clock::now();
            }
            m_busy.store(true);
            PushLog(std::string("Running ") + test.GetName() +
                     (job.mode == RunMode::SingleCore ? " (single-core)..." : " (multi-core)..."));

            BenchmarkResult result = (job.mode == RunMode::SingleCore)
                ? BenchmarkRunner::RunSingleCore(test, job.durationSeconds, &m_cancelCurrent)
                : BenchmarkRunner::RunMultiCore(test, job.durationSeconds, job.threadCountOverride, &m_cancelCurrent);

            {
                std::lock_guard<std::mutex> lock(m_resultsMutex);
                m_results.push_back(result);
            }
            PushLog(result.cancelled
                ? (test.GetName() + " cancelled.")
                : (test.GetName() + " finished: " + FormatScore(result)));

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_currentTestName.clear();
            }
            m_busy.store(false);
        }
    }

    static std::string FormatScore(const BenchmarkResult& r) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%.2f %s (%d thread%s)%s",
                 r.score, r.unit.c_str(), r.threadsUsed, r.threadsUsed == 1 ? "" : "s",
                 r.affinityPinned ? "" : " [unpinned: core affinity not supported here]");
        return buf;
    }

    void PushLog(std::string line) {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_log.push_back(std::move(line));
    }

    std::thread m_worker;
    std::atomic<bool> m_stop;
    std::atomic<bool> m_cancelCurrent{false};
    std::atomic<bool> m_busy{false};

    mutable std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::deque<QueuedRun> m_queue;

    mutable std::mutex m_stateMutex;
    std::string m_currentTestName;
    std::chrono::steady_clock::time_point m_currentJobStart;
    double m_currentJobDuration = 0.0;

    std::mutex m_resultsMutex;
    std::vector<BenchmarkResult> m_results;

    std::mutex m_logMutex;
    std::vector<std::string> m_log;
};

} // namespace brazen
