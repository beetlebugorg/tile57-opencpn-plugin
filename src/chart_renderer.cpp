// chart_renderer.cpp - see chart_renderer.h.
#include "chart_renderer.h"
#include "chart_atlases.h"
#include "chart_programs.h"
#include "gl.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <wx/log.h>

namespace t57 {

namespace {

// Camera changes closer together than this belong to one gesture.
constexpr auto kGestureHold = std::chrono::milliseconds(200);
// Through a gesture a scene may drift this far in zoom before it is rebuilt.
constexpr double kGestureZoomBand = 1.0;
// A build for a zoom-out is placed this far outward, so its coverage is a superset
// of every view on the way there.
constexpr double kZoomLead = 0.15;

bool debug_enabled() {
    static const bool on = std::getenv("TILE57_DEBUG") != nullptr;
    return on;
}

// Supersample factor: TILE57_SS overrides; software renderers get 1.
int ss_factor() {
    static int k = -1;
    if (k >= 0)
        return k;
    if (const char* e = std::getenv("TILE57_SS")) {
        k = std::clamp(std::atoi(e), 1, 4);
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

// The header this plugin compiled against must describe the library it links.
bool abi_layout_matches() {
    static const bool ok = [] {
        const uint32_t expect =
            (uint32_t)sizeof(tile57_gpu_vertex) | (uint32_t)sizeof(tile57_gpu_quad) << 8 |
            (uint32_t)sizeof(tile57_gpu_range) << 16 | (uint32_t)sizeof(tile57_gpu_uniforms) << 24;
        const uint32_t have = tile57_abi_gpu_layout();
        if (have != expect)
            wxLogMessage("tile57: GPU ABI layout mismatch (header %08x, library %08x)", expect,
                         have);
        return have == expect;
    }();
    return ok;
}

} // namespace

// ---- chart -----------------------------------------------------------------------

bool ChartRenderer::open_chart(const std::string& path) {
    if (chart_)
        return true;
    tile57_error err{};
    if (tile57_chart_open(path.c_str(), &chart_, &err) != TILE57_OK) {
        wxLogMessage("tile57: open failed: %s", err.message);
        chart_ = nullptr;
        return false;
    }
    if (!chart_)
        return false;
    tile57_info info{};
    tile57_chart_get_info(chart_, &info);
    min_zoom_ = info.min_zoom;
    max_zoom_ = info.max_zoom;
    return true;
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

void ChartRenderer::set_progress_callback(std::function<void()> cb) {
    on_progress_ = std::move(cb);
    builder_.set_on_done(on_progress_);
}

// ---- GL --------------------------------------------------------------------------

bool ChartRenderer::ensure_gl() {
    if (gl_ready_)
        return true;
    if (!t57_gl_loader_init() || !abi_layout_matches())
        return false;
    if (!ChartPrograms::shared().ensure())
        return false;
    gl_ready_ = true;
    return true;
}

bool ChartRenderer::ensure_supersample(int w, int h) {
    if (w <= 0 || h <= 0)
        return false;
    const int k = ss_factor();
    const int sw = w * k, sh = h * k;
    if (ss_.fbo && ss_.w == sw && ss_.h == sh)
        return ss_.ok;
    if (!ss_.fbo) {
        ss_.fbo.gen();
        ss_.color.gen();
        ss_.depth.gen();
    }
    glBindTexture(GL_TEXTURE_2D, ss_.color.id());
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, ss_.depth.id());
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, sw, sh);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, ss_.fbo.id());
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ss_.color.id(), 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ss_.depth.id());
    ss_.ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ss_.w = sw;
    ss_.h = sh;
    return ss_.ok;
}

// ---- scene policy ----------------------------------------------------------------

// The view center must sit inside the coverage box with the whole viewport around
// it. The viewport at the live zoom is the build viewport scaled by 2^(build - live).
bool ChartRenderer::covers(const SceneCoverage& c, double wx, double wy, double zoom,
                           double zoom_band) {
    if (std::fabs(zoom - c.zoom) > zoom_band)
        return false;
    const double grow = std::pow(2.0, c.build_zoom - zoom);
    const double view_x = (c.half_wx / kSceneOverscan) * grow;
    const double view_y = (c.half_wy / kSceneOverscan) * grow;
    return std::fabs(wx - c.cx) <= c.half_wx - view_x && std::fabs(wy - c.cy) <= c.half_wy - view_y;
}

