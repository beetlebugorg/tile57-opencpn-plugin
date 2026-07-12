// chart_renderer.cpp — see chart_renderer.h.
#include "chart_renderer.h"
#include "earcut.hpp"
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
#include <wx/image.h>
#include <wx/log.h> // reliable capture in opencpn.log (stderr isn't captured here)
#include <wx/mstream.h>

namespace t57 {

namespace {
constexpr double kPi = 3.14159265358979323846;
// A feature with no SCAMIN is never scale-culled: give it an effectively infinite one, so
// the "display denominator > SCAMIN" test below can stay branchless.
constexpr float kNeverCulled = 1e30f;
// SUPER-SCAMIN (see ChartRenderer::feature_scamin): how far past its own compilation scale a
// cell's SCAMIN-LESS detail may still draw. OpenCPN uses 2x (s52plib.cpp: SuperScamin =
// chart_ref_scale * 2), so a 1:12,000 cell's ungated symbology dies at 1:24,000.
constexpr double kSuperScaminUnderzoom = 2.0;

void lonlat_to_world(double lon, double lat, double& wx, double& wy) {
    wx = (lon + 180.0) / 360.0;
    double s = std::sin(lat * kPi / 180.0);
    wy = 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * kPi);
}
// SCAMIN travels to the GPU AS ITSELF — the raw 1:N denominator — and is compared against
// the host's actual display denominator (see render's scamin_denom / the uScaminDenom
// uniform). That is exactly what native OpenCPN does:
//
//     if (vp_plib.chart_scale > rzRules->obj->Scamin) b_visible = false;   // s52plib.cpp
//
// This used to convert SCAMIN into a web-mercator zoom threshold, log2(DENOM_Z0 / scamin),
// and compare zooms. That is MapLibre's convention and it bakes in the EQUATOR: the scale
// denominator at a given zoom shrinks with cos(latitude), so the formula drifted from the
// real display scale by log2(cos lat) — ~0.36 of a zoom level at latitude 39, i.e. features
// popping in and out ~28% of a scale step away from where the native charts do it.
// The host knows its own scale exactly; there is nothing to model.
float scamin_denom(int64_t scamin) { return scamin > 0 ? (float)scamin : kNeverCulled; }
// TILE57_DEBUG: name the mariner fields that CHANGED — a cache clear costs a full re-portray
// of every visible tile, so a hash that flaps every frame is a performance cliff, and the
// hash alone ("a -> b") cannot say which setting moved. Compares the portrayal-relevant
// fields by name; anything not listed here cannot invalidate the cache anyway.
std::string mariner_diff(const tile57_mariner& a, const tile57_mariner& b) {
    std::string d;
    auto num = [&d](const char* n, double x, double y) {
        if (x != y)
            d += std::string(d.empty() ? "" : " ") + n + "=" + std::to_string(x) + "->" +
                 std::to_string(y);
    };
    auto flag = [&d](const char* n, bool x, bool y) {
        if (x != y)
            d += std::string(d.empty() ? "" : " ") + n + "=" + (x ? "1" : "0") + "->" +
                 (y ? "1" : "0");
    };
    num("safety_contour", a.safety_contour, b.safety_contour);
    num("shallow_contour", a.shallow_contour, b.shallow_contour);
    num("deep_contour", a.deep_contour, b.deep_contour);
    num("safety_depth", a.safety_depth, b.safety_depth);
    num("size_scale", a.size_scale, b.size_scale);
    num("text_size_scale", a.text_size_scale, b.text_size_scale);
    num("sounding_size_scale", a.sounding_size_scale, b.sounding_size_scale);
    flag("four_shade_water", a.four_shade_water, b.four_shade_water);
    flag("display_base", a.display_base, b.display_base);
    flag("display_standard", a.display_standard, b.display_standard);
    flag("display_other", a.display_other, b.display_other);
    flag("data_quality", a.data_quality, b.data_quality);
    flag("show_inform_callouts", a.show_inform_callouts, b.show_inform_callouts);
    flag("show_meta_bounds", a.show_meta_bounds, b.show_meta_bounds);
    flag("simplified_points", a.simplified_points, b.simplified_points);
    flag("show_full_sector_lines", a.show_full_sector_lines, b.show_full_sector_lines);
    flag("text_names", a.text_names, b.text_names);
    flag("text_other", a.text_other, b.text_other);
    flag("show_light_descriptions", a.show_light_descriptions, b.show_light_descriptions);
    flag("ignore_scamin", a.ignore_scamin, b.ignore_scamin);
    flag("show_overscale", a.show_overscale, b.show_overscale);
    flag("date_dependent", a.date_dependent, b.date_dependent);
    if (a.viewing_groups_off_len != b.viewing_groups_off_len)
        d += " viewing_groups_len=" + std::to_string(a.viewing_groups_off_len) + "->" +
             std::to_string(b.viewing_groups_off_len);
    if (a.viewing_groups_off != b.viewing_groups_off)
        d += " viewing_groups_PTR(moved)"; // the vector reallocated: hashed, but not a change
    if (d.empty())
        d = "(no named field changed — PADDING or an unlisted field)";
    return d;
}

uint64_t mariner_hash(const tile57_mariner& m) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    mix(&m, sizeof(m));
    // viewing_groups_off is a POINTER in the struct, so the bytes above hash the address,
    // not the groups. Hash what it points AT as well — otherwise swapping one denied
    // group for another (same buffer, same length) leaves the hash unchanged and the tile
    // cache would keep serving geometry portrayed under the old settings.
    if (m.viewing_groups_off && m.viewing_groups_off_len)
        mix(m.viewing_groups_off, m.viewing_groups_off_len * sizeof(int32_t));
    return h;
}
} // namespace

// ---- tessellation ----------------------------------------------------------
// World area fill: earcut the rings (outer + holes) directly in world space.
// Rings are decimated to the portrayal's pixel grid first — dropping sub-half-
// pixel detail cuts the triangle count hugely on dense coastlines/depth areas
// (the per-frame draw cost, hence the zoom framerate) with no visible change.
void ChartRenderer::on_fill_area(const tile57_world_rings* p, tile57_rgba c, float scamin) {
    if (!p || p->n < 3)
        return;
    using Pt = std::array<double, 2>;
    const double eps2 = decimate_eps_ * decimate_eps_;
    std::vector<std::vector<Pt>> poly;
    for (uint32_t r = 0; r < p->ring_count; ++r) {
        uint32_t s = p->ring_starts[r];
        uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
        if (e <= s + 2)
            continue;
        std::vector<Pt> ring{{p->pts[s].x, p->pts[s].y}};
        for (uint32_t i = s + 1; i < e; ++i) {
            double dx = p->pts[i].x - ring.back()[0], dy = p->pts[i].y - ring.back()[1];
            if (i == e - 1 || dx * dx + dy * dy >= eps2)
                ring.push_back({p->pts[i].x, p->pts[i].y});
        }
        if (ring.size() >= 3)
            poly.push_back(std::move(ring));
    }
    if (poly.empty())
        return;
    std::vector<Pt> flat;
    for (const auto& ring : poly)
        flat.insert(flat.end(), ring.begin(), ring.end());
    std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            double wx = flat[idx[i + k]][0] - ref_wx_, wy = flat[idx[i + k]][1] - ref_wy_;
            area_.push_back({(float)wx, (float)wy, 0, 0, c.r, c.g, c.b, c.a, scamin, 0.0f});
        }
}

// World line: expand each segment to a screen-px-wide quad — aWorld is the
// endpoint (world), aPost carries the perpendicular * half-width in px, applied
// AFTER the world transform so the line is a constant screen width at any zoom.
void ChartRenderer::on_stroke_line(const tile57_world_rings* p, float width_px, tile57_rgba c,
                                   float scamin) {
    if (!p || p->n < 2)
        return;
    float hw = std::max(0.5f, width_px * 0.5f);
    const double eps2 = decimate_eps_ * decimate_eps_;
    for (uint32_t r = 0; r < p->ring_count; ++r) {
        uint32_t s = p->ring_starts[r];
        uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
        // Decimate to the pixel grid before expanding (see on_fill_area).
        std::vector<std::array<double, 2>> kept{{p->pts[s].x, p->pts[s].y}};
        for (uint32_t i = s + 1; i < e; ++i) {
            double dx = p->pts[i].x - kept.back()[0], dy = p->pts[i].y - kept.back()[1];
            if (i == e - 1 || dx * dx + dy * dy >= eps2)
                kept.push_back({p->pts[i].x, p->pts[i].y});
        }
        for (size_t i = 0; i + 1 < kept.size(); ++i) {
            double ax = kept[i][0] - ref_wx_, ay = kept[i][1] - ref_wy_;
            double bx = kept[i + 1][0] - ref_wx_, by = kept[i + 1][1] - ref_wy_;
            double dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-12)
                continue;
            float nx = (float)(-dy / len) * hw, ny = (float)(dx / len) * hw; // px offset
            auto v = [&](double wx, double wy, float ox, float oy) {
                line_.push_back({(float)wx, (float)wy, ox, oy, c.r, c.g, c.b, c.a, scamin, 1.0f});
            };
            v(ax, ay, nx, ny);
            v(bx, by, nx, ny);
            v(bx, by, -nx, -ny);
            v(ax, ay, nx, ny);
            v(bx, by, -nx, -ny);
            v(ax, ay, -nx, -ny);
        }
    }
}

// Anchored fill (symbol / text): tessellate the local (px) rings with an
// even-odd rule (group rings by containment parity so counters cut out and no
// triangles bridge between letters), anchored at one world point via aWorld.
static void tess_local_even_odd(const tile57_local_rings* p, double awx, double awy, tile57_rgba c,
                                float scamin, float postrot, std::vector<ChartRenderer::Vtx>& out) {
    if (!p || p->n < 3)
        return;
    using Pt = std::array<float, 2>;
    std::vector<std::vector<Pt>> rings;
    for (uint32_t r = 0; r < p->ring_count; ++r) {
        uint32_t s = p->ring_starts[r];
        uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
        if (e <= s + 2)
            continue;
        std::vector<Pt> ring;
        for (uint32_t i = s; i < e; ++i)
            ring.push_back({p->pts[i].x, p->pts[i].y});
        rings.push_back(std::move(ring));
    }
    const size_t n = rings.size();
    if (n == 0)
        return;
    auto in_ring = [](float x, float y, const std::vector<Pt>& ring) {
        bool in = false;
        for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
            if (((ring[i][1] > y) != (ring[j][1] > y)) &&
                (x < (ring[j][0] - ring[i][0]) * (y - ring[i][1]) / (ring[j][1] - ring[i][1]) +
                         ring[i][0]))
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
            if (i == j)
                continue;
            if (in_ring(rings[i][0][0], rings[i][0][1], rings[j])) {
                ++depth[i];
                if (parent[i] < 0 || area[j] < area[parent[i]])
                    parent[i] = (int)j;
            }
        }
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2)
            continue;
        std::vector<std::vector<Pt>> poly;
        std::vector<Pt> flat;
        poly.push_back(rings[i]);
        flat.insert(flat.end(), rings[i].begin(), rings[i].end());
        for (size_t j = 0; j < n; ++j) {
            if (parent[j] != (int)i || depth[j] % 2 == 0)
                continue;
            poly.push_back(rings[j]);
            flat.insert(flat.end(), rings[j].begin(), rings[j].end());
        }
        std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
            for (int k = 0; k < 3; ++k)
                out.push_back({(float)awx, (float)awy, flat[idx[t + k]][0], flat[idx[t + k]][1],
                               c.r, c.g, c.b, c.a, scamin});
    }
}

