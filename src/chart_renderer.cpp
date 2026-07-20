// chart_renderer.cpp — see chart_renderer.h.
//
// Consumes tile57's draw-ready GPU scene (tile57_chart_gpu_scene): the whole view,
// pre-tessellated and in paint order. We upload the buffers ONCE per view and, on
// pan/zoom/rotate, just re-transform them (uScale/uOrigin/uRot) and re-gate SCAMIN
// + display category in the vertex shader — no re-portray. The scene overscans the
// viewport and rebuilds only when the view leaves that coverage or the mariner
// settings change (the lookout-core model).
#include "chart_renderer.h"
#include "gl.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/mstream.h>

namespace t57 {

namespace {
constexpr double kPi = 3.14159265358979323846;
// Overscan the built view so panning within the margin re-uses the scene, and the
// zoom drift that forces a fresh build (2^0.3 < 1.25, so a rebuild lands before the
// margin is exhausted). Mirrors lookout-core/src/gpu.zig (OVERSCAN / ZOOM_REBUILD).
constexpr double kOverscan = 1.25;
constexpr double kZoomRebuild = 0.3;

void lonlat_to_world(double lon, double lat, double& wx, double& wy) {
    wx = (lon + 180.0) / 360.0;
    double s = std::sin(lat * kPi / 180.0);
    wy = 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * kPi);
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
    // viewing_groups_off is a POINTER; hash what it points at too (swapping one denied
    // group for another, same buffer/length, must still invalidate the scene).
    if (m.viewing_groups_off && m.viewing_groups_off_len)
        mix(m.viewing_groups_off, m.viewing_groups_off_len * sizeof(int32_t));
    return h;
}

// ---- shared atlas textures (baked once per process, in a live GL context) --------
// The GPU scene's quad UVs are baked against exactly these atlases, so all we need
// from them is the texture — no JSON metrics (tile57 already placed every glyph).
struct AtlasTex {
    uint32_t tex = 0;
    bool tried = false, ok = false;
};
AtlasTex g_sprite, g_glyph;

uint32_t upload_png_rgba(const uint8_t* png, size_t len) {
    if (!png || !len)
        return 0;
    wxMemoryInputStream mis(png, len);
    wxImage img;
    if (!img.LoadFile(mis, wxBITMAP_TYPE_PNG))
        return 0;
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
    uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void load_sprite_atlas() {
    if (g_sprite.tried)
        return;
    g_sprite.tried = true;
    tile57_assets a{};
    if (tile57_bake_sprite_mln(nullptr, &a, nullptr) == TILE57_OK) {
        g_sprite.tex = upload_png_rgba(a.sprite_png, a.sprite_png_len);
        g_sprite.ok = g_sprite.tex != 0;
    }
    tile57_assets_free(&a);
}
void load_glyph_atlas() {
    if (g_glyph.tried)
        return;
    g_glyph.tried = true;
    tile57_assets a{};
    if (tile57_bake_glyph_sdf(&a, nullptr) == TILE57_OK) {
        g_glyph.tex = upload_png_rgba(a.sprite_png, a.sprite_png_len);
        g_glyph.ok = g_glyph.tex != 0;
    }
    tile57_assets_free(&a);
}

// ---- shaders (GLSL 1.20 — no integer/bitwise ops) --------------------------------
// One transform, shared by every program: screen_px = R*(aWorld*uScale) + uOrigin +
// post, with aWorld camera-relative to the scene's build centre (uOrigin folds in the
// live pan). aPost is the tile57_gpu_vertex/quad screen-px offset (line half-width,
// glyph/sprite corner); MAP-aligned marks (aMapAlign=1) rotate their post with the
// chart, viewport-aligned ones stay upright. The SCAMIN + display-category gate mirror
// lookout's chart.vert: base category (aDispCat 0) is never SCAMIN-culled; uCat is
// per-category visibility (base, standard, other) as 0/1.
#define T57_XFORM_UNIFORMS                                                                          \
    "uniform float uScale; uniform vec2 uOrigin; uniform vec2 uVp; uniform float uScaminDenom;"     \
    "uniform vec2 uRot; uniform vec3 uCat;\n"                                                       \
    "vec2 t57rot(vec2 p){ return vec2(p.x*uRot.x - p.y*uRot.y, p.x*uRot.y + p.y*uRot.x); }\n"       \
    "bool t57cull(float dispCat, float scamin){\n"                                                  \
    "  float catv = dispCat < 0.5 ? uCat.x : (dispCat < 1.5 ? uCat.y : uCat.z);\n"                  \
    "  return (catv < 0.5) || (scamin > 0.0 && dispCat > 0.5 && uScaminDenom > scamin);\n"          \
    "}\n"

static const char* VS_FLAT = T57_XFORM_UNIFORMS
    "attribute vec2 aWorld; attribute vec2 aPost; attribute float aScamin;\n"
    "attribute float aDispCat; attribute float aMapAlign;\n"
    "uniform vec4 uColor;\n"
    "varying vec4 vCol;\n"
    "void main(){\n"
    "  if (t57cull(aDispCat, aScamin)) { gl_Position = vec4(2.0); vCol = vec4(0.0); return; }\n"
    "  vec2 post = (aMapAlign > 0.5) ? t57rot(aPost) : aPost;\n"
    "  vec2 screen = t57rot(aWorld*uScale) + uOrigin + post;\n"
    "  gl_Position = vec4(screen.x/uVp.x*2.0-1.0, 1.0 - screen.y/uVp.y*2.0, 0.0, 1.0);\n"
    "  vCol = uColor;\n"
    "}\n";
static const char* FS_FLAT =
    "varying vec4 vCol;\n"
    "void main(){ gl_FragColor = vec4(vCol.rgb*vCol.a, vCol.a); }\n";

// Pattern: the polygon interior (aPost is 0), tiled per-fragment at a constant screen
// size, anchored to the CHART. vWorldPx = aWorld*uScale is camera-relative screen px
// (UNROTATED, so the lattice turns rigidly with the chart); phase = fract(vWorldPx /
// period) keeps the cell fixed to the chart under a pan. Using a varying (not
// gl_FragCoord) keeps the phase correct in the supersample FBO.
static const char* VS_PAT = T57_XFORM_UNIFORMS
    "attribute vec2 aWorld; attribute float aScamin; attribute float aDispCat;\n"
    "varying vec2 vWorldPx;\n"
    "void main(){\n"
    "  if (t57cull(aDispCat, aScamin)) { gl_Position = vec4(2.0); vWorldPx = vec2(0.0); return; }\n"
    "  vec2 wpx = aWorld*uScale;\n"
    "  vec2 screen = t57rot(wpx) + uOrigin;\n"
    "  gl_Position = vec4(screen.x/uVp.x*2.0-1.0, 1.0 - screen.y/uVp.y*2.0, 0.0, 1.0);\n"
    "  vWorldPx = wpx;\n"
    "}\n";
static const char* FS_PAT =
    "uniform sampler2D uCell; uniform vec2 uPeriod;\n"
    "varying vec2 vWorldPx;\n"
    "void main(){\n"
    "  vec2 uv = fract(vWorldPx / max(uPeriod, vec2(1.0)));\n"
    "  vec4 c = texture2D(uCell, uv);\n"
    "  if (c.a < 0.02) discard;\n"
    "  gl_FragColor = vec4(c.rgb*c.a, c.a);\n"
    "}\n";

// Sprite: a symbol/sounding quad. The anchor (aWorld) rides the chart; the corner
// (aPost) stays upright unless MAP-aligned. Artwork carries its own colour.
static const char* VS_SPRITE = T57_XFORM_UNIFORMS
    "attribute vec2 aWorld; attribute vec2 aPost; attribute vec2 aUV; attribute float aScamin;\n"
    "attribute float aDispCat; attribute float aMapAlign;\n"
    "varying vec2 vUV;\n"
    "void main(){\n"
    "  if (t57cull(aDispCat, aScamin)) { gl_Position = vec4(2.0); vUV = vec2(0.0); return; }\n"
    "  vec2 post = (aMapAlign > 0.5) ? t57rot(aPost) : aPost;\n"
    "  vec2 screen = t57rot(aWorld*uScale) + uOrigin + post;\n"
    "  gl_Position = vec4(screen.x/uVp.x*2.0-1.0, 1.0 - screen.y/uVp.y*2.0, 0.0, 1.0);\n"
    "  vUV = aUV;\n"
    "}\n";
static const char* FS_SPRITE =
    "uniform sampler2D uAtlas;\n"
    "varying vec2 vUV;\n"
    "void main(){ vec4 t = texture2D(uAtlas, vUV); gl_FragColor = vec4(t.rgb*t.a, t.a); }\n";

// SDF glyph: same quad, but sample the signed-distance field, antialias with fwidth,
// tint by the per-vertex colour, embolden by the per-vertex weight (S-52 important text).
static const char* VS_GLYPH = T57_XFORM_UNIFORMS
    "attribute vec2 aWorld; attribute vec2 aPost; attribute vec2 aUV; attribute vec4 aColor;\n"
    "attribute float aWeight; attribute float aScamin; attribute float aDispCat;\n"
    "attribute float aMapAlign;\n"
    "varying vec2 vUV; varying vec4 vCol; varying float vWeight;\n"
    "void main(){\n"
    "  if (t57cull(aDispCat, aScamin)) { gl_Position = vec4(2.0); vUV = vec2(0.0); vCol = "
    "vec4(0.0); vWeight = 0.0; return; }\n"
    "  vec2 post = (aMapAlign > 0.5) ? t57rot(aPost) : aPost;\n"
    "  vec2 screen = t57rot(aWorld*uScale) + uOrigin + post;\n"
    "  gl_Position = vec4(screen.x/uVp.x*2.0-1.0, 1.0 - screen.y/uVp.y*2.0, 0.0, 1.0);\n"
    "  vUV = aUV; vCol = aColor; vWeight = aWeight;\n"
    "}\n";
static const char* FS_GLYPH =
    "uniform sampler2D uAtlas;\n"
    "varying vec2 vUV; varying vec4 vCol; varying float vWeight;\n"
    "void main(){\n"
    "  float d = texture2D(uAtlas, vUV).r;\n"
    "  float w = fwidth(d);\n"
    "  float edge = 0.5 - vWeight;\n"
    "  float a = smoothstep(edge - w, edge + w, d);\n"
    "  if (a <= 0.0) discard;\n"
    "  float al = vCol.a * a;\n"
    "  gl_FragColor = vec4(vCol.rgb * al, al);\n" // premultiplied
    "}\n";

static const char* VS_BLIT =
    "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex;\n"
    "void main(){ vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char* FS_BLIT =
    "uniform sampler2D uTex; varying vec2 vTex;\n"
    "void main(){ gl_FragColor = texture2D(uTex, vTex); }\n";

uint32_t compile(GLenum t, const char* body) {
    std::string src = std::string(T57_GLSL_VERSION) + body;
    const char* s = src.c_str();
    uint32_t sh = glCreateShader(t);
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        wxLogMessage("tile57 shader compile failed: %s", log);
    }
    return sh;
}

// Programs are shared process-wide (every quilt cell renders into the same context);
// building them per renderer recompiled all five on every chart close/open churn.
struct GlPrograms {
    uint32_t flat = 0, pat = 0, sprite = 0, glyph = 0, blit = 0, vbo_quad = 0;
    int f_scale, f_origin, f_vp, f_denom, f_rot, f_cat, f_color;
    int p_scale, p_origin, p_vp, p_denom, p_rot, p_cat, p_cell, p_period;
    int s_scale, s_origin, s_vp, s_denom, s_rot, s_cat, s_atlas;
    int g_scale, g_origin, g_vp, g_denom, g_rot, g_cat, g_atlas;
    int b_tex;
    bool ready = false;
};
GlPrograms g_prog;

void bind_xform_attribs(uint32_t p) {
    glBindAttribLocation(p, 0, "aWorld");
    glBindAttribLocation(p, 1, "aPost");
    glBindAttribLocation(p, 2, "aScamin");
    glBindAttribLocation(p, 3, "aDispCat");
    glBindAttribLocation(p, 4, "aMapAlign");
}
void get_xform_uniforms(uint32_t p, int& sc, int& or_, int& vp, int& dn, int& rot, int& cat) {
    sc = glGetUniformLocation(p, "uScale");
    or_ = glGetUniformLocation(p, "uOrigin");
    vp = glGetUniformLocation(p, "uVp");
    dn = glGetUniformLocation(p, "uScaminDenom");
    rot = glGetUniformLocation(p, "uRot");
    cat = glGetUniformLocation(p, "uCat");
}

void build_programs() {
    GlPrograms& g = g_prog;

    g.flat = glCreateProgram();
    glAttachShader(g.flat, compile(GL_VERTEX_SHADER, VS_FLAT));
    glAttachShader(g.flat, compile(GL_FRAGMENT_SHADER, FS_FLAT));
    bind_xform_attribs(g.flat);
    glLinkProgram(g.flat);
    get_xform_uniforms(g.flat, g.f_scale, g.f_origin, g.f_vp, g.f_denom, g.f_rot, g.f_cat);
    g.f_color = glGetUniformLocation(g.flat, "uColor");

    g.pat = glCreateProgram();
    glAttachShader(g.pat, compile(GL_VERTEX_SHADER, VS_PAT));
    glAttachShader(g.pat, compile(GL_FRAGMENT_SHADER, FS_PAT));
    glBindAttribLocation(g.pat, 0, "aWorld");
    glBindAttribLocation(g.pat, 2, "aScamin");
    glBindAttribLocation(g.pat, 3, "aDispCat");
    glLinkProgram(g.pat);
    get_xform_uniforms(g.pat, g.p_scale, g.p_origin, g.p_vp, g.p_denom, g.p_rot, g.p_cat);
    g.p_cell = glGetUniformLocation(g.pat, "uCell");
    g.p_period = glGetUniformLocation(g.pat, "uPeriod");

    g.sprite = glCreateProgram();
    glAttachShader(g.sprite, compile(GL_VERTEX_SHADER, VS_SPRITE));
    glAttachShader(g.sprite, compile(GL_FRAGMENT_SHADER, FS_SPRITE));
    glBindAttribLocation(g.sprite, 0, "aWorld");
    glBindAttribLocation(g.sprite, 1, "aPost");
    glBindAttribLocation(g.sprite, 2, "aUV");
    glBindAttribLocation(g.sprite, 3, "aScamin");
    glBindAttribLocation(g.sprite, 4, "aDispCat");
    glBindAttribLocation(g.sprite, 5, "aMapAlign");
    glLinkProgram(g.sprite);
    get_xform_uniforms(g.sprite, g.s_scale, g.s_origin, g.s_vp, g.s_denom, g.s_rot, g.s_cat);
    g.s_atlas = glGetUniformLocation(g.sprite, "uAtlas");

    g.glyph = glCreateProgram();
    glAttachShader(g.glyph, compile(GL_VERTEX_SHADER, VS_GLYPH));
    glAttachShader(g.glyph, compile(GL_FRAGMENT_SHADER, FS_GLYPH));
    glBindAttribLocation(g.glyph, 0, "aWorld");
    glBindAttribLocation(g.glyph, 1, "aPost");
    glBindAttribLocation(g.glyph, 2, "aUV");
    glBindAttribLocation(g.glyph, 3, "aColor");
    glBindAttribLocation(g.glyph, 4, "aWeight");
    glBindAttribLocation(g.glyph, 5, "aScamin");
    glBindAttribLocation(g.glyph, 6, "aDispCat");
    glBindAttribLocation(g.glyph, 7, "aMapAlign");
    glLinkProgram(g.glyph);
    get_xform_uniforms(g.glyph, g.g_scale, g.g_origin, g.g_vp, g.g_denom, g.g_rot, g.g_cat);
    g.g_atlas = glGetUniformLocation(g.glyph, "uAtlas");

    g.blit = glCreateProgram();
    glAttachShader(g.blit, compile(GL_VERTEX_SHADER, VS_BLIT));
    glAttachShader(g.blit, compile(GL_FRAGMENT_SHADER, FS_BLIT));
    glBindAttribLocation(g.blit, 0, "aPos");
    glBindAttribLocation(g.blit, 1, "aTex");
    glLinkProgram(g.blit);
    g.b_tex = glGetUniformLocation(g.blit, "uTex");
    const float quad[] = {-1, -1, 0, 0, 1, -1, 1, 0, 1,  1, 1, 1,
                          -1, -1, 0, 0, 1, 1,  1, 1, -1, 1, 0, 1};
    glGenBuffers(1, &g.vbo_quad);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    g.ready = true;
}

// ---- supersample AA target (shared; render kSS× then composite down) -------------
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

std::string real_path(const std::string& path) { return path; }
} // namespace

