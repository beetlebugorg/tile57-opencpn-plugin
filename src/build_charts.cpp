// build_charts.cpp — see build_charts.h.
//
// Threading/lifetime: ONE coordinator std::thread launches N worker threads
// (~hardware_concurrency, one bake per core — a single-chart bake is
// single-threaded) that pull cell paths from a shared deque. The
// coordinator joins all workers then flips finished_. The main-thread timer
// polls the atomics for the gauge/status and, on finished_, joins the
// coordinator, stops the timer, and (if not cancelled) registers the
// destination — the only place OpenCPN API calls happen. StopBake() sets
// cancel_, stops the timer (so it can't touch state mid-teardown) and joins the
// coordinator; it's called on Cancel, dialog Close, and DeInit, guaranteeing
// every thread is joined before this dialog (which owns the shared state) dies.
#include "build_charts.h"

#include "ocpn_plugin.h"
#include "tile57.h"
#include <cstdint>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/filepicker.h>
#include <wx/gauge.h>
#include <wx/log.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

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

// Baked charts always land in a FIXED XDG-cache dir (no picker) — the plugin reads its
// OWN *.pmtiles from here (never the raw .000), which is the whole "load tiles only" win.
std::string FixedDest() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    fs::path base = (xdg && *xdg) ? fs::path(xdg) : fs::path(home ? home : "/tmp") / ".cache";
    return (base / "tile57" / "charts").string();
}

