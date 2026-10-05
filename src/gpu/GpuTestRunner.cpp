#include "GpuTestRunner.h"
#include "../core/Log.h"
#include <algorithm>
#include <cstdio>

namespace brazen {

bool GpuTestRunner::Init(GLGetProcAddressFn getProcAddress) {
    if (!LoadGLFunctions(getProcAddress)) {
        m_error = "Failed to resolve required OpenGL 3.3 core functions on this system.";
        m_supported = false;
        return false;
    }
    std::string err;
    if (!m_test.Init(&err)) {
        m_error = err;
        m_supported = false;
        return false;
    }

    const unsigned char* vendor = gl.GetString(GL_VENDOR_);
    const unsigned char* renderer = gl.GetString(GL_RENDERER_);
    m_vendor = vendor ? reinterpret_cast<const char*>(vendor) : "Unknown";
    m_renderer = renderer ? reinterpret_cast<const char*>(renderer) : "Unknown";

    m_supported = true;
    return true;
}

void GpuTestRunner::Shutdown() {
    if (m_supported) m_test.Shutdown();
}

bool GpuTestRunner::SetResolution(int width, int height) {
    if (!m_supported || m_state != State::Idle) return false; // don't resize mid-run
    std::string err;
    if (!m_test.SetResolution(width, height, &err)) {
        m_error = err;
        return false;
    }
    return true;
}

void GpuTestRunner::RequestRun(double durationSeconds, unsigned workloadMask) {
    if (!m_supported || m_state != State::Idle) return;
    if ((workloadMask & 0x7u) == 0) return;
    // NaN/negative/absurd durations would end a workload instantly or never.
    if (!(durationSeconds > 0.0)) durationSeconds = 1.0;
    if (durationSeconds > 3600.0) durationSeconds = 3600.0;
    m_requestedDuration = durationSeconds;
    m_workloadMask = workloadMask & 0x7u;
    m_state = State::Calibrating;
    m_cancelRequested = false;
    m_elapsedSeconds = 0.0;
    m_totalOps = 0;
    m_workloadIndex = 0;
    BeginWorkload();
}

void GpuTestRunner::Cancel() {
    if (m_state != State::Idle) m_cancelRequested = true;
}

std::string GpuTestRunner::StatusText() const {
    if (m_state == State::Calibrating) return "Calibrating...";
    if (m_state == State::Running) {
        char buf[64];
        double wallSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_workloadStart).count();
        int selectedTotal = 0;
        int selectedOrdinal = 0;
        for (int i = 0; i < 3; ++i) {
            if (!(m_workloadMask & (1u << i))) continue;
            ++selectedTotal;
            if (i <= m_workloadIndex) selectedOrdinal = selectedTotal;
        }
        snprintf(buf, sizeof(buf), "%s %d/%d... %.1fs / %.0fs", WorkloadName(),
                 selectedOrdinal, selectedTotal, wallSeconds, m_requestedDuration);
        return buf;
    }
    return "";
}

namespace {
// Longest a single draw may block the UI thread (and the GPU) before the
// run is abandoned. Windows resets a GPU that stays busy for ~2 s (TDR),
// which would take the display driver, and our window, down with it.
constexpr double kMaxChunkSeconds = 1.0;
// How long one timed chunk should take.
constexpr double kTargetChunkSeconds = 0.015;
// A chunk must be at least this long before comparing timer-query time with
// wall-clock time is meaningful (very short draws are dominated by fixed
// submission/synchronisation overhead on the wall-clock side).
constexpr double kMinComparableSeconds = 0.002;
} // namespace

