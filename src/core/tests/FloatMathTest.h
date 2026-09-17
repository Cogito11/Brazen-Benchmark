#pragma once
#include "../IBenchmarkTest.h"
#include <cmath>

namespace brazen {

// Numerical integration of a transcendental function (sin/cos/sqrt mix)
// via repeated trapezoidal steps. Stresses the FPU / vectorizable
// floating point path.
class FloatMathTest : public IBenchmarkTest {
public:
    std::string GetName() const override { return "Floating Point"; }
    std::string GetDescription() const override {
        return "Trigonometric/sqrt numerical integration throughput";
    }
    TestCategory GetCategory() const override { return TestCategory::Cpu; }
    std::string GetUnit() const override { return "Mops/s"; }

    uint64_t RunWorkChunk() override {
        constexpr uint64_t kIterations = 500'000;
        double x = m_x;
        double acc = m_acc;
        for (uint64_t i = 0; i < kIterations; ++i) {
            x += 0.0001;
            double s = std::sin(x);
            double c = std::cos(x * 1.0001);
            acc += std::sqrt(std::abs(s * c)) - s * c;
            if (x > 1000.0) x -= 1000.0;
        }
        m_x = x;
        m_acc = acc;
        return kIterations;
    }

    std::unique_ptr<IBenchmarkTest> Clone() const override {
        return std::make_unique<FloatMathTest>(*this);
    }

private:
    double m_x = 0.1;
    double m_acc = 0.0;
};

} // namespace brazen
