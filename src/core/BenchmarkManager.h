#pragma once
#include "BenchmarkRunner.h"
#include "Log.h"
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
//
// For fixed-work tests (IBenchmarkTest::RunsToCompletion(), e.g. disk I/O)
// durationSeconds is only a safety time limit: the run ends when the test
// has done all its work, however long that takes on the hardware at hand.
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
        {
            // The stop flag must change under the same mutex the worker
            // holds while checking its wait predicate. Setting it outside
            // the lock allows a lost wake-up: the worker checks the
            // predicate (false), we set the flag and notify, and only then
            // does the worker start waiting, so it sleeps forever and the
            // join below never returns.
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_stop.store(true);
            m_cancelCurrent.store(true);
        }
        m_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    // Enqueue a fully-configured test instance directly (e.g. a
    // RamBandwidthTest constructed with a non-default buffer size).
    void Enqueue(std::shared_ptr<IBenchmarkTest> prototype, RunMode mode,
                 double durationSeconds, unsigned threadCountOverride = 0) {
        if (!prototype) {
            // A job with nothing to run must never be counted as pending,
            // or IsIdle() would never become true again.
            LogWarn("Run", "Ignored a request to run a test that doesn't exist.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queue.push_back({std::move(prototype), mode, durationSeconds, threadCountOverride});
            m_pendingJobs.fetch_add(1);
        }
        m_cv.notify_all();
    }

    // Convenience overload: run a registered test with its default
    // configuration (what TestRegistry's factory produces).
    void Enqueue(size_t testIndex, RunMode mode, double durationSeconds, unsigned threadCountOverride = 0) {
        // Create() returns nullptr for an unknown index; the overload above
        // then logs and ignores it.
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
        size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            dropped = m_queue.size();
            m_queue.clear();
            m_pendingJobs.fetch_sub(dropped);
            m_cancelCurrent.store(true);
        }
        if (dropped > 0 || IsBusy())
            LogInfo("Run", "Cancel requested: %zu queued job%s removed, stopping the current run.",
                    dropped, dropped == 1 ? "" : "s");
    }

    bool IsBusy() const {
        return m_busy.load(std::memory_order_relaxed);
    }

    // True when nothing is queued AND nothing is running. Unlike
    // `!IsBusy() && QueueSize() == 0`, this has no gap in the instant
    // between a job leaving the queue and being marked busy, so it's the
    // right check for "start the next thing once everything has drained".
    bool IsIdle() const {
        return m_pendingJobs.load() == 0;
    }

    // Name of the test currently running, empty if idle.
    std::string CurrentlyRunning() const {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_currentTestName;
    }

    // Snapshot of the currently-running job, for the status bar.
    // elapsedSeconds is computed here (now - job start) rather than
    // pushed incrementally from the worker thread, since BenchmarkRunner's
    // timed loop runs to completion without checkpointing progress back
    // out. Time-boxed tests show "elapsed / duration"; fixed-work tests
    // (fixedWork == true) have no meaningful duration, so the UI shows the
    // elapsed timer plus fraction/phase when the test can report them.
    struct RunProgress {
        bool busy = false;
        std::string testName;
        double elapsedSeconds = 0.0;
        double durationSeconds = 0.0;
        bool fixedWork = false;
        double fraction = -1.0;  // 0..1, or negative if unknown
        std::string phase;       // e.g. "Preparing test file"; may be empty
    };

    RunProgress GetCurrentProgress() const {
        RunProgress p;
        std::shared_ptr<IBenchmarkTest> proto;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            p.busy = !m_currentTestName.empty();
            if (!p.busy) return p;
            p.testName = m_currentTestName;
            p.durationSeconds = m_currentJobDuration;
            p.fixedWork = m_currentFixedWork;
            p.elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_currentJobStart).count();
            proto = m_currentPrototype;
        }
        // Outside the lock: the test's own progress accessor is thread-safe
        // and may take its own locks.
        if (proto) {
            TestProgress tp = proto->GetProgress();
            p.fraction = tp.fraction;
            p.phase = std::move(tp.phase);
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
            if (!haveJob) continue;
            if (!job.prototype) { // can't happen via Enqueue(), but never leak the pending count
                m_pendingJobs.fetch_sub(1);
                continue;
            }
            try {
                RunJob(job);
            } catch (...) {
                // Last resort (e.g. out of memory while building an error
                // message). RunJob's scope guard has already done the
                // bookkeeping; keep the worker alive for the next job.
            }
        }
    }

    // Runs one job to completion and publishes its result. Nothing in
    // here is allowed to leave this function by throwing: an exception
    // escaping the worker thread would terminate the whole process, and
    // skipping the bookkeeping would leave the manager "busy" forever.
    void RunJob(QueuedRun& job) {
        // Bookkeeping that must happen however this function exits.
        struct JobScope {
            BenchmarkManager& m;
            explicit JobScope(BenchmarkManager& mgr) : m(mgr) {}
            ~JobScope() {
                {
                    std::lock_guard<std::mutex> lock(m.m_stateMutex);
                    m.m_currentTestName.clear();
                    m.m_currentPrototype.reset();
                }
                m.m_busy.store(false);
                m.m_pendingJobs.fetch_sub(1); // after the result is published (see below)
            }
        };

        JobScope scope(*this); // destroyed last, i.e. after the result below is published

        std::string name = "Unknown test";
        BenchmarkResult result;
        try {
            IBenchmarkTest& test = *job.prototype;
            name = test.GetName();
            const bool fixedWork = test.RunsToCompletion();
            const char* modeLabel = job.mode == RunMode::SingleCore ? "single-core" : "multi-core";

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_currentTestName = name;
                m_currentJobDuration = job.durationSeconds;
                m_currentFixedWork = fixedWork;
                m_currentPrototype = job.prototype;
                m_currentJobStart = std::chrono::steady_clock::now();
            }
            m_busy.store(true);
            if (fixedWork) {
                LogInfo("Run", "Starting %s (%s). This test runs until its work is done, so the time it takes depends on your hardware.",
                        name.c_str(), modeLabel);
            } else if (job.mode == RunMode::MultiCore && job.threadCountOverride > 0) {
                LogInfo("Run", "Starting %s (%s, %u threads, %.0f s)...", name.c_str(), modeLabel,
                        job.threadCountOverride, job.durationSeconds);
            } else {
                LogInfo("Run", "Starting %s (%s, %.0f s)...", name.c_str(), modeLabel, job.durationSeconds);
            }

            result = (job.mode == RunMode::SingleCore)
                ? BenchmarkRunner::RunSingleCore(test, job.durationSeconds, &m_cancelCurrent)
                : BenchmarkRunner::RunMultiCore(test, job.durationSeconds, job.threadCountOverride, &m_cancelCurrent);

            // Log the outcome *before* publishing the result, so the log
            // never lags behind what the Results tab shows.
            if (result.failed) {
                LogError("Run", "%s failed: %s", name.c_str(),
                         result.notes.empty() ? "unknown error" : result.notes.c_str());
            } else if (result.cancelled) {
                LogInfo("Run", "%s cancelled after %.1f s.", name.c_str(), result.elapsedSeconds);
            } else {
                LogInfo("Run", "%s finished in %.1f s: %s", name.c_str(), result.elapsedSeconds,
                        FormatScore(result).c_str());
                if (!result.notes.empty())
                    LogInfo("Run", "%s: %s", name.c_str(), result.notes.c_str());
                if (fixedWork) {
                    TestProgress progress = test.GetProgress();
                    if (progress.fraction >= 0.0 && progress.fraction < 0.999)
                        LogWarn("Run", "%s hit its safety time limit (%.0f s) before finishing; the score reflects the part that completed.",
                                name.c_str(), job.durationSeconds);
                }
                if (!result.affinityPinned && result.mode != RunMode::Gpu)
                    LogWarn("Run", "%s: the OS refused to pin threads to cores, so this score may carry some scheduler noise.",
                            name.c_str());
            }
        } catch (const std::exception& e) {
            result = BenchmarkResult();
            result.testName = name;
            result.mode = job.mode;
            result.failed = true;
            result.notes = std::string("Unexpected error: ") + e.what();
            LogError("Run", "%s failed: %s", name.c_str(), result.notes.c_str());
        } catch (...) {
            result = BenchmarkResult();
            result.testName = name;
            result.mode = job.mode;
            result.failed = true;
            result.notes = "Unexpected error.";
            LogError("Run", "%s failed: %s", name.c_str(), result.notes.c_str());
        }

        try {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_results.push_back(std::move(result));
        } catch (...) {
            // Out of memory while publishing: nothing more can be done, but
            // the bookkeeping in `scope` must still run and the thread must live.
        }
    }

    static std::string FormatScore(const BenchmarkResult& r) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%.2f %s (%d thread%s)",
                 r.score, r.unit.c_str(), r.threadsUsed, r.threadsUsed == 1 ? "" : "s");
        return buf;
    }

    std::thread m_worker;
    std::atomic<bool> m_stop;
    std::atomic<bool> m_cancelCurrent{false};
    std::atomic<bool> m_busy{false};
    std::atomic<size_t> m_pendingJobs{0}; // queued + running

    mutable std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::deque<QueuedRun> m_queue;

    mutable std::mutex m_stateMutex;
    std::string m_currentTestName;
    std::chrono::steady_clock::time_point m_currentJobStart;
    double m_currentJobDuration = 0.0;
    bool m_currentFixedWork = false;
    std::shared_ptr<IBenchmarkTest> m_currentPrototype;

    std::mutex m_resultsMutex;
    std::vector<BenchmarkResult> m_results;
};

} // namespace brazen
