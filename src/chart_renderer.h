// chart_renderer.h — draws a tile57 chart from a draw-ready GPU scene.
//
// tile57 portrays a whole view ONCE (tile57_chart_gpu_scene / _compose_gpu_scene)
// into DRAW-READY buffers: geometry already tessellated, already in S-52 paint
// order, split into ranges that each draw with one pipeline. The host owns no
// scene and knows no S-52: upload the three buffers (triangles: vertices+indices;
// sprites/SDF glyphs: quads) plus the two baked atlas textures, then walk the
// ranges in order and draw each. Every vertex is WORLD web-mercator [0,1]; the
// camera transforms it per frame (uScale/uOrigin/uRot), so pan/zoom/rotate
// re-portray NOTHING. The scene OVERSCANS the viewport; it rebuilds only when the
// view leaves that coverage margin/zoom band or the mariner settings change —
// the lookout-core model (see lookout-core/src/gpu.zig).
//
// Per-vertex `scamin` and `disp_cat` gates ride to the GPU and are culled in the
// vertex shader from uniforms (uScaminDenom, uCatMask), so category / SCAMIN
// changes never force a rebuild either.
#pragma once
#include "tile57.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace t57 {

// tile57 hands colours across the C ABI as a packed 0xRRGGBBAA scalar
// (tile57_color) rather than a 4-byte struct — passing a small struct by value
// across callconv(.c) is miscompiled by zig on aarch64 in optimized builds.
struct Rgba8 {
    unsigned char r, g, b, a;
};
static inline Rgba8 tile57_unpack(tile57_color c) {
    return Rgba8{(unsigned char)((c >> 24) & 0xFF), (unsigned char)((c >> 16) & 0xFF),
                 (unsigned char)((c >> 8) & 0xFF), (unsigned char)(c & 0xFF)};
}

class ChartRenderer {
  public:
    enum class Pass { kBase, kText, kAll };

