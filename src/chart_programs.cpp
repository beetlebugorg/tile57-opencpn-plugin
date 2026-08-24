// chart_programs.cpp - see chart_programs.h.
#include "chart_programs.h"
#include "tile57.h"
#include <cstddef>
#include <string>
#include <wx/log.h>

namespace t57 {

// The attribute pointers below use these byte offsets.
static_assert(sizeof(tile57_gpu_vertex) == 32, "tile57_gpu_vertex layout changed");
static_assert(offsetof(tile57_gpu_vertex, ox) == 8, "tile57_gpu_vertex layout changed");
static_assert(offsetof(tile57_gpu_vertex, scamin) == 16, "tile57_gpu_vertex layout changed");
static_assert(offsetof(tile57_gpu_vertex, disp_cat) == 20, "tile57_gpu_vertex layout changed");
static_assert(offsetof(tile57_gpu_vertex, map_align) == 21, "tile57_gpu_vertex layout changed");
static_assert(offsetof(tile57_gpu_vertex, color) == 24, "tile57_gpu_vertex layout changed");
static_assert(offsetof(tile57_gpu_vertex, depth) == 28, "tile57_gpu_vertex layout changed");
static_assert(sizeof(tile57_gpu_quad) == 44, "tile57_gpu_quad layout changed");
static_assert(offsetof(tile57_gpu_quad, u) == 16, "tile57_gpu_quad layout changed");
static_assert(offsetof(tile57_gpu_quad, color) == 24, "tile57_gpu_quad layout changed");
static_assert(offsetof(tile57_gpu_quad, weight) == 28, "tile57_gpu_quad layout changed");
static_assert(offsetof(tile57_gpu_quad, scamin) == 32, "tile57_gpu_quad layout changed");
static_assert(offsetof(tile57_gpu_quad, disp_cat) == 36, "tile57_gpu_quad layout changed");
static_assert(offsetof(tile57_gpu_quad, flip) == 38, "tile57_gpu_quad layout changed");
static_assert(offsetof(tile57_gpu_quad, tangent_q) == 39, "tile57_gpu_quad layout changed");
static_assert(offsetof(tile57_gpu_quad, depth) == 40, "tile57_gpu_quad layout changed");

namespace {

enum Attr : uint32_t {
    kWorld = 0,
    kPost = 1,
    kScamin = 2,
    kDispCat = 3,
    kMapAlign = 4,
    kColor = 5,
    kDepth = 6,
    kUV = 7,
    kWeight = 8,
    kFlipTangent = 9,
    kAttrCount = 10
};

// ---- GLSL 1.20 sources -------------------------------------------------------------
// One transform, shared by every pipeline:
//   screen_px = R * (aWorld * uScale) + uOrigin + post
// aWorld is world position relative to the scene's build center. post is the vertex's
// screen-space offset in device px (a line half-width, a glyph or symbol corner); a
// map-aligned post turns with the chart, a viewport-aligned one stays upright. Depth
// is tile57's paint-order key mapped to NDC z; later paint is nearer.
//
// The visibility gate moves a vertex outside the clip volume when its display
// category is off, or its SCAMIN is finer than the live display scale. The base
// category is never SCAMIN-culled.
#define T57_COMMON                                                                                  \
    "uniform float uScale; uniform vec2 uOrigin; uniform vec2 uVp; uniform vec2 uRot;\n"           \
    "uniform float uScaminDenom; uniform vec3 uCat;\n"                                             \
    "vec2 t57rot(vec2 p){ return vec2(p.x*uRot.x - p.y*uRot.y, p.x*uRot.y + p.y*uRot.x); }\n"       \
    "bool t57cull(float dispCat, float scamin){\n"                                                  \
    "  float catv = dispCat < 0.5 ? uCat.x : (dispCat < 1.5 ? uCat.y : uCat.z);\n"                  \
    "  return (catv < 0.5) || (scamin > 0.0 && dispCat > 0.5 && uScaminDenom > scamin);\n"          \
    "}\n"                                                                                           \
    "vec4 t57clip(vec2 s, float depth){\n"                                                          \
    "  return vec4(s.x/uVp.x*2.0-1.0, 1.0 - s.y/uVp.y*2.0, depth*2.0-1.0, 1.0);\n"                  \
    "}\n"

const char* kFlatVS = T57_COMMON
    "attribute vec2 aWorld; attribute vec2 aPost; attribute float aScamin;\n"
    "attribute float aDispCat; attribute float aMapAlign; attribute vec4 aColor;\n"
    "attribute float aDepth;\n"
    "varying vec4 vCol;\n"
    "void main(){\n"
    "  if (t57cull(aDispCat, aScamin)) { gl_Position = vec4(2.0); vCol = vec4(0.0); return; }\n"
    "  vec2 post = (aMapAlign > 0.5) ? t57rot(aPost) : aPost;\n"
    "  gl_Position = t57clip(t57rot(aWorld*uScale) + uOrigin + post, aDepth);\n"
    "  vCol = aColor;\n"
    "}\n";
const char* kFlatFS = "varying vec4 vCol;\n"
                      "void main(){ gl_FragColor = vec4(vCol.rgb*vCol.a, vCol.a); }\n";

// Pattern fill: the polygon interior, tiled per fragment at a constant screen size and
// anchored to the CHART. vWorldPx is unrotated camera-relative screen px, so the lattice
// turns rigidly with the chart and holds still under a pan. A varying rather than
// gl_FragCoord keeps the phase right inside the supersample target.
const char* kPatternVS = T57_COMMON
    "attribute vec2 aWorld; attribute float aScamin; attribute float aDispCat;\n"
    "attribute float aDepth;\n"
    "varying vec2 vWorldPx;\n"
    "void main(){\n"
    "  if (t57cull(aDispCat, aScamin)) { gl_Position = vec4(2.0); vWorldPx = vec2(0.0); return; }\n"
    "  vec2 wpx = aWorld*uScale;\n"
    "  gl_Position = t57clip(t57rot(wpx) + uOrigin, aDepth);\n"
    "  vWorldPx = wpx;\n"
    "}\n";
const char* kPatternFS = "uniform sampler2D uTex; uniform vec2 uPeriod;\n"
                         "varying vec2 vWorldPx;\n"
                         "void main(){\n"
                         "  vec2 uv = fract(vWorldPx / max(uPeriod, vec2(1.0)));\n"
                         "  vec4 c = texture2D(uTex, uv);\n"
                         "  if (c.a < 0.02) discard;\n"
                         "  gl_FragColor = vec4(c.rgb*c.a, c.a);\n"
                         "}\n";

// Quads: a symbol sprite or an SDF glyph. The anchor rides the chart; the corner offset
// keeps a fixed screen size. A run flagged `flip` (a depth-contour value laid along its
// contour) turns 180° about its anchor whenever its tangent, after the view rotation,
// would read into the screen's left half-plane, so the number is never upside down.
const char* kQuadVS = T57_COMMON
    "attribute vec2 aWorld; attribute vec2 aPost; attribute vec2 aUV; attribute vec4 aColor;\n"
    "attribute float aWeight; attribute float aScamin; attribute float aDispCat;\n"
    "attribute float aMapAlign; attribute vec2 aFlipTangent; attribute float aDepth;\n"
    "varying vec2 vUV; varying vec4 vCol; varying float vWeight;\n"
    "void main(){\n"
    "  if (t57cull(aDispCat, aScamin)) {\n"
    "    gl_Position = vec4(2.0); vUV = vec2(0.0); vCol = vec4(0.0); vWeight = 0.0; return;\n"
    "  }\n"
    "  vec2 post = aPost;\n"
    "  if (aFlipTangent.x > 0.5) {\n"
    "    float t = aFlipTangent.y / 256.0 * 6.28318530718;\n"
    "    if (cos(t)*uRot.x - sin(t)*uRot.y < 0.0) post = -post;\n"
    "  }\n"
    "  if (aMapAlign > 0.5) post = t57rot(post);\n"
    "  gl_Position = t57clip(t57rot(aWorld*uScale) + uOrigin + post, aDepth);\n"
    "  vUV = aUV; vCol = aColor; vWeight = aWeight;\n"
    "}\n";
const char* kSpriteFS = "uniform sampler2D uTex;\n"
                        "varying vec2 vUV; varying vec4 vCol; varying float vWeight;\n"
                        "void main(){\n"
                        "  vec4 t = texture2D(uTex, vUV) * vCol;\n"
                        "  if (t.a < 1.0/255.0) discard;\n"
                        "  gl_FragColor = vec4(t.rgb*t.a, t.a);\n"
                        "}\n";
// SDF text: antialias the distance field with its screen-space derivative. vWeight is
// the halo width in field units (0 = none); the halo is drawn in the palette's
// background colour (uHalo) so it lifts a name off busy soundings without glaring at
// night.
const char* kSdfFS = "uniform sampler2D uTex; uniform vec4 uHalo;\n"
                     "varying vec2 vUV; varying vec4 vCol; varying float vWeight;\n"
                     "void main(){\n"
                     "  float d = texture2D(uTex, vUV).r;\n"
                     "  float w = fwidth(d);\n"
                     "  float a = smoothstep(0.5 - w, 0.5 + w, d);\n"
                     "  if (vWeight > 0.0) {\n"
                     "    float halo = smoothstep(0.5 - vWeight - w, 0.5 - vWeight + w, d);\n"
                     "    float cov = max(a, halo);\n"
                     "    if (cov <= 0.0) discard;\n"
                     "    float al = cov * vCol.a;\n"
                     "    gl_FragColor = vec4(mix(uHalo.rgb, vCol.rgb, a) * al, al);\n"
                     "    return;\n"
                     "  }\n"
                     "  if (a <= 0.0) discard;\n"
                     "  float al = vCol.a * a;\n"
                     "  gl_FragColor = vec4(vCol.rgb * al, al);\n"
                     "}\n";

const char* kBlitVS = "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex;\n"
                      "void main(){ vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
const char* kBlitFS = "uniform sampler2D uTex; varying vec2 vTex;\n"
                      "void main(){ gl_FragColor = texture2D(uTex, vTex); }\n";

uint32_t compile(GLenum type, const char* body, bool& ok) {
    std::string src = std::string(T57_GLSL_VERSION) + body;
    const char* s = src.c_str();
    uint32_t sh = glCreateShader(type);
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        wxLogMessage("tile57: shader compile failed: %s", log);
        ok = false;
    }
    return sh;
}

uint32_t link(const char* vs, const char* fs, bool& ok) {
    uint32_t p = glCreateProgram();
    uint32_t v = compile(GL_VERTEX_SHADER, vs, ok);
    uint32_t f = compile(GL_FRAGMENT_SHADER, fs, ok);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, kWorld, "aWorld");
    glBindAttribLocation(p, kPost, "aPost");
    glBindAttribLocation(p, kScamin, "aScamin");
    glBindAttribLocation(p, kDispCat, "aDispCat");
    glBindAttribLocation(p, kMapAlign, "aMapAlign");
    glBindAttribLocation(p, kColor, "aColor");
    glBindAttribLocation(p, kDepth, "aDepth");
    glBindAttribLocation(p, kUV, "aUV");
    glBindAttribLocation(p, kWeight, "aWeight");
    glBindAttribLocation(p, kFlipTangent, "aFlipTangent");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint status = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        wxLogMessage("tile57: program link failed: %s", log);
        ok = false;
    }
    return p;
}

} // namespace

