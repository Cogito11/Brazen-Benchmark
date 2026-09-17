#pragma once
#include "BenchmarkRunner.h"
#include "TestRegistry.h"
#include <map>
#include <string>
#include <vector>

namespace brazen {

struct CatRank {
    std::string name;
    double minPoints; // inclusive lower bound on the composite points scale
};

// Ranks ordered weakest -> strongest. The points scale is Brazen's own
// composite metric (see BaselineForTest below): 1000 points is single-
// core performance matching an Intel Core i9-13900H, a real reference
// machine rather than a guessed-at "average." The rank thresholds
// themselves (where each tier starts) are still an illustrative curve
// built around that anchor, not derived from a published benchmark
// database. Treat the rank as a fun relative label, and adjust the
// thresholds/baselines here further if you calibrate against more
// known hardware.
inline const std::vector<CatRank>& CatRankTable() {
    static const std::vector<CatRank> table = {
        {"Alley Cat", 0.0},
        {"House Cat", 300.0},
        {"Tabby",     600.0},
        {"Bobcat",    950.0},
        {"Lynx",      1350.0},
        {"Cougar",    1850.0},
        {"Panther",   2500.0},
        {"Tiger",     3500.0},
        {"Lion",      5000.0},
        {"Cheetah",   7500.0},
    };
    return table;
}

inline std::string RankForPoints(double points) {
    const auto& table = CatRankTable();
    std::string rank = table.front().name;
    for (const auto& r : table) {
        if (points >= r.minPoints) rank = r.name;
        else break;
    }
    return rank;
}

// Reference throughput per test, calibrated so that an Intel Core
// i9-13900H running Brazen's own single-core CPU tests lands almost
// exactly at 1000 points (a real, checkable machine beats a made-up
// "average desktop" as an anchor for what these numbers mean). These
// figures are the raw single-core scores measured directly from that
// hardware, not derived from any published benchmark database, since
// Brazen's workloads are custom and wouldn't map cleanly onto one
// anyway.
//
// Note: this baseline is only meaningfully "1000 points" for
// Single-Core results. ComputeComposite() below applies the same
// baseline to Multi-Core results too (it always has), so a Multi-Core
// composite naturally lands well above 1000 purely from running on more
// than one core. That reflects core count, not a separate multi-core
// reference point.
//
// If you have another known machine to add as a second data point (or
// want to recalibrate around different hardware), swap these five
// numbers for that machine's single-core scores from the CPU tab.
inline double BaselineForTest(const std::string& testName) {
    if (testName == "Integer Math")   return 311.91; // Mops/s, i9-13900H single-core
    if (testName == "Floating Point") return 30.46;  // Mops/s, i9-13900H single-core
    if (testName == "Prime Sieve")    return 220.97; // sieves/s, i9-13900H single-core
    if (testName == "Hashing")        return 517.50; // MB/s, i9-13900H single-core
    if (testName == "Sorting")        return 15.54;  // Melems/s, i9-13900H single-core
    return 1.0;
}

struct CompositeScore {
    double points = 0.0;
    std::string rank;
    int testsScored = 0;
    int testsTotal = 0;
    bool complete = false; // true once every available CPU test has contributed a result
};

// Computes one composite score from the latest result per test (already
// filtered to a single run mode by the caller, see BrazenApp's
// m_latestSingleCore / m_latestMultiCore maps) by normalizing each
// result against BaselineForTest() and averaging the points across every
// currently-available CPU test. Tests without a result yet simply don't
// contribute, so the score fills in incrementally as tests complete.
inline CompositeScore ComputeComposite(const std::map<std::string, BenchmarkResult>& latestResultsForMode) {
    auto& reg = TestRegistry::Instance();
    CompositeScore out;
    double sum = 0.0;
    for (const auto& sample : reg.Samples()) {
        if (sample->GetCategory() != TestCategory::Cpu || !sample->IsAvailable()) continue;
        out.testsTotal++;
        auto it = latestResultsForMode.find(sample->GetName());
        if (it == latestResultsForMode.end()) continue;
        double baseline = BaselineForTest(sample->GetName());
        sum += (it->second.score / baseline) * 1000.0;
        out.testsScored++;
    }
    if (out.testsScored > 0) {
        out.points = sum / out.testsScored;
        out.rank = RankForPoints(out.points);
    }
    out.complete = out.testsTotal > 0 && out.testsScored == out.testsTotal;
    return out;
}

} // namespace brazen
