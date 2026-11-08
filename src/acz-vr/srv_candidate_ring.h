#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>

namespace acz::diagnostics {
// Mutex-protected bookkeeping only. Detached pointers are OWNED references;
// caller releases outside the mutex. Pending/faulted slots cannot be reused.
template<class Resource, size_t Slots=1024, size_t Heaps=8> struct SrvSlotCandidates {
    static_assert(Slots>0 && Slots<=1024 && Heaps>0 && Heaps<=8);
    static constexpr size_t slots_per_heap=Slots,heap_capacity=Heaps,capacity=Slots*Heaps;
    struct Key {
        uint64_t address=0,heap=0;
        unsigned ordinal=0,slot=0;
        bool operator==(const Key& b) const noexcept {
            return address==b.address&&heap==b.heap&&ordinal==b.ordinal&&slot==b.slot;
        }
    };
    struct Entry {Key key;uint64_t ticket=0;Resource* held=nullptr;bool pending=false,cancelled=false,fault=false;};
    struct Reservation {size_t slot=capacity;uint64_t ticket=0;Resource* evicted=nullptr;};
    struct Heap {uint64_t serial=0;size_t occupied=0;bool retired=false;std::array<bool,Slots> written{};};
    std::array<Entry,capacity> entries{};
    std::array<Heap,Heaps> heaps{};
    uint64_t clock=0;
    unsigned acquired=0,evicted=0,promoted=0,refused=0;
    unsigned held=0,replaced=0,releases=0,cap_refused=0,unsupported=0,max_slot=0,distinct=0;
    static void increment(unsigned& n) noexcept {if(n<65535)++n;}
    size_t heap_index(uint64_t serial,bool create=false) noexcept {
        size_t free=Heaps;
        for(size_t h=0;h<Heaps;++h) {
            if(heaps[h].serial==serial&&serial)return h;
            if(!heaps[h].serial&&free==Heaps)free=h;
        }
        if(create&&serial&&free<Heaps){heaps[free].serial=serial;return free;}
        return Heaps;
    }
    size_t index(uint64_t heap,unsigned slot) noexcept {
        const auto h=heap_index(heap);return h<Heaps&&slot<Slots?h*Slots+slot:capacity;
    }
    // Once per resolved destination write, including non-SRV/copy writes.
    void written(uint64_t serial,unsigned slot,bool supported=true) noexcept {
        if(slot>max_slot)max_slot=slot;
        if(!supported){increment(unsupported);return;}
        if(slot>=Slots)return;
        const auto h=heap_index(serial,true);
        if(h==Heaps)return;
        if(!heaps[h].written[slot]){heaps[h].written[slot]=true;increment(distinct);}
    }
    void clear(size_t i) noexcept {
        auto& h=heaps[i/Slots];entries[i]={};--h.occupied;
        if(h.retired&&!h.occupied)h={};
    }
    Reservation discard(uint64_t heap,unsigned slot) noexcept {
        const auto i=index(heap,slot);if(i==capacity)return {};
        auto& e=entries[i];
        if(!e.ticket||e.cancelled)return {};
        e.cancelled=true;increment(replaced);
        if(e.held){auto* owned=e.held;e.held=nullptr;e.pending=true;--held;increment(evicted);return {i,e.ticket,owned};}
        if(!e.pending&&!e.fault)clear(i);
        return {};
    }
    void released(Reservation r,bool success=true) noexcept {
        if(r.slot>=capacity||entries[r.slot].ticket!=r.ticket)return;
        auto& e=entries[r.slot];
        if(success){increment(releases);clear(r.slot);}
        else {e.pending=false;e.cancelled=true;e.fault=true;increment(refused);}
    }
    Reservation reserve(Key key) noexcept {
        if(key.slot>=Slots){increment(cap_refused);increment(refused);return {};}
        const auto h=heap_index(key.heap,true);
        if(h==Heaps){increment(cap_refused);increment(refused);return {};}
        const auto i=h*Slots+key.slot;
        if(!key.address||!key.ordinal||heaps[h].retired||clock==UINT64_MAX||entries[i].ticket){increment(refused);return {};}
        const auto ticket=++clock;
        entries[i]={key,ticket,nullptr,true,false,false};++heaps[h].occupied;
        return {i,ticket,nullptr};
    }
    Resource* finish(Reservation r,Resource* owned,bool fault=false) noexcept {
        if(r.slot>=capacity)return owned;
        auto& e=entries[r.slot];
        if(e.ticket!=r.ticket)return owned;
        e.pending=false;
        if(fault){e.fault=true;increment(refused);return nullptr;}
        if(!owned){clear(r.slot);increment(refused);return nullptr;}
        increment(acquired);
        if(e.cancelled||heaps[r.slot/Slots].retired){e.cancelled=true;e.pending=true;increment(evicted);return owned;}
        e.held=owned;++held;return nullptr;
    }
    Resource* promote(Key key) noexcept {
        const auto i=index(key.heap,key.slot);
        if(i<capacity){auto& e=entries[i];
            if(!heaps[i/Slots].retired&&e.ticket&&!e.cancelled&&!e.pending&&!e.fault&&e.held&&e.key==key) {
                auto* owned=e.held;--held;clear(i);increment(promoted);return owned;
            }
        }
        increment(refused);return nullptr;
    }
    // Retirement/reset is drained one slot at a time, with no automatic array.
    void retire(uint64_t serial) noexcept {
        const auto h=heap_index(serial);if(h<Heaps){heaps[h].retired=true;if(!heaps[h].occupied)heaps[h]={};}
    }
    size_t occupancy() const noexcept {size_t n=0;for(const auto& h:heaps)n+=h.occupied;return n;}
    void append(char* text,size_t bytes) const noexcept {
        const auto n=std::strlen(text);if(n>=bytes)return;
        std::snprintf(text+n,bytes-n,";ca=%x;ce=%x;cp=%x;cf=%x",acquired,evicted,promoted,refused);
    }
    void counts(char* text,size_t bytes) const noexcept {
        std::snprintf(text,bytes,"srv-slot-counts;held=%x;replaced=%x;released=%x;promoted=%x;cap=%x;max=%x;distinct=%x;unsupported=%x;acquired=%x;refused=%x",
            held,replaced,releases,promoted,cap_refused,max_slot,distinct,unsupported,acquired,refused);
    }
};
}
