// chart_renderer.h — draws a tile57 chart view on the GPU.
//
// tile57 paints a view through a callback canvas as flattened, resolved
// primitives in pixel space (see tile57_chart_render_view_cb). We tessellate
// those to triangles and draw them pixel->NDC.
//
// To feel like a vector chart (not a re-rasterized picture), portrayal is NOT
// re-run per view: geometry is built for an over-scanned CAMERA — the view
// centre at a half-level-snapped zoom, with a lateral margin — and each frame
// maps camera pixel space onto the live view with an affine (uScale/uOffset)
// in the vertex shader. Panning inside the margin and fractional zoom around
// the snap are pure uniform updates; portrayal re-runs only when the view
// leaves the margin, the snapped zoom steps, or mariner settings change.
//
// Geometry is kept in TWO buffers — base (fills/strokes/patterns) and text
// (glyph outlines) — because OpenCPN draws quilted vector charts in two passes:
// per-patch geometry first (RenderRegionViewOnGLNoText), then one decluttered
// text pass over the whole quilt (RenderRegionViewOnGLTextOnly).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "tile57.h"

namespace t57 {

class ChartRenderer {
public:
    // Which cached buffer(s) a render pass draws.
    enum class Pass { kBase, kText, kAll };

    bool open_chart(const std::string& pmtiles_path);
    bool ensure_gl();                       // compile program + buffers (GL context must be current)
    // Draw the chart for this view into the current framebuffer. lon/lat is the
    // view centre, zoom the TRUE fractional web-mercator zoom (the renderer
    // snaps/clamps its portrayal camera internally), w/h the pixel extent of
    // the render target (the GL viewport). stencil_clip: clip the draw to the
    // stencil mask OpenCPN pre-wrote for this chart's quilt patch (stencil==1).
    void render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                const tile57_mariner& m, Pass pass, bool stencil_clip);
    void shutdown();
    bool has_chart() const { return chart_ != nullptr; }
    // Fixed chart metadata (zoom range, bounds, anchor). False if no chart open.
    bool get_info(tile57_chart_info& out) const;

    // Geometry sinks for the C callbacks (public so the trampolines can reach
    // them). Fills/strokes/patterns land in base_tris; glyph text in text_tris.
    struct Vtx { float x, y; uint8_t r, g, b, a; };
    std::vector<Vtx> base_tris, text_tris;
    void add_fill(const tile57_rings* p, tile57_rgba c, std::vector<Vtx>& out);
    void add_stroke(const tile57_rings* p, float w, tile57_rgba c);
    // Glyph text: unlike area fills (ring 0 = outer, rest = holes), a label's
    // rings are every letter's contours as flat siblings; group them by
    // containment before tessellating (else earcut bridges letters into bars).
    void add_glyph(const tile57_rings* p, tile57_rgba c);

private:
    // Re-run tile57 portrayal for the camera and rebuild both triangle buffers.
    void rebuild(const tile57_mariner& m);
    void draw_buffer(uint32_t vbo, uint32_t count);

    tile57_chart* chart_ = nullptr;
    uint32_t prog_ = 0, vbo_base_ = 0, vbo_text_ = 0;
    int u_vp_ = -1, u_scale_ = -1, u_offset_ = -1;
    bool gl_ready_ = false;
    uint32_t base_count_ = 0, text_count_ = 0;

    // Baked zoom range of the chart (portrayal is only available within it).
    bool have_range_ = false;
    double min_zoom_ = 0, max_zoom_ = 0;

    // The over-scanned portrayal camera the cached geometry was built for.
    bool have_cam_ = false;
    double cam_lon_ = 0, cam_lat_ = 0, cam_zoom_ = 0;
    uint32_t cam_w_ = 0, cam_h_ = 0;
    uint64_t cam_mhash_ = 0;
};

} // namespace t57
