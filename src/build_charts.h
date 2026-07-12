// build_charts.h — "Build Charts" settings dialog + bake pipeline.
//
// A modeless wxDialog (surfaced via the plugin's ShowPreferencesDialog) that lets the
// user pick an ENC source folder and an output folder, then hands the whole tree to
// tile57_bake_tree: the engine walks the source for *.000, bakes each chart in
// parallel to the SAME relative path under the output dir (ENC_ROOT/d1/US4CT1AA.000 ->
// out/d1/US4CT1AA.pmtiles, plus a .sha sidecar), and skips any chart whose archive is
// already newer than its input — so Build is an incremental resume by construction.
// When the sweep finishes it registers the output dir as a chart directory so the
// baked charts appear. All OpenCPN API calls (AddChartDirectory/ForceChartDBUpdate)
// happen on the main thread from the progress timer — never a worker.
//
// Threading/lifetime: ONE coordinator std::thread makes the (blocking) bake_tree call;
// the engine owns the worker pool inside it. tile57_bake_progress fires from those
// engine threads, so the callback touches ONLY atomics, and returning false cancels
// the bake. The main-thread timer polls those atomics for the gauge/status and, on
// finished_, joins the coordinator and registers the destination. StopBake() sets
// cancel_ (so the next progress tick aborts the bake), stops the timer, and joins the
// coordinator; it's called on Cancel, dialog Close, and DeInit. That join is what
// makes unload safe: libtile57 is linked INTO this plugin's shared object, so a bake
// thread must never outlive the module OpenCPN is about to dlclose(). Cancellation is
// at chart granularity, so the join returns within about one chart's bake time.
#pragma once

#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/timer.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

class wxDirPickerCtrl;
class wxButton;
class wxGauge;
class wxStaticText;

// Posted at the dialog by the engine's bake threads (progress) and by the coordinator
// (completion) via wxQueueEvent — the thread-safe way to reach the GUI thread.
wxDECLARE_EVENT(TILE57_EVT_BAKE_PROGRESS, wxThreadEvent);
wxDECLARE_EVENT(TILE57_EVT_BAKE_DONE, wxThreadEvent);

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
    void RefreshUI();            // main thread: paint gauge/status/stats from the atomics
    void Finish();               // main-thread completion: register dir, reset UI
    void SetRunningUI(bool running);

    // The output dir the user picked, or the default cache path when the field is
    // empty. Never empty.
    std::string DestDir() const;

    // tile57_bake_progress: called CONCURRENTLY from the engine's bake threads. It stores
    // the counts in the atomics below and POSTS an event to this dialog — wxQueueEvent is
    // the one wx call that is safe from a non-GUI thread; the handler then paints on the
    // main thread. It must not touch a widget itself. Returns false to cancel the bake.
    //
    // Do NOT go back to polling these atomics from a wxTimer: inside OpenCPN the dialog's
    // wxEVT_TIMER never arrived, so the gauge sat frozen at "Scanning…" through an entire
    // bake and completion never ran. The bake thread has to push, not the UI poll.
    static bool ProgressTick(void* ctx, uint32_t done, uint32_t total);

    // --- widgets ---
    wxDirPickerCtrl* encPicker_ = nullptr;
    wxDirPickerCtrl* outPicker_ = nullptr; // defaults to the XDG cache charts dir
    wxButton* buildBtn_ = nullptr;
    wxButton* rebuildBtn_ = nullptr; // force re-bake (delete existing archives first)
    wxButton* cancelBtn_ = nullptr;
    wxGauge* gauge_ = nullptr;
    wxStaticText* status_ = nullptr;
    wxStaticText* stats_ = nullptr; // elapsed / rate / ETA
    wxTimer timer_;

    // --- bake state (shared with the engine's bake threads via ProgressTick) ---
    // total_ is what the ENGINE reports: the charts it will bake THIS run (already-
    // current archives are skipped before the first tick), so it is 0 until the first
    // progress callback and is NOT the cell count of the whole tree.
    std::atomic<uint32_t> total_{0}, done_{0};
    std::atomic<bool> cancel_{false}, running_{false}, finished_{false};
    // The engine's out_baked: charts that actually produced an archive this run.
    std::atomic<uint32_t> baked_{0};
    // Set when the bake_tree call itself failed; err_msg_ is written by the coordinator
    // before it flips finished_, and read on the main thread after the join.
    std::atomic<bool> failed_{false};
    std::string err_msg_;
    // Resolved output dir for the run in flight, and whether it was a forced Rebuild
    // (archives deleted up front). Touched only on the main thread.
    std::string dest_;
    bool force_ = false;
    // Main thread only: Finish() can be reached from the completion event, from the timer
    // fallback, and from Cancel, so it latches to run exactly once per bake.
    bool ui_finished_ = true;
    std::chrono::steady_clock::time_point start_time_;

    std::thread coordinator_; // makes the one blocking tile57_bake_tree call
};