// ---- chart open / info -----------------------------------------------------------
bool ChartRenderer::open_chart(const std::string& path) {
    if (chart_)
        return true;
    tile57_error err{};
    if (tile57_chart_open(real_path(path).c_str(), &chart_, &err) != TILE57_OK) {
        wxLogMessage("tile57: open failed: %s", err.message);
        chart_ = nullptr;
        return false;
    }
    return chart_ != nullptr;
}

bool ChartRenderer::get_info(tile57_info& out) const {
    if (!chart_)
        return false;
    tile57_chart_get_info(chart_, &out);
    return true;
}

void ChartRenderer::set_super_scamin(bool on, double native_scale) {
    super_scamin_ = on;
    native_scale_ = native_scale;
}

// ---- GL setup --------------------------------------------------------------------
bool ChartRenderer::ensure_gl() {
    if (gl_ready_)
        return true;
    if (!t57_gl_loader_init())
        return false;
    if (g_prog.ready && !glIsProgram(g_prog.flat))
        g_prog = GlPrograms{}; // dead context (plugin reload) -> rebuild the shared block
    if (!g_prog.ready)
        build_programs();
    const GlPrograms& g = g_prog;
    prog_ = g.flat;
    u_scale_ = g.f_scale;
    u_origin_ = g.f_origin;
    u_vp_ = g.f_vp;
    u_denom_ = g.f_denom;
    u_rot_ = g.f_rot;
    u_cat_ = g.f_cat;
    u_color_ = g.f_color;
    prog_pat_ = g.pat;
    pu_scale_ = g.p_scale;
    pu_origin_ = g.p_origin;
    pu_vp_ = g.p_vp;
    pu_denom_ = g.p_denom;
    pu_rot_ = g.p_rot;
    pu_cat_ = g.p_cat;
    pu_cell_ = g.p_cell;
    pu_period_ = g.p_period;
    prog_sprite_ = g.sprite;
    su_scale_ = g.s_scale;
    su_origin_ = g.s_origin;
    su_vp_ = g.s_vp;
    su_denom_ = g.s_denom;
    su_rot_ = g.s_rot;
    su_cat_ = g.s_cat;
    su_atlas_ = g.s_atlas;
    prog_glyph_ = g.glyph;
    gu_scale_ = g.g_scale;
    gu_origin_ = g.g_origin;
    gu_vp_ = g.g_vp;
    gu_denom_ = g.g_denom;
    gu_rot_ = g.g_rot;
    gu_cat_ = g.g_cat;
    gu_atlas_ = g.g_atlas;
    prog_blit_ = g.blit;
    bu_tex_ = g.b_tex;
    vbo_quad_ = g.vbo_quad;

    load_sprite_atlas();
    load_glyph_atlas();
    gl_ready_ = true;
    return true;
}

