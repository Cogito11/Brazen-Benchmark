#include "SelfTest.h"

#include "core/BenchmarkManager.h"
#include "core/BenchmarkRunner.h"
#include "core/RegisterTests.h"
#include "core/TestRegistry.h"
#include "core/tests/DiskIoTest.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace brazen;
using namespace std::chrono_literals;

namespace {

// --------------------------------------------------------------------
// A scriptable fake test, used to provoke the situations real tests can
// get into: exceptions, zero work, slow setup, fixed-work completion.
// --------------------------------------------------------------------
struct Script {
    bool throwInSetup = false;
    bool throwInChunk = false;     // throws on the 3rd chunk
    int  throwInCloneAfter = -1;   // Clone() throws once this many clones exist
    bool zeroWork = false;         // chunks "work" but report 0 operations
    int  setupSleepMsPerIndex = 0; // clone i sleeps i * this in Setup()
    bool fixedWork = false;        // RunsToCompletion(); done after chunksBeforeComplete
    int  chunksBeforeComplete = 3;
    std::atomic<bool>* cancelOnLastChunk = nullptr;

    std::atomic<int> clones{0};
    std::atomic<int> chunks{0};
    std::atomic<long long> firstChunkNs[16];
    Script() { for (auto& a : firstChunkNs) a.store(0); }
};

class ScriptedTest : public IBenchmarkTest {
public:
    explicit ScriptedTest(std::shared_ptr<Script> s, int index = 0) : m_s(std::move(s)), m_index(index) {}

    std::string GetName() const override { return "Scripted"; }
    std::string GetDescription() const override { return "test double"; }
    TestCategory GetCategory() const override { return TestCategory::Cpu; }
    std::string GetUnit() const override { return "ops/s"; }

    void Setup() override {
        if (m_s->setupSleepMsPerIndex > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(m_s->setupSleepMsPerIndex * m_index));
        if (m_s->throwInSetup) throw std::bad_alloc();
    }

    uint64_t RunWorkChunk() override {
        if (m_index < 16) {
            long long expected = 0;
            long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
            m_s->firstChunkNs[m_index].compare_exchange_strong(expected, now);
        }
        ++m_myChunks;
        int n = ++m_s->chunks;
        if (m_s->throwInChunk && m_myChunks >= 3) throw std::runtime_error("boom in chunk");
        std::this_thread::sleep_for(1ms);
        if (m_s->fixedWork && m_myChunks >= m_s->chunksBeforeComplete) {
            m_done = true;
            if (m_s->cancelOnLastChunk) m_s->cancelOnLastChunk->store(true);
        }
        (void)n;
        return m_s->zeroWork ? 0 : 1;
    }

    bool RunsToCompletion() const override { return m_s->fixedWork; }
    bool IsComplete() const override { return m_done; }

    std::unique_ptr<IBenchmarkTest> Clone() const override {
        int idx = m_s->clones.fetch_add(1);
        if (m_s->throwInCloneAfter >= 0 && idx >= m_s->throwInCloneAfter) throw std::bad_alloc();
        return std::make_unique<ScriptedTest>(m_s, idx);
    }

private:
    std::shared_ptr<Script> m_s;
    int m_index;
    int m_myChunks = 0;
    bool m_done = false;
};

bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds limit) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

} // namespace

// ====================================================================
// Runner
// ====================================================================

TEST_CASE(runner_exception_in_setup_is_reported_not_fatal) {
    auto s = std::make_shared<Script>();
    s->throwInSetup = true;
    ScriptedTest proto(s);
    for (unsigned threads : {1u, 3u}) {
        BenchmarkResult r = threads == 1 ? BenchmarkRunner::RunSingleCore(proto, 0.2)
                                         : BenchmarkRunner::RunMultiCore(proto, 0.2, threads);
        CHECK(r.failed);
        CHECK(!r.notes.empty());
    }
}

TEST_CASE(runner_exception_in_chunk_is_reported_not_fatal) {
    auto s = std::make_shared<Script>();
    s->throwInChunk = true;
    ScriptedTest proto(s);
    BenchmarkResult r = BenchmarkRunner::RunMultiCore(proto, 0.5, 2);
    CHECK(r.failed);
    CHECK(r.notes.find("boom") != std::string::npos);
}

