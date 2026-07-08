// build_charts.cpp — see build_charts.h.
//
// Threading/lifetime: ONE coordinator std::thread launches N worker threads
// (hardware_concurrency-2, min 1) that pull cell paths from a shared deque. The
// coordinator joins all workers then flips finished_. The main-thread timer
// polls the atomics for the gauge/status and, on finished_, joins the
// coordinator, stops the timer, and (if not cancelled) registers the
// destination — the only place OpenCPN API calls happen. StopBake() sets
// cancel_, stops the timer (so it can't touch state mid-teardown) and joins the
// coordinator; it's called on Cancel, dialog Close, and DeInit, guaranteeing
// every thread is joined before this dialog (which owns the shared state) dies.
#include "build_charts.h"

#include <cstdint>
#include "ocpn_plugin.h"
#include "tile57.h"

#include <wx/filepicker.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/gauge.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/log.h>

#include <algorithm>
#include <cmath>
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
    return wxFileName(wxStandardPaths::Get().GetUserConfigDir(),
                      _T("tile57_pi.ini")).GetFullPath();
}

void LoadPaths(wxString& enc, wxString& dest, int& detail) {
    wxFileConfig cfg(wxEmptyString, wxEmptyString, ConfigPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    cfg.SetPath(_T("/tile57"));
    cfg.Read(_T("EncSource"), &enc, wxEmptyString);
    cfg.Read(_T("Dest"), &dest, wxEmptyString);
    cfg.Read(_T("Detail"), &detail, 1);
}

void SavePaths(const wxString& enc, const wxString& dest, int detail) {
    wxFileConfig cfg(wxEmptyString, wxEmptyString, ConfigPath(), wxEmptyString,
                     wxCONFIG_USE_LOCAL_FILE);
    cfg.SetPath(_T("/tile57"));
    cfg.Write(_T("EncSource"), enc);
    cfg.Write(_T("Dest"), dest);
    cfg.Write(_T("Detail"), detail);
    cfg.Flush();
}

bool IsDotZeroZeroZero(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".000";
}

}  // namespace

