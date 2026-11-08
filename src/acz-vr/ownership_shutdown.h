#pragma once
#include <atomic>
#include <cstdint>

namespace acz::diagnostics {
// Nonblocking admission barrier. Closing succeeds ONLY at quiescence; a busy
// attempt does not start shutdown. Never wait for another thread in DllMain.
class OwnershipShutdown {
    static constexpr uint64_t closed = uint64_t(1) << 63;
    std::atomic<uint64_t> value{0};
public:
    bool enter() noexcept {
        auto n=value.load();
        while (!(n&closed)) {
            if(n==closed-1)return false;
            if(value.compare_exchange_weak(n,n+1))return true;
        }
        return false;
    }
    void leave() noexcept {value.fetch_sub(1);}
    bool stopped() const noexcept {return (value.load()&closed)!=0;}
    bool close() noexcept {
        uint64_t expected=0;
        return value.compare_exchange_strong(expected,closed);
    }
};
}
