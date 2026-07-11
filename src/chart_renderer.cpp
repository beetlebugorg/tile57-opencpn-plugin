// chart_renderer.cpp — see chart_renderer.h.
#include "chart_renderer.h"
#include "gl.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <wx/log.h>   // reliable capture in opencpn.log (stderr isn't captured here)
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
// Normalised UV rect + the cell's logical px size (atlas px / pixelRatio), used
// as the pattern tile's screen size (× size_scale).
struct AtlasRect { float u0, v0, u1, v1, px_w, px_h; };
struct SpriteAtlas {
    uint32_t tex = 0;
    std::unordered_map<std::string, AtlasRect> uv;  // name -> rect
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
                       std::unordered_map<std::string, AtlasRect>& out) {
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
        double ratio = json_num(s, os, i, "pixelRatio"); if (ratio <= 0) ratio = 1;
        if (w > 0 && h > 0)
            out[name] = { (float)(x/tw), (float)(y/th), (float)((x+w)/tw), (float)((y+h)/th),
                          (float)(w/ratio), (float)(h/ratio) };
        ws(); if (i < len && s[i] == ',') ++i;
    }
}

void load_sprite_atlas() {
    if (g_atlas.tried) return;
    g_atlas.tried = true;
    tile57_assets assets{};
    // sprite-mln: pivot-centred cells + {name:{x,y,w,h}} JSON. NULL => embedded.
    if (tile57_bake_sprite_mln(nullptr, &assets, nullptr) != TILE57_OK) return;
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

// ---- SDF glyph atlas (shared; loaded once in a GL context) -----------------
struct GlyphInfo { float u0, v0, u1, v1, ox, oy, w, h, adv; };  // UV + EM-space quad
struct GlyphAtlas {
    uint32_t tex = 0;
    std::unordered_map<uint32_t, GlyphInfo> g;  // codepoint -> metrics
    bool tried = false, ok = false;
};
GlyphAtlas g_glyph;

// Parse {"em_px":..,"pad":..,"glyphs":{"cp":[u0,v0,u1,v1,ox,oy,w,h,adv],..}}.
void parse_glyph_json(const char* s, size_t len, std::unordered_map<uint32_t, GlyphInfo>& out) {
    if (!s || len < 8) return;
    size_t i = 0;
    for (; i + 8 < len; ++i) if (std::memcmp(s + i, "\"glyphs\"", 8) == 0) break;
    i += 8;
    while (i < len && s[i] != '{') ++i;
    if (i >= len) return;
    ++i;
    auto ws = [&] { while (i < len && (s[i]==' '||s[i]=='\n'||s[i]=='\t'||s[i]=='\r'||s[i]==',')) ++i; };
    while (i < len) {
        ws(); if (i < len && s[i] == '}') break;
        if (i >= len || s[i] != '"') break;
        ++i; uint32_t cp = (uint32_t)std::atoi(s + i);
        while (i < len && s[i] != '"') ++i; if (i < len) ++i;
        ws(); if (i < len && s[i] == ':') ++i; ws();
        if (i >= len || s[i] != '[') break; ++i;
        float v[9] = {0};
        for (int k = 0; k < 9; ++k) {
            ws(); v[k] = (float)std::atof(s + i);
            while (i < len && s[i] != ',' && s[i] != ']') ++i;
            if (i < len && s[i] == ',') ++i;
        }
        while (i < len && s[i] != ']') ++i; if (i < len) ++i;
        out[cp] = { v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8] };
    }
}

void load_glyph_atlas() {
    if (g_glyph.tried) return;
    g_glyph.tried = true;
    tile57_assets a{};
    if (tile57_bake_glyph_sdf(&a, nullptr) != TILE57_OK) return;
    if (a.sprite_png && a.sprite_png_len) {
        wxMemoryInputStream mis(a.sprite_png, a.sprite_png_len);
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
            glGenTextures(1, &g_glyph.tex);
            glBindTexture(GL_TEXTURE_2D, g_glyph.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            parse_glyph_json((const char*)a.sprite_json, a.sprite_json_len, g_glyph.g);
            g_glyph.ok = g_glyph.tex != 0 && !g_glyph.g.empty();
        }
    }
    tile57_assets_free(&a);
}

// Minimal UTF-8 decode (advances i); covers the atlas's ASCII + Latin-1 range.
uint32_t decode_utf8(const char* s, size_t len, size_t& i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int n; uint32_t cp;
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else return c;
    for (int k = 0; k < n && i < len; ++k) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}
}  // namespace

// Point symbol as a textured quad from the atlas: quad of ±half centred on the
// anchor, rotated, mapped to the symbol's atlas cell (AA'd by texture filtering).
void ChartRenderer::on_draw_sprite(const char* name, size_t len, tile57_world_point anchor,
                                   float rot_deg, float hw, float hh, float thresh) {
    if (!g_atlas.ok || hw <= 0 || hh <= 0) return;
    auto it = g_atlas.uv.find(std::string(name, len));
    if (it == g_atlas.uv.end()) return;
    const AtlasRect& r = it->second;
    float awx = (float)(anchor.x - ref_wx_), awy = (float)(anchor.y - ref_wy_);
    float rad = rot_deg * (float)kPi / 180.0f, c = std::cos(rad), s = std::sin(rad);
    auto corner = [&](float lx, float ly, float u, float vv) {
        sprite_.push_back({ awx, awy, lx*c - ly*s, lx*s + ly*c, u, vv, thresh });
    };
    corner(-hw, -hh, r.u0, r.v0); corner(hw, -hh, r.u1, r.v0); corner(hw, hh, r.u1, r.v1);
    corner(-hw, -hh, r.u0, r.v0); corner(hw, hh, r.u1, r.v1); corner(-hw, hh, r.u0, r.v1);
}