TEST_CASE(runner_exception_in_clone_is_reported_not_fatal) {
    auto s = std::make_shared<Script>();
    s->throwInCloneAfter = 2; // the 3rd clone fails
    ScriptedTest proto(s);
    BenchmarkResult r = BenchmarkRunner::RunMultiCore(proto, 0.2, 4);
    CHECK(r.failed);
}

TEST_CASE(runner_zero_work_is_a_failure_not_a_zero_score) {
    auto s = std::make_shared<Script>();
    s->zeroWork = true;
    ScriptedTest proto(s);
    BenchmarkResult r = BenchmarkRunner::RunSingleCore(proto, 0.2);
    CHECK(r.failed);
    CHECK(!r.cancelled);
}

TEST_CASE(runner_threads_begin_timing_together) {
    // Thread i takes i * 70 ms to finish Setup(). Timing must not start on
    // any thread until every thread is ready, otherwise early threads run
    // alone for a while and the aggregate (e.g. RAM bandwidth) is inflated.
    auto s = std::make_shared<Script>();
    s->setupSleepMsPerIndex = 70;
    ScriptedTest proto(s);
    BenchmarkResult r = BenchmarkRunner::RunMultiCore(proto, 0.3, 4);
    CHECK(!r.failed);
    long long lo = std::numeric_limits<long long>::max(), hi = 0;
    for (int i = 0; i < 4; ++i) {
        long long t = s->firstChunkNs[i].load();
        CHECK(t != 0);
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    double spreadMs = static_cast<double>(hi - lo) / 1e6;
    std::printf("    first-chunk spread across threads: %.1f ms\n", spreadMs);
    CHECK(spreadMs < 40.0);
}

TEST_CASE(runner_completed_run_is_not_marked_cancelled_by_a_late_cancel) {
    // The cancel flag is raised on the very last chunk of a fixed-work run.
    // The run still completed all its work, so it must not be reported as
    // cancelled (it used to be, because the flag was read after the fact).
    std::atomic<bool> cancel{false};
    auto s = std::make_shared<Script>();
    s->fixedWork = true;
    s->cancelOnLastChunk = &cancel;
    ScriptedTest proto(s);
    BenchmarkResult r = BenchmarkRunner::RunSingleCore(proto, 30.0, &cancel);
    CHECK(!r.failed);
    CHECK(!r.cancelled);
    CHECK(r.totalOps == 3);
}

TEST_CASE(runner_survives_absurd_durations) {
    // NaN / negative / infinite / enormous durations used to hit undefined
    // behavior in the double->integer clock conversion. Use a fixed-work
    // test so that "run forever" durations still terminate promptly.
    for (double d : {std::numeric_limits<double>::quiet_NaN(), -5.0, 0.0,
                     std::numeric_limits<double>::infinity(), 1e300}) {
        auto s = std::make_shared<Script>();
        s->fixedWork = true;
        ScriptedTest proto(s);
        BenchmarkResult r = BenchmarkRunner::RunSingleCore(proto, d);
        CHECK(!r.cancelled);
    }
}

// ====================================================================
// Manager
// ====================================================================

TEST_CASE(manager_rejects_null_and_out_of_range_jobs_and_stays_idle) {
    RegisterBuiltInTests();
    BenchmarkManager m;
    m.Enqueue(std::shared_ptr<IBenchmarkTest>(), RunMode::SingleCore, 0.1);
    m.Enqueue(static_cast<size_t>(999999), RunMode::SingleCore, 0.1);
    CHECK(WaitUntil([&] { return m.IsIdle(); }, 1000ms));
}

TEST_CASE(manager_turns_a_throwing_job_into_a_failed_result_and_keeps_going) {
    BenchmarkManager m;
    auto bad = std::make_shared<Script>();
    bad->throwInSetup = true;
    m.Enqueue(std::make_shared<ScriptedTest>(bad), RunMode::MultiCore, 0.2, 2);
    auto good = std::make_shared<Script>();
    m.Enqueue(std::make_shared<ScriptedTest>(good), RunMode::SingleCore, 0.2);
    REQUIRE(WaitUntil([&] { return m.IsIdle(); }, 5000ms));
    auto results = m.DrainResults();
    REQUIRE(results.size() == 2);
    CHECK(results[0].failed);
    CHECK(!results[1].failed);
    CHECK(results[1].totalOps > 0);
}

TEST_CASE(manager_construct_destroy_stress_never_hangs) {
    // A missed wake-up in shutdown would make a destructor wait forever.
    // Watchdog: abort the whole process if this takes absurdly long.
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 600 && !finished.load(); ++i) std::this_thread::sleep_for(100ms);
        if (!finished.load()) {
            std::printf("    watchdog: manager shutdown hung\n");
            std::fflush(stdout);
            std::_Exit(3);
        }
    });
    for (int i = 0; i < 1500; ++i) {
        BenchmarkManager m;
        if (i % 3 == 0) std::this_thread::yield();
    }
    finished = true;
    watchdog.join();
    CHECK(true);
}