// ---- scene build / free ----------------------------------------------------------
void ChartRenderer::free_scene() {
    if (scene_.vbo)
        glDeleteBuffers(1, &scene_.vbo);
    if (scene_.ibo)
        glDeleteBuffers(1, &scene_.ibo);
    if (scene_.qbo)
        glDeleteBuffers(1, &scene_.qbo);
    for (uint32_t t : scene_.pat_tex)
        if (t)
            glDeleteTextures(1, &t);
    scene_ = Scene{};
}

void ChartRenderer::build_scene(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                                const tile57_mariner& m) {
    free_scene();
    if (!chart_)
        return;
    double cx, cy;
    lonlat_to_world(lon, lat, cx, cy);
    // Clamp the build zoom to the archive's baked band (a chart only has tiles inside
    // [min,max]); the camera over/under-scales the deepest/coarsest band past it.
    double bz = std::clamp(zoom, min_zoom_, max_zoom_);
    // Overscan the portrayed view so a pan within the margin reuses the scene.
    uint32_t ow = (uint32_t)std::lround(w * kOverscan);
    uint32_t oh = (uint32_t)std::lround(h * kOverscan);

    tile57_gpu_scene sc{};
    tile57_error err{};
    tile57_mariner mm = m;
    if (tile57_chart_gpu_scene(chart_, lon, lat, bz, ow, oh, &mm, &sc, &err) != TILE57_OK) {
        wxLogMessage("tile57: gpu_scene failed: %s", err.message);
        return;
    }

    // Rebase the triangle verts camera-relative to (cx,cy): the ABI hands absolute
    // web-mercator [0,1] as f32, which loses precision at harbour zoom; small values
    // keep f32 exact (the live pan is folded back in via uOrigin per frame).
    if (sc.vertex_count) {
        std::vector<tile57_gpu_vertex> v(sc.vertices, sc.vertices + sc.vertex_count);
        for (auto& p : v) {
            p.x -= (float)cx;
            p.y -= (float)cy;
        }
        glGenBuffers(1, &scene_.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, scene_.vbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(tile57_gpu_vertex), v.data(),
                     GL_STATIC_DRAW);
    }
    if (sc.index_count) {
        glGenBuffers(1, &scene_.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene_.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sc.index_count * sizeof(uint32_t), sc.indices,
                     GL_STATIC_DRAW);
    }
    if (sc.quad_count) {
        std::vector<tile57_gpu_quad> q(sc.quads, sc.quads + sc.quad_count);
        for (auto& p : q) {
            p.x -= (float)cx;
            p.y -= (float)cy;
        }
        glGenBuffers(1, &scene_.qbo);
        glBindBuffer(GL_ARRAY_BUFFER, scene_.qbo);
        glBufferData(GL_ARRAY_BUFFER, q.size() * sizeof(tile57_gpu_quad), q.data(), GL_STATIC_DRAW);
        scene_.quad_verts = (uint32_t)sc.quad_count;
    }
    // One texture per area-fill pattern cell (RGBA8, already at the scene's screen
    // density, so its w/h ARE the on-screen tiling period in device px).
    scene_.pat_tex.resize(sc.pattern_count, 0);
    scene_.pat_wh.resize(sc.pattern_count, {1, 1});
    for (size_t i = 0; i < sc.pattern_count; ++i) {
        const tile57_gpu_pattern& p = sc.patterns[i];
        if (!p.rgba || !p.w || !p.h)
            continue;
        uint32_t tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)p.w, (GLsizei)p.h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, p.rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        scene_.pat_tex[i] = tex;
        scene_.pat_wh[i] = {(int)p.w, (int)p.h};
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    scene_.ranges.reserve(sc.range_count);
    for (size_t i = 0; i < sc.range_count; ++i) {
        const tile57_gpu_range& r = sc.ranges[i];
        Range out;
        out.first = r.first;
        out.count = r.count;
        out.kind = r.kind;
        out.prim = r.prim;
        out.atlas = r.atlas;
        out.pattern = r.pattern;
        std::memcpy(out.color, r.color, 4);
        scene_.ranges.push_back(out);
    }

    scene_.cx = cx;
    scene_.cy = cy;
    scene_.build_zoom = bz;
    double wp = 256.0 * std::pow(2.0, bz); // geographic px per world unit at the build zoom
    scene_.half_wx = (ow * 0.5) / wp;
    scene_.half_wy = (oh * 0.5) / wp;
    scene_.ok = true;
    tile57_gpu_scene_free(&sc);
}

