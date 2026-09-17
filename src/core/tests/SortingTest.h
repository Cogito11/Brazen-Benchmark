#pragma once
#include "../IBenchmarkTest.h"
#include <algorithm>
#include <vector>

namespace brazen {

// Repeatedly re-shuffles and sorts a fixed-size array of integers.
// Exercises branch prediction, comparisons, and cache-friendly-ish
// memory access rather than raw sequential bandwidth.
class SortingTest : public IBenchmarkTest {
public:
    std::string GetName() const override { return "Sorting"; }
    std::string GetDescription() const override {
        return "std::sort throughput on pseudo-random integer arrays";
    }
    TestCategory GetCategory() const override { return TestCategory::Cpu; }
    std::string GetUnit() const override { return "Melems/s"; }

    void Setup() override {
        m_data.resize(kElements);
        m_rngState = 12345;
    }

    uint64_t RunWorkChunk() override {
        for (auto& v : m_data) {
            m_rngState = m_rngState * 1103515245u + 12345u;
            v = static_cast<int>(m_rngState >> 8);
        }
        std::sort(m_data.begin(), m_data.end());
        return kElements;
    }

    std::unique_ptr<IBenchmarkTest> Clone() const override {
        return std::make_unique<SortingTest>(*this);
    }

private:
    static constexpr size_t kElements = 200'000;
    std::vector<int> m_data;
    uint32_t m_rngState = 12345;
};

} // namespace brazen
