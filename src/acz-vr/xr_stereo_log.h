#pragma once
// J-xr-log: routing of the in-proxy consumer's `xr-stereo;` evidence lines.
//
// Every `xr-stereo;` line goes to the dedicated feature log (feature_log.h)
//   %LOCALAPPDATA%\acz-vr\logs\xr-stereo.log   (previous run: xr-stereo.prev.log)
// with the line text unchanged (feature_log adds only its `<ms>|` prefix). That
// file is the evidence of record; the shared diagnostics log no longer carries
// the periodic summaries, which used to exhaust its ordinary budget mid-run.
//
// Shared-log mirror (unchanged from before, but bounded): lifecycle lines within
// the first `lifecycle_cap` still go to the shared log's bounded STARTUP
// partition (Event::device) so the shared log keeps its correlation points. No
// `xr-stereo;` line uses the shared log's ordinary ring any more, except as the
// legacy fallback below.
//
// Fallback: while diagnostics logging is not enabled the dedicated file is not
// opened (and the line goes to the shared log exactly as before, where a disabled
// sink drops it). If the dedicated file cannot be opened the router latches the
// legacy routing for subsequent callers (startup for the first 48 lifecycle
// lines, ordinary for others). Lines admitted before an asynchronous open failure
// may be lost; incomplete() latches that failure. No disk operation runs on emit.
//
// All lifecycle and periodic callers only queue (including the final line).
// A dedicated I/O owner opens the file and drains bounded batches. shutdown()
// stops admission, drains and waits at most its deadline for the owner to exit.
// A wedged synchronous sink cannot be forcibly joined safely: timeout returns
// false, latches incomplete evidence, and retains the owner/state for a later
// join. The production module is pinned before the monitor is launched.
#include "feature_log.h"
#include <atomic>
#include <new>