void LoadPaths(wxString& enc) {
    wxFileConfig cfg(wxEmptyString, wxEmptyString, ConfigPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    cfg.SetPath(_T("/tile57"));
    cfg.Read(_T("EncSource"), &enc, wxEmptyString);
}

void SavePaths(const wxString& enc) {
    wxFileConfig cfg(wxEmptyString, wxEmptyString, ConfigPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    cfg.SetPath(_T("/tile57"));
    cfg.Write(_T("EncSource"), enc);
    cfg.Flush();
}

bool IsDotZeroZeroZero(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".000";
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
    : wxDialog(parent, wxID_ANY, _T("Build Charts — tile57"), wxDefaultPosition, wxSize(520, 260),
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
    row(new wxStaticText(
        this, wxID_ANY,
        wxString::Format(_T("Output: %s"), wxString::FromUTF8(FixedDest().c_str()))));

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

    // Restore persisted settings.
    wxLogMessage("tile57: BuildChartsDialog loading config");
    wxString enc;
    LoadPaths(enc);
    if (!enc.IsEmpty())
        encPicker_->SetPath(enc);

    buildBtn_->Bind(wxEVT_BUTTON, &BuildChartsDialog::OnBuild, this);
    rebuildBtn_->Bind(wxEVT_BUTTON, &BuildChartsDialog::OnRebuild, this);
    cancelBtn_->Bind(wxEVT_BUTTON, &BuildChartsDialog::OnCancel, this);
    closeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    Bind(wxEVT_CLOSE_WINDOW, &BuildChartsDialog::OnClose, this);
    timer_.SetOwner(this);
    Bind(wxEVT_TIMER, &BuildChartsDialog::OnTimer, this);
    // Size in the log confirms which build is loaded: this (padded) build fits to ~600
    // wide with a Rebuild All button; an older build reads ~560 or ~520 and no Rebuild.
    wxLogMessage("tile57: BuildChartsDialog ctor end, size=%dx%d", GetSize().GetWidth(),
                 GetSize().GetHeight());
}

BuildChartsDialog::~BuildChartsDialog() { StopBake(); }

void BuildChartsDialog::OnBuild(wxCommandEvent&) { StartBuild(false); }
void BuildChartsDialog::OnRebuild(wxCommandEvent&) { StartBuild(true); }

void BuildChartsDialog::SetRunningUI(bool running) {
    buildBtn_->Enable(!running);
    rebuildBtn_->Enable(!running);
    cancelBtn_->Enable(running);
    encPicker_->Enable(!running);
}

void BuildChartsDialog::StartBuild(bool force) {
    if (running_.load())
        return;

    const std::string enc = std::string(encPicker_->GetPath().mb_str());
    const std::string dest = FixedDest(); // always the fixed XDG-cache charts dir

    std::error_code ec;
    if (enc.empty() || !fs::is_directory(enc, ec)) {
        status_->SetLabel(_T("ENC source folder does not exist"));
        return;
    }
    fs::create_directories(dest, ec);
    if (ec || !fs::is_directory(dest, ec)) {
        status_->SetLabel(_T("Cannot create the output cache folder"));
        return;
    }

    SavePaths(encPicker_->GetPath());

    // Enumerate .000 cells (case-insensitive) recursively under enc.
    std::vector<std::string> cells;
    for (auto it = fs::recursive_directory_iterator(
             enc, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_regular_file(ec) && IsDotZeroZeroZero(it->path()))
            cells.push_back(it->path().string());
    }

    if (cells.empty()) {
        status_->SetLabel(_T("No .000 cells found"));
        return;
    }

    // Arm shared state, flip the UI to "running", and hand the work off.
    dest_ = dest;
    force_.store(force);
    total_.store((int)cells.size());
    done_.store(0);
    baked_.store(0);
    failed_.store(0);
    cancel_.store(false);
    finished_.store(false);
    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(status_mtx_);
        current_cell_.clear();
    }
    gauge_->SetRange((int)cells.size());
    gauge_->SetValue(0);
    SetRunningUI(true);
    status_->SetLabel(force ? wxString::Format(_T("Rebuilding %d cells…"), (int)cells.size())
                            : wxString::Format(_T("Baking %d cells…"), (int)cells.size()));
    stats_->SetLabel(wxEmptyString);

    StartWorkers(std::move(cells));
    timer_.Start(kTimerMs);
}

void BuildChartsDialog::StartWorkers(std::vector<std::string> cells) {
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        queue_.assign(cells.begin(), cells.end());
    }

    coordinator_ = std::thread([this]() {
        // A single-chart bake (tile57_bake_chart_bytes) is SINGLE-THREADED — the
        // engine's parallel unit is the chart, so one core per bake. The outer
        // pool is therefore what uses the machine: one worker per core (less one
        // for the UI/coordinator), capped at 8 as a memory bound (each concurrent
        // bake holds a whole chart's parse+portray+raster working set).
        unsigned hw = std::thread::hardware_concurrency();
        unsigned n = hw > 1 ? std::min(8u, hw - 1) : 1;

        auto worker = [this]() {
            for (;;) {
                if (cancel_.load())
                    break;
                std::string path;
                {
                    std::lock_guard<std::mutex> lk(queue_mtx_);
                    if (queue_.empty())
                        break;
                    path = std::move(queue_.front());
                    queue_.pop_front();
                }

                fs::path in(path);
                const fs::path out = fs::path(dest_) / (in.stem().string() + ".pmtiles");

                std::error_code ec;
                if (!force_.load() && fs::exists(out, ec)) { // idempotent resume (Build)
                    {
                        std::lock_guard<std::mutex> lk(status_mtx_);
                        current_cell_ = in.stem().string();
                    }
                    done_.fetch_add(1);
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lk(status_mtx_);
                    current_cell_ = in.stem().string();
                }

                // The engine bakes the chart's native band zoom range (v0.3 owns
                // the zoom cap the plugin used to compute per cell).
                uint8_t* bytes = nullptr;
                size_t len = 0;
                if (tile57_bake_chart_bytes(path.c_str(), &bytes, &len, nullptr) == TILE57_OK &&
                    bytes) {
                    const fs::path tmp = out.string() + ".tmp";
                    {
                        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
                        ofs.write(reinterpret_cast<const char*>(bytes), (std::streamsize)len);
                    }
                    tile57_free(bytes);
                    fs::rename(tmp, out, ec);
                    if (ec) {
                        fs::remove(tmp, ec);
                        failed_.fetch_add(1);
                    }
                } else {
                    if (bytes)
                        tile57_free(bytes);
                    failed_.fetch_add(1);
                }
                baked_.fetch_add(1); // actually baked this run (drives the rate)
                done_.fetch_add(1);
            }
        };

        workers_.reserve(n);
        for (unsigned i = 0; i < n; ++i)
            workers_.emplace_back(worker);
        for (auto& t : workers_)
            t.join();
        workers_.clear();
        finished_.store(true);
    });
}

void BuildChartsDialog::OnTimer(wxTimerEvent&) {
    const int total = total_.load();
    const int done = done_.load();
    gauge_->SetRange(std::max(1, total));
    gauge_->SetValue(std::min(done, total));

    if (finished_.load()) {
        Finish();
        return;
    }

    std::string cur;
    {
        std::lock_guard<std::mutex> lk(status_mtx_);
        cur = current_cell_;
    }
    status_->SetLabel(wxString::Format(_T("%s %s (%d/%d)…"),
                                       force_.load() ? _T("Rebuilding") : _T("Baking"),
                                       wxString::FromUTF8(cur.c_str()), done, total));

    // Rate/ETA count ONLY cells actually baked this run (baked_), not the instant skips
    // of a resume — otherwise hundreds of near-zero-time skips inflate cells/s wildly.
    const int baked = baked_.load();
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
    const double rate = (baked > 0 && elapsed > 0.3) ? baked / elapsed : 0.0; // baked/s
    const double eta = rate > 1e-6 ? (total - done) / rate : -1.0;
    stats_->SetLabel(wxString::Format(_T("elapsed %s   ·   %.1f cells/s   ·   ETA %s"),
                                      FmtDur(elapsed), rate, FmtDur(eta)));
}

void BuildChartsDialog::Finish() {
    // Timer is still on the main thread; stop it before joining so it can't
    // re-enter while the coordinator winds down.
    timer_.Stop();
    if (coordinator_.joinable())
        coordinator_.join();

    running_.store(false);
    SetRunningUI(false);

    const int total = total_.load();
    const int failed = failed_.load();
    const int done = done_.load();
    const int baked = baked_.load();
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
    const double rate = (baked > 0 && elapsed > 0.1) ? baked / elapsed : 0.0;
    if (cancel_.load()) {
        status_->SetLabel(_T("Cancelled"));
        stats_->SetLabel(
            wxString::Format(_T("stopped after %s · %d/%d cells"), FmtDur(elapsed), done, total));
        return;
    }

    // Main thread: register the fixed cache dir as a chart directory and rescan.
    wxString destWx = wxString::FromUTF8(dest_.c_str());
    AddChartDirectory(destWx);
    ForceChartDBUpdate();

    const int added = total - failed;
    wxString msg = wxString::Format(_T("Done — %d charts"), added);
    if (failed > 0)
        msg += wxString::Format(_T(" (%d failed)"), failed);
    status_->SetLabel(msg);
    const int skipped = total - baked;
    wxString st = wxString::Format(_T("baked %d cells in %s   ·   %.1f cells/s avg"),
                                   baked - failed, FmtDur(elapsed), rate);
    if (skipped > 0)
        st += wxString::Format(_T("   ·   %d already cached"), skipped);
    stats_->SetLabel(st);
}

void BuildChartsDialog::OnCancel(wxCommandEvent&) {
    if (!running_.load())
        return;
    status_->SetLabel(_T("Cancelling…"));
    StopBake();
    // StopBake joins everything; reflect the cancelled end-state now.
    running_.store(false);
    SetRunningUI(false);
    gauge_->SetValue(done_.load());
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
    status_->SetLabel(_T("Cancelled"));
    stats_->SetLabel(wxString::Format(_T("stopped after %s · %d/%d cells"), FmtDur(elapsed),
                                      done_.load(), total_.load()));
}

void BuildChartsDialog::OnClose(wxCloseEvent& e) {
    StopBake();
    // Modeless: hide for reuse rather than destroy (the plugin owns the ptr and
    // Destroy()s it in DeInit).
    Hide();
    e.Veto();
}

void BuildChartsDialog::StopBake() {
    timer_.Stop(); // never let the timer touch state mid-teardown
    cancel_.store(true);
    if (coordinator_.joinable())
        coordinator_.join(); // joins workers, then returns
    running_.store(false);
}
