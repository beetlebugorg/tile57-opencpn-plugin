// chart_renderer.h — draws a tile57 chart as a GPU vector chart.
//
// tile57 portrays a view ONCE (tile57_chart_render_surface_cb) into a WORLD-SPACE
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
#include <cstdint>
#include <string>
#include <vector>
#include "tile57.h"

namespace t57 {

class ChartRenderer {
public:
    enum class Pass { kBase, kText, kAll };

    bool open_chart(const std::string& pmtiles_path);   // pmtiles OR .000 cell
    bool ensure_gl();
    // Portray (if needed) for this view, then draw into the current framebuffer.
    // lon/lat = view centre, zoom = fractional web-mercator zoom, w/h = the GL
    // viewport, m = mariner. `pass` selects base geometry / text / both.
    void render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                const tile57_mariner& m, Pass pass, bool stencil_clip);
    void shutdown();
    bool has_chart() const { return chart_ != nullptr; }
    bool get_info(tile57_chart_info& out) const;
    tile57_chart* chart_handle() const { return chart_; }   // for object-query

    // Unified GPU vertex (see header note).
    struct Vtx { float wx, wy, px, py; uint8_t r, g, b, a; float thresh; };
    // Sprite vertex: world anchor + screen-px quad corner + atlas UV + SCAMIN.
    struct SpriteVtx { float wx, wy, px, py, u, v, thresh; };
    // Pattern vertex: world position + atlas cell rect + tile screen px + SCAMIN.
    struct PatVtx { float wx, wy, u0, v0, u1, v1, tw, th, thresh; };
    // Glyph vertex: world anchor + screen-px quad corner + SDF atlas UV + colour
    // + SCAMIN (text drawn as SDF quads from the glyph atlas).
    struct GlyphVtx { float wx, wy, px, py, u, v; uint8_t r, g, b, a; float thresh; };

    // TEMP diagnostic snapshot of the last render().
    struct Dbg { uint32_t area, line, symbol, text; bool rebuilt; double cam_zoom; };
    Dbg dbg() const { return { n_area_, n_line_, n_symbol_, n_text_, last_rebuilt_, cam_zoom_ }; }

    // Geometry sinks the C surface callbacks append to (public for the
    // trampolines). Grouped by paint layer so we draw area->line->symbol->text.
    std::vector<Vtx> area_, line_, symbol_, text_;
    std::vector<SpriteVtx> sprite_;   // point symbols drawn from the atlas
    std::vector<PatVtx> pattern_;     // area fills tiled from the pattern atlas
    std::vector<GlyphVtx> glyph_;     // text drawn as SDF quads from the glyph atlas
    // The world reference point offsets are relative to (set per portrayal).
    double ref_wx_ = 0, ref_wy_ = 0;
    // Geometry decimation epsilon in world units (~half a portrayal pixel).
    double decimate_eps_ = 0;
    // Display scale (mariner size_scale) — pattern tile screen size.
    double size_scale_ = 1.0;
    // Callback handlers (world/local geometry -> Vtx).
    void on_fill_area(const tile57_world_rings* r, tile57_rgba c, float thresh);
    void on_stroke_line(const tile57_world_rings* l, float width_px, tile57_rgba c, float thresh);
    void on_draw_symbol(tile57_world_point anchor, const tile57_local_rings* r,
                        tile57_rgba c, int even_odd, float stroke_w, float thresh);
    void on_draw_text(tile57_world_point anchor, const tile57_local_rings* g,
                      tile57_rgba c, tile57_rgba halo, float thresh);
    void on_draw_sprite(const char* name, size_t len, tile57_world_point anchor,
                        float rot_deg, float half_w, float half_h, float thresh);
    void on_draw_pattern(const char* name, size_t len, const tile57_world_rings* rings,
                         float thresh);
    // Lay out `text` from the SDF glyph atlas at (anchor + origin px), size in px,
    // appending one quad per glyph to glyph_.
    void on_draw_text_str(tile57_world_point anchor, float ox, float oy, const char* text,
                          size_t len, float size_px, tile57_rgba color, tile57_rgba halo,
                          float thresh);

private:
    void rebuild(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                 const tile57_mariner& m);
    void upload();
    void draw_range(uint32_t vbo, uint32_t count);
    void composite_ss();   // draw the resolved MSAA texture over OpenCPN's FBO

    tile57_chart* chart_ = nullptr;
    uint32_t prog_ = 0, vbo_area_ = 0, vbo_line_ = 0, vbo_symbol_ = 0, vbo_text_ = 0;
    uint32_t n_area_ = 0, n_line_ = 0, n_symbol_ = 0, n_text_ = 0;
    int u_scale_ = -1, u_origin_ = -1, u_vp_ = -1, u_zoom_ = -1;
    // Sprite (textured) program + buffer for point symbols.
    uint32_t prog_sprite_ = 0, vbo_sprite_ = 0, n_sprite_ = 0;
    int su_scale_ = -1, su_origin_ = -1, su_vp_ = -1, su_zoom_ = -1, su_atlas_ = -1;
    // Pattern (tiled-texture) program + buffer for area fills.
    uint32_t prog_pat_ = 0, vbo_pat_ = 0, n_pat_ = 0;
    int pu_scale_ = -1, pu_origin_ = -1, pu_vp_ = -1, pu_zoom_ = -1, pu_atlas_ = -1;
    // Glyph (SDF text) program + buffer.
    uint32_t prog_glyph_ = 0, vbo_glyph_ = 0, n_glyph_ = 0;
    int gu_scale_ = -1, gu_origin_ = -1, gu_vp_ = -1, gu_zoom_ = -1, gu_atlas_ = -1;
    // Composite program + fullscreen quad: draw the resolved MSAA texture (a
    // shared multisampled FBO, see chart_renderer.cpp) over OpenCPN's FBO. MSAA
    // antialiases tessellated text/lines/area edges at ~1x fill cost.
    uint32_t prog_blit_ = 0, vbo_quad_ = 0;
    int bu_tex_ = -1;
    bool gl_ready_ = false;

    bool have_range_ = false;
    double min_zoom_ = 0, max_zoom_ = 0;

    // Cached portrayal camera: the geometry is valid for views inside this
    // world window; leaving it (pan) or a big zoom step re-portrays.
    bool have_cam_ = false;
    double cam_lon_ = 0, cam_lat_ = 0, cam_zoom_ = 0;
    double last_zoom_ = -1;   // previous frame's view zoom (settle detection)
    double last_lon_ = 1e9, last_lat_ = 1e9, last_zoom_r_ = 1e9;  // last render view
    bool last_rebuilt_ = false;   // TEMP diagnostic
    uint32_t cam_w_ = 0, cam_h_ = 0;
    uint64_t cam_mhash_ = 0;
};

} // namespace t57
