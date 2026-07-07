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

// Glyph text arrives as ONE tile57_rings holding EVERY letter's contours
// (outlines and counters) as flat sibling rings. Earcutting that as a single
// polygon-with-holes bridges triangles between letters — labels rendered as
// solid bars. Group rings by containment parity instead: even nesting depth is
// a letter outline, odd depth a counter of its immediate parent; tessellate
// each letter as its own polygon.
static bool point_in_ring(float x, float y, const std::vector<std::array<float, 2>>& ring) {
    bool in = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        if (((ring[i][1] > y) != (ring[j][1] > y)) &&
            (x < (ring[j][0] - ring[i][0]) * (y - ring[i][1]) / (ring[j][1] - ring[i][1]) + ring[i][0]))
            in = !in;
    }
    return in;
}

void ChartRenderer::add_glyph(const tile57_rings* p, tile57_rgba c) {
    if (!p || p->n < 3) return;
    using Pt = std::array<float, 2>;
    std::vector<std::vector<Pt>> rings;
    for (uint32_t r = 0; r < p->ring_count; ++r) {
        uint32_t s = p->ring_starts[r];
        uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
        if (e <= s + 2) continue;
        std::vector<Pt> ring;
        for (uint32_t i = s; i < e; ++i) ring.push_back({p->pts[i].x, p->pts[i].y});
        rings.push_back(std::move(ring));
    }
    if (rings.empty()) return;

    const size_t n = rings.size();
    std::vector<double> area(n, 0.0);           // |area|, for immediate-parent choice
    for (size_t i = 0; i < n; ++i) {
        double a = 0;
        for (size_t k = 0, j = rings[i].size() - 1; k < rings[i].size(); j = k++)
            a += (double)rings[i][j][0] * rings[i][k][1] - (double)rings[i][k][0] * rings[i][j][1];
        area[i] = std::fabs(a) * 0.5;
    }
    // Nesting depth of each ring = how many other rings contain it; the
    // immediate parent is the smallest containing ring. (Glyph contours nest
    // but never intersect, so a single sample vertex decides containment.)
    std::vector<int> depth(n, 0), parent(n, -1);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            if (point_in_ring(rings[i][0][0], rings[i][0][1], rings[j])) {
                ++depth[i];
                if (parent[i] < 0 || area[j] < area[parent[i]]) parent[i] = (int)j;
            }
        }

    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2) continue;             // counters are handled with their letter
        std::vector<std::vector<Pt>> poly;
        std::vector<Pt> flat;
        poly.push_back(rings[i]);
        flat.insert(flat.end(), rings[i].begin(), rings[i].end());
        for (size_t j = 0; j < n; ++j) {
            if (parent[j] != (int)i || depth[j] % 2 == 0) continue;
            poly.push_back(rings[j]);
            flat.insert(flat.end(), rings[j].begin(), rings[j].end());
        }
        std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
            for (int k = 0; k < 3; ++k)
                text_tris.push_back({ flat[idx[t + k]][0], flat[idx[t + k]][1], c.r, c.g, c.b, c.a });
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
    static_cast<ChartRenderer*>(c)->add_glyph(r, col);
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
attribute vec2 aPx;      // camera pixel space
attribute vec4 aCol;
uniform vec2 uVp;        // view size (px)
uniform vec2 uOffset;    // camera -> view affine: view_px = uOffset + uScale*aPx
uniform float uScale;
varying vec4 vCol;
void main(){
  vec2 px = uOffset + aPx * uScale;
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
    u_offset_ = glGetUniformLocation(prog_, "uOffset");
    glGenBuffers(1, &vbo_base_);
    glGenBuffers(1, &vbo_text_);
    gl_ready_ = true;
    return true;
}

