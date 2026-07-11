// bake_manager.cpp — see bake_manager.h.
#include "bake_manager.h"

#include <filesystem>
#include <fstream>

#include "tile57.h"

namespace fs = std::filesystem;

namespace t57 {

BakeManager& BakeManager::instance() {
    static BakeManager m;
    return m;
}

BakeManager::~BakeManager() { stop(); }

void BakeManager::enqueue(std::string cell_path, std::string cache_file) {
    std::error_code ec;
    if (fs::exists(cache_file, ec)) return;   // already baked
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_) return;
        if (!queued_.insert(cache_file).second) return;   // already queued
        queue_.push_back({std::move(cell_path), std::move(cache_file)});
        enqueued_.fetch_add(1, std::memory_order_relaxed);
        if (!worker_started_) {
            worker_started_ = true;
            worker_ = std::thread([this] { worker_loop(); });
        }
    }
    cv_.notify_all();
}

void BakeManager::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            // Wait for work, but yield to any interactive (bake_now) bake in flight.
            cv_.wait(lk, [this] { return stop_ || (!queue_.empty() && priority_ == 0); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        bake_coordinated(job.cell, job.cache);
        processed_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool BakeManager::bake_now(const std::string& cell_path, const std::string& cache_file) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ++priority_;   // pause the background sweep while we bake
    }
    bool ok = bake_coordinated(cell_path, cache_file);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        --priority_;
    }
    cv_.notify_all();   // let the worker resume
    return ok;
}

bool BakeManager::bake_coordinated(const std::string& cell_path, const std::string& cache_file) {
    std::error_code ec;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        // If another thread is baking this exact cell, wait for it rather than duplicate.
        cv_.wait(lk, [&] { return stop_ || inflight_.find(cache_file) == inflight_.end(); });
        if (stop_) return false;
        if (fs::exists(cache_file, ec)) return true;   // baked while we waited
        inflight_.insert(cache_file);
    }

    // Bake outside the lock (the slow part).
    bool ok = false;
    uint8_t* bytes = nullptr;
    size_t len = 0;
    // The engine bakes the chart's NATIVE band zoom range (v0.3 owns the cap that
    // the plugin used to compute from the CSCL).
    if (tile57_bake_chart_bytes(cell_path.c_str(), &bytes, &len, nullptr) == TILE57_OK && bytes) {
        std::string tmp = cache_file + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(len));
        }
        fs::rename(tmp, cache_file, ec);
        tile57_free(bytes);
        if (ec) {
            fs::remove(tmp, ec);   // write failed -> leave no partial
        } else {
            ok = true;
            // Sweep this cell's now-stale bakes (same <stem>-<pathhash>- prefix).
            fs::path p(cache_file);
            std::string fn = p.filename().string();
            size_t dash = fn.rfind('-');
            if (dash != std::string::npos) {
                std::string prefix = fn.substr(0, dash + 1);
                for (const auto& e : fs::directory_iterator(p.parent_path(), ec)) {
                    std::string efn = e.path().filename().string();
                    if (efn.rfind(prefix, 0) == 0 && efn != fn) fs::remove(e.path(), ec);
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        inflight_.erase(cache_file);
    }
    cv_.notify_all();
    return ok || fs::exists(cache_file, ec);
}

void BakeManager::stop() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_) return;
        stop_ = true;
        t = std::move(worker_);
    }
    cv_.notify_all();
    if (t.joinable()) t.join();
}

}  // namespace t57
