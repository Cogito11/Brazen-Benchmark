#pragma once
#include "../IBenchmarkTest.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace brazen {

// Sequential memory copy (STREAM "Copy" kernel style) over a buffer
// deliberately larger than any consumer CPU's last-level cache, so the
// measurement reflects actual RAM bandwidth rather than cache bandwidth.
// Like the CPU tests, this scales naturally across threads: multiple
// threads each hammering their own large buffer concurrently is exactly
// how real-world memory bandwidth benchmarks (e.g. STREAM run in
// parallel, Intel MLC) saturate the memory controller for a multi-core
// figure, so no special-casing is needed in BenchmarkRunner -- "multi
// core" here means "multiple threads contending for memory bandwidth",
// which is a meaningful and standard thing to measure.
class RamBandwidthTest : public IBenchmarkTest {
public:
    // bytesPerArray: size of each of the two (src/dst) buffers used per
    // thread instance. Defaults to 32 MB, comfortably bigger than the L3
    // cache on most consumer/prosumer CPUs -- but some high-end
    // workstation/server chips ship 60-256+ MB of L3, so this is exposed
    // as a setting (see the RAM benchmark configuration modal) rather
    // than a fixed guess, letting anyone with unusually large cache
    // deliberately size the buffer to exceed it.
    explicit RamBandwidthTest(size_t bytesPerArray = kDefaultBytesPerArray)
        : m_bytesPerArray(bytesPerArray) {}

    std::string GetName() const override { return "RAM Bandwidth"; }
    std::string GetDescription() const override {
        return "Sequential memcpy-style throughput over a large (>cache) buffer";
    }
    TestCategory GetCategory() const override { return TestCategory::Ram; }
    std::string GetUnit() const override { return "GB/s"; }

    static constexpr size_t kDefaultBytesPerArray = 32ull * 1024 * 1024;

    void Setup() override {
        size_t elements = m_bytesPerArray / sizeof(uint64_t);
        m_src.assign(elements, 0);
        m_dst.assign(elements, 0);
        // Fill with a non-trivial pattern so the buffer is actually
        // committed to physical pages (first touch) before timing starts,
        // and isn't just a block of zero pages the OS could special-case.
        for (size_t i = 0; i < elements; ++i)
            m_src[i] = static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ull;
    }

    uint64_t RunWorkChunk() override {
        // Alternate copy direction each chunk so we're not always reading
        // src's pages and writing dst's -- both buffers get exercised as
        // both source and destination over the course of a run.
        if (m_forward)
            std::copy(m_src.begin(), m_src.end(), m_dst.begin());
        else
            std::copy(m_dst.begin(), m_dst.end(), m_src.begin());
        m_forward = !m_forward;

        // "Bytes moved" counts both the read and the write side of the
        // copy, matching how STREAM and similar tools report bandwidth.
        return static_cast<uint64_t>(m_src.size()) * sizeof(uint64_t) * 2;
    }

    double ComputeScore(uint64_t totalOps, double elapsedSeconds) const override {
        if (elapsedSeconds <= 0.0) return 0.0;
        double bytesPerSec = static_cast<double>(totalOps) / elapsedSeconds;
        return bytesPerSec / (1000.0 * 1000.0 * 1000.0); // GB/s (decimal, matching how RAM/storage is usually marketed)
    }

    // Clone() must preserve the configured buffer size, since
    // BenchmarkRunner creates one clone per worker thread and each needs
    // to allocate the same size the user configured, not silently fall
    // back to the default.
    std::unique_ptr<IBenchmarkTest> Clone() const override {
        return std::make_unique<RamBandwidthTest>(*this);
    }

private:
    size_t m_bytesPerArray;
    std::vector<uint64_t> m_src;
    std::vector<uint64_t> m_dst;
    bool m_forward = true;
};

} // namespace brazen
