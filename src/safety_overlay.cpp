// safety_overlay.cpp — see safety_overlay.h.
#include "safety_overlay.h"
#include "gl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace t57 {

// ---- a compact 5x7 uppercase font (only the glyphs the banner needs) -------
// 7 rows, low 5 bits each (bit4 = leftmost column). Space = all zero.
struct Glyph { char c; uint8_t rows[7]; };
static const Glyph FONT[] = {
    {' ', {0,0,0,0,0,0,0}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'I', {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
};
static const Glyph* find_glyph(char c) {
    for (auto& g : FONT) if (g.c == c) return &g;
    return &FONT[0]; // space fallback
}

static const char* BANNER = "NOT FOR NAVIGATION";

static const char* VS = R"(
layout(location=0) in vec2 aPx;      // screen px, top-left origin (y down)
uniform vec2 uVp;
void main(){
  vec2 ndc = vec2(aPx.x / uVp.x * 2.0 - 1.0, 1.0 - aPx.y / uVp.y * 2.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
}
)";
static const char* FS = R"(
uniform vec4 uColor;
out vec4 o;
void main(){ o = vec4(uColor.rgb * uColor.a, uColor.a); }
)";

static uint32_t compile(GLenum t, const char* body) {
    std::string full = std::string(T57_GLSL_VERSION) + body;
    const char* s = full.c_str();
    uint32_t sh = glCreateShader(t); glShaderSource(sh,1,&s,nullptr); glCompileShader(sh);
    GLint ok=0; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
    if(!ok){char l[512];glGetShaderInfoLog(sh,512,nullptr,l);std::fprintf(stderr,"safety shader: %s\n",l);std::exit(2);}
    return sh;
}

bool SafetyOverlay::init() {
    prog_ = glCreateProgram();
    uint32_t v = compile(GL_VERTEX_SHADER, VS), f = compile(GL_FRAGMENT_SHADER, FS);
    glAttachShader(prog_,v); glAttachShader(prog_,f); glLinkProgram(prog_);
    glDeleteShader(v); glDeleteShader(f);
    u_vp_ = glGetUniformLocation(prog_, "uVp");
    u_color_ = glGetUniformLocation(prog_, "uColor");
    glGenVertexArrays(1,&vao_); glGenBuffers(1,&vbo_);
    return true;
}

void SafetyOverlay::draw_quads(const float* xy, int vert_count,
                               float r,float g,float b,float a, uint32_t w, uint32_t h) {
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, vert_count * 2 * sizeof(float), xy, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,2*sizeof(float),(void*)0);
    float vp[2] = { (float)w, (float)h };
    glUniform2fv(u_vp_, 1, vp);
    glUniform4f(u_color_, r, g, b, a);
    glDrawArrays(GL_TRIANGLES, 0, vert_count);
}

static void push_rect(std::vector<float>& v, float x, float y, float w, float h) {
    float x1=x+w, y1=y+h;
    v.insert(v.end(), { x,y, x1,y, x1,y1,  x,y, x1,y1, x,y1 });
}

void SafetyOverlay::render(uint32_t w, uint32_t h) {
    glUseProgram(prog_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    const float barH = 24.f;
    // 1) the warning bar across the top
    std::vector<float> bar; push_rect(bar, 0.f, 0.f, (float)w, barH);
    draw_quads(bar.data(), (int)(bar.size()/2), 0.72f, 0.0f, 0.72f, 0.90f, w, h); // magenta

    // 2) the banner text, centered in the bar (5x7 font, filled px cells)
    const float cell = 2.0f;                 // px per font pixel
    const float gw = 5*cell, gh = 7*cell, adv = gw + cell; // glyph + 1px gap
    int n = (int)std::strlen(BANNER);
    float textW = n * adv - cell;
    float ox = (w - textW) * 0.5f;
    float oy = (barH - gh) * 0.5f;
    std::vector<float> text;
    for (int i = 0; i < n; ++i) {
        const Glyph* g = find_glyph(BANNER[i]);
        float gx = ox + i * adv;
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (g->rows[row] & (1 << (4 - col)))
                    push_rect(text, gx + col*cell, oy + row*cell, cell, cell);
    }
    if (!text.empty())
        draw_quads(text.data(), (int)(text.size()/2), 1.f, 1.f, 1.f, 1.f, w, h); // white
    glBindVertexArray(0);
}

void SafetyOverlay::shutdown() {
    glDeleteProgram(prog_); glDeleteBuffers(1,&vbo_); glDeleteVertexArrays(1,&vao_);
}

} // namespace t57
