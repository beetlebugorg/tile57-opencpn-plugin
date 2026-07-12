// tile57_pi.cpp — OpenCPN plugin entry point.
//
// This plugin installs a first-class GL vector chart (see tile57_chart /
// ChartTile57): OpenCPN discovers a baked tile57 bundle's chart.pmtiles, adds it
// to the chart database, and draws it like any native chart — quilting, chart
// bar and scale transitions included. The plugin itself only advertises the
// chart class; the "NOT FOR NAVIGATION" warning (tile57 is experimental) rides
// in the chart's name and description, where OpenCPN surfaces it.
#include "build_charts.h"
#include "ocpn_plugin.h"
#include "tile57_chart.h"
#include <cstdint> // ocpn_plugin.h (api-18) references uint8_t before including it
#include <cstdlib> // getenv (startup build/debug marker)
#include <wx/html/htmlwin.h>
#include <wx/wx.h>

class Tile57Plugin : public opencpn_plugin_119 {
  public:
    explicit Tile57Plugin(void* pmgr) : opencpn_plugin_119(pmgr) {}

    int Init() override {
        wxLogMessage("tile57_pi: initialised [build " __DATE__ " " __TIME__ ", TILE57_DEBUG=%s]",
                     std::getenv("TILE57_DEBUG") ? std::getenv("TILE57_DEBUG") : "(unset)");
        // Populate tile57's process-global read-only registries (S-100 catalogue +
        // linestyles) on this, the main thread, before the chart-DB scan creates any
        // ChartTile57 and the GL thread first renders.
        tile57_warmup();
        return INSTALLS_PLUGIN_CHART | INSTALLS_PLUGIN_CHART_GL | WANTS_MOUSE_EVENTS |
               WANTS_CURSOR_LATLON | WANTS_PREFERENCES | // the "Build Charts" settings panel
               WANTS_PLUGIN_MESSAGING;                   // the "OpenCPN Config" S52 state push
    }

    // OpenCPN broadcasts its live S52PLIB configuration as a JSON message whenever the
    // S52 state is reconfigured. It is the only channel that carries some of the mariner
    // settings — notably the per-canvas data-quality toggle, which the canvas pushes
    // straight into the s52plib and never writes to the config object a plugin can read.
    void SetPluginMessage(wxString& message_id, wxString& message_body) override {
        if (message_id == _T("OpenCPN Config"))
            ChartTile57::ApplyS52ConfigMessage(message_body);
    }
    bool DeInit() override {
        if (query_dlg_) {
            query_dlg_->Destroy();
            query_dlg_ = nullptr;
        }
        if (prefs_dlg_) {
            prefs_dlg_->StopBake();
            prefs_dlg_->Destroy();
            prefs_dlg_ = nullptr;
        }
        return true;
    }

    // Settings panel: lazily create the modeless "Build Charts" dialog and
    // surface it. It bakes an ENC_ROOT to per-cell *.pmtiles and registers the
    // destination as a chart directory (see build_charts.*).
    void ShowPreferencesDialog(wxWindow* parent) override {
        wxLogMessage("tile57: ShowPreferencesDialog enter (parent=%p)", (void*)parent);
        if (!prefs_dlg_) {
            // Parent to the canvas (a stable top-level that outlives the transient
            // preferences window), falling back to the passed parent.
            wxWindow* p = GetOCPNCanvasWindow();
            prefs_dlg_ = new BuildChartsDialog(p ? p : parent);
        }
        prefs_dlg_->Show();
        prefs_dlg_->Raise();
        wxLogMessage("tile57: ShowPreferencesDialog shown");
    }

    int GetAPIVersionMajor() override { return 1; }
    // 1.19, not 1.18: only 1.19+ exposes the per-canvas ENC display state (text,
    // soundings, lights, light descriptions, display category) that the chart mirrors
    // into tile57's mariner settings. See CMakeLists.
    int GetAPIVersionMinor() override { return 19; }
    int GetPlugInVersionMajor() override { return 0; }
    int GetPlugInVersionMinor() override { return 1; }
    wxBitmap* GetPlugInBitmap() override {
        static wxBitmap b(32, 32);
        return &b;
    }
    wxString GetCommonName() override { return _T("tile57 Vector Chart (EXPERIMENTAL)"); }
    wxString GetShortDescription() override {
        return _T("S-57/S-101 vector chart via tile57 — NOT FOR NAVIGATION");
    }
    wxString GetLongDescription() override {
        return _T("Installs an ENC chart driven by tile57's live S-52 portrayal, "
                  "rendered on the GPU. Add the folder holding a baked bundle's "
                  "chart.pmtiles as a chart directory. EXPERIMENTAL / NOT FOR NAVIGATION.");
    }

    // OpenCPN scans our pre-baked *.pmtiles bundles (opened directly, fast). Prepare
    // charts (bake ENC cells -> *.pmtiles) via the Build Charts panel.
    wxArrayString GetDynamicChartClassNameArray() override {
        wxArrayString a;
        a.Add(_T("ChartTile57Pmtiles"));
        return a;
    }

    // OpenCPN feeds us the cursor position in lat/lon as it moves.
    void SetCursorLatLon(double lat, double lon) override {
        cur_lat_ = lat;
        cur_lon_ = lon;
    }

    // Object query on a single click (a press+release that did NOT drag, so
    // panning is unaffected): query the charts under the cursor and show the
    // result in a floating, non-modal panel that updates on each click. We do NOT
    // consume the click, so normal chart interaction still works.
    bool MouseEventHook(wxMouseEvent& event) override {
        if (!event.LeftDClick())
            return false; // double-click = the OpenCPN pick gesture
        wxString body;
        for (auto* c : ChartTile57::instances())
            if (c->covers(cur_lon_, cur_lat_))
                body += c->QueryDescription(cur_lon_, cur_lat_);
        if (body.IsEmpty())
            return false; // nothing here — let OpenCPN handle it
        show_query(body);
        return true; // consume: suppress OpenCPN's own query and its center-on-double-click
    }

  private:
    double cur_lat_ = 0, cur_lon_ = 0;
    wxDialog* query_dlg_ = nullptr;
    wxHtmlWindow* query_html_ = nullptr;
    BuildChartsDialog* prefs_dlg_ = nullptr;

    void show_query(const wxString& body) {
        if (!query_dlg_) {
            query_dlg_ =
                new wxDialog(GetOCPNCanvasWindow(), wxID_ANY, _T("Object Query — tile57"),
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
