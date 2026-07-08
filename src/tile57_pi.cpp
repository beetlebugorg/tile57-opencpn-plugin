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
#include <wx/html/htmlwin.h>

class Tile57Plugin : public opencpn_plugin_118 {
public:
    explicit Tile57Plugin(void* pmgr) : opencpn_plugin_118(pmgr) {}

    int Init() override {
        wxLogMessage("tile57_pi: initialised");
        // Populate tile57's process-global read-only registries (S-100 catalogue +
        // linestyles) on this, the main thread, BEFORE any chart spawns a background
        // bake thread — thereafter they're read-only, so concurrent bake/render is
        // race-free. Must precede the chart-DB scan (which creates ChartTile57s).
        tile57_warmup();
        return INSTALLS_PLUGIN_CHART | INSTALLS_PLUGIN_CHART_GL
             | WANTS_MOUSE_EVENTS | WANTS_CURSOR_LATLON;
    }
    bool DeInit() override {
        if (query_dlg_) { query_dlg_->Destroy(); query_dlg_ = nullptr; }
        return true;
    }

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

    // Register both chart classes so OpenCPN scans baked bundles (*.pmtiles) and live
    // cells (*.t57) — one dynamic class per extension (see the ChartTile57 subclasses).
    wxArrayString GetDynamicChartClassNameArray() override {
        wxArrayString a;
        a.Add(_T("ChartTile57Pmtiles"));
        a.Add(_T("ChartTile57Cell"));
        return a;
    }

    // OpenCPN feeds us the cursor position in lat/lon as it moves.
    void SetCursorLatLon(double lat, double lon) override { cur_lat_ = lat; cur_lon_ = lon; }

    // Object query on a single click (a press+release that did NOT drag, so
    // panning is unaffected): query the charts under the cursor and show the
    // result in a floating, non-modal panel that updates on each click. We do NOT
    // consume the click, so normal chart interaction still works.
    bool MouseEventHook(wxMouseEvent& event) override {
        if (!event.LeftDClick()) return false;   // double-click = the OpenCPN pick gesture
        wxString body;
        for (auto* c : ChartTile57::instances())
            if (c->covers(cur_lon_, cur_lat_)) body += c->QueryDescription(cur_lon_, cur_lat_);
        if (body.IsEmpty()) return false;        // nothing here — let OpenCPN handle it
        show_query(body);
        return true;   // consume: suppress OpenCPN's own query and its center-on-double-click
    }

private:
    double cur_lat_ = 0, cur_lon_ = 0;
    wxDialog* query_dlg_ = nullptr;
    wxHtmlWindow* query_html_ = nullptr;

    void show_query(const wxString& body) {
        if (!query_dlg_) {
            query_dlg_ = new wxDialog(GetOCPNCanvasWindow(), wxID_ANY, _T("Object Query — tile57"),
                                      wxDefaultPosition, wxSize(440, 500),
                                      wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT);
            auto* sizer = new wxBoxSizer(wxVERTICAL);
            query_html_ = new wxHtmlWindow(query_dlg_);
            sizer->Add(query_html_, 1, wxEXPAND | wxALL, 4);
            query_dlg_->SetSizer(sizer);
            // Closing the panel hides it (keep it for reuse) rather than deleting.
            query_dlg_->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { query_dlg_->Hide(); });
        }
        query_html_->SetPage("<html><body>" + body + "</body></html>");
        query_dlg_->Show();
        query_dlg_->Raise();
    }
};

extern "C" DECL_EXP opencpn_plugin* create_pi(void* pmgr) { return new Tile57Plugin(pmgr); }
extern "C" DECL_EXP void destroy_pi(opencpn_plugin* p) { delete p; }
