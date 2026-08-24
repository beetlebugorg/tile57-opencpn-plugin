// gpu_scene.h - a portrayed view resident on the GPU.
//
// Uploads a SceneData once and draws it any number of times under a per-frame
// transform. Ranges are batched into draw calls with tile57_gpu_batch; the draw
// list for a given set of options is built on first use and kept.
#pragma once
#include "chart_atlases.h"
#include "chart_programs.h"
#include "gl_objects.h"
#include "scene_build.h"
#include <cstdint>
#include <utility>
#include <vector>

namespace t57 {

class GpuScene {
  public:
    // Upload on the GL thread.
    explicit GpuScene(const SceneData& data);

    struct DrawOptions {
        bool geometry = true; // draw everything but text
        bool text = true;     // draw text
        bool text_on = true;  // the mariner's text switch
        bool sound_on = true; // the mariner's soundings switch
        bool depth = false;   // a cleared depth buffer is attached; use it
        double pixel_ratio = 1;
        tile57_scheme scheme = TILE57_SCHEME_DAY;
    };

    void draw(ChartPrograms& programs, ChartAtlases& atlases, const FrameUniforms& frame,
              const DrawOptions& opts);

    const SceneCoverage& coverage() const { return coverage_; }
    uint64_t mariner_hash() const { return mariner_hash_; }
    uint64_t generation() const { return generation_; }
    // Draw calls issued by the last draw().
    size_t last_draw_calls() const { return last_draw_calls_; }

  private:
    struct DrawList {
        bool geometry = false, text = false, text_on = false, sound_on = false, depth = false;
        uint8_t atlas_have = 0;
        std::vector<tile57_gpu_draw> opaque;  // front-to-back, depth-tested
        std::vector<tile57_gpu_draw> ordered; // paint order, blended
    };
    const DrawList& draw_list(const DrawOptions& opts, uint8_t atlas_have, const float halo[4]);
    void batch(const std::vector<tile57_gpu_range>& ranges, const tile57_gpu_batch_opts& opts,
               std::vector<tile57_gpu_draw>& out) const;

    GlBuffer vbo_, ibo_, qbo_;
    std::vector<GlTexture> pattern_tex_;
    std::vector<std::pair<int, int>> pattern_period_; // device px per cell
    std::vector<tile57_gpu_range> ranges_;
    std::vector<DrawList> lists_;
    SceneCoverage coverage_;
    uint64_t mariner_hash_ = 0;
    uint64_t generation_ = 0;
    size_t last_draw_calls_ = 0;
};

} // namespace t57
