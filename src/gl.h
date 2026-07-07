// gl.h — desktop OpenGL headers (via GLEW) and the GLSL version prologue.
//
// Shader bodies in the .cpp files carry no #version line; compile() prepends
// T57_GLSL_VERSION. OpenCPN provides the GL context; we only load entry points.
#pragma once
#include <GL/glew.h>

// GLSL 1.20 / GL 2.1: OpenCPN provides a legacy compatibility context (macOS
// caps legacy GL at 2.1 / GLSL 1.20; a core profile would be needed for 3.30).
// So the shaders use attribute/varying/gl_FragColor and we bind attribute
// locations explicitly rather than with layout(location=…), and use no VAOs.
#define T57_GLSL_VERSION "#version 120\n"

static inline bool t57_gl_loader_init() {
    glewExperimental = GL_TRUE;
    GLenum e = glewInit();
    // GLEW can report GLEW_ERROR_NO_GLX_DISPLAY under some setups yet still load
    // every entry point; accept that as long as the functions we use are present.
    if (e == GLEW_OK) return true;
    glGetError();
    return glCreateProgram != nullptr && glGenBuffers != nullptr;
}
