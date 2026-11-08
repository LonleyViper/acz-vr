// Included inside d3d12_hook.cpp's anonymous namespace, after saved dispatch and
// list routing are available. No native object probes or ownership inference.
bool copyObservationEnabled() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_copyObservationTest)return true;
#endif
    auto* log=diagnostics::sink();return log && log->gpu_copy_witness_observation();
}
std::shared_ptr<gpu_copy::ObservationFeed> copyFeed(void* list,void** canonical=nullptr) noexcept {
    if(!copyObservationEnabled())return {};
    try {
    auto& s=state();std::lock_guard lock(s.mutex);
    auto key=listKey_locked(list);
    auto it=s.states.find(key);
    if(it==s.states.end())return nullptr;
    if(canonical)*canonical=key;
    return it->second.copyObservation;
    } catch(...) {return {};}
}
struct CopyNativeCall {
    std::shared_ptr<gpu_copy::ObservationFeed> feed;
    void* key=nullptr; // canonical list key at entry (provider route), null while OFF
    explicit CopyNativeCall(void* list):feed(copyFeed(list,&key)){if(feed)feed->begin_native();}
    ~CopyNativeCall(){if(feed)feed->end_native();}
    void event(gpu_copy::Event e) noexcept {if(feed)feed->event(e);}
};
// Absolute SDK C-vtable slots; cross-checked against the installed header by
// gpu_copy_observation_indices.cpp. Inherited slots stay identical through v10.
constexpr int copyBeginQuery=gpu_copy::slots::begin_query,copyEndQuery=gpu_copy::slots::end_query,
    copyResolveQuery=gpu_copy::slots::resolve_query,copyPredication=gpu_copy::slots::predication,
    copyProtected=gpu_copy::slots::protected_session,copyBeginPass=gpu_copy::slots::begin_pass,
    copyEndPass=gpu_copy::slots::end_pass;
