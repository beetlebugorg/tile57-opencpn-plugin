// tile57_pi.cpp — OpenCPN plugin entry point.
//
// This plugin installs a first-class GL vector chart (see tile57_chart /
// ChartTile57): OpenCPN discovers a baked tile57 bundle's chart.pmtiles, adds it
// to the chart database, and draws it like any native chart — quilting, chart
// bar and scale transitions included. The plugin itself only advertises the
// chart class; the "NOT FOR NAVIGATION" warning (tile57 is experimental) rides
// in the chart's name and description, where OpenCPN surfaces it.
#include <cstdint>  // ocpn_plugin.h (api-18) references uint8_t before including it
#include "ocpn_plugin.h"
#include "tile57_chart.h"
#include <wx/wx.h>

class Tile57Plugin : public opencpn_plugin_118 {
public:
    explicit Tile57Plugin(void* pmgr) : opencpn_plugin_118(pmgr) {}

    int Init() override {
        wxLogMessage("tile57_pi: initialised");
        return INSTALLS_PLUGIN_CHART | INSTALLS_PLUGIN_CHART_GL;
    }
    bool DeInit() override { return true; }

    int GetAPIVersionMajor() override { return 1; }
    int GetAPIVersionMinor() override { return 18; }
    int GetPlugInVersionMajor() override { return 0; }
    int GetPlugInVersionMinor() override { return 1; }
    wxBitmap* GetPlugInBitmap() override { static wxBitmap b(32, 32); return &b; }
    wxString GetCommonName() override      { return _T("tile57 Vector Chart (EXPERIMENTAL)"); }
    wxString GetShortDescription() override { return _T("S-57/S-101 vector chart via tile57 — NOT FOR NAVIGATION"); }
    wxString GetLongDescription() override  { return _T("Installs an ENC chart driven by tile57's live S-52 portrayal, "
                                                        "rendered on the GPU. Add the folder holding a baked bundle's "
                                                        "chart.pmtiles as a chart directory. EXPERIMENTAL / NOT FOR NAVIGATION."); }

    // Register the plugin chart class so OpenCPN instantiates ChartTile57 for
    // every *.pmtiles it finds (see ChartTile57::GetFileSearchMask).
    wxArrayString GetDynamicChartClassNameArray() override {
        wxArrayString a;
        a.Add(_T("ChartTile57"));
        return a;
    }
};

extern "C" DECL_EXP opencpn_plugin* create_pi(void* pmgr) { return new Tile57Plugin(pmgr); }
extern "C" DECL_EXP void destroy_pi(opencpn_plugin* p) { delete p; }
