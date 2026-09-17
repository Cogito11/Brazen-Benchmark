#pragma once
#include <string>
#include <memory>
#include <cstdint>

namespace brazen {

// Category of a benchmark test. Used for grouping in the UI. Note that
// GPU tests don't actually implement IBenchmarkTest at all (see
// src/gpu/GpuTestRunner.h for why). TestCategory::Gpu exists here only
// so ToString() has a label for the GPU section header in the UI.
//
// Ssd tests (see core/tests/DiskIoTest.h) DO implement IBenchmarkTest
// and run through the normal BenchmarkManager/BenchmarkRunner path like
// CPU/RAM. But like RamBandwidthTest's buffer size, a disk test needs
// a caller-chosen target directory (which drive to test), so it isn't
// registered in TestRegistry with the CPU/RAM tests; the UI constructs
// it directly once a drive is picked. See BrazenApp::StartSsdBenchmark.
enum class TestCategory {
    Cpu,
    Ram,
    Gpu,
    Ssd
};

inline const char* ToString(TestCategory c) {
    switch (c) {
        case TestCategory::Cpu: return "CPU";
        case TestCategory::Ram: return "RAM";
        case TestCategory::Gpu: return "GPU";
        case TestCategory::Ssd: return "SSD";
    }
    return "Unknown";
}

// Interface every benchmark test implements. A test performs a fixed,
// self-contained "chunk" of work each call to RunWorkChunk() and reports
// how many logical operations it completed. The BenchmarkRunner calls
// RunWorkChunk() in a loop for a fixed wall-clock duration and aggregates
// the operation counts into a throughput score.
//
// Tests must be cheap to Clone() because the runner creates one instance
// per worker thread for multi-core runs, so each thread gets independent
// state (buffers, RNG, etc.) and results aren't skewed by contention that
// isn't intentional to the test itself.
class IBenchmarkTest {
public:
    virtual ~IBenchmarkTest() = default;

    virtual std::string GetName() const = 0;
    virtual std::string GetDescription() const = 0;
    virtual TestCategory GetCategory() const = 0;

    // Unit label for the score this test produces, e.g. "Mops/s", "MB/s".
    virtual std::string GetUnit() const = 0;

    // Called once on a thread before timed work begins (allocate buffers,
    // seed RNG, warm caches, etc.). Not included in the timed measurement.
    virtual void Setup() {}

    // Perform one chunk of work and return the number of logical
    // operations completed. Should take roughly 1-50ms so the runner
    // can respect the requested duration reasonably precisely.
    virtual uint64_t RunWorkChunk() = 0;

    // Convert a raw aggregate operation count + elapsed seconds into the
    // final displayed score, in whatever unit GetUnit() describes.
    // Default: operations per second, expressed in millions (Mops/s).
    virtual double ComputeScore(uint64_t totalOps, double elapsedSeconds) const {
        if (elapsedSeconds <= 0.0) return 0.0;
        return (static_cast<double>(totalOps) / elapsedSeconds) / 1'000'000.0;
    }

    // Create a fresh, independent instance of this test (same config).
    virtual std::unique_ptr<IBenchmarkTest> Clone() const = 0;

    // Whether this test is currently runnable. Every built-in CPU/RAM
    // test returns true (the default); this exists as a hook for a
    // future test that might need a runtime capability check (e.g. "only
    // available if the CPU supports AVX2") without changing the UI or
    // TestRegistry.
    virtual bool IsAvailable() const { return true; }
};

} // namespace brazen