bool ChartRenderer::scene_covers(double vwx, double vwy, double zoom) const {
    if (!scene_.ok)
        return false;
    if (std::fabs(zoom - scene_.build_zoom) > kZoomRebuild)
        return false;
    // The live view's centre must stay inside the overscanned box, with room for at
    // least the un-overscanned viewport half around it (approximated by half the
    // margin, so a pan toward an edge rebuilds before it runs off the coverage).
    double keep_x = scene_.half_wx * (1.0 / kOverscan);
    double keep_y = scene_.half_wy * (1.0 / kOverscan);
    return std::fabs(vwx - scene_.cx) <= (scene_.half_wx - keep_x) &&
           std::fabs(vwy - scene_.cy) <= (scene_.half_wy - keep_y);
}

// ---- draw ------------------------------------------------------------------------
void ChartRenderer::draw_range(const Range& r) {
    glBindBuffer(GL_ARRAY_BUFFER, scene_.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene_.ibo);
    const GLsizei stride = sizeof(tile57_gpu_vertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);              // aWorld
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)8);              // aPost
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)16);             // aScamin
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, (void*)20);     // aDispCat
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, (void*)21);     // aMapAlign
    float col[4] = {r.color[0] / 255.f, r.color[1] / 255.f, r.color[2] / 255.f, r.color[3] / 255.f};
    glUniform4fv(u_color_, 1, col);
    glDrawElements(GL_TRIANGLES, (GLsizei)r.count, GL_UNSIGNED_INT,
                   (void*)(size_t)(r.first * sizeof(uint32_t)));
}