SceneRequest ChartRenderer::make_request(const Camera& cam, double build_zoom, uint32_t w,
                                         uint32_t h, double device_scale, const tile57_mariner& m,
                                         uint64_t mhash) {
    SceneRequest r;
    r.chart = chart_;
    r.chart_mutex = &portray_mu_;
    r.lon = cam.lon;
    r.lat = cam.lat;
    r.zoom = build_zoom;
    // The build works in geographic px; the framebuffer is device_scale times denser.
    r.width = (uint32_t)std::lround(w / device_scale);
    r.height = (uint32_t)std::lround(h / device_scale);
    r.pixel_ratio = device_scale;
    r.mariner = m;
    if (r.mariner.device_scale <= 0)
        r.mariner.device_scale = device_scale;
    r.mariner_hash = mhash;
    r.generation = next_generation_++;
    return r;
}

void ChartRenderer::adopt_ready() {
    std::unique_ptr<SceneData> data = builder_.take_ready();
    if (!data)
        return;
    requested_ = builder_.busy();
    if (!data->ok)
        return;
    scene_ = std::make_unique<GpuScene>(*data);
    if (debug_enabled())
        wxLogMessage("tile57: scene %llu built in %.1f ms: %zu verts, %zu quads, %zu ranges",
                     (unsigned long long)data->generation, data->build_ms, data->vertices.size(),
                     data->quads.size(), data->ranges.size());
}

void ChartRenderer::plan_build(const Camera& cam, uint32_t w, uint32_t h, double device_scale,
                               const tile57_mariner& m, uint64_t mhash, bool gesture) {
    if (requested_ && requested_mhash_ == mhash &&
        covers(requested_cov_, cam.wx, cam.wy, cam.zoom, kSceneZoomBand))
        return; // the build in flight will cover this view

    if (!scene_) {
        // Nothing is on screen, so build now rather than show a blank frame.
        SceneData data = build_scene(make_request(cam, cam.zoom, w, h, device_scale, m, mhash));
        if (data.ok)
            scene_ = std::make_unique<GpuScene>(data);
        if (debug_enabled())
            wxLogMessage("tile57: first scene built in %.1f ms", data.build_ms);
        return;
    }

    const SceneCoverage& cov = scene_->coverage();
    const bool settings_changed = scene_->mariner_hash() != mhash;
    const bool zoom_break = std::fabs(cam.zoom - cov.zoom) > kSceneZoomBand;
    const bool pan_break = !covers(cov, cam.wx, cam.wy, cam.zoom, kGestureZoomBand);
    if (!settings_changed && !zoom_break && !pan_break)
        return;
    // A zoom drift inside a gesture waits: each rebuild re-places labels, and doing
    // that several times per level reads as shaking. A pan off the coverage cannot
    // wait, and neither can a settings change.
    if (gesture && !settings_changed && !pan_break &&
        std::fabs(cam.zoom - cov.zoom) <= kGestureZoomBand)
        return;

    double zoom = cam.zoom;
    if (cam.zoom < cov.zoom)
        zoom -= kZoomLead;

    SceneRequest req = make_request(cam, zoom, w, h, device_scale, m, mhash);
    const double build_zoom = std::clamp(zoom, min_zoom_, max_zoom_);
    const double world_px = 256.0 * std::pow(2.0, build_zoom);
    requested_ = true;
    requested_mhash_ = mhash;
    requested_cov_.cx = cam.wx;
    requested_cov_.cy = cam.wy;
    requested_cov_.zoom = zoom;
    requested_cov_.build_zoom = build_zoom;
    requested_cov_.half_wx = (req.width * kSceneOverscan * 0.5) / world_px;
    requested_cov_.half_wy = (req.height * kSceneOverscan * 0.5) / world_px;
    builder_.request(std::move(req));
}

// ---- render ----------------------------------------------------------------------

