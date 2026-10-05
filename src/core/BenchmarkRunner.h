#pragma once
#include "IBenchmarkTest.h"
#include "Log.h"
#include "ThreadAffinity.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace brazen {

enum class RunMode { SingleCore, MultiCore, Gpu };

struct BenchmarkResult {
    std::string testName;
    std::string unit;
    RunMode mode = RunMode::SingleCore;
    int threadsUsed = 1;
    double score = 0.0;
    double elapsedSeconds = 0.0;
    uint64_t totalOps = 0;
    bool cancelled = false;

    // The run couldn't produce a meaningful score (e.g. the disk test
    // couldn't create its temporary file). `notes` carries the reason.
    // Failed results are logged and shown, but never feed the composite score.
    bool failed = false;

    // Optional human-readable detail from the test (how it ran, warnings).
    std::string notes;

    // Whether every worker thread's core-pinning request was accepted by
    // the OS. If false, at least one thread ran unpinned and the score
    // may carry extra scheduler-induced noise (see ThreadAffinity.h).
    bool affinityPinned = false;
};

// Runs a single IBenchmarkTest either on one thread ("single core") or
// across N threads ("multi core"), each running independent clones of the
// test concurrently for a fixed duration, then aggregates throughput.
//
// Reliability guarantees (each is covered by tests/SelfTest.cpp):
//  * Nothing a test does can take the process down: exceptions from
//    Clone()/Setup()/RunWorkChunk() (bad_alloc above all) become a result
//    with `failed == true` and the reason in `notes`.
//  * All worker threads finish Setup() before any of them starts its
//    timed loop, so every thread is measured over the same window. (A
//    thread that started early would otherwise run alone for a while, with
//    no contention, and inflate aggregate figures such as RAM bandwidth.)
//  * `cancelled` is true only if a worker actually observed the cancel
//    request, so a run that finished all its work isn't mislabelled just
//    because Cancel was clicked a moment later.
//  * A run that completes without doing any work is a failure, not a
//    plausible-looking zero score.
//  * Nonsensical durations (NaN, negative, huge) are clamped instead of
//    overflowing the clock arithmetic.
class BenchmarkRunner {
public:
    // Longest time-box the runner will accept; also the cap applied when a
    // caller passes something unusable.
    static constexpr double kMaxDurationSeconds = 24.0 * 60.0 * 60.0;
    static constexpr double kFallbackDurationSeconds = 1.0;

    // durationSeconds: how long the timed portion of the run should last
    // (for fixed-work tests -- see IBenchmarkTest::RunsToCompletion() --
    // this is only a safety time limit; they end when their work is done).
    // cancel: optional flag the caller can set from another thread to stop early.
    static BenchmarkResult RunSingleCore(const IBenchmarkTest& test,
                                          double durationSeconds,
                                          std::atomic<bool>* cancel = nullptr) {
        return RunInternal(test, durationSeconds, /*threadCount=*/1, RunMode::SingleCore, cancel);
    }

    // threadCount == 0 means "one thread per core this process may use"
    // (see AllowedCores()), which can be fewer than the machine's total.
    static BenchmarkResult RunMultiCore(const IBenchmarkTest& test,
                                         double durationSeconds,
                                         unsigned threadCount,
                                         std::atomic<bool>* cancel = nullptr) {
        unsigned n = threadCount == 0 ? static_cast<unsigned>(AllowedCores().size()) : threadCount;
        if (n == 0) n = 1;
        return RunInternal(test, durationSeconds, n, RunMode::MultiCore, cancel);
    }

private:
    using clock = std::chrono::steady_clock;

