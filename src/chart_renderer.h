// chart_renderer.h - draws one tile57 chart for the OpenCPN host.
//
// The renderer keeps one GPU scene per chart: the whole view, portrayed once by
// tile57 with an overscan margin, uploaded, and drawn under a per-frame transform.
// A pan, zoom, or rotation inside the margin is a uniform change. The scene is
// rebuilt on a worker thread when the view leaves the margin or the zoom band, or
// the mariner settings change; the old scene stays on screen until the new one is
// ready.
//
// Layout of the pieces:
//   chart_programs   the GLSL programs and vertex stream bindings
//   chart_atlases    symbol and glyph textures, halo colors
//   scene_build      the CPU build and its worker thread
//   gpu_scene        a resident scene and its batched draw lists
//   chart_renderer   this class: policy, render targets, the host-facing API
#pragma once
#include "gl_objects.h"
#include "gpu_scene.h"
#include "scene_build.h"
#include "tile57.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace t57 {

// tile57 packs colors as 0xRRGGBBAA.
struct Rgba8 {
    unsigned char r, g, b, a;
};
static inline Rgba8 tile57_unpack(tile57_color c) {
    return Rgba8{(unsigned char)((c >> 24) & 0xFF), (unsigned char)((c >> 16) & 0xFF),
                 (unsigned char)((c >> 8) & 0xFF), (unsigned char)(c & 0xFF)};
}

class ChartRenderer {
  public:
    // OpenCPN renders a chart in two passes so text lands above its own overlays.
    enum class Pass { kBase, kText, kAll };

    bool open_chart(const std::string& pmtiles_path);
    bool ensure_gl();
    // Records the host's "Use SCAMIN" intent. The live cull denominator arrives per
    // render() call.
    void set_super_scamin(bool on, double native_scale);
    // Draw the chart into the current framebuffer. lon/lat: view center. zoom:
    // web-mercator zoom of the geographic view. w/h: the GL viewport in framebuffer
    // px. device_scale: framebuffer px per logical px. cull_bias: extra SCAMIN
    // thinning in zoom levels. rotation: view rotation in radians. scamin_display_denom:
    // the host's display scale 1:N, 0 for no cull. patch_fb: the quilt patch this
    // chart owns, in framebuffer px, or null for the whole viewport.
    void render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                const tile57_mariner& m, Pass pass, bool stencil_clip, double device_scale = 1.0,
                double cull_bias = 0.0, double rotation = 0.0, double scamin_display_denom = 0.0,
                const int* patch_fb = nullptr);
    void shutdown();
    bool has_chart() const { return chart_ != nullptr; }
    // True while a scene build is queued or running.
    bool tiles_pending() const { return builder_.busy(); }
    bool get_info(tile57_info& out) const;
    tile57_chart* chart_handle() const { return chart_; }
    // Serializes every call into the chart handle: the build worker and the host's
    // object query both take it.
    std::mutex& portray_mutex() const { return portray_mu_; }
    // Called from the build worker when a scene is ready to adopt.
    void set_progress_callback(std::function<void()> cb);
    ~ChartRenderer();

  private:
    using Clock = std::chrono::steady_clock;

    struct Camera {
        double lon = 0, lat = 0, zoom = 0, rotation = 0;
        double wx = 0, wy = 0; // world position of the center
    };

    // Adopt a finished build, if there is one.
    void adopt_ready();
    // Decide whether this view needs a new scene and queue or run the build.
    void plan_build(const Camera& cam, uint32_t w, uint32_t h, double device_scale,
                    const tile57_mariner& m, uint64_t mhash, bool gesture);
    SceneRequest make_request(const Camera& cam, double build_zoom, uint32_t w, uint32_t h,
                              double device_scale, const tile57_mariner& m, uint64_t mhash);
    static bool covers(const SceneCoverage& c, double wx, double wy, double zoom, double zoom_band);
    bool ensure_supersample(int w, int h);

    tile57_chart* chart_ = nullptr;
    double min_zoom_ = 0, max_zoom_ = 0; // the chart's baked zoom band
    mutable std::mutex portray_mu_;
    SceneBuilder builder_;
    std::unique_ptr<GpuScene> scene_;
    uint64_t next_generation_ = 1;

    // The build in flight or queued, so a frame does not ask for it twice.
    bool requested_ = false;
    SceneCoverage requested_cov_;
    uint64_t requested_mhash_ = 0;

    // Motion tracking: a gesture is a run of camera changes closer together than
    // kGestureHold.
    Camera last_cam_;
    bool have_last_cam_ = false;
    Clock::time_point last_motion_{};

    // Supersample target: the scene renders at kSS times the viewport into this
    // framebuffer, then composites down. Depth is attached so opaque fills can
    // draw front to back.
    struct Supersample {
        GlFramebuffer fbo;
        GlTexture color;
        GlRenderbuffer depth;
        int w = 0, h = 0;
        bool ok = false;
    } ss_;
    bool host_stencil_mode_ = false; // the host has no FBO; draw direct, no composite
    bool gl_ready_ = false;

    double native_scale_ = 0.0;
    bool super_scamin_ = true;
    std::function<void()> on_progress_;
};

} // namespace t57
