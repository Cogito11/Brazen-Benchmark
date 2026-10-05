#include "GpuComputeTest.h"
#include <chrono>
#include <cstdio>
#include <vector>

namespace brazen {
namespace {

const char* kVertexShaderSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// Three deliberately different fragment workloads share one program so the
// suite remains compatible with OpenGL 3.3: ALU arithmetic, texture sampling,
// and a light fill-rate path. The selected uniform branch is constant for a
// run, and each result is labeled separately rather than pretending to be a
// universal GPU score.
const char* kFragmentShaderSrc = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform int uIterations;
uniform int uWorkload;
uniform sampler2D uSource;

void main() {
    float acc = 0.0;
    if (uWorkload == 0) {
        vec2 c = vUV * 2.0 - 1.0;
        vec2 z = vec2(0.0);
        for (int i = 0; i < uIterations; ++i) {
            float x = z.x * z.x - z.y * z.y + c.x;
            float y = 2.0 * z.x * z.y + c.y;
            z = vec2(x, y);
            acc += dot(z, z);
        }
    } else if (uWorkload == 1) {
        for (int i = 0; i < uIterations; ++i) {
            vec2 uv = fract(vUV + vec2(float(i) * 0.013, float(i) * 0.007));
            acc += dot(texture(uSource, uv), vec4(0.25));
            acc += dot(texture(uSource, uv * 0.73), vec4(0.25));
            acc += dot(texture(uSource, uv * 1.31), vec4(0.25));
            acc += dot(texture(uSource, uv * 1.91), vec4(0.25));
        }
    } else {
        // Keep this path intentionally light: the measured quantity is the
        // number of rasterized pixels per second, not ALU work.
        acc = vUV.x * 0.25 + vUV.y * 0.5;
    }
    FragColor = vec4(fract(acc), fract(acc * 0.5), fract(acc * 0.25), 1.0);
}
)GLSL";

// Discards any GL errors left over from earlier calls (e.g. by another
// part of the app) so that an error seen after our own work is really ours.
void ClearGlErrors() {
    if (!gl.GetError) return;
    for (int i = 0; i < 16 && gl.GetError() != GL_NO_ERROR_; ++i) {}
}

std::string DescribeGlError(GLenum error) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "OpenGL error 0x%04X%s", static_cast<unsigned>(error),
                  error == GL_OUT_OF_MEMORY_ ? " (out of memory)" : "");
    return buf;
}

bool CompileShader(GLenum type, const char* source, GLuint* outShader, std::string* err) {
    GLuint shader = gl.CreateShader(type);
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);

    GLint success = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS_, &success);
    if (!success) {
        char log[1024];
        GLsizei len = 0;
        gl.GetShaderInfoLog(shader, sizeof(log), &len, log);
        if (err) *err = std::string("Shader compile error: ") + std::string(log, static_cast<size_t>(len));
        gl.DeleteShader(shader);
        return false;
    }
    *outShader = shader;
    return true;
}

} // namespace

bool GpuComputeTest::Init(std::string* errorOut) {
    GLuint vs = 0, fs = 0;
    if (!CompileShader(GL_VERTEX_SHADER_, kVertexShaderSrc, &vs, errorOut)) return false;
    if (!CompileShader(GL_FRAGMENT_SHADER_, kFragmentShaderSrc, &fs, errorOut)) {
        gl.DeleteShader(vs);
        return false;
    }

    m_program = gl.CreateProgram();
    gl.AttachShader(m_program, vs);
    gl.AttachShader(m_program, fs);
    gl.LinkProgram(m_program);

    GLint linked = 0;
    gl.GetProgramiv(m_program, GL_LINK_STATUS_, &linked);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    if (!linked) {
        char log[1024];
        GLsizei len = 0;
        gl.GetProgramInfoLog(m_program, sizeof(log), &len, log);
        if (errorOut) *errorOut = std::string("Program link error: ") + std::string(log, static_cast<size_t>(len));
        gl.DeleteProgram(m_program);
        m_program = 0;
        return false;
    }
    m_uniformIterations = gl.GetUniformLocation(m_program, "uIterations");
    m_uniformWorkload = gl.GetUniformLocation(m_program, "uWorkload");
    m_uniformSource = gl.GetUniformLocation(m_program, "uSource");

    // Fullscreen quad as a triangle strip (4 verts covering NDC [-1,1]).
    float verts[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    gl.GenVertexArrays(1, &m_vao);
    gl.BindVertexArray(m_vao);
    gl.GenBuffers(1, &m_vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER_, m_vbo);
    gl.BufferData(GL_ARRAY_BUFFER_, static_cast<GLsizeiptr>(sizeof(verts)), verts, GL_STATIC_DRAW_);
    gl.VertexAttribPointer(0, 2, GL_FLOAT_, GL_FALSE_, 0, nullptr);
    gl.EnableVertexAttribArray(0);
    gl.BindVertexArray(0);

    if (!CreateFramebuffer(m_width, m_height, errorOut)) {
        Shutdown();
        return false;
    }
    if (!CreateSourceTexture(errorOut)) {
        Shutdown();
        return false;
    }
    GLint extensionCount = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS_, &extensionCount);
    bool timerExtension = false;
    for (GLint i = 0; i < extensionCount && !timerExtension; ++i) {
        const char* extension = reinterpret_cast<const char*>(gl.GetStringi(GL_EXTENSIONS_, static_cast<GLuint>(i)));
        timerExtension = extension &&
            (std::string(extension) == "GL_ARB_timer_query" || std::string(extension) == "GL_EXT_timer_query");
    }
    if (timerExtension && gl.GenQueries && gl.DeleteQueries && gl.BeginQuery && gl.EndQuery && gl.GetQueryObjectui64v) {
        gl.GenQueries(1, &m_query);
    }

    m_initialized = true;
    return true;
}

