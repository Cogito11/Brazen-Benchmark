#include "GpuTestRunner.h"
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

bool GpuTestRunner::PollAndAdvance(BenchmarkResult* outResult) {
    if (!m_supported || m_state == State::Idle) return false;

    if (m_cancelRequested) {
        FinalizeResult(outResult, /*cancelled=*/true);
        return true;
    }

    using clock = std::chrono::steady_clock;

    if (m_state == State::Calibrating) {
        // Warm-up + calibration: run a small, fixed iteration count and
        // measure how long it actually takes on this GPU, then scale up
        // to hit a ~15ms per-chunk target. Without this, a fixed
        // iteration count picked for, say, a discrete desktop GPU would
        // make a weak integrated GPU stall for a very long time per
        // chunk, while a count picked for a weak GPU would be dominated
        // by per-draw/glFinish overhead on a fast one.
        int calibrationIters = m_test.GetWorkload() == GpuComputeTest::Workload::Fill ? 1 : 50;
        double seconds = 0.0;
        for (int sample = 0; sample < 3; ++sample) {
            auto t0 = clock::now();
            double gpuSeconds = m_test.RunDraw(calibrationIters);
            auto t1 = clock::now();
            seconds += gpuSeconds > 0.0 ? gpuSeconds
                                        : std::chrono::duration<double>(t1 - t0).count();
        }
        seconds /= 3.0;
        if (seconds <= 0.0) seconds = 1e-6;

        constexpr double kTargetChunkSeconds = 0.015;
        double scale = kTargetChunkSeconds / seconds;
        long long scaled = static_cast<long long>(calibrationIters * scale);
        m_iterationsPerDraw = static_cast<int>(std::max<long long>(20, std::min<long long>(scaled, 2'000'000)));

        // Calibration is excluded from the user-requested workload duration.
        m_workloadStart = clock::now();
        m_state = State::Running;
        return false;
    }

    // Running: one timed chunk per call.
    auto t0 = clock::now();
    double gpuSeconds = m_test.RunDraw(m_iterationsPerDraw);
    auto t1 = clock::now();
    double seconds = gpuSeconds > 0.0 ? gpuSeconds
                                      : std::chrono::duration<double>(t1 - t0).count();
    m_elapsedSeconds += seconds;

    uint64_t pixels = static_cast<uint64_t>(m_test.Width()) * static_cast<uint64_t>(m_test.Height());
    if (m_test.GetWorkload() == GpuComputeTest::Workload::Alu) {
        m_totalOps += pixels * static_cast<uint64_t>(m_iterationsPerDraw) *
                      static_cast<uint64_t>(GpuComputeTest::kApproxFlopsPerIteration);
    } else if (m_test.GetWorkload() == GpuComputeTest::Workload::Texture) {
        m_totalOps += pixels * static_cast<uint64_t>(m_iterationsPerDraw) *
                      GpuComputeTest::kTextureBytesPerIteration;
    } else {
        m_totalOps += pixels;
    }

    double wallSeconds = std::chrono::duration<double>(clock::now() - m_workloadStart).count();
    if (wallSeconds >= m_requestedDuration) {
        FinalizeResult(outResult, /*cancelled=*/false);
        return true;
    }
    return false;
}

void GpuTestRunner::FinalizeResult(BenchmarkResult* outResult, bool cancelled) {
    if (outResult) {
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
        if (m_test.GetWorkload() == GpuComputeTest::Workload::Alu) {
            outResult->unit = "GFLOPS";
            outResult->score = (m_elapsedSeconds > 0.0)
                ? (static_cast<double>(m_totalOps) / m_elapsedSeconds) / 1'000'000'000.0 : 0.0;
        } else if (m_test.GetWorkload() == GpuComputeTest::Workload::Texture) {
            outResult->unit = "GB/s";
            outResult->score = (m_elapsedSeconds > 0.0)
                ? (static_cast<double>(m_totalOps) / m_elapsedSeconds) / 1'000'000'000.0 : 0.0;
        } else {
            outResult->unit = "Gpixels/s";
            outResult->score = (m_elapsedSeconds > 0.0)
                ? (static_cast<double>(m_totalOps) / m_elapsedSeconds) / 1'000'000'000.0 : 0.0;
        }
    }
    if (!cancelled && m_workloadIndex < 2) {
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
