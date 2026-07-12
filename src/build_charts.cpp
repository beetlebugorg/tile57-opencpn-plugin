// build_charts.cpp — see build_charts.h.
#include "build_charts.h"

#include "ocpn_plugin.h"
#include "tile57.h"

#include <wx/button.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/filepicker.h>
#include <wx/gauge.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kTimerMs = 200;

// Persisted config in a PLUGIN-OWNED ini file (wxStandardPaths user-config dir) —
// deliberately NOT GetOCPNConfigObject(): that host symbol is resolved lazily via
// -undefined dynamic_lookup and, if a given OpenCPN build doesn't export it, calling
// it jumps to a bad address and crashes the moment the dialog is constructed. A
// self-owned wxFileConfig depends only on the wx libs the plugin already links.
wxString ConfigPath() {
    return wxFileName(wxStandardPaths::Get().GetUserConfigDir(), _T("tile57_pi.ini")).GetFullPath();
}

// Where baked charts land when the user hasn't chosen somewhere else: an XDG-cache dir
// the plugin owns. The output dir is the plugin's OWN chart library — it reads its
// *.pmtiles from here, never the raw .000 — so it defaults somewhere private and
// disposable, but nothing depends on the location: tile57_bake_tree lets the caller own
// the cache, so pointing it at an existing baked tree (one the tile57 CLI or another
// host filled) works and simply skips everything already current.
std::string DefaultDest() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    fs::path base = (xdg && *xdg) ? fs::path(xdg) : fs::path(home ? home : "/tmp") / ".cache";
    return (base / "tile57" / "charts").string();
}

void LoadPaths(wxString& enc, wxString& out) {
    wxFileConfig cfg(wxEmptyString, wxEmptyString, ConfigPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    cfg.SetPath(_T("/tile57"));
    cfg.Read(_T("EncSource"), &enc, wxEmptyString);
    cfg.Read(_T("OutputDir"), &out, wxEmptyString);
}

void SavePaths(const wxString& enc, const wxString& out) {
    wxFileConfig cfg(wxEmptyString, wxEmptyString, ConfigPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    cfg.SetPath(_T("/tile57"));
    cfg.Write(_T("EncSource"), enc);
    cfg.Write(_T("OutputDir"), out);
    cfg.Flush();
}

// The archives (and .sha sidecars) tile57_bake_tree writes. Rebuild All deletes exactly
// these and nothing else: the output dir is now user-chosen, so it may hold files the
// plugin didn't create and has no business removing.
bool IsBakeOutput(const fs::path& p) {
    const std::string s = p.filename().string();
    auto ends_with = [&](const char* suf) {
        const size_t n = std::char_traits<char>::length(suf);
        return s.size() > n && s.compare(s.size() - n, n, suf) == 0;
    };
    return ends_with(".pmtiles") || ends_with(".pmtiles.sha");
}

// Does `dir` hold any S-57 base cell at all? tile57_bake_tree reports "baked 0" both for
// a warm cache AND for a directory with nothing to bake, so without this an ENC folder
// picked by mistake would come back as the cheerful "up to date". Stops at the first hit.
bool HasEncCells(const std::string& dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (ext == ".000" && it->is_regular_file(ec))
            return true;
    }
    return false;
}

std::vector<fs::path> FindBakeOutputs(const std::string& dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_regular_file(ec) && IsBakeOutput(it->path()))
            v.push_back(it->path());
    }
    return v;
}

// Seconds -> "M:SS" (or "H:MM:SS" past an hour) for the timing readout.
wxString FmtDur(double secs) {
    if (secs < 0 || !std::isfinite(secs))
        return _T("—");
    long s = (long)(secs + 0.5);
    long h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    return h > 0 ? wxString::Format(_T("%ld:%02ld:%02ld"), h, m, sec)
                 : wxString::Format(_T("%ld:%02ld"), m, sec);
}

} // namespace