ChartPrograms& ChartPrograms::shared() {
    static ChartPrograms p;
    return p;
}

ChartPrograms::Program& ChartPrograms::program(Pipe p) {
    switch (p) {
    case Pipe::kPattern:
        return pattern_;
    case Pipe::kSprite:
        return sprite_;
    case Pipe::kSdf:
        return sdf_;
    default:
        return flat_;
    }
}

bool ChartPrograms::ensure() {
    // A program id that GL no longer recognises means the context it was made in is
    // gone (plugin reload); build afresh in the live one.
    if (ready_ && !glIsProgram(flat_.id)) {
        blit_quad_.release();
        ready_ = false;
    }
    return ready_ || build();
}

bool ChartPrograms::build() {
    bool ok = true;
    auto setup = [&](Program& prog, const char* vs, const char* fs) {
        prog.id = link(vs, fs, ok);
        prog.u_scale = glGetUniformLocation(prog.id, "uScale");
        prog.u_origin = glGetUniformLocation(prog.id, "uOrigin");
        prog.u_vp = glGetUniformLocation(prog.id, "uVp");
        prog.u_rot = glGetUniformLocation(prog.id, "uRot");
        prog.u_denom = glGetUniformLocation(prog.id, "uScaminDenom");
        prog.u_cat = glGetUniformLocation(prog.id, "uCat");
        prog.u_tex = glGetUniformLocation(prog.id, "uTex");
        prog.u_period = glGetUniformLocation(prog.id, "uPeriod");
        prog.u_halo = glGetUniformLocation(prog.id, "uHalo");
    };
    setup(flat_, kFlatVS, kFlatFS);
    setup(pattern_, kPatternVS, kPatternFS);
    setup(sprite_, kQuadVS, kSpriteFS);
    setup(sdf_, kQuadVS, kSdfFS);

    blit_prog_ = link(kBlitVS, kBlitFS, ok);
    blit_tex_ = glGetUniformLocation(blit_prog_, "uTex");
    const float quad[] = {-1, -1, 0, 0, 1, -1, 1, 0, 1,  1, 1, 1,
                          -1, -1, 0, 0, 1, 1,  1, 1, -1, 1, 0, 1};
    blit_quad_.gen();
    glBindBuffer(GL_ARRAY_BUFFER, blit_quad_.id());
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (!ok)
        destroy();
    ready_ = ok;
    return ok;
}