TEST_CASE(manager_cancel_all_stops_current_and_drops_queued) {
    BenchmarkManager m;
    for (int i = 0; i < 4; ++i) {
        auto s = std::make_shared<Script>();
        // Time-boxed 30 s jobs: only cancellation can end them quickly.
        m.Enqueue(std::make_shared<ScriptedTest>(s), RunMode::SingleCore, 30.0);
    }
    REQUIRE(WaitUntil([&] { return m.IsBusy(); }, 2000ms));
    std::this_thread::sleep_for(100ms);
    m.CancelAll();
    CHECK(WaitUntil([&] { return m.IsIdle(); }, 3000ms));
    auto results = m.DrainResults();
    REQUIRE(results.size() == 1);
    CHECK(results[0].cancelled);
}

// ====================================================================
// Registry
// ====================================================================

TEST_CASE(registry_create_out_of_range_returns_null) {
    RegisterBuiltInTests();
    CHECK(TestRegistry::Instance().Create(999999) == nullptr);
}

TEST_CASE(registering_builtin_tests_twice_does_not_duplicate_them) {
    RegisterBuiltInTests();
    size_t once = TestRegistry::Instance().Count();
    RegisterBuiltInTests();
    CHECK(TestRegistry::Instance().Count() == once);
    CHECK(once == 6);
}

// ====================================================================
// Individual tests: robustness
// ====================================================================

TEST_CASE(cpu_tests_survive_being_run_without_setup) {
    // The runner always calls Setup() first, but a test must never turn a
    // missed Setup() into an out-of-bounds write.
    PrimeSieveTest sieve;
    CHECK(sieve.RunWorkChunk() > 0);
    HashingTest hashing;
    CHECK(hashing.RunWorkChunk() > 0);
    SortingTest sorting;
    CHECK(sorting.RunWorkChunk() > 0);
}

TEST_CASE(ram_test_clamps_a_zero_size_buffer) {
    RamBandwidthTest ram(0);
    ram.Setup();
    CHECK(ram.RunWorkChunk() > 0);
}

TEST_CASE(ram_test_huge_allocation_is_a_reported_failure) {
    // An absurd buffer size must become a failed result with a clear
    // reason, and nothing must be allocated (on over-committing systems an
    // allocation like this would "succeed" and the OS would kill us when
    // the buffer was touched).
    RamBandwidthTest ram(static_cast<size_t>(1) << 60);
    BenchmarkResult r = BenchmarkRunner::RunMultiCore(ram, 0.2, 2);
    CHECK(r.failed);
}

// ====================================================================
// Individual tests: is the computed answer actually right?
// ====================================================================

TEST_CASE(prime_sieve_finds_exactly_the_primes_below_two_million) {
    PrimeSieveTest t;
    t.Setup();
    t.RunWorkChunk();
    CHECK(t.CountPrimes() == 148933); // pi(2,000,000)
    t.RunWorkChunk();                 // a second pass must give the same answer
    CHECK(t.CountPrimes() == 148933);
}

TEST_CASE(hashing_matches_an_independent_fnv1a_reference) {
    HashingTest t;
    t.Setup();
    t.RunWorkChunk();
    // Re-derive the buffer and hash it with a separate FNV-1a implementation.
    constexpr size_t kBytes = 4 * 1024 * 1024;
    std::vector<unsigned char> buf(kBytes);
    uint32_t seed = 2166136261u;
    for (auto& b : buf) {
        seed = seed * 16777619u + 1;
        b = static_cast<unsigned char>(seed & 0xFF);
    }
    uint64_t h = 1469598103934665603ull;
    for (unsigned char b : buf) { h ^= b; h *= 1099511628211ull; }
    CHECK(t.Checksum() == h);
}

