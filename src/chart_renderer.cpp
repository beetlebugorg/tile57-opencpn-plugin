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
void ChartRenderer::add_fill(const tile57_rings* p, tile57_rgba c, std::vector<Vtx>& out) {
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
            out.push_back({ flat[idx[i + k]][0], flat[idx[i + k]][1], c.r, c.g, c.b, c.a });
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
            auto v = [&](float x, float y) { base_tris.push_back({x, y, c.r, c.g, c.b, c.a}); };
            v(ax + nx, ay + ny); v(bx + nx, by + ny); v(bx - nx, by - ny);
            v(ax + nx, ay + ny); v(bx - nx, by - ny); v(ax - nx, ay - ny);
        }
    }
}

// ---- C trampolines (ctx == ChartRenderer*) -----------------------------
static void tr_fill(void* c, const tile57_rings* r, tile57_rgba col, int) {
    auto* self = static_cast<ChartRenderer*>(c);
    self->add_fill(r, col, self->base_tris);
}
static void tr_stroke(void* c, const tile57_rings* r, float w, float, float, tile57_rgba col) {
    static_cast<ChartRenderer*>(c)->add_stroke(r, w, col);
}
static void tr_pattern(void* c, const tile57_rings* r, uint32_t, uint32_t, const uint8_t*) {
    // Pattern fills are approximated with a flat tint for now.
    auto* self = static_cast<ChartRenderer*>(c);
    self->add_fill(r, tile57_rgba{160, 160, 170, 140}, self->base_tris);
}
static void tr_glyphs(void* c, const tile57_rings* r, tile57_rgba col, tile57_rgba, float) {
    // Glyph outlines are filled as-is; letter counters need an even-odd fill.
    auto* self = static_cast<ChartRenderer*>(c);
    self->add_fill(r, col, self->text_tris);
}

// ---- chart / GL lifecycle --------------------------------------------------
bool ChartRenderer::open_chart(const std::string& path) {
    if (chart_) return true;
    chart_ = tile57_chart_open_pmtiles(path.c_str());
    return chart_ != nullptr;
}

bool ChartRenderer::get_info(tile57_chart_info& out) const {
    if (!chart_) return false;
    tile57_chart_get_info(chart_, &out);
    return true;
}

static const char* VS = R"(
attribute vec2 aPx;
attribute vec4 aCol;
uniform vec2 uVp;
uniform vec2 uCenter;   // overzoom pivot (viewport centre, px)
uniform float uScale;   // overzoom factor (1.0 = none)
varying vec4 vCol;
void main(){
  vec2 px = uCenter + (aPx - uCenter) * uScale;
  vec2 ndc = vec2(px.x/uVp.x*2.0-1.0, 1.0 - px.y/uVp.y*2.0);
  gl_Position = vec4(ndc, 0.0, 1.0); vCol = aCol;
}
)";
static const char* FS = R"(
varying vec4 vCol;
void main(){ gl_FragColor = vec4(vCol.rgb*vCol.a, vCol.a); }
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
    // GLSL 1.20 has no layout(location=…); bind generic attribute slots before link.
    glBindAttribLocation(prog_, 0, "aPx");
    glBindAttribLocation(prog_, 1, "aCol");
    glLinkProgram(prog_);
    u_vp_ = glGetUniformLocation(prog_, "uVp");
    u_scale_ = glGetUniformLocation(prog_, "uScale");
    u_center_ = glGetUniformLocation(prog_, "uCenter");
    glGenBuffers(1, &vbo_base_);
    glGenBuffers(1, &vbo_text_);
    gl_ready_ = true;
    return true;
}

