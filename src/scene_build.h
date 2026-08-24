// scene_build.h - portray a view into CPU buffers, on a worker thread.
//
// A build captures every input it needs (camera, viewport, mariner settings, pixel
// ratio) at request time and runs tile57_chart_gpu_scene on its own thread. The
// render thread keeps drawing the scene it already has and adopts the result when
// it is complete. One request is pending at a time: a newer request replaces one
// that has not started.
#pragma once
#include "tile57.h"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace t57 {

// The world box a scene was portrayed for. World is web-mercator [0,1], y down.
struct SceneCoverage {
    double cx = 0, cy = 0;           // build view center
    double zoom = 0;                 // the view zoom the scene was requested for
    double build_zoom = 0;           // that zoom clamped to the chart's baked band
    double half_wx = 0, half_wy = 0; // half extents of the overscanned box
};

// Everything one build needs, copied so the worker never reads live host state.
struct SceneRequest {
    tile57_chart* chart = nullptr;
    std::mutex* chart_mutex = nullptr; // serializes every call into `chart`
    double lon = 0, lat = 0, zoom = 0;
    uint32_t width = 0, height = 0; // the viewport; the build overscans it
    double pixel_ratio = 1;
    tile57_mariner mariner{};
    std::vector<int32_t> viewing_groups_off; // backing store for mariner.viewing_groups_off
    uint64_t mariner_hash = 0;
    uint64_t generation = 0;

    // Point mariner.viewing_groups_off at this request's own copy.
    void own_mariner_buffers();
};

struct PatternCell {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// A portrayed view, ready to upload. Vertex and quad positions are relative to the
// coverage center so f32 keeps its precision at harbor zoom.
struct SceneData {
    std::vector<tile57_gpu_vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<tile57_gpu_quad> quads;
    std::vector<tile57_gpu_range> ranges;
    std::vector<PatternCell> patterns;
    SceneCoverage coverage;
    uint64_t mariner_hash = 0;
    uint64_t generation = 0;
    double build_ms = 0;
    bool ok = false;
};

// The overscan factor a build applies to the viewport, and the zoom drift that
// calls for a new build. 2^0.3 is under 1.25, so the coverage still holds the
// viewport when the zoom trigger fires.
constexpr double kSceneOverscan = 1.25;
constexpr double kSceneZoomBand = 0.3;

void lonlat_to_world(double lon, double lat, double& wx, double& wy);
uint64_t mariner_hash(const tile57_mariner& m);

// Run one build on the calling thread.
SceneData build_scene(const SceneRequest& req);

// Owns the worker thread. Thread-safe.
class SceneBuilder {
  public:
    SceneBuilder() = default;
    ~SceneBuilder();
    SceneBuilder(const SceneBuilder&) = delete;
    SceneBuilder& operator=(const SceneBuilder&) = delete;

    // Queue a build, replacing any request that has not started.
    void request(SceneRequest req);
    // The most recent finished build, or null. Older unclaimed results are dropped.
    std::unique_ptr<SceneData> take_ready();
    // True while a request is queued or running.
    bool busy() const;
    // Called on the worker thread when a build finishes.
    void set_on_done(std::function<void()> cb);
    // Stop the worker and join it. A running build finishes first.
    void stop();

  private:
    void run();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    std::unique_ptr<SceneRequest> pending_;
    std::unique_ptr<SceneData> ready_;
    std::function<void()> on_done_;
    bool running_ = false; // a build is executing
    bool stop_ = false;
};

} // namespace t57