TEST_CASE(sorting_really_sorts_non_degenerate_data) {
    SortingTest t;
    t.Setup();
    t.RunWorkChunk();
    const auto& d = t.Data();
    CHECK(d.size() == 200000);
    CHECK(std::is_sorted(d.begin(), d.end()));
    CHECK(d.front() < d.back());                          // not all equal
    std::set<int> distinct(d.begin(), d.end());
    CHECK(distinct.size() > d.size() / 2);                // genuinely varied input
    int before = d[0];
    t.RunWorkChunk();                                     // next pass reshuffles different data
    CHECK(t.Data()[0] != before || t.Data()[100] != d[100]);
}

TEST_CASE(integer_math_is_deterministic_and_changes_state) {
    IntegerMathTest a, b;
    uint64_t start = a.State();
    for (int i = 0; i < 3; ++i) { a.RunWorkChunk(); b.RunWorkChunk(); }
    CHECK(a.State() == b.State());
    CHECK(a.State() != start);
    auto c = a.Clone();
    c->RunWorkChunk();
    a.RunWorkChunk();
    CHECK(static_cast<IntegerMathTest*>(c.get())->State() == a.State());
}

TEST_CASE(float_math_stays_finite_and_is_deterministic) {
    FloatMathTest a, b;
    for (int i = 0; i < 40; ++i) { a.RunWorkChunk(); b.RunWorkChunk(); }
    CHECK(std::isfinite(a.Accumulator()));
    CHECK(a.Accumulator() > 0.0);
    CHECK(a.Position() >= 0.0 && a.Position() <= 1000.5);
    CHECK(a.Accumulator() == b.Accumulator());
}

TEST_CASE(ram_copy_leaves_both_buffers_identical_and_populated) {
    RamBandwidthTest t(2 * 1024 * 1024);
    t.Setup();
    for (int i = 0; i < 3; ++i) CHECK(t.RunWorkChunk() == 2 * 1024 * 1024 * 2);
    CHECK(t.BuffersMatchAndAreNonZero());
}

TEST_CASE(every_registered_test_produces_a_sane_score_through_the_runner) {
    RegisterBuiltInTests();
    auto& reg = TestRegistry::Instance();
    for (size_t i = 0; i < reg.Count(); ++i) {
        auto proto = reg.Create(i);
        REQUIRE(proto != nullptr);
        for (unsigned threads : {1u, 2u}) {
            BenchmarkResult r = threads == 1 ? BenchmarkRunner::RunSingleCore(*proto, 0.15)
                                             : BenchmarkRunner::RunMultiCore(*proto, 0.15, threads);
            if (r.failed || r.cancelled || !(r.score > 0.0) || !std::isfinite(r.score) || r.totalOps == 0) {
                std::printf("    %s (%u threads): failed=%d score=%g ops=%llu\n", r.testName.c_str(), threads,
                            r.failed, r.score, static_cast<unsigned long long>(r.totalOps));
            }
            CHECK(!r.failed);
            CHECK(!r.cancelled);
            CHECK(r.totalOps > 0);
            CHECK(std::isfinite(r.score) && r.score > 0.0);
            CHECK(r.elapsedSeconds > 0.05);
        }
    }
}

// ====================================================================
// Thread affinity
// ====================================================================

TEST_CASE(allowed_cores_are_sorted_unique_and_non_empty) {
    const auto& cores = AllowedCores();
    REQUIRE(!cores.empty());
    CHECK(std::is_sorted(cores.begin(), cores.end()));
    CHECK(std::adjacent_find(cores.begin(), cores.end()) == cores.end());
}

TEST_CASE(pinning_accepts_allowed_cores_and_rejects_nonsense) {
    bool okAllowed = false, okAbsurd = true, okHuge = true;
    std::thread t([&] {
        okAllowed = PinCurrentThreadToCore(AllowedCores().back());
        okAbsurd = PinCurrentThreadToCore(100000);
        okHuge = PinCurrentThreadToCore(0xFFFFFFFFu);
    });
    t.join();
    CHECK(okAllowed);
    CHECK(!okAbsurd);
    CHECK(!okHuge);
}

