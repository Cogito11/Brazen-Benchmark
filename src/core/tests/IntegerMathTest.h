#pragma once
#include "../IBenchmarkTest.h"

namespace brazen {

// Tight loop of mixed integer arithmetic (add/mul/xor/mod) designed to
// stress the integer ALU pipeline. Each chunk performs a fixed number of
// iterations so RunWorkChunk() stays short and the runner can time-slice
// accurately.
class IntegerMathTest : public IBenchmarkTest {
public:
    std::string GetName() const override { return "Integer Math"; }
    std::string GetDescription() const override {
        return "Mixed integer add/multiply/xor/modulo throughput";
    }
    TestCategory GetCategory() const override { return TestCategory::Cpu; }
    std::string GetUnit() const override { return "Mops/s"; }

    uint64_t RunWorkChunk() override {
        constexpr uint64_t kIterations = 2'000'000;
        uint64_t a = m_state;
        uint64_t b = 0x9E3779B97F4A7C15ull;
        for (uint64_t i = 0; i < kIterations; ++i) {
            a = a * 6364136223846793005ull + 1442695040888963407ull;
            b ^= a >> 17;
            b += (a & 0xFFFF) * 31;
            b %= 0xFFFFFFFFFFFFFF1Full;
        }
        m_state = a ^ b;
        return kIterations;
    }

    std::unique_ptr<IBenchmarkTest> Clone() const override {
        return std::make_unique<IntegerMathTest>(*this);
    }

private:
    uint64_t m_state = 88172645463325252ull;
};

} // namespace brazen
