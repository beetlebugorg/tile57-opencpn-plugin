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
#include <vector>
#include <atomic>
#include <thread>
#include <string>
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
    // *.pmtiles = baked bundles; *.t57 = a symlink to a live .000 cell (a plugin-owned
    // extension so OpenCPN's built-in S-57 engine doesn't claim it — see scripts).
    wxString GetFileSearchMask(void) override { return _T("*.t57"); }

    int Init(const wxString& full_path, int init_flags) override;
    void SetColorScheme(int cs, bool bApplyImmediate) override;

    // Ready as soon as Init returns (metadata + coverage are loaded). Tiles bake on a
    // background thread and stream in via the render path; reporting "not ready" here
    // instead makes OpenCPN destroy + re-create the chart every frame (re-baking
    // forever). So we stay ready and simply draw nothing until the first band lands.
    bool IsReadyToRender() override { return true; }

    double GetNormalScaleMin(double canvas_scale_factor, bool b_allow_overzoom) override;
    double GetNormalScaleMax(double canvas_scale_factor, int canvas_width) override;
    double GetNearestPreferredScalePPM(double target_scale_ppm) override;

    bool GetChartExtent(ExtentPI* pext) override;
    void GetValidCanvasRegion(const PlugIn_ViewPort& VPoint, wxRegion* pValidRegion) override;

    // Vector chart coverage (GetCOVR*, not GetNoCOVR* — that's the exclusion tables):
    // the cell's real M_COVR data-coverage polygons when available (so OpenCPN quilts
    // gaps to coarser cells), else the bounding rectangle (baked pmtiles carry no
    // coverage polygon).
    int GetCOVREntries() override {
        return covr_tables_.empty() ? (covr_valid_ ? 1 : 0) : (int)covr_tables_.size();
    }
    int GetCOVRTablePoints(int iTable) override {
        return covr_tables_.empty() ? 4 : (int)(covr_tables_[iTable].size() / 2);
    }
    int GetCOVRTablenPoints(int iTable) override { return GetCOVRTablePoints(iTable); }
    float* GetCOVRTableHead(int iTable) override {
        return covr_tables_.empty() ? covr_ : covr_tables_[iTable].data();
    }

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

    // Object query (S-52 §10.8 pick). The plugin drives this from its own mouse
    // handler rather than OpenCPN's GetObjRuleListAtLatLon path, whose PI_S57Obj
    // constructor isn't exported on macOS. QueryDescription returns HTML feature
    // blocks for the features under (lon,lat); covers() is the coverage test;
    // instances() is the live registry of open charts.
    wxString QueryDescription(double lon, double lat) const;
    bool covers(double lon, double lat) const;
    static const std::vector<ChartTile57*>& instances();

    // True while this cell is baking its tiles and has nothing to draw yet — the plugin
    // overlay paints the animated 8-bit loader over this bbox meanwhile. Writes bounds
    // and returns true only when loading.
    bool loading_extent(double& w, double& s, double& e, double& n) const {
        if (!loading_.load(std::memory_order_acquire)) return false;
        w = bounds_west_; s = bounds_south_; e = bounds_east_; n = bounds_north_;
        return true;
    }

private:
    // Extract bbox / native scale / M_COVR coverage from an open chart handle into
    // this chart's members (GetChartExtent / m_Chart_Scale / GetCOVR*).
    void apply_info(tile57_chart* h, const tile57_chart_info& info);
    // The on-disk per-cell tile cache path (keyed by cell + freshest mtime + update
    // count + zoom + bake version), so the slow bake is one-time across runs.
    std::string cache_path(const std::string& cell_path, uint8_t maxz) const;
    // Background bake (full init): bake the quick native band then the full range,
    // handing each result to the render thread via pending_chart_. See tile57_chart.cpp
    // for the thread-safety contract (single-writer chart_, atomic handoff, warmup).
    void start_bake();
    void publish(tile57_chart* c);     // bake thread -> render thread handoff
    void adopt_pending();              // render thread: swap in a freshly baked chart

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
    double last_zoom_ = 0;   // last rendered view zoom, for the object-query pick

    double center_lat_ = 0.0;
    double bounds_west_ = 0.0, bounds_south_ = 0.0, bounds_east_ = 0.0, bounds_north_ = 0.0;
    int min_zoom_ = 0, max_zoom_ = 0;
    bool covr_valid_ = false;
    float covr_[8] = {0};   // bbox fallback: 4 points, each (lat, lon), float_2Dpt order
    // Real M_COVR coverage: one entry per polygon, a flat (lat,lon) float array. Empty
    // => fall back to covr_ (the bbox), e.g. for a baked pmtiles chart.
    std::vector<std::vector<float>> covr_tables_;

    wxBitmap dc_bmp_;       // backing store for the DC fallback

    int plib_hash_ = -1;    // last PI_GetPLIBStateHash() folded into mariner_

    // --- Background progressive bake (full init only) ---
    // Thread model (portable, no platform APIs): the bake thread NEVER touches the
    // renderer's chart_; it publishes each baked handle into `pending_chart_` (a
    // lock-free atomic pointer). The main/GL thread is the sole consumer/owner of
    // chart_ (adopt_pending swaps it in during render). Each handle is closed exactly
    // once, by one thread. `ready_` gates IsReadyToRender(). Global tile57 registries
    // are warmed on the main thread at plugin load (tile57_warmup) so the concurrent
    // bake only READS them; the allocator is thread-safe and portrayal is threadlocal.
    std::atomic<bool> ready_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> loading_{false};   // baking, nothing to draw yet -> show the loader
    std::atomic<tile57_chart*> pending_chart_{nullptr};
    std::thread bake_thread_;
    std::string bake_path_;             // resolved .000 path (set before the thread starts)
    std::string cache_file_;            // on-disk baked-tile cache for this cell
    uint8_t bake_quick_max_ = 12;       // quick-phase max zoom (coarse first paint)
    uint8_t bake_full_max_ = 16;        // full-phase max zoom = the cell's native zoom
                                        // (baking past it overzooms => tile-count blowup)
    wxWindow* canvas_ = nullptr;        // for a thread-safe CallAfter refresh

    wxDECLARE_DYNAMIC_CLASS(ChartTile57);
};

// Two concrete chart classes so OpenCPN scans BOTH extensions and the user can switch
// renderers by which folder is added: baked bundles (*.pmtiles, ENC_ROOT_TILES) or
// live cells (*.t57, ENC_ROOT_CELLS). GetFileSearchMask can't return multiple masks
// (OpenCPN uses the whole string as one wildcard), so each is its own dynamic class;
// all behaviour lives in the shared base (Init branches on the file extension).
class ChartTile57Pmtiles : public ChartTile57 {
public:
    wxString GetFileSearchMask(void) override { return _T("*.pmtiles"); }
private:
    wxDECLARE_DYNAMIC_CLASS(ChartTile57Pmtiles);
};

class ChartTile57Cell : public ChartTile57 {
public:
    wxString GetFileSearchMask(void) override { return _T("*.t57"); }
private:
    wxDECLARE_DYNAMIC_CLASS(ChartTile57Cell);
};
