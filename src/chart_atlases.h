// chart_atlases.h - the textures a GPU scene samples, baked once per process.
//
// A scene's quads carry UVs into two kinds of atlas: the S-101 symbol sheet, baked at
// a given pixel ratio and color scheme, and the SDF glyph sheets (regular, bold,
// italic). tile57 bakes them; this class uploads each on first use and keeps it for
// every chart in the process. It also holds the per-scheme halo color that SDF text
// draws its outline in.
#pragma once
#include "gl_objects.h"
#include "tile57.h"
#include <map>
#include <utility>

namespace t57 {

class ChartAtlases {
  public:
    static ChartAtlases& shared();

    // The symbol atlas for a pixel ratio and scheme, uploading it on first use.
    // Returns 0 when tile57 cannot bake it.
    uint32_t sprite(double pixel_ratio, tile57_scheme scheme);
    // The SDF glyph atlas for a tile57_gpu_atlas value (GLYPH, GLYPH_BOLD, GLYPH_ITALIC).
    // Returns 0 when that face is unavailable.
    uint32_t glyph(uint8_t atlas);
    // Bit mask over tile57_gpu_atlas of the textures available for this view, in the
    // form tile57_gpu_batch_opts.atlas_have expects.
    uint8_t available(double pixel_ratio, tile57_scheme scheme);
    // The scheme's background color (S-52 NODATA), straight alpha 0..1.
    const float* halo(tile57_scheme scheme);

    // Forget every texture. Call when the GL context they were made in is gone.
    void invalidate();

  private:
    struct Entry {
        GlTexture tex;
        bool tried = false;
    };
    using SpriteKey = std::pair<int, int>; // (pixel ratio in hundredths, scheme)

    void load_halos();

    std::map<SpriteKey, Entry> sprites_;
    Entry glyphs_[3];
    float halos_[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    bool halos_loaded_ = false;
};

} // namespace t57