BuildChartsDialog::BuildChartsDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _T("Build Charts — tile57"), wxDefaultPosition, wxSize(520, 300),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    wxLogMessage("tile57: BuildChartsDialog ctor begin");
    // ONE left/right margin for every row (labels AND inputs) so they align; all
    // vertical spacing is explicit AddSpacer — the earlier mix of a wide label margin
    // and a narrow input margin made the inputs run to the edges and read as cramped.
    const int B = 18;
    auto* top = new wxBoxSizer(wxVERTICAL);
    auto label = [&](const wxString& t) {
        top->Add(new wxStaticText(this, wxID_ANY, t), 0, wxLEFT | wxRIGHT, B);
        top->AddSpacer(5);
    };
    auto row = [&](wxWindow* w) { top->Add(w, 0, wxEXPAND | wxLEFT | wxRIGHT, B); };

    top->AddSpacer(B);
    label(_T("ENC source (ENC_ROOT):"));
    encPicker_ = new wxDirPickerCtrl(this, wxID_ANY, wxEmptyString, _T("Select ENC source folder"),
                                     wxDefaultPosition, wxDefaultSize,
                                     wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
    row(encPicker_);

    top->AddSpacer(16);
    label(_T("Output (baked charts):"));
    // No wxDIRP_DIR_MUST_EXIST: the destination is created on demand, and the default
    // cache dir legitimately doesn't exist until the first bake.
    outPicker_ = new wxDirPickerCtrl(this, wxID_ANY, wxEmptyString, _T("Select output folder"),
                                     wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
    row(outPicker_);

    top->AddSpacer(24);
    gauge_ = new wxGauge(this, wxID_ANY, 100);
    row(gauge_);
    top->AddSpacer(10);
    status_ = new wxStaticText(this, wxID_ANY, _T("Idle"));
    row(status_);
    top->AddSpacer(4);
    stats_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
    row(stats_);

    top->AddSpacer(24);
    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    buildBtn_ = new wxButton(this, wxID_ANY, _T("Build Charts"));
    rebuildBtn_ = new wxButton(this, wxID_ANY, _T("Rebuild All"));
    cancelBtn_ = new wxButton(this, wxID_ANY, _T("Cancel"));
    cancelBtn_->Enable(false);
    auto* closeBtn = new wxButton(this, wxID_CLOSE, _T("Close"));
    btns->Add(buildBtn_, 0, wxRIGHT, 10);
    btns->Add(rebuildBtn_, 0, wxRIGHT, 10);
    btns->Add(cancelBtn_, 0, wxRIGHT, 10);
    btns->AddStretchSpacer();
    btns->Add(closeBtn, 0);
    top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT, B);
    top->AddSpacer(B);

    SetSizerAndFit(top);
    SetSize(wxSize(600, GetSize().GetHeight())); // widen for readable paths
    SetMinSize(GetSize());

    // Restore persisted settings. The output field is left EMPTY when unset rather than
    // pre-filled with the default: an empty field means "use the default", so the
    // default can move (or the plugin can be reinstalled) without a stale path stuck in
    // the ini. The placeholder below shows what empty resolves to.
    wxLogMessage("tile57: BuildChartsDialog loading config");
    wxString enc, out;
    LoadPaths(enc, out);
    if (!enc.IsEmpty())
        encPicker_->SetPath(enc);
    if (!out.IsEmpty())
        outPicker_->SetPath(out);
    else
        outPicker_->SetPath(wxString::FromUTF8(DefaultDest().c_str()));

    buildBtn_->Bind(wxEVT_BUTTON, &BuildChartsDialog::OnBuild, this);
    rebuildBtn_->Bind(wxEVT_BUTTON, &BuildChartsDialog::OnRebuild, this);
    cancelBtn_->Bind(wxEVT_BUTTON, &BuildChartsDialog::OnCancel, this);
    closeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    Bind(wxEVT_CLOSE_WINDOW, &BuildChartsDialog::OnClose, this);
    timer_.SetOwner(this);
    Bind(wxEVT_TIMER, &BuildChartsDialog::OnTimer, this);
    wxLogMessage("tile57: BuildChartsDialog ctor end, size=%dx%d", GetSize().GetWidth(),
                 GetSize().GetHeight());
}

BuildChartsDialog::~BuildChartsDialog() { StopBake(); }

void BuildChartsDialog::OnBuild(wxCommandEvent&) { StartBuild(false); }
void BuildChartsDialog::OnRebuild(wxCommandEvent&) { StartBuild(true); }

std::string BuildChartsDialog::DestDir() const {
    const wxString p = outPicker_->GetPath();
    if (p.IsEmpty())
        return DefaultDest();
    return std::string(p.mb_str());
}

void BuildChartsDialog::SetRunningUI(bool running) {
    buildBtn_->Enable(!running);
    rebuildBtn_->Enable(!running);
    cancelBtn_->Enable(running);
    encPicker_->Enable(!running);
    outPicker_->Enable(!running);
}