// Area fill pattern: tessellate the polygon and tile the pattern cell across it
// (world-anchored, constant screen size) via the pattern program.
void ChartRenderer::on_draw_pattern(const char* name, size_t len,
                                    const tile57_world_rings* p, float thresh) {
    if (!g_atlas.ok || !p || p->n < 3) return;
    auto it = g_atlas.uv.find("pat:" + std::string(name, len));
    if (it == g_atlas.uv.end()) {  // unknown pattern -> flat translucent tint
        on_fill_area(p, tile57_rgba{160, 160, 170, 140}, thresh);
        return;
    }
    const AtlasRect& r = it->second;
    float tw = std::max(1.0f, r.px_w * (float)size_scale_);   // tile size in screen px
    float th = std::max(1.0f, r.px_h * (float)size_scale_);
    using Pt = std::array<double, 2>;
    const double eps2 = decimate_eps_ * decimate_eps_;
    std::vector<std::vector<Pt>> poly;
    for (uint32_t rr = 0; rr < p->ring_count; ++rr) {
        uint32_t s = p->ring_starts[rr];
        uint32_t e = (rr + 1 < p->ring_count) ? p->ring_starts[rr + 1] : p->n;
        if (e <= s + 2) continue;
        std::vector<Pt> ring{{p->pts[s].x, p->pts[s].y}};
        for (uint32_t i = s + 1; i < e; ++i) {
            double dx = p->pts[i].x - ring.back()[0], dy = p->pts[i].y - ring.back()[1];
            if (i == e - 1 || dx*dx + dy*dy >= eps2) ring.push_back({p->pts[i].x, p->pts[i].y});
        }
        if (ring.size() >= 3) poly.push_back(std::move(ring));
    }
    if (poly.empty()) return;
    std::vector<Pt> flat;
    for (const auto& ring : poly) flat.insert(flat.end(), ring.begin(), ring.end());
    std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            float wx = (float)(flat[idx[i + k]][0] - ref_wx_), wy = (float)(flat[idx[i + k]][1] - ref_wy_);
            pattern_.push_back({ wx, wy, r.u0, r.v0, r.u1, r.v1, tw, th, thresh });
        }
}

// Lay a label out from the SDF glyph atlas: one textured quad per glyph at the
// anchor + local px (origin + pen + glyph offset, all × the text px size). The
// atlas is monochrome SDF, tinted by the text colour in the glyph shader.
void ChartRenderer::on_draw_text_str(tile57_world_point anchor, float ox, float oy,
                                     const char* text, size_t len, float size_px,
                                     tile57_rgba c, tile57_rgba /*halo*/, float thresh) {
    if (!g_glyph.ok || size_px <= 0 || !text) return;
    float awx = (float)(anchor.x - ref_wx_), awy = (float)(anchor.y - ref_wy_);
    float pen = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = decode_utf8(text, len, i);
        auto it = g_glyph.g.find(cp);
        if (it == g_glyph.g.end()) { it = g_glyph.g.find('?'); if (it == g_glyph.g.end()) continue; }
        const GlyphInfo& g = it->second;
        if (g.w > 0 && g.h > 0) {
            float x0 = ox + pen + g.ox * size_px, y0 = oy + g.oy * size_px;
            float x1 = x0 + g.w * size_px, y1 = y0 + g.h * size_px;
            auto v = [&](float px, float py, float u, float vv) {
                glyph_.push_back({ awx, awy, px, py, u, vv, c.r, c.g, c.b, c.a, thresh });
            };
            v(x0, y0, g.u0, g.v0); v(x1, y0, g.u1, g.v0); v(x1, y1, g.u1, g.v1);
            v(x0, y0, g.u0, g.v0); v(x1, y1, g.u1, g.v1); v(x0, y1, g.u0, g.v1);
        }
        pen += g.adv * size_px;
    }
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
static void tr_pattern(void* c, const tile57_feature* f, const char* name, size_t len,
                       const tile57_world_rings* rings) {
    static_cast<ChartRenderer*>(c)->on_draw_pattern(name, len, rings, feat_thresh(f));
}
static void tr_text_str(void* c, const tile57_feature* f, tile57_world_point a, float ox, float oy,
                        const char* text, size_t len, float size, tile57_rgba col, tile57_rgba halo) {
    static_cast<ChartRenderer*>(c)->on_draw_text_str(a, ox, oy, text, len, size, col, halo, feat_thresh(f));
}