    static clock::duration SanitizedDuration(double seconds) {
        if (!(seconds > 0.0)) { // NaN, zero or negative
            LogWarn("Run", "Invalid run duration (%g s); using %.0f s instead.", seconds,
                    kFallbackDurationSeconds);
            seconds = kFallbackDurationSeconds;
        }
        if (seconds > kMaxDurationSeconds) seconds = kMaxDurationSeconds; // also catches +infinity
        return std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(seconds));
    }

    // State shared by the calling thread and its workers for one run.
    struct RunState {
        std::mutex mutex;
        std::condition_variable cv;
        unsigned ready = 0;   // workers that have finished (or abandoned) Setup()
        bool go = false;      // released once every started worker is ready
        std::string error;    // first failure message, if any
        std::atomic<bool> failed{false};
        std::atomic<bool> cancelObserved{false};
        std::atomic<bool> allPinned{true};
    };

    static void RecordFailure(RunState& st, const std::string& message) {
        std::lock_guard<std::mutex> lock(st.mutex);
        if (!st.failed.load()) {
            st.error = message;
            st.failed.store(true);
        }
    }

    static std::string Describe(const std::exception& e) {
        std::string what = e.what();
        // bad_alloc's what() is the unhelpful "std::bad_alloc".
        if (dynamic_cast<const std::bad_alloc*>(&e)) return "Out of memory.";
        return what.empty() ? "Unknown error." : what;
    }

    static BenchmarkResult RunInternal(const IBenchmarkTest& test,
                                        double durationSeconds,
                                        unsigned threadCount,
                                        RunMode mode,
                                        std::atomic<bool>* cancel) {
        BenchmarkResult result;
        result.testName = test.GetName();
        result.unit = test.GetUnit();
        result.mode = mode;
        result.threadsUsed = static_cast<int>(threadCount);

        const clock::duration runDuration = SanitizedDuration(durationSeconds);
        RunState st;

        std::vector<std::unique_ptr<IBenchmarkTest>> instances;
        try {
            instances.reserve(threadCount);
            for (unsigned i = 0; i < threadCount; ++i) instances.push_back(test.Clone());
        } catch (const std::exception& e) {
            RecordFailure(st, "Couldn't set up the test: " + Describe(e));
        } catch (...) {
            RecordFailure(st, "Couldn't set up the test: unknown error.");
        }
        if (st.failed.load()) {
            result.failed = true;
            result.notes = st.error;
            return result;
        }

        std::vector<uint64_t> perThreadOps(threadCount, 0);
        std::vector<double> perThreadElapsed(threadCount, 0.0);
        const std::vector<unsigned>& cores = AllowedCores();

        auto threadBody = [&](unsigned idx) {
            IBenchmarkTest& t = *instances[idx];
            bool setupOk = true;
            try {
                // Single-core runs pin to the highest-numbered allowed core
                // rather than the first: core 0 typically fields more
                // OS/interrupt traffic, which adds noise to a single-thread
                // measurement. Multi-core runs pin thread i to the i-th
                // allowed core so every usable core actually gets used
                // instead of the scheduler load-balancing onto a subset.
                unsigned coreToPin = (threadCount == 1) ? cores.back() : cores[idx % cores.size()];
                if (!PinCurrentThreadToCore(coreToPin))
                    st.allPinned.store(false, std::memory_order_relaxed);

                t.SetCancelFlag(cancel);
                t.Setup();
            } catch (const std::exception& e) {
                RecordFailure(st, "Setup failed: " + Describe(e));
                setupOk = false;
            } catch (...) {
                RecordFailure(st, "Setup failed: unknown error.");
                setupOk = false;
            }

            // Wait until every worker has finished Setup() (which can be
            // slow and uneven, e.g. each RAM thread allocating and filling
            // large buffers) and only then start timing, together.
            {
                std::unique_lock<std::mutex> lock(st.mutex);
                ++st.ready;
                st.cv.notify_all();
                st.cv.wait(lock, [&] { return st.go; });
            }

            if (cancel && cancel->load(std::memory_order_relaxed)) st.cancelObserved.store(true);
            if (!setupOk) return;

            // The deadline is computed here, after the gate, so Setup()
            // time never counts against the timed portion.
            auto loopStart = clock::now();
            auto deadline = loopStart + runDuration;
            uint64_t ops = 0;
            try {
                // IsComplete() is always false for time-boxed tests, so for
                // them this is the plain "run until the deadline" loop.
                // Fixed-work tests (e.g. disk) stop as soon as they've done
                // all their work; for those, `deadline` is a safety cap.
                // A failure on any thread stops the others promptly.
                while (!t.IsComplete() && !st.failed.load(std::memory_order_relaxed) &&
                       clock::now() < deadline) {
                    if (cancel && cancel->load(std::memory_order_relaxed)) {
                        st.cancelObserved.store(true);
                        break;
                    }
                    ops += t.RunWorkChunk();
                }
            } catch (const std::exception& e) {
                RecordFailure(st, "The test failed while running: " + Describe(e));
            } catch (...) {
                RecordFailure(st, "The test failed while running: unknown error.");
            }
            auto loopEnd = clock::now();

            perThreadOps[idx] = ops;
            perThreadElapsed[idx] = std::chrono::duration<double>(loopEnd - loopStart).count();
        };

        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (unsigned i = 0; i < threadCount; ++i) {
            try {
                workers.emplace_back(threadBody, i);
            } catch (const std::exception& e) {
                // Couldn't start thread i (out of threads/memory). The ones
                // already running must still be released and joined.
                RecordFailure(st, "Couldn't start worker thread " + std::to_string(i + 1) + " of " +
                                      std::to_string(threadCount) + ": " + Describe(e));
                break;
            }
        }
        {
            std::unique_lock<std::mutex> lock(st.mutex);
            st.cv.wait(lock, [&] { return st.ready >= workers.size(); });
            st.go = true;
        }
        st.cv.notify_all();
        for (auto& w : workers) w.join();

        // Average of each thread's own timed-portion duration -- not
        // wall-clock time across thread creation/join, which would still
        // include Setup() cost and quietly deflate the score.
        double totalElapsed = 0.0;
        for (double e : perThreadElapsed) totalElapsed += e;
        result.elapsedSeconds = threadCount > 0 ? totalElapsed / threadCount : 0.0;

        uint64_t totalOps = 0;
        for (auto o : perThreadOps) totalOps += o;
        result.totalOps = totalOps;
        result.cancelled = st.cancelObserved.load();
        result.affinityPinned = st.allPinned.load(std::memory_order_relaxed);

        // Failures: the runner's own (exceptions, thread creation) take
        // precedence; otherwise ask the test (error state shared by all
        // clones lives on the prototype, see DiskIoTest).
        if (st.failed.load()) {
            result.failed = true;
            result.notes = st.error;
        } else {
            result.failed = test.HasFailed();
            result.notes = test.StatusNote();
            if (!result.failed && !result.cancelled && totalOps == 0) {
                result.failed = true;
                result.notes = "The test finished without doing any work.";
            }
        }

        // A failed run has no meaningful score. A cancelled run keeps the
        // score for the part that completed.
        result.score = result.failed ? 0.0 : test.ComputeScore(totalOps, result.elapsedSeconds);
        return result;
    }
};

} // namespace brazen