void ChartRenderer::rebuild(const tile57_mariner& m) {
    base_tris.clear(); text_tris.clear();
    tile57_canvas_cb cb{};
    cb.ctx = this;
    cb.fill_path = tr_fill; cb.stroke_path = tr_stroke;
    cb.fill_pattern = tr_pattern; cb.draw_glyphs = tr_glyphs;
    int rc = tile57_chart_render_view_cb(chart_, cam_lon_, cam_lat_, cam_zoom_,
                                         cam_w_, cam_h_, &m, &cb);
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

// Global web-mercator pixel coordinates of a lon/lat at a zoom.
static void world_px(double lon, double lat, double zoom, double& X, double& Y) {
    const double kPI = 3.14159265358979323846;
    double n = 256.0 * std::pow(2.0, zoom);
    X = (lon + 180.0) / 360.0 * n;
    double s = std::sin(lat * kPI / 180.0);
    Y = (0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * kPI)) * n;
}

void ChartRenderer::render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                           const tile57_mariner& m, Pass pass, bool stencil_clip) {
    if (!chart_ || !ensure_gl()) return;
    if (!have_range_) {
        tile57_chart_info info{};
        tile57_chart_get_info(chart_, &info);
        min_zoom_ = info.min_zoom; max_zoom_ = info.max_zoom; have_range_ = true;
    }

    // Portrayal camera zoom: the view zoom snapped to half levels — so panning
    // and fractional zoom replay cached geometry — and clamped to the baked
    // band, which also implements overzoom (past max_zoom the affine below
    // magnifies real vectors instead of tile57 painting its flat NODTA fill).
    double cz = 0.5 * std::round(zoom * 2.0);
    if (cz < min_zoom_) cz = min_zoom_;
    if (cz > max_zoom_) cz = max_zoom_;
    double s = std::pow(2.0, zoom - cz);        // view px per camera px

    // Camera extent: the view plus a pan margin, in camera pixels (capped: far
    // below min_zoom the view outgrows any camera; the cell is tiny there).
    auto cam_dim = [&](uint32_t view_px) {
        double d = std::ceil(view_px / s * 1.5);
        return (uint32_t)std::min(d, 4096.0);
    };

    uint64_t mh = mariner_hash(m);
    bool rebuild_needed = !have_cam_ || cz != cam_zoom_ || mh != cam_mhash_
                       || cam_w_ < cam_dim(w) / 2 || cam_h_ < cam_dim(h) / 2;

    // Does the view still fit inside the cached camera's margin?
    if (!rebuild_needed) {
        double ccx, ccy, vcx, vcy;
        world_px(cam_lon_, cam_lat_, cam_zoom_, ccx, ccy);
        world_px(lon, lat, cam_zoom_, vcx, vcy);
        double cx = cam_w_ / 2.0 + (vcx - ccx);   // view centre in camera px
        double cy = cam_h_ / 2.0 + (vcy - ccy);
        double hw = w / (2.0 * s), hh = h / (2.0 * s);
        if (cx - hw < 0 || cy - hh < 0 || cx + hw > cam_w_ || cy + hh > cam_h_)
            rebuild_needed = true;
    }

    if (rebuild_needed) {
        cam_lon_ = lon; cam_lat_ = lat; cam_zoom_ = cz;
        cam_w_ = cam_dim(w); cam_h_ = cam_dim(h);
        cam_mhash_ = mh;
        rebuild(m);
        have_cam_ = true;
    }

    // Camera -> view affine: view_px = uOffset + s * cam_px.
    double ccx, ccy, vcx, vcy;
    world_px(cam_lon_, cam_lat_, cam_zoom_, ccx, ccy);
    world_px(lon, lat, cam_zoom_, vcx, vcy);
    double cx = cam_w_ / 2.0 + (vcx - ccx);
    double cy = cam_h_ / 2.0 + (vcy - ccy);
    float off[2] = { float(w / 2.0 - cx * s), float(h / 2.0 - cy * s) };

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
    glUniform2fv(u_offset_, 1, off);
    glUniform1f(u_scale_, float(s));

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
    base_count_ = 0; text_count_ = 0; have_cam_ = false;
    have_range_ = false;
    if (chart_) { tile57_chart_close(chart_); chart_ = nullptr; }
}

} // namespace t57