    bool open_chart(const std::string& pmtiles_path); // baked .pmtiles archive
    bool ensure_gl();
    // Kept for the host's "Use SCAMIN" wiring in tile57_chart.cpp. The engine now
    // imputes a SCAMIN for ungated features from the cell's compilation scale, so
    // this only records intent; the live cull denominator arrives per render().
    void set_super_scamin(bool on, double native_scale);
    // Portray (if the view left the cached scene's coverage) then draw into the
    // current framebuffer. Args unchanged from the surface-callback renderer so
    // tile57_chart.cpp needs no change: lon/lat = view centre, zoom = GEOGRAPHIC
    // web-mercator zoom (tile detail + SCAMIN cull, tracks the physical chart
    // scale), w/h = the GL viewport (physical px), device_scale = framebuffer/
    // logical px ratio (HiDPI), cull_bias = extra SCAMIN thinning (levels),
    // rotation = ViewPort rotation (rad, course-up), scamin_display_denom = the
    // host's true display scale (1:N; 0 = cull off), patch_fb = the quilt patch
    // {x,y,w,h} in framebuffer px this chart owns (null = whole viewport).
    void render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                const tile57_mariner& m, Pass pass, bool stencil_clip, double device_scale = 1.0,
                double cull_bias = 0.0, double rotation = 0.0, double scamin_display_denom = 0.0,
                const int* patch_fb = nullptr);
    void shutdown();
    bool has_chart() const { return chart_ != nullptr; }
    // The lookout-core model rebuilds the whole scene synchronously on a coverage
    // miss, so nothing is ever deferred to a follow-up frame.
    bool tiles_pending() const { return false; }
    bool get_info(tile57_info& out) const;
    tile57_chart* chart_handle() const { return chart_; } // for object-query
    // Serializes every call into chart_ (the tile57 handle is not internally
    // synchronized): the render/build path and the object-query path both take it.
    std::mutex& portray_mutex() const { return portray_mu_; }
    // Retained for ABI parity with the host; the synchronous rebuild never calls it.
    void set_progress_callback(std::function<void()> cb) { on_progress_ = std::move(cb); }
    ~ChartRenderer();

  private:
    // The view rotation as (cos, sin) — the GPU transform's 2x2, y-down.
    struct Rot {
        double c = 1.0, s = 0.0;
    };

    // One contiguous draw out of the uploaded buffers, one pipeline. Copied from
    // tile57_gpu_range at build time (the scene is freed once uploaded).
    struct Range {
        uint32_t first = 0, count = 0;
        uint8_t kind = 0;              // tile57_gpu_kind (paint order is already the array order)
        uint8_t prim = 0;             // tile57_gpu_prim: TRIANGLES (indices) | QUADS (quad verts)
        uint8_t atlas = 0;            // tile57_gpu_atlas (QUADS only)
        uint32_t pattern = 0xFFFFFFFF; // index into pat_tex_, or TILE57_GPU_NO_PATTERN
        unsigned char color[4] = {0, 0, 0, 255}; // resolved range colour (flat fills / SDF tint)
    };

    // A whole view uploaded to GL: the three buffers, the per-range draw list, and
    // one texture per area-fill pattern cell. Rebuilt only on a coverage miss.
    struct Scene {
        uint32_t vbo = 0;    // tile57_gpu_vertex[]  (triangles)
        uint32_t ibo = 0;    // uint32 indices
        uint32_t qbo = 0;    // tile57_gpu_quad[] * 6 per quad (sprites / SDF glyphs)
        uint32_t quad_verts = 0; // total quad vertices in qbo (6 * quad_count)
        std::vector<Range> ranges;
        std::vector<uint32_t> pat_tex;             // GL texture per pattern cell
        std::vector<std::pair<int, int>> pat_wh;   // each cell's device-px period (w,h)
        bool ok = false;
        // Coverage: the view this scene was portrayed for. A rebuild is needed once
        // the live view pans/zooms out of the overscanned world box, or the zoom
        // drifts past ZOOM_REBUILD (see render()).
        double cx = 0, cy = 0;    // build view centre, world [0,1]
        double build_zoom = 0;    // zoom the scene was portrayed at
        double half_wx = 0, half_wy = 0; // overscanned world half-extents covered
    };

    // Build (portray + upload) a fresh scene for this view; frees any prior one.
    void build_scene(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                     const tile57_mariner& m);
    void free_scene();
    // Does the cached scene still cover this view (within the overscan margin and
    // zoom band)? False forces a rebuild.
    bool scene_covers(double vwx, double vwy, double zoom) const;
    // Walk the scene's ranges in paint order, drawing each with its pipeline. The
    // per-frame transform (scale_px / origin / rot) and the live SCAMIN / category
    // gates ride the uniforms; nothing here is rebuilt on a pan.
    void draw_scene(double scale_px, const double origin[2], uint32_t vw, uint32_t vh, Rot rot,
                    float cull_denom, uint32_t cat_mask, Pass pass);
    void draw_range(const Range& r);         // flat triangles (area / line)
    void draw_pat_range(const Range& r);     // pattern-tiled triangles
    void draw_quad_range(const Range& r);    // sprite or SDF glyph quads
    void composite_ss();                     // draw the resolved SS texture over OpenCPN's FBO

    tile57_chart* chart_ = nullptr;
    Scene scene_;
    uint64_t scene_mhash_ = 0; // mariner hash the scene was portrayed at (rebuild on change)

    // Shared GL programs (adopted from the process-wide g_prog in ensure_gl).
    // Flat triangles (area + line): world transform, per-RANGE flat colour, SCAMIN
    // + category gate.
    uint32_t prog_ = 0;
    int u_scale_ = -1, u_origin_ = -1, u_vp_ = -1, u_denom_ = -1, u_rot_ = -1, u_cat_ = -1,
        u_color_ = -1;
    // Pattern-tiled area fills (per-range cell texture, world-anchored tiling).
    uint32_t prog_pat_ = 0;
    int pu_scale_ = -1, pu_origin_ = -1, pu_vp_ = -1, pu_denom_ = -1, pu_rot_ = -1, pu_cat_ = -1,
        pu_cell_ = -1, pu_period_ = -1, pu_worg_ = -1;
    // Sprite (textured point symbols / soundings) — quads, atlas artwork.
    uint32_t prog_sprite_ = 0;
    int su_scale_ = -1, su_origin_ = -1, su_vp_ = -1, su_denom_ = -1, su_atlas_ = -1, su_rot_ = -1,
        su_cat_ = -1;
    // SDF text — quads, glyph atlas, per-vertex tint + embolden weight.
    uint32_t prog_glyph_ = 0;
    int gu_scale_ = -1, gu_origin_ = -1, gu_vp_ = -1, gu_denom_ = -1, gu_atlas_ = -1, gu_rot_ = -1,
        gu_cat_ = -1;
    // Composite (draw the resolved supersample texture over OpenCPN's FBO).
    uint32_t prog_blit_ = 0, vbo_quad_ = 0;
    int bu_tex_ = -1;
    bool gl_ready_ = false;

    bool have_range_ = false;
    double min_zoom_ = 0, max_zoom_ = 0;
    bool have_bounds_ = false;
    double b_w_ = 0, b_s_ = 0, b_e_ = 0, b_n_ = 0; // chart coverage bbox (deg)

    // Motion detection (gates the supersample AA: AA when settled, direct while moving).
    double last_lon_ = 1e9, last_lat_ = 1e9, last_zoom_r_ = 1e9, last_rot_ = 1e9;
    bool host_stencil_mode_ = false; // host lacks FBOs -> direct render, no SS composite

    // Kept for set_super_scamin (see above); the live cull rides render()'s args.
    double native_scale_ = 0.0;
    bool super_scamin_ = true;

    mutable std::mutex portray_mu_;     // chart_ : one user at a time (render vs object-query)
    std::function<void()> on_progress_; // unused in the synchronous model; kept for the setter
};

} // namespace t57