TEST_CASE(default_multicore_thread_count_is_the_number_of_allowed_cores) {
    auto s = std::make_shared<Script>();
    ScriptedTest proto(s);
    BenchmarkResult r = BenchmarkRunner::RunMultiCore(proto, 0.1, 0);
    CHECK(r.threadsUsed == static_cast<int>(AllowedCores().size()));
}

// ====================================================================
// Manager under concurrency (most useful under ThreadSanitizer)
// ====================================================================

TEST_CASE(manager_survives_concurrent_enqueue_and_cancel) {
    BenchmarkManager m;
    std::atomic<bool> stop{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < 25; ++i) {
                auto s = std::make_shared<Script>();
                s->fixedWork = true;
                s->chunksBeforeComplete = 2;
                m.Enqueue(std::make_shared<ScriptedTest>(s), RunMode::SingleCore, 5.0);
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    std::thread canceller([&] {
        while (!stop.load()) {
            m.CancelAll();
            std::this_thread::sleep_for(15ms);
        }
    });
    std::thread poller([&] {
        while (!stop.load()) {
            (void)m.GetCurrentProgress();
            (void)m.DrainResults();
            (void)m.QueueSize();
            std::this_thread::sleep_for(1ms);
        }
    });
    for (auto& t : producers) t.join();
    m.CancelAll();
    CHECK(WaitUntil([&] { return m.IsIdle(); }, 15000ms));
    stop = true;
    canceller.join();
    poller.join();
    CHECK(m.IsIdle());
}

// ====================================================================
// Disk test
// ====================================================================

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::random_device rd;
        char name[64];
        std::snprintf(name, sizeof(name), "brazen_selftest_%08x%08x", rd(), rd());
        path = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    int CountTestFiles() const {
        int n = 0;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(path, ec))
            if (e.path().filename().string().rfind("brazen_disktest_", 0) == 0) ++n;
        return n;
    }
};

constexpr uint64_t kMiB = 1024ull * 1024;

// Free bytes in `dir`, or 0 if unknown.
uint64_t FreeBytes(const std::filesystem::path& dir) {
    std::error_code ec;
    auto info = std::filesystem::space(dir, ec);
    return ec ? 0 : static_cast<uint64_t>(info.available);
}

} // namespace

TEST_CASE(disk_write_then_read_moves_exactly_the_requested_data_and_cleans_up) {
    TempDir dir;
    const uint64_t total = 16 * kMiB;
    SKIP_UNLESS(FreeBytes(dir.path) > total * 4 + DiskIoTest::kFreeSpaceReserveBytes, "not enough free disk space");
    for (DiskIoMode mode : {DiskIoMode::Write, DiskIoMode::Read}) {
        for (unsigned threads : {1u, 2u}) {
            DiskIoTest proto(dir.path, mode, total);
            BenchmarkResult r = threads == 1 ? BenchmarkRunner::RunSingleCore(proto, 60.0)
                                             : BenchmarkRunner::RunMultiCore(proto, 60.0, threads);
            CHECK(!r.failed);
            CHECK(!r.cancelled);
            CHECK(r.totalOps == total * threads); // every worker moves the whole file once
            CHECK(r.score > 0.0);
            CHECK(!r.notes.empty());
        }
    }
    CHECK(dir.CountTestFiles() == 0);
}

TEST_CASE(disk_test_reports_an_unusable_directory_as_a_failure_with_a_reason) {
    for (DiskIoMode mode : {DiskIoMode::Write, DiskIoMode::Read}) {
        DiskIoTest proto("/this/path/does/not/exist", mode, 8 * kMiB);
        BenchmarkResult r = BenchmarkRunner::RunSingleCore(proto, 30.0);
        CHECK(r.failed);
        CHECK(!r.notes.empty());
        CHECK(r.score == 0.0);
    }
}

TEST_CASE(disk_test_refuses_to_start_when_there_is_not_enough_free_space) {
    TempDir dir;
    uint64_t avail = FreeBytes(dir.path);
    // Request more than is free. (Skip on a machine so empty that the request
    // would exceed the 1 TB per-file cap and become a real 1 TB write.)
    SKIP_UNLESS(avail > 0 && avail < DiskIoTest::kMaxTotalBytes - 64 * 1024 * kMiB,
                "free space unknown or larger than the per-file cap");
    auto start = std::chrono::steady_clock::now();
    for (DiskIoMode mode : {DiskIoMode::Write, DiskIoMode::Read}) {
        DiskIoTest proto(dir.path, mode, avail + 8 * 1024 * kMiB);
        BenchmarkResult r = BenchmarkRunner::RunSingleCore(proto, 30.0);
        CHECK(r.failed);
        CHECK(r.notes.find("Not enough free space") != std::string::npos);
        CHECK(r.totalOps == 0);
    }
    CHECK(dir.CountTestFiles() == 0);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(secs < 5.0); // it must refuse up front, not write until the drive is full
}

