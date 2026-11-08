#pragma once
// The key injector's logic (state machine, watchdog, release recovery) without any OS call: header-only, driven through the OsBackend
// interface so the offline tests can inject partial SendInput failures, a failing thread start, deferred consumption and clock control.
// The Windows backend and the registration live in look_drive_keys.cpp (J-look-drive-findings.md, "Codex review J-179 resolution").
//
// Safety model (what is guaranteed and what is not):
//  * Requests: apply() stores {mask, generation, stamp} for EVERY decision, a zero included. The worker consumes the newest request.
//    It re-checks the stamp (watchdog), the foreground window, the kill-file latch and the request itself right before each key-down.
//  * Releases: key-ups go first. A key-up that SendInput does not confirm leaves the key in down() and raises fault_release_pending:
//    no key-down of any kind is sent until every required key-up succeeded, and the worker retries the release every tick (a
//    release-only owner: it keeps running after shutdown until nothing is held). The opposing-key guard looks at the ACTUAL held set.
//  * Thread start failure: fail closed. Existing keys are released, fault_thread_start is latched for the process and no key-down is
//    ever issued (there is no watchdog to bound it).
//  * Shutdown (release_all): stopping_ forbids every further down; it reports false and latches fault_shutdown_unconfirmed when a key
//    is still down; the worker (if any) keeps retrying.
//  * NOT covered: abrupt process death while a key is down (see J-look-drive-findings.md for the precise statement).
#include "look_drive_keys.h"
#include <atomic>
#include <cstdio>
#include <mutex>

