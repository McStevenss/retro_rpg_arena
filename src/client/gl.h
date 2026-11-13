// Minimal OpenGL 3.3 core loader. GL 1.1 functions come from libGL directly;
// everything newer is fetched at runtime via SDL_GL_GetProcAddress into the
// glfn namespace (so our pointers never clash with libGL's exported symbols).
#pragma once
#include <SDL3/SDL_opengl.h>

#define BOYZ_GL_FUNCS(X)                                              \
    X(PFNGLCREATESHADERPROC, glCreateShader)                          \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                          \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                        \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                            \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                  \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                          \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                        \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                          \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                            \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                          \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                        \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                              \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)              \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                \
    X(PFNGLUNIFORM3FPROC, glUniform3f)                                \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                  \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                    \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)              \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                    \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                              \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                        \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                              \
    X(PFNGLBUFFERDATAPROC, glBufferData)                              \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                        \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)            \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)    \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                    \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)              \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                    \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)          \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)      \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                  \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)            \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                  \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)            \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)    \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)

namespace glfn {
#define BOYZ_GL_DECL(type, name) extern type name;
BOYZ_GL_FUNCS(BOYZ_GL_DECL)
#undef BOYZ_GL_DECL

// Call after creating the GL context. Returns false if anything is missing.
bool load();
}  // namespace glfn