bool GpuTestRunner::Calibrate(std::string* error) {
    const bool isFill = m_test.GetWorkload() == GpuComputeTest::Workload::Fill;
    const int kMin = isFill ? 1 : 4;
    const int kMax = isFill ? 200'000 : 2'000'000;
    int iters = kMin;

    // 1. Ramp up from a tiny draw until one is long enough to time, so the
    //    very first draws are short even on a slow GPU or at a high
    //    resolution (a fixed starting size could block the UI for seconds).
    //
    //    The very first draw is a warm-up and is *not* held to the speed
    //    limit: many drivers compile or specialise the shader lazily on the
    //    first draw (a software rasterizer can spend about a second on it),
    //    which says nothing about how fast the GPU actually is.
    GpuComputeTest::DrawTiming t = m_test.RunDraw(iters);
    if (!t.ok) { *error = t.error; return false; }
    if (t.wallSeconds > 10.0) {
        *error = "The first GPU draw took " + std::to_string(static_cast<int>(t.wallSeconds)) +
                 " s (shader compilation or an unresponsive driver).";
        return false;
    }
    t = m_test.RunDraw(iters);
    for (int step = 0; step < 16; ++step) {
        if (!t.ok) { *error = t.error; return false; }
        if (t.wallSeconds > kMaxChunkSeconds) {
            *error = "The GPU is too slow for this resolution (one minimal draw took " +
                     std::to_string(static_cast<int>(t.wallSeconds * 1000.0)) +
                     " ms). Choose a lower resolution.";
            return false;
        }
        if (t.wallSeconds >= kMinComparableSeconds || iters >= kMax) break;
        iters = std::min(kMax, iters * 4);
        t = m_test.RunDraw(iters);
    }

    // 2. Refine toward the target chunk length, and decide which clock to
    //    trust. The timer query is normally exact, but some drivers (e.g.
    //    software rasterizers) report a fraction of the real time, which
    //    would inflate scores many times over. We always wait for each draw
    //    to finish, so the query must roughly agree with wall-clock time.
    bool useQuery = false;
    for (int round = 0; round < 5; ++round) {
        double wall = 0.0, gpu = 0.0;
        constexpr int kSamples = 2;
        for (int i = 0; i < kSamples; ++i) {
            t = m_test.RunDraw(iters);
            if (!t.ok) { *error = t.error; return false; }
            wall += t.wallSeconds;
            gpu += t.gpuSeconds;
        }
        wall /= kSamples;
        gpu /= kSamples;
        if (wall > kMaxChunkSeconds) {
            if (iters <= kMin) {
                *error = "The GPU is too slow for this resolution (one minimal draw took " +
                         std::to_string(static_cast<int>(wall * 1000.0)) + " ms). Choose a lower resolution.";
                return false;
            }
            iters = std::max(kMin, static_cast<int>(iters * (kTargetChunkSeconds / wall)));
            continue;
        }
        useQuery = m_test.HasGpuTimer() && gpu > 0.0 && wall >= kMinComparableSeconds && gpu >= 0.5 * wall;
        double measured = useQuery ? gpu : wall;
        if (measured <= 0.0) measured = 1e-6;
        if (measured >= 0.6 * kTargetChunkSeconds && measured <= 1.6 * kTargetChunkSeconds) break;
        double scale = std::max(0.1, std::min(8.0, kTargetChunkSeconds / measured));
        long long scaled = static_cast<long long>(static_cast<double>(iters) * scale);
        iters = static_cast<int>(std::max<long long>(kMin, std::min<long long>(scaled, kMax)));
    }

    m_iterationsPerDraw = iters;
    m_useGpuTimer = useQuery;
    if (m_test.HasGpuTimer() && !useQuery)
        LogWarn("GPU", "%s: this driver's GPU timer disagrees with wall-clock time, so wall-clock timing is used instead.",
                WorkloadName());
    LogInfo("GPU", "%s calibrated: %d %s per chunk, timing source: %s; measuring for %.0f s.", WorkloadName(),
            m_iterationsPerDraw, isFill ? "full-target draws" : "shader iterations",
            m_useGpuTimer ? "GPU timer query" : "wall clock", m_requestedDuration);
    return true;
}

bool GpuTestRunner::PollAndAdvance(BenchmarkResult* outResult) {
    if (!m_supported || m_state == State::Idle) return false;

    if (m_cancelRequested) {
        FinalizeResult(outResult, /*cancelled=*/true);
        return true;
    }

    using clock = std::chrono::steady_clock;

    if (m_state == State::Calibrating) {
        std::string error;
        if (!Calibrate(&error)) {
            FailRun(outResult, error);
            return true;
        }
        // Calibration is excluded from the user-requested workload duration.
        m_workloadStart = clock::now();
        m_state = State::Running;
        return false;
    }

    // Running: one timed chunk per call.
    GpuComputeTest::DrawTiming t = m_test.RunDraw(m_iterationsPerDraw);
    if (!t.ok) {
        FailRun(outResult, t.error);
        return true;
    }
    if (t.wallSeconds > 2.0 * kMaxChunkSeconds) {
        FailRun(outResult, "A single draw took " + std::to_string(static_cast<int>(t.wallSeconds * 1000.0)) +
                               " ms; stopping before the driver's watchdog resets the GPU.");
        return true;
    }
    double seconds = (m_useGpuTimer && t.gpuSeconds > 0.0) ? t.gpuSeconds : t.wallSeconds;
    m_elapsedSeconds += seconds;

    uint64_t pixels = static_cast<uint64_t>(m_test.Width()) * static_cast<uint64_t>(m_test.Height());
    if (m_test.GetWorkload() == GpuComputeTest::Workload::Alu) {
        m_totalOps += pixels * static_cast<uint64_t>(m_iterationsPerDraw) *
                      static_cast<uint64_t>(GpuComputeTest::kApproxFlopsPerIteration);
    } else if (m_test.GetWorkload() == GpuComputeTest::Workload::Texture) {
        m_totalOps += pixels * static_cast<uint64_t>(m_iterationsPerDraw) *
                      GpuComputeTest::kTextureBytesPerIteration;
    } else {
        m_totalOps += pixels * static_cast<uint64_t>(m_iterationsPerDraw); // one full-target draw each
    }

    double wallSeconds = std::chrono::duration<double>(clock::now() - m_workloadStart).count();
    if (wallSeconds >= m_requestedDuration) {
        FinalizeResult(outResult, /*cancelled=*/false);
        return true;
    }
    return false;
}