namespace acz::look_drive {

struct OsBackend {
    virtual ~OsBackend()=default;
    virtual bool send_key(uint8_t bit,bool up) noexcept=0;  // one key event; true = SendInput confirmed it
    virtual uint64_t now_ms() noexcept=0;                   // monotonic milliseconds
    virtual bool foreground() noexcept=0;                   // the GAME'S window (not merely this process) is the foreground window
    virtual bool kill_file() noexcept=0;                    // the kill-switch file exists (cached by the backend)
    virtual bool start_worker(void (*fn)(void*),void* ctx) noexcept=0; // false = the thread could not be created
    virtual void wake() noexcept=0;                         // wake a sleeping worker
    virtual void wait_ms(unsigned ms) noexcept=0;           // worker sleep (interruptible by wake)
    // Diagnostics only: which window holds the foreground (class, process) for the per-second log line. Default: nothing.
    virtual void describe_foreground(char* buf,size_t n) noexcept {if(buf&&n)buf[0]=0;}
};

class InjectorCore final:public KeyInjector {
public:
    explicit InjectorCore(OsBackend& os) noexcept:os_(os) {}
    bool foreground() noexcept override {return os_.foreground();}
    bool kill_switch() noexcept override {return os_.kill_file();}
    void apply(uint8_t mask,double) noexcept override {
        {
            std::lock_guard<std::mutex> lk(req_);
            req_mask_=mask;++req_gen_;req_stamp_=os_.now_ms();
        }
        if(mask==0&&!started_.load(std::memory_order_acquire))return;  // nothing pending and nothing held: no thread needed for a zero
        if(fault_.load()&fault_thread_start) { // fail closed: release only
            std::lock_guard<std::mutex> lk(m_);flush_locked(0,os_.now_ms());return;
        }
        if(!started_.exchange(true)) {
            if(!os_.start_worker(&InjectorCore::trampoline,this)) {
                fault_.fetch_or(fault_thread_start);
                std::lock_guard<std::mutex> lk(m_);flush_locked(0,os_.now_ms()); // release anything held; NEVER a new down from here
                return;
            }
        }
        os_.wake();
    }
    bool release_all() noexcept override {
        stopping_.store(true);
        {std::lock_guard<std::mutex> lk(req_);req_mask_=0;++req_gen_;}
        std::lock_guard<std::mutex> lk(m_);
        for(int attempt=0;attempt<3&&down_.load();++attempt)flush_locked(0,os_.now_ms());
        if(down_.load()){fault_.fetch_or(fault_release_pending|fault_shutdown_unconfirmed);return false;}
        return true;
    }
    uint8_t down() const noexcept override {return down_.load(std::memory_order_relaxed);}
    unsigned fault() const noexcept override {return fault_.load();}
    void status_text(char* buf,size_t n) noexcept override {
        if(!buf||!n)return;
        char fg[96];fg[0]=0;os_.describe_foreground(fg,sizeof fg);
        std::snprintf(buf,n,"sends=down:%u,up:%u,fail:%u,%u;%s",sent_downs_.load(),sent_ups_.load(),down_failures_.load(),up_failures_.load(),fg);
    }
    // One worker iteration (the thread loop body; tests call it directly). false = the worker may exit (stopped and nothing held).
    bool tick() noexcept {
        std::lock_guard<std::mutex> lk(m_);
        const uint64_t now=os_.now_ms();
        uint8_t want;uint64_t stamp;
        {std::lock_guard<std::mutex> rk(req_);want=req_mask_;stamp=req_stamp_;}
        if(stopping_.load())want=0;
        if(now>stamp&&now-stamp>uint64_t(watchdog_ms))want=0;            // the producer stopped ticking
        if(!os_.foreground())want=0;                                     // lost focus: never type into another window
        if(kill_latched_||os_.kill_file()){kill_latched_=true;want=0;}   // the worker honours the kill switch itself, latched for the process
        flush_locked(want,now);
        return !(stopping_.load()&&!down_.load());
    }
    unsigned up_failures() const noexcept {return up_failures_.load();}
    unsigned down_failures() const noexcept {return down_failures_.load();}
private:
    static void trampoline(void* p) noexcept {static_cast<InjectorCore*>(p)->worker();}
    void worker() noexcept {while(tick())os_.wait_ms(50);}
    static int bitIndex(uint8_t b) noexcept {return b==key_left?0:(b==key_right?1:2);}
    bool request_allows(uint8_t bit) noexcept {std::lock_guard<std::mutex> lk(req_);return (req_mask_&bit)!=0;}
    // Releases first, then presses. A key held longer than hard_key_down_ms is released and stays blocked until the desired set dropped
    // it once (defence in depth behind the controller's cap). m_ held.
    void flush_locked(uint8_t want,uint64_t now) noexcept {
        static constexpr uint8_t bits[3]={key_left,key_right,key_up};
        uint8_t cur=down_.load(std::memory_order_relaxed);
        for(uint8_t b:bits) {
            if(!(want&b))blocked_&=uint8_t(~b);
            if((cur&b)&&now-since_[bitIndex(b)]>=uint64_t(hard_key_down_ms)){want&=uint8_t(~b);blocked_|=b;}
        }
        want&=uint8_t(~blocked_);
        if((want&key_left)&&(want&key_right))want&=uint8_t(~(key_left|key_right)); // never opposing keys requested
        bool upFailed=false;
        for(uint8_t b:bits)if((cur&b)&&!(want&b)) {
            if(os_.send_key(b,true)){cur&=uint8_t(~b);++sent_ups_;}else {upFailed=true;++up_failures_;}
        }
        down_.store(cur,std::memory_order_relaxed);
        if(upFailed){fault_.fetch_or(fault_release_pending);return;}                 // no down of any kind until every required up succeeded
        fault_.fetch_and(~unsigned(fault_release_pending));
        if(stopping_.load()||(fault_.load()&fault_thread_start))return;              // release-only modes
        for(uint8_t b:bits) {
            if((cur&b)||!(want&b))continue;
            if(b==key_left&&(cur&key_right))continue;                                // the ACTUAL held set, not only the wanted one
            if(b==key_right&&(cur&key_left))continue;
            if(!request_allows(b))continue;                                          // cancelled between the read and the send
            if(os_.send_key(b,false)){cur|=b;since_[bitIndex(b)]=now;++sent_downs_;}else ++down_failures_;
        }
        down_.store(cur,std::memory_order_relaxed);
    }
    OsBackend& os_;
    std::mutex m_,req_;                       // m_: flush state (held across SendInput); req_: the request only (never across an OS call)
    uint8_t req_mask_=0;uint64_t req_gen_=0,req_stamp_=0;
    std::atomic<uint8_t> down_{0};std::atomic<unsigned> fault_{0},up_failures_{0},down_failures_{0},sent_downs_{0},sent_ups_{0};
    std::atomic<bool> started_{false},stopping_{false};
    uint8_t blocked_=0;uint64_t since_[3]{};bool kill_latched_=false;
};

} // namespace acz::look_drive
