#pragma once
#include "gpu_copy_ledger.h"
// Only tests define this friend. Production has no arming, recording, creation,
// execution or closure entrypoint until later reviewed integration slices.
namespace acz::gpu_copy {
struct FixtureAccess {
    static bool arm(Session& s, bool on, Backend& b, const Admission& a) {return s.arm(on,b,a);}
    static bool reserve(Session& s,const Admission& a) {return s.reserve(a);}
    static bool created(Session& s,bool ok=true,uint64_t output=11,uint64_t source=22) {return s.created(ok,output,source);}
    static bool owns(Session& s,uint64_t output,uint64_t source) {
        std::lock_guard<std::mutex> lock(s.mutex_);
        return s.private_==output && s.source_==source;
    }
    static bool record(Session& s) {return s.possible_recording();}
    static uint64_t execute(Session& s,const Key& k,unsigned count=1) {return s.begin_execute(k,count);}
    static bool signal(Session& s,uint64_t join=1,bool ok=true) {return s.end_signal(join,ok);}
    static bool close(Session& s,const Key& k) {return s.close_recording(k);}
    static void uncertain(Session& s) {s.uncertain();}
};
inline Admission fixture_admission() {
    Admission a;a.key={1,2,3,4,5};a.allocation_bytes=4096;
    a.history_complete=true;a.retired_generation=false;
    a.private_fence=a.authenticated_queue=a.exclusive_signaller=true;return a;
}
}
