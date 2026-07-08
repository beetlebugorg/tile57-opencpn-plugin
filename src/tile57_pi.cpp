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
#include "bake_manager.h"
#include "gl.h"
#include <chrono>
#include <wx/wx.h>
#include <wx/html/htmlwin.h>

// While cells bake in the background, force periodic canvas redraws so the pre-bake
// progress bar + the per-cell 8-bit loader animate (a background bake doesn't itself
// trigger a redraw). Idle (no baking) => a cheap check, no redraw.
class BakeRefreshTimer : public wxTimer {
public:
    void Notify() override {
        if (t57::BakeManager::instance().pending() > 0) {
            if (wxWindow* w = GetOCPNCanvasWindow()) w->Refresh(false);
        }
    }
};

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
        bake_timer_.Start(110);   // ~9 fps redraws while baking (loader + progress bar)
        return INSTALLS_PLUGIN_CHART | INSTALLS_PLUGIN_CHART_GL
             | WANTS_MOUSE_EVENTS | WANTS_CURSOR_LATLON
             | WANTS_OPENGL_OVERLAY_CALLBACK;   // for the unobtrusive pre-bake indicator
    }
    bool DeInit() override {
        bake_timer_.Stop();
        t57::BakeManager::instance().stop();   // stop + join the pre-bake worker
        if (query_dlg_) { query_dlg_->Destroy(); query_dlg_ = nullptr; }
        return true;
    }

    // Two unobtrusive indicators, both fixed-function so they compose with OpenCPN's GL:
    //   * an animated 8-bit "still baking" loader over each cell that has no tiles yet;
    //   * a slim pre-bake progress bar bottom-left while the background sweep runs.
    bool RenderGLOverlay(wxGLContext* /*ctx*/, PlugIn_ViewPort* vp) override {
        if (!vp) return false;
        auto& bm = t57::BakeManager::instance();
        const int total = bm.total();
        const int pending = bm.pending();

        // Any cells currently loading (no tiles yet)?
        bool any_loading = false;
        for (ChartTile57* c : ChartTile57::instances()) {
            double w, s, e, n;
            if (c && c->loading_extent(w, s, e, n)) { any_loading = true; break; }
        }
        if (!any_loading && (total <= 0 || pending <= 0)) return false;

        const float W = (float)vp->pix_width, H = (float)vp->pix_height;
        glUseProgram(0);   // drop any shader so fixed-function immediate mode draws
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, W, H, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        auto rect = [](float x, float y, float rw, float rh) {
            glBegin(GL_QUADS);
            glVertex2f(x, y); glVertex2f(x + rw, y);
            glVertex2f(x + rw, y + rh); glVertex2f(x, y + rh);
            glEnd();
        };

        // --- 8-bit loader over each baking cell's on-screen bbox (opaque, scrolling) ---
        if (any_loading) {
            static const float pal[][3] = {
                {0.05f, 0.07f, 0.32f}, {0.09f, 0.18f, 0.58f}, {0.15f, 0.50f, 0.85f},
                {0.45f, 0.82f, 1.00f}, {0.88f, 0.96f, 1.00f}, {0.15f, 0.50f, 0.85f},
            };
            const int nc = 6;
            const float barH = 10.f;
            const double t = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count();
            const int off = (int)(t * 13.0);   // scroll speed
            glDisable(GL_BLEND);
            for (ChartTile57* c : ChartTile57::instances()) {
                double bw, bs, be, bn;
                if (!c || !c->loading_extent(bw, bs, be, bn)) continue;
                wxPoint nw, se;
                GetCanvasPixLL(vp, &nw, bn, bw);
                GetCanvasPixLL(vp, &se, bs, be);
                float x0 = std::max(0.f, (float)std::min(nw.x, se.x));
                float y0 = std::max(0.f, (float)std::min(nw.y, se.y));
                float x1 = std::min(W, (float)std::max(nw.x, se.x));
                float y1 = std::min(H, (float)std::max(nw.y, se.y));
                if (x1 <= x0 || y1 <= y0) continue;   // off-screen
                int row = 0;
                for (float y = y0; y < y1; y += barH, ++row) {
                    const int ci = ((row + off) % nc + nc) % nc;
                    glColor3f(pal[ci][0], pal[ci][1], pal[ci][2]);
                    rect(x0, y, x1 - x0, std::min(y + barH, y1) - y);
                }
            }
        }

        // --- pre-bake progress bar (bottom-left) ---
        if (total > 0 && pending > 0) {
            const float frac = (float)(total - pending) / (float)total;
            const float bx = 14.f, bh = 4.f, by = H - 18.f, bw = 150.f;
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(0.f, 0.f, 0.f, 0.40f); rect(bx - 3, by - 3, bw + 6, bh + 6);
            glColor4f(1.f, 1.f, 1.f, 0.22f); rect(bx, by, bw, bh);
            glColor4f(0.30f, 0.72f, 1.f, 0.85f); rect(bx, by, bw * frac, bh);
        }

        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
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
    BakeRefreshTimer bake_timer_;

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
