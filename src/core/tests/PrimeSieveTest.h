#pragma once
#include "../IBenchmarkTest.h"
#include <algorithm>
#include <vector>

namespace brazen {

// Sieve of Eratosthenes over a fixed range, repeated. Mixes memory
// access patterns with integer logic; each chunk is one full sieve pass.
class PrimeSieveTest : public IBenchmarkTest {
public:
    std::string GetName() const override { return "Prime Sieve"; }
    std::string GetDescription() const override {
        return "Sieve of Eratosthenes passes per second";
    }
    TestCategory GetCategory() const override { return TestCategory::Cpu; }
    std::string GetUnit() const override { return "sieves/s"; }

    void Setup() override {
        m_sieve.assign(kLimit + 1, true);
    }

    uint64_t RunWorkChunk() override {
        // The runner always calls Setup() first; this keeps a missed
        // Setup() from becoming an out-of-bounds write.
        if (m_sieve.size() != kLimit + 1) Setup();
        std::fill(m_sieve.begin(), m_sieve.end(), true);
        m_sieve[0] = m_sieve[1] = false;
        for (size_t p = 2; p * p <= kLimit; ++p) {
            if (!m_sieve[p]) continue;
            for (size_t multiple = p * p; multiple <= kLimit; multiple += p)
                m_sieve[multiple] = false;
        }
        return 1; // one full sieve pass counted as one "operation"
    }

    // For this test the natural score is sieve passes per second rather
    // than raw op-count-derived Mops/s, so override the score computation.
    double ComputeScore(uint64_t totalOps, double elapsedSeconds) const override {
        if (elapsedSeconds <= 0.0) return 0.0;
        return static_cast<double>(totalOps) / elapsedSeconds;
    }

    std::unique_ptr<IBenchmarkTest> Clone() const override {
        return std::make_unique<PrimeSieveTest>(*this);
    }

    // Verification hook (not used while timing): number of primes found by
    // the most recent pass. There are exactly 148,933 primes <= 2,000,000,
    // so a different answer means the test is computing the wrong thing.
    static constexpr size_t kLimitForVerification = 2'000'000;
    uint64_t CountPrimes() const {
        uint64_t count = 0;
        for (size_t i = 2; i < m_sieve.size(); ++i) count += m_sieve[i] ? 1 : 0;
        return count;
    }

private:
    static constexpr size_t kLimit = 2'000'000;
    std::vector<bool> m_sieve;
};

} // namespace brazen
