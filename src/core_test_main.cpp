// Small console-only entry point used to sanity-check the benchmarking
// core independent of the GUI. Not part of the shipped app.
#include "core/RegisterTests.h"
#include "core/BenchmarkRunner.h"
#include <cstdio>

int main() {
    brazen::RegisterBuiltInTests();
    auto& reg = brazen::TestRegistry::Instance();
    printf("Registered %zu tests\n", reg.Count());

    for (size_t i = 0; i < reg.Count(); ++i) {
        const auto& sample = reg.Samples()[i];
        printf("- [%s] %s (%s) available=%d\n",
               brazen::ToString(sample->GetCategory()), sample->GetName().c_str(),
               sample->GetUnit().c_str(), sample->IsAvailable());
        if (!sample->IsAvailable()) continue;

        auto test = reg.Create(i);
        auto single = brazen::BenchmarkRunner::RunSingleCore(*test, 0.3);
        printf("    single-core: %.3f %s in %.3fs (%llu ops)\n",
               single.score, single.unit.c_str(), single.elapsedSeconds,
               (unsigned long long)single.totalOps);

        auto multi = brazen::BenchmarkRunner::RunMultiCore(*test, 0.3, 0);
        printf("    multi-core (%d threads): %.3f %s in %.3fs\n",
               multi.threadsUsed, multi.score, multi.unit.c_str(), multi.elapsedSeconds);
    }
    return 0;
}
