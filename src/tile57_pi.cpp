// tile57_pi.cpp — OpenCPN plugin entry point.
//
// A GL overlay plugin that draws an S-57/S-101 chart using tile57's live S-52
// portrayal. On each frame it converts OpenCPN's ViewPort into a tile57 camera,
// asks tile57 to paint the view through a callback canvas, and renders the
// resulting geometry on the GPU (see chart_renderer). A persistent
// "NOT FOR NAVIGATION" banner is drawn over everything (see safety_overlay).
#include "ocpn_plugin.h"
#include "chart_renderer.h"
#include "safety_overlay.h"
#include <wx/wx.h>
#include <cmath>
#include <cstdlib>

extern "C" void tile57_mariner_defaults(tile57_mariner*);

// Continuous web-mercator zoom for a ground resolution (metres/pixel) at a
// latitude:  resolution(z, phi) = cos(phi) * 2*pi*R / (256 * 2^z).
static double zoom_for_resolution(double mpp, double lat_deg) {
    const double R = 6378137.0, PI = 3.14159265358979323846;
    double world_m = std::cos(lat_deg * PI / 180.0) * 2.0 * PI * R;
    return std::log2(world_m / (256.0 * mpp));
}

class Tile57Plugin : public opencpn_plugin_118 {
public:
    explicit Tile57Plugin(void* pmgr) : opencpn_plugin_118(pmgr) {
        tile57_mariner_defaults(&mariner_);
    }

    int Init() override {
        // Chart location: a baked tile57 bundle's chart.pmtiles, from the
        // environment or a default.
        const char* p = std::getenv("OPENCPN_T57_CHART");
        chart_path_ = p ? p : "/tmp/baked_enc/tiles/chart.pmtiles";
        return WANTS_OPENGL_OVERLAY_CALLBACK | WANTS_PREFERENCES;
    }
    bool DeInit() override { renderer_.shutdown(); safety_.shutdown(); return true; }

    int GetAPIVersionMajor() override { return 1; }
    int GetAPIVersionMinor() override { return 18; }
    int GetPlugInVersionMajor() override { return 0; }
    int GetPlugInVersionMinor() override { return 1; }
    wxBitmap* GetPlugInBitmap() override { static wxBitmap b(32, 32); return &b; }
    wxString GetCommonName() override      { return _T("tile57 Vector Chart (EXPERIMENTAL)"); }
    wxString GetShortDescription() override { return _T("S-57/S-101 vector chart via tile57 — NOT FOR NAVIGATION"); }
    wxString GetLongDescription() override  { return _T("Draws an ENC using tile57's live S-52 portrayal, rendered on the "
                                                        "GPU. EXPERIMENTAL / NOT FOR NAVIGATION."); }

    bool RenderGLOverlayMultiCanvas(wxGLContext*, PlugIn_ViewPort* vp, int, int) override {
        if (!vp || !vp->bValid) return false;
        if (!renderer_.has_chart()) {
            if (!renderer_.open_chart(chart_path_)) {
                wxLogMessage("tile57_pi: failed to open chart %s", chart_path_.c_str());
                return false;
            }
            renderer_.ensure_gl();
            safety_.init();
        }
        // OpenCPN ViewPort -> tile57 camera (centre + web-mercator zoom).
        double mpp = (vp->view_scale_ppm > 0) ? 1.0 / vp->view_scale_ppm : 100.0;
        double zoom = zoom_for_resolution(mpp, vp->clat);
        renderer_.render(vp->clon, vp->clat, zoom,
                         (uint32_t)vp->pix_width, (uint32_t)vp->pix_height,
                         mariner_, /*draw_text=*/true);
        safety_.render((uint32_t)vp->pix_width, (uint32_t)vp->pix_height);
        return true;
    }

private:
    t57::ChartRenderer renderer_;
    t57::SafetyOverlay safety_;
    tile57_mariner mariner_{};
    std::string chart_path_;
};

extern "C" DECL_EXP opencpn_plugin* create_pi(void* pmgr) { return new Tile57Plugin(pmgr); }
extern "C" DECL_EXP void destroy_pi(opencpn_plugin* p) { delete p; }