void ChartRenderer::on_draw_symbol(tile57_world_point anchor, const tile57_local_rings* p,
                                   tile57_rgba c, int even_odd, float stroke_w,
                                   tile57_rot_align align, float scamin) {
    (void)even_odd;
    double awx = anchor.x - ref_wx_, awy = anchor.y - ref_wy_;
    // The outline arrives already turned to the symbol's own angle; MAP means that angle is
    // chart-relative, so it must ALSO turn with the view (an ORIENT'd light, a linestyle brick).
    const float postrot = (align == TILE57_ALIGN_MAP) ? 1.0f : 0.0f;
    if (stroke_w > 0) {
        // Symbol stroke: expand local polylines to px quads (aPost in local px).
        float hw = std::max(0.5f, stroke_w * 0.5f);
        for (uint32_t r = 0; r < p->ring_count; ++r) {
            uint32_t s = p->ring_starts[r];
            uint32_t e = (r + 1 < p->ring_count) ? p->ring_starts[r + 1] : p->n;
            for (uint32_t i = s; i + 1 < e; ++i) {
                float ax = p->pts[i].x, ay = p->pts[i].y, bx = p->pts[i + 1].x,
                      by = p->pts[i + 1].y;
                float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-4f)
                    continue;
                float nx = -dy / len * hw, ny = dx / len * hw;
                auto v = [&](float ox, float oy) {
                    symbol_.push_back(
                        {(float)awx, (float)awy, ox, oy, c.r, c.g, c.b, c.a, scamin, postrot});
                };
                v(ax + nx, ay + ny);
                v(bx + nx, by + ny);
                v(bx - nx, by - ny);
                v(ax + nx, ay + ny);
                v(bx - nx, by - ny);
                v(ax - nx, ay - ny);
            }
        }
    } else {
        tess_local_even_odd(p, awx, awy, c, scamin, postrot, symbol_);
    }
}

void ChartRenderer::on_draw_text(tile57_world_point anchor, const tile57_local_rings* g,
                                 tile57_rgba c, tile57_rgba /*halo*/, tile57_rot_align align,
                                 float scamin) {
    tess_local_even_odd(g, anchor.x - ref_wx_, anchor.y - ref_wy_, c, scamin,
                        (align == TILE57_ALIGN_MAP) ? 1.0f : 0.0f, text_);
}

