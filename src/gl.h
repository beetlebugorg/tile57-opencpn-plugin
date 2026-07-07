// gl.h — desktop OpenGL headers (via GLEW) and the GLSL version prologue.
//
// Shader bodies in the .cpp files carry no #version line; compile() prepends
// T57_GLSL_VERSION. OpenCPN provides the GL context; we only load entry points.
#pragma once
#include <GL/glew.h>

#define T57_GLSL_VERSION "#version 330 core\n"

static inline bool t57_gl_loader_init() {
    glewExperimental = GL_TRUE;
    GLenum e = glewInit();
    // GLEW can report GLEW_ERROR_NO_GLX_DISPLAY under some setups yet still load
    // every entry point; accept that as long as the functions we use are present.
    if (e == GLEW_OK) return true;
    glGetError();
    return glGenVertexArrays != nullptr && glCreateProgram != nullptr;
}
