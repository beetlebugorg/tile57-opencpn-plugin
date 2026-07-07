// tile57_chart.h — a first-class OpenCPN plugin chart backed by tile57.
//
// ChartTile57 is a PlugInChartBaseGL (VECTOR family): OpenCPN discovers a baked
// tile57 bundle's chart.pmtiles via GetFileSearchMask, adds it to the chart
// database like any native chart, and drives it through the chart bar, quilting
// and scale transitions. Rendering reuses the GL vector renderer used by the
// former overlay (see chart_renderer): each RenderRegionViewOnGL converts the
// ViewPort into a tile57 camera and paints tile57's live S-52 portrayal.
//
// The class is registered with wxWidgets' dynamic-class system so OpenCPN can
// create instances by name (the name it returns from
// Tile57Plugin::GetDynamicChartClassNameArray).
#pragma once
#include <cstdint>  // ocpn_plugin.h (api-18) references uint8_t before including it
#include "ocpn_plugin.h"
#include "chart_renderer.h"
#include <wx/bitmap.h>
#include <wx/object.h>

extern "C" void tile57_mariner_defaults(tile57_mariner*);

class ChartTile57 : public PlugInChartBaseGL {
public:
    ChartTile57();
    virtual ~ChartTile57();

    // OpenCPN discovers baked tile57 bundles by this mask.
    wxString GetFileSearchMask(void) override { return _T("*.pmtiles"); }

    int Init(const wxString& full_path, int init_flags) override;
    void SetColorScheme(int cs, bool bApplyImmediate) override;

    double GetNormalScaleMin(double canvas_scale_factor, bool b_allow_overzoom) override;
    double GetNormalScaleMax(double canvas_scale_factor, int canvas_width) override;
    double GetNearestPreferredScalePPM(double target_scale_ppm) override;

    bool GetChartExtent(ExtentPI* pext) override;
    void GetValidCanvasRegion(const PlugIn_ViewPort& VPoint, wxRegion* pValidRegion) override;

    // Vector chart coverage: a single table = the chart's bounding rectangle.
    int GetNoCOVREntries() override { return covr_valid_ ? 1 : 0; }
    int GetNoCOVRTablePoints(int iTable) override { return 4; }
    int GetNoCOVRTablenPoints(int iTable) override { return 4; }
    float* GetNoCOVRTableHead(int iTable) override { return covr_; }

    // GL vector render path — the primary path when OpenCPN runs with OpenGL.
    int RenderRegionViewOnGL(const wxGLContext& glc, const PlugIn_ViewPort& VPoint,
                             const wxRegion& Region, bool b_use_stencil) override;
    // DC fallback (non-GL canvas). This plugin needs OpenGL to draw; on the DC
    // canvas it returns a transparent bitmap so nothing is clobbered.
    wxBitmap& RenderRegionView(const PlugIn_ViewPort& VPoint, const wxRegion& Region) override;

    wxBitmap* GetThumbnail(int tnx, int tny, int cs) override { return nullptr; }

private:
    t57::ChartRenderer renderer_;
    tile57_mariner mariner_{};

    double center_lat_ = 0.0;
    double bounds_west_ = 0.0, bounds_south_ = 0.0, bounds_east_ = 0.0, bounds_north_ = 0.0;
    int min_zoom_ = 0, max_zoom_ = 0;
    bool covr_valid_ = false;
    float covr_[8] = {0};   // 4 points, each (lat, lon), OpenCPN float_2Dpt order

    wxBitmap dc_bmp_;       // backing store for the DC fallback

    wxDECLARE_DYNAMIC_CLASS(ChartTile57);
};
