// chart_renderer.h — draws a tile57 chart as a GPU vector chart.
//
// tile57 portrays a view ONCE (tile57_chart_surface) into a WORLD-SPACE
// tagged stream: area/line geometry in web-mercator [0,1]; point symbols and text
// as a world anchor + a local reference-px outline; each draw call tagged with
// its feature SCAMIN. We tessellate that once into a single vertex buffer and,
// every frame, transform it on the GPU and cull by SCAMIN — so pan and zoom
// re-portray NOTHING (like a native S57 SENC). Portrayal re-runs only when the
// view leaves the cached camera's margin/zoom band or mariner settings change.
//
// One vertex, one program: screen_px = aWorld * uScale + uOrigin + aPost.
//   aWorld  world offset from a per-chart reference point (f32; small -> precise)
//   aPost   a screen-space px offset applied AFTER the world transform: 0 for
//           areas, perp*halfwidth for lines, the local glyph/symbol px for
//           anchored symbols & text (so those stay a constant screen size).
//   aThresh SCAMIN cull: the vertex is dropped when the view zoom < aThresh.
#pragma once
#include "tile57.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace t57 {

class ChartRenderer {
  public:
    enum class Pass { kBase, kText, kAll };

    bool open_chart(const std::string& pmtiles_path); // baked .pmtiles archive
    bool ensure_gl();
    // Portray (if needed) for this view, then draw into the current framebuffer.
    // lon/lat = view centre, zoom = the GEOGRAPHIC (chart-scale) web-mercator zoom —
    // used for tile detail + the SCAMIN cull, so those track the physical chart scale
    // regardless of display density. w/h = the GL viewport (physical px). device_scale
    // = the framebuffer/logical px ratio (contentScale on HiDPI): the projection
    // scales by it so geometry fills the physical framebuffer while SCAMIN does not.
    // cull_bias = zoom levels subtracted from the SCAMIN cull zoom: enlarged symbols
    // (on HiDPI, or via TILE57_DECLUTTER) drop out that many mercator levels earlier.
    void render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                const tile57_mariner& m, Pass pass, bool stencil_clip, double device_scale = 1.0,
                double cull_bias = 0.0);
    void shutdown();
    bool has_chart() const { return chart_ != nullptr; }
    // True when the last render deferred work that a follow-up redraw would finish:
    // tiles left un-portrayed by the per-frame budget, OR a stale whole-view label
    // portray postponed until motion stops. The caller requests another redraw so the
    // view fills in / labels refresh (progressive fill instead of a one-frame freeze).
    bool tiles_pending() const { return tiles_pending_ || labels_pending_; }
    bool get_info(tile57_info& out) const;
    tile57_chart* chart_handle() const { return chart_; } // for object-query

    // Unified GPU vertex (see header note).
    struct Vtx {
        float wx, wy, px, py;
        uint8_t r, g, b, a;
        float thresh;
    };
    // Sprite vertex: world anchor + screen-px quad corner + atlas UV + SCAMIN.
    struct SpriteVtx {
        float wx, wy, px, py, u, v, thresh;
    };
    // Pattern vertex: world position + atlas cell rect + tile screen px + SCAMIN.
    struct PatVtx {
        float wx, wy, u0, v0, u1, v1, tw, th, thresh;
    };
    // Glyph vertex: world anchor + screen-px quad corner + SDF atlas UV + colour
    // + SCAMIN (text drawn as SDF quads from the glyph atlas).
    struct GlyphVtx {
        float wx, wy, px, py, u, v;
        uint8_t r, g, b, a;
        float thresh;
    };

    // Geometry sinks the C surface callbacks append to (public for the
    // trampolines). Grouped by paint layer so we draw area->line->symbol->text.
    std::vector<Vtx> area_, line_, symbol_, text_;
    std::vector<SpriteVtx> sprite_; // point symbols drawn from the atlas
    std::vector<PatVtx> pattern_;   // area fills tiled from the pattern atlas
    std::vector<GlyphVtx> glyph_;   // text drawn as SDF quads from the glyph atlas
    // The world reference point offsets are relative to (set per portrayal).
    double ref_wx_ = 0, ref_wy_ = 0;
    // Geometry decimation epsilon in world units (~half a portrayal pixel).
    double decimate_eps_ = 0;
    // Display scale (mariner size_scale) — pattern tile screen size.
    double size_scale_ = 1.0;
    // Callback handlers (world/local geometry -> Vtx).
    void on_fill_area(const tile57_world_rings* r, tile57_rgba c, float thresh);
    void on_stroke_line(const tile57_world_rings* l, float width_px, tile57_rgba c, float thresh);
    void on_draw_symbol(tile57_world_point anchor, const tile57_local_rings* r, tile57_rgba c,
                        int even_odd, float stroke_w, float thresh);
    void on_draw_text(tile57_world_point anchor, const tile57_local_rings* g, tile57_rgba c,
                      tile57_rgba halo, float thresh);
    void on_draw_sprite(const char* name, size_t len, tile57_world_point anchor, float rot_deg,
                        float half_w, float half_h, float thresh);
    void on_draw_pattern(const char* name, size_t len, const tile57_world_rings* rings,
                         float thresh);
    // Lay out `text` from the SDF glyph atlas at (anchor + origin px), size in px,
    // appending one quad per glyph to glyph_.
    void on_draw_text_str(tile57_world_point anchor, float ox, float oy, const char* text,
                          size_t len, float size_px, tile57_rgba color, tile57_rgba halo,
                          float thresh);

    // ---- tiled path (MapLibre model): portray+tessellate each baked tile ONCE,
    // cache its GPU geometry, compose the view from cached tiles. This is the only
    // render path. One tile's tessellated GPU geometry (its own VBOs) + the world
    // origin its verts are relative to (the tile's NW corner, so aWorld stays f32-tiny).
    struct TileGeom {
        uint32_t vbo[7] = {0, 0, 0, 0, 0, 0, 0}; // area,line,symbol,text,sprite,pat,glyph
        uint32_t n[7] = {0, 0, 0, 0, 0, 0, 0};
        double ref_wx = 0, ref_wy = 0;
        int64_t used_ms = 0; // last frame this tile was drawn (LRU evict)
    };

  private:
    void draw_range(uint32_t vbo, uint32_t count);        // Vtx layout (prog_)
    void draw_pat_range(uint32_t vbo, uint32_t count);    // PatVtx layout (prog_pat_)
    void draw_sprite_range(uint32_t vbo, uint32_t count); // SpriteVtx layout (prog_sprite_)
    void draw_glyph_range(uint32_t vbo, uint32_t count);  // GlyphVtx layout (prog_glyph_)
    void composite_ss(); // draw the resolved MSAA texture over OpenCPN's FBO

    // Tiled path helpers (chart_renderer.cpp).
    void render_tiled(uint32_t w, uint32_t h, const tile57_mariner& m, float cull_zoom, double vwx,
                      double vwy, double scale_px, int z, uint32_t x0, uint32_t x1, uint32_t y0,
                      uint32_t y1, int budget, int max_portray_ms);
    TileGeom& ensure_tile(int z, uint32_t x, uint32_t y, const tile57_mariner& m);
    void clear_tiles();
    void evict_lru(size_t cap); // drop least-recently-drawn tiles above `cap`
    // Whole-view label pass (shared declutter grid; fixes per-tile seam label drops).
    void portray_view_labels(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                             const tile57_mariner& m);
    void draw_view_labels(double scale_px, float cull_zoom, uint32_t w, uint32_t h, double vwx,
                          double vwy);
    static uint64_t tile_key(int z, uint32_t x, uint32_t y) {
        return ((uint64_t)(z & 0x1f) << 58) | ((uint64_t)(x & 0x1fffffff) << 29) | (y & 0x1fffffff);
    }

    tile57_chart* chart_ = nullptr;
    uint32_t prog_ = 0; // solid Vtx program (area/line/symbol/text tile layers)
    int u_scale_ = -1, u_origin_ = -1, u_vp_ = -1, u_zoom_ = -1;
    // Sprite (textured point symbols) program.
    uint32_t prog_sprite_ = 0;
    int su_scale_ = -1, su_origin_ = -1, su_vp_ = -1, su_zoom_ = -1, su_atlas_ = -1;
    // Pattern (tiled-texture area fills) program.
    uint32_t prog_pat_ = 0;
    int pu_scale_ = -1, pu_origin_ = -1, pu_vp_ = -1, pu_zoom_ = -1, pu_atlas_ = -1;
    // Glyph (SDF text) program.
    uint32_t prog_glyph_ = 0;
    int gu_scale_ = -1, gu_origin_ = -1, gu_vp_ = -1, gu_zoom_ = -1, gu_atlas_ = -1;
    // Composite program + fullscreen quad: draw the resolved MSAA texture (a
    // shared multisampled FBO, see chart_renderer.cpp) over OpenCPN's FBO. MSAA
    // antialiases tessellated text/lines/area edges at ~1x fill cost.
    uint32_t prog_blit_ = 0, vbo_quad_ = 0;
    int bu_tex_ = -1;
    bool gl_ready_ = false;

    bool have_range_ = false;
    double min_zoom_ = 0, max_zoom_ = 0;
    bool have_bounds_ = false;
    double b_w_ = 0, b_s_ = 0, b_e_ = 0, b_n_ = 0; // chart coverage bbox (deg)

    // Previous render's view centre/zoom — motion detection (gates the AA supersample).
    double last_lon_ = 1e9, last_lat_ = 1e9, last_zoom_r_ = 1e9;

    // Tiled path state (the only render path).
    std::unordered_map<uint64_t, TileGeom> tiles_; // (z,x,y) -> cached tile geometry
    uint64_t tiles_mhash_ = 0;                     // mariner hash the cache was portrayed at
    bool tiles_pending_ = false; // last render deferred some tiles (portray budget)
    bool host_stencil_mode_ =
        false; // host lacks FBOs (stencil-clips quilt) -> force direct render, no SS composite
    int backdrop_z_ =
        -1; // newest fully-cached zoom level, drawn under a still-loading zoom to avoid gray gaps
    // Whole-view label buffers (one declutter grid → no per-tile seam drops).
    uint32_t vbo_vtext_ = 0, vbo_vglyph_ = 0, n_vtext_ = 0, n_vglyph_ = 0;
    // Label cache: the whole-view label portray (pmtiles decode + declutter + soundings)
    // is the costliest per-frame item, so — like the geometry tiles — portray only on
    // settle / view-change and reuse the buffers (drawn with a transformed origin) during
    // a pan/zoom gesture. lbl_*_ is the view the cache was portrayed at; lbl_prev_*_ is the
    // previous text frame (settle detection, kept separate from last_*_ which both passes
    // stamp — so it would always read "still" in the text pass).
    bool labels_valid_ = false;
    bool labels_pending_ = false; // stale portray deferred until motion stops
    double lbl_lon_ = 1e9, lbl_lat_ = 1e9, lbl_zoom_ = 1e9;
    double lbl_prev_lon_ = 1e9, lbl_prev_lat_ = 1e9, lbl_prev_zoom_ = 1e9;
    double lbl_ref_wx_ = 0, lbl_ref_wy_ = 0; // cache's world reference (draw origin)
};

} // namespace t57
