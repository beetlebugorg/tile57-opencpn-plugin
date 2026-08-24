// gpu_scene.cpp - see gpu_scene.h.
#include "gpu_scene.h"
#include <algorithm>
#include <cstring>

namespace t57 {

GpuScene::GpuScene(const SceneData& data)
    : coverage_(data.coverage), mariner_hash_(data.mariner_hash), generation_(data.generation) {
    if (!data.vertices.empty()) {
        vbo_.gen();
        glBindBuffer(GL_ARRAY_BUFFER, vbo_.id());
        glBufferData(GL_ARRAY_BUFFER, data.vertices.size() * sizeof(tile57_gpu_vertex),
                     data.vertices.data(), GL_STATIC_DRAW);
    }
    if (!data.indices.empty()) {
        ibo_.gen();
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_.id());
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, data.indices.size() * sizeof(uint32_t),
                     data.indices.data(), GL_STATIC_DRAW);
    }
    if (!data.quads.empty()) {
        qbo_.gen();
        glBindBuffer(GL_ARRAY_BUFFER, qbo_.id());
        glBufferData(GL_ARRAY_BUFFER, data.quads.size() * sizeof(tile57_gpu_quad),
                     data.quads.data(), GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // One texture per pattern cell. The cell is rasterized at the scene's density, so
    // its size is the on-screen tiling period.
    pattern_tex_.reserve(data.patterns.size());
    pattern_period_.reserve(data.patterns.size());
    for (const PatternCell& p : data.patterns) {
        if (p.rgba.empty()) {
            pattern_tex_.emplace_back();
            pattern_period_.emplace_back(1, 1);
            continue;
        }
        pattern_tex_.push_back(make_rgba_texture(p.rgba.data(), (int)p.w, (int)p.h, GL_REPEAT));
        pattern_period_.emplace_back((int)p.w, (int)p.h);
    }
    ranges_ = data.ranges;
}

void GpuScene::batch(const std::vector<tile57_gpu_range>& ranges,
                     const tile57_gpu_batch_opts& opts, std::vector<tile57_gpu_draw>& out) const {
    out.clear();
    if (ranges.empty())
        return;
    // A batch only merges ranges, so range_count draws always fit.
    out.resize(ranges.size());
    const size_t n = tile57_gpu_batch(ranges.data(), ranges.size(), &opts, out.data(), out.size());
    out.resize(n <= ranges.size() ? n : 0);
}

const GpuScene::DrawList& GpuScene::draw_list(const DrawOptions& o, uint8_t atlas_have,
                                              const float halo[4]) {
    for (const DrawList& l : lists_)
        if (l.geometry == o.geometry && l.text == o.text && l.text_on == o.text_on &&
            l.sound_on == o.sound_on && l.depth == o.depth && l.atlas_have == atlas_have)
            return l;

    DrawList list;
    list.geometry = o.geometry;
    list.text = o.text;
    list.text_on = o.text_on;
    list.sound_on = o.sound_on;
    list.depth = o.depth;
    list.atlas_have = atlas_have;

    // The host draws text as its own pass; select the ranges this pass owns.
    std::vector<tile57_gpu_range> pass_ranges;
    pass_ranges.reserve(ranges_.size());
    for (const tile57_gpu_range& r : ranges_) {
        const bool is_text = r.kind == TILE57_GPU_TEXT;
        if (is_text ? o.text : o.geometry)
            pass_ranges.push_back(r);
    }

    tile57_gpu_batch_opts opts{};
    opts.text_on = o.text_on;
    opts.sound_on = o.sound_on;
    opts.exclude_opaque_tris = o.depth;
    opts.atlas_have = atlas_have;
    std::memcpy(opts.halo, halo, sizeof(opts.halo));
    batch(pass_ranges, opts, list.ordered);

    if (o.depth) {
        // Opaque fills draw first, front to back, and the depth test drops what later
        // paint covers. The batcher merges them like any other range.
        std::vector<tile57_gpu_range> opaque;
        for (const tile57_gpu_range& r : pass_ranges)
            if ((r.flags & 1u) && r.prim == TILE57_GPU_TRIANGLES &&
                r.pattern == TILE57_GPU_NO_PATTERN)
                opaque.push_back(r);
        tile57_gpu_batch_opts oo = opts;
        oo.exclude_opaque_tris = false;
        batch(opaque, oo, list.opaque);
        std::reverse(list.opaque.begin(), list.opaque.end());
    }
    lists_.push_back(std::move(list));
    return lists_.back();
}

void GpuScene::draw(ChartPrograms& programs, ChartAtlases& atlases, const FrameUniforms& frame,
                    const DrawOptions& opts) {
    last_draw_calls_ = 0;
    const uint8_t atlas_have = atlases.available(opts.pixel_ratio, opts.scheme);
    const float* halo = atlases.halo(opts.scheme);
    const DrawList& list = draw_list(opts, atlas_have, halo);
    if (list.ordered.empty() && list.opaque.empty())
        return;

    // Pipeline, stream, and per-draw uniform state, changed only when a draw needs it.
    enum class Stream { kNone, kTriangles, kQuads };
    Stream stream = Stream::kNone;
    int pipe = -1;
    uint32_t cat_or = 0;
    uint32_t bound_tex = 0;
    bool have_halo = false;
    float halo_now[4] = {0, 0, 0, 0};

    auto issue = [&](const tile57_gpu_draw& d) {
        if (d.pipeline == TILE57_GPU_PIPE_PATTERN &&
            (d.pattern >= pattern_tex_.size() || !pattern_tex_[d.pattern]))
            return; // the cell never rasterized; the flat fill under it already drew
        if (pipe != d.pipeline || cat_or != d.cat_mask_or) {
            pipe = d.pipeline;
            cat_or = d.cat_mask_or;
            FrameUniforms f = frame;
            for (int i = 0; i < 3; ++i)
                if (cat_or & (1u << i))
                    f.cat[i] = 1.f;
            programs.use((ChartPrograms::Pipe)pipe, f);
            bound_tex = 0;
            have_halo = false;
        }
        const Stream want = d.prim == TILE57_GPU_QUADS ? Stream::kQuads : Stream::kTriangles;
        if (stream != want) {
            stream = want;
            if (want == Stream::kQuads)
                programs.bind_quad_stream(qbo_.id());
            else
                programs.bind_triangle_stream(vbo_.id(), ibo_.id());
        }
        switch (d.pipeline) {
        case TILE57_GPU_PIPE_PATTERN: {
            const uint32_t tex = pattern_tex_[d.pattern].id();
            if (bound_tex != tex) {
                bound_tex = tex;
                programs.set_pattern_cell(tex, (float)pattern_period_[d.pattern].first,
                                          (float)pattern_period_[d.pattern].second);
            }
            break;
        }
        case TILE57_GPU_PIPE_SPRITE: {
            const uint32_t tex = atlases.sprite(opts.pixel_ratio, opts.scheme);
            if (bound_tex != tex) {
                bound_tex = tex;
                programs.set_atlas(tex);
            }
            break;
        }
        case TILE57_GPU_PIPE_SDF: {
            const uint32_t tex = atlases.glyph(d.atlas);
            if (bound_tex != tex) {
                bound_tex = tex;
                programs.set_atlas(tex);
            }
            if (!have_halo || std::memcmp(halo_now, d.color, sizeof(halo_now)) != 0) {
                have_halo = true;
                std::memcpy(halo_now, d.color, sizeof(halo_now));
                programs.set_halo(d.color);
            }
            break;
        }
        default:
            break;
        }
        if (d.prim == TILE57_GPU_QUADS)
            glDrawArrays(GL_TRIANGLES, (GLint)d.first, (GLsizei)d.count);
        else
            glDrawElements(GL_TRIANGLES, (GLsizei)d.count, GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>((size_t)d.first * sizeof(uint32_t)));
        ++last_draw_calls_;
    };

    if (opts.depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        for (const tile57_gpu_draw& d : list.opaque)
            issue(d);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    for (const tile57_gpu_draw& d : list.ordered)
        issue(d);

    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    programs.unbind_streams();
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

} // namespace t57
