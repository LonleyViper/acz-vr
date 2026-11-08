#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::diagnostics {
// Observation only. Caller serializes access; never retain a lock over native
// dispatch. Storage/log budget deliberately refuses layouts above 16 DWORDs.
struct RootConstantWitness {
    static constexpr unsigned capacity=16;
    enum Reason : unsigned { covered, history, overlap, thread, stale, layout,
        root_changed, partial, bundle, indirect, interval, count };
    struct Call { uint64_t before=0,after=0,list=0,generation=0; unsigned thread=0; };
    struct Word {
        uint32_t value=0; bool written=false;
        uint64_t root=0,token=0,list=0,generation=0,revision=0,before=0,after=0;
        unsigned thread=0;
    };
    struct Snapshot {
        Reason reason=history;
        uint64_t list=0,generation=0,revision=0,before=0,after=0;
        uint64_t root=0,token=0,binding_revision=0;
        unsigned index=0,thread=0;
        unsigned stages=0,size=0,mask=0;
        std::array<uint32_t,capacity> values{};
        // Only written values are emitted; unwritten words use ????????.
        void append(char* text,size_t bytes) const noexcept {
            auto n=std::strlen(text);if(n>=bytes)return;
            std::snprintf(text+n,bytes-n,";rc=%x;rv=%x;rm=%04x;rw=",unsigned(reason),stages,mask);
            for(unsigned i=0;i<size;++i) {
                n=std::strlen(text);if(n>=bytes)return;
                if(mask&(1u<<i))std::snprintf(text+n,bytes-n,"%s%08x",i?",":"",values[i]);
                else std::snprintf(text+n,bytes-n,"%s????????",i?",":"");
            }
        }
    };
    std::array<Word,capacity> words{};
    uint64_t clock=0,revision=0,list=0,generation=0,root=0,token=0,currentBefore=0,currentAfter=0;
    unsigned owner=0,active=0,index=0,size=0,stages=0;
    Reason poison=covered,persistent=covered;
    bool configured=false,changed=false;
    void fail(Reason reason) noexcept {
        // A first bundle/indirect refusal must not hide a later lifetime gap
        // when Reset clears the recording-local refusal.
        if(reason!=bundle&&reason!=indirect&&persistent==covered)persistent=reason;
        if(poison==covered)poison=reason;
    }
    uint64_t tick() noexcept {
        if(clock==UINT64_MAX){fail(interval);return clock;}
        return ++clock;
    }
    Call begin(uint64_t id,uint64_t gen,unsigned tid) noexcept {
        if(active)fail(owner==tid?overlap:thread);
        if(owner&&owner!=tid)fail(thread);
        if(!owner)owner=tid;
        if(active==UINT32_MAX)fail(interval);else ++active;
        currentBefore=tick();currentAfter=0;
        return {currentBefore,0,id,gen,tid};
    }
    void returned(Call& call) noexcept {
        call.after=tick();currentAfter=call.after;
        if(revision==UINT64_MAX)fail(interval);else ++revision;
    }
    void end(const Call& call) noexcept {
        if(!call.after)fail(interval);
        if(active)--active;else fail(interval);
    }
    void reset(uint64_t id,uint64_t gen) noexcept {
        // Preserve active intervals and lifetime thread/overlap failures.
        if(poison==bundle||poison==indirect)poison=persistent;
        words.fill(Word{});list=id;generation=gen;root=token=0;
        configured=false;changed=false;size=stages=0;
    }
    void bind(uint64_t id,uint64_t gen,uint64_t key,uint64_t identity,
              unsigned parameter,unsigned length,unsigned visibility,bool accepted) noexcept {
        if(list!=id||generation!=gen)reset(id,gen);
        if(root!=key||token!=identity) {
            changed=configured;words.fill(Word{});root=key;token=identity;
        }
        index=parameter;size=length<=capacity?length:0;stages=visibility;
        configured=accepted&&key&&identity&&length&&length<=capacity;
    }
    void write(const Call& call,uint64_t id,uint64_t gen,unsigned parameter,
               unsigned offset,const uint32_t* data,unsigned length) noexcept {
        if(parameter!=index||!configured)return;
        if(!call.after||call.before>=call.after||call.list!=id||call.generation!=gen||
           list!=id||generation!=gen||active!=1||call.thread!=owner) {fail(interval);return;}
        if(!data||offset>size||length>size-offset) {fail(interval);return;}
        for(unsigned i=0;i<length;++i)words[offset+i]={data[i],true,root,token,id,gen,revision,call.before,call.after,call.thread};
    }
    // J-world-evidence: ONE word's value as of the draw being recorded (called from the Draw seam, BEFORE the native Draw returns, so the
    // draw's own interval is still open: no `after` yet). Same gates as snapshot() for the recording, the root and the word itself, but
    // the other words of the parameter need not have been written. Returns the reason (covered = `value` is the witnessed dword).
    Reason word_at_draw(uint64_t id,uint64_t gen,uint64_t key,uint64_t identity,unsigned tid,bool complete,
                        unsigned i,uint32_t& value) const noexcept {
        if(poison!=covered)return poison;
        if(!complete||!id||!gen)return history;
        if(active!=1)return overlap;               // the current Draw owns the sole interval
        if(!currentBefore)return interval;
        if(tid!=owner)return thread;
        if(list!=id||generation!=gen)return stale;
        if(root!=key||token!=identity)return root_changed;
        if(!configured||i>=size)return layout;
        const auto& w=words[i];
        if(!w.written)return changed?root_changed:partial;
        if(w.root!=key||w.token!=identity)return root_changed;
        if(w.list!=id||w.generation!=gen||w.revision>revision)return stale;
        if(w.thread!=tid||!w.before||w.before>=w.after||w.after>clock)return interval;
        value=w.value;return covered;
    }
    static const char* reason_name(unsigned r) noexcept {
        static constexpr const char* n[]={"covered","history","overlap","thread","stale","layout","root-changed","partial","bundle","indirect","interval"};
        return r<count?n[r]:"?";
    }
    Snapshot snapshot(uint64_t id,uint64_t gen,uint64_t key,uint64_t identity,
                      unsigned tid,bool complete) const noexcept {
        Snapshot out;out.list=id;out.generation=gen;out.revision=revision;
        out.root=root;out.token=token;out.index=index;out.thread=owner;
        out.stages=stages;out.size=size;out.before=currentBefore;out.after=currentAfter;
        for(unsigned i=0;i<size;++i)if(words[i].written) {out.mask|=1u<<i;out.values[i]=words[i].value;}
        if(poison!=covered)out.reason=poison;
        else if(!complete||!id||!gen)out.reason=history;
        else if(active!=1)out.reason=overlap; // current Draw owns the sole interval
        else if(!currentBefore||currentAfter<=currentBefore)out.reason=interval;
        else if(tid!=owner)out.reason=thread;
        else if(list!=id||generation!=gen)out.reason=stale;
        else if(root!=key||token!=identity)out.reason=root_changed;
        else if(!configured)out.reason=layout;
        else {
            out.reason=covered;
            for(unsigned i=0;i<size;++i) {
                const auto& w=words[i];
                if(!w.written) {out.reason=changed?root_changed:partial;break;}
                if(w.root!=key||w.token!=identity) {out.reason=root_changed;break;}
                if(w.list!=id||w.generation!=gen||w.revision>revision) {out.reason=stale;break;}
                if(w.thread!=tid||!w.before||w.before>=w.after||w.after>clock) {out.reason=interval;break;}
            }
        }
        return out;
    }
};
struct RootConstantCounters {
    std::array<unsigned,RootConstantWitness::count> reasons{};
    std::array<uint32_t,16> distinct{};
    unsigned values=0;
    void record(const RootConstantWitness::Snapshot& snap) noexcept {
        auto& n=reasons[snap.reason];if(n<65535)++n;
        for(unsigned i=0;i<snap.size;++i)if(snap.mask&(1u<<i)) {
            bool seen=false;for(unsigned j=0;j<values;++j)seen|=distinct[j]==snap.values[i];
            if(!seen&&values<distinct.size())distinct[values++]=snap.values[i];
        }
    }
    void append(char* text,size_t bytes) const noexcept {
        auto n=std::strlen(text);if(n>=bytes)return;
        std::snprintf(text+n,bytes-n,";wc=");
        for(auto value:reasons) {n=std::strlen(text);if(n>=bytes)return;std::snprintf(text+n,bytes-n,"%04x",value);}
        n=std::strlen(text);if(n<bytes)std::snprintf(text+n,bytes-n,";wv=%02x",values);
    }
};
} // namespace acz::diagnostics
