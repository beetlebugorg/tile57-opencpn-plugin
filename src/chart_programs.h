// chart_programs.h - the GLSL 1.20 programs that draw tile57's GPU scene streams
// (tile57.h, "draw-ready GPU scenes").
//
// Four pipelines (flat triangles, pattern-tiled triangles, sprite quads, SDF glyph
// quads) share one camera transform and one visibility gate. A fifth program blits
// the supersample target. Attribute locations are the same in every program, so a
// vertex stream is bound once per stream, not once per pipeline:
//   0 aWorld  1 aPost  2 aScamin  3 aDispCat  4 aMapAlign  5 aColor  6 aDepth
//   7 aUV     8 aWeight  9 aFlipTangent
//
// The programs are shared by every chart in the process. ensure() rebuilds them
// when the GL context they were made in is gone.
#pragma once
#include "gl_objects.h"
#include <cstdint>

namespace t57 {

// The per-frame camera and gate state every pipeline reads.
struct FrameUniforms {
    float scale = 1;            // framebuffer px per world unit (camera-relative world)
    float origin[2] = {0, 0};   // framebuffer px of the scene's build center
    float viewport[2] = {1, 1}; // framebuffer size in px (NDC mapping)
    float rot[2] = {1, 0};      // view rotation as (cos, sin)
    float scamin_denom = 0;     // live 1:N display scale; 0 disables the SCAMIN cull
    float cat[3] = {1, 1, 1};   // display-category visibility (base, standard, other)
};

class ChartPrograms {
  public:
    enum class Pipe { kFlat, kPattern, kSprite, kSdf };

    static ChartPrograms& shared();

    // Build (or rebuild after a context loss) every program. False if a shader
    // failed to compile, in which case nothing can be drawn.
    bool ensure();

    // Select a pipeline and load the frame uniforms into it.
    void use(Pipe pipe, const FrameUniforms& frame);
    // Pipeline-specific state, valid after use() of the matching pipeline.
    void set_pattern_cell(uint32_t tex, float period_w, float period_h); // kPattern
    void set_atlas(uint32_t tex);                                        // kSprite / kSdf
    void set_halo(const float rgba[4]);                                  // kSdf

    // Bind the triangle stream (tile57_gpu_vertex + uint32 indices) or the quad
    // stream (tile57_gpu_quad) to the shared attribute locations.
    void bind_triangle_stream(uint32_t vbo, uint32_t ibo);
    void bind_quad_stream(uint32_t qbo);
    void unbind_streams();

    // Composite a texture over the current framebuffer with premultiplied blending.
    void blit(uint32_t tex);

  private:
    struct Program {
        uint32_t id = 0;
        int u_scale = -1, u_origin = -1, u_vp = -1, u_rot = -1, u_denom = -1, u_cat = -1;
        int u_tex = -1, u_period = -1, u_halo = -1;
    };
    Program& program(Pipe p);
    bool build();
    void destroy();

    Program flat_, pattern_, sprite_, sdf_;
    uint32_t blit_prog_ = 0;
    int blit_tex_ = -1;
    GlBuffer blit_quad_;
    bool ready_ = false;
};

} // namespace t57
