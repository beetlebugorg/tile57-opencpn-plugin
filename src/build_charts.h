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
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class wxDirPickerCtrl;
class wxButton;
class wxGauge;
class wxStaticText;

class BuildChartsDialog : public wxDialog {
public:
    explicit BuildChartsDialog(wxWindow* parent);
    ~BuildChartsDialog() override;

    // Stop + join any running bake (safe to call repeatedly). Called from the
    // dialog's own close handler and from the plugin's DeInit before Destroy().
    void StopBake();

private:
    void OnBuild(wxCommandEvent&);
    void OnCancel(wxCommandEvent&);
    void OnClose(wxCloseEvent&);
    void OnTimer(wxTimerEvent&);

    void StartWorkers(std::vector<std::string> cells);
    void Finish();   // main-thread completion: register dir, reset UI

    // --- widgets ---
    wxDirPickerCtrl* encPicker_ = nullptr;
    wxDirPickerCtrl* destPicker_ = nullptr;
    wxButton*        buildBtn_ = nullptr;
    wxButton*        cancelBtn_ = nullptr;
    wxGauge*         gauge_ = nullptr;
    wxStaticText*    status_ = nullptr;
    wxTimer          timer_;

    // --- bake state (shared with workers) ---
    std::atomic<int>  total_{0}, done_{0}, failed_{0};
    std::atomic<bool> cancel_{false}, running_{false}, finished_{false};
    std::mutex        status_mtx_;
    std::string       current_cell_;
    std::string       dest_;

    // Work queue + worker pool, driven by a single coordinator thread.
    std::deque<std::string>  queue_;
    std::mutex               queue_mtx_;
    std::thread              coordinator_;
    std::vector<std::thread> workers_;
};