// ---- chart / GL lifecycle --------------------------------------------------
bool ChartRenderer::open_chart(const std::string& path) {
    if (chart_) return true;
    // Only baked .pmtiles archives open as charts (the v0.3 ABI bakes raw cells
    // first — see ChartTile57::start_bake). Resolve symlinks so mmap sees the
    // real file.
    std::string real = path;
    if (char* rp = realpath(path.c_str(), nullptr)) { real = rp; std::free(rp); }
    return tile57_chart_open(real.c_str(), &chart_, nullptr) == TILE57_OK && chart_;
}
void ChartRenderer::set_chart(tile57_chart* c) {
    if (c == chart_) return;
    if (chart_) tile57_chart_close(chart_);
    chart_ = c;
    have_cam_ = false;    // force a re-portray against the newly swapped-in chart
    have_range_ = false;  // re-fetch zoom range + coverage bounds for the new chart
    if (gl_ready_) clear_tiles();   // the cached tiles belonged to the old chart
}
bool ChartRenderer::get_info(tile57_info& out) const {
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
// Pattern program: tile a pattern cell across a polygon, world-anchored at a
// constant screen size. vWorldPx (= aWorld*uScale) is the screen offset from the
// chart reference point, so fract(vWorldPx/tile) keeps the pattern fixed to the
// chart. The cell is a sub-rect of the atlas; wrap into it with fract.
static const char* VS_PAT = R"(
attribute vec2 aWorld;
attribute vec4 aRect;
attribute vec2 aTile;
attribute float aThresh;
uniform float uScale;
uniform vec2 uOrigin;
uniform vec2 uVp;
uniform float uZoom;
varying vec4 vRect;
varying vec2 vTile;
varying vec2 vWorldPx;
void main(){
  if (uZoom < aThresh) { gl_Position = vec4(2.0,2.0,2.0,1.0); return; }
  vec2 wpx = aWorld*uScale;
  vec2 screen = wpx + uOrigin;
  vec2 ndc = vec2(screen.x/uVp.x*2.0-1.0, 1.0 - screen.y/uVp.y*2.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
  vRect = aRect; vTile = aTile; vWorldPx = wpx;
}
)";
static const char* FS_PAT = R"(
uniform sampler2D uAtlas;
varying vec4 vRect;
varying vec2 vTile;
varying vec2 vWorldPx;
void main(){
  vec2 f = fract(vWorldPx / vTile);
  vec2 uv = vRect.xy + f * (vRect.zw - vRect.xy);
  vec4 t = texture2D(uAtlas, uv);
  gl_FragColor = vec4(t.rgb*t.a, t.a);
}
)";
// Glyph program: same world transform + aPost (local px), sample the SDF atlas
// and threshold to a crisp, size-independent edge (fwidth = the on-screen slope),
// tinted by the per-glyph colour. Output premultiplied.
static const char* VS_GLYPH = R"(
attribute vec2 aWorld;
attribute vec2 aPost;
attribute vec2 aUV;
attribute vec4 aColor;
attribute float aThresh;
uniform float uScale;
uniform vec2 uOrigin;
uniform vec2 uVp;
uniform float uZoom;
varying vec2 vUV;
varying vec4 vCol;
void main(){
  if (uZoom < aThresh) { gl_Position = vec4(2.0,2.0,2.0,1.0); vUV = vec2(0.0); vCol = vec4(0.0); return; }
  vec2 screen = aWorld*uScale + uOrigin + aPost;
  vec2 ndc = vec2(screen.x/uVp.x*2.0-1.0, 1.0 - screen.y/uVp.y*2.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
  vUV = aUV; vCol = aColor;
}
)";
static const char* FS_GLYPH = R"(
uniform sampler2D uAtlas;
varying vec2 vUV;
varying vec4 vCol;
void main(){
  float d = texture2D(uAtlas, vUV).a;
  float w = fwidth(d);
  float a = smoothstep(0.5 - w, 0.5 + w, d);
  gl_FragColor = vec4(vCol.rgb*vCol.a, vCol.a) * a;
}
)";
// Composite program: draw the supersampled texture as a fullscreen quad,
// down-sampled by the sampler's linear filter (already premultiplied).
static const char* VS_BLIT = R"(
attribute vec2 aPos;
attribute vec2 aTex;
varying vec2 vTex;
void main(){ vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }
)";
static const char* FS_BLIT = R"(
uniform sampler2D uTex;
varying vec2 vTex;
void main(){ gl_FragColor = texture2D(uTex, vTex); }
)";
static uint32_t compile(GLenum t, const char* body) {
    std::string src = std::string(T57_GLSL_VERSION) + body;
    const char* s = src.c_str();
    uint32_t sh = glCreateShader(t); glShaderSource(sh, 1, &s, nullptr); glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[512]; glGetShaderInfoLog(sh, 512, nullptr, l); wxLogMessage("tile57 shader FAIL: %s", l); }
    else wxLogMessage("tile57 shader ok (type=0x%x)", (unsigned)t);
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

    // Pattern program (tiled area fills).
    prog_pat_ = glCreateProgram();
    glAttachShader(prog_pat_, compile(GL_VERTEX_SHADER, VS_PAT));
    glAttachShader(prog_pat_, compile(GL_FRAGMENT_SHADER, FS_PAT));
    glBindAttribLocation(prog_pat_, 0, "aWorld");
    glBindAttribLocation(prog_pat_, 1, "aRect");
    glBindAttribLocation(prog_pat_, 2, "aTile");
    glBindAttribLocation(prog_pat_, 3, "aThresh");
    glLinkProgram(prog_pat_);
    pu_scale_ = glGetUniformLocation(prog_pat_, "uScale");
    pu_origin_ = glGetUniformLocation(prog_pat_, "uOrigin");
    pu_vp_ = glGetUniformLocation(prog_pat_, "uVp");
    pu_zoom_ = glGetUniformLocation(prog_pat_, "uZoom");
    pu_atlas_ = glGetUniformLocation(prog_pat_, "uAtlas");
    glGenBuffers(1, &vbo_pat_);

    // Glyph program (SDF text).
    prog_glyph_ = glCreateProgram();
    glAttachShader(prog_glyph_, compile(GL_VERTEX_SHADER, VS_GLYPH));
    glAttachShader(prog_glyph_, compile(GL_FRAGMENT_SHADER, FS_GLYPH));
    glBindAttribLocation(prog_glyph_, 0, "aWorld");
    glBindAttribLocation(prog_glyph_, 1, "aPost");
    glBindAttribLocation(prog_glyph_, 2, "aUV");
    glBindAttribLocation(prog_glyph_, 3, "aColor");
    glBindAttribLocation(prog_glyph_, 4, "aThresh");
    glLinkProgram(prog_glyph_);
    gu_scale_ = glGetUniformLocation(prog_glyph_, "uScale");
    gu_origin_ = glGetUniformLocation(prog_glyph_, "uOrigin");
    gu_vp_ = glGetUniformLocation(prog_glyph_, "uVp");
    gu_zoom_ = glGetUniformLocation(prog_glyph_, "uZoom");
    gu_atlas_ = glGetUniformLocation(prog_glyph_, "uAtlas");
    glGenBuffers(1, &vbo_glyph_);

    // Composite program + a unit fullscreen quad (pos.xy, tex.uv).
    prog_blit_ = glCreateProgram();
    glAttachShader(prog_blit_, compile(GL_VERTEX_SHADER, VS_BLIT));
    glAttachShader(prog_blit_, compile(GL_FRAGMENT_SHADER, FS_BLIT));
    glBindAttribLocation(prog_blit_, 0, "aPos");
    glBindAttribLocation(prog_blit_, 1, "aTex");
    glLinkProgram(prog_blit_);
    bu_tex_ = glGetUniformLocation(prog_blit_, "uTex");
    const float quad[] = { -1,-1, 0,0,  1,-1, 1,0,  1,1, 1,1,
                           -1,-1, 0,0,  1,1, 1,1,  -1,1, 0,1 };
    glGenBuffers(1, &vbo_quad_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_quad_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    load_sprite_atlas();
    load_glyph_atlas();

    // Tiled path (the MapLibre model): portray+tessellate each baked tile once, cache
    // its GPU geometry, compose the view from cached tiles. This is the default and
    // primary path; TILE57_WHOLEVIEW forces the legacy whole-view re-portray fallback.
    tiled_ = std::getenv("TILE57_WHOLEVIEW") == nullptr;

    gl_ready_ = true;
    return true;
}

