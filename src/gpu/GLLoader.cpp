#include "GLLoader.h"
#include <cstring>

namespace brazen {

GLFunctions gl;

namespace {

// Casts + null-checks a single resolved proc address into `outSlot`,
// returning false (and leaving outSlot untouched) if it wasn't found.
template <typename FnPtr>
bool LoadOne(GLGetProcAddressFn getProcAddress, const char* name, FnPtr& outSlot) {
    void* p = getProcAddress(name);
    if (!p) return false;
    outSlot = reinterpret_cast<FnPtr>(p);
    return true;
}

} // namespace

bool LoadGLFunctions(GLGetProcAddressFn getProcAddress) {
    if (!getProcAddress) return false;
    bool ok = true;

#define BRAZEN_LOAD(field, glName) ok = LoadOne(getProcAddress, glName, gl.field) && ok

    BRAZEN_LOAD(GenVertexArrays, "glGenVertexArrays");
    BRAZEN_LOAD(BindVertexArray, "glBindVertexArray");
    BRAZEN_LOAD(DeleteVertexArrays, "glDeleteVertexArrays");
    BRAZEN_LOAD(GenBuffers, "glGenBuffers");
    BRAZEN_LOAD(BindBuffer, "glBindBuffer");
    BRAZEN_LOAD(BufferData, "glBufferData");
    BRAZEN_LOAD(DeleteBuffers, "glDeleteBuffers");
    BRAZEN_LOAD(VertexAttribPointer, "glVertexAttribPointer");
    BRAZEN_LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");

    BRAZEN_LOAD(CreateShader, "glCreateShader");
    BRAZEN_LOAD(ShaderSource, "glShaderSource");
    BRAZEN_LOAD(CompileShader, "glCompileShader");
    BRAZEN_LOAD(GetShaderiv, "glGetShaderiv");
    BRAZEN_LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    BRAZEN_LOAD(DeleteShader, "glDeleteShader");

    BRAZEN_LOAD(CreateProgram, "glCreateProgram");
    BRAZEN_LOAD(AttachShader, "glAttachShader");
    BRAZEN_LOAD(LinkProgram, "glLinkProgram");
    BRAZEN_LOAD(GetProgramiv, "glGetProgramiv");
    BRAZEN_LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
    BRAZEN_LOAD(DeleteProgram, "glDeleteProgram");
    BRAZEN_LOAD(UseProgram, "glUseProgram");

    BRAZEN_LOAD(GetUniformLocation, "glGetUniformLocation");
    BRAZEN_LOAD(Uniform1i, "glUniform1i");
    BRAZEN_LOAD(Uniform1f, "glUniform1f");

    BRAZEN_LOAD(GenFramebuffers, "glGenFramebuffers");
    BRAZEN_LOAD(BindFramebuffer, "glBindFramebuffer");
    BRAZEN_LOAD(FramebufferTexture2D, "glFramebufferTexture2D");
    BRAZEN_LOAD(CheckFramebufferStatus, "glCheckFramebufferStatus");
    BRAZEN_LOAD(DeleteFramebuffers, "glDeleteFramebuffers");

    BRAZEN_LOAD(GenTextures, "glGenTextures");
    BRAZEN_LOAD(BindTexture, "glBindTexture");
    BRAZEN_LOAD(TexImage2D, "glTexImage2D");
    BRAZEN_LOAD(TexParameteri, "glTexParameteri");
    BRAZEN_LOAD(DeleteTextures, "glDeleteTextures");

    BRAZEN_LOAD(Viewport, "glViewport");
    BRAZEN_LOAD(ClearColor, "glClearColor");
    BRAZEN_LOAD(Clear, "glClear");
    BRAZEN_LOAD(DrawArrays, "glDrawArrays");
    BRAZEN_LOAD(Finish, "glFinish");
    // Timer queries are optional on an OpenGL 3.3 context. The benchmark
    // falls back to synchronized CPU timing when the extension is absent.
    LoadOne(getProcAddress, "glGenQueries", gl.GenQueries);
    LoadOne(getProcAddress, "glDeleteQueries", gl.DeleteQueries);
    LoadOne(getProcAddress, "glBeginQuery", gl.BeginQuery);
    LoadOne(getProcAddress, "glEndQuery", gl.EndQuery);
    bool timer64 = LoadOne(getProcAddress, "glGetQueryObjectui64v", gl.GetQueryObjectui64v);
    if (!timer64) LoadOne(getProcAddress, "glGetQueryObjectui64vEXT", gl.GetQueryObjectui64v);
    LoadOne(getProcAddress, "glActiveTexture", gl.ActiveTexture);
    BRAZEN_LOAD(GetError, "glGetError");
    BRAZEN_LOAD(Disable, "glDisable");
    BRAZEN_LOAD(GetString, "glGetString");
    BRAZEN_LOAD(GetStringi, "glGetStringi");
    BRAZEN_LOAD(GetIntegerv, "glGetIntegerv");

#undef BRAZEN_LOAD

    return ok;
}

} // namespace brazen
