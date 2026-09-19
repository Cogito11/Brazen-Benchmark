#pragma once
#include "GLLoader.h"
#include "GpuComputeTest.h"
#include "../core/BenchmarkRunner.h"
#include <chrono>
#include <string>

namespace brazen {

// Drives a small GPU workload suite from the main/render thread, one work
// chunk per call to PollAndAdvance(). This is intentionally NOT run
// through BenchmarkManager's background thread pool: an OpenGL context
// can only be current on (and safely driven from) one thread at a time,
// so GPU work has to happen on whichever thread owns the GL context --
// here, that's the same thread running the render loop in main.cpp.
//
// A side effect worth knowing: each work chunk waits for GPU completion so
// the result is measured rather than merely queued, so the UI renders at a
// reduced frame rate while a GPU run is in progress.
class GpuTestRunner {
public:
    // Loads the required GL functions and compiles/links the test's
    // shaders. Call once, on the thread with the GL context current,
    // after the context is created. Returns false (see GetError()) if
    // this GPU/driver doesn't support what's needed -- e.g. no OpenGL
    // 3.3 core support -- in which case the GPU test simply won't be
    // offered rather than crashing.
    bool Init(GLGetProcAddressFn getProcAddress);
    void Shutdown();

    bool IsSupported() const { return m_supported; }
    const std::string& GetError() const { return m_error; }

    // GL_VENDOR / GL_RENDERER strings, e.g. "NVIDIA Corporation" /
    // "NVIDIA GeForce RTX 4070/PCIe/SSE2". Populated by Init() -- these
    // are legacy OpenGL 1.0 queries so they don't need the custom
    // loader, but are grouped here since GpuTestRunner already owns "GL
    // context is current" timing knowledge.
    const std::string& GetVendor() const { return m_vendor; }
    const std::string& GetRenderer() const { return m_renderer; }

    // Changes the render target resolution for future runs (256/512/
    // 1024/2048 are reasonable presets). Ignored while a run is in
    // progress -- call this between runs, not mid-run.
    bool SetResolution(int width, int height);
    int Width() const { return m_test.Width(); }
    int Height() const { return m_test.Height(); }

    // Starts a run; ignored if one is already in progress or the GPU
    // test isn't supported on this system. workloadMask uses bit 0 for
    // ALU, bit 1 for texture, and bit 2 for fill rate.
    void RequestRun(double durationSeconds, unsigned workloadMask = 0x7u);
    void Cancel();
    bool IsBusy() const { return m_state != State::Idle; }
    std::string StatusText() const;

    // Advances the run by one step (a calibration draw, or one timed
    // work chunk). Must be called every frame from the GL thread with
    // framebuffer 0 bound on entry; guarantees framebuffer 0 is bound
    // again on return so the caller's own rendering (ImGui) is
    // unaffected. Returns true and fills *outResult on the frame a run
    // finishes (completed or cancelled).
    bool PollAndAdvance(BenchmarkResult* outResult);

private:
    enum class State { Idle, Calibrating, Running };

    void FinalizeResult(BenchmarkResult* outResult, bool cancelled);
    void BeginWorkload();
    const char* WorkloadName() const;

    GpuComputeTest m_test;
    bool m_supported = false;
    std::string m_error;
    std::string m_vendor;
    std::string m_renderer;

    State m_state = State::Idle;
    bool m_cancelRequested = false;
    double m_requestedDuration = 5.0;
    int m_iterationsPerDraw = 200; // set by calibration at the start of each run
    double m_elapsedSeconds = 0.0;
    uint64_t m_totalOps = 0;
    int m_workloadIndex = 0;
    unsigned m_workloadMask = 0x7u;
    std::chrono::steady_clock::time_point m_workloadStart;
};

} // namespace brazen