namespace {
// One supersample colour texture FBO shared by every chart (they render
// sequentially into the same GL context). Rendering into it at kSS× the
// viewport then compositing back down-sampled antialiases tessellated edges.
// One shared FBO (not one per chart) keeps it cheap; a plain texture target
// (no multisample resolve blit) avoids the scroll-compose glitch that a
// resolve introduced.
//
// Supersample factor. 2× SSAA costs 4× the fragment work plus a full-screen
// composite — fine on a real GPU, ruinous on a SOFTWARE rasterizer (llvmpipe &
// co.), where every fragment is CPU work. So detect a software renderer once
// (from GL_RENDERER, needs a live context) and drop to 1 (AA off, render
// direct) there. TILE57_SS=<n> forces it (1 = off, 2 = 2×). Must be called with
// a current GL context (render() guarantees it via ensure_gl()).
int ss_factor() {
    static int k = -1;
    if (k >= 0) return k;
    if (const char* e = std::getenv("TILE57_SS")) {
        int v = std::atoi(e);
        k = v < 1 ? 1 : (v > 4 ? 4 : v);
        return k;
    }
    k = 2;
    if (const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER))) {
        std::string s(r);
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        for (const char* sw : {"llvmpipe", "softpipe", "swrast", "swr", "software",
                               "swiftshader", "microsoft basic"})
            if (s.find(sw) != std::string::npos) { k = 1; break; }
    }
    wxLogMessage("tile57: supersample factor = %d", k);
    return k;
}
struct SsTarget {
    uint32_t fbo = 0, tex = 0; int w = 0, h = 0; bool ok = false;
};
SsTarget g_ss;

bool ensure_ss(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    int kSS = ss_factor();
    int sw = w * kSS, sh = h * kSS;
    if (g_ss.fbo && g_ss.w == sw && g_ss.h == sh) return g_ss.ok;
    if (!g_ss.fbo) { glGenFramebuffers(1, &g_ss.fbo); glGenTextures(1, &g_ss.tex); }
    glBindTexture(GL_TEXTURE_2D, g_ss.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, g_ss.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ss.tex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_ss.w = sw; g_ss.h = sh; g_ss.ok = ok;
    return ok;
}
}  // namespace