void ChartRenderer::draw_pat_range(const Range& r) {
    if (r.pattern == 0xFFFFFFFFu || r.pattern >= scene_.pat_tex.size())
        return;
    glBindBuffer(GL_ARRAY_BUFFER, scene_.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scene_.ibo);
    const GLsizei stride = sizeof(tile57_gpu_vertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);          // aWorld
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)16);         // aScamin
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, (void*)20); // aDispCat
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_.pat_tex[r.pattern]);
    glUniform1i(pu_cell_, 0);
    float period[2] = {(float)scene_.pat_wh[r.pattern].first, (float)scene_.pat_wh[r.pattern].second};
    glUniform2fv(pu_period_, 1, period);
    glDrawElements(GL_TRIANGLES, (GLsizei)r.count, GL_UNSIGNED_INT,
                   (void*)(size_t)(r.first * sizeof(uint32_t)));
}

void ChartRenderer::draw_quad_range(const Range& r) {
    glBindBuffer(GL_ARRAY_BUFFER, scene_.qbo);
    const GLsizei stride = sizeof(tile57_gpu_quad);
    // tile57_gpu_quad: x,y(0) ox,oy(8) u,v(16) color[4](24) weight(28) scamin(32)
    //                  disp_cat(36) map_align(37)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);              // aWorld
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)8);              // aPost
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)16);             // aUV
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)24);      // aColor
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)28);             // aWeight
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride, (void*)32);             // aScamin
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, (void*)36);     // aDispCat
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, (void*)37);     // aMapAlign
    glDrawArrays(GL_TRIANGLES, (GLint)r.first, (GLsizei)r.count);
}