TEST_CASE(disk_test_clamps_an_overflowing_size_request) {
    TempDir dir;
    uint64_t avail = FreeBytes(dir.path);
    SKIP_UNLESS(avail > 0 && avail < DiskIoTest::kMaxTotalBytes - 64 * 1024 * kMiB,
                "free space unknown or larger than the per-file cap");
    // UINT64_MAX used to wrap around inside the block-rounding arithmetic.
    DiskIoTest proto(dir.path, DiskIoMode::Write, std::numeric_limits<uint64_t>::max());
    BenchmarkResult r = BenchmarkRunner::RunSingleCore(proto, 30.0);
    CHECK(r.failed);
    CHECK(r.notes.find("Not enough free space") != std::string::npos);
}

TEST_CASE(disk_read_preparation_can_be_cancelled_and_leaves_nothing_behind) {
    TempDir dir;
    SKIP_UNLESS(FreeBytes(dir.path) > 3 * 1024 * kMiB, "needs about 3 GB free disk space");
    DiskIoTest proto(dir.path, DiskIoMode::Read, 2 * 1024 * kMiB);
    std::atomic<bool> cancel{false};
    BenchmarkResult r;
    std::thread runner([&] { r = BenchmarkRunner::RunSingleCore(proto, 60.0, &cancel); });
    std::this_thread::sleep_for(150ms);
    cancel = true;
    runner.join();
    CHECK(r.cancelled);
    CHECK(!r.failed);
    // The prototype owns the shared fixture; it is removed when it goes away.
}

TEST_CASE(disk_test_sweeps_stale_leftovers_but_not_recent_or_unrelated_files) {
    namespace fs = std::filesystem;
    TempDir dir;
    auto touch = [&](const char* name, std::chrono::minutes age) {
        fs::path p = dir.path / name;
        std::FILE* f = std::fopen(p.string().c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fputs("x", f);
        std::fclose(f);
        fs::last_write_time(p, fs::file_time_type::clock::now() - age);
    };
    touch("brazen_disktest_aaaaaaaa_00000001.tmp", std::chrono::minutes(120)); // stale leftover
    touch(".brazen_write_probe_aaaaaaaa_00000001.tmp", std::chrono::minutes(120)); // stale probe
    touch("brazen_disktest_bbbbbbbb_00000002.tmp", std::chrono::minutes(1));   // possibly another live run
    touch("notes.tmp", std::chrono::minutes(120));                              // not ours
    touch("brazen_disktest_cccccccc_00000003.txt", std::chrono::minutes(120)); // wrong extension

    DiskIoTest proto(dir.path, DiskIoMode::Write, 8 * kMiB);
    BenchmarkResult r = BenchmarkRunner::RunSingleCore(proto, 30.0);
    CHECK(!r.failed);
    CHECK(!fs::exists(dir.path / "brazen_disktest_aaaaaaaa_00000001.tmp"));
    CHECK(!fs::exists(dir.path / ".brazen_write_probe_aaaaaaaa_00000001.tmp"));
    CHECK(fs::exists(dir.path / "brazen_disktest_bbbbbbbb_00000002.tmp"));
    CHECK(fs::exists(dir.path / "notes.tmp"));
    CHECK(fs::exists(dir.path / "brazen_disktest_cccccccc_00000003.txt"));
}

TEST_CASE(disk_test_failure_in_one_worker_stops_the_others_quickly) {
    // Multi-core write to a missing directory: every worker fails in Setup;
    // the run must end immediately rather than waiting for the time limit.
    DiskIoTest proto("/this/path/does/not/exist", DiskIoMode::Write, 64 * kMiB);
    auto start = std::chrono::steady_clock::now();
    BenchmarkResult r = BenchmarkRunner::RunMultiCore(proto, 120.0, 4);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(r.failed);
    CHECK(secs < 5.0);
}

int main(int argc, char** argv) { return selftest::Main(argc, argv); }
