#pragma once
// PERF (docs/world-render/perf-impl.md): the xr_png_once file writer. SDK-free; the proxy (xr_stereo_proxy.cpp) supplies the Windows file sink,
// the offline test (tests/acz-vr/png_dump_offline_tests.cpp) a counting one.
//
// Before (step65 integrated run 2): every one of the three images was COPIED into its own detached thread, each of which built the whole PNG in
// memory with png_encode (filtered rows + zlib stream + file image: ~3-4 copies of the image), all three at once: about 13 x 13 MB of heap at the
// peak for a 2400x1350 eye on top of the readback buffer and the consumer's own three images.
// Now: push() TAKES the pixels (swap, no copy); ONE worker writes the jobs sequentially, streaming each through png_stream (one row + a 64 KiB
// buffer), and frees each image as soon as its file is written. The bytes of every file are unchanged (png_stream == png_encode, tested).
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace acz::png_dump {

inline constexpr uint32_t kMaxSide = 8192;                     // larger than any swapchain the consumer creates (cap 2400 by default)
inline constexpr uint64_t kMaxImageBytes = uint64_t(64) << 20; // one RGBA8 image

// A tight RGBA8 image of w x h that the writer accepts (size cap; the vector holds exactly w*h*4 bytes).
inline bool admissible(size_t bytes, uint32_t w, uint32_t h) noexcept {
    return w && h && w <= kMaxSide && h <= kMaxSide && uint64_t(w) * h * 4 == bytes && bytes <= kMaxImageBytes;
}

struct Job {
    std::string name;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
};

// One sequential writer. `write(job)` runs on the worker thread, one job at a time, in push order.
class Queue {
public:
    using WriteFn = std::function<bool(const Job&)>;
    // Final per-job outcome, on the worker thread right after that job's write (before its image is freed); must be lifecycle-safe (process-lifetime
    // state only) and must not block. A push() success means QUEUED only: this is where "written" / "failed" becomes known.
    using ResultFn = std::function<void(const Job&, bool ok)>;
    explicit Queue(WriteFn fn, ResultFn result = nullptr) : write_(std::move(fn)), result_(std::move(result)) {}
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    ~Queue() { wait_idle(); }   // the proxy's instance is never destroyed (process lifetime); tests wait

    // Takes the pixels (rgba is left empty) and returns at once; false = refused (size cap, or the worker could not start), rgba untouched.
    bool push(std::string name, std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
        if (!admissible(rgba.size(), w, h)) return false;
        try {
            std::unique_lock<std::mutex> lk(m_);
            Job j; j.name = std::move(name); j.w = w; j.h = h;
            q_.push_back(std::move(j));
            q_.back().rgba.swap(rgba);
            ++pushed_;
            if (!running_) {
                running_ = true;
                try { std::thread([this] { run(); }).detach(); }
                catch (...) { running_ = false; rgba.swap(q_.back().rgba); q_.pop_back(); --pushed_; return false; }
            }
            return true;
        } catch (...) { return false; }
    }
    // Tests: wait until every pushed job was written (or failed). true = idle.
    bool wait_idle(unsigned timeout_ms = 60000) {
        std::unique_lock<std::mutex> lk(m_);
        return idle_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return !running_ && q_.empty(); });
    }
    uint64_t written() const { std::lock_guard<std::mutex> lk(m_); return written_; }
    uint64_t failed() const { std::lock_guard<std::mutex> lk(m_); return failed_; }
    uint64_t pushed() const { std::lock_guard<std::mutex> lk(m_); return pushed_; }

private:
    void run() {
        for (;;) {
            Job j;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (q_.empty()) { running_ = false; idle_.notify_all(); return; }
                j = std::move(q_.front());
                q_.pop_front();
            }
            bool ok = false;
            try { ok = write_ && write_(j); } catch (...) { ok = false; }
            if (result_) { try { result_(j, ok); } catch (...) {} }
            std::vector<uint8_t>().swap(j.rgba);   // free this image before the next file
            std::lock_guard<std::mutex> lk(m_);
            if (ok) ++written_; else ++failed_;
        }
    }
    WriteFn write_;
    ResultFn result_;
    mutable std::mutex m_;
    std::condition_variable idle_;
    std::deque<Job> q_;
    bool running_ = false;
    uint64_t written_ = 0, failed_ = 0, pushed_ = 0;
};

} // namespace acz::png_dump