void ChartPrograms::destroy() {
    for (Program* p : {&flat_, &pattern_, &sprite_, &sdf_}) {
        if (p->id)
            glDeleteProgram(p->id);
        *p = Program{};
    }
    if (blit_prog_)
        glDeleteProgram(blit_prog_);
    blit_prog_ = 0;
    blit_quad_.reset();
    ready_ = false;
}

void ChartPrograms::use(Pipe pipe, const FrameUniforms& f) {
    const Program& p = program(pipe);
    glUseProgram(p.id);
    glUniform1f(p.u_scale, f.scale);
    glUniform2fv(p.u_origin, 1, f.origin);
    glUniform2fv(p.u_vp, 1, f.viewport);
    glUniform2fv(p.u_rot, 1, f.rot);
    glUniform1f(p.u_denom, f.scamin_denom);
    glUniform3fv(p.u_cat, 1, f.cat);
    if (p.u_tex >= 0)
        glUniform1i(p.u_tex, 0);
}

void ChartPrograms::set_pattern_cell(uint32_t tex, float period_w, float period_h) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    const float period[2] = {period_w, period_h};
    glUniform2fv(pattern_.u_period, 1, period);
}

void ChartPrograms::set_atlas(uint32_t tex) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
}

void ChartPrograms::set_halo(const float rgba[4]) { glUniform4fv(sdf_.u_halo, 1, rgba); }

