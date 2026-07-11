// build_charts.h — "Build Charts" settings dialog + bake pipeline.
//
// A modeless wxDialog (surfaced via the plugin's ShowPreferencesDialog) that
// lets the user pick an ENC source folder + a destination, then bakes every
// .000 cell to dest/<stem>.pmtiles on a bounded worker pool. When the sweep
// finishes it auto-registers the destination as a chart directory so the baked
// charts appear. All OpenCPN API calls (AddChartDirectory/ForceChartDBUpdate)
// happen on the main thread from the progress timer — never a worker.
#pragma once

#include <wx/dialog.h>
#include <wx/timer.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class wxDirPickerCtrl;
class wxButton;
class wxGauge;
class wxStaticText;
class wxChoice;

class BuildChartsDialog : public wxDialog {
  public:
    explicit BuildChartsDialog(wxWindow* parent);
    ~BuildChartsDialog() override;

    // Stop + join any running bake (safe to call repeatedly). Called from the
    // dialog's own close handler and from the plugin's DeInit before Destroy().
    void StopBake();

  private:
    void OnBuild(wxCommandEvent&);
    void OnRebuild(wxCommandEvent&);
    void OnCancel(wxCommandEvent&);
    void OnClose(wxCloseEvent&);
    void OnTimer(wxTimerEvent&);

    void StartBuild(bool force); // shared by Build (resume) + Rebuild (force)
    void StartWorkers(std::vector<std::string> cells);
    void Finish(); // main-thread completion: register dir, reset UI
    void SetRunningUI(bool running);

    // --- widgets ---
    wxDirPickerCtrl* encPicker_ = nullptr;
    wxButton* buildBtn_ = nullptr;
    wxButton* rebuildBtn_ = nullptr; // force re-bake (ignore existing outputs)
    wxButton* cancelBtn_ = nullptr;
    wxGauge* gauge_ = nullptr;
    wxStaticText* status_ = nullptr;
    wxStaticText* stats_ = nullptr; // elapsed / rate / ETA
    wxTimer timer_;

    // --- bake state (shared with workers) ---
    // done_ = cells processed (incl. skipped already-baked); baked_ = cells actually
    // baked THIS run (drives the rate/ETA so a resume's instant skips don't skew it).
    std::atomic<int> total_{0}, done_{0}, baked_{0}, failed_{0};
    std::atomic<bool> cancel_{false}, running_{false}, finished_{false};
    std::mutex status_mtx_;
    std::string current_cell_;
    std::string dest_;
    std::atomic<bool> force_{false};                   // Rebuild: overwrite existing outputs
    std::chrono::steady_clock::time_point start_time_; // set when a run begins

    // Work queue + worker pool, driven by a single coordinator thread.
    std::deque<std::string> queue_;
    std::mutex queue_mtx_;
    std::thread coordinator_;
    std::vector<std::thread> workers_;
};
