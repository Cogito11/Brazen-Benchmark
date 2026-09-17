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

void GpuTestRunner::RequestRun(double durationSeconds) {
    if (!m_supported || m_state != State::Idle) return;
    m_requestedDuration = durationSeconds;
    m_state = State::Calibrating;
    m_cancelRequested = false;
    m_elapsedSeconds = 0.0;
    m_totalOps = 0;
}

void GpuTestRunner::Cancel() {
    if (m_state != State::Idle) m_cancelRequested = true;
}

std::string GpuTestRunner::StatusText() const {
    if (m_state == State::Calibrating) return "Calibrating...";
    if (m_state == State::Running) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Running... %.1fs / %.0fs", m_elapsedSeconds, m_requestedDuration);
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
        constexpr int kCalibrationIters = 50;
        auto t0 = clock::now();
        m_test.RunDraw(kCalibrationIters);
        auto t1 = clock::now();
        double seconds = std::chrono::duration<double>(t1 - t0).count();
        if (seconds <= 0.0) seconds = 1e-6;

        constexpr double kTargetChunkSeconds = 0.015;
        double scale = kTargetChunkSeconds / seconds;
        long long scaled = static_cast<long long>(kCalibrationIters * scale);
        m_iterationsPerDraw = static_cast<int>(std::max<long long>(20, std::min<long long>(scaled, 2'000'000)));

        m_state = State::Running;
        return false;
    }

    // Running: one timed chunk per call.
    auto t0 = clock::now();
    m_test.RunDraw(m_iterationsPerDraw);
    auto t1 = clock::now();
    m_elapsedSeconds += std::chrono::duration<double>(t1 - t0).count();

    uint64_t pixels = static_cast<uint64_t>(m_test.Width()) * static_cast<uint64_t>(m_test.Height());
    uint64_t flopsThisChunk = pixels * static_cast<uint64_t>(m_iterationsPerDraw) *
                               static_cast<uint64_t>(GpuComputeTest::kApproxFlopsPerIteration);
    m_totalOps += flopsThisChunk;

    if (m_elapsedSeconds >= m_requestedDuration) {
        FinalizeResult(outResult, /*cancelled=*/false);
        return true;
    }
    return false;
}

void GpuTestRunner::FinalizeResult(BenchmarkResult* outResult, bool cancelled) {
    if (outResult) {
        outResult->testName = "GPU Compute";
        outResult->unit = "GFLOPS";
        outResult->mode = RunMode::Gpu;
        outResult->threadsUsed = 1;
        outResult->elapsedSeconds = m_elapsedSeconds;
        outResult->totalOps = m_totalOps;
        outResult->cancelled = cancelled;
        // Core affinity isn't a meaningful concept for GPU work; mark it
        // "pinned" so the results table doesn't show a misleading
        // "unpinned, noisy result" warning that only applies to CPU tests.
        outResult->affinityPinned = true;
        outResult->score = (m_elapsedSeconds > 0.0)
            ? (static_cast<double>(m_totalOps) / m_elapsedSeconds) / 1'000'000'000.0
            : 0.0;
    }
    m_state = State::Idle;
    m_cancelRequested = false;
}

} // namespace brazen