void ChartRenderer::draw_scene(double scale_px, const double origin[2], uint32_t vw, uint32_t vh,
                               Rot rot, float cull_denom, uint32_t cat_mask, Pass pass) {
    if (!scene_.ok)
        return;
    const bool want_text = pass != Pass::kBase;
    const bool want_geom = pass != Pass::kText;
    const float sc = (float)scale_px;
    const float vp[2] = {(float)vw, (float)vh};
    const float org[2] = {(float)origin[0], (float)origin[1]};
    const float rv[2] = {(float)rot.c, (float)rot.s};
    const float cat[3] = {(cat_mask & 1) ? 1.f : 0.f, (cat_mask & 2) ? 1.f : 0.f,
                          (cat_mask & 4) ? 1.f : 0.f};
    auto set_xform = [&](uint32_t prog, int us, int uo, int uv, int ud, int ur, int uc) {
        glUseProgram(prog);
        glUniform1f(us, sc);
        glUniform2fv(uo, 1, org);
        glUniform2fv(uv, 1, vp);
        glUniform1f(ud, cull_denom);
        glUniform2fv(ur, 1, rv);
        glUniform3fv(uc, 1, cat);
    };
    // Ranges are already sorted into S-52 paint order — walk them in order, switching
    // pipeline per kind. draw_pass filters TEXT so the host's base/text split works.
    for (const Range& r : scene_.ranges) {
        const bool is_text = r.kind == TILE57_GPU_TEXT;
        if (is_text ? !want_text : !want_geom)
            continue;
        switch (r.kind) {
        case TILE57_GPU_AREA:
        case TILE57_GPU_LINE:
            if (r.pattern != 0xFFFFFFFFu) {
                set_xform(prog_pat_, pu_scale_, pu_origin_, pu_vp_, pu_denom_, pu_rot_, pu_cat_);
                draw_pat_range(r);
            } else {
                set_xform(prog_, u_scale_, u_origin_, u_vp_, u_denom_, u_rot_, u_cat_);
                draw_range(r);
            }
            break;
        case TILE57_GPU_PATTERN:
            set_xform(prog_pat_, pu_scale_, pu_origin_, pu_vp_, pu_denom_, pu_rot_, pu_cat_);
            draw_pat_range(r);
            break;
        case TILE57_GPU_SYMBOL:
        case TILE57_GPU_SOUNDING:
            set_xform(prog_sprite_, su_scale_, su_origin_, su_vp_, su_denom_, su_rot_, su_cat_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_sprite.tex);
            glUniform1i(su_atlas_, 0);
            draw_quad_range(r);
            break;
        case TILE57_GPU_TEXT:
            set_xform(prog_glyph_, gu_scale_, gu_origin_, gu_vp_, gu_denom_, gu_rot_, gu_cat_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_glyph.tex);
            glUniform1i(gu_atlas_, 0);
            draw_quad_range(r);
            break;
        default:
            break;
        }
    }
    for (int i = 0; i < 8; ++i)
        glDisableVertexAttribArray(i);
}

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

