// chart_renderer.cpp — see chart_renderer.h.
#include "chart_renderer.h"
#include "gl.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <wx/image.h>
#include <wx/mstream.h>
#include "earcut.hpp"

namespace t57 {

namespace {
constexpr double kPi = 3.14159265358979323846;
// SCAMIN gate constant (tile57 render/resolve.zig DENOM_Z0): a feature shows at
// zoom >= log2(DENOM_Z0 / scamin).
constexpr double kDenomZ0 = 279541132.0;
constexpr float kAlwaysVisible = -1e30f;

void lonlat_to_world(double lon, double lat, double& wx, double& wy) {
    wx = (lon + 180.0) / 360.0;
    double s = std::sin(lat * kPi / 180.0);
    wy = 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * kPi);
}
float scamin_threshold(int64_t scamin) {
    if (scamin <= 0) return kAlwaysVisible;
    return (float)std::log2(kDenomZ0 / (double)scamin);
}
uint64_t mariner_hash(const tile57_mariner& m) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&m);
    for (size_t i = 0; i < sizeof(m); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
}  // namespace

// ---- tessellation ----------------------------------------------------------
// World area fill: earcut the rings (outer + holes) directly in world space.
// Rings are decimated to the portrayal's pixel grid first — dropping sub-half-
// pixel detail cuts the triangle count hugely on dense coastlines/depth areas
// (the per-frame draw cost, hence the zoom framerate) with no visible change.
void ChartRenderer::on_fill_area(const tile57_world_rings* p, tile57_rgba c, float thresh) {
    if (!p || p->n < 3) return;
    using Pt = std::array<double, 2>;
    const double eps2 = decimate_eps_ * decimate_eps_;
    std::vector<std::vector<Pt>> poly;
    for (uint32_t r = 0; r < p->ring_count; ++r) {
        uint32_t s = p->ring_starts[r];
        uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
        if (e <= s + 2) continue;
        std::vector<Pt> ring{{p->pts[s].x, p->pts[s].y}};
        for (uint32_t i = s + 1; i < e; ++i) {
            double dx = p->pts[i].x - ring.back()[0], dy = p->pts[i].y - ring.back()[1];
            if (i == e - 1 || dx * dx + dy * dy >= eps2) ring.push_back({p->pts[i].x, p->pts[i].y});
        }
        if (ring.size() >= 3) poly.push_back(std::move(ring));
    }
    if (poly.empty()) return;
    std::vector<Pt> flat;
    for (const auto& ring : poly) flat.insert(flat.end(), ring.begin(), ring.end());
    std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            double wx = flat[idx[i + k]][0] - ref_wx_, wy = flat[idx[i + k]][1] - ref_wy_;
            area_.push_back({ (float)wx, (float)wy, 0, 0, c.r, c.g, c.b, c.a, thresh });
        }
}

// World line: expand each segment to a screen-px-wide quad — aWorld is the
// endpoint (world), aPost carries the perpendicular * half-width in px, applied
// AFTER the world transform so the line is a constant screen width at any zoom.
void ChartRenderer::on_stroke_line(const tile57_world_rings* p, float width_px, tile57_rgba c, float thresh) {
    if (!p || p->n < 2) return;
    float hw = std::max(0.5f, width_px * 0.5f);
    const double eps2 = decimate_eps_ * decimate_eps_;
    for (uint32_t r = 0; r < p->ring_count; ++r) {
        uint32_t s = p->ring_starts[r];
        uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
        // Decimate to the pixel grid before expanding (see on_fill_area).
        std::vector<std::array<double, 2>> kept{{p->pts[s].x, p->pts[s].y}};
        for (uint32_t i = s + 1; i < e; ++i) {
            double dx = p->pts[i].x - kept.back()[0], dy = p->pts[i].y - kept.back()[1];
            if (i == e - 1 || dx * dx + dy * dy >= eps2) kept.push_back({p->pts[i].x, p->pts[i].y});
        }
        for (size_t i = 0; i + 1 < kept.size(); ++i) {
            double ax = kept[i][0] - ref_wx_, ay = kept[i][1] - ref_wy_;
            double bx = kept[i + 1][0] - ref_wx_, by = kept[i + 1][1] - ref_wy_;
            double dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-12) continue;
            float nx = (float)(-dy / len) * hw, ny = (float)(dx / len) * hw;  // px offset
            auto v = [&](double wx, double wy, float ox, float oy) {
                line_.push_back({ (float)wx, (float)wy, ox, oy, c.r, c.g, c.b, c.a, thresh });
            };
            v(ax, ay, nx, ny); v(bx, by, nx, ny); v(bx, by, -nx, -ny);
            v(ax, ay, nx, ny); v(bx, by, -nx, -ny); v(ax, ay, -nx, -ny);
        }
    }
}