HRESULT STDMETHODCALLTYPE l3Close(ID3D12GraphicsCommandList* self);
void STDMETHODCALLTYPE copySetPredication(ID3D12GraphicsCommandList* self,ID3D12Resource* resource,
                                          UINT64 offset,D3D12_PREDICATION_OP operation) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall observation(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,D3D12_PREDICATION_OP);
    original<Fn>(self,copyPredication)(self,resource,offset,operation);
    observation.event(resource?gpu_copy::Event::predication_on:gpu_copy::Event::predication_off);
}
void STDMETHODCALLTYPE copyBeginQueryHook(ID3D12GraphicsCommandList* self,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,UINT index) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall observation(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12QueryHeap*,D3D12_QUERY_TYPE,UINT);
    original<Fn>(self,copyBeginQuery)(self,heap,type,index);
    observation.event(gpu_copy::Event::begin_query);
}
void STDMETHODCALLTYPE copyEndQueryHook(ID3D12GraphicsCommandList* self,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,UINT index) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall observation(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12QueryHeap*,D3D12_QUERY_TYPE,UINT);
    original<Fn>(self,copyEndQuery)(self,heap,type,index);
    observation.event(gpu_copy::Event::end_query);
}
void STDMETHODCALLTYPE copyResolveQueryHook(ID3D12GraphicsCommandList* self,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,
                                             UINT first,UINT count,ID3D12Resource* dest,UINT64 offset) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall observation(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12QueryHeap*,D3D12_QUERY_TYPE,UINT,UINT,ID3D12Resource*,UINT64);
    original<Fn>(self,copyResolveQuery)(self,heap,type,first,count,dest,offset);
    observation.event(gpu_copy::Event::resolve_query);
}
void STDMETHODCALLTYPE copyProtectedHook(ID3D12GraphicsCommandList3* self,ID3D12ProtectedResourceSession* session) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall observation(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList3*,ID3D12ProtectedResourceSession*);
    original<Fn>(self,copyProtected)(self,session);
    observation.event(gpu_copy::Event::protected_change);
}
void STDMETHODCALLTYPE copyBeginPassHook(ID3D12GraphicsCommandList4* self,UINT count,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,
    D3D12_RENDER_PASS_FLAGS flags) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall observation(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,UINT,const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,D3D12_RENDER_PASS_FLAGS);
    original<Fn>(self,copyBeginPass)(self,count,targets,depth,flags);
    if(auto& feed=observation.feed) {
        feed->event(gpu_copy::Event::begin_pass);
        if(flags&D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS)feed->event(gpu_copy::Event::suspended_pass);
        if(flags&D3D12_RENDER_PASS_FLAG_RESUMING_PASS)feed->event(gpu_copy::Event::resumed_pass);
        // Conservative context policy: every render pass refuses this lifetime.
        // Thus no unobserved cross-list pass can become insertion-eligible.
        feed->event(gpu_copy::Event::cross_list_pass);
    }
}
void STDMETHODCALLTYPE copyEndPassHook(ID3D12GraphicsCommandList4* self) {
    HookAdmission admission;if(!admission)return;
    CopyNativeCall observation(self);
    using Fn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);
    original<Fn>(self,copyEndPass)(self);
    observation.event(gpu_copy::Event::end_pass);
}
void copyBeforeExecute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
    if(!copyObservationEnabled())return;
    auto& s=state();std::lock_guard lock(s.mutex);
    // Explicit cap. Refuse every retained candidate if the array cannot be
    // inventoried completely. Production currently has no retained candidate.
    if(count>64 || (count&&!lists)) {
        for(auto& slot:s.copyFeeds)if(auto feed=slot.lock())feed->uncertain("batch-uninventoried");
        return;
    }
    // Three linear passes; fixed per-list scratch, no batch array/allocation.
    for(UINT i=0;i<count;++i) {
        auto it=s.states.find(listKey_locked(lists[i]));
        if(it!=s.states.end())if(auto& f=it->second.copyObservation)f->occurrences=0;
    }
    for(UINT i=0;i<count;++i) {
        auto it=s.states.find(listKey_locked(lists[i]));
        if(it!=s.states.end())if(auto& f=it->second.copyObservation){++f->occurrences;f->last_ordinal=i;}
    }
    for(UINT i=0;i<count;++i) {
        auto it=s.states.find(listKey_locked(lists[i]));
        if(it!=s.states.end())if(auto& f=it->second.copyObservation)
            if(f->last_ordinal==i)f->before_execute(queue,f->occurrences);
    }
}
bool copyInstallTable_locked(void** table,unsigned version,Hooks& hooks) {
    if(!copyObservationEnabled())return true;
    for(const auto entry:{std::pair<int,void*>{9,reinterpret_cast<void*>(&l3Close)},
        {copyBeginQuery,reinterpret_cast<void*>(&copyBeginQueryHook)},
        {copyEndQuery,reinterpret_cast<void*>(&copyEndQueryHook)},
        {copyResolveQuery,reinterpret_cast<void*>(&copyResolveQueryHook)},
        {copyPredication,reinterpret_cast<void*>(&copySetPredication)}})
        if(!patch(table,entry.first,entry.second,hooks))return false;
    if(version>=3&&!patch(table,copyProtected,reinterpret_cast<void*>(&copyProtectedHook),hooks))return false;
    if(version>=4) {
        if(!patch(table,copyBeginPass,reinterpret_cast<void*>(&copyBeginPassHook),hooks))return false;
        if(!patch(table,copyEndPass,reinterpret_cast<void*>(&copyEndPassHook),hooks))return false;
    }
    return true;
}
void copyAttest_locked(ListState& list,uint64_t aliases) {
    if(!copyObservationEnabled())return;
    if(!list.copyObservation) {
        auto& s=state();
        // Step-1 experiment only: an EXPIRED weak slot (its list retired, no
        // ListState, CopyNativeCall or step-1 owner still holds that feed) may be
        // reused. Without the experiment the 3a permanent-reservation rule holds.
        std::weak_ptr<gpu_copy::ObservationFeed>* slot=nullptr;bool fresh=false;
        if(s.copyFeedReservations<s.copyFeeds.size()){slot=&s.copyFeeds[s.copyFeedReservations];fresh=true;}
        else if(g_step1.load()&&step1Enabled())for(auto& expired:s.copyFeeds)if(expired.expired()){slot=&expired;break;}
        if(!slot) {
            if(s.copyFeedUnavailable<65535)++s.copyFeedUnavailable;
            return; // no witness, all coverage false, native forwarding unchanged
        }
        // 3a: production ownership unavailable (all-null provider). Step 1:
        // the experiment owner's witnessed-creation registry (pure lookups).
        gpu_copy::OwnershipProvider provider=step1Provider();
#ifdef ACZ_VR_OFFLINE_TEST
        if(!provider.reset)provider=g_copyOwnershipTest;
#endif
        list.copyObservation=std::make_shared<gpu_copy::ObservationFeed>(provider);
        if(fresh)++s.copyFeedReservations;
        *slot=list.copyObservation;
    }
    using gpu_copy::Coverage;using gpu_copy::coverage_bit;
    // Ownership bits are supplied only by a complete provider at actual Reset.
    uint64_t bits=0;
    if(aliases&coverage_bit(Coverage::alias0))
        bits=coverage_bit(Coverage::predication)|coverage_bit(Coverage::begin_query)|
            coverage_bit(Coverage::end_query)|coverage_bit(Coverage::resolve_query)|
            coverage_bit(Coverage::reset)|coverage_bit(Coverage::clear_state)|
            coverage_bit(Coverage::bundles)|coverage_bit(Coverage::indirect);
    if(aliases&coverage_bit(Coverage::alias3))bits|=coverage_bit(Coverage::protected_session);
    if(aliases&coverage_bit(Coverage::alias4))bits|=coverage_bit(Coverage::begin_pass)|
        coverage_bit(Coverage::end_pass)|coverage_bit(Coverage::pass_context);
    list.copyObservation->installed(bits|aliases);
}