bool GpuComputeTest::CreateSourceTexture(std::string* errorOut) {
    constexpr int size = 1024;
    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            size_t offset = (static_cast<size_t>(y) * size + x) * 4;
            pixels[offset + 0] = static_cast<unsigned char>((x * 13 + y * 7) & 0xFF);
            pixels[offset + 1] = static_cast<unsigned char>((x * 3 + y * 17) & 0xFF);
            pixels[offset + 2] = static_cast<unsigned char>((x ^ y) & 0xFF);
            pixels[offset + 3] = 255;
        }
    }
    gl.GenTextures(1, &m_sourceTexture);
    gl.BindTexture(GL_TEXTURE_2D_, m_sourceTexture);
    gl.TexImage2D(GL_TEXTURE_2D_, 0, static_cast<GLint>(GL_RGBA8_), size, size, 0,
                  GL_RGBA_, GL_UNSIGNED_BYTE_, pixels.data());
    gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, static_cast<GLint>(GL_LINEAR_));
    gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, static_cast<GLint>(GL_LINEAR_));
    gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, static_cast<GLint>(GL_CLAMP_TO_EDGE_));
    gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, static_cast<GLint>(GL_CLAMP_TO_EDGE_));
    gl.BindTexture(GL_TEXTURE_2D_, 0);
    if (gl.GetError && gl.GetError() != GL_NO_ERROR_) {
        if (errorOut) *errorOut = "GPU source texture creation failed";
        gl.DeleteTextures(1, &m_sourceTexture);
        m_sourceTexture = 0;
        return false;
    }
    return true;
}

bool GpuComputeTest::CreateFramebuffer(int width, int height, std::string* errorOut) {
    // Small offscreen render target -- deliberately not the default
    // framebuffer, so this test never touches what's actually on screen;
    // RunDraw() restores framebuffer 0 before returning so ImGui's own
    // rendering later in the frame is completely unaffected.
    GLint maxSize = 1024; // the minimum a GL 3.3 driver must support
    if (gl.GetIntegerv) gl.GetIntegerv(GL_MAX_TEXTURE_SIZE_, &maxSize);
    int limit = maxSize < kMaxTargetSize ? static_cast<int>(maxSize) : kMaxTargetSize;
    if (width < kMinTargetSize || height < kMinTargetSize || width > limit || height > limit) {
        if (errorOut)
            *errorOut = "Render target size " + std::to_string(width) + "x" + std::to_string(height) +
                        " is outside the supported range (" + std::to_string(kMinTargetSize) + " to " +
                        std::to_string(limit) + " pixels per side on this GPU).";
        return false;
    }

    ClearGlErrors();
    GLuint texture = 0, fbo = 0;
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D_, texture);
    gl.TexImage2D(GL_TEXTURE_2D_, 0, static_cast<GLint>(GL_RGBA8_), width, height, 0,
                  GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
    GLenum allocError = gl.GetError ? gl.GetError() : GL_NO_ERROR_;
    gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, static_cast<GLint>(GL_NEAREST_));
    gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, static_cast<GLint>(GL_NEAREST_));
    if (allocError != GL_NO_ERROR_) {
        gl.BindTexture(GL_TEXTURE_2D_, 0);
        gl.DeleteTextures(1, &texture);
        if (errorOut) *errorOut = "Couldn't allocate the render target: " + DescribeGlError(allocError);
        return false;
    }

    gl.GenFramebuffers(1, &fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER_, fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, texture, 0);
    GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER_);
    gl.BindFramebuffer(GL_FRAMEBUFFER_, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE_) {
        if (errorOut) *errorOut = "Offscreen framebuffer incomplete";
        gl.DeleteFramebuffers(1, &fbo);
        gl.DeleteTextures(1, &texture);
        return false;
    }

    m_fbo = fbo;
    m_texture = texture;
    m_width = width;
    m_height = height;
    return true;
}