// Anchored fill (symbol / text): tessellate the local (px) rings with an
// even-odd rule (group rings by containment parity so counters cut out and no
// triangles bridge between letters), anchored at one world point via aWorld.
static void tess_local_even_odd(const tile57_local_rings* p, double awx, double awy,
                                tile57_rgba c, float thresh, std::vector<ChartRenderer::Vtx>& out) {
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
    const size_t n = rings.size();
    if (n == 0) return;
    auto in_ring = [](float x, float y, const std::vector<Pt>& ring) {
        bool in = false;
        for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
            if (((ring[i][1] > y) != (ring[j][1] > y)) &&
                (x < (ring[j][0] - ring[i][0]) * (y - ring[i][1]) / (ring[j][1] - ring[i][1]) + ring[i][0]))
                in = !in;
        return in;
    };
    std::vector<double> area(n, 0.0);
    std::vector<int> depth(n, 0), parent(n, -1);
    for (size_t i = 0; i < n; ++i) {
        double a = 0;
        for (size_t k = 0, j = rings[i].size() - 1; k < rings[i].size(); j = k++)
            a += (double)rings[i][j][0] * rings[i][k][1] - (double)rings[i][k][0] * rings[i][j][1];
        area[i] = std::fabs(a) * 0.5;
    }
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            if (in_ring(rings[i][0][0], rings[i][0][1], rings[j])) {
                ++depth[i];
                if (parent[i] < 0 || area[j] < area[parent[i]]) parent[i] = (int)j;
            }
        }
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2) continue;
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
                out.push_back({ (float)awx, (float)awy, flat[idx[t + k]][0], flat[idx[t + k]][1],
                                c.r, c.g, c.b, c.a, thresh });
    }
}

void ChartRenderer::on_draw_symbol(tile57_world_point anchor, const tile57_local_rings* p,
                                   tile57_rgba c, int even_odd, float stroke_w, float thresh) {
    (void)even_odd;
    double awx = anchor.x - ref_wx_, awy = anchor.y - ref_wy_;
    if (stroke_w > 0) {
        // Symbol stroke: expand local polylines to px quads (aPost in local px).
        float hw = std::max(0.5f, stroke_w * 0.5f);
        for (uint32_t r = 0; r < p->ring_count; ++r) {
            uint32_t s = p->ring_starts[r];
            uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
            for (uint32_t i = s; i + 1 < e; ++i) {
                float ax = p->pts[i].x, ay = p->pts[i].y, bx = p->pts[i + 1].x, by = p->pts[i + 1].y;
                float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-4f) continue;
                float nx = -dy / len * hw, ny = dx / len * hw;
                auto v = [&](float ox, float oy) {
                    symbol_.push_back({ (float)awx, (float)awy, ox, oy, c.r, c.g, c.b, c.a, thresh });
                };
                v(ax + nx, ay + ny); v(bx + nx, by + ny); v(bx - nx, by - ny);
                v(ax + nx, ay + ny); v(bx - nx, by - ny); v(ax - nx, ay - ny);
            }
        }
    } else {
        tess_local_even_odd(p, awx, awy, c, thresh, symbol_);
    }
}

