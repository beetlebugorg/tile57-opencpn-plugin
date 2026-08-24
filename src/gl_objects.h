// gl_objects.h - move-only owners for GL buffers, textures, renderbuffers and
// framebuffers. The object is deleted when its owner is destroyed, which must
// happen on the GL thread.
#pragma once
#include "gl.h"
#include <cstdint>
#include <utility>

namespace t57 {

// CRTP base: `Traits` names the gen/delete pair. `id()` is 0 until `gen()` runs.
template <class Traits> class GlObject {
  public:
    GlObject() = default;
    GlObject(const GlObject&) = delete;
    GlObject& operator=(const GlObject&) = delete;
    GlObject(GlObject&& o) noexcept : id_(std::exchange(o.id_, 0)) {}
    GlObject& operator=(GlObject&& o) noexcept {
        if (this != &o) {
            reset();
            id_ = std::exchange(o.id_, 0);
        }
        return *this;
    }
    ~GlObject() { reset(); }

    uint32_t id() const { return id_; }
    explicit operator bool() const { return id_ != 0; }
    uint32_t gen() {
        reset();
        Traits::gen(1, &id_);
        return id_;
    }
    void reset() {
        if (id_) {
            Traits::del(1, &id_);
            id_ = 0;
        }
    }
    // Hand the id back without deleting it (a context that already died owns nothing).
    uint32_t release() { return std::exchange(id_, 0); }

  private:
    uint32_t id_ = 0;
};

struct BufferTraits {
    static void gen(GLsizei n, GLuint* ids) { glGenBuffers(n, ids); }
    static void del(GLsizei n, const GLuint* ids) { glDeleteBuffers(n, ids); }
};
struct TextureTraits {
    static void gen(GLsizei n, GLuint* ids) { glGenTextures(n, ids); }
    static void del(GLsizei n, const GLuint* ids) { glDeleteTextures(n, ids); }
};
struct RenderbufferTraits {
    static void gen(GLsizei n, GLuint* ids) { glGenRenderbuffers(n, ids); }
    static void del(GLsizei n, const GLuint* ids) { glDeleteRenderbuffers(n, ids); }
};
struct FramebufferTraits {
    static void gen(GLsizei n, GLuint* ids) { glGenFramebuffers(n, ids); }
    static void del(GLsizei n, const GLuint* ids) { glDeleteFramebuffers(n, ids); }
};

using GlBuffer = GlObject<BufferTraits>;
using GlTexture = GlObject<TextureTraits>;
using GlRenderbuffer = GlObject<RenderbufferTraits>;
using GlFramebuffer = GlObject<FramebufferTraits>;

// Upload an RGBA8 image as a 2D texture with linear filtering. `wrap` is GL_REPEAT
// for a pattern cell that tiles, GL_CLAMP_TO_EDGE for an atlas.
inline GlTexture make_rgba_texture(const uint8_t* rgba, int w, int h, GLint wrap) {
    GlTexture tex;
    tex.gen();
    glBindTexture(GL_TEXTURE_2D, tex.id());
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

} // namespace t57