void GpuTestRunner::FailRun(BenchmarkResult* outResult, const std::string& reason) {
    if (outResult) {
        *outResult = BenchmarkResult();
        outResult->testName = WorkloadName();
        outResult->mode = RunMode::Gpu;
        outResult->threadsUsed = 1;
        outResult->affinityPinned = true;
        outResult->failed = true;
        outResult->notes = reason;
        switch (m_test.GetWorkload()) {
            case GpuComputeTest::Workload::Alu: outResult->unit = "GFLOPS"; break;
            case GpuComputeTest::Workload::Texture: outResult->unit = "GB/s"; break;
            case GpuComputeTest::Workload::Fill: outResult->unit = "Gpixels/s"; break;
        }
    }
    LogError("GPU", "%s failed: %s", WorkloadName(), reason.c_str());
    m_state = State::Idle; // skip any remaining workloads: the GPU path is unusable
    m_cancelRequested = false;
}

void GpuTestRunner::FinalizeResult(BenchmarkResult* outResult, bool cancelled) {
    if (outResult) {
        *outResult = BenchmarkResult();
        outResult->testName = WorkloadName();
        outResult->mode = RunMode::Gpu;
        outResult->threadsUsed = 1;
        outResult->elapsedSeconds = m_elapsedSeconds;
        outResult->totalOps = m_totalOps;
        outResult->cancelled = cancelled;
        // Core affinity isn't a meaningful concept for GPU work; mark it
        // "pinned" so the results table doesn't show a misleading
        // "unpinned, noisy result" warning that only applies to CPU tests.
        outResult->affinityPinned = true;
        switch (m_test.GetWorkload()) {
            case GpuComputeTest::Workload::Alu: outResult->unit = "GFLOPS"; break;
            case GpuComputeTest::Workload::Texture: outResult->unit = "GB/s"; break;
            case GpuComputeTest::Workload::Fill: outResult->unit = "Gpixels/s"; break;
        }
        outResult->score = (m_elapsedSeconds > 0.0)
            ? (static_cast<double>(m_totalOps) / m_elapsedSeconds) / 1'000'000'000.0 : 0.0;
        if (!cancelled && (m_totalOps == 0 || !(m_elapsedSeconds > 0.0))) {
            outResult->failed = true;
            outResult->score = 0.0;
            outResult->notes = "The GPU test finished without producing any timing data.";
        } else {
            outResult->notes = std::string("Timing: ") +
                (m_useGpuTimer ? "GPU timer query" : "wall clock") + ", " +
                std::to_string(m_iterationsPerDraw) +
                (m_test.GetWorkload() == GpuComputeTest::Workload::Fill ? " draws" : " iterations") + " per chunk";
        }
    }
    if (!cancelled && m_workloadIndex < 2 && !(outResult && outResult->failed)) {
        ++m_workloadIndex;
        BeginWorkload();
    } else {
        m_state = State::Idle;
    }
    m_cancelRequested = false;
}

void GpuTestRunner::BeginWorkload() {
    while (m_workloadIndex < 3 && !(m_workloadMask & (1u << m_workloadIndex)))
        ++m_workloadIndex;
    if (m_workloadIndex >= 3) {
        m_state = State::Idle;
        return;
    }
    m_elapsedSeconds = 0.0;
    m_totalOps = 0;
    m_workloadStart = std::chrono::steady_clock::now();
    auto workload = static_cast<GpuComputeTest::Workload>(m_workloadIndex);
    m_test.SetWorkload(workload);
    LogInfo("GPU", "Starting %s (calibrating first)...", WorkloadName());
    m_state = State::Calibrating;
}

const char* GpuTestRunner::WorkloadName() const {
    switch (m_test.GetWorkload()) {
        case GpuComputeTest::Workload::Alu: return "GPU ALU";
        case GpuComputeTest::Workload::Texture: return "GPU Texture";
        case GpuComputeTest::Workload::Fill: return "GPU Fill Rate";
    }
    return "GPU";
}

} // namespace brazen