void ChartPrograms::bind_triangle_stream(uint32_t vbo, uint32_t ibo) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    const GLsizei stride = sizeof(tile57_gpu_vertex);
    auto at = [](size_t off) { return reinterpret_cast<const void*>(off); };
    glVertexAttribPointer(kWorld, 2, GL_FLOAT, GL_FALSE, stride, at(0));
    glVertexAttribPointer(kPost, 2, GL_FLOAT, GL_FALSE, stride, at(8));
    glVertexAttribPointer(kScamin, 1, GL_FLOAT, GL_FALSE, stride, at(16));
    glVertexAttribPointer(kDispCat, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, at(20));
    glVertexAttribPointer(kMapAlign, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, at(21));
    glVertexAttribPointer(kColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(24));
    glVertexAttribPointer(kDepth, 1, GL_FLOAT, GL_FALSE, stride, at(28));
    for (uint32_t a = kWorld; a <= kDepth; ++a)
        glEnableVertexAttribArray(a);
    for (uint32_t a = kUV; a < kAttrCount; ++a)
        glDisableVertexAttribArray(a);
}

void ChartPrograms::bind_quad_stream(uint32_t qbo) {
    glBindBuffer(GL_ARRAY_BUFFER, qbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    const GLsizei stride = sizeof(tile57_gpu_quad);
    auto at = [](size_t off) { return reinterpret_cast<const void*>(off); };
    glVertexAttribPointer(kWorld, 2, GL_FLOAT, GL_FALSE, stride, at(0));
    glVertexAttribPointer(kPost, 2, GL_FLOAT, GL_FALSE, stride, at(8));
    glVertexAttribPointer(kUV, 2, GL_FLOAT, GL_FALSE, stride, at(16));
    glVertexAttribPointer(kColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(24));
    glVertexAttribPointer(kWeight, 1, GL_FLOAT, GL_FALSE, stride, at(28));
    glVertexAttribPointer(kScamin, 1, GL_FLOAT, GL_FALSE, stride, at(32));
    glVertexAttribPointer(kDispCat, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, at(36));
    glVertexAttribPointer(kMapAlign, 1, GL_UNSIGNED_BYTE, GL_FALSE, stride, at(37));
    glVertexAttribPointer(kFlipTangent, 2, GL_UNSIGNED_BYTE, GL_FALSE, stride, at(38));
    glVertexAttribPointer(kDepth, 1, GL_FLOAT, GL_FALSE, stride, at(40));
    for (uint32_t a = 0; a < kAttrCount; ++a)
        glEnableVertexAttribArray(a);
}

void ChartPrograms::unbind_streams() {
    for (uint32_t a = 0; a < kAttrCount; ++a)
        glDisableVertexAttribArray(a);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void ChartPrograms::blit(uint32_t tex) {
    glUseProgram(blit_prog_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(blit_tex_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, blit_quad_.id());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

} // namespace t57