// ---- render ----------------------------------------------------------------------
void ChartRenderer::render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                           const tile57_mariner& m, Pass pass, bool stencil_clip,
                           double device_scale, double cull_bias, double rotation,
                           double scamin_display_denom, const int* patch_fb) {
    (void)patch_fb; // one overscanned scene covers the whole view; the host's GL scissor clips it
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

    double vwx, vwy;
    lonlat_to_world(lon, lat, vwx, vwy);

    // Rebuild the scene on a mariner change or when the view leaves the cached
    // coverage / zoom band. Serialize with the object-query path (chart_ is not
    // internally synchronized).
    uint64_t mh = mariner_hash(m);
    {
        std::lock_guard<std::mutex> lk(portray_mu_);
        if (mh != scene_mhash_ || !scene_covers(vwx, vwy, zoom)) {
            build_scene(lon, lat, zoom, w, h, m);
            scene_mhash_ = mh;
        }
    }

    bool moving = (std::fabs(lon - last_lon_) > 1e-9 || std::fabs(lat - last_lat_) > 1e-9 ||
                   std::fabs(zoom - last_zoom_r_) > 1e-4 || std::fabs(rotation - last_rot_) > 1e-3);
    last_lon_ = lon;
    last_lat_ = lat;
    last_zoom_r_ = zoom;
    last_rot_ = rotation;

    const Rot rot{std::cos(rotation), std::sin(rotation)};
    // px per world[0,1] at the PHYSICAL framebuffer (geographic zoom * HiDPI density).
    double scale_px = 256.0 * std::pow(2.0, zoom) * device_scale;
    // uOrigin folds in the live pan relative to the scene's build centre (kept small
    // for f32): screen(worldRel=0) = vp_centre - R*((view - build)*scale).
    double dcx = (vwx - scene_.cx) * scale_px;
    double dcy = (vwy - scene_.cy) * scale_px;
    double origin[2] = {w * 0.5 - (rot.c * dcx - rot.s * dcy),
                        h * 0.5 - (rot.s * dcx + rot.c * dcy)};
    // SCAMIN cull denominator (host display scale; 0 disables). cull_bias doublings.
    float cull_denom = (float)(scamin_display_denom * std::pow(2.0, cull_bias));
    if (scamin_display_denom <= 0.0)
        cull_denom = 0.0f;
    // Display-category mask (base always on; the engine also gates by disp_cat).
    uint32_t cat_mask = (m.display_base ? 1u : 0u) | (m.display_standard ? 2u : 0u) |
                        (m.display_other ? 4u : 0u);

    if (stencil_clip)
        host_stencil_mode_ = true;
    static const bool noss = std::getenv("TILE57_NOSS") != nullptr;
    const int kSS = ss_factor();
    bool ss = !noss && !host_stencil_mode_ && kSS > 1 && ensure_ss((int)w, (int)h) &&
              !(moving && pass != Pass::kText);

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

    // The host renders a base pass (everything but text) then a text pass, so text
    // lands above OpenCPN's own overlays; one scene serves both. uVp stays the PHYSICAL
    // viewport even under supersample — the kSS× GL viewport does the upscale, and the
    // screen-px transform (scale_px / origin) is in physical px.
    draw_scene(scale_px, origin, w, h, rot, cull_denom, cat_mask, pass);

    if (ss) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
        if (sc_on)
            glScissor(prev_sc[0], prev_sc[1], prev_sc[2], prev_sc[3]);
        composite_ss();
    }
    for (int i = 0; i < 8; ++i)
        glDisableVertexAttribArray(i);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void ChartRenderer::shutdown() {
    std::lock_guard<std::mutex> lk(portray_mu_);
    free_scene();
    scene_mhash_ = 0;
    gl_ready_ = false;
    prog_ = prog_pat_ = prog_sprite_ = prog_glyph_ = prog_blit_ = 0;
    vbo_quad_ = 0;
    have_range_ = false;
    // Do NOT reset g_prog: it is shared by every renderer and shutdown() runs on every
    // chart close (OpenCPN churns charts while quilting). A dead context is caught in
    // ensure_gl via glIsProgram.
    if (chart_) {
        tile57_chart_close(chart_);
        chart_ = nullptr;
    }
}

ChartRenderer::~ChartRenderer() { shutdown(); }

} // namespace t57