namespace acz::xr_stereo::evidence {
inline constexpr const char* log_name = "xr-stereo";
inline constexpr unsigned lifecycle_cap = 48;

enum class Shared { none, startup, ordinary };
// flush is a legacy policy hint for the I/O owner; emit never flushes synchronously.
struct Route { bool dedicated = false; Shared shared = Shared::none; bool flush = false; };

// Pure routing policy. `lifecycle_index` is this line's 0-based position among
// lifecycle lines (ignored for periodic lines).
constexpr Route route(bool lifecycle, unsigned lifecycle_index, bool dedicated_open) noexcept {
    const bool startup = lifecycle && lifecycle_index < lifecycle_cap;
    if (!dedicated_open) return Route{false, startup ? Shared::startup : Shared::ordinary, false}; // legacy
    return Route{true, startup ? Shared::startup : Shared::none, lifecycle};
}

class Router {
    struct State {
        SRWLOCK lock=SRWLOCK_INIT;
        char lines[feature_log::queue_lines][feature_log::line_cap]{};
        size_t head=0,tail=0,drained=0;
        bool stopping=false;
        HANDLE wake=nullptr,thread=nullptr;
        std::atomic<int> handle{-2};
        std::atomic<bool> incomplete{false};
        // Owner-only latch: incomplete never clears, so recovery does not re-arm
        // this marker. feature_log owns its separate write-failure reporting.
        bool incompleteReported=false;
    };
public:
    using SharedEmit = void (*)(bool startup_partition, const char* text) noexcept;
    using Enabled = bool (*)() noexcept;
    Router(SharedEmit shared, Enabled enabled) noexcept : shared_(shared), enabled_(enabled), s_(new(std::nothrow) State) {
        if(s_)s_->wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    }
    ~Router() {
        if(shutdown()) {if(s_){if(s_->wake)CloseHandle(s_->wake);delete s_;}}
        // On timeout the still-running owner uses only State, never this Router.
        // Retain it and the OS thread handle, rather than detach/free live storage.
    }
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    void emit(bool lifecycle, const char* text) noexcept {
        if(!text)return;
        const unsigned index=lifecycle?lifecycle_.fetch_add(1,std::memory_order_relaxed):lifecycle_cap;
        const bool enabled=enabled_&&enabled_();
        bool dedicated=enabled&&s_&&s_->wake&&s_->handle.load()!=feature_log::invalid;
        if(dedicated) {
            AcquireSRWLockExclusive(&s_->lock);
            if(!s_->thread&&!s_->stopping) {
                // The owner can outlive a bounded shutdown timeout. Pin its code.
                HMODULE module=nullptr;
                const bool pinned=GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&drain),&module)!=FALSE;
                s_->thread=pinned?CreateThread(nullptr,0,&drain,s_,0,nullptr):nullptr;
                if(!s_->thread){s_->incomplete=true;s_->handle=feature_log::invalid;dedicated=false;}
            }
            if(!s_->stopping&&dedicated) {
                if(s_->head-s_->tail<feature_log::queue_lines) {
                    auto& line=s_->lines[s_->head%feature_log::queue_lines];
                    const size_t n=std::strlen(text);
                    if(n>=feature_log::line_cap)s_->incomplete=true;
                    strncpy_s(line,text,_TRUNCATE);++s_->head;
                } else s_->incomplete=true;
            } else s_->incomplete=true;
            ReleaseSRWLockExclusive(&s_->lock);
            SetEvent(s_->wake);
        }
        const auto r=route(lifecycle,index,dedicated);
        if(r.shared!=Shared::none&&shared_)shared_(r.shared==Shared::startup,text);
        if(std::strncmp(text,"xr-stereo;result=",17)==0)finals_.fetch_add(1,std::memory_order_release);
    }
    // Called only by a serialized lifecycle owner, never a frame/summary caller.
    // Success means the final admitted line was drained and the thread joined.
    bool shutdown(DWORD timeout_ms=2000) noexcept {
        if(!s_)return true;
        AcquireSRWLockExclusive(&s_->lock);s_->stopping=true;HANDLE thread=s_->thread;ReleaseSRWLockExclusive(&s_->lock);
        if(!thread)return true;
        SetEvent(s_->wake);
        if(WaitForSingleObject(thread,timeout_ms)!=WAIT_OBJECT_0){s_->incomplete=true;return false;}
        AcquireSRWLockExclusive(&s_->lock);CloseHandle(thread);s_->thread=nullptr;ReleaseSRWLockExclusive(&s_->lock);return true;
    }
    // Owner::finished() precedes the consumer final callback. The monitor must
    // observe this separate admission fence before closing the evidence owner.
    unsigned final_count() const noexcept {return finals_.load(std::memory_order_acquire);}
    bool wait_final_after(unsigned before,DWORD timeout_ms=2000) noexcept {
        const auto start=GetTickCount64();
        while(final_count()==before){if(GetTickCount64()-start>=timeout_ms)return false;Sleep(1);}return true;
    }
    // Lifecycle-owner/test barrier; never used on frame callers.
    bool wait_idle(DWORD timeout_ms=2000) noexcept {
        if(!s_)return false;const auto start=GetTickCount64();
        for(;;){AcquireSRWLockShared(&s_->lock);const bool idle=s_->head==s_->drained;ReleaseSRWLockShared(&s_->lock);
            if(idle)return true;if(GetTickCount64()-start>=timeout_ms)return false;Sleep(1);}
    }
    void mark_incomplete() noexcept {if(s_){s_->incomplete=true;SetEvent(s_->wake);}}
    bool incomplete() const noexcept {
        if(!s_)return true;
        return s_->incomplete.load()||feature_log::stats(s_->handle.load()).incomplete;
    }
    feature_log::Handle dedicated_handle() const noexcept {return s_?s_->handle.load():feature_log::invalid;}
private:
    static DWORD WINAPI drain(void* context) noexcept {
        auto& s=*static_cast<State*>(context);
        const auto h=feature_log::open(log_name);s.handle=h;
        if(h==feature_log::invalid)s.incomplete=true;
        for(;;) {
            WaitForSingleObject(s.wake,100);
            // At most one bounded router queue per iteration. No producer lock in I/O.
            size_t remaining=0;AcquireSRWLockShared(&s.lock);remaining=s.head-s.tail;ReleaseSRWLockShared(&s.lock);
            while(remaining--) {
                char line[feature_log::line_cap];
                AcquireSRWLockExclusive(&s.lock);
                std::memcpy(line,s.lines[s.tail%feature_log::queue_lines],sizeof line);++s.tail;
                ReleaseSRWLockExclusive(&s.lock);
                feature_log::write(h,line);
            }
            if(s.incomplete.load()&&!s.incompleteReported) {
                feature_log::write(h,"feature-log;event=incomplete-evidence;source=consumer-router");
                s.incompleteReported=true;
            }
            feature_log::flush();
            AcquireSRWLockExclusive(&s.lock);s.drained=s.tail;ReleaseSRWLockExclusive(&s.lock);
            AcquireSRWLockShared(&s.lock);const bool done=s.stopping&&s.head==s.tail;ReleaseSRWLockShared(&s.lock);
            if(done)return 0;
        }
    }
    SharedEmit shared_;Enabled enabled_;State* s_;
    std::atomic<unsigned> lifecycle_{0},finals_{0};
};
} // namespace acz::xr_stereo::evidence
