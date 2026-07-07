// chart_renderer.cpp — see chart_renderer.h.
#include "chart_renderer.h"
#include "gl.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "earcut.hpp"

namespace t57 {

// FNV-1a over the mariner struct; any change triggers a geometry rebuild.
static uint64_t mariner_hash(const tile57_mariner& m) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&m);
    for (size_t i = 0; i < sizeof(m); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// ---- tessellation (pixel space) --------------------------------------------
void ChartRenderer::add_fill(const tile57_rings* p, tile57_rgba c) {
    if (!p || p->n < 3) return;
    using Pt = std::array<float, 2>;
    std::vector<std::vector<Pt>> poly;
    std::vector<Pt> flat;
    for (uint32_t r = 0; r < p->ring_count; ++r) {
        uint32_t s = p->ring_starts[r];
        uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
        if (e <= s + 2) continue;
        std::vector<Pt> ring;
        for (uint32_t i = s; i < e; ++i) { ring.push_back({p->pts[i].x, p->pts[i].y}); flat.push_back({p->pts[i].x, p->pts[i].y}); }
        poly.push_back(std::move(ring));
    }
    if (poly.empty()) return;
    std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        for (int k = 0; k < 3; ++k)
            tris.push_back({ flat[idx[i + k]][0], flat[idx[i + k]][1], c.r, c.g, c.b, c.a });
}

void ChartRenderer::add_stroke(const tile57_rings* p, float w, tile57_rgba c) {
    if (!p || p->n < 2) return;
    float hw = std::max(0.5f, w * 0.5f);
    for (uint32_t r = 0; r < p->ring_count; ++r) {
        uint32_t s = p->ring_starts[r];
        uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
        for (uint32_t i = s; i + 1 < e; ++i) {
            float ax = p->pts[i].x, ay = p->pts[i].y, bx = p->pts[i + 1].x, by = p->pts[i + 1].y;
            float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f) continue;
            float nx = -dy / len * hw, ny = dx / len * hw;
            auto v = [&](float x, float y) { tris.push_back({x, y, c.r, c.g, c.b, c.a}); };
            v(ax + nx, ay + ny); v(bx + nx, by + ny); v(bx - nx, by - ny);
            v(ax + nx, ay + ny); v(bx - nx, by - ny); v(ax - nx, ay - ny);
        }
    }
}

// ---- C trampolines (ctx == ChartRenderer*) -----------------------------
static void tr_fill(void* c, const tile57_rings* r, tile57_rgba col, int) {
    static_cast<ChartRenderer*>(c)->add_fill(r, col);
}
static void tr_stroke(void* c, const tile57_rings* r, float w, float, float, tile57_rgba col) {
    static_cast<ChartRenderer*>(c)->add_stroke(r, w, col);
}
static void tr_pattern(void* c, const tile57_rings* r, uint32_t, uint32_t, const uint8_t*) {
    // Pattern fills are approximated with a flat tint for now.
    static_cast<ChartRenderer*>(c)->add_fill(r, tile57_rgba{160, 160, 170, 140});
}
static void tr_glyphs(void* c, const tile57_rings* r, tile57_rgba col, tile57_rgba, float) {
    // Glyph outlines are filled as-is; letter counters need an even-odd fill.
    auto* self = static_cast<ChartRenderer*>(c);
    if (!self->skip_text) self->add_fill(r, col);
}

// ---- chart / GL lifecycle --------------------------------------------------
bool ChartRenderer::open_chart(const std::string& path) {
    if (chart_) return true;
    chart_ = tile57_chart_open_pmtiles(path.c_str());
    return chart_ != nullptr;
}

static const char* VS = R"(
layout(location=0) in vec2 aPx;
layout(location=1) in vec4 aCol;
uniform vec2 uVp;
out vec4 vCol;
void main(){ vec2 ndc = vec2(aPx.x/uVp.x*2.0-1.0, 1.0 - aPx.y/uVp.y*2.0); gl_Position=vec4(ndc,0.0,1.0); vCol=aCol; }
)";
static const char* FS = R"(
in vec4 vCol; out vec4 o;
void main(){ o = vec4(vCol.rgb*vCol.a, vCol.a); }
)";
static uint32_t compile(GLenum t, const char* body) {
    std::string src = std::string(T57_GLSL_VERSION) + body;
    const char* s = src.c_str();
    uint32_t sh = glCreateShader(t); glShaderSource(sh, 1, &s, nullptr); glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[512]; glGetShaderInfoLog(sh, 512, nullptr, l); std::fprintf(stderr, "tile57 shader: %s\n", l); }
    return sh;
}

bool ChartRenderer::ensure_gl() {
    if (gl_ready_) return true;
    if (!t57_gl_loader_init()) return false;
    prog_ = glCreateProgram();
    glAttachShader(prog_, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(prog_, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(prog_);
    u_vp_ = glGetUniformLocation(prog_, "uVp");
    glGenVertexArrays(1, &vao_); glGenBuffers(1, &vbo_);
    gl_ready_ = true;
    return true;
}

void ChartRenderer::rebuild(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                            const tile57_mariner& m) {
    tris.clear();
    tile57_canvas_cb cb{};
    cb.ctx = this;
    cb.fill_path = tr_fill; cb.stroke_path = tr_stroke;
    cb.fill_pattern = tr_pattern; cb.draw_glyphs = tr_glyphs;
    int rc = tile57_chart_render_view_cb(chart_, lon, lat, zoom, w, h, &m, &cb);
    if (rc != 0) { std::fprintf(stderr, "t57 render_view_cb rc=%d\n", rc); return; }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, tris.size() * sizeof(Vtx), tris.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)offsetof(Vtx, r));
    vbo_count_ = static_cast<uint32_t>(tris.size());
    glBindVertexArray(0);
}

void ChartRenderer::render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                               const tile57_mariner& m, bool draw_text) {
    if (!chart_ || !ensure_gl()) return;
    skip_text = !draw_text;

    // Rebuild geometry only when the view or mariner changed; otherwise replay.
    uint64_t mh = mariner_hash(m);
    bool changed = !have_last_ || lon != last_lon_ || lat != last_lat_ || zoom != last_zoom_
                || w != last_w_ || h != last_h_ || mh != last_mhash_ || draw_text != last_text_;
    if (changed) {
        rebuild(lon, lat, zoom, w, h, m);
        have_last_ = true; last_lon_ = lon; last_lat_ = lat; last_zoom_ = zoom;
        last_w_ = w; last_h_ = h; last_mhash_ = mh; last_text_ = draw_text;
    }
    if (!vbo_count_) return;

    glUseProgram(prog_);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_DEPTH_TEST);
    float vp[2] = { float(w), float(h) };
    glUniform2fv(u_vp_, 1, vp);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vbo_count_);
    glBindVertexArray(0);
}

void ChartRenderer::shutdown() {
    if (gl_ready_) { glDeleteProgram(prog_); glDeleteBuffers(1, &vbo_); glDeleteVertexArrays(1, &vao_); gl_ready_ = false; }
    if (chart_) { tile57_chart_close(chart_); chart_ = nullptr; }
}

} // namespace t57