void GpuComputeTest::DestroyFramebuffer() {
    if (m_fbo) { gl.DeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
    if (m_texture) { gl.DeleteTextures(1, &m_texture); m_texture = 0; }
}

bool GpuComputeTest::SetResolution(int width, int height, std::string* errorOut) {
    if (!m_initialized) return false;
    if (width == m_width && height == m_height) return true; // no-op, already this size

    GLuint oldFbo = m_fbo, oldTexture = m_texture;
    m_fbo = 0;
    m_texture = 0;
    if (!CreateFramebuffer(width, height, errorOut)) {
        // Restore the old (still-valid) framebuffer rather than leaving
        // the test in a broken, un-renderable state.
        m_fbo = oldFbo;
        m_texture = oldTexture;
        return false;
    }
    if (oldFbo) gl.DeleteFramebuffers(1, &oldFbo);
    if (oldTexture) gl.DeleteTextures(1, &oldTexture);
    return true;
}

GpuComputeTest::DrawTiming GpuComputeTest::RunDraw(int iterations) {
    using clock = std::chrono::steady_clock;
    DrawTiming out;
    if (!m_initialized) {
        out.ok = false;
        out.error = "The GPU test isn't initialized.";
        return out;
    }
    if (iterations < 1) iterations = 1;
    ClearGlErrors();

    gl.BindFramebuffer(GL_FRAMEBUFFER_, m_fbo);
    gl.Viewport(0, 0, m_width, m_height);
    gl.UseProgram(m_program);
    if (m_uniformIterations >= 0) gl.Uniform1i(m_uniformIterations, iterations);
    if (m_uniformWorkload >= 0) gl.Uniform1i(m_uniformWorkload, static_cast<int>(m_workload));
    if (m_uniformSource >= 0) gl.Uniform1i(m_uniformSource, 0);
    gl.BindTexture(GL_TEXTURE_2D_, m_sourceTexture);
    gl.BindVertexArray(m_vao);

    // Fill rate: the shader is deliberately trivial, so one draw finishes in
    // microseconds -- shorter than the timer query's own resolution/latency.
    // Issue `iterations` full-target draws inside one timed region instead.
    const int draws = (m_workload == Workload::Fill) ? iterations : 1;

    auto start = clock::now();
    if (m_query) gl.BeginQuery(GL_TIME_ELAPSED_, m_query);
    for (int i = 0; i < draws; ++i) gl.DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
    if (m_query) {
        gl.EndQuery(GL_TIME_ELAPSED_);
        GLuint64 nanoseconds = 0;
        gl.GetQueryObjectui64v(m_query, GL_QUERY_RESULT_, &nanoseconds);
        out.gpuSeconds = static_cast<double>(nanoseconds) / 1'000'000'000.0;
    }
    // Always wait for completion so wallSeconds covers real GPU execution
    // (GL calls are asynchronous), even if a driver's query returns early.
    gl.Finish();
    out.wallSeconds = std::chrono::duration<double>(clock::now() - start).count();

    gl.BindVertexArray(0);
    gl.BindTexture(GL_TEXTURE_2D_, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER_, 0);

    // A failed draw "completes" instantly and would otherwise be scored as
    // an absurdly fast GPU, so a GL error invalidates the measurement.
    GLenum error = gl.GetError ? gl.GetError() : GL_NO_ERROR_;
    if (error != GL_NO_ERROR_) {
        out.ok = false;
        out.error = DescribeGlError(error) + " while rendering";
    }
    return out;
}

void GpuComputeTest::Shutdown() {
    DestroyFramebuffer();
    if (m_sourceTexture) { gl.DeleteTextures(1, &m_sourceTexture); m_sourceTexture = 0; }
    if (m_query) { gl.DeleteQueries(1, &m_query); m_query = 0; }
    if (m_vbo) { gl.DeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_vao) { gl.DeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_program) { gl.DeleteProgram(m_program); m_program = 0; }
    m_initialized = false;
}

} // namespace brazen
