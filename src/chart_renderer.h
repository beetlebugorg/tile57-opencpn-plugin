// chart_renderer.h — draws a tile57 chart view on the GPU.
//
// tile57 paints a view through a callback canvas as flattened, resolved
// primitives in pixel space (see tile57_chart_render_view_cb). We tessellate
// those to triangles and draw them pixel->NDC. tile57 portrayal is re-run only
// when the view or mariner settings change; otherwise the cached geometry is
// replayed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "tile57.h"

namespace t57 {

class ChartRenderer {
public:
    bool open_chart(const std::string& pmtiles_path);
    bool ensure_gl();                       // compile program + buffers (GL context must be current)
    // Render the chart for this view into the current framebuffer. lon/lat is
    // the view centre, zoom is the web-mercator zoom, w/h the pixel extent.
    void render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                const tile57_mariner& m, bool draw_text);
    void shutdown();
    bool has_chart() const { return chart_ != nullptr; }
    // Fixed chart metadata (zoom range, bounds, anchor). False if no chart open.
    bool get_info(tile57_chart_info& out) const;

    // Geometry sink for the C callbacks (public so the trampolines can reach it).
    struct Vtx { float x, y; uint8_t r, g, b, a; };
    std::vector<Vtx> tris;
    void add_fill(const tile57_rings* p, tile57_rgba c);
    void add_stroke(const tile57_rings* p, float w, tile57_rgba c);
    bool skip_text = false;

private:
    // Re-run tile57 portrayal and rebuild the triangle buffer for this view.
    void rebuild(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                 const tile57_mariner& m);

    tile57_chart* chart_ = nullptr;
    uint32_t prog_ = 0, vbo_ = 0;
    int u_vp_ = -1;
    bool gl_ready_ = false;
    uint32_t vbo_count_ = 0;

    // Cache key: the last view/mariner we built geometry for.
    bool have_last_ = false;
    double last_lon_ = 0, last_lat_ = 0, last_zoom_ = 0;
    uint32_t last_w_ = 0, last_h_ = 0;
    uint64_t last_mhash_ = 0;
    bool last_text_ = true;
};

} // namespace t57