void ChartRenderer::rebuild(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                            const tile57_mariner& m) {
    base_tris.clear(); text_tris.clear();
    tile57_canvas_cb cb{};
    cb.ctx = this;
    cb.fill_path = tr_fill; cb.stroke_path = tr_stroke;
    cb.fill_pattern = tr_pattern; cb.draw_glyphs = tr_glyphs;
    int rc = tile57_chart_render_view_cb(chart_, lon, lat, zoom, w, h, &m, &cb);
    if (rc != 0) { std::fprintf(stderr, "t57 render_view_cb rc=%d\n", rc); return; }

    // No VAO in GL 2.1 — just upload; attribute pointers are set at draw time.
    glBindBuffer(GL_ARRAY_BUFFER, vbo_base_);
    glBufferData(GL_ARRAY_BUFFER, base_tris.size() * sizeof(Vtx), base_tris.data(), GL_DYNAMIC_DRAW);
    base_count_ = static_cast<uint32_t>(base_tris.size());
    glBindBuffer(GL_ARRAY_BUFFER, vbo_text_);
    glBufferData(GL_ARRAY_BUFFER, text_tris.size() * sizeof(Vtx), text_tris.data(), GL_DYNAMIC_DRAW);
    text_count_ = static_cast<uint32_t>(text_tris.size());
}

void ChartRenderer::draw_buffer(uint32_t vbo, uint32_t count) {
    if (!count) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)offsetof(Vtx, r));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

void ChartRenderer::render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                           const tile57_mariner& m, Pass pass, bool stencil_clip) {
    if (!chart_ || !ensure_gl()) return;

    // tile57 only portrays within the chart's baked [min_zoom, max_zoom]; beyond
    // it, it paints the flat S-52 NODTA fill. So render at the range-clamped zoom
    // and, when the view is outside it, scale the (vector) geometry to the real
    // view about its centre — crisp overzoom instead of a grey no-data plate.
    if (!have_range_) {
        tile57_chart_info info{};
        tile57_chart_get_info(chart_, &info);
        min_zoom_ = info.min_zoom; max_zoom_ = info.max_zoom; have_range_ = true;
    }
    double zr = zoom;
    if (zr < min_zoom_) zr = min_zoom_;
    if (zr > max_zoom_) zr = max_zoom_;
    double oscale = std::pow(2.0, zoom - zr);   // 1.0 when in range

    // Rebuild geometry only when the view or (clamped) render zoom changed.
    // Both passes of one frame share a single rebuild.
    uint64_t mh = mariner_hash(m);
    bool changed = !have_last_ || lon != last_lon_ || lat != last_lat_ || zr != last_zoom_
                || w != last_w_ || h != last_h_ || mh != last_mhash_;
    if (changed) {
        rebuild(lon, lat, zr, w, h, m);
        have_last_ = true; last_lon_ = lon; last_lat_ = lat; last_zoom_ = zr;
        last_w_ = w; last_h_ = h; last_mhash_ = mh;
    }

    glUseProgram(prog_);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_DEPTH_TEST);
    // Clip to the stencil mask OpenCPN pre-wrote for this chart's quilt patch
    // (glChartCanvas::SetClipRegion tags the patch region with stencil 1).
    if (stencil_clip) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    }
    float vp[2] = { float(w), float(h) };
    glUniform2fv(u_vp_, 1, vp);
    float center[2] = { w * 0.5f, h * 0.5f };
    glUniform2fv(u_center_, 1, center);
    glUniform1f(u_scale_, float(oscale));

    if (pass != Pass::kText) draw_buffer(vbo_base_, base_count_);
    if (pass != Pass::kBase) draw_buffer(vbo_text_, text_count_);

    // Restore state so OpenCPN's fixed-function (legacy) drawing isn't disturbed.
    if (stencil_clip) glDisable(GL_STENCIL_TEST);
    glUseProgram(0);
}

void ChartRenderer::shutdown() {
    // OpenCPN calls DeInit() without a current GL context, so deleting GL objects
    // here crashes. Drop the chart and force a fresh GL setup on re-enable; the
    // tiny leaked program/VBO are reclaimed when the GL context is destroyed.
    gl_ready_ = false; prog_ = 0; vbo_base_ = 0; vbo_text_ = 0;
    base_count_ = 0; text_count_ = 0; have_last_ = false;
    have_range_ = false;
    if (chart_) { tile57_chart_close(chart_); chart_ = nullptr; }
}

} // namespace t57