void ChartRenderer::on_draw_text(tile57_world_point anchor, const tile57_local_rings* g,
                                 tile57_rgba c, tile57_rgba /*halo*/, float thresh) {
    tess_local_even_odd(g, anchor.x - ref_wx_, anchor.y - ref_wy_, c, thresh, text_);
}

// ---- sprite atlas (shared; loaded once in a GL context) --------------------
namespace {
struct SpriteAtlas {
    uint32_t tex = 0;
    std::unordered_map<std::string, std::array<float, 4>> uv;  // name -> u0,v0,u1,v1
    bool tried = false, ok = false;
};
SpriteAtlas g_atlas;

double json_num(const char* s, size_t lo, size_t hi, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    for (size_t p = lo; p + k.size() < hi; ++p)
        if (std::memcmp(s + p, k.data(), k.size()) == 0) {
            size_t q = p + k.size();
            while (q < hi && (s[q] == ' ' || s[q] == ':')) ++q;
            return std::atof(s + q);
        }
    return 0;
}

// Parse a MapLibre sprite JSON ({"name":{"x":,"y":,"width":,"height":},..}) into
// name -> normalised UV rect. The pivot-centred entry is the bare name.
void parse_sprite_json(const char* s, size_t len, int tw, int th,
                       std::unordered_map<std::string, std::array<float, 4>>& out) {
    if (tw <= 0 || th <= 0 || !s) return;
    size_t i = 0;
    auto ws = [&] { while (i < len && (s[i]==' '||s[i]=='\n'||s[i]=='\t'||s[i]=='\r')) ++i; };
    ws(); if (i >= len || s[i] != '{') return; ++i;
    while (i < len) {
        ws(); if (i < len && s[i] == '}') break;
        if (i >= len || s[i] != '"') break;
        ++i; size_t ns = i; while (i < len && s[i] != '"') ++i;
        std::string name(s + ns, i - ns); if (i < len) ++i;
        ws(); if (i < len && s[i] == ':') ++i; ws();
        if (i >= len || s[i] != '{') break;
        int depth = 0; size_t os = i;
        while (i < len) { char ch = s[i]; if (ch=='{') ++depth; else if (ch=='}') { if (--depth==0) { ++i; break; } } ++i; }
        double x = json_num(s, os, i, "x"), y = json_num(s, os, i, "y");
        double w = json_num(s, os, i, "width"), h = json_num(s, os, i, "height");
        if (w > 0 && h > 0)
            out[name] = { (float)(x/tw), (float)(y/th), (float)((x+w)/tw), (float)((y+h)/th) };
        ws(); if (i < len && s[i] == ',') ++i;
    }
}

void load_sprite_atlas() {
    if (g_atlas.tried) return;
    g_atlas.tried = true;
    tile57_assets assets{};
    // sprite-mln: pivot-centred cells + {name:{x,y,w,h}} JSON. NULL => embedded.
    if (!tile57_bake_sprite_mln(nullptr, &assets)) return;
    if (assets.sprite_png && assets.sprite_png_len) {
        wxMemoryInputStream mis(assets.sprite_png, assets.sprite_png_len);
        wxImage img;
        if (img.LoadFile(mis, wxBITMAP_TYPE_PNG)) {
            int tw = img.GetWidth(), th = img.GetHeight();
            std::vector<uint8_t> rgba((size_t)tw * th * 4);
            const uint8_t* rgb = img.GetData();
            const uint8_t* al = img.HasAlpha() ? img.GetAlpha() : nullptr;
            for (int p = 0; p < tw * th; ++p) {
                rgba[p*4+0] = rgb[p*3+0]; rgba[p*4+1] = rgb[p*3+1]; rgba[p*4+2] = rgb[p*3+2];
                rgba[p*4+3] = al ? al[p] : 255;
            }
            glGenTextures(1, &g_atlas.tex);
            glBindTexture(GL_TEXTURE_2D, g_atlas.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            parse_sprite_json((const char*)assets.sprite_json, assets.sprite_json_len, tw, th, g_atlas.uv);
            g_atlas.ok = g_atlas.tex != 0 && !g_atlas.uv.empty();
        }
    }
    tile57_assets_free(&assets);
}
}  // namespace

// Point symbol as a textured quad from the atlas: quad of ±half centred on the
// anchor, rotated, mapped to the symbol's atlas cell (AA'd by texture filtering).
void ChartRenderer::on_draw_sprite(const char* name, size_t len, tile57_world_point anchor,
                                   float rot_deg, float hw, float hh, float thresh) {
    if (!g_atlas.ok || hw <= 0 || hh <= 0) return;
    auto it = g_atlas.uv.find(std::string(name, len));
    if (it == g_atlas.uv.end()) return;
    const auto& uv = it->second;  // u0, v0, u1, v1
    float awx = (float)(anchor.x - ref_wx_), awy = (float)(anchor.y - ref_wy_);
    float rad = rot_deg * (float)kPi / 180.0f, c = std::cos(rad), s = std::sin(rad);
    auto corner = [&](float lx, float ly, float u, float vv) {
        sprite_.push_back({ awx, awy, lx*c - ly*s, lx*s + ly*c, u, vv, thresh });
    };
    corner(-hw, -hh, uv[0], uv[1]); corner(hw, -hh, uv[2], uv[1]); corner(hw, hh, uv[2], uv[3]);
    corner(-hw, -hh, uv[0], uv[1]); corner(hw, hh, uv[2], uv[3]); corner(-hw, hh, uv[0], uv[3]);
}

// ---- C surface trampolines (ctx == ChartRenderer*) -------------------------
static float feat_thresh(const tile57_feature* f) { return scamin_threshold(f ? f->scamin : 0); }
static void tr_fill(void* c, const tile57_feature* f, const tile57_world_rings* r, tile57_rgba col, int) {
    static_cast<ChartRenderer*>(c)->on_fill_area(r, col, feat_thresh(f));
}
static void tr_stroke(void* c, const tile57_feature* f, const tile57_world_rings* l, float w, float, float, tile57_rgba col) {
    static_cast<ChartRenderer*>(c)->on_stroke_line(l, w, col, feat_thresh(f));
}
static void tr_symbol(void* c, const tile57_feature* f, tile57_world_point a, const tile57_local_rings* r, tile57_rgba col, int eo, float sw) {
    static_cast<ChartRenderer*>(c)->on_draw_symbol(a, r, col, eo, sw, feat_thresh(f));
}
static void tr_text(void* c, const tile57_feature* f, tile57_world_point a, const tile57_local_rings* g, tile57_rgba col, tile57_rgba halo, float) {
    static_cast<ChartRenderer*>(c)->on_draw_text(a, g, col, halo, feat_thresh(f));
}
static void tr_sprite(void* c, const tile57_feature* f, const char* name, size_t len,
                      tile57_world_point a, float rot, float hw, float hh) {
    static_cast<ChartRenderer*>(c)->on_draw_sprite(name, len, a, rot, hw, hh, feat_thresh(f));
}

// ---- chart / GL lifecycle --------------------------------------------------
bool ChartRenderer::open_chart(const std::string& path) {
    if (chart_) return true;
    // A .pmtiles bundle and a live .000 cell take different open entry points
    // (open_pmtiles streams the archive; open reads an S-57 cell / ENC_ROOT).
    auto ends_with = [&](const char* suf) {
        std::string s(suf);
        return path.size() >= s.size() && path.compare(path.size() - s.size(), s.size(), s) == 0;
    };
    chart_ = ends_with(".pmtiles") ? tile57_chart_open_pmtiles(path.c_str())
                                   : tile57_chart_open(path.c_str());
    return chart_ != nullptr;
}
bool ChartRenderer::get_info(tile57_chart_info& out) const {
    if (!chart_) return false;
    tile57_chart_get_info(chart_, &out);
    return true;
}

static const char* VS = R"(
attribute vec2 aWorld;
attribute vec2 aPost;
attribute vec4 aColor;
attribute float aThresh;
uniform float uScale;
uniform vec2 uOrigin;
uniform vec2 uVp;
uniform float uZoom;
varying vec4 vCol;
void main(){
  if (uZoom < aThresh) { gl_Position = vec4(2.0,2.0,2.0,1.0); vCol = vec4(0.0); return; }
  vec2 screen = aWorld*uScale + uOrigin + aPost;
  vec2 ndc = vec2(screen.x/uVp.x*2.0-1.0, 1.0 - screen.y/uVp.y*2.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
  vCol = aColor;
}
)";
static const char* FS = R"(
varying vec4 vCol;
void main(){ gl_FragColor = vec4(vCol.rgb*vCol.a, vCol.a); }
)";
// Sprite program: same world transform, but sample the atlas at aUV. Output
// premultiplied so it composites with GL_ONE, GL_ONE_MINUS_SRC_ALPHA.
static const char* VS_SPRITE = R"(
attribute vec2 aWorld;
attribute vec2 aPost;
attribute vec2 aUV;
attribute float aThresh;
uniform float uScale;
uniform vec2 uOrigin;
uniform vec2 uVp;
uniform float uZoom;
varying vec2 vUV;
void main(){
  if (uZoom < aThresh) { gl_Position = vec4(2.0,2.0,2.0,1.0); vUV = vec2(0.0); return; }
  vec2 screen = aWorld*uScale + uOrigin + aPost;
  vec2 ndc = vec2(screen.x/uVp.x*2.0-1.0, 1.0 - screen.y/uVp.y*2.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
  vUV = aUV;
}
)";
static const char* FS_SPRITE = R"(
uniform sampler2D uAtlas;
varying vec2 vUV;
void main(){ vec4 t = texture2D(uAtlas, vUV); gl_FragColor = vec4(t.rgb*t.a, t.a); }
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
    glBindAttribLocation(prog_, 0, "aWorld");
    glBindAttribLocation(prog_, 1, "aPost");
    glBindAttribLocation(prog_, 2, "aColor");
    glBindAttribLocation(prog_, 3, "aThresh");
    glLinkProgram(prog_);
    u_scale_ = glGetUniformLocation(prog_, "uScale");
    u_origin_ = glGetUniformLocation(prog_, "uOrigin");
    u_vp_ = glGetUniformLocation(prog_, "uVp");
    u_zoom_ = glGetUniformLocation(prog_, "uZoom");
    glGenBuffers(1, &vbo_area_);
    glGenBuffers(1, &vbo_line_);
    glGenBuffers(1, &vbo_symbol_);
    glGenBuffers(1, &vbo_text_);

    // Sprite program (textured point symbols).
    prog_sprite_ = glCreateProgram();
    glAttachShader(prog_sprite_, compile(GL_VERTEX_SHADER, VS_SPRITE));
    glAttachShader(prog_sprite_, compile(GL_FRAGMENT_SHADER, FS_SPRITE));
    glBindAttribLocation(prog_sprite_, 0, "aWorld");
    glBindAttribLocation(prog_sprite_, 1, "aPost");
    glBindAttribLocation(prog_sprite_, 2, "aUV");
    glBindAttribLocation(prog_sprite_, 3, "aThresh");
    glLinkProgram(prog_sprite_);
    su_scale_ = glGetUniformLocation(prog_sprite_, "uScale");
    su_origin_ = glGetUniformLocation(prog_sprite_, "uOrigin");
    su_vp_ = glGetUniformLocation(prog_sprite_, "uVp");
    su_zoom_ = glGetUniformLocation(prog_sprite_, "uZoom");
    su_atlas_ = glGetUniformLocation(prog_sprite_, "uAtlas");
    glGenBuffers(1, &vbo_sprite_);
    load_sprite_atlas();

    gl_ready_ = true;
    return true;
}