// ---- sprite atlas (shared; loaded once in a GL context) --------------------
namespace {
// Normalised UV rect + the cell's logical px size (atlas px / pixelRatio), used
// as the pattern tile's screen size (× size_scale).
struct AtlasRect {
    float u0, v0, u1, v1, px_w, px_h;
};
struct SpriteAtlas {
    uint32_t tex = 0;
    std::unordered_map<std::string, AtlasRect> uv; // name -> rect
    bool tried = false, ok = false;
};
SpriteAtlas g_atlas;

double json_num(const char* s, size_t lo, size_t hi, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    for (size_t p = lo; p + k.size() < hi; ++p)
        if (std::memcmp(s + p, k.data(), k.size()) == 0) {
            size_t q = p + k.size();
            while (q < hi && (s[q] == ' ' || s[q] == ':'))
                ++q;
            return std::atof(s + q);
        }
    return 0;
}

// Parse a MapLibre sprite JSON ({"name":{"x":,"y":,"width":,"height":},..}) into
// name -> normalised UV rect. The pivot-centred entry is the bare name.
void parse_sprite_json(const char* s, size_t len, int tw, int th,
                       std::unordered_map<std::string, AtlasRect>& out) {
    if (tw <= 0 || th <= 0 || !s)
        return;
    size_t i = 0;
    auto ws = [&] {
        while (i < len && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r'))
            ++i;
    };
    ws();
    if (i >= len || s[i] != '{')
        return;
    ++i;
    while (i < len) {
        ws();
        if (i < len && s[i] == '}')
            break;
        if (i >= len || s[i] != '"')
            break;
        ++i;
        size_t ns = i;
        while (i < len && s[i] != '"')
            ++i;
        std::string name(s + ns, i - ns);
        if (i < len)
            ++i;
        ws();
        if (i < len && s[i] == ':')
            ++i;
        ws();
        if (i >= len || s[i] != '{')
            break;
        int depth = 0;
        size_t os = i;
        while (i < len) {
            char ch = s[i];
            if (ch == '{')
                ++depth;
            else if (ch == '}') {
                if (--depth == 0) {
                    ++i;
                    break;
                }
            }
            ++i;
        }
        double x = json_num(s, os, i, "x"), y = json_num(s, os, i, "y");
        double w = json_num(s, os, i, "width"), h = json_num(s, os, i, "height");
        double ratio = json_num(s, os, i, "pixelRatio");
        if (ratio <= 0)
            ratio = 1;
        if (w > 0 && h > 0)
            out[name] = {(float)(x / tw),       (float)(y / th),    (float)((x + w) / tw),
                         (float)((y + h) / th), (float)(w / ratio), (float)(h / ratio)};
        ws();
        if (i < len && s[i] == ',')
            ++i;
    }
}

void load_sprite_atlas() {
    if (g_atlas.tried)
        return;
    g_atlas.tried = true;
    tile57_assets assets{};
    // sprite-mln: pivot-centred cells + {name:{x,y,w,h}} JSON. NULL => embedded.
    if (tile57_bake_sprite_mln(nullptr, &assets, nullptr) != TILE57_OK)
        return;
    if (assets.sprite_png && assets.sprite_png_len) {
        wxMemoryInputStream mis(assets.sprite_png, assets.sprite_png_len);
        wxImage img;
        if (img.LoadFile(mis, wxBITMAP_TYPE_PNG)) {
            int tw = img.GetWidth(), th = img.GetHeight();
            std::vector<uint8_t> rgba((size_t)tw * th * 4);
            const uint8_t* rgb = img.GetData();
            const uint8_t* al = img.HasAlpha() ? img.GetAlpha() : nullptr;
            for (int p = 0; p < tw * th; ++p) {
                rgba[p * 4 + 0] = rgb[p * 3 + 0];
                rgba[p * 4 + 1] = rgb[p * 3 + 1];
                rgba[p * 4 + 2] = rgb[p * 3 + 2];
                rgba[p * 4 + 3] = al ? al[p] : 255;
            }
            glGenTextures(1, &g_atlas.tex);
            glBindTexture(GL_TEXTURE_2D, g_atlas.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         rgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            parse_sprite_json((const char*)assets.sprite_json, assets.sprite_json_len, tw, th,
                              g_atlas.uv);
            g_atlas.ok = g_atlas.tex != 0 && !g_atlas.uv.empty();
        }
    }
    tile57_assets_free(&assets);
}

// ---- SDF glyph atlas (shared; loaded once in a GL context) -----------------
struct GlyphInfo {
    float u0, v0, u1, v1, ox, oy, w, h, adv;
}; // UV + EM-space quad
struct GlyphAtlas {
    uint32_t tex = 0;
    std::unordered_map<uint32_t, GlyphInfo> g; // codepoint -> metrics
    bool tried = false, ok = false;
};
GlyphAtlas g_glyph;

// Parse {"em_px":..,"pad":..,"glyphs":{"cp":[u0,v0,u1,v1,ox,oy,w,h,adv],..}}.
void parse_glyph_json(const char* s, size_t len, std::unordered_map<uint32_t, GlyphInfo>& out) {
    if (!s || len < 8)
        return;
    size_t i = 0;
    for (; i + 8 < len; ++i)
        if (std::memcmp(s + i, "\"glyphs\"", 8) == 0)
            break;
    i += 8;
    while (i < len && s[i] != '{')
        ++i;
    if (i >= len)
        return;
    ++i;
    auto ws = [&] {
        while (i < len &&
               (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r' || s[i] == ','))
            ++i;
    };
    while (i < len) {
        ws();
        if (i < len && s[i] == '}')
            break;
        if (i >= len || s[i] != '"')
            break;
        ++i;
        uint32_t cp = (uint32_t)std::atoi(s + i);
        while (i < len && s[i] != '"')
            ++i;
        if (i < len)
            ++i;
        ws();
        if (i < len && s[i] == ':')
            ++i;
        ws();
        if (i >= len || s[i] != '[')
            break;
        ++i;
        float v[9] = {0};
        for (int k = 0; k < 9; ++k) {
            ws();
            v[k] = (float)std::atof(s + i);
            while (i < len && s[i] != ',' && s[i] != ']')
                ++i;
            if (i < len && s[i] == ',')
                ++i;
        }
        while (i < len && s[i] != ']')
            ++i;
        if (i < len)
            ++i;
        out[cp] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]};
    }
}

void load_glyph_atlas() {
    if (g_glyph.tried)
        return;
    g_glyph.tried = true;
    tile57_assets a{};
    if (tile57_bake_glyph_sdf(&a, nullptr) != TILE57_OK)
        return;
    if (a.sprite_png && a.sprite_png_len) {
        wxMemoryInputStream mis(a.sprite_png, a.sprite_png_len);
        wxImage img;
        if (img.LoadFile(mis, wxBITMAP_TYPE_PNG)) {
            int tw = img.GetWidth(), th = img.GetHeight();
            std::vector<uint8_t> rgba((size_t)tw * th * 4);
            const uint8_t* rgb = img.GetData();
            const uint8_t* al = img.HasAlpha() ? img.GetAlpha() : nullptr;
            for (int p = 0; p < tw * th; ++p) {
                rgba[p * 4 + 0] = rgb[p * 3 + 0];
                rgba[p * 4 + 1] = rgb[p * 3 + 1];
                rgba[p * 4 + 2] = rgb[p * 3 + 2];
                rgba[p * 4 + 3] = al ? al[p] : 255;
            }
            glGenTextures(1, &g_glyph.tex);
            glBindTexture(GL_TEXTURE_2D, g_glyph.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         rgba.data());
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
    if (c < 0x80)
        return c;
    int n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07;
    } else
        return c;
    for (int k = 0; k < n && i < len; ++k)
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}
} // namespace

// Point symbol as a textured quad from the atlas: quad of ±half centred on the
// anchor, rotated, mapped to the symbol's atlas cell (AA'd by texture filtering).
void ChartRenderer::on_draw_sprite(const char* name, size_t len, tile57_world_point anchor,
                                   float rot_deg, tile57_rot_align align, float hw, float hh,
                                   float scamin) {
    if (!g_atlas.ok || hw <= 0 || hh <= 0)
        return;
    auto it = g_atlas.uv.find(std::string(name, len));
    if (it == g_atlas.uv.end())
        return;
    const AtlasRect& r = it->second;
    float awx = (float)(anchor.x - ref_wx_), awy = (float)(anchor.y - ref_wy_);
    float rad = rot_deg * (float)kPi / 180.0f, c = std::cos(rad), s = std::sin(rad);
    // rot_deg is baked into the quad here (a portray-time constant); the VIEW rotation is
    // added per frame in the shader when this sprite is MAP-aligned — it must NOT be baked,
    // or a cached tile would go stale on every degree of a turn.
    const float postrot = (align == TILE57_ALIGN_MAP) ? 1.0f : 0.0f;
    auto corner = [&](float lx, float ly, float u, float vv) {
        sprite_.push_back({awx, awy, lx * c - ly * s, lx * s + ly * c, u, vv, scamin, postrot});
    };
    corner(-hw, -hh, r.u0, r.v0);
    corner(hw, -hh, r.u1, r.v0);
    corner(hw, hh, r.u1, r.v1);
    corner(-hw, -hh, r.u0, r.v0);
    corner(hw, hh, r.u1, r.v1);
    corner(-hw, hh, r.u0, r.v1);
}

// Area fill pattern: tessellate the polygon and tile the pattern cell across it
// (world-anchored, constant screen size) via the pattern program.
void ChartRenderer::on_draw_pattern(const char* name, size_t len, const tile57_world_rings* p,
                                    float scamin) {
    if (!g_atlas.ok || !p || p->n < 3)
        return;
    auto it = g_atlas.uv.find("pat:" + std::string(name, len));
    if (it == g_atlas.uv.end()) { // unknown pattern -> flat translucent tint
        on_fill_area(p, tile57_rgba{160, 160, 170, 140}, scamin);
        return;
    }
    const AtlasRect& r = it->second;
    float tw = std::max(1.0f, r.px_w * (float)size_scale_); // tile size in screen px
    float th = std::max(1.0f, r.px_h * (float)size_scale_);
    using Pt = std::array<double, 2>;
    const double eps2 = decimate_eps_ * decimate_eps_;
    std::vector<std::vector<Pt>> poly;
    for (uint32_t rr = 0; rr < p->ring_count; ++rr) {
        uint32_t s = p->ring_starts[rr];
        uint32_t e = (rr + 1 < p->ring_count) ? p->ring_starts[rr + 1] : p->n;
        if (e <= s + 2)
            continue;
        std::vector<Pt> ring{{p->pts[s].x, p->pts[s].y}};
        for (uint32_t i = s + 1; i < e; ++i) {
            double dx = p->pts[i].x - ring.back()[0], dy = p->pts[i].y - ring.back()[1];
            if (i == e - 1 || dx * dx + dy * dy >= eps2)
                ring.push_back({p->pts[i].x, p->pts[i].y});
        }
        if (ring.size() >= 3)
            poly.push_back(std::move(ring));
    }
    if (poly.empty())
        return;
    std::vector<Pt> flat;
    for (const auto& ring : poly)
        flat.insert(flat.end(), ring.begin(), ring.end());
    std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            float wx = (float)(flat[idx[i + k]][0] - ref_wx_),
                  wy = (float)(flat[idx[i + k]][1] - ref_wy_);
            pattern_.push_back({wx, wy, r.u0, r.v0, r.u1, r.v1, tw, th, scamin});
        }
}

// Lay a label out from the SDF glyph atlas: one textured quad per glyph at the
// anchor + local px (origin + pen + glyph offset, all × the text px size). The
// atlas is monochrome SDF, tinted by the text colour in the glyph shader.
void ChartRenderer::on_draw_text_str(tile57_world_point anchor, float ox, float oy,
                                     const char* text, size_t len, float size_px, float rot_deg,
                                     tile57_rot_align align, tile57_rgba c, tile57_rgba /*halo*/,
                                     float scamin) {
    if (!g_glyph.ok || size_px <= 0 || !text)
        return;
    float awx = (float)(anchor.x - ref_wx_), awy = (float)(anchor.y - ref_wy_);
    // The label's OWN angle — a depth-contour value arrives with its contour's tangent, an
    // ordinary label with 0. Turn the laid-out run about the anchor by it (same convention as
    // the sprite quad). The VIEW rotation is NOT folded in here: it is added per frame in the
    // shader for a MAP-aligned label, so the cached label buffer survives a turn.
    const float rad = rot_deg * (float)kPi / 180.0f;
    const float rc = std::cos(rad), rs = std::sin(rad);
    const float postrot = (align == TILE57_ALIGN_MAP) ? 1.0f : 0.0f;
    float pen = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = decode_utf8(text, len, i);
        auto it = g_glyph.g.find(cp);
        if (it == g_glyph.g.end()) {
            it = g_glyph.g.find('?');
            if (it == g_glyph.g.end())
                continue;
        }
        const GlyphInfo& g = it->second;
        if (g.w > 0 && g.h > 0) {
            float x0 = ox + pen + g.ox * size_px, y0 = oy + g.oy * size_px;
            float x1 = x0 + g.w * size_px, y1 = y0 + g.h * size_px;
            auto v = [&](float px, float py, float u, float vv) {
                glyph_.push_back({awx, awy, px * rc - py * rs, px * rs + py * rc, u, vv, c.r, c.g,
                                  c.b, c.a, scamin, postrot});
            };
            v(x0, y0, g.u0, g.v0);
            v(x1, y0, g.u1, g.v0);
            v(x1, y1, g.u1, g.v1);
            v(x0, y0, g.u0, g.v0);
            v(x1, y1, g.u1, g.v1);
            v(x0, y1, g.u0, g.v1);
        }
        pen += g.adv * size_px;
    }
}

// ---- C surface trampolines (ctx == ChartRenderer*) -------------------------
// The scale denominator a feature is culled at — the ONE place the rule lives, so every
// draw kind (area, line, symbol, sprite, pattern, text) gates identically.
//
// SUPER-SCAMIN. An ENC may leave SCAMIN off a feature entirely, and S-52 reads that as
// "never scale-cull me". Honoured literally, a 1:12,000 harbour cell's SCAMIN-less buoys,
// lights and soundings keep drawing however far you zoom out — at 1:1.5M the whole cell
// lands on screen at once and the chart is a carpet of symbology. OpenCPN has the same
// problem with the same ENCs and solves it by IMPUTING a SCAMIN from the cell's own
// compilation scale (s52plib.cpp, m_bUseSUPER_SCAMIN):
//
//     if (obj->Scamin > 9e6)                      // i.e. undefined
//         obj->SuperScamin = chart_ref_scale * 2;
//     if (SuperScamin > 0 && vp_plib.chart_scale > obj->SuperScamin) b_visible = false;
//
// so a cell's ungated detail survives to 2x its compilation scale and no further. We do
// the same, with the same factor.
//
// DISPLAY BASE is exempt — land areas, depth areas, the coastline: the skeleton that must
// remain at every scale (OpenCPN spells this as an explicit class list, LNDARE/DEPARE/
// COALNE/DRGARE/...; tile57 hands us the category directly, which is the same rule stated
// once). SCAMIN never applies to it, imputed or authored.
float ChartRenderer::feature_scamin(const tile57_feature* f) const {
    if (!f)
        return kNeverCulled;
    if (f->disp_cat == TILE57_DISP_BASE)
        return kNeverCulled; // the chart's skeleton: never scale-culled (tile57.h)
    if (f->scamin > 0)
        return (float)f->scamin; // the producer said so
    if (!super_scamin_ || native_scale_ <= 0)
        return kNeverCulled; // no imputation possible / turned off
    return (float)(native_scale_ * kSuperScaminUnderzoom);
}
static float feat_scamin(void* c, const tile57_feature* f) {
    return static_cast<ChartRenderer*>(c)->feature_scamin(f);
}
static void tr_fill(void* c, const tile57_feature* f, const tile57_world_rings* r, tile57_rgba col,
                    int) {
    static_cast<ChartRenderer*>(c)->on_fill_area(r, col, feat_scamin(c, f));
}
static void tr_stroke(void* c, const tile57_feature* f, const tile57_world_rings* l, float w, float,
                      float, tile57_rgba col) {
    static_cast<ChartRenderer*>(c)->on_stroke_line(l, w, col, feat_scamin(c, f));
}
static void tr_symbol(void* c, const tile57_feature* f, tile57_world_point a,
                      const tile57_local_rings* r, tile57_rgba col, int eo, float sw,
                      tile57_rot_align align) {
    static_cast<ChartRenderer*>(c)->on_draw_symbol(a, r, col, eo, sw, align, feat_scamin(c, f));
}
static void tr_text(void* c, const tile57_feature* f, tile57_world_point a,
                    const tile57_local_rings* g, tile57_rgba col, tile57_rgba halo, float,
                    tile57_rot_align align) {
    static_cast<ChartRenderer*>(c)->on_draw_text(a, g, col, halo, align, feat_scamin(c, f));
}
static void tr_sprite(void* c, const tile57_feature* f, const char* name, size_t len,
                      tile57_world_point a, float rot, tile57_rot_align align, float hw, float hh) {
    static_cast<ChartRenderer*>(c)->on_draw_sprite(name, len, a, rot, align, hw, hh,
                                                   feat_scamin(c, f));
}
static void tr_pattern(void* c, const tile57_feature* f, const char* name, size_t len,
                       const tile57_world_rings* rings) {
    static_cast<ChartRenderer*>(c)->on_draw_pattern(name, len, rings, feat_scamin(c, f));
}
static void tr_text_str(void* c, const tile57_feature* f, tile57_world_point a, float ox, float oy,
                        const char* text, size_t len, float size, float rot, tile57_rot_align align,
                        tile57_rgba col, tile57_rgba halo) {
    static_cast<ChartRenderer*>(c)->on_draw_text_str(a, ox, oy, text, len, size, rot, align, col,
                                                     halo, feat_scamin(c, f));
}

// ---- chart / GL lifecycle --------------------------------------------------
// realpath(3) is POSIX; MSVC spells it _fullpath. Both return a malloc'd absolute
// path (or null on failure), so the caller frees either the same way.
static std::string real_path(const std::string& path) {
#ifdef _WIN32
    char* rp = _fullpath(nullptr, path.c_str(), 0);
#else
    char* rp = realpath(path.c_str(), nullptr);
#endif
    if (!rp)
        return path;
    std::string real(rp);
    std::free(rp);
    return real;
}

bool ChartRenderer::open_chart(const std::string& path) {
    if (chart_)
        return true;
    // Only baked .pmtiles archives open as charts (cells are baked to bundles up
    // front via the Build Charts dialog). Resolve symlinks so mmap sees the real file.
    std::string real = real_path(path);
    return tile57_chart_open(real.c_str(), &chart_, nullptr) == TILE57_OK && chart_;
}
bool ChartRenderer::get_info(tile57_info& out) const {
    if (!chart_)
        return false;
    tile57_chart_get_info(chart_, &out);
    return true;
}

// View rotation (course-up et al). uRot = (cos, sin) of the OpenCPN ViewPort rotation;
// (1,0) is north-up. The world term is always rotated — that IS the chart turning.
//
// The 2x2 must match ViewPort::GetPixFromLL or the chart turns AGAINST the core's overlays
// (ownship, routes, AIS). OpenCPN rotates in a y-UP frame and flips y only at the end:
//     dxr = epix*cos + npix*sin;  dyr = npix*cos - epix*sin;   // npix = +NORTH
//     screen = (w/2 + dxr, h/2 - dyr);                         // <- y flipped here
// Our world is web-mercator y-DOWN, so npix = -uy, and that reduces to the STANDARD
// rotation — NOT its transpose (which is what rotating naively in the y-down frame gives,
// and which counter-rotates the chart):
//     sx = ux*cos - uy*sin
//     sy = ux*sin + uy*cos
//
// aPost is NOT: it is a screen-px offset, and whether it turns with the chart depends on
// what it means (uPostRot selects, per draw):
//   uPostRot=1  CHART-relative — a line's half-width normal, derived from the segment's
//               WORLD direction at tessellation time. Leave it unrotated and the stroke
//               offset stops being perpendicular to the rotated segment: lines shear and
//               (at 90°) collapse to nothing.
//   uPostRot=0  SCREEN-relative — a symbol's or glyph's local px outline. These stay
//               UPRIGHT on screen under rotation (S-52 / MapLibre "viewport" alignment),
//               so text stays readable and buoys stay the right way up.
// Symbols whose rotation is referenced to NORTH (S-52 ORIENT — "map" alignment) ought to
// turn with the chart, but tile57's surface API does not yet tell a host which those are:
// the engine computes the flag and then drops it (`_ = rot_north` in its vector surface),
// and neither draw_sprite nor draw_symbol carries it. They render viewport-aligned here
// until the API exposes it.
static const char* VS = R"(
attribute vec2 aWorld;
attribute vec2 aPost;
attribute vec4 aColor;
attribute float aScamin;
attribute float aPostRot;
uniform float uScale;
uniform vec2 uOrigin;
uniform vec2 uVp;
uniform float uScaminDenom;
uniform vec2 uRot;
varying vec4 vCol;
vec2 t57rot(vec2 p){ return vec2(p.x*uRot.x - p.y*uRot.y, p.x*uRot.y + p.y*uRot.x); }
void main(){
  if (uScaminDenom > aScamin) { gl_Position = vec4(2.0,2.0,2.0,1.0); vCol = vec4(0.0); return; }
  vec2 screen = t57rot(aWorld*uScale) + uOrigin + mix(aPost, t57rot(aPost), aPostRot);
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
// The anchor rotates with the chart; the quad (aPost) stays upright — see VS.
static const char* VS_SPRITE = R"(
attribute vec2 aWorld;
attribute vec2 aPost;
attribute vec2 aUV;
attribute float aScamin;
attribute float aPostRot;
uniform float uScale;
uniform vec2 uOrigin;
uniform vec2 uVp;
uniform float uScaminDenom;
uniform vec2 uRot;
varying vec2 vUV;
vec2 t57rot(vec2 p){ return vec2(p.x*uRot.x - p.y*uRot.y, p.x*uRot.y + p.y*uRot.x); }
void main(){
  if (uScaminDenom > aScamin) { gl_Position = vec4(2.0,2.0,2.0,1.0); vUV = vec2(0.0); return; }
  // MAP-aligned sprites (ORIENT'd lights, linestyle bricks) turn their quad with the chart;
  // VIEWPORT-aligned ones (buoys, beacons) stay upright. aPostRot picks, per sprite.
  vec2 screen = t57rot(aWorld*uScale) + uOrigin + mix(aPost, t57rot(aPost), aPostRot);
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
attribute float aScamin;
uniform float uScale;
uniform vec2 uOrigin;
uniform vec2 uVp;
uniform float uScaminDenom;
uniform vec2 uRot;
varying vec4 vRect;
varying vec2 vTile;
varying vec2 vWorldPx;
void main(){
  if (uScaminDenom > aScamin) { gl_Position = vec4(2.0,2.0,2.0,1.0); return; }
  vec2 wpx = aWorld*uScale;
  vec2 screen = vec2(wpx.x*uRot.x - wpx.y*uRot.y, wpx.x*uRot.y + wpx.y*uRot.x) + uOrigin;
  vec2 ndc = vec2(screen.x/uVp.x*2.0-1.0, 1.0 - screen.y/uVp.y*2.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
  // vWorldPx stays UNROTATED: the fill lattice is anchored in the CHART frame, so the
  // pattern turns rigidly with the chart (as printed on it) instead of the cells sliding
  // through the polygon as the view spins.
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
attribute float aScamin;
attribute float aPostRot;
uniform float uScale;
uniform vec2 uOrigin;
uniform vec2 uVp;
uniform float uScaminDenom;
uniform vec2 uRot;
varying vec2 vUV;
varying vec4 vCol;
vec2 t57rot(vec2 p){ return vec2(p.x*uRot.x - p.y*uRot.y, p.x*uRot.y + p.y*uRot.x); }
void main(){
  if (uScaminDenom > aScamin) { gl_Position = vec4(2.0,2.0,2.0,1.0); vUV = vec2(0.0); vCol = vec4(0.0); return; }
  // The anchor always turns with the chart. The glyph quad turns only for a MAP-aligned
  // label — a depth-contour value, which must follow its contour; an ordinary label is
  // VIEWPORT-aligned and stays upright and readable.
  vec2 screen = t57rot(aWorld*uScale) + uOrigin + mix(aPost, t57rot(aPost), aPostRot);
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
    uint32_t sh = glCreateShader(t);
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char l[512];
        glGetShaderInfoLog(sh, 512, nullptr, l);
        wxLogMessage("tile57 shader FAIL: %s", l);
    } else
        wxLogMessage("tile57 shader ok (type=0x%x)", (unsigned)t);
    return sh;
}

namespace {
// The shader programs + uniform locations + composite quad VBO are IDENTICAL for
// every ChartRenderer, so compile them ONCE process-wide (like the atlases) instead
// of once per quilt cell. Per-instance compiles also forced a fresh Metal render-
// pipeline build per cell on macOS. ensure_gl() copies these handles into the
// instance, so all the render code that reads prog_/u_scale_/… is unchanged.
struct GlPrograms {
    uint32_t prog = 0, prog_sprite = 0, prog_pat = 0, prog_glyph = 0, prog_blit = 0, vbo_quad = 0;
    int u_scale = -1, u_origin = -1, u_vp = -1, u_denom = -1, u_rot = -1;
    int su_scale = -1, su_origin = -1, su_vp = -1, su_denom = -1, su_atlas = -1, su_rot = -1;
    int pu_scale = -1, pu_origin = -1, pu_vp = -1, pu_denom = -1, pu_atlas = -1, pu_rot = -1;
    int gu_scale = -1, gu_origin = -1, gu_vp = -1, gu_denom = -1, gu_atlas = -1, gu_rot = -1;
    int bu_tex = -1;
    bool ready = false;
};
GlPrograms g_prog;

void build_programs() {
    GlPrograms& g = g_prog;
    g.prog = glCreateProgram();
    glAttachShader(g.prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(g.prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(g.prog, 0, "aWorld");
    glBindAttribLocation(g.prog, 1, "aPost");
    glBindAttribLocation(g.prog, 2, "aColor");
    glBindAttribLocation(g.prog, 3, "aScamin");
    glBindAttribLocation(g.prog, 4, "aPostRot");
    glLinkProgram(g.prog);
    g.u_scale = glGetUniformLocation(g.prog, "uScale");
    g.u_origin = glGetUniformLocation(g.prog, "uOrigin");
    g.u_vp = glGetUniformLocation(g.prog, "uVp");
    g.u_denom = glGetUniformLocation(g.prog, "uScaminDenom");
    g.u_rot = glGetUniformLocation(g.prog, "uRot");

    // Sprite program (textured point symbols).
    g.prog_sprite = glCreateProgram();
    glAttachShader(g.prog_sprite, compile(GL_VERTEX_SHADER, VS_SPRITE));
    glAttachShader(g.prog_sprite, compile(GL_FRAGMENT_SHADER, FS_SPRITE));
    glBindAttribLocation(g.prog_sprite, 0, "aWorld");
    glBindAttribLocation(g.prog_sprite, 1, "aPost");
    glBindAttribLocation(g.prog_sprite, 2, "aUV");
    glBindAttribLocation(g.prog_sprite, 3, "aScamin");
    glBindAttribLocation(g.prog_sprite, 4, "aPostRot");
    glLinkProgram(g.prog_sprite);
    g.su_scale = glGetUniformLocation(g.prog_sprite, "uScale");
    g.su_origin = glGetUniformLocation(g.prog_sprite, "uOrigin");
    g.su_vp = glGetUniformLocation(g.prog_sprite, "uVp");
    g.su_denom = glGetUniformLocation(g.prog_sprite, "uScaminDenom");
    g.su_atlas = glGetUniformLocation(g.prog_sprite, "uAtlas");
    g.su_rot = glGetUniformLocation(g.prog_sprite, "uRot");

    // Pattern program (tiled area fills).
    g.prog_pat = glCreateProgram();
    glAttachShader(g.prog_pat, compile(GL_VERTEX_SHADER, VS_PAT));
    glAttachShader(g.prog_pat, compile(GL_FRAGMENT_SHADER, FS_PAT));
    glBindAttribLocation(g.prog_pat, 0, "aWorld");
    glBindAttribLocation(g.prog_pat, 1, "aRect");
    glBindAttribLocation(g.prog_pat, 2, "aTile");
    glBindAttribLocation(g.prog_pat, 3, "aScamin");
    glLinkProgram(g.prog_pat);
    g.pu_scale = glGetUniformLocation(g.prog_pat, "uScale");
    g.pu_origin = glGetUniformLocation(g.prog_pat, "uOrigin");
    g.pu_vp = glGetUniformLocation(g.prog_pat, "uVp");
    g.pu_denom = glGetUniformLocation(g.prog_pat, "uScaminDenom");
    g.pu_atlas = glGetUniformLocation(g.prog_pat, "uAtlas");
    g.pu_rot = glGetUniformLocation(g.prog_pat, "uRot");

    // Glyph program (SDF text).
    g.prog_glyph = glCreateProgram();
    glAttachShader(g.prog_glyph, compile(GL_VERTEX_SHADER, VS_GLYPH));
    glAttachShader(g.prog_glyph, compile(GL_FRAGMENT_SHADER, FS_GLYPH));
    glBindAttribLocation(g.prog_glyph, 0, "aWorld");
    glBindAttribLocation(g.prog_glyph, 1, "aPost");
    glBindAttribLocation(g.prog_glyph, 2, "aUV");
    glBindAttribLocation(g.prog_glyph, 3, "aColor");
    glBindAttribLocation(g.prog_glyph, 4, "aScamin");
    glBindAttribLocation(g.prog_glyph, 5, "aPostRot");
    glLinkProgram(g.prog_glyph);
    g.gu_scale = glGetUniformLocation(g.prog_glyph, "uScale");
    g.gu_origin = glGetUniformLocation(g.prog_glyph, "uOrigin");
    g.gu_vp = glGetUniformLocation(g.prog_glyph, "uVp");
    g.gu_denom = glGetUniformLocation(g.prog_glyph, "uScaminDenom");
    g.gu_atlas = glGetUniformLocation(g.prog_glyph, "uAtlas");
    g.gu_rot = glGetUniformLocation(g.prog_glyph, "uRot");

    // Composite program + a unit fullscreen quad (pos.xy, tex.uv).
    g.prog_blit = glCreateProgram();
    glAttachShader(g.prog_blit, compile(GL_VERTEX_SHADER, VS_BLIT));
    glAttachShader(g.prog_blit, compile(GL_FRAGMENT_SHADER, FS_BLIT));
    glBindAttribLocation(g.prog_blit, 0, "aPos");
    glBindAttribLocation(g.prog_blit, 1, "aTex");
    glLinkProgram(g.prog_blit);
    g.bu_tex = glGetUniformLocation(g.prog_blit, "uTex");
    const float quad[] = {-1, -1, 0, 0, 1, -1, 1, 0, 1,  1, 1, 1,
                          -1, -1, 0, 0, 1, 1,  1, 1, -1, 1, 0, 1};
    glGenBuffers(1, &g.vbo_quad);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    g.ready = true;
}
} // namespace

bool ChartRenderer::ensure_gl() {
    if (gl_ready_)
        return true;
    if (!t57_gl_loader_init())
        return false;
    if (!g_prog.ready)
        build_programs();         // compile shaders/programs ONCE process-wide
    const GlPrograms& g = g_prog; // adopt the shared handles into this instance
    prog_ = g.prog;
    u_scale_ = g.u_scale;
    u_origin_ = g.u_origin;
    u_vp_ = g.u_vp;
    u_denom_ = g.u_denom;
    u_rot_ = g.u_rot;
    prog_sprite_ = g.prog_sprite;
    su_scale_ = g.su_scale;
    su_origin_ = g.su_origin;
    su_vp_ = g.su_vp;
    su_denom_ = g.su_denom;
    su_atlas_ = g.su_atlas;
    su_rot_ = g.su_rot;
    prog_pat_ = g.prog_pat;
    pu_scale_ = g.pu_scale;
    pu_origin_ = g.pu_origin;
    pu_vp_ = g.pu_vp;
    pu_denom_ = g.pu_denom;
    pu_atlas_ = g.pu_atlas;
    pu_rot_ = g.pu_rot;
    prog_glyph_ = g.prog_glyph;
    gu_scale_ = g.gu_scale;
    gu_origin_ = g.gu_origin;
    gu_vp_ = g.gu_vp;
    gu_denom_ = g.gu_denom;
    gu_atlas_ = g.gu_atlas;
    gu_rot_ = g.gu_rot;
    prog_blit_ = g.prog_blit;
    bu_tex_ = g.bu_tex;
    vbo_quad_ = g.vbo_quad;

    // Per-instance whole-view label buffers (each chart's own decluttered text).
    glGenBuffers(1, &vbo_vtext_);
    glGenBuffers(1, &vbo_vglyph_);

    load_sprite_atlas();
    load_glyph_atlas();

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
    if (k >= 0)
        return k;
    if (const char* e = std::getenv("TILE57_SS")) {
        int v = std::atoi(e);
        k = v < 1 ? 1 : (v > 4 ? 4 : v);
        return k;
    }
    k = 2;
    if (const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER))) {
        std::string s(r);
        for (char& c : s)
            c = (char)std::tolower((unsigned char)c);
        for (const char* sw : {"llvmpipe", "softpipe", "swrast", "swr", "software", "swiftshader",
                               "microsoft basic"})
            if (s.find(sw) != std::string::npos) {
                k = 1;
                break;
            }
    }
    wxLogMessage("tile57: supersample factor = %d", k);
    return k;
}
struct SsTarget {
    uint32_t fbo = 0, tex = 0;
    int w = 0, h = 0;
    bool ok = false;
};
SsTarget g_ss;

bool ensure_ss(int w, int h) {
    if (w <= 0 || h <= 0)
        return false;
    int kSS = ss_factor();
    int sw = w * kSS, sh = h * kSS;
    if (g_ss.fbo && g_ss.w == sw && g_ss.h == sh)
        return g_ss.ok;
    if (!g_ss.fbo) {
        glGenFramebuffers(1, &g_ss.fbo);
        glGenTextures(1, &g_ss.tex);
    }
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
    g_ss.w = sw;
    g_ss.h = sh;
    g_ss.ok = ok;
    return ok;
}
} // namespace

void ChartRenderer::composite_ss() {
    glUseProgram(prog_blit_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
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

// No-op sinks: a portray pass that wants only SOME draw types still gets every
// feature portrayed by the dep; unwanted types get a no-op (emit nothing). Note the
// dep's drawText calls draw_text UNCONDITIONALLY (no null-check) — a null there
// crashes — so a text-suppressing pass MUST supply a non-null no-op draw_text_str
// (dep takes the SDF path and returns before that call).
static void tr_noop_fill(void*, const tile57_feature*, const tile57_world_rings*, tile57_rgba,
                         int) {}
static void tr_noop_stroke(void*, const tile57_feature*, const tile57_world_rings*, float, float,
                           float, tile57_rgba) {}
static void tr_noop_symbol(void*, const tile57_feature*, tile57_world_point,
                           const tile57_local_rings*, tile57_rgba, int, float, tile57_rot_align) {}
static void tr_noop_sprite(void*, const tile57_feature*, const char*, size_t, tile57_world_point,
                           float, tile57_rot_align, float, float) {}
static void tr_noop_pattern(void*, const tile57_feature*, const char*, size_t,
                            const tile57_world_rings*) {}
static void tr_noop_text(void*, const tile57_feature*, tile57_world_point,
                         const tile57_local_rings*, tile57_rgba, tile57_rgba, float,
                         tile57_rot_align) {}
static void tr_noop_text_str(void*, const tile57_feature*, tile57_world_point, float, float,
                             const char*, size_t, float, float, tile57_rot_align, tile57_rgba,
                             tile57_rgba) {}

// GEOMETRY callbacks (per-tile portray): everything EXCEPT text/labels. Text is
// portrayed separately for the whole view (fill_surface_cb_labels) so its declutter
// grid spans the view — a per-tile grid dropped labels near tile seams (missing
// lights). Symbols do not declutter (S-52 icon-allow-overlap) so they're safe per-tile.
// Text is suppressed via a no-op draw_text_str (routes the dep to the cheap SDF path
// and emits nothing — a null there would crash in emitText).
static void fill_surface_cb(tile57_surface_cb& cb, ChartRenderer* self) {
    cb.ctx = self;
    cb.fill_area = tr_fill;
    cb.stroke_line = tr_stroke;
    cb.draw_symbol = tr_symbol;
    if (g_atlas.ok) {
        cb.draw_sprite = tr_sprite;
        cb.draw_pattern = tr_pattern;
    }
    cb.draw_text = tr_noop_text;
    cb.draw_text_str = tr_noop_text_str;
}

// LABEL callbacks (whole-view text pass): only text/glyph emit; the rest are no-ops
// so the dep still resolves features + runs its declutter place() across the whole
// view, giving one shared grid so labels near tile seams survive.
static void fill_surface_cb_labels(tile57_surface_cb& cb, ChartRenderer* self) {
    cb.ctx = self;
    cb.fill_area = tr_noop_fill;
    cb.stroke_line = tr_noop_stroke;
    cb.draw_symbol = tr_noop_symbol;
    cb.draw_sprite = tr_noop_sprite;
    cb.draw_pattern = tr_noop_pattern;
    cb.draw_text = tr_text; // tessellated fallback (decluttered)
    if (g_glyph.ok)
        cb.draw_text_str = tr_text_str; // SDF glyphs (decluttered)
}

// ---- tiled path: portray + cache + compose per tile (see chart_renderer.h) -----
// Portray a single (z,x,y) tile into its own VBOs, ONCE, and cache it. Verts are
// stored relative to the tile's NW world corner (ref), so aWorld stays f32-tiny and
// the cached geometry is view-independent — compose places it via a per-tile uOrigin.
ChartRenderer::TileGeom& ChartRenderer::ensure_tile(int z, uint32_t x, uint32_t y,
                                                    const tile57_mariner& m) {
    uint64_t key = tile_key(z, x, y);
    auto it = tiles_.find(key);
    if (it != tiles_.end())
        return it->second;

    TileGeom t{};
    double n = std::pow(2.0, z);
    t.ref_wx = (double)x / n; // tile NW corner in web-mercator [0,1]
    t.ref_wy = (double)y / n;

    // Portray this tile through the shared tessellation sinks (reuse the member
    // scratch vectors), referenced to the tile origin, decimated at the tile zoom.
    area_.clear();
    line_.clear();
    symbol_.clear();
    text_.clear();
    sprite_.clear();
    pattern_.clear();
    glyph_.clear();
    ref_wx_ = t.ref_wx;
    ref_wy_ = t.ref_wy;
    decimate_eps_ = 0.5 / (256.0 * n);
    size_scale_ = m.size_scale > 0 ? m.size_scale : 1.0;
    tile57_surface_cb cb{};
    fill_surface_cb(cb, this);
    tile57_chart_tile_surface(chart_, (uint8_t)z, x, y, &m, &cb, nullptr);

    // Upload each layer to its own VBO (empty layers keep vbo==0, n==0).
    struct LayerSrc {
        const void* data;
        size_t bytes;
        uint32_t count;
    };
    const LayerSrc src[7] = {
        {area_.data(), area_.size() * sizeof(Vtx), (uint32_t)area_.size()},
        {line_.data(), line_.size() * sizeof(Vtx), (uint32_t)line_.size()},
        {symbol_.data(), symbol_.size() * sizeof(Vtx), (uint32_t)symbol_.size()},
        {text_.data(), text_.size() * sizeof(Vtx), (uint32_t)text_.size()},
        {sprite_.data(), sprite_.size() * sizeof(SpriteVtx), (uint32_t)sprite_.size()},
        {pattern_.data(), pattern_.size() * sizeof(PatVtx), (uint32_t)pattern_.size()},
        {glyph_.data(), glyph_.size() * sizeof(GlyphVtx), (uint32_t)glyph_.size()},
    };
    for (int i = 0; i < 7; ++i) {
        t.n[i] = src[i].count;
        if (!t.n[i])
            continue;
        glGenBuffers(1, &t.vbo[i]);
        glBindBuffer(GL_ARRAY_BUFFER, t.vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, src[i].bytes, src[i].data, GL_STATIC_DRAW);
    }
    return tiles_.emplace(key, t).first->second;
}

// The cell's compilation scale (1:N) + whether to impute SCAMIN from it. Both feed
// feature_scamin, which runs at PORTRAY time and bakes the result into the vertex buffers —
// so a change here invalidates every cached tile and the label cache with them.
void ChartRenderer::set_super_scamin(bool on, double native_scale) {
    if (on == super_scamin_ && native_scale == native_scale_)
        return;
    super_scamin_ = on;
    native_scale_ = native_scale;
    clear_tiles();
}

void ChartRenderer::clear_tiles() {
    for (auto& kv : tiles_)
        for (int i = 0; i < 7; ++i)
            if (kv.second.vbo[i])
                glDeleteBuffers(1, &kv.second.vbo[i]);
    tiles_.clear();
    labels_valid_ = false; // labels share the portrayal that just changed (mariner/chart)
}

// Evict least-recently-drawn tiles (and free their VBOs) until at most `cap` remain.
// Tiles drawn this frame carry the newest used_ms, so they sort last and are kept.
void ChartRenderer::evict_lru(size_t cap) {
    if (tiles_.size() <= cap)
        return;
    std::vector<std::pair<int64_t, uint64_t>> by_age; // (used_ms, key)
    by_age.reserve(tiles_.size());
    for (auto& kv : tiles_)
        by_age.push_back({kv.second.used_ms, kv.first});
    std::sort(by_age.begin(), by_age.end()); // oldest first
    size_t to_drop = tiles_.size() - cap;
    for (size_t i = 0; i < to_drop && i < by_age.size(); ++i) {
        auto it = tiles_.find(by_age[i].second);
        if (it == tiles_.end())
            continue;
        for (int j = 0; j < 7; ++j)
            if (it->second.vbo[j])
                glDeleteBuffers(1, &it->second.vbo[j]);
        tiles_.erase(it);
    }
}

// Compose the view from cached tiles: ensure each visible tile is portrayed (once)
// then draw the cached tile VBOs in paint order, each with its own per-tile origin
// uniform. Layer indices: 0 area, 1 line, 2 symbol, 3 text, 4 sprite, 5 pattern,
// 6 glyph. Batched by program (one program bind per layer, inner loop over tiles).
void ChartRenderer::render_tiled(uint32_t w, uint32_t h, const tile57_mariner& m, float cull_denom,
                                 double vwx, double vwy, double scale_px, int z, uint32_t x0,
                                 uint32_t x1, uint32_t y0, uint32_t y1, int budget,
                                 int max_portray_ms, Rot rot) {
    // Budget the per-frame portray: a big first-visit burst (many tiles entering on a
    // zoom-out) is spread over frames instead of freezing one. Uncached tiles beyond
    // the budget are skipped this frame and flagged pending; the plugin re-requests a
    // redraw (tiles_pending()) so they finish — progressive fill, not a hitch. Cached
    // tiles always draw. (unordered_map refs stay valid across inserts/erases of OTHER
    // elements, so the pointers gathered here remain good through eviction + draw.)
    const int kPortrayBudgetPerFrame = budget;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    tiles_pending_ = false;
    int portrayed = 0;
    std::vector<const TileGeom*> vis;
    vis.reserve((size_t)(x1 - x0 + 1) * (y1 - y0 + 1));
    for (uint32_t ty = y0; ty <= y1; ++ty)
        for (uint32_t tx = x0; tx <= x1; ++tx) {
            uint64_t key = tile_key(z, tx, ty);
            auto it = tiles_.find(key);
            if (it == tiles_.end()) {
                // Two ways to defer an uncached tile to a later frame: a hard COUNT cap
                // (used while MOVING, to keep the pan frame cheap) and a wall-clock TIME
                // cap (used when SETTLED, where we want to finish the whole view in as few
                // redraws as possible — ideally one — without freezing on a huge burst).
                // Each deferral schedules exactly one more redraw (tiles_pending_). Fewer
                // redraws == fewer full-canvas repaints == fewer core basemap re-tessellations.
                bool over_count = portrayed >= kPortrayBudgetPerFrame;
                bool over_time = max_portray_ms > 0 && portrayed > 0 &&
                                 (std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count() -
                                  now) >= max_portray_ms;
                if (over_count || over_time) {
                    tiles_pending_ = true;
                    continue;
                }
                ensure_tile(z, tx, ty, m); // portray + insert (once, ever)
                ++portrayed;
                it = tiles_.find(key);
                if (it == tiles_.end())
                    continue; // (defensive; ensure_tile always inserts)
            }
            it->second.used_ms = now; // LRU: mark drawn this frame
            const TileGeom& t = it->second;
            for (int i = 0; i < 7; ++i)
                if (t.n[i]) {
                    vis.push_back(&t);
                    break;
                }
        }
    // BACKDROP (fixes the "gray tiles fill in top-down during zoom" flicker): OpenCPN
    // disables its FBO chart cache on Retina, so there is no cached frame to show while a
    // new zoom level's tiles portray — cold patches read as gray. Instead, when the target
    // zoom still has cold tiles, draw the last FULLY-cached zoom's tiles underneath. They
    // sit at the correct geo position (per-tile ref_wx/ref_wy origin) just at a coarser
    // bake, so the view stays continuous (blurry -> sharp, map-style) instead of gray.
    // Cached tiles only — the backdrop NEVER portrays (no added per-frame cost).
    std::vector<const TileGeom*> back_vis;
    if (tiles_pending_ && backdrop_z_ >= 0 && backdrop_z_ != z && std::abs(backdrop_z_ - z) <= 4) {
        double half_wx, half_wy;
        rotated_half_extent(w, h, scale_px, rot, half_wx, half_wy);
        double nb = std::pow(2.0, backdrop_z_), maxb = nb - 1.0;
        long bx0 = (long)std::floor(std::clamp((vwx - half_wx) * nb, 0.0, maxb));
        long bx1 = (long)std::floor(std::clamp((vwx + half_wx) * nb, 0.0, maxb));
        long by0 = (long)std::floor(std::clamp((vwy - half_wy) * nb, 0.0, maxb));
        long by1 = (long)std::floor(std::clamp((vwy + half_wy) * nb, 0.0, maxb));
        if ((bx1 - bx0 + 1) * (by1 - by0 + 1) <= 4096) {
            for (long ty = by0; ty <= by1; ++ty)
                for (long tx = bx0; tx <= bx1; ++tx) {
                    auto it = tiles_.find(tile_key(backdrop_z_, (uint32_t)tx, (uint32_t)ty));
                    if (it == tiles_.end())
                        continue;             // draw cached backdrop tiles only
                    it->second.used_ms = now; // keep them alive through eviction
                    const TileGeom& t = it->second;
                    for (int i = 0; i < 7; ++i)
                        if (t.n[i]) {
                            back_vis.push_back(&t);
                            break;
                        }
                }
        }
    }

    // TILE57_DEBUG: portraying is the expensive path — a healthy steady frame portrays 0
    // tiles. This line printing every frame with the SAME z and a cache pinned at the LRU
    // cap means eviction churn; alternating z values mean the zoom is flapping across a
    // band boundary; see also the "cache CLEARED" line (mariner-hash flap) in render().
    static const bool dbg_portray = std::getenv("TILE57_DEBUG") != nullptr;
    if (dbg_portray && portrayed > 0)
        wxLogMessage("tile57 DBG: portrayed %d tiles z=%d range=%ux%u cache=%zu pending=%d",
                     portrayed, z, x1 - x0 + 1, y1 - y0 + 1, tiles_.size(), (int)tiles_pending_);
    // Bound cache memory: drop least-recently-drawn tiles (never this frame's target OR
    // backdrop — both just stamped used_ms=now, so they sort last and survive; the gathered
    // pointers stay valid across erases of OTHER elements).
    evict_lru(512);
    if (vis.empty() && back_vis.empty())
        return;

    const float vp[2] = {(float)w, (float)h};
    const float rv[2] = {(float)rot.c, (float)rot.s};
    // The tile's world reference point, placed on screen THROUGH the rotation. The shader
    // rotates only the aWorld term, so pre-rotating the origin here completes the rotation
    // about the viewport centre: screen = R*(world) + [C + R*(ref - view)].
    auto set_origin = [&](int uloc, const TileGeom* t) {
        const double ox = (t->ref_wx - vwx) * scale_px, oy = (t->ref_wy - vwy) * scale_px;
        float o[2] = {(float)(ox * rot.c - oy * rot.s + w * 0.5),
                      (float)(ox * rot.s + oy * rot.c + h * 0.5)};
        glUniform2fv(uloc, 1, o);
    };
    // Whether a vertex's local px offset turns with the chart is now PER-VERTEX (aPostRot),
    // because tile57 reports rotation-alignment per feature and one buffer mixes both kinds.
    auto layer = [&](const std::vector<const TileGeom*>& list, int prog, int u_scale, int u_vp,
                     int u_denom, int u_origin, int u_rot, int idx,
                     void (ChartRenderer::*drawfn)(uint32_t, uint32_t), uint32_t tex, int u_atlas) {
        glUseProgram(prog);
        glUniform1f(u_scale, (float)scale_px);
        glUniform2fv(u_vp, 1, vp);
        glUniform1f(u_denom, cull_denom);
        glUniform2fv(u_rot, 1, rv);
        if (tex) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform1i(u_atlas, 0);
        }
        for (auto* t : list)
            if (t->n[idx]) {
                set_origin(u_origin, t);
                (this->*drawfn)(t->vbo[idx], t->n[idx]);
            }
        if (tex)
            glBindTexture(GL_TEXTURE_2D, 0);
    };
    // Geometry only (paint order). Layers 3/6 (text/glyph) are never populated for
    // tiles now — labels are drawn by the whole-view text pass (draw_view_labels).
    auto draw_layers = [&](const std::vector<const TileGeom*>& list) {
        if (list.empty())
            return;
        layer(list, prog_, u_scale_, u_vp_, u_denom_, u_origin_, u_rot_, 0,
              &ChartRenderer::draw_range, 0, -1); // area
        if (g_atlas.ok)
            layer(list, prog_pat_, pu_scale_, pu_vp_, pu_denom_, pu_origin_, pu_rot_, 5,
                  &ChartRenderer::draw_pat_range, g_atlas.tex, pu_atlas_); // pattern
        layer(list, prog_, u_scale_, u_vp_, u_denom_, u_origin_, u_rot_, 1,
              &ChartRenderer::draw_range, 0, -1); // line
        layer(list, prog_, u_scale_, u_vp_, u_denom_, u_origin_, u_rot_, 2,
              &ChartRenderer::draw_range, 0, -1); // symbol (tessellated)
        if (g_atlas.ok)
            layer(list, prog_sprite_, su_scale_, su_vp_, su_denom_, su_origin_, su_rot_, 4,
                  &ChartRenderer::draw_sprite_range, g_atlas.tex, su_atlas_); // sprite
    };
    draw_layers(back_vis); // coarser fallback zoom underneath (only when target incomplete)
    draw_layers(vis);      // target zoom on top
    glUseProgram(0);

    // Remember the newest zoom that is fully cached for the whole view — that is what a
    // later (still-loading) zoom uses as its backdrop.
    if (!tiles_pending_)
        backdrop_z_ = z;
}

// Portray the WHOLE view's labels (one shared declutter grid → no per-tile seam drops)
// into the view-label buffers, referenced to the view centre. Geometry callbacks are
// no-ops (the dep still runs its declutter), but this is NOT cheap — it decodes the
// pmtiles and decluttters every sounding, so render() calls it only on settle/view-change
// (see the label cache there), not every frame.
void ChartRenderer::portray_view_labels(double lon, double lat, double zoom, double rotation,
                                        uint32_t w, uint32_t h, const tile57_mariner& m) {
    text_.clear();
    glyph_.clear();
    lonlat_to_world(lon, lat, ref_wx_, ref_wy_); // labels referenced to view centre
    decimate_eps_ = 0.5 / (256.0 * std::pow(2.0, zoom));
    size_scale_ = m.size_scale > 0 ? m.size_scale : 1.0;
    tile57_surface_cb cb{};
    fill_surface_cb_labels(cb, this);
    tile57_chart_surface(chart_, lon, lat, zoom, rotation, w, h, &m, &cb, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_vtext_);
    glBufferData(GL_ARRAY_BUFFER, text_.size() * sizeof(Vtx), text_.data(), GL_DYNAMIC_DRAW);
    n_vtext_ = (uint32_t)text_.size();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_vglyph_);
    glBufferData(GL_ARRAY_BUFFER, glyph_.size() * sizeof(GlyphVtx), glyph_.data(), GL_DYNAMIC_DRAW);
    n_vglyph_ = (uint32_t)glyph_.size();
}

// Draw the view-label buffers. Their verts are relative to the world point the cache was
// portrayed at (lbl_ref_*), so place that point on screen for the CURRENT view — the same
// per-tile origin math as render_tiled. When the cache was portrayed THIS frame lbl_ref_*
// == the view centre, so the origin is just (w/2, h/2); when reused across a pan/zoom the
// labels ride the view correctly.
void ChartRenderer::draw_view_labels(double scale_px, float cull_denom, uint32_t w, uint32_t h,
                                     double vwx, double vwy, Rot rot) {
    if (!n_vtext_ && !n_vglyph_)
        return;
    // Same pre-rotated origin as the tiles: place the cache's world reference point on
    // screen THROUGH the rotation, so the labels ride the turning chart. Their glyph quads
    // (aPost) are left unrotated by the shaders, so the text itself stays upright.
    const double ox = (lbl_ref_wx_ - vwx) * scale_px, oy = (lbl_ref_wy_ - vwy) * scale_px;
    const float origin[2] = {(float)(ox * rot.c - oy * rot.s + w * 0.5),
                             (float)(ox * rot.s + oy * rot.c + h * 0.5)};
    const float vp[2] = {(float)w, (float)h};
    const float rv[2] = {(float)rot.c, (float)rot.s};
    if (n_vtext_) { // tessellated fallback (usually empty)
        glUseProgram(prog_);
        glUniform1f(u_scale_, (float)scale_px);
        glUniform2fv(u_origin_, 1, origin);
        glUniform2fv(u_vp_, 1, vp);
        glUniform1f(u_denom_, cull_denom);
        glUniform2fv(u_rot_, 1, rv);
        draw_range(vbo_vtext_, n_vtext_);
    }
    if (n_vglyph_ && g_glyph.ok) {
        glUseProgram(prog_glyph_);
        glUniform1f(gu_scale_, (float)scale_px);
        glUniform2fv(gu_origin_, 1, origin);
        glUniform2fv(gu_vp_, 1, vp);
        glUniform1f(gu_denom_, cull_denom);
        glUniform2fv(gu_rot_, 1, rv);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_glyph.tex);
        glUniform1i(gu_atlas_, 0);
        draw_glyph_range(vbo_vglyph_, n_vglyph_);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUseProgram(0);
}

void ChartRenderer::draw_range(uint32_t vbo, uint32_t count) {
    if (!count)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, wx));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, px));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)offsetof(Vtx, r));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, scamin));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, postrot));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
}