void ChartRenderer::render(double lon, double lat, double zoom, uint32_t w, uint32_t h,
                           const tile57_mariner& m, Pass pass, bool stencil_clip,
                           double device_scale, double cull_bias, double rotation,
                           double scamin_display_denom, const int* patch_fb) {
    (void)patch_fb; // one scene covers the view; the host's scissor clips the patch
    if (!chart_ || !ensure_gl() || w == 0 || h == 0)
        return;
    if (device_scale <= 0)
        device_scale = 1.0;

    Camera cam;
    cam.lon = lon;
    cam.lat = lat;
    cam.zoom = zoom;
    cam.rotation = rotation;
    lonlat_to_world(lon, lat, cam.wx, cam.wy);

    const auto now = Clock::now();
    const bool moving = have_last_cam_ && (std::fabs(lon - last_cam_.lon) > 1e-9 ||
                                           std::fabs(lat - last_cam_.lat) > 1e-9 ||
                                           std::fabs(zoom - last_cam_.zoom) > 1e-4 ||
                                           std::fabs(rotation - last_cam_.rotation) > 1e-3);
    if (moving)
        last_motion_ = now;
    const bool gesture = have_last_cam_ && (now - last_motion_) < kGestureHold;
    last_cam_ = cam;
    have_last_cam_ = true;

    const uint64_t mhash = mariner_hash(m);
    adopt_ready();
    plan_build(cam, w, h, device_scale, m, mhash, gesture);
    if (!scene_)
        return;

    // Frame transform: screen(world_rel = 0) = viewport center - R * (view - build) * scale.
    const SceneCoverage& cov = scene_->coverage();
    const double scale_px = 256.0 * std::pow(2.0, zoom) * device_scale;
    const double c = std::cos(rotation), s = std::sin(rotation);
    const double dcx = (cam.wx - cov.cx) * scale_px;
    const double dcy = (cam.wy - cov.cy) * scale_px;
    FrameUniforms frame;
    frame.scale = (float)scale_px;
    frame.origin[0] = (float)(w * 0.5 - (c * dcx - s * dcy));
    frame.origin[1] = (float)(h * 0.5 - (s * dcx + c * dcy));
    frame.viewport[0] = (float)w;
    frame.viewport[1] = (float)h;
    frame.rot[0] = (float)c;
    frame.rot[1] = (float)s;
    frame.scamin_denom =
        scamin_display_denom > 0 ? (float)(scamin_display_denom * std::pow(2.0, cull_bias)) : 0.f;
    frame.cat[0] = m.display_base ? 1.f : 0.f;
    frame.cat[1] = m.display_standard ? 1.f : 0.f;
    frame.cat[2] = m.display_other ? 1.f : 0.f;

    GpuScene::DrawOptions opts;
    opts.geometry = pass != Pass::kText;
    opts.text = pass != Pass::kBase;
    opts.text_on = m.text_names || m.text_other;
    opts.sound_on = m.soundings != 0;
    opts.pixel_ratio = device_scale;
    opts.scheme = m.scheme;

    // Supersample when settled; draw direct while the view moves.
    if (stencil_clip)
        host_stencil_mode_ = true;
    static const bool no_ss = std::getenv("TILE57_NOSS") != nullptr;
    const int k = ss_factor();
    const bool ss = !no_ss && !host_stencil_mode_ && k > 1 && ensure_supersample((int)w, (int)h) &&
                    !(moving && pass != Pass::kText);

    GLint prev_fbo = 0, prev_vp[4] = {0, 0, 0, 0}, prev_sc[4] = {0, 0, 0, 0};
    GLboolean scissor_on = GL_FALSE;
    if (ss) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glGetIntegerv(GL_VIEWPORT, prev_vp);
        glGetIntegerv(GL_SCISSOR_BOX, prev_sc);
        scissor_on = glIsEnabled(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, ss_.fbo.id());
        glViewport(0, 0, (GLsizei)(w * k), (GLsizei)(h * k));
        if (scissor_on)
            glScissor(prev_sc[0] * k, prev_sc[1] * k, prev_sc[2] * k, prev_sc[3] * k);
        glClearColor(0, 0, 0, 0);
        glClearDepth(1.0);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        opts.depth = true;
    } else {
        GLint depth_bits = 0;
        glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
        opts.depth = depth_bits > 0;
        if (opts.depth) {
            glClearDepth(1.0);
            glDepthMask(GL_TRUE);
            glClear(GL_DEPTH_BUFFER_BIT);
        }
    }

    scene_->draw(ChartPrograms::shared(), ChartAtlases::shared(), frame, opts);

    if (ss) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
        if (scissor_on)
            glScissor(prev_sc[0], prev_sc[1], prev_sc[2], prev_sc[3]);
        ChartPrograms::shared().blit(ss_.color.id());
    }
}

void ChartRenderer::shutdown() {
    builder_.stop();
    {
        std::lock_guard<std::mutex> lock(portray_mu_);
        scene_.reset();
        ss_ = Supersample{};
        requested_ = false;
        have_last_cam_ = false;
        gl_ready_ = false;
        if (chart_) {
            tile57_chart_close(chart_);
            chart_ = nullptr;
        }
    }
}

ChartRenderer::~ChartRenderer() { shutdown(); }

} // namespace t57
