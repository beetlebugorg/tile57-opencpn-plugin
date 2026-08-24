// chart_atlases.cpp - see chart_atlases.h.
#include "chart_atlases.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <wx/image.h>
#include <wx/jsonreader.h>
#include <wx/log.h>
#include <wx/mstream.h>

namespace t57 {

namespace {

GlTexture upload_png(const uint8_t* png, size_t len) {
    if (!png || !len)
        return GlTexture();
    wxMemoryInputStream mis(png, len);
    wxImage img;
    if (!img.LoadFile(mis, wxBITMAP_TYPE_PNG))
        return GlTexture();
    const int w = img.GetWidth(), h = img.GetHeight();
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    const uint8_t* rgb = img.GetData();
    const uint8_t* alpha = img.HasAlpha() ? img.GetAlpha() : nullptr;
    for (int p = 0; p < w * h; ++p) {
        rgba[p * 4 + 0] = rgb[p * 3 + 0];
        rgba[p * 4 + 1] = rgb[p * 3 + 1];
        rgba[p * 4 + 2] = rgb[p * 3 + 2];
        rgba[p * 4 + 3] = alpha ? alpha[p] : 255;
    }
    return make_rgba_texture(rgba.data(), w, h, GL_CLAMP_TO_EDGE);
}

// "#RRGGBB" or "#RRGGBBAA" to straight-alpha floats. False when malformed.
bool parse_hex_color(const wxString& hex, float out[4]) {
    std::string s(hex.mb_str());
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8)
        return false;
    unsigned long v = std::strtoul(s.c_str(), nullptr, 16);
    if (s.size() == 6)
        v = (v << 8) | 0xFF;
    out[0] = ((v >> 24) & 0xFF) / 255.f;
    out[1] = ((v >> 16) & 0xFF) / 255.f;
    out[2] = ((v >> 8) & 0xFF) / 255.f;
    out[3] = (v & 0xFF) / 255.f;
    return true;
}

// The context that held a texture may be gone (plugin reload). Drop the id so the
// entry bakes again in the live context.
void revalidate(GlTexture& tex, bool& tried) {
    if (tex && !glIsTexture(tex.id())) {
        tex.release();
        tried = false;
    }
}

} // namespace

ChartAtlases& ChartAtlases::shared() {
    static ChartAtlases a;
    return a;
}

uint32_t ChartAtlases::sprite(double pixel_ratio, tile57_scheme scheme) {
    if (pixel_ratio <= 0)
        pixel_ratio = 1;
    const SpriteKey key{(int)std::lround(pixel_ratio * 100), (int)scheme};
    Entry& e = sprites_[key];
    revalidate(e.tex, e.tried);
    if (!e.tried) {
        e.tried = true;
        tile57_assets a{};
        tile57_error err{};
        if (tile57_bake_sprite_mln(nullptr, pixel_ratio, scheme, &a, &err) == TILE57_OK)
            e.tex = upload_png(a.sprite_png, a.sprite_png_len);
        else
            wxLogMessage("tile57: sprite atlas bake failed: %s", err.message);
        tile57_assets_free(&a);
    }
    return e.tex.id();
}

uint32_t ChartAtlases::glyph(uint8_t atlas) {
    int face;
    switch (atlas) {
    case TILE57_GPU_ATLAS_GLYPH:
        face = 0;
        break;
    case TILE57_GPU_ATLAS_GLYPH_BOLD:
        face = 1;
        break;
    case TILE57_GPU_ATLAS_GLYPH_ITALIC:
        face = 2;
        break;
    default:
        return 0;
    }
    Entry& e = glyphs_[face];
    revalidate(e.tex, e.tried);
    if (!e.tried) {
        e.tried = true;
        tile57_assets a{};
        tile57_error err{};
        if (tile57_bake_glyph_sdf_face(&a, face, &err) == TILE57_OK)
            e.tex = upload_png(a.sprite_png, a.sprite_png_len);
        else
            wxLogMessage("tile57: glyph atlas bake failed (face %d): %s", face, err.message);
        tile57_assets_free(&a);
    }
    return e.tex.id();
}

uint8_t ChartAtlases::available(double pixel_ratio, tile57_scheme scheme) {
    uint8_t have = 0;
    if (sprite(pixel_ratio, scheme))
        have |= 1u << TILE57_GPU_ATLAS_SPRITE;
    for (uint8_t a : {TILE57_GPU_ATLAS_GLYPH, TILE57_GPU_ATLAS_GLYPH_BOLD,
                      TILE57_GPU_ATLAS_GLYPH_ITALIC})
        if (glyph(a))
            have |= 1u << a;
    return have;
}

const float* ChartAtlases::halo(tile57_scheme scheme) {
    if (!halos_loaded_)
        load_halos();
    const int i = (scheme >= 0 && scheme <= 2) ? (int)scheme : 0;
    return halos_[i];
}

// tile57_colortables_default returns {"day":{token:hex,...},"dusk":{...},"night":{...}}.
void ChartAtlases::load_halos() {
    halos_loaded_ = true;
    uint8_t* json = nullptr;
    size_t len = 0;
    tile57_error err{};
    if (tile57_colortables_default(&json, &len, &err) != TILE57_OK) {
        wxLogMessage("tile57: colortables unavailable: %s", err.message);
        return;
    }
    wxJSONValue root;
    wxJSONReader reader;
    const wxString text = wxString::FromUTF8(reinterpret_cast<const char*>(json), len);
    tile57_free(json);
    if (reader.Parse(text, &root) != 0)
        return;
    const char* keys[3] = {"day", "dusk", "night"};
    for (int i = 0; i < 3; ++i) {
        const wxJSONValue palette = root.ItemAt(wxString(keys[i]));
        if (palette.HasMember("NODATA"))
            parse_hex_color(palette.ItemAt("NODATA").AsString(), halos_[i]);
    }
}

void ChartAtlases::invalidate() {
    for (auto& kv : sprites_) {
        kv.second.tex.release();
        kv.second.tried = false;
    }
    for (Entry& g : glyphs_) {
        g.tex.release();
        g.tried = false;
    }
}

} // namespace t57