// Draw one PatVtx buffer (prog_pat_ must be bound + uniforms/atlas set by caller).
void ChartRenderer::draw_pat_range(uint32_t vbo, uint32_t count) {
    if (!count)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, wx));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, u0));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(PatVtx), (void*)offsetof(PatVtx, tw));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(PatVtx),
                          (void*)offsetof(PatVtx, scamin));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
}

// Draw one SpriteVtx buffer (prog_sprite_ bound + uniforms/atlas set by caller).
void ChartRenderer::draw_sprite_range(uint32_t vbo, uint32_t count) {
    if (!count)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx),
                          (void*)offsetof(SpriteVtx, wx));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx),
                          (void*)offsetof(SpriteVtx, px));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx),
                          (void*)offsetof(SpriteVtx, u));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx),
                          (void*)offsetof(SpriteVtx, scamin));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(SpriteVtx),
                          (void*)offsetof(SpriteVtx, postrot));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
}

// Draw one GlyphVtx buffer (prog_glyph_ bound + uniforms/atlas set by caller).
void ChartRenderer::draw_glyph_range(uint32_t vbo, uint32_t count) {
    if (!count)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx),
                          (void*)offsetof(GlyphVtx, wx));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx),
                          (void*)offsetof(GlyphVtx, px));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx), (void*)offsetof(GlyphVtx, u));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlyphVtx),
                          (void*)offsetof(GlyphVtx, r));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx),
                          (void*)offsetof(GlyphVtx, scamin));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(GlyphVtx),
                          (void*)offsetof(GlyphVtx, postrot));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
}