// Engine bake thread — atomics only. Returning false cancels the bake (tile57 stops
// picking up charts; the ones in flight finish), which is what lets StopBake() join
// promptly instead of blocking for the whole tree.
bool BuildChartsDialog::ProgressTick(void* ctx, uint32_t done, uint32_t total) {
    auto* self = static_cast<BuildChartsDialog*>(ctx);
    self->total_.store(total);
    self->done_.store(done);
    return !self->cancel_.load();
}

void BuildChartsDialog::StartBuild(bool force) {
    if (running_.load())
        return;

    const std::string enc = std::string(encPicker_->GetPath().mb_str());
    const std::string dest = DestDir();

    std::error_code ec;
    if (enc.empty() || !fs::is_directory(enc, ec)) {
        status_->SetLabel(_T("ENC source folder does not exist"));
        return;
    }
    if (!HasEncCells(enc)) {
        status_->SetLabel(_T("No .000 cells found under the ENC source folder"));
        return;
    }
    fs::create_directories(dest, ec);
    if (ec || !fs::is_directory(dest, ec)) {
        status_->SetLabel(_T("Cannot create the output folder"));
        return;
    }

    // Rebuild All: tile57_bake_tree is incremental by construction (it skips any archive
    // already newer than its chart), so forcing a re-bake means removing what's there.
    // The output dir is user-chosen, so confirm first and delete only our own outputs.
    if (force) {
        const std::vector<fs::path> outs = FindBakeOutputs(dest);
        if (!outs.empty()) {
            const wxString q =
                wxString::Format(_T("Rebuild All deletes %d baked file(s) under\n\n%s\n\n"
                                    "and bakes every chart again. Continue?"),
                                 (int)outs.size(), wxString::FromUTF8(dest.c_str()));
            if (wxMessageBox(q, _T("Rebuild All — tile57"),
                             wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES)
                return;
            // A file we fail to delete stays newer than its chart, so the engine would
            // skip it and "Rebuild All" would quietly not rebuild it. Say so instead.
            int undeleted = 0;
            for (const auto& p : outs) {
                std::error_code rm;
                fs::remove(p, rm);
                if (rm)
                    ++undeleted;
            }
            if (undeleted > 0) {
                status_->SetLabel(wxString::Format(
                    _T("Could not delete %d baked file(s) — they will not be rebuilt"), undeleted));
                return;
            }
        }
    }

    SavePaths(encPicker_->GetPath(), outPicker_->GetPath());

    // Arm shared state, flip the UI to "running", and hand the whole tree to the engine.
    dest_ = dest;
    force_ = force;
    total_.store(0);
    done_.store(0);
    baked_.store(0);
    cancel_.store(false);
    finished_.store(false);
    failed_.store(false);
    err_msg_.clear();
    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    gauge_->SetValue(0);
    SetRunningUI(true);
    // The chart count isn't known until the engine has walked the tree and applied the
    // incremental skip, so the first tick is what fills it in.
    status_->SetLabel(force ? _T("Rebuilding — scanning…") : _T("Scanning…"));
    stats_->SetLabel(wxEmptyString);

    coordinator_ = std::thread([this, enc, dest]() {
        // workers is a MEMORY bound, not a core count: each concurrent bake holds a
        // whole chart's parse+portray+raster working set. One per core (less the UI
        // thread), capped at 8.
        const unsigned hw = std::thread::hardware_concurrency();
        const uint32_t workers = hw > 1 ? std::min(8u, hw - 1) : 1u;

        uint32_t baked = 0;
        tile57_error err{};
        const tile57_status st =
            tile57_bake_tree(enc.c_str(), dest.c_str(), workers, &BuildChartsDialog::ProgressTick,
                             this, &baked, &err);
        baked_.store(baked);
        if (st != TILE57_OK) {
            failed_.store(true);
            err_msg_ = err.message[0] ? err.message : tile57_status_str(st);
        }
        finished_.store(true); // the timer joins us
    });
    timer_.Start(kTimerMs);
}

void BuildChartsDialog::OnTimer(wxTimerEvent&) {
    const uint32_t total = total_.load();
    const uint32_t done = done_.load();

    if (finished_.load()) {
        Finish();
        return;
    }

    // total is 0 until the engine's first tick — it walks the tree and skips the
    // already-current archives before it bakes anything, and on a big tree that takes a
    // moment. Pulse rather than show a bogus 0/0 bar.
    if (total == 0) {
        gauge_->Pulse();
        return;
    }
    gauge_->SetRange((int)total);
    gauge_->SetValue((int)std::min(done, total));
    status_->SetLabel(
        wxString::Format(_T("%s… (%u/%u)"), force_ ? _T("Rebuilding") : _T("Baking"), done, total));

    // Every tick is a chart actually baked this run (the engine skips current archives
    // BEFORE the first tick, so they never inflate the rate).
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
    const double rate = (done > 0 && elapsed > 0.3) ? done / elapsed : 0.0; // charts/s
    const double eta = rate > 1e-6 ? (total - done) / rate : -1.0;
    stats_->SetLabel(wxString::Format(_T("elapsed %s   ·   %.1f charts/s   ·   ETA %s"),
                                      FmtDur(elapsed), rate, FmtDur(eta)));
}

void BuildChartsDialog::Finish() {
    // Timer is still on the main thread; stop it before joining so it can't re-enter
    // while the coordinator winds down.
    timer_.Stop();
    if (coordinator_.joinable())
        coordinator_.join();

    running_.store(false);
    SetRunningUI(false);

    const uint32_t total = total_.load();
    const uint32_t done = done_.load();
    const uint32_t baked = baked_.load();
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
    const double rate = (baked > 0 && elapsed > 0.1) ? baked / elapsed : 0.0;
    gauge_->SetRange((int)std::max(1u, total));
    gauge_->SetValue((int)std::min(done, total));

    if (failed_.load()) {
        status_->SetLabel(_T("Bake failed"));
        stats_->SetLabel(wxString::FromUTF8(err_msg_.c_str()));
        return;
    }

    if (cancel_.load()) {
        // Cancelling is not a failure: every archive the engine finished is complete, so
        // Build picks up from here (it re-skips them).
        status_->SetLabel(_T("Cancelled"));
        stats_->SetLabel(wxString::Format(
            _T("stopped after %s · %u chart(s) baked — Build resumes"), FmtDur(elapsed), baked));
        return;
    }

    // A warm cache ticks nothing at all: the engine skipped every chart, so there was
    // never a first progress callback. That's success, not an empty run.
    if (total == 0) {
        status_->SetLabel(_T("Up to date — nothing to bake"));
        stats_->SetLabel(wxString::Format(_T("every chart under %s is current"),
                                          wxString::FromUTF8(dest_.c_str())));
    } else {
        const uint32_t failed = done > baked ? done - baked : 0;
        wxString msg = wxString::Format(_T("Done — %u chart(s) baked"), baked);
        if (failed > 0)
            msg += wxString::Format(_T(" (%u failed)"), failed);
        status_->SetLabel(msg);
        stats_->SetLabel(wxString::Format(_T("baked %u in %s   ·   %.1f charts/s avg"), baked,
                                          FmtDur(elapsed), rate));
    }

    // Main thread only: register the output dir as a chart directory and rescan. Done
    // even on a warm cache — the user may be pointing OpenCPN at an already-baked tree,
    // which is exactly the case a fixed cache path used to make impossible.
    wxString destWx = wxString::FromUTF8(dest_.c_str()); // AddChartDirectory takes wxString&
    AddChartDirectory(destWx);
    ForceChartDBUpdate();
}

void BuildChartsDialog::OnCancel(wxCommandEvent&) {
    if (!running_.load())
        return;
    // Cancelling is not instant — the charts already in flight still have to finish — so
    // say so and paint it before StopBake() blocks on the join.
    status_->SetLabel(_T("Cancelling…"));
    Update();
    StopBake();
    Finish();
}

void BuildChartsDialog::OnClose(wxCloseEvent& e) {
    StopBake();
    // Modeless: hide for reuse rather than destroy (the plugin owns the ptr and
    // Destroy()s it in DeInit).
    Hide();
    e.Veto();
}

void BuildChartsDialog::StopBake() {
    // Never let the timer touch state mid-teardown.
    timer_.Stop();
    // The engine's next progress tick reads this, returns false, and unwinds the bake —
    // so the join below returns in about one chart's bake time, not one tree's.
    cancel_.store(true);
    if (coordinator_.joinable())
        coordinator_.join();
    running_.store(false);
}