void ChartRenderer::rebuild(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                            const tile57_mariner& m) {
    area_.clear(); line_.clear(); symbol_.clear(); text_.clear(); sprite_.clear();
    // World reference: the view centre, so vertex offsets stay tiny (f32-precise).
    lonlat_to_world(lon, lat, ref_wx_, ref_wy_);
    // Decimate geometry to ~half a pixel at the portrayal zoom.
    decimate_eps_ = 0.5 / (256.0 * std::pow(2.0, zoom));

    tile57_surface_cb cb{};
    cb.ctx = this;
    cb.fill_area = tr_fill;
    cb.stroke_line = tr_stroke;
    cb.draw_symbol = tr_symbol;
    cb.draw_text = tr_text;
    // Point symbols draw as atlas sprites when the atlas loaded; else tessellate.
    if (g_atlas.ok) cb.draw_sprite = tr_sprite;
    int rc = tile57_chart_render_surface_cb(chart_, lon, lat, zoom, w, h, &m, &cb);
    if (rc != 0) std::fprintf(stderr, "t57 render_surface_cb rc=%d\n", rc);
    upload();
}

void ChartRenderer::upload() {
    auto up = [](uint32_t vbo, const std::vector<Vtx>& v, uint32_t& n) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(Vtx), v.data(), GL_DYNAMIC_DRAW);
        n = (uint32_t)v.size();
    };
    up(vbo_area_, area_, n_area_);
    up(vbo_line_, line_, n_line_);
    up(vbo_symbol_, symbol_, n_symbol_);
    up(vbo_text_, text_, n_text_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_sprite_);
    glBufferData(GL_ARRAY_BUFFER, sprite_.size() * sizeof(SpriteVtx), sprite_.data(), GL_DYNAMIC_DRAW);
    n_sprite_ = (uint32_t)sprite_.size();
}