BuildChartsDialog::BuildChartsDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _T("Build Charts — tile57"),
               wxDefaultPosition, wxSize(520, 260),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    wxLogMessage("tile57: BuildChartsDialog ctor begin");
    const int B = 12;   // outer margin
    auto* top = new wxBoxSizer(wxVERTICAL);

    top->Add(new wxStaticText(this, wxID_ANY, _T("ENC source (ENC_ROOT):")),
             0, wxLEFT | wxRIGHT | wxTOP, B);
    encPicker_ = new wxDirPickerCtrl(this, wxID_ANY, wxEmptyString,
                                     _T("Select ENC source folder"),
                                     wxDefaultPosition, wxDefaultSize,
                                     wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
    top->Add(encPicker_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

    top->AddSpacer(10);
    top->Add(new wxStaticText(this, wxID_ANY, _T("Destination folder (baked charts):")),
             0, wxLEFT | wxRIGHT, B);
    destPicker_ = new wxDirPickerCtrl(this, wxID_ANY, wxEmptyString,
                                      _T("Select destination folder"),
                                      wxDefaultPosition, wxDefaultSize,
                                      wxDIRP_USE_TEXTCTRL);
    top->Add(destPicker_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

    top->AddSpacer(10);
    top->Add(new wxStaticText(this, wxID_ANY, _T("Detail (max zoom vs bake time):")),
             0, wxLEFT | wxRIGHT, B);
    detailChoice_ = new wxChoice(this, wxID_ANY);
    // The GL renderer overzooms native tiles, so a level below native is barely
    // distinguishable but bakes ~2x faster (measured 1.02s -> 0.59s/cell).
    detailChoice_->Append(_T("Native (crispest, slowest)"));
    detailChoice_->Append(_T("Native −1 (recommended — ~2× faster)"));
    detailChoice_->Append(_T("Native −2 (fastest — ~3× faster)"));
    detailChoice_->SetSelection(1);
    top->Add(detailChoice_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

    top->AddSpacer(16);
    gauge_ = new wxGauge(this, wxID_ANY, 100);
    top->Add(gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT, B);
    status_ = new wxStaticText(this, wxID_ANY, _T("Idle"));
    top->Add(status_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

    top->AddSpacer(16);
    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    buildBtn_ = new wxButton(this, wxID_ANY, _T("Build Charts"));
    cancelBtn_ = new wxButton(this, wxID_ANY, _T("Cancel"));
    cancelBtn_->Enable(false);
    auto* closeBtn = new wxButton(this, wxID_CLOSE, _T("Close"));
    btns->Add(buildBtn_, 0, wxRIGHT, 8);
    btns->Add(cancelBtn_, 0, wxRIGHT, 8);
    btns->AddStretchSpacer();
    btns->Add(closeBtn, 0);
    top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, B);

    SetSizerAndFit(top);
    SetSize(wxSize(560, GetSize().GetHeight()));   // widen for readable paths
    SetMinSize(GetSize());

    // Restore persisted paths.
    wxLogMessage("tile57: BuildChartsDialog loading config");
    wxString enc, dest;
    int detail = 1;
    LoadPaths(enc, dest, detail);
    if (!enc.IsEmpty()) encPicker_->SetPath(enc);
    if (!dest.IsEmpty()) destPicker_->SetPath(dest);
    if (detail >= 0 && detail <= 2) detailChoice_->SetSelection(detail);

    buildBtn_->Bind(wxEVT_BUTTON, &BuildChartsDialog::OnBuild, this);
    cancelBtn_->Bind(wxEVT_BUTTON, &BuildChartsDialog::OnCancel, this);
    closeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    Bind(wxEVT_CLOSE_WINDOW, &BuildChartsDialog::OnClose, this);
    timer_.SetOwner(this);
    Bind(wxEVT_TIMER, &BuildChartsDialog::OnTimer, this);
    wxLogMessage("tile57: BuildChartsDialog ctor end");
}

BuildChartsDialog::~BuildChartsDialog() {
    StopBake();
}

void BuildChartsDialog::OnBuild(wxCommandEvent&) {
    if (running_.load()) return;

    const std::string enc = std::string(encPicker_->GetPath().mb_str());
    const std::string dest = std::string(destPicker_->GetPath().mb_str());

    std::error_code ec;
    if (enc.empty() || !fs::is_directory(enc, ec)) {
        status_->SetLabel(_T("ENC source folder does not exist"));
        return;
    }
    if (dest.empty()) {
        status_->SetLabel(_T("Pick a destination folder"));
        return;
    }
    fs::create_directories(dest, ec);
    if (ec || !fs::is_directory(dest, ec)) {
        status_->SetLabel(_T("Destination folder is not creatable"));
        return;
    }

    const int detail = detailChoice_->GetSelection();
    zoom_reduce_.store(detail >= 0 ? detail : 1);
    SavePaths(encPicker_->GetPath(), destPicker_->GetPath(), zoom_reduce_.load());

    // Enumerate .000 cells (case-insensitive) recursively under enc.
    std::vector<std::string> cells;
    for (auto it = fs::recursive_directory_iterator(
             enc, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_regular_file(ec) && IsDotZeroZeroZero(it->path()))
            cells.push_back(it->path().string());
    }

    if (cells.empty()) {
        status_->SetLabel(_T("No .000 cells found"));
        return;
    }

    // Arm shared state, flip the UI to "running", and hand the work off.
    dest_ = dest;
    total_.store((int)cells.size());
    done_.store(0);
    failed_.store(0);
    cancel_.store(false);
    finished_.store(false);
    running_.store(true);
    {
        std::lock_guard<std::mutex> lk(status_mtx_);
        current_cell_.clear();
    }
    gauge_->SetRange((int)cells.size());
    gauge_->SetValue(0);
    buildBtn_->Enable(false);
    cancelBtn_->Enable(true);
    status_->SetLabel(_T("Baking…"));

    StartWorkers(std::move(cells));
    timer_.Start(kTimerMs);
}

void BuildChartsDialog::StartWorkers(std::vector<std::string> cells) {
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        queue_.assign(cells.begin(), cells.end());
    }

    coordinator_ = std::thread([this]() {
        // tile57_bake_cell_bytes is ALREADY internally parallel (~cores threads via
        // its tile parallelFor). Profiling showed an outer pool beyond ~2 concurrent
        // cells barely helps (the internal pools contend) while spawning cores×N
        // threads, so cap the outer fan-out low rather than at hardware_concurrency-2.
        unsigned hw = std::thread::hardware_concurrency();
        unsigned n = hw > 2 ? std::min(4u, hw - 2) : 1;

        auto worker = [this]() {
            for (;;) {
                if (cancel_.load()) break;
                std::string path;
                {
                    std::lock_guard<std::mutex> lk(queue_mtx_);
                    if (queue_.empty()) break;
                    path = std::move(queue_.front());
                    queue_.pop_front();
                }

                fs::path in(path);
                const fs::path out = fs::path(dest_) / (in.stem().string() + ".pmtiles");

                std::error_code ec;
                if (fs::exists(out, ec)) {   // idempotent resume
                    {
                        std::lock_guard<std::mutex> lk(status_mtx_);
                        current_cell_ = in.stem().string();
                    }
                    done_.fetch_add(1);
                    continue;
                }

                // Per-cell native maxzoom — mirrors ChartTile57::Init exactly.
                tile57_chart* h = tile57_chart_open_header(path.c_str());
                if (!h) { failed_.fetch_add(1); done_.fetch_add(1); continue; }
                tile57_chart_info info{};
                tile57_chart_get_info(h, &info);
                tile57_chart_close(h);
                int zn = info.native_scale > 0
                             ? (int)std::lround(std::log2(559082264.0 / (double)info.native_scale))
                             : 14;
                // Cap at native, then drop the user's detail levels (the GL renderer
                // overzooms, so a level below native bakes ~2x faster, barely visible).
                zn = std::max(4, std::min(zn, 16) - zoom_reduce_.load());

                {
                    std::lock_guard<std::mutex> lk(status_mtx_);
                    current_cell_ = in.stem().string();
                }

                uint8_t* bytes = nullptr;
                size_t len = 0;
                if (tile57_bake_cell_bytes(path.c_str(), 0, (uint8_t)zn, &bytes, &len) == 1) {
                    const fs::path tmp = out.string() + ".tmp";
                    {
                        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
                        ofs.write(reinterpret_cast<const char*>(bytes), (std::streamsize)len);
                    }
                    tile57_free(bytes, len);
                    fs::rename(tmp, out, ec);
                    if (ec) { fs::remove(tmp, ec); failed_.fetch_add(1); }
                } else {
                    if (bytes) tile57_free(bytes, len);
                    failed_.fetch_add(1);
                }
                done_.fetch_add(1);
            }
        };

        workers_.reserve(n);
        for (unsigned i = 0; i < n; ++i) workers_.emplace_back(worker);
        for (auto& t : workers_) t.join();
        workers_.clear();
        finished_.store(true);
    });
}

void BuildChartsDialog::OnTimer(wxTimerEvent&) {
    const int total = total_.load();
    const int done = done_.load();
    gauge_->SetRange(std::max(1, total));
    gauge_->SetValue(std::min(done, total));

    if (finished_.load()) { Finish(); return; }

    std::string cur;
    {
        std::lock_guard<std::mutex> lk(status_mtx_);
        cur = current_cell_;
    }
    status_->SetLabel(wxString::Format(_T("Baking %s (%d/%d)…"),
                                       wxString::FromUTF8(cur.c_str()), done, total));
}

void BuildChartsDialog::Finish() {
    // Timer is still on the main thread; stop it before joining so it can't
    // re-enter while the coordinator winds down.
    timer_.Stop();
    if (coordinator_.joinable()) coordinator_.join();

    running_.store(false);
    buildBtn_->Enable(true);
    cancelBtn_->Enable(false);

    const int total = total_.load();
    const int failed = failed_.load();
    if (cancel_.load()) {
        status_->SetLabel(_T("Cancelled"));
        return;
    }

    // Main thread: register the destination as a chart directory and rescan.
    wxString destWx = wxString::FromUTF8(dest_.c_str());
    AddChartDirectory(destWx);
    ForceChartDBUpdate();

    const int added = total - failed;
    wxString msg = wxString::Format(_T("Done — added %d charts"), added);
    if (failed > 0) msg += wxString::Format(_T(" (%d failed)"), failed);
    status_->SetLabel(msg);
}

void BuildChartsDialog::OnCancel(wxCommandEvent&) {
    if (!running_.load()) return;
    status_->SetLabel(_T("Cancelling…"));
    StopBake();
    // StopBake joins everything; reflect the cancelled end-state now.
    running_.store(false);
    buildBtn_->Enable(true);
    cancelBtn_->Enable(false);
    gauge_->SetValue(done_.load());
    status_->SetLabel(_T("Cancelled"));
}

void BuildChartsDialog::OnClose(wxCloseEvent& e) {
    StopBake();
    // Modeless: hide for reuse rather than destroy (the plugin owns the ptr and
    // Destroy()s it in DeInit).
    Hide();
    e.Veto();
}

void BuildChartsDialog::StopBake() {
    timer_.Stop();               // never let the timer touch state mid-teardown
    cancel_.store(true);
    if (coordinator_.joinable()) coordinator_.join();   // joins workers, then returns
    running_.store(false);
}
