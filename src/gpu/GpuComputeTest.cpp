#include "GpuComputeTest.h"

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

// Escape-time-style iterative loop per pixel. Each iteration performs a
// small, fixed amount of floating point work (see kApproxFlopsPerIteration
// in the header for how that's counted), and the accumulator is folded
// into the output color so the GLSL compiler can't dead-code-eliminate
// the loop as unused work.
const char* kFragmentShaderSrc = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform int uIterations;

void main() {
    vec2 c = vUV * 2.0 - 1.0;
    vec2 z = vec2(0.0);
    float acc = 0.0;
    for (int i = 0; i < uIterations; ++i) {
        float x = z.x * z.x - z.y * z.y + c.x;
        float y = 2.0 * z.x * z.y + c.y;
        z = vec2(x, y);
        acc += dot(z, z);
    }
    FragColor = vec4(fract(acc), fract(acc * 0.5), fract(acc * 0.25), 1.0);
}
)GLSL";

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

    m_initialized = true;
    return true;
}

bool GpuComputeTest::CreateFramebuffer(int width, int height, std::string* errorOut) {
    // Small offscreen render target -- deliberately not the default
    // framebuffer, so this test never touches what's actually on screen;
    // RunDraw() restores framebuffer 0 before returning so ImGui's own
    // rendering later in the frame is completely unaffected.
    GLuint texture = 0, fbo = 0;
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D_, texture);
    gl.TexImage2D(GL_TEXTURE_2D_, 0, static_cast<GLint>(GL_RGBA8_), width, height, 0,
                  GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
    gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, static_cast<GLint>(GL_NEAREST_));
    gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, static_cast<GLint>(GL_NEAREST_));

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

void GpuComputeTest::RunDraw(int iterations) {
    if (!m_initialized) return;

    gl.BindFramebuffer(GL_FRAMEBUFFER_, m_fbo);
    gl.Viewport(0, 0, m_width, m_height);
    gl.UseProgram(m_program);
    if (m_uniformIterations >= 0) gl.Uniform1i(m_uniformIterations, iterations);
    gl.BindVertexArray(m_vao);
    gl.DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
    gl.BindVertexArray(0);

    // Force the GPU to actually finish this draw before we return, so the
    // caller's wall-clock measurement around RunDraw() reflects real
    // execution time rather than just how fast the driver accepted the
    // command (GL calls are asynchronous by default).
    gl.Finish();

    gl.BindFramebuffer(GL_FRAMEBUFFER_, 0);
}

void GpuComputeTest::Shutdown() {
    DestroyFramebuffer();
    if (m_vbo) { gl.DeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_vao) { gl.DeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_program) { gl.DeleteProgram(m_program); m_program = 0; }
    m_initialized = false;
}

} // namespace brazen
