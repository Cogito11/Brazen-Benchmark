#pragma once
#include "../IBenchmarkTest.h"
#include <vector>

namespace brazen {

// Hashes a fixed in-memory buffer repeatedly with a fast FNV-1a-style
// mixer. Stresses sequential memory read bandwidth plus integer logic,
// similar in spirit to compression/hashing workloads.
class HashingTest : public IBenchmarkTest {
public:
    std::string GetName() const override { return "Hashing"; }
    std::string GetDescription() const override {
        return "FNV-style buffer hashing throughput";
    }
    TestCategory GetCategory() const override { return TestCategory::Cpu; }
    std::string GetUnit() const override { return "MB/s"; }

    void Setup() override {
        m_buffer.resize(kBufferBytes);
        uint32_t seed = 2166136261u;
        for (auto& byte : m_buffer) {
            seed = seed * 16777619u + 1;
            byte = static_cast<unsigned char>(seed & 0xFF);
        }
    }

    uint64_t RunWorkChunk() override {
        uint64_t hash = 1469598103934665603ull;
        const unsigned char* data = m_buffer.data();
        for (size_t i = 0; i < kBufferBytes; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
        m_lastHash ^= hash;
        return kBufferBytes; // bytes processed this chunk
    }

    // Report throughput in MB/s rather than generic ops/s.
    double ComputeScore(uint64_t totalOps, double elapsedSeconds) const override {
        if (elapsedSeconds <= 0.0) return 0.0;
        double bytesPerSec = static_cast<double>(totalOps) / elapsedSeconds;
        return bytesPerSec / (1024.0 * 1024.0);
    }

    std::unique_ptr<IBenchmarkTest> Clone() const override {
        return std::make_unique<HashingTest>(*this);
    }

private:
    static constexpr size_t kBufferBytes = 4 * 1024 * 1024; // 4 MB
    std::vector<unsigned char> m_buffer;
    uint64_t m_lastHash = 0;
};

} // namespace brazen