void ChartRenderer::rotated_half_extent(uint32_t w, uint32_t h, double scale_px, Rot rot,
                                        double& half_wx, double& half_wy, double slack) {
    const double ac = std::min(1.0, std::fabs(rot.c) + slack);
    const double as = std::min(1.0, std::fabs(rot.s) + slack);
    half_wx = (w * ac + h * as) * 0.5 / scale_px;
    half_wy = (w * as + h * ac) * 0.5 / scale_px;
}

void ChartRenderer::render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                           const tile57_mariner& m, Pass pass, bool stencil_clip,
                           double device_scale, double cull_bias, double rotation,
                           double scamin_display_denom) {
    if (!chart_ || !ensure_gl())
        return;
    if (!have_range_) {
        tile57_info info{};
        tile57_chart_get_info(chart_, &info);
        min_zoom_ = info.min_zoom;
        max_zoom_ = info.max_zoom;
        have_range_ = true;
        have_bounds_ = info.has_bounds;
        b_w_ = info.west;
        b_s_ = info.south;
        b_e_ = info.east;
        b_n_ = info.north;
    }
    // The tile cache is the ONLY geometry source. Invalidate it when the mariner
    // settings change (they change the portrayal).
    // TILE57_DEBUG: a cache clear is supposed to be RARE (an options change). If the
    // log shows this line every frame the hash is flapping — portray cost then explodes
    // to a full re-tessellation per frame, which is exactly what an 80%-in-ensure_tile
    // profile looks like. The hash values identify WHICH two states it flaps between.
    static const bool dbg_cache = std::getenv("TILE57_DEBUG") != nullptr;
    uint64_t mh = mariner_hash(m);
    if (mh != tiles_mhash_) {
        if (dbg_cache && tiles_mhash_)
            wxLogMessage("tile57 DBG: tile cache CLEARED (%zu tiles) mariner hash %llx -> %llx "
                         "changed: %s",
                         tiles_.size(), (unsigned long long)tiles_mhash_, (unsigned long long)mh,
                         mariner_diff(tiles_m_, m).c_str());
        clear_tiles();
        tiles_mhash_ = mh;
        tiles_m_ = m; // the settings this cache is portrayed under (for the diff above)
    }

    // Motion (pan, zoom or TURN) this frame — used only to gate the offscreen supersample
    // (AA when settled, direct during a gesture). Tiles are cached, so pan/zoom/rotate just
    // re-transform them on the GPU; no re-portray is tied to motion. The rotation epsilon
    // is coarse (~0.06°) so a dithering course-up heading doesn't read as perpetual motion
    // and suppress AA forever.
    bool moving = (std::fabs(lon - last_lon_) > 1e-9 || std::fabs(lat - last_lat_) > 1e-9 ||
                   std::fabs(zoom - last_zoom_r_) > 1e-4 || std::fabs(rotation - last_rot_) > 1e-3);
    last_lon_ = lon;
    last_lat_ = lat;
    last_zoom_r_ = zoom;
    last_rot_ = rotation;

    // Per-frame transform: screen = R*(aWorld*uScale) + uOrigin + aPost (uOrigin per tile,
    // pre-rotated on the CPU in set_origin). R is the view rotation about the viewport
    // centre, in the y-DOWN screen frame — the same 2x2 OpenCPN's ViewPort::GetPixFromLL
    // applies, so our geometry lands where the core's overlays (ownship, routes, AIS) do:
    //   sx = ux*cos - uy*sin
    //   sy = ux*sin + uy*cos
    // (See the VS comment: OpenCPN rotates y-UP and flips y last, so this is the STANDARD
    // matrix. Using its transpose here turned the chart the wrong way.)
    const Rot rot{std::cos(rotation), std::sin(rotation)};
    double vwx, vwy;
    lonlat_to_world(lon, lat, vwx, vwy);
    // Projection at the PHYSICAL framebuffer: geographic zoom scaled by device_scale
    // (a 2x HiDPI framebuffer gets 2x px per world unit; zoom — and the SCAMIN cull
    // below — stays geographic).
    double scale_px = 256.0 * std::pow(2.0, zoom) * device_scale; // px per world[0,1]
    // The SCAMIN cull denominator: the host's real display scale (1:N), compared per vertex
    // against the feature's own SCAMIN — hide when the display is SMALLER scale (zoomed out)
    // than the feature allows, i.e. denom > SCAMIN. 0 disables the cull entirely (OpenCPN's
    // "Use SCAMIN" off), since 0 can never exceed any SCAMIN.
    //
    // cull_bias is 0 unless the user opted into extra thinning (TILE57_DECLUTTER): a bias
    // of B levels is a factor 2^B on the denominator (one zoom level = one doubling of 1:N),
    // applied to EVERY layer — labels included — so a feature's symbol and text always hide
    // at the same zoom.
    float cull_denom = (float)(scamin_display_denom * std::pow(2.0, cull_bias));
    if (scamin_display_denom <= 0.0)
        cull_denom = 0.0f; // cull off — keep it exactly 0, don't let the bias resurrect it

    // When OpenCPN can't use FBOs it clips quilt patches with the STENCIL buffer and
    // passes b_use_stencil=true (arrives here as stencil_clip) on the base pass. Our
    // offscreen-supersample composite clips with SCISSOR, which does NOT align with that
    // stencil path — the composite renders black/misplaced, and because only SOME passes/
    // cells hit the SS branch, the view alternates good<->black frame to frame == flicker.
    // Latch it: once the host has ever asked for stencil clipping, this GL context has no
    // usable FBO path, so force DIRECT rendering for every pass (== auto TILE57_NOSS). The
    // text/unquilted passes pass stencil_clip=false, hence the sticky flag rather than a
    // per-call test. (Machines WITH FBOs never set it, so they keep the AA path.)
    if (stencil_clip)
        host_stencil_mode_ = true;
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
    const int kSS = ss_factor(); // 1 on software GL (AA off); 2 on real GPUs
    bool ss = !noss && !host_stencil_mode_ && kSS > 1 && ensure_ss((int)w, (int)h) &&
              !(moving && pass != Pass::kText);
    // Saving GL state (glGetIntegerv/glIsEnabled) forces a driver sync — a GPU
    // pipeline stall. It is only needed to RESTORE the FBO/viewport/scissor after
    // an offscreen pass, so read it back only when supersampling; the direct
    // motion path (the interactive hot path) skips all four reads.
    GLint prev_fbo = 0, prev_vp[4] = {0, 0, 0, 0}, prev_sc[4] = {0, 0, 0, 0};
    GLboolean sc_on = GL_FALSE;
    if (ss) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glGetIntegerv(GL_VIEWPORT, prev_vp);
        glGetIntegerv(GL_SCISSOR_BOX, prev_sc);
        sc_on = glIsEnabled(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_ss.fbo);
        glViewport(0, 0, (GLsizei)(w * kSS), (GLsizei)(h * kSS));
        if (sc_on)
            glScissor(prev_sc[0] * kSS, prev_sc[1] * kSS, prev_sc[2] * kSS, prev_sc[3] * kSS);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    // Do NOT touch GL_STENCIL_TEST: the caller may have a quilt-patch mask enabled in it
    // (see QuiltClip in tile57_chart.cpp). Disabling it here is what let every cell in the
    // quilt paint the whole canvas. We never WRITE stencil (the mask is 0 there), so leaving
    // the caller's test enabled is safe; when there is no clip it is simply off already.

    // GEOMETRY (base pass / unquilted): compose from cached tiles. Pick the tile zoom
    // (the baked band the view falls in) and the visible x/y range, clamped to the
    // chart coverage so a zoomed-out view doesn't enumerate empty tiles. render_tiled
    // portrays any missing tile (budgeted) and draws the cached tiles per-tile.
    if (pass != Pass::kText) {
        int z = (int)std::lround(zoom);
        if (z < (int)min_zoom_)
            z = (int)min_zoom_;
        if (z > (int)max_zoom_)
            z = (int)max_zoom_;
        double n = std::pow(2.0, z);
        double half_wx, half_wy;
        rotated_half_extent(w, h, scale_px, rot, half_wx, half_wy);
        double wminx = vwx - half_wx, wmaxx = vwx + half_wx;
        double wminy = vwy - half_wy, wmaxy = vwy + half_wy;
        if (have_bounds_) {
            double nwx, nwy, sex, sey;
            lonlat_to_world(b_w_, b_n_, nwx, nwy); // NW -> (min x, min y)
            lonlat_to_world(b_e_, b_s_, sex, sey); // SE -> (max x, max y)
            wminx = std::max(wminx, nwx);
            wmaxx = std::min(wmaxx, sex);
            wminy = std::max(wminy, nwy);
            wmaxy = std::min(wmaxy, sey);
        }
        double maxt = n - 1.0;
        long x0 = (long)std::floor(std::clamp(wminx * n, 0.0, maxt));
        long x1 = (long)std::floor(std::clamp(wmaxx * n, 0.0, maxt));
        long y0 = (long)std::floor(std::clamp(wminy * n, 0.0, maxt));
        long y1 = (long)std::floor(std::clamp(wmaxy * n, 0.0, maxt));
        // WHILE MOVING: portray only a few cold tiles per frame (hard count cap) so the
        // pan frame stays cheap — OpenCPN is already repainting every frame during the
        // gesture, so tiles fill as you move. WHEN SETTLED: finish the whole view in as
        // FEW redraws as possible (ideally one) — a count cap of 8 spread a cold view over
        // ceil(N/8) frames, and because OpenCPN re-tessellates the world basemap on the CPU
        // every full repaint, each of those frames was a fresh basemap re-tessellation ->
        // the visible "rebuild"/flicker as tiles dribbled in. So lift the count cap and
        // instead bound the frame by wall-clock: portray until ~30 ms elapses, defer the
        // rest to one more redraw. Typical views finish in a single frame; a pathological
        // burst still can't freeze.
        // The backdrop (coarser cached zoom, drawn underneath) now hides any not-yet-portrayed
        // target tiles, so motion no longer has to crawl at a 2-tile budget to avoid gray gaps.
        // Give motion a WALL-CLOCK budget instead: portray as many as fit in ~8 ms/frame so the
        // sharp layer catches up fast over the blur, while the time cap keeps the pan/zoom frame
        // bounded. Settled: finish the view (up to 30 ms) in ideally one redraw.
        int budget = moving ? 256 : 4096;
        int portray_ms = moving ? 8 : 30;
        if (wmaxx >= wminx && wmaxy >= wminy && (x1 - x0 + 1) * (y1 - y0 + 1) <= 4096)
            render_tiled(w, h, m, cull_denom, vwx, vwy, scale_px, z, (uint32_t)x0, (uint32_t)x1,
                         (uint32_t)y0, (uint32_t)y1, budget, portray_ms, rot);
    }
    // LABELS (text pass / unquilted): portray the whole view's text with a single
    // declutter grid (per-tile grids dropped labels at seams — the missing lights). This
    // portray (pmtiles decode + declutter + soundings) is the costliest per-frame item
    // (~21% in Instruments), so cache it like the geometry tiles: portray only when the
    // view has SETTLED at a new place/zoom (or nothing is cached yet); during a gesture
    // reuse the cached buffers, drawn with a transformed origin. `moving` is unreliable
    // here (the base pass already stamped last_*_ this frame), so detect settle from the
    // previous TEXT frame (lbl_prev_*_).
    if (pass != Pass::kBase) {
        // A turn only invalidates the label cache's EXTENT and declutter, not its contents
        // (see lbl_rot_): tolerate kLblRotSlack of drift before re-portraying, and portray
        // the extent with that much slack built in so the tolerated angles stay covered.
        constexpr double kLblRotSlack = 0.09; // ~5°
        bool lbl_moving =
            std::fabs(lon - lbl_prev_lon_) > 1e-9 || std::fabs(lat - lbl_prev_lat_) > 1e-9 ||
            std::fabs(zoom - lbl_prev_zoom_) > 1e-4 || std::fabs(rotation - lbl_prev_rot_) > 1e-3;
        lbl_prev_lon_ = lon;
        lbl_prev_lat_ = lat;
        lbl_prev_zoom_ = zoom;
        lbl_prev_rot_ = rotation;
        bool stale = std::fabs(lon - lbl_lon_) > 1e-9 || std::fabs(lat - lbl_lat_) > 1e-9 ||
                     std::fabs(zoom - lbl_zoom_) > 1e-4 ||
                     std::fabs(rotation - lbl_rot_) > kLblRotSlack;
        labels_pending_ = false;
        if (!labels_valid_ || (stale && !lbl_moving)) {
            // Portray the ROTATED view's world AABB, widened by the rotation slack above so
            // labels exist out to the corners a turn exposes. North-up (rotation exactly 0,
            // the common case) takes no slack and portrays exactly the view, as before.
            double lhx, lhy;
            rotated_half_extent(w, h, scale_px, rot, lhx, lhy,
                                rotation == 0.0 ? 0.0 : kLblRotSlack);
            portray_view_labels(lon, lat, zoom, rotation,
                                (uint32_t)std::lround(2.0 * lhx * scale_px),
                                (uint32_t)std::lround(2.0 * lhy * scale_px), m);
            lbl_lon_ = lon;
            lbl_lat_ = lat;
            lbl_zoom_ = zoom;
            lbl_rot_ = rotation;
            lbl_ref_wx_ = ref_wx_;
            lbl_ref_wy_ = ref_wy_; // portray set these to the view centre
            labels_valid_ = true;
        } else if (stale) {
            labels_pending_ =
                true; // moved this frame — refresh once motion stops (ask for a redraw)
        }
        // Labels cull at the SAME denominator as every other layer. SCAMIN hides the
        // whole feature — symbol and text together; when this pass used the unbiased
        // denominator while the base pass biased its cull, features in the gap drew
        // their TEXT with no SYMBOL. cull_denom == the true display denominator unless
        // the user opted into TILE57_DECLUTTER, and then the bias moves both in lockstep.
        draw_view_labels(scale_px, cull_denom, w, h, vwx, vwy, rot);
    }

    if (ss) {
        // Composite the supersampled texture over OpenCPN's FBO, down-sampled by
        // the sampler's linear filter, clipped to the same patch scissor.
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
        if (sc_on)
            glScissor(prev_sc[0], prev_sc[1], prev_sc[2], prev_sc[3]);
        composite_ss();
    }
    // Restore the GL state OpenCPN's fixed-function / gluTess clip-region drawing
    // relies on. Leaving a vertex-attrib array enabled or a VBO bound makes its
    // immediate-mode glDrawArrays read OUR buffer out of bounds → crash (seen on
    // macOS: SetClipRegion → gluTessEndPolygon → glDrawArrays_IMM_Exec). Belt-and-
    // suspenders: our draw_* helpers already disable their arrays, but tiles churn
    // VBOs so we hard-reset here after every render.
    for (int i = 0; i < 6; ++i)
        glDisableVertexAttribArray(i);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    static const bool dbg = std::getenv("TILE57_DEBUG") != nullptr;
    if (dbg) {
        static int cnt = 0;
        if (cnt++ < 10) {
            GLenum e = glGetError();
            wxLogMessage("tile57 GL: pass=%d ss=%d tiles=%zu pending=%d prev_fbo=%d "
                         "vp=[%d,%d,%d,%d] scissor_on=%d sc=[%d,%d,%d,%d] glerr=0x%x",
                         (int)pass, ss ? 1 : 0, tiles_.size(), (int)tiles_pending_, prev_fbo,
                         prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3], (int)sc_on, prev_sc[0],
                         prev_sc[1], prev_sc[2], prev_sc[3], (unsigned)e);
        }
    }
}

void ChartRenderer::shutdown() {
    gl_ready_ = false;
    prog_ = prog_sprite_ = prog_pat_ = prog_glyph_ = prog_blit_ = 0;
    vbo_quad_ = 0;
    have_range_ = false;
    tiles_.clear();
    tiles_mhash_ = 0; // GL buffers die with the context on shutdown
    // The shared programs die with the context too — force a rebuild on the next
    // ensure_gl (e.g. after a plugin reload / context recreate).
    g_prog = GlPrograms{};
    if (chart_) {
        tile57_chart_close(chart_);
        chart_ = nullptr;
    }
}

} // namespace t57
