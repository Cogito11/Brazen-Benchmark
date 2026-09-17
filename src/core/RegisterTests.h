#pragma once
#include "TestRegistry.h"
#include "tests/IntegerMathTest.h"
#include "tests/FloatMathTest.h"
#include "tests/PrimeSieveTest.h"
#include "tests/HashingTest.h"
#include "tests/SortingTest.h"
#include "tests/RamBandwidthTest.h"

namespace brazen {

// Registers every built-in CPU/RAM test (i.e. everything that's
// single-threaded-clonable and safe to run on an arbitrary worker
// thread). GPU testing is intentionally NOT part of this registry: a GL
// context can only be driven from one thread at a time, so it doesn't
// fit the "clone across N worker threads" shape this registry assumes.
// See src/gpu/GpuTestRunner.h for how GPU testing is wired up instead.
inline void RegisterBuiltInTests() {
    auto& reg = TestRegistry::Instance();
    reg.Register([] { return std::make_unique<IntegerMathTest>(); });
    reg.Register([] { return std::make_unique<FloatMathTest>(); });
    reg.Register([] { return std::make_unique<PrimeSieveTest>(); });
    reg.Register([] { return std::make_unique<HashingTest>(); });
    reg.Register([] { return std::make_unique<SortingTest>(); });
    reg.Register([] { return std::make_unique<RamBandwidthTest>(); });
}

} // namespace brazen
