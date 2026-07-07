// tile57_chart.h — a first-class OpenCPN plugin chart backed by tile57.
//
// ChartTile57 is a PlugInChartBaseExtended (VECTOR family): OpenCPN discovers a
// baked tile57 bundle's chart.pmtiles via GetFileSearchMask, adds it to the
// chart database like any native chart, and drives it through the chart bar,
// quilting and scale transitions.
//
// Extended (not the legacy PlugInChartBaseGL) matters for quilting: OpenCPN
// draws quilted vector charts in two passes — per-patch geometry
// (RenderRegionViewOnGLNoText, clipped to the patch via a stencil mask the core
// pre-writes) and then ONE decluttered text pass across the whole quilt
// (RenderRegionViewOnGLTextOnly, possibly with a laterally-expanded ViewPort).
// A legacy BaseGL chart is wedged through a per-rect compatibility path that
// composites its full render (text included) at patch granularity, which shows
// seams and doubled text. Rendering reuses the GL vector renderer (see
// chart_renderer); each pass converts the ViewPort into a tile57 camera and
// draws the cached portrayal geometry.
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

class ChartTile57 : public PlugInChartBaseExtended {
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

    // GL render passes. Quilted: NoText per patch (stencil-clipped) + TextOnly
    // once across the quilt. Single-chart mode uses the combined render.
    int RenderRegionViewOnGL(const wxGLContext& glc, const PlugIn_ViewPort& VPoint,
                             const wxRegion& Region, bool b_use_stencil) override;
    int RenderRegionViewOnGLNoText(const wxGLContext& glc, const PlugIn_ViewPort& VPoint,
                                   const wxRegion& Region, bool b_use_stencil) override;
    int RenderRegionViewOnGLTextOnly(const wxGLContext& glc, const PlugIn_ViewPort& VPoint,
                                     const wxRegion& Region, bool b_use_stencil) override;

    // DC fallback (non-GL canvas). This plugin needs OpenGL to draw; on the DC
    // canvas it returns a transparent bitmap so nothing is clobbered.
    wxBitmap& RenderRegionView(const PlugIn_ViewPort& VPoint, const wxRegion& Region) override;
    wxBitmap& RenderRegionViewOnDCNoText(const PlugIn_ViewPort& VPoint, const wxRegion& Region) override;
    bool RenderRegionViewOnDCTextOnly(wxMemoryDC& dc, const PlugIn_ViewPort& VPoint,
                                      const wxRegion& Region) override;

    wxBitmap* GetThumbnail(int tnx, int tny, int cs) override { return nullptr; }

private:
    // Shared per-pass render: ViewPort -> tile57 camera -> draw `pass` buffers.
    int render_pass(const PlugIn_ViewPort& vp, t57::ChartRenderer::Pass pass,
                    bool stencil_clip);
    wxBitmap& transparent_bitmap(const PlugIn_ViewPort& vp);
    // Pull OpenCPN's S52/vector-chart options (soundings, display category,
    // text, contours, …) into the tile57 mariner. Cheap: re-reads only when
    // PI_GetPLIBStateHash() changes.
    void refresh_mariner();

    t57::ChartRenderer renderer_;
    tile57_mariner mariner_{};

    double center_lat_ = 0.0;
    double bounds_west_ = 0.0, bounds_south_ = 0.0, bounds_east_ = 0.0, bounds_north_ = 0.0;
    int min_zoom_ = 0, max_zoom_ = 0;
    bool covr_valid_ = false;
    float covr_[8] = {0};   // 4 points, each (lat, lon), OpenCPN float_2Dpt order

    wxBitmap dc_bmp_;       // backing store for the DC fallback

    int plib_hash_ = -1;    // last PI_GetPLIBStateHash() folded into mariner_

    wxDECLARE_DYNAMIC_CLASS(ChartTile57);
};
