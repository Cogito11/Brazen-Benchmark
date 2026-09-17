#pragma once
#include "IBenchmarkTest.h"
#include <functional>
#include <memory>
#include <vector>

namespace brazen {

// Central place that knows about every benchmark test in the app.
// Adding a new test (including future RAM/GPU tests) means writing a
// class that implements IBenchmarkTest and registering a factory for it
// here (or wherever RegisterBuiltInTests() is called from) — nothing
// else in the UI or runner needs to change.
class TestRegistry {
public:
    using Factory = std::function<std::unique_ptr<IBenchmarkTest>()>;

    static TestRegistry& Instance() {
        static TestRegistry instance;
        return instance;
    }

    void Register(Factory factory) {
        // Instantiate once now purely to read metadata (name/category);
        // the UI and runner create their own fresh instances via Clone()
        // or by calling the factory again.
        auto sample = factory();
        m_factories.push_back(factory);
        m_samples.push_back(std::move(sample));
    }

    const std::vector<std::unique_ptr<IBenchmarkTest>>& Samples() const { return m_samples; }

    std::unique_ptr<IBenchmarkTest> Create(size_t index) const {
        return m_factories[index]();
    }

    size_t Count() const { return m_factories.size(); }

private:
    std::vector<Factory> m_factories;
    std::vector<std::unique_ptr<IBenchmarkTest>> m_samples;
};

// Helper for static self-registration, used by BRAZEN_REGISTER_TEST below.
struct AutoRegister {
    explicit AutoRegister(TestRegistry::Factory factory) {
        TestRegistry::Instance().Register(std::move(factory));
    }
};

} // namespace brazen

// Place at global/namespace scope in a test's .cpp file to register it
// automatically at program startup, e.g.:
//   BRAZEN_REGISTER_TEST(IntegerMathTest)
#define BRAZEN_REGISTER_TEST(TestClass)                                              \
    namespace {                                                                      \
    static ::brazen::AutoRegister _brazen_autoreg_##TestClass(                       \
        []() -> std::unique_ptr<::brazen::IBenchmarkTest> {                          \
            return std::make_unique<TestClass>();                                    \
        });                                                                          \
    }