void ChartRenderer::composite_ss() {
    glUseProgram(prog_blit_);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_ss.tex);
    glUniform1i(bu_tex_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_quad_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

// Wire the surface callbacks to this renderer's tessellation sinks. Shared by the
// whole-view rebuild() and the tiled per-tile portray.
static void fill_surface_cb(tile57_surface_cb& cb, ChartRenderer* self) {
    cb.ctx = self;
    cb.fill_area = tr_fill;
    cb.stroke_line = tr_stroke;
    cb.draw_symbol = tr_symbol;
    cb.draw_text = tr_text;
    // Point symbols/patterns draw from the atlas when it loaded; else tessellate.
    if (g_atlas.ok) { cb.draw_sprite = tr_sprite; cb.draw_pattern = tr_pattern; }
    // SDF glyph atlas: send text as strings (skips glyph tessellation). If it did
    // not load, draw_text_str stays null and text tessellates via draw_text.
    if (g_glyph.ok) cb.draw_text_str = tr_text_str;
}

void ChartRenderer::rebuild(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                            const tile57_mariner& m) {
    area_.clear(); line_.clear(); symbol_.clear(); text_.clear(); sprite_.clear(); pattern_.clear(); glyph_.clear();
    // World reference: the view centre, so vertex offsets stay tiny (f32-precise).
    lonlat_to_world(lon, lat, ref_wx_, ref_wy_);
    // Decimate geometry to ~half a pixel at the portrayal zoom.
    decimate_eps_ = 0.5 / (256.0 * std::pow(2.0, zoom));
    size_scale_ = m.size_scale > 0 ? m.size_scale : 1.0;

    tile57_surface_cb cb{};
    fill_surface_cb(cb, this);
    tile57_status rc = tile57_chart_surface(chart_, lon, lat, zoom, w, h, &m, &cb, nullptr);
    if (rc != TILE57_OK) std::fprintf(stderr, "tile57: chart_surface: %s\n", tile57_status_str(rc));
    upload();
}

// ---- tiled path: portray + cache + compose per tile (see chart_renderer.h) -----
// Portray a single (z,x,y) tile into its own VBOs, ONCE, and cache it. Verts are
// stored relative to the tile's NW world corner (ref), so aWorld stays f32-tiny and
// the cached geometry is view-independent — compose places it via a per-tile uOrigin.
ChartRenderer::TileGeom& ChartRenderer::ensure_tile(int z, uint32_t x, uint32_t y, const tile57_mariner& m) {
    uint64_t key = tile_key(z, x, y);
    auto it = tiles_.find(key);
    if (it != tiles_.end()) return it->second;

    TileGeom t{};
    double n = std::pow(2.0, z);
    t.ref_wx = (double)x / n;                 // tile NW corner in web-mercator [0,1]
    t.ref_wy = (double)y / n;

    // Portray this tile through the shared tessellation sinks (reuse the member
    // scratch vectors), referenced to the tile origin, decimated at the tile zoom.
    area_.clear(); line_.clear(); symbol_.clear(); text_.clear(); sprite_.clear(); pattern_.clear(); glyph_.clear();
    ref_wx_ = t.ref_wx; ref_wy_ = t.ref_wy;
    decimate_eps_ = 0.5 / (256.0 * n);
    size_scale_ = m.size_scale > 0 ? m.size_scale : 1.0;
    tile57_surface_cb cb{};
    fill_surface_cb(cb, this);
    tile57_chart_tile_surface(chart_, (uint8_t)z, x, y, &m, &cb, nullptr);

    // Upload each layer to its own VBO (empty layers keep vbo==0, n==0).
    struct LayerSrc { const void* data; size_t bytes; uint32_t count; };
    const LayerSrc src[7] = {
        { area_.data(),    area_.size()*sizeof(Vtx),       (uint32_t)area_.size() },
        { line_.data(),    line_.size()*sizeof(Vtx),       (uint32_t)line_.size() },
        { symbol_.data(),  symbol_.size()*sizeof(Vtx),     (uint32_t)symbol_.size() },
        { text_.data(),    text_.size()*sizeof(Vtx),       (uint32_t)text_.size() },
        { sprite_.data(),  sprite_.size()*sizeof(SpriteVtx),(uint32_t)sprite_.size() },
        { pattern_.data(), pattern_.size()*sizeof(PatVtx), (uint32_t)pattern_.size() },
        { glyph_.data(),   glyph_.size()*sizeof(GlyphVtx), (uint32_t)glyph_.size() },
    };
    for (int i = 0; i < 7; ++i) {
        t.n[i] = src[i].count;
        if (!t.n[i]) continue;
        glGenBuffers(1, &t.vbo[i]);
        glBindBuffer(GL_ARRAY_BUFFER, t.vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, src[i].bytes, src[i].data, GL_STATIC_DRAW);
    }
    return tiles_.emplace(key, t).first->second;
}

void ChartRenderer::clear_tiles() {
    for (auto& kv : tiles_)
        for (int i = 0; i < 7; ++i)
            if (kv.second.vbo[i]) glDeleteBuffers(1, &kv.second.vbo[i]);
    tiles_.clear();
}

// Evict least-recently-drawn tiles (and free their VBOs) until at most `cap` remain.
// Tiles drawn this frame carry the newest used_ms, so they sort last and are kept.
void ChartRenderer::evict_lru(size_t cap) {
    if (tiles_.size() <= cap) return;
    std::vector<std::pair<int64_t, uint64_t>> by_age;   // (used_ms, key)
    by_age.reserve(tiles_.size());
    for (auto& kv : tiles_) by_age.push_back({ kv.second.used_ms, kv.first });
    std::sort(by_age.begin(), by_age.end());             // oldest first
    size_t to_drop = tiles_.size() - cap;
    for (size_t i = 0; i < to_drop && i < by_age.size(); ++i) {
        auto it = tiles_.find(by_age[i].second);
        if (it == tiles_.end()) continue;
        for (int j = 0; j < 7; ++j)
            if (it->second.vbo[j]) glDeleteBuffers(1, &it->second.vbo[j]);
        tiles_.erase(it);
    }
}

// Compose the view from cached tiles: ensure each visible tile is portrayed (once)
// then draw the cached tile VBOs in paint order, each with its own per-tile origin
// uniform. Layer indices: 0 area, 1 line, 2 symbol, 3 text, 4 sprite, 5 pattern,
// 6 glyph. Batched by program (one program bind per layer, inner loop over tiles).
void ChartRenderer::render_tiled(uint32_t w, uint32_t h, const tile57_mariner& m, Pass pass,
                                 float cull_zoom, double vwx, double vwy, double scale_px, int z,
                                 uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) {
    // Budget the per-frame portray: a big first-visit burst (many tiles entering on a
    // zoom-out) is spread over frames instead of freezing one. Uncached tiles beyond
    // the budget are skipped this frame and flagged pending; the plugin re-requests a
    // redraw (tiles_pending()) so they finish — progressive fill, not a hitch. Cached
    // tiles always draw. (unordered_map refs stay valid across inserts/erases of OTHER
    // elements, so the pointers gathered here remain good through eviction + draw.)
    constexpr int kPortrayBudgetPerFrame = 6;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
    tiles_pending_ = false;
    int portrayed = 0;
    std::vector<const TileGeom*> vis;
    vis.reserve((size_t)(x1 - x0 + 1) * (y1 - y0 + 1));
    for (uint32_t ty = y0; ty <= y1; ++ty)
        for (uint32_t tx = x0; tx <= x1; ++tx) {
            uint64_t key = tile_key(z, tx, ty);
            auto it = tiles_.find(key);
            if (it == tiles_.end()) {
                if (portrayed >= kPortrayBudgetPerFrame) { tiles_pending_ = true; continue; }
                ensure_tile(z, tx, ty, m);          // portray + insert (once, ever)
                ++portrayed;
                it = tiles_.find(key);
                if (it == tiles_.end()) continue;   // (defensive; ensure_tile always inserts)
            }
            it->second.used_ms = now;               // LRU: mark drawn this frame
            const TileGeom& t = it->second;
            for (int i = 0; i < 7; ++i) if (t.n[i]) { vis.push_back(&t); break; }
        }
    // Bound cache memory: drop least-recently-drawn tiles (never this frame's — they
    // hold the max used_ms so they sort last and survive; vis pointers stay valid).
    evict_lru(512);
    if (vis.empty()) return;

    const float vp[2] = { (float)w, (float)h };
    auto set_origin = [&](int uloc, const TileGeom* t) {
        float o[2] = { (float)((t->ref_wx - vwx) * scale_px + w * 0.5),
                       (float)((t->ref_wy - vwy) * scale_px + h * 0.5) };
        glUniform2fv(uloc, 1, o);
    };
    auto layer = [&](int prog, int u_scale, int u_vp, int u_zoom, int u_origin, int idx,
                     void (ChartRenderer::*drawfn)(uint32_t, uint32_t), uint32_t tex, int u_atlas) {
        glUseProgram(prog);
        glUniform1f(u_scale, (float)scale_px); glUniform2fv(u_vp, 1, vp); glUniform1f(u_zoom, cull_zoom);
        if (tex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); glUniform1i(u_atlas, 0); }
        for (auto* t : vis) if (t->n[idx]) { set_origin(u_origin, t); (this->*drawfn)(t->vbo[idx], t->n[idx]); }
        if (tex) glBindTexture(GL_TEXTURE_2D, 0);
    };

    if (pass != Pass::kText) {
        layer(prog_, u_scale_, u_vp_, u_zoom_, u_origin_, 0, &ChartRenderer::draw_range, 0, -1);          // area
        if (g_atlas.ok)
            layer(prog_pat_, pu_scale_, pu_vp_, pu_zoom_, pu_origin_, 5, &ChartRenderer::draw_pat_range, g_atlas.tex, pu_atlas_);  // pattern
        layer(prog_, u_scale_, u_vp_, u_zoom_, u_origin_, 1, &ChartRenderer::draw_range, 0, -1);          // line
        layer(prog_, u_scale_, u_vp_, u_zoom_, u_origin_, 2, &ChartRenderer::draw_range, 0, -1);          // symbol (tessellated)
        if (g_atlas.ok)
            layer(prog_sprite_, su_scale_, su_vp_, su_zoom_, su_origin_, 4, &ChartRenderer::draw_sprite_range, g_atlas.tex, su_atlas_);  // sprite
    }
    if (pass != Pass::kBase) {
        layer(prog_, u_scale_, u_vp_, u_zoom_, u_origin_, 3, &ChartRenderer::draw_range, 0, -1);          // text (tessellated fallback)
        if (g_glyph.ok)
            layer(prog_glyph_, gu_scale_, gu_vp_, gu_zoom_, gu_origin_, 6, &ChartRenderer::draw_glyph_range, g_glyph.tex, gu_atlas_);   // glyph (SDF)
    }
    glUseProgram(0);
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
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pat_);
    glBufferData(GL_ARRAY_BUFFER, pattern_.size() * sizeof(PatVtx), pattern_.data(), GL_DYNAMIC_DRAW);
    n_pat_ = (uint32_t)pattern_.size();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_glyph_);
    glBufferData(GL_ARRAY_BUFFER, glyph_.size() * sizeof(GlyphVtx), glyph_.data(), GL_DYNAMIC_DRAW);
    n_glyph_ = (uint32_t)glyph_.size();
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

