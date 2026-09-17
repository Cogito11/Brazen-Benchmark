#pragma once
// A small, self-contained loader for the specific OpenGL 3.3 core-profile
// functions Brazen's GPU test needs.
//
// Why this exists instead of just including <GL/gl.h>: on essentially
// every platform, the system OpenGL header/library only guarantees
// symbols up to OpenGL 1.1 at link time (this is a long-standing
// Windows/driver-model quirk that Linux and macOS toolchains mirror for
// consistency). Anything newer -- shaders, VAOs, framebuffer objects,
// all of which this test needs -- has to be resolved at runtime via
// glfwGetProcAddress(), the same way libraries like GLAD/GLEW/GL3W work
// under the hood. Writing our own tiny loader for just the ~30 functions
// we actually use avoids pulling in and configuring a whole extra
// dependency for that.
//
// GL type definitions and enum values below are fixed by the Khronos
// OpenGL specification and don't vary by platform or vendor.

#include <cstddef>

#if defined(_WIN32)
    #define BRAZEN_GLAPI __stdcall
#else
    #define BRAZEN_GLAPI
#endif

namespace brazen {

// ---- Minimal GL type definitions (avoids depending on a system GL header) ----
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLsizeiptr = ptrdiff_t;

// ---- Constants used by this file (fixed by the GL spec) ----
constexpr GLenum GL_FALSE_ = 0;
constexpr GLenum GL_TRUE_ = 1;
constexpr GLenum GL_COLOR_BUFFER_BIT_ = 0x00004000;
constexpr GLenum GL_TRIANGLE_STRIP_ = 0x0005;
constexpr GLenum GL_FLOAT_ = 0x1406;
constexpr GLenum GL_RGBA_ = 0x1908;
constexpr GLenum GL_RGBA8_ = 0x8058;
constexpr GLenum GL_UNSIGNED_BYTE_ = 0x1401;
constexpr GLenum GL_TEXTURE_2D_ = 0x0DE1;
constexpr GLenum GL_TEXTURE_MIN_FILTER_ = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER_ = 0x2800;
constexpr GLenum GL_NEAREST_ = 0x2600;
constexpr GLenum GL_FRAMEBUFFER_ = 0x8D40;
constexpr GLenum GL_COLOR_ATTACHMENT0_ = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5;
constexpr GLenum GL_ARRAY_BUFFER_ = 0x8892;
constexpr GLenum GL_STATIC_DRAW_ = 0x88E4;
constexpr GLenum GL_VERTEX_SHADER_ = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER_ = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS_ = 0x8B81;
constexpr GLenum GL_LINK_STATUS_ = 0x8B82;
constexpr GLenum GL_NO_ERROR_ = 0;
constexpr GLenum GL_DEPTH_TEST_ = 0x0B71;
constexpr GLenum GL_VENDOR_ = 0x1F00;
constexpr GLenum GL_RENDERER_ = 0x1F01;

// ---- Function pointer typedefs (mirrors the real GL function signatures) ----
using PFN_glGenVertexArrays = void(BRAZEN_GLAPI*)(GLsizei, GLuint*);
using PFN_glBindVertexArray = void(BRAZEN_GLAPI*)(GLuint);
using PFN_glDeleteVertexArrays = void(BRAZEN_GLAPI*)(GLsizei, const GLuint*);
using PFN_glGenBuffers = void(BRAZEN_GLAPI*)(GLsizei, GLuint*);
using PFN_glBindBuffer = void(BRAZEN_GLAPI*)(GLenum, GLuint);
using PFN_glBufferData = void(BRAZEN_GLAPI*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glDeleteBuffers = void(BRAZEN_GLAPI*)(GLsizei, const GLuint*);
using PFN_glVertexAttribPointer = void(BRAZEN_GLAPI*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glEnableVertexAttribArray = void(BRAZEN_GLAPI*)(GLuint);

using PFN_glCreateShader = GLuint(BRAZEN_GLAPI*)(GLenum);
using PFN_glShaderSource = void(BRAZEN_GLAPI*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void(BRAZEN_GLAPI*)(GLuint);
using PFN_glGetShaderiv = void(BRAZEN_GLAPI*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog = void(BRAZEN_GLAPI*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glDeleteShader = void(BRAZEN_GLAPI*)(GLuint);

using PFN_glCreateProgram = GLuint(BRAZEN_GLAPI*)();
using PFN_glAttachShader = void(BRAZEN_GLAPI*)(GLuint, GLuint);
using PFN_glLinkProgram = void(BRAZEN_GLAPI*)(GLuint);
using PFN_glGetProgramiv = void(BRAZEN_GLAPI*)(GLuint, GLenum, GLint*);
using PFN_glGetProgramInfoLog = void(BRAZEN_GLAPI*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glDeleteProgram = void(BRAZEN_GLAPI*)(GLuint);
using PFN_glUseProgram = void(BRAZEN_GLAPI*)(GLuint);

using PFN_glGetUniformLocation = GLint(BRAZEN_GLAPI*)(GLuint, const GLchar*);
using PFN_glUniform1i = void(BRAZEN_GLAPI*)(GLint, GLint);
using PFN_glUniform1f = void(BRAZEN_GLAPI*)(GLint, GLfloat);

using PFN_glGenFramebuffers = void(BRAZEN_GLAPI*)(GLsizei, GLuint*);
using PFN_glBindFramebuffer = void(BRAZEN_GLAPI*)(GLenum, GLuint);
using PFN_glFramebufferTexture2D = void(BRAZEN_GLAPI*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glCheckFramebufferStatus = GLenum(BRAZEN_GLAPI*)(GLenum);
using PFN_glDeleteFramebuffers = void(BRAZEN_GLAPI*)(GLsizei, const GLuint*);

using PFN_glGenTextures = void(BRAZEN_GLAPI*)(GLsizei, GLuint*);
using PFN_glBindTexture = void(BRAZEN_GLAPI*)(GLenum, GLuint);
using PFN_glTexImage2D = void(BRAZEN_GLAPI*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using PFN_glTexParameteri = void(BRAZEN_GLAPI*)(GLenum, GLenum, GLint);
using PFN_glDeleteTextures = void(BRAZEN_GLAPI*)(GLsizei, const GLuint*);

using PFN_glViewport = void(BRAZEN_GLAPI*)(GLint, GLint, GLsizei, GLsizei);
using PFN_glClearColor = void(BRAZEN_GLAPI*)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glClear = void(BRAZEN_GLAPI*)(GLbitfield);
using PFN_glDrawArrays = void(BRAZEN_GLAPI*)(GLenum, GLint, GLsizei);
using PFN_glFinish = void(BRAZEN_GLAPI*)();
using PFN_glGetError = GLenum(BRAZEN_GLAPI*)();
using PFN_glDisable = void(BRAZEN_GLAPI*)(GLenum);
using PFN_glGetString = const unsigned char*(BRAZEN_GLAPI*)(GLenum);

// Global function pointers, populated by LoadGLFunctions(). Left null
// until then; callers must check GLLoaderReady() before use.
struct GLFunctions {
    PFN_glGenVertexArrays GenVertexArrays = nullptr;
    PFN_glBindVertexArray BindVertexArray = nullptr;
    PFN_glDeleteVertexArrays DeleteVertexArrays = nullptr;
    PFN_glGenBuffers GenBuffers = nullptr;
    PFN_glBindBuffer BindBuffer = nullptr;
    PFN_glBufferData BufferData = nullptr;
    PFN_glDeleteBuffers DeleteBuffers = nullptr;
    PFN_glVertexAttribPointer VertexAttribPointer = nullptr;
    PFN_glEnableVertexAttribArray EnableVertexAttribArray = nullptr;

    PFN_glCreateShader CreateShader = nullptr;
    PFN_glShaderSource ShaderSource = nullptr;
    PFN_glCompileShader CompileShader = nullptr;
    PFN_glGetShaderiv GetShaderiv = nullptr;
    PFN_glGetShaderInfoLog GetShaderInfoLog = nullptr;
    PFN_glDeleteShader DeleteShader = nullptr;

    PFN_glCreateProgram CreateProgram = nullptr;
    PFN_glAttachShader AttachShader = nullptr;
    PFN_glLinkProgram LinkProgram = nullptr;
    PFN_glGetProgramiv GetProgramiv = nullptr;
    PFN_glGetProgramInfoLog GetProgramInfoLog = nullptr;
    PFN_glDeleteProgram DeleteProgram = nullptr;
    PFN_glUseProgram UseProgram = nullptr;

    PFN_glGetUniformLocation GetUniformLocation = nullptr;
    PFN_glUniform1i Uniform1i = nullptr;
    PFN_glUniform1f Uniform1f = nullptr;

    PFN_glGenFramebuffers GenFramebuffers = nullptr;
    PFN_glBindFramebuffer BindFramebuffer = nullptr;
    PFN_glFramebufferTexture2D FramebufferTexture2D = nullptr;
    PFN_glCheckFramebufferStatus CheckFramebufferStatus = nullptr;
    PFN_glDeleteFramebuffers DeleteFramebuffers = nullptr;

    PFN_glGenTextures GenTextures = nullptr;
    PFN_glBindTexture BindTexture = nullptr;
    PFN_glTexImage2D TexImage2D = nullptr;
    PFN_glTexParameteri TexParameteri = nullptr;
    PFN_glDeleteTextures DeleteTextures = nullptr;

    PFN_glViewport Viewport = nullptr;
    PFN_glClearColor ClearColor = nullptr;
    PFN_glClear Clear = nullptr;
    PFN_glDrawArrays DrawArrays = nullptr;
    PFN_glFinish Finish = nullptr;
    PFN_glGetError GetError = nullptr;
    PFN_glDisable Disable = nullptr;
    PFN_glGetString GetString = nullptr;
};

// The single global set of loaded function pointers. Defined in
// GLLoader.cpp; populated by LoadGLFunctions().
extern GLFunctions gl;

// getProcAddress should be glfwGetProcAddress (passed in rather than
// linking GLFW into this header, keeping the loader itself GLFW-agnostic).
// Returns true if every required function resolved successfully.
using GLGetProcAddressFn = void* (*)(const char* name);
bool LoadGLFunctions(GLGetProcAddressFn getProcAddress);

} // namespace brazen
