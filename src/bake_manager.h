// bake_manager.h — process-wide background baker for the live-cell tile cache.
//
// Pre-bakes every cell OpenCPN scans to its on-disk PMTiles cache, so a cell renders
// instantly the first time it's viewed instead of stalling on a ~2-11s bake. It is
// also the SINGLE place the (slow) bake runs, so the background sweep and a
// just-viewed cell never bake the same cell twice, and the background sweep defers
// while a viewed cell is baking so it doesn't degrade the interactive path.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace t57 {

class BakeManager {
public:
    static BakeManager& instance();

    // Queue a cell for LOW-priority background pre-baking to cache_file. No-op if it is
    // already cached or already queued. Survives the caller (a transient scan chart).
    void enqueue(std::string cell_path, uint8_t max_zoom, std::string cache_file);

    // Ensure cache_file is baked NOW (a viewed cell needs it immediately): bakes it
    // here, or waits if another thread is already baking this exact cell. Bumps the
    // priority count so the background sweep pauses meanwhile. Returns true if present.
    bool bake_now(const std::string& cell_path, uint8_t max_zoom, const std::string& cache_file);

    // Plugin DeInit: stop + join the worker (may wait out one in-flight bake).
    void stop();

    // Pre-bake progress, for an unobtrusive UI indicator. total() = cells enqueued so
    // far; pending() = not yet baked. Lock-free (approximate, fine for a progress bar).
    int total() const { return enqueued_.load(std::memory_order_relaxed); }
    int pending() const {
        return enqueued_.load(std::memory_order_relaxed) - processed_.load(std::memory_order_relaxed);
    }

private:
    BakeManager() = default;
    ~BakeManager();
    BakeManager(const BakeManager&) = delete;
    BakeManager& operator=(const BakeManager&) = delete;

    void worker_loop();
    // Bake cell_path -> cache_file (atomic tmp+rename) + sweep stale siblings, letting
    // only one thread bake a given cache_file at a time. Returns true if present after.
    bool bake_coordinated(const std::string& cell_path, uint8_t max_zoom,
                          const std::string& cache_file);

    struct Job { std::string cell; uint8_t max_zoom; std::string cache; };
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::set<std::string> queued_;    // cache files enqueued (dedup)
    std::set<std::string> inflight_;  // cache files currently baking
    int priority_ = 0;                // active bake_now() calls; worker pauses while > 0
    std::thread worker_;
    bool worker_started_ = false;
    bool stop_ = false;
    std::atomic<int> enqueued_{0};    // cumulative cells queued for pre-bake
    std::atomic<int> processed_{0};   // cumulative pre-bake jobs finished
};

}  // namespace t57
