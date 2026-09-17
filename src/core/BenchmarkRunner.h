#pragma once
#include "IBenchmarkTest.h"
#include "ThreadAffinity.h"
#include <atomic>
#include <chrono>
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

    // Whether every worker thread's core-pinning request was accepted by
    // the OS. If false, at least one thread ran unpinned and the score
    // may carry extra scheduler-induced noise (see ThreadAffinity.h).
    bool affinityPinned = false;
};

// Runs a single IBenchmarkTest either on one thread ("single core") or
// across N threads ("multi core"), each running independent clones of the
// test concurrently for a fixed duration, then aggregates throughput.
class BenchmarkRunner {
public:
    // durationSeconds: how long the timed portion of the run should last.
    // cancel: optional flag the caller can set from another thread to stop early.
    static BenchmarkResult RunSingleCore(const IBenchmarkTest& test,
                                          double durationSeconds,
                                          std::atomic<bool>* cancel = nullptr) {
        return RunInternal(test, durationSeconds, /*threadCount=*/1, RunMode::SingleCore, cancel);
    }

    // threadCount == 0 means "use std::thread::hardware_concurrency()".
    static BenchmarkResult RunMultiCore(const IBenchmarkTest& test,
                                         double durationSeconds,
                                         unsigned threadCount,
                                         std::atomic<bool>* cancel = nullptr) {
        unsigned n = threadCount == 0 ? std::thread::hardware_concurrency() : threadCount;
        if (n == 0) n = 4; // fallback if hardware_concurrency() can't tell
        return RunInternal(test, durationSeconds, n, RunMode::MultiCore, cancel);
    }

private:
    static BenchmarkResult RunInternal(const IBenchmarkTest& test,
                                        double durationSeconds,
                                        unsigned threadCount,
                                        RunMode mode,
                                        std::atomic<bool>* cancel) {
        using clock = std::chrono::steady_clock;

        std::vector<std::unique_ptr<IBenchmarkTest>> instances;
        instances.reserve(threadCount);
        for (unsigned i = 0; i < threadCount; ++i)
            instances.push_back(test.Clone());

        std::vector<uint64_t> perThreadOps(threadCount, 0);
        std::vector<double> perThreadElapsed(threadCount, 0.0);
        std::vector<std::thread> workers;
        workers.reserve(threadCount);

        unsigned hwThreads = std::thread::hardware_concurrency();
        if (hwThreads == 0) hwThreads = threadCount; // best guess if the OS can't tell us

        std::atomic<bool> allPinned{true};

        auto threadBody = [&](unsigned idx) {
            // Single-core runs pin to the highest-numbered core rather than
            // core 0: core 0 typically fields more OS/interrupt traffic,
            // which adds noise to a single-thread measurement. Multi-core
            // runs pin thread i to core i so every logical core actually
            // gets used instead of the scheduler load-balancing threads
            // onto a subset of cores.
            unsigned coreToPin = (threadCount == 1) ? (hwThreads > 0 ? hwThreads - 1 : 0)
                                                     : (idx % hwThreads);
            if (!PinCurrentThreadToCore(coreToPin))
                allPinned.store(false, std::memory_order_relaxed);

            IBenchmarkTest& t = *instances[idx];
            t.Setup();

            // The deadline is computed here, after Setup() returns, and
            // independently per thread -- not once up front before any
            // thread starts. Setup() can be non-trivial (e.g. the RAM
            // test allocates and fills tens of megabytes per thread), and
            // if it were counted against a shared pre-computed deadline,
            // slow setup could eat into -- or on a short duration,
            // entirely consume -- the time meant for actual timed work,
            // silently producing an artificially low (or zero) score.
            auto loopStart = clock::now();
            auto deadline = loopStart + std::chrono::duration_cast<clock::duration>(
                                             std::chrono::duration<double>(durationSeconds));
            uint64_t ops = 0;
            while (clock::now() < deadline) {
                if (cancel && cancel->load(std::memory_order_relaxed)) break;
                ops += t.RunWorkChunk();
            }
            auto loopEnd = clock::now();

            perThreadOps[idx] = ops;
            perThreadElapsed[idx] = std::chrono::duration<double>(loopEnd - loopStart).count();
        };

        for (unsigned i = 0; i < threadCount; ++i)
            workers.emplace_back(threadBody, i);
        for (auto& w : workers)
            w.join();

        BenchmarkResult result;
        result.testName = test.GetName();
        result.unit = test.GetUnit();
        result.mode = mode;
        result.threadsUsed = static_cast<int>(threadCount);

        // Average of each thread's own timed-portion duration -- not
        // wall-clock time across thread creation/join, which would still
        // include Setup() cost and quietly deflate the score. Threads
        // target the same duration, so this converges close to
        // durationSeconds in the common case, while still reflecting
        // reality if a run was cancelled partway through.
        double totalElapsed = 0.0;
        for (double e : perThreadElapsed) totalElapsed += e;
        result.elapsedSeconds = threadCount > 0 ? totalElapsed / threadCount : 0.0;

        result.cancelled = cancel && cancel->load(std::memory_order_relaxed);
        result.affinityPinned = allPinned.load(std::memory_order_relaxed);

        uint64_t totalOps = 0;
        for (auto o : perThreadOps) totalOps += o;
        result.totalOps = totalOps;
        result.score = test.ComputeScore(totalOps, result.elapsedSeconds);
        return result;
    }
};

} // namespace brazen