// Draw one PatVtx buffer (prog_pat_ must be bound + uniforms/atlas set by caller).
void ChartRenderer::draw_pat_range(uint32_t vbo, uint32_t count) {
    if (!count) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, wx));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, u0));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, tw));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, thresh));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2); glDisableVertexAttribArray(3);
}

// Draw one SpriteVtx buffer (prog_sprite_ bound + uniforms/atlas set by caller).
void ChartRenderer::draw_sprite_range(uint32_t vbo, uint32_t count) {
    if (!count) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx), (void*)offsetof(SpriteVtx, wx));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx), (void*)offsetof(SpriteVtx, px));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx), (void*)offsetof(SpriteVtx, u));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx), (void*)offsetof(SpriteVtx, thresh));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2); glDisableVertexAttribArray(3);
}

// Draw one GlyphVtx buffer (prog_glyph_ bound + uniforms/atlas set by caller).
void ChartRenderer::draw_glyph_range(uint32_t vbo, uint32_t count) {
    if (!count) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, wx));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, px));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, u));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, r));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, thresh));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3); glDisableVertexAttribArray(4);
}

void ChartRenderer::render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                           const tile57_mariner& m, Pass pass, bool stencil_clip,
                           double device_scale, double cull_bias) {
    if (!chart_ || !ensure_gl()) return;
    if (!have_range_) {
        tile57_info info{};
        tile57_chart_get_info(chart_, &info);
        min_zoom_ = info.min_zoom; max_zoom_ = info.max_zoom; have_range_ = true;
        have_bounds_ = info.has_bounds;
        b_w_ = info.west; b_s_ = info.south; b_e_ = info.east; b_n_ = info.north;
    }
    // Tiled path: portray+cache+compose per tile (MapLibre model). Invalidate the
    // tile cache when the mariner settings change (they change the portrayal).
    if (tiled_) {
        uint64_t mhx = mariner_hash(m);
        if (mhx != tiles_mhash_) { clear_tiles(); tiles_mhash_ = mhx; }
    }
    // Portray at the view zoom clamped to the baked band (tile selection); the
    // GPU then transforms to ANY zoom, so pan and zoom are re-portray-free.
    double cz = zoom;
    if (cz < min_zoom_) cz = min_zoom_;
    if (cz > max_zoom_) cz = max_zoom_;

    // Re-portray on zoom only once the gesture SETTLES (detected via `moving`
    // below): while the zoom changes we transform the cached geometry (smooth,
    // like a native chart) and refresh to crisp detail when it STOPS. A large
    // excursion (>3 levels) refreshes even mid-gesture to bound extreme scaling.
    bool zoom_stale = std::fabs(cz - cam_zoom_) > 0.3;

    uint64_t mh = mariner_hash(m);
    // Is the view in active motion (pan or zoom) this frame? While it is we do NOT
    // re-portray on the render thread — we transform the CACHED geometry on the GPU
    // (smooth) and re-portray once the gesture settles. Re-portraying mid-pan was
    // the killer: it re-tessellated every quilt chart every frame (~tens of ms
    // each), and being CPU-bound it janks on ANY GPU. The resulting low framerate
    // makes each frame's pan step larger, tripping the margin again — a
    // self-sustaining low-fps spiral. (Zoom already deferred to settle via
    // zoom_stale/zoom_settled; this extends the same rule to pan.)
    bool moving = (std::fabs(lon - last_lon_) > 1e-9 || std::fabs(lat - last_lat_) > 1e-9
                   || std::fabs(zoom - last_zoom_r_) > 1e-4);
    last_lon_ = lon; last_lat_ = lat; last_zoom_r_ = zoom;
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count();
    // Pan margin: overscan so a settled view — and moderate pans before the settle
    // re-portray lands — is covered without blanking at the edges. Bigger = fewer
    // re-portrays / less transient blank, but more geometry drawn per frame and a
    // costlier settle rebuild. TILE57_OVERSCAN overrides the default.
    static const double overscan = [] {
        const char* e = std::getenv("TILE57_OVERSCAN");
        double v = e ? std::atof(e) : 0.0;
        return v >= 1.0 ? v : 1.5;
    }();
    auto cam_dim = [&](uint32_t v) { return (uint32_t)std::min(std::ceil(v * overscan), 4096.0); };
    // Which trigger fired (for TILE57_PROF): init = first portray of a chart (a
    // freshly-visible quilt cell, e.g. many at once on zoom-out — these CANNOT be
    // deferred, there is no cached geometry to transform); grow = viewport bigger
    // than the cache; zoom = a settled/large zoom step; pan = view left the window.
    const char* reason = !have_cam_ ? "init"
                       : (mh != cam_mhash_) ? "mariner"
                       : (cam_w_ < cam_dim(w) / 2 || cam_h_ < cam_dim(h) / 2) ? "grow"
                       : (zoom_stale && std::fabs(cz - cam_zoom_) > 3.0) ? "zoom"  // extreme-scale safety
                       : nullptr;
    bool need = reason != nullptr;
    if (!need) {
        // Deferred re-portray triggers — kept OFF during a gesture (we transform the
        // cached geometry, smooth) and fired on SETTLE:
        //  - pan: view centre left the portrayed window — fire on settle, or throttled
        //    during continuous motion (auto-follow keeps up without re-tessellating
        //    every frame);
        //  - zoom: the portray zoom drifted >0.3 so tile detail no longer matches —
        //    fire on settle only (a zoom gesture always ends; the >3 excursion above
        //    is the mid-gesture safety). Gating on !moving (ANY motion this frame)
        //    instead of a frame-delta "settled" proxy is what stops a slow / high-fps
        //    zoom from re-portraying the whole quilt mid-gesture.
        constexpr int64_t kReportrayThrottleMs = 200;
        double cwx, cwy, vwx, vwy;
        lonlat_to_world(cam_lon_, cam_lat_, cwx, cwy);
        lonlat_to_world(lon, lat, vwx, vwy);
        double scale_px = 256.0 * std::pow(2.0, cam_zoom_) * device_scale;
        double dx = std::fabs(vwx - cwx) * scale_px, dy = std::fabs(vwy - cwy) * scale_px;
        bool out = (dx + w * 0.5 > cam_w_ * 0.5 || dy + h * 0.5 > cam_h_ * 0.5);
        if (out && (!moving || now_ms - last_portray_ms_ >= kReportrayThrottleMs)) {
            need = true; reason = "pan";
        } else if (zoom_stale && !moving) {
            need = true; reason = "zoom";
        }
    }
    // The text pass never re-portrays (it trails the base pass on OpenCPN's
    // compose; regenerating would desync the shared buffers).
    if (pass == Pass::kText && have_cam_) { need = false; reason = nullptr; }

    // TILE57_PROF: log every re-portray (the synchronous CPU tessellation on the
    // render thread) with its cost and how many frames since the last one — so the
    // rebuild FREQUENCY and per-rebuild ms are both visible in opencpn.log. This is
    // the GPU-independent cost that makes pan/zoom janky even on a fast GPU.
    static const bool prof = std::getenv("TILE57_PROF") != nullptr;
    static long prof_frames = 0, prof_rebuilds = 0, prof_last_frame = 0;
    if (prof && pass != Pass::kText) ++prof_frames;
    if (need && !tiled_) {
        cam_lon_ = lon; cam_lat_ = lat; cam_zoom_ = cz;
        cam_w_ = cam_dim(w); cam_h_ = cam_dim(h); cam_mhash_ = mh;
        last_portray_ms_ = now_ms;
        if (prof) {
            auto t0 = std::chrono::steady_clock::now();
            rebuild(lon, lat, cz, cam_w_, cam_h_, m);
            double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
            wxLogMessage("tile57 PROF rebuild #%ld  %-7s  %.2f ms  (+%ld frames)  z=%.2f  "
                         "win=%ux%u  verts a=%u l=%u sym=%u spr=%u pat=%u gly=%u",
                         ++prof_rebuilds, reason ? reason : "?", ms,
                         prof_frames - prof_last_frame, cz, cam_w_, cam_h_,
                         n_area_, n_line_, n_symbol_, n_sprite_, n_pat_, n_glyph_);
            prof_last_frame = prof_frames;
        } else {
            rebuild(lon, lat, cz, cam_w_, cam_h_, m);
        }
        have_cam_ = true;
    }

    // Per-frame transform: screen = aWorld*uScale + uOrigin + aPost.
    double vwx, vwy;
    lonlat_to_world(lon, lat, vwx, vwy);
    // Projection at the PHYSICAL framebuffer: geographic zoom scaled by device_scale
    // (so e.g. a 2x HiDPI framebuffer gets 2x px per world unit, while zoom — and thus
    // the SCAMIN cull below — stays geographic).
    double scale_px = 256.0 * std::pow(2.0, zoom) * device_scale;   // px per world[0,1]
    double ox = (ref_wx_ - vwx) * scale_px + w * 0.5;
    double oy = (ref_wy_ - vwy) * scale_px + h * 0.5;
    // SCAMIN cull zoom: symbols/text are enlarged (content scale on HiDPI), so pull the
    // cull zoom DOWN by cull_bias — bigger symbols drop out that many mercator levels
    // earlier, keeping decluttering while zoomed out. Driven by the SAME content-scale
    // that enlarges the symbols (NOT the projection's device_scale, which is 1.0 when
    // OpenCPN hands a physical-px ViewPort — decoupling the cull from the enlargement).
    float cull_zoom = (float)(zoom - cull_bias);

    (void)stencil_clip;
    // Render the scene into the SS FBO (at kSS× res), then composite it back
    // down-sampled — antialiasing tessellated text/lines/area edges. Clip to the
    // chart's quilt patch via OpenCPN's SCISSOR (SetClipRect) rather than a
    // stencil EQUAL test (unreliable across GL backends).
    // Antialias (offscreen supersample) only when the view is SETTLED (see the
    // `moving` computation above). During pan/zoom motion render DIRECT into
    // OpenCPN's cache so accelerated panning's partial strip updates align
    // (offscreen compositing tore); AA pops in when motion stops. Text (a light,
    // separate pass) always supersamples.
    // TILE57_NOSS forces DIRECT rendering (no offscreen supersample FBO). Needed when
    // OpenCPN itself reports "Framebuffer Objects unavailable" and falls back to stencil
    // clipping (some NVIDIA/driver combos): our offscreen FBO + composite doesn't align
    // with that no-FBO path and renders black. Direct render (as during pan) works.
    static const bool noss = std::getenv("TILE57_NOSS") != nullptr;
    const int kSS = ss_factor();   // 1 on software GL (AA off); 2 on real GPUs
    bool ss = !noss && kSS > 1 && ensure_ss((int)w, (int)h) && !(moving && pass != Pass::kText);
    // Saving GL state (glGetIntegerv/glIsEnabled) forces a driver sync — a GPU
    // pipeline stall. It is only needed to RESTORE the FBO/viewport/scissor after
    // an offscreen pass, so read it back only when supersampling; the direct
    // motion path (the interactive hot path) skips all four reads.
    GLint prev_fbo = 0, prev_vp[4] = {0,0,0,0}, prev_sc[4] = {0,0,0,0};
    GLboolean sc_on = GL_FALSE;
    if (ss) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glGetIntegerv(GL_VIEWPORT, prev_vp);
        glGetIntegerv(GL_SCISSOR_BOX, prev_sc);
        sc_on = glIsEnabled(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_ss.fbo);
        glViewport(0, 0, (GLsizei)(w * kSS), (GLsizei)(h * kSS));
        if (sc_on) glScissor(prev_sc[0]*kSS, prev_sc[1]*kSS, prev_sc[2]*kSS, prev_sc[3]*kSS);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glUseProgram(prog_);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glUniform1f(u_scale_, (float)scale_px);
    float origin[2] = { (float)ox, (float)oy };
    glUniform2fv(u_origin_, 1, origin);
    float vp[2] = { (float)w, (float)h };
    glUniform2fv(u_vp_, 1, vp);
    glUniform1f(u_zoom_, cull_zoom);

    if (tiled_) {
        // Pick the tile zoom (the baked band the view falls in) and the visible tile
        // x/y range, clamped to the chart's coverage so a zoomed-out view doesn't
        // enumerate a sea of empty tiles. render_tiled portrays any missing tile ONCE
        // and composes the cached tiles.
        int z = (int)std::lround(zoom);
        if (z < (int)min_zoom_) z = (int)min_zoom_;
        if (z > (int)max_zoom_) z = (int)max_zoom_;
        double n = std::pow(2.0, z);
        double half_wx = (w * 0.5) / scale_px, half_wy = (h * 0.5) / scale_px;
        double wminx = vwx - half_wx, wmaxx = vwx + half_wx;
        double wminy = vwy - half_wy, wmaxy = vwy + half_wy;
        if (have_bounds_) {
            double nwx, nwy, sex, sey;
            lonlat_to_world(b_w_, b_n_, nwx, nwy);   // NW -> (min x, min y)
            lonlat_to_world(b_e_, b_s_, sex, sey);   // SE -> (max x, max y)
            wminx = std::max(wminx, nwx); wmaxx = std::min(wmaxx, sex);
            wminy = std::max(wminy, nwy); wmaxy = std::min(wmaxy, sey);
        }
        double maxt = n - 1.0;
        long x0 = (long)std::floor(std::clamp(wminx * n, 0.0, maxt));
        long x1 = (long)std::floor(std::clamp(wmaxx * n, 0.0, maxt));
        long y0 = (long)std::floor(std::clamp(wminy * n, 0.0, maxt));
        long y1 = (long)std::floor(std::clamp(wmaxy * n, 0.0, maxt));
        if (wmaxx >= wminx && wmaxy >= wminy && (x1 - x0 + 1) * (y1 - y0 + 1) <= 4096)
            render_tiled(w, h, m, pass, cull_zoom, vwx, vwy, scale_px, z,
                         (uint32_t)x0, (uint32_t)x1, (uint32_t)y0, (uint32_t)y1);
    }

    if (!tiled_ && pass != Pass::kText) {
        draw_range(vbo_area_, n_area_);
        if (n_pat_ && g_atlas.ok) {
            glUseProgram(prog_pat_);
            glUniform1f(pu_scale_, (float)scale_px);
            glUniform2fv(pu_origin_, 1, origin);
            glUniform2fv(pu_vp_, 1, vp);
            glUniform1f(pu_zoom_, cull_zoom);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_atlas.tex);
            glUniform1i(pu_atlas_, 0);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_pat_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, wx));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, u0));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, tw));
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, thresh));
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)n_pat_);
            glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(2); glDisableVertexAttribArray(3);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(prog_);
        }
        draw_range(vbo_line_, n_line_);
        draw_range(vbo_symbol_, n_symbol_);
        if (n_sprite_ && g_atlas.ok) {
            glUseProgram(prog_sprite_);
            glUniform1f(su_scale_, (float)scale_px);
            glUniform2fv(su_origin_, 1, origin);
            glUniform2fv(su_vp_, 1, vp);
            glUniform1f(su_zoom_, cull_zoom);
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
    if (!tiled_ && pass != Pass::kBase) {
        draw_range(vbo_text_, n_text_);   // tessellated fallback (empty when SDF text is active)
        if (n_glyph_ && g_glyph.ok) {
            glUseProgram(prog_glyph_);
            glUniform1f(gu_scale_, (float)scale_px);
            glUniform2fv(gu_origin_, 1, origin);
            glUniform2fv(gu_vp_, 1, vp);
            glUniform1f(gu_zoom_, cull_zoom);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_glyph.tex);
            glUniform1i(gu_atlas_, 0);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_glyph_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, wx));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, px));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, u));
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, r));
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, thresh));
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)n_glyph_);
            glDisableVertexAttribArray(0); glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
            glDisableVertexAttribArray(3); glDisableVertexAttribArray(4);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(prog_);
        }
    }

    if (ss) {
        // Composite the supersampled texture over OpenCPN's FBO, down-sampled by
        // the sampler's linear filter, clipped to the same patch scissor.
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
        if (sc_on) glScissor(prev_sc[0], prev_sc[1], prev_sc[2], prev_sc[3]);
        composite_ss();
    }
    glUseProgram(0);
    static const bool dbg = std::getenv("TILE57_DEBUG") != nullptr;
    if (dbg) {
        static int cnt = 0;
        if (cnt++ < 10) {
            GLenum e = glGetError();
            wxLogMessage("tile57 GL: pass=%d ss=%d n_area=%u n_line=%u n_sym=%u prev_fbo=%d "
                         "vp=[%d,%d,%d,%d] scissor_on=%d sc=[%d,%d,%d,%d] glerr=0x%x",
                         (int)pass, ss ? 1 : 0, n_area_, n_line_, n_symbol_, prev_fbo,
                         prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3], (int)sc_on,
                         prev_sc[0], prev_sc[1], prev_sc[2], prev_sc[3], (unsigned)e);
        }
    }
}

void ChartRenderer::shutdown() {
    gl_ready_ = false; prog_ = prog_sprite_ = prog_pat_ = prog_glyph_ = prog_blit_ = 0;
    vbo_area_ = vbo_line_ = vbo_symbol_ = vbo_text_ = vbo_sprite_ = vbo_pat_ = vbo_glyph_ = vbo_quad_ = 0;
    n_area_ = n_line_ = n_symbol_ = n_text_ = n_sprite_ = n_pat_ = n_glyph_ = 0;
    have_cam_ = false; have_range_ = false;
    tiles_.clear(); tiles_mhash_ = 0;   // GL buffers die with the context on shutdown
    if (chart_) { tile57_chart_close(chart_); chart_ = nullptr; }
}

} // namespace t57