void ChartRenderer::draw_range(uint32_t vbo, uint32_t count) {
    if (!count) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, wx));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, px));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)offsetof(Vtx, r));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, thresh));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
}

void ChartRenderer::render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                           const tile57_mariner& m, Pass pass, bool stencil_clip) {
    if (!chart_ || !ensure_gl()) return;
    if (!have_range_) {
        tile57_chart_info info{};
        tile57_chart_get_info(chart_, &info);
        min_zoom_ = info.min_zoom; max_zoom_ = info.max_zoom; have_range_ = true;
    }
    // Portray at the view zoom clamped to the baked band (tile selection); the
    // GPU then transforms to ANY zoom, so pan and zoom are re-portray-free.
    double cz = zoom;
    if (cz < min_zoom_) cz = min_zoom_;
    if (cz > max_zoom_) cz = max_zoom_;

    // Re-portray on zoom only once the gesture SETTLES: while the zoom is still
    // changing frame-to-frame we transform the cached geometry (smooth, like a
    // native chart), and refresh to crisp detail when it stops. A large excursion
    // (>3 levels) refreshes even mid-gesture to avoid extreme scaling artefacts.
    bool zoom_settled = std::fabs(zoom - last_zoom_) < 0.02;
    if (pass != Pass::kText) last_zoom_ = zoom;
    bool zoom_stale = std::fabs(cz - cam_zoom_) > 0.3;

    uint64_t mh = mariner_hash(m);
    // Pan margin: a small overscan so short pans replay without re-portraying.
    // Kept tight — portrayal + tessellation cost scales with camera AREA, and a
    // big buffer is slow to draw every frame (it was the zoom-blank cause).
    auto cam_dim = [&](uint32_t v) { return (uint32_t)std::min(std::ceil(v * 1.15), 4096.0); };
    bool need = !have_cam_ || mh != cam_mhash_
             || cam_w_ < cam_dim(w) / 2 || cam_h_ < cam_dim(h) / 2
             || (zoom_stale && (zoom_settled || std::fabs(cz - cam_zoom_) > 3.0));
    if (!need) {
        // Pan check: is the view centre still well inside the portrayed window?
        double cwx, cwy, vwx, vwy;
        lonlat_to_world(cam_lon_, cam_lat_, cwx, cwy);
        lonlat_to_world(lon, lat, vwx, vwy);
        double scale_px = 256.0 * std::pow(2.0, cam_zoom_);
        double dx = std::fabs(vwx - cwx) * scale_px, dy = std::fabs(vwy - cwy) * scale_px;
        if (dx + w * 0.5 > cam_w_ * 0.5 || dy + h * 0.5 > cam_h_ * 0.5) need = true;
    }
    // The text pass never re-portrays (it trails the base pass on OpenCPN's
    // compose; regenerating would desync the shared buffers).
    if (pass == Pass::kText && have_cam_) need = false;

    if (need) {
        cam_lon_ = lon; cam_lat_ = lat; cam_zoom_ = cz;
        cam_w_ = cam_dim(w); cam_h_ = cam_dim(h); cam_mhash_ = mh;
        rebuild(lon, lat, cz, cam_w_, cam_h_, m);
        have_cam_ = true;
    }

    // Per-frame transform: screen = aWorld*uScale + uOrigin + aPost.
    double vwx, vwy;
    lonlat_to_world(lon, lat, vwx, vwy);
    double scale_px = 256.0 * std::pow(2.0, zoom);   // px per world[0,1] unit
    double ox = (ref_wx_ - vwx) * scale_px + w * 0.5;
    double oy = (ref_wy_ - vwy) * scale_px + h * 0.5;

    (void)stencil_clip;
    glUseProgram(prog_);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_DEPTH_TEST);
    // Clip to the chart's quilt patch via OpenCPN's SCISSOR (glChartCanvas::
    // SetClipRect sets it) rather than a stencil EQUAL test: the stencil buffer
    // isn't reliably marked for us across GL backends (culled the whole base on
    // some drivers, leaving only the un-stencilled text pass).
    glDisable(GL_STENCIL_TEST);
    glUniform1f(u_scale_, (float)scale_px);
    float origin[2] = { (float)ox, (float)oy };
    glUniform2fv(u_origin_, 1, origin);
    float vp[2] = { (float)w, (float)h };
    glUniform2fv(u_vp_, 1, vp);
    glUniform1f(u_zoom_, (float)zoom);

    if (pass != Pass::kText) {
        draw_range(vbo_area_, n_area_);
        draw_range(vbo_line_, n_line_);
        draw_range(vbo_symbol_, n_symbol_);
        if (n_sprite_ && g_atlas.ok) {
            glUseProgram(prog_sprite_);
            glUniform1f(su_scale_, (float)scale_px);
            glUniform2fv(su_origin_, 1, origin);
            glUniform2fv(su_vp_, 1, vp);
            glUniform1f(su_zoom_, (float)zoom);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_atlas.tex);
            glUniform1i(su_atlas_, 0);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_sprite_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx), (void*)offsetof(SpriteVtx, wx));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx), (void*)offsetof(SpriteVtx, px));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx), (void*)offsetof(SpriteVtx, u));
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx), (void*)offsetof(SpriteVtx, thresh));
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)n_sprite_);
            glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(2); glDisableVertexAttribArray(3);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(prog_);
        }
    }
    if (pass != Pass::kBase) draw_range(vbo_text_, n_text_);

    if (stencil_clip) glDisable(GL_STENCIL_TEST);
    glUseProgram(0);
}

void ChartRenderer::shutdown() {
    gl_ready_ = false; prog_ = prog_sprite_ = 0;
    vbo_area_ = vbo_line_ = vbo_symbol_ = vbo_text_ = vbo_sprite_ = 0;
    n_area_ = n_line_ = n_symbol_ = n_text_ = n_sprite_ = 0;
    have_cam_ = false; have_range_ = false;
    if (chart_) { tile57_chart_close(chart_); chart_ = nullptr; }
}

} // namespace t57
