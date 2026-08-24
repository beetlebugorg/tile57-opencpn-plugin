// scene_build.cpp - see scene_build.h.
#include "scene_build.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <wx/log.h>

namespace t57 {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void lonlat_to_world(double lon, double lat, double& wx, double& wy) {
    wx = (lon + 180.0) / 360.0;
    const double s = std::sin(lat * kPi / 180.0);
    wy = 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * kPi);
}

// FNV-1a over the struct bytes and the viewing-group list it points at.
uint64_t mariner_hash(const tile57_mariner& m) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    tile57_mariner flat = m;
    flat.viewing_groups_off = nullptr; // the pointer value is not part of the settings
    mix(&flat, sizeof(flat));
    if (m.viewing_groups_off && m.viewing_groups_off_len)
        mix(m.viewing_groups_off, m.viewing_groups_off_len * sizeof(int32_t));
    return h;
}

void SceneRequest::own_mariner_buffers() {
    if (mariner.viewing_groups_off && mariner.viewing_groups_off_len) {
        viewing_groups_off.assign(mariner.viewing_groups_off,
                                  mariner.viewing_groups_off + mariner.viewing_groups_off_len);
        mariner.viewing_groups_off = viewing_groups_off.data();
    } else {
        viewing_groups_off.clear();
        mariner.viewing_groups_off = nullptr;
        mariner.viewing_groups_off_len = 0;
    }
}

SceneData build_scene(const SceneRequest& req) {
    SceneData out;
    out.generation = req.generation;
    out.mariner_hash = req.mariner_hash;
    if (!req.chart || !req.chart_mutex || !req.width || !req.height)
        return out;
    const auto t0 = std::chrono::steady_clock::now();

    double cx, cy;
    lonlat_to_world(req.lon, req.lat, cx, cy);
    const uint32_t ow = (uint32_t)std::lround(req.width * kSceneOverscan);
    const uint32_t oh = (uint32_t)std::lround(req.height * kSceneOverscan);

    tile57_gpu_scene sc{};
    tile57_error err{};
    double bz;
    {
        std::lock_guard<std::mutex> lock(*req.chart_mutex);
        // A chart has tiles inside its baked zoom band only; the camera scales the
        // nearest band past it.
        tile57_info info{};
        tile57_chart_get_info(req.chart, &info);
        bz = std::clamp(req.zoom, (double)info.min_zoom, (double)info.max_zoom);
        tile57_mariner m = req.mariner;
        if (tile57_chart_gpu_scene(req.chart, req.lon, req.lat, bz, ow, oh, &m, req.pixel_ratio,
                                   &sc, &err) != TILE57_OK) {
            wxLogMessage("tile57: gpu_scene failed: %s", err.message);
            return out;
        }
    }

    out.vertices.assign(sc.vertices, sc.vertices + sc.vertex_count);
    for (tile57_gpu_vertex& v : out.vertices) {
        v.x -= (float)cx;
        v.y -= (float)cy;
    }
    out.indices.assign(sc.indices, sc.indices + sc.index_count);
    out.quads.assign(sc.quads, sc.quads + sc.quad_count);
    for (tile57_gpu_quad& q : out.quads) {
        q.x -= (float)cx;
        q.y -= (float)cy;
    }
    out.ranges.assign(sc.ranges, sc.ranges + sc.range_count);
    out.patterns.resize(sc.pattern_count);
    for (size_t i = 0; i < sc.pattern_count; ++i) {
        const tile57_gpu_pattern& p = sc.patterns[i];
        if (!p.rgba || !p.w || !p.h)
            continue;
        out.patterns[i].w = p.w;
        out.patterns[i].h = p.h;
        out.patterns[i].rgba.assign(p.rgba, p.rgba + p.rgba_len);
    }
    tile57_gpu_scene_free(&sc);

    const double world_px = 256.0 * std::pow(2.0, bz);
    out.coverage.cx = cx;
    out.coverage.cy = cy;
    out.coverage.zoom = req.zoom;
    out.coverage.build_zoom = bz;
    out.coverage.half_wx = (ow * 0.5) / world_px;
    out.coverage.half_wy = (oh * 0.5) / world_px;
    out.build_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    out.ok = true;
    return out;
}

// ---- SceneBuilder ------------------------------------------------------------------

SceneBuilder::~SceneBuilder() { stop(); }

void SceneBuilder::request(SceneRequest req) {
    std::lock_guard<std::mutex> lock(mu_);
    if (stop_)
        return;
    pending_ = std::make_unique<SceneRequest>(std::move(req));
    pending_->own_mariner_buffers();
    if (!thread_.joinable())
        thread_ = std::thread([this] { run(); });
    cv_.notify_one();
}

std::unique_ptr<SceneData> SceneBuilder::take_ready() {
    std::lock_guard<std::mutex> lock(mu_);
    return std::move(ready_);
}

bool SceneBuilder::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_ || pending_ != nullptr;
}

void SceneBuilder::set_on_done(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mu_);
    on_done_ = std::move(cb);
}

void SceneBuilder::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        pending_.reset();
    }
    cv_.notify_one();
    if (thread_.joinable())
        thread_.join();
    std::lock_guard<std::mutex> lock(mu_);
    ready_.reset();
    stop_ = false;
}

void SceneBuilder::run() {
    for (;;) {
        std::unique_ptr<SceneRequest> req;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || pending_; });
            if (stop_)
                return;
            req = std::move(pending_);
            running_ = true;
        }
        auto result = std::make_unique<SceneData>(build_scene(*req));
        std::function<void()> done;
        {
            std::lock_guard<std::mutex> lock(mu_);
            ready_ = std::move(result);
            running_ = false;
            done = on_done_;
        }
        if (done)
            done();
    }
}

} // namespace t57
