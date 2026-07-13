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
//   aScamin SCAMIN cull: the vertex is dropped when the view zoom < aScamin.
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
    // SUPER-SCAMIN: impute a SCAMIN for features the ENC left ungated, from this cell's
    // compilation scale (see feature_scamin). `native_scale` is the cell's 1:N denominator.
    // Changing either clears the tile cache — the value is baked into the vertex buffers.
    void set_super_scamin(bool on, double native_scale);
    // OpenCPN's display category + its separate "show soundings" switch. S-52 files soundings
    // under the OTHER category, but OpenCPN (like every ECDIS) lets you show them WITHOUT
    // turning that whole category on. We portray with display_other=true so soundings exist,
    // then drop the rest of OTHER here — tile57 tags every feature with its category, so this
    // is one test, not a class list. Changing it clears the tile cache (it is baked in).
    void set_display_filter(bool other_category, bool soundings);
    // Should this feature be emitted at all? (The category filter above.)
    bool feature_visible(const tile57_feature* f) const;
    // The 1:N denominator this feature is culled at (authored SCAMIN, imputed super-SCAMIN,
    // or "never"). Public because the C surface trampolines route every draw call through it.
    float feature_scamin(const tile57_feature* f) const;
    // Portray (if needed) for this view, then draw into the current framebuffer.
    // lon/lat = view centre, zoom = the GEOGRAPHIC (chart-scale) web-mercator zoom —
    // used for tile detail + the SCAMIN cull, so those track the physical chart scale
    // regardless of display density. w/h = the GL viewport (physical px). device_scale
    // = the framebuffer/logical px ratio (contentScale on HiDPI): the projection
    // scales by it so geometry fills the physical framebuffer while SCAMIN does not.
    // scamin_display_denom = the host's TRUE display scale denominator (OpenCPN's
    // vp.chart_scale). Each feature's SCAMIN rides to the GPU as itself and is compared
    // against this, exactly as native OpenCPN does — no zoom-threshold formula, so no
    // equator assumption. 0 disables the cull ("Use SCAMIN" off).
    // cull_bias = zoom levels of extra culling (TILE57_DECLUTTER opt-in, normally 0);
    // one level = one doubling of the denominator, applied to every layer alike so a
    // feature's symbol and label always hide at the same zoom.
    // rotation = OpenCPN's ViewPort rotation in radians (course-up etc; 0 = north-up),
    // applied about the viewport centre on the GPU. It is a pure VIEW transform: the
    // portrayed geometry stays north-up in world space, so the tile + label caches
    // survive a turn untouched and rotating re-portrays nothing.
    // patch_fb = the quilt patch this chart owns, {x, y, w, h} in framebuffer px (top-left
    // origin, like the shader's screen frame); null = the whole viewport. Tiles outside it
    // can never appear on screen, so they are never portrayed — without this every cell in a
    // quilt tessellates the whole canvas and the scissor discards nearly all of it.
    void render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                const tile57_mariner& m, Pass pass, bool stencil_clip, double device_scale = 1.0,
                double cull_bias = 0.0, double rotation = 0.0, double scamin_display_denom = 0.0,
                const int* patch_fb = nullptr);
    void shutdown();
    bool has_chart() const { return chart_ != nullptr; }
    // True when the last render deferred work that a follow-up redraw would finish:
    // tiles left un-portrayed by the per-frame budget, OR a stale whole-view label
    // portray postponed until motion stops. The caller requests another redraw so the
    // view fills in / labels refresh (progressive fill instead of a one-frame freeze).
    bool tiles_pending() const { return tiles_pending_ || labels_pending_; }
    bool get_info(tile57_info& out) const;
    tile57_chart* chart_handle() const { return chart_; } // for object-query

    // postrot (on Vtx/SpriteVtx/GlyphVtx): does this vertex's screen-px offset (px,py) turn
    // with the chart under a rotated view? 1 = yes (MAP-aligned), 0 = no (VIEWPORT-aligned,
    // stays upright on screen). It is PER-VERTEX and not a per-draw uniform because tile57
    // reports rotation-alignment per FEATURE: one tile's symbol/sprite/text buffer mixes
    // MAP-aligned marks (an ORIENT'd light, a traffic-lane arrow, a depth-contour value that
    // must follow its contour) with VIEWPORT-aligned ones (a buoy, an ordinary label), so a
    // single uniform for the whole buffer cannot express it. Lines are always 1 (their aPost
    // is a half-width normal taken from the segment's WORLD direction); areas are always 0
    // (their aPost is zero anyway).

    // Unified GPU vertex (see header note).
    struct Vtx {
        float wx, wy, px, py;
        uint8_t r, g, b, a;
        float scamin, postrot;
    };
    // Sprite vertex: world anchor + screen-px quad corner + atlas UV + SCAMIN.
    struct SpriteVtx {
        float wx, wy, px, py, u, v, scamin, postrot;
    };
    // Pattern vertex: world position + atlas cell rect + tile screen px + SCAMIN.
    // (No postrot: a pattern has no local px offset — the fill lattice rides the chart.)
    struct PatVtx {
        float wx, wy, u0, v0, u1, v1, tw, th, scamin;
    };
    // Glyph vertex: world anchor + screen-px quad corner + SDF atlas UV + colour
    // + SCAMIN (text drawn as SDF quads from the glyph atlas).
    struct GlyphVtx {
        float wx, wy, px, py, u, v;
        uint8_t r, g, b, a;
        float scamin, postrot;
    };

    // Geometry sinks the C surface callbacks append to (public for the
    // trampolines). Grouped by paint layer so we draw area->line->symbol->text.
    std::vector<Vtx> area_, line_, symbol_, text_;
    std::vector<SpriteVtx> sprite_; // point symbols drawn from the atlas
    std::vector<PatVtx> pattern_;   // area fills tiled from the pattern atlas
    std::vector<GlyphVtx> glyph_;   // text drawn as SDF quads from the glyph atlas
    // The S-52 draw priority of every vertex above, in step with it (one entry per vertex).
    // tile57 tags each feature with `plane` and leaves the paint order to us — batching by
    // geometry KIND (all sprites after all symbols) ignores it, which is what put soundings
    // on top of the symbols they should sit under. Portray records it, ensure_tile sorts by
    // it. Scratch only: it does not ride to the GPU (the sort makes it implicit).
    std::vector<uint8_t> area_pl_, line_pl_, symbol_pl_, text_pl_, sprite_pl_, pattern_pl_,
        glyph_pl_;
    int cur_plane_ = 0; // the feature being portrayed (set per draw call by the trampolines)
    void set_plane(const tile57_feature* f) { cur_plane_ = f ? (int)f->plane : 0; }
    // Tag the vertices [from, to) of `planes`' buffer with the current feature's priority.
    void tag_plane(std::vector<uint8_t>& planes, size_t from, size_t to) {
        planes.resize(from); // portray clears verts + planes together; keep them in lockstep
        planes.resize(to, (uint8_t)std::min(std::max(cur_plane_, 0), 255));
    }
    // The world reference point offsets are relative to (set per portrayal).
    double ref_wx_ = 0, ref_wy_ = 0;
    // Geometry decimation epsilon in world units (~half a portrayal pixel).
    double decimate_eps_ = 0;
    // Display scale (mariner size_scale) — pattern tile screen size.
    double size_scale_ = 1.0;
    // Super-SCAMIN state (see feature_scamin / set_super_scamin): the cell's compilation
    // scale, and whether to impute a SCAMIN from it for features the ENC left ungated.
    double native_scale_ = 0.0;
    bool super_scamin_ = true;
    bool other_category_ = false; // the mariner asked for the whole OTHER category
    bool soundings_ = true;       // ...or just its soundings (OpenCPN's own switch)
    // Callback handlers (world/local geometry -> Vtx). `align` is tile57's per-feature
    // rotation-alignment; it becomes the vertex's postrot (see the note above).
    void on_fill_area(const tile57_world_rings* r, tile57_rgba c, float scamin);
    void on_stroke_line(const tile57_world_rings* l, float width_px, tile57_rgba c, float scamin);
    void on_draw_symbol(tile57_world_point anchor, const tile57_local_rings* r, tile57_rgba c,
                        int even_odd, float stroke_w, tile57_rot_align align, float scamin);
    void on_draw_text(tile57_world_point anchor, const tile57_local_rings* g, tile57_rgba c,
                      tile57_rgba halo, tile57_rot_align align, float scamin);
    void on_draw_sprite(const char* name, size_t len, tile57_world_point anchor, float rot_deg,
                        tile57_rot_align align, float half_w, float half_h, float scamin);
    void on_draw_pattern(const char* name, size_t len, const tile57_world_rings* rings,
                         float scamin);
    // Lay out `text` from the SDF glyph atlas at (anchor + origin px), size in px, the whole
    // run turned by rot_deg about the anchor (a depth-contour value arrives with its
    // contour's tangent), appending one quad per glyph to glyph_.
    void on_draw_text_str(tile57_world_point anchor, float ox, float oy, const char* text,
                          size_t len, float size_px, float rot_deg, tile57_rot_align align,
                          tile57_rgba color, tile57_rgba halo, float scamin);

    // ---- tiled path (MapLibre model): portray+tessellate each baked tile ONCE,
    // cache its GPU geometry, compose the view from cached tiles. This is the only
    // render path. One tile's tessellated GPU geometry (its own VBOs) + the world
    // origin its verts are relative to (the tile's NW corner, so aWorld stays f32-tiny).
    // A run of triangles sharing one S-52 draw priority, inside a kind's VBO. The tile's
    // vertices are SORTED by priority at portray time, so a priority is one contiguous range
    // and the paint loop can walk priorities without touching the data again.
    struct Range {
        uint8_t plane;          // S-52 draw priority (the paint order key)
        uint32_t first, count;  // vertices, within this kind's VBO
    };
    struct TileGeom {
        uint32_t vbo[7] = {0, 0, 0, 0, 0, 0, 0}; // area,line,symbol,text,sprite,pat,glyph
        uint32_t n[7] = {0, 0, 0, 0, 0, 0, 0};
        std::vector<Range> ranges[7]; // ascending by plane; empty kinds stay empty
        double ref_wx = 0, ref_wy = 0;
        int64_t used_ms = 0; // last frame this tile was drawn (LRU evict)
    };

  private:
    // Draw [first, first+count) vertices of a VBO. The range is how one S-52 draw priority is
    // painted out of a tile whose triangles are sorted by priority (see sort_by_plane).
    void draw_range(uint32_t vbo, uint32_t first, uint32_t count);        // Vtx (prog_)
    void draw_pat_range(uint32_t vbo, uint32_t first, uint32_t count);    // PatVtx (prog_pat_)
    void draw_sprite_range(uint32_t vbo, uint32_t first, uint32_t count); // SpriteVtx
    void draw_glyph_range(uint32_t vbo, uint32_t first, uint32_t count);  // GlyphVtx
    void composite_ss(); // draw the resolved MSAA texture over OpenCPN's FBO

    // The view rotation as (cos, sin) — the GPU transform's 2x2, y-down (see VS).
    struct Rot {
        double c = 1.0, s = 0.0;
    };
    // Half-extents (world units) of the AABB that CONTAINS the rotated view rect. Turning
    // the view sweeps the screen rect over a bigger world box, so tile enumeration (and the
    // label portray) must cover |w·cos|+|h·sin| by |w·sin|+|h·cos| — at 45° that is ~1.41x
    // each way — or the corners the turn exposes come up empty. `slack` (radians) widens
    // the box to also contain every rotation within ±slack: sin/cos are 1-Lipschitz, so
    // padding |cos| and |sin| by slack is a sound bound. slack=0 (north-up, and the tile
    // path) reproduces the old w/2, h/2 exactly.
    static void rotated_half_extent(uint32_t w, uint32_t h, double scale_px, Rot rot,
                                    double& half_wx, double& half_wy, double slack = 0.0);
    // Tiled path helpers (chart_renderer.cpp).
    void render_tiled(uint32_t w, uint32_t h, const tile57_mariner& m, float cull_denom, double vwx,
                      double vwy, double scale_px, int z, uint32_t x0, uint32_t x1, uint32_t y0,
                      uint32_t y1, int budget, int max_portray_ms, Rot rot);
    TileGeom& ensure_tile(int z, uint32_t x, uint32_t y, const tile57_mariner& m);
    void clear_tiles();
    void evict_lru(size_t cap); // drop least-recently-drawn tiles above `cap`
    // Whole-view label pass (shared declutter grid; fixes per-tile seam label drops).
    // rotation: handed to tile57 for the VIEW pass only — it decides the declutter frame and
    // keeps a tangent-rotated contour value from reading upside down. (The TILE pass gets no
    // rotation: its geometry is cached and must stay north-up, per tile57_chart_tile_surface.)
    void portray_view_labels(double lon, double lat, double zoom, double rotation, uint32_t w,
                             uint32_t h, const tile57_mariner& m);
    void draw_view_labels(double scale_px, float cull_denom, uint32_t w, uint32_t h, double vwx,
                          double vwy, Rot rot);
    static uint64_t tile_key(int z, uint32_t x, uint32_t y) {
        return ((uint64_t)(z & 0x1f) << 58) | ((uint64_t)(x & 0x1fffffff) << 29) | (y & 0x1fffffff);
    }

    tile57_chart* chart_ = nullptr;
    uint32_t prog_ = 0; // solid Vtx program (area/line/symbol/text tile layers)
    int u_scale_ = -1, u_origin_ = -1, u_vp_ = -1, u_denom_ = -1, u_rot_ = -1;
    // Sprite (textured point symbols) program.
    uint32_t prog_sprite_ = 0;
    int su_scale_ = -1, su_origin_ = -1, su_vp_ = -1, su_denom_ = -1, su_atlas_ = -1, su_rot_ = -1;
    // Pattern (tiled-texture area fills) program.
    uint32_t prog_pat_ = 0;
    int pu_scale_ = -1, pu_origin_ = -1, pu_vp_ = -1, pu_denom_ = -1, pu_atlas_ = -1, pu_rot_ = -1;
    // Glyph (SDF text) program.
    uint32_t prog_glyph_ = 0;
    int gu_scale_ = -1, gu_origin_ = -1, gu_vp_ = -1, gu_denom_ = -1, gu_atlas_ = -1, gu_rot_ = -1;
    int gu_halo_ = -1, gu_halow_ = -1; // S-52 text halo (see FS_GLYPH)
    float halo_rgb_[3] = {1.0f, 1.0f, 1.0f}; // halo colour for the active palette
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

    // Previous render's view centre/zoom/rotation — motion detection (gates the AA
    // supersample; a turn is motion just like a pan).
    double last_lon_ = 1e9, last_lat_ = 1e9, last_zoom_r_ = 1e9, last_rot_ = 1e9;

    // Tiled path state (the only render path).
    std::unordered_map<uint64_t, TileGeom> tiles_; // (z,x,y) -> cached tile geometry
    uint64_t tiles_mhash_ = 0;                     // mariner hash the cache was portrayed at
    tile57_mariner tiles_m_{};                     // ...and the settings themselves (debug diff)
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
    // Rotation the label cache was portrayed at. Label POSITIONS are world anchors and
    // their glyph quads stay upright, so a cached label draws correctly at ANY rotation —
    // only the portray EXTENT (the rotated view's world AABB) and tile57's declutter grid
    // were computed for this angle. So don't re-portray on every degree of a turn (the
    // heading dithers constantly under course-up and this is the costliest per-frame item):
    // re-portray only once the heading has moved past kLblRotSlack, the same slack the
    // portray extent is widened by, so the angles we tolerate are the ones we covered.
    double lbl_rot_ = 1e9, lbl_prev_rot_ = 1e9;
};

} // namespace t57
