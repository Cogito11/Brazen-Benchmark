#pragma once
#include "GLLoader.h"
#include <cstdint>
#include <string>

namespace brazen {

// A small OpenGL 3.3-compatible GPU workload suite rendered through a
// fullscreen quad: arithmetic ALU, texture sampling, and fill rate. Each
// workload writes to an offscreen texture and is reported separately so a
// single synthetic score does not hide different GPU bottlenecks.
//
// This is a synthetic, relative benchmark -- like the CPU tests, it's
// meant for comparing this GPU against itself over time or against
// other GPUs running the same code, not as a substitute for vendor
// tools (FurMark, GPU-Z, etc).
class GpuComputeTest {
public:
    enum class Workload { Alu, Texture, Fill };

    bool Init(std::string* errorOut);
    void Shutdown();

    // Changes the offscreen render target size, recreating the texture
    // and framebuffer if the size actually differs from the current one.
    // Must be called after Init() and before the run currently in
    // progress starts (i.e. between runs, not mid-run) since it touches
    // the same GL objects RunDraw() uses. Returns false on failure (GL
    // resource creation error), leaving the previous resolution intact.
    bool SetResolution(int width, int height, std::string* errorOut);

    // Issues one draw call with the given per-pixel iteration count and
    // blocks (via glFinish) until the GPU has actually finished it, so
    // the caller's wall-clock timing around this call reflects real GPU
    // execution time rather than just command-submission time.
    void SetWorkload(Workload workload) { m_workload = workload; }
    Workload GetWorkload() const { return m_workload; }

    // Runs one draw and returns GPU time in seconds when timer queries are
    // available, or 0 when the caller should use its CPU-side fallback.
    double RunDraw(int iterations);
    bool HasGpuTimer() const { return m_query != 0; }

    int Width() const { return m_width; }
    int Height() const { return m_height; }

    // Approximate floating-point operations performed per loop iteration
    // in the fragment shader, counted by hand from the GLSL source (see
    // GpuComputeTest.cpp): a complex-number multiply/add step plus an
    // accumulation. This gives a rough "GFLOPS"-shaped number, not a
    // cycle-accurate instruction count -- GPU compilers can fuse
    // multiply-adds and reorder operations, so treat the resulting score
    // as a relative/comparative figure rather than an absolute one.
    static constexpr double kApproxFlopsPerIteration = 11.0;
    static constexpr uint64_t kTextureBytesPerIteration = 16;

    static constexpr int kDefaultWidth = 512;
    static constexpr int kDefaultHeight = 512;

private:
    bool CreateFramebuffer(int width, int height, std::string* errorOut);
    void DestroyFramebuffer();
    bool CreateSourceTexture(std::string* errorOut);

    int m_width = kDefaultWidth;
    int m_height = kDefaultHeight;

    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_fbo = 0;
    GLuint m_texture = 0;
    GLuint m_sourceTexture = 0;
    GLuint m_query = 0;
    GLint m_uniformIterations = -1;
    GLint m_uniformWorkload = -1;
    GLint m_uniformSource = -1;
    bool m_initialized = false;
    Workload m_workload = Workload::Alu;
};

} // namespace brazen
