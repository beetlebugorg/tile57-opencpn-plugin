// safety_overlay.h — persistent "NOT FOR NAVIGATION" banner.
//
// tile57 is experimental and not for navigation, so the warning is drawn every
// frame in screen space, independent of the chart view. Self-contained: its own
// GL program and a compact 5x7 vector font for the fixed banner text. The plugin
// composites it last, over everything.
#pragma once
#include <cstdint>

namespace t57 {

class SafetyOverlay {
public:
    bool init();
    // Draw the banner sized to the current framebuffer (w,h in px). Persistent:
    // callers MUST invoke this after the chart every frame.
    void render(uint32_t w, uint32_t h);
    void shutdown();

private:
    uint32_t prog_ = 0, vbo_ = 0;
    int u_vp_ = -1, u_color_ = -1;
    void draw_quads(const float* xy, int vert_count,
                    float r, float g, float b, float a, uint32_t w, uint32_t h);
};

} // namespace t57
