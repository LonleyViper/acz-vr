// GPU-copy step 3b part 2: the design section 6 "Step 1" experiment owner.
// Included inside d3d12_hook.cpp's anonymous namespace, BEFORE
// gpu_copy_observation_hooks.inl (whose copyAttest_locked supplies this
// owner's ownership provider to new observation feeds).
//
// Default OFF. Nothing below allocates, patches, creates or records unless
// step1Enabled(): logging + diagnostic_ownership_pins + gpu_copy_witness_observation
// + gpu_copy_step1_experiment are ALL latched. While OFF the only residue on the
// hook paths is one relaxed atomic load of g_step1Phase (value 0).
//
// One attempt, one in-flight transaction, no game resource touched:
//   owner context (worker thread started outside loader lock at swap-chain
//   discovery): create proxy UPLOAD (GENERIC_READ) + READBACK (COPY_DEST)
//   4096-byte committed buffers on the game's device via saved native dispatch,
//   write the deterministic pattern once, then never map the source again;
//   create the private fence (Timeline) on the game's direct queue.
//   Draw seam (game thread): first matched DrawCPT draw on a fully witnessed list
//   arms the ledger + timeline and records ONE CopyBufferRegion(readback, 0,
//   upload, 0, 4096) through saved dispatch, then the hook forwards the original
//   Draw unchanged exactly once. No allocation, resource creation or logging here.
//   Execute (game thread): Timeline::begin before native forwarding, ONE
//   queue->Signal(fence, 1) after native return, witness/ledger join.
//   Worker: nonblocking bounded polls, Map only after same-timeline completion
//   AND observed Reset closure, byte compare + SHA-256, release on PASS only.
// Every failure or ambiguity quarantines (retains every owner) and closes the
// experiment for the session. Nothing retries, waits or blocks a game thread.
//
// Lock order: step-1 mutex -> hook-state mutex (canonical routes / saved
// dispatch); step-1 mutex -> feed -> witness -> ledger; step-1 mutex ->
// timeline -> registry. No path takes the step-1 mutex while holding the
// hook-state mutex. No native call is made with the step-1 mutex held:
// CopyBufferRegion, Signal, GetCompletedValue and Map all run unlocked
// (GetDeviceRemovedReason in the worker's failure disposition is the one
// diagnostic exception; it is not a game-thread path).
#ifndef ACZ_VR_SOURCE_PIN
#define ACZ_VR_SOURCE_PIN "unconfigured"
#endif
#ifdef ACZ_VR_OFFLINE_TEST
bool g_step1Test=false;                     // force enablement without a sink
bool g_step1NoThread=false;                 // fixture drives step1Tick()
void (*g_step1Capture)(const char*)=nullptr; // every emitted step-1 line
bool g_step1EarlyTest=false;                // force gpu_copy_early_device_hook without a sink
bool g_step2Test=false;                     // J-3b6: with g_step1Test, run the step-2 mode
bool g_step3Test=false;                     // J-3b7: with g_step1Test, run the step-3 mode
bool g_step3WholeSetTest=false;             // J-3b7f: with g_step3Test, the whole-set mode
bool g_step5Test=false;                     // J-3b8: with g_step3WholeSetTest, the continuous producer
const wchar_t* g_step3PngDir=nullptr;       // J-3b7: PNG directory instead of the log directory
#endif
// J-3b8: `continuous` = the step-5 producer is running (never a one-shot transaction).
enum class Step1Phase : int {off=0,preparing=1,ready=2,inserted=3,done=4,continuous=5};
std::atomic<int> g_step1Phase{0};
// J-3b6: which experiment the shared owner runs. step2 additionally requires
// gpu_copy_early_device_hook; configuring both experiments refuses both.
// J-3b7: step3 has step 2's opt-ins; any two experiments configured refuse all.
enum class Step1Mode : unsigned {off=0,step1=1,step2=2,conflict=3,step3=4};
Step1Mode step1Mode() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_step3Test?Step1Mode::step3:g_step2Test?Step1Mode::step2:Step1Mode::step1;
#endif
    auto* log=diagnostics::sink();
    if(!log)return Step1Mode::off;
    const bool one=log->gpu_copy_step1_experiment(),two=log->gpu_copy_step2_experiment(),three=log->gpu_copy_step3_experiment();
    if(unsigned(one)+unsigned(two)+unsigned(three)>1)return Step1Mode::conflict;
    if(!log->ownership_pins()||!log->gpu_copy_witness_observation())return Step1Mode::off;
    if(three)return log->gpu_copy_early_device_hook()?Step1Mode::step3:Step1Mode::off;
    if(two)return log->gpu_copy_early_device_hook()?Step1Mode::step2:Step1Mode::off;
    return one?Step1Mode::step1:Step1Mode::off;
}
bool step1Enabled() noexcept {
    const auto mode=step1Mode();
    return mode==Step1Mode::step1||mode==Step1Mode::step2||mode==Step1Mode::step3;
}
bool step2Mode() noexcept {return step1Mode()==Step1Mode::step2;}
bool step3Mode() noexcept {return step1Mode()==Step1Mode::step3;}
// J-3b7f: whole-set sub-mode of step 3 (latched at owner creation).
bool step3WholeSetConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_step3WholeSetTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step3_whole_set();
}
// Draw path: subsequent members of an inserted set (relaxed load; 0 while OFF).
std::atomic<int> g_step3SetActive{0};
// J-3b4: the early device notification (from the optional d3d12.dll proxy) is
// acted on only with the whole step-1 enablement AND gpu_copy_early_device_hook.
bool step1EarlyEnabled() noexcept {
    if(!step1Enabled())return false;
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1EarlyTest)return true;
#endif
    auto* log=diagnostics::sink();
    return log&&log->gpu_copy_early_device_hook();
}
using Step1Route=gpu_copy::owner::Route;
template<class T> Step1Route step1Route(T* p) noexcept {return reinterpret_cast<Step1Route>(p);}
// Saved native dispatch for a slot: the retained original when this proxy
// patched it, else the live table entry (which then is not a proxy hook).
void* step1Native(void* object,int slot) noexcept {
    auto& s=state();std::lock_guard lock(s.mutex);
    void** table=vtable(object);
    for(auto* map:{&s.lists,&s.queues,&s.devices,&s.objects,&s.compatibility}) {
        auto it=map->find(table);
        if(it!=map->end()&&it->second.originals[slot])return it->second.originals[slot];
    }
    return table[slot];
}
// J-3b6: in step-2 mode every shared-machinery line is relabelled
// "gpu-copy-step2" (J-3b7: "gpu-copy-step3" in step-3 mode; same width, same
// reserved partition). Worker/discovery paths only (never the Draw seam), so
// the one-record local is not a Draw frame.
void step1Relabel(const char* text,char (&out)[diagnostics::text_capacity],char digit) noexcept {
    std::snprintf(out,sizeof out,"%s",text);
    if(std::strncmp(out,"gpu-copy-step1",14)==0)out[13]=digit;
}
char step1LabelDigit() noexcept {return step3Mode()?'3':step2Mode()?'2':'1';}
void step1Emit(const char* text) noexcept {
    char relabelled[diagnostics::text_capacity];
    if(const char d=step1LabelDigit();d!='1'){step1Relabel(text,relabelled,d);text=relabelled;}
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Capture)g_step1Capture(text);
#endif
    diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
}
// J-3b4 early-device evidence goes to the STARTUP partition (Event::device),
// never to the experiment partition (step1_records).
void step1EmitStartup(const char* text) noexcept {
    char relabelled[diagnostics::text_capacity];
    if(const char d=step1LabelDigit();d!='1'){step1Relabel(text,relabelled,d);text=relabelled;}
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Capture)g_step1Capture(text);
#endif
    diagnostics::emit(diagnostics::record(diagnostics::Event::device,0,0,0,text));
}
bool step1Sha256(const void* bytes,size_t n,char (&out)[65]) noexcept {
    uint8_t digest[32]{};
    const auto status=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,
        const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)),static_cast<ULONG>(n),digest,sizeof digest);
    if(status<0){std::snprintf(out,sizeof out,"unavailable");return false;}
    gpu_copy::step1::hex(digest,sizeof digest,out);return true;
}
// Worker only: streaming SHA-256 of a file (DLL / config pin). Bounded reads.
bool step1FileSha256(const wchar_t* path,char (&out)[65]) noexcept {
    std::snprintf(out,sizeof out,"unavailable");
    if(!path||!*path)return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BCRYPT_HASH_HANDLE hash=nullptr;bool ok=BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE,&hash,nullptr,0,nullptr,0,0)>=0;
    std::unique_ptr<uint8_t[]> buffer(new(std::nothrow) uint8_t[65536]);
    uint64_t total=0;
    while(ok&&buffer) {
        DWORD got=0;
        if(!ReadFile(file,buffer.get(),65536,&got,nullptr)){ok=false;break;}
        if(!got)break;
        total+=got;if(total>(uint64_t{256}<<20)){ok=false;break;} // 256 MiB bound
        ok=BCryptHashData(hash,buffer.get(),got,0)>=0;
    }
    uint8_t digest[32]{};
    if(hash){ok=ok&&buffer&&BCryptFinishHash(hash,digest,sizeof digest,0)>=0;BCryptDestroyHash(hash);}
    CloseHandle(file);
    if(ok)gpu_copy::step1::hex(digest,sizeof digest,out);
    return ok;
}
struct Step1State;
std::atomic<Step1State*> g_step1{nullptr};
// Private fence backend: CreateFence(0), Signal on the bound queue, nonblocking
// GetCompletedValue. Device slot 36, queue slot 14 and fence slots are never
// patched by this proxy; ordinary dispatch reaches the runtime directly.
struct Step1Fence final:gpu_copy::owner::FenceBackend {
    ID3D12Fence* fence=nullptr;
    HRESULT createHr=E_PENDING,signalHr=E_PENDING;
    bool create(Step1Route device,Step1Route& out) noexcept override {
        ID3D12Fence* value=nullptr;
        createHr=reinterpret_cast<ID3D12Device*>(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&value));
        fence=value;out=step1Route(value); // a failed non-null output is retained, never released
        return SUCCEEDED(createHr)&&value;
    }
    bool signal(Step1Route queue,Step1Route value,uint64_t v) noexcept override {
        signalHr=reinterpret_cast<ID3D12CommandQueue*>(queue)->Signal(reinterpret_cast<ID3D12Fence*>(value),v);
        return SUCCEEDED(signalHr);
    }
    bool sawMax=false; // device-removal sentinel observed (never completion)
    uint64_t completed(Step1Route value) noexcept override {
        const auto v=reinterpret_cast<ID3D12Fence*>(value)->GetCompletedValue();
        if(v==(std::numeric_limits<uint64_t>::max)())sawMax=true;
        return v;
    }
};
// READBACK Map: subresource 0, exact read range, Unmap with an empty written range.
struct Step1Map final:gpu_copy::owner::ReadbackMap {
    ID3D12Resource* readback=nullptr;
    HRESULT mapHr=E_PENDING;unsigned maps=0,unmaps=0;
    bool map(uint64_t,uint64_t offset,uint64_t bytes,const uint8_t*& data) noexcept override {
        using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
        const D3D12_RANGE range{static_cast<SIZE_T>(offset),static_cast<SIZE_T>(offset+bytes)};
        void* p=nullptr;++maps;
        mapHr=reinterpret_cast<Fn>(step1Native(readback,8))(readback,0,&range,&p);
        data=SUCCEEDED(mapHr)&&p?static_cast<const uint8_t*>(p)+offset:nullptr;
        return data!=nullptr;
    }
    void unmap(uint64_t) noexcept override {
        using Fn=void(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*);
        const D3D12_RANGE none{0,0};++unmaps;
        reinterpret_cast<Fn>(step1Native(readback,9))(readback,0,&none);
    }
};
struct Step1Ledger final:gpu_copy::Backend {
    Step1State* owner=nullptr;
    gpu_copy::FenceSample poll() noexcept override;
    bool read() noexcept override;
    void drop_logical_source_lease(uint64_t) noexcept override {} // logical only
    void release_private(uint64_t) noexcept override;
};
// J-3b3 creation-witness diagnostics. Counts only; never an ownership input.
enum class Step1List1Hook : unsigned {not_attempted=0,installed=1,distinct_table=2,qi_failed=3,patch_failed=4};
struct Step1Creates {
    // seen: wrapper calls; recorded: live registry record (DIRECT or not);
    // nondirect: recorded but not DIRECT; unregistered: DIRECT success whose
    // capture certification failed (tombstoned); failed: failed HRESULT or null
    // output; collision/parent/exhausted: Registry::Created outcomes.
    uint32_t seen=0,recorded=0,nondirect=0,unregistered=0,failed=0,collision=0,parent=0,exhausted=0;
};
// Distinct routes seen at refused Resets (diagnostic routing values only; a
// full set saturates and is reported with '+').
struct Step1Routes {
    std::array<uintptr_t,32> routes{};unsigned count=0;bool overflow=false;
    void add(uintptr_t r) noexcept {
        if(!r)return;
        for(unsigned i=0;i<count;++i)if(routes[i]==r)return;
        if(count<routes.size())routes[count++]=r;else overflow=true;
    }
};
// J-3b7 step-3 owner facts (heap storage inside Step1State). The Draw seam
// writes shape/live/restore facts under the step-1 mutex; the owner context
// (worker) creates the private objects and publishes them by setting `armed`
// under the same mutex. Defined here so Step1State can hold it by value.
struct Step3Fields {
    gpu_copy::step3::Shape shape{},live{};   // observed shape; shape at insertion
    gpu_copy::step3::Targets targets{};      // decided from `shape`
    const char* refusal=nullptr;             // observation refused (static literal)
    const char* waitReason="not-observed";   // why the targets are not armed yet
    const char* mismatch=nullptr;            // first reason a later draw did not match
    uint64_t mismatches=0,waitingDraws=0,tObserved=0,tArmed=0;
    bool observed=false,armed=false,emittedObserve=false,finished=false;
    // Restore facts captured at the inserting draw (the game's current state).
    UINT omCount=0;BOOL omSingle=FALSE;std::array<D3D12_CPU_DESCRIPTOR_HANDLE,8> omRtv{};
    bool omDsv=false;D3D12_CPU_DESCRIPTOR_HANDLE omDsvHandle{};
    UINT t10=0;D3D12_GPU_VIRTUAL_ADDRESS t10Original=0;
    // Private objects (owner context only; never created on a Draw path).
    ID3D12Resource* rt=nullptr;ID3D12Resource* ds=nullptr;ID3D12Resource* upload=nullptr;ID3D12Resource* readback=nullptr;
    ID3D12DescriptorHeap* rtvHeap=nullptr;ID3D12DescriptorHeap* dsvHeap=nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{},dsv{};D3D12_GPU_VIRTUAL_ADDRESS uploadVa=0;
    HRESULT rtHr=E_PENDING,dsHr=E_PENDING,uploadHr=E_PENDING,readbackHr=E_PENDING,rtvHeapHr=E_PENDING,dsvHeapHr=E_PENDING,writeHr=E_PENDING;
    gpu_copy::step3::Substitution sub{};
    uint64_t snapList=0,snapGen=0,snapDraw=0,snapMs=0,snapVa=0,snapResource=0;
    std::atomic<unsigned> issued{0};
    // Results (worker).
    std::unique_ptr<uint8_t[]> pixels;uint64_t pixelBytes=0;bool read=false;HRESULT mapHr=E_PENDING;
    std::array<gpu_copy::step3::Pixels,gpu_copy::step3::replay_count> stats{};
    char sha[gpu_copy::step3::replay_count][65]{};
    char png[gpu_copy::step3::replay_count][160]{};char pngSha[gpu_copy::step3::replay_count][65]{};
    uint64_t pngBytes[gpu_copy::step3::replay_count]{};bool pngOk[gpu_copy::step3::replay_count]{};
    uint64_t controlDiff=0,eyeDiff=0;int64_t controlFirst=-1,eyeFirst=-1;
    const char* contentFail=nullptr;bool processed=false;
    // J-3b7b: the draw's vertex buffer from the same CPU snapshot (crop input),
    // the crop, FP16 statistics and the bounded owner-context waits.
    uint32_t vbStride=0;const char* vbReason="not-sampled";
    // ---- J-3b7d selection of a VISIBLE candidate ----
    // Draw seam: fixed-size candidate records into a bounded ring (step-1
    // mutex already held on that path; full ring = drop + count, never wait).
    struct Candidate {gpu_copy::step3::Shape shape{};uint64_t seq=0,tick=0;};
    std::array<Candidate,gpu_copy::step3::candidate_ring> ring{};
    uint64_t ringHead=0,ringTail=0,recorded=0,dropped=0;
    bool selectionClosed=false;unsigned selectTicks=0;
    // Worker only: evaluation results and the chosen candidate's sampled bytes.
    uint64_t evaluated=0,visibleCount=0,unreadable=0;unsigned tableLines=0;
    bool haveBest=false;Candidate best{};gpu_copy::step3::CandidateEval bestEval{};
    std::array<uint8_t,416> bestSrt{},srcSrt{},liveSrt{};
    std::vector<uint8_t> bestVb,bestIb,sampleVb,sampleIb,scratch;
    uint32_t bestVbFirst=0,bestVbCount=0;uint64_t srcTick=0,srcVa=0;
    // Insertion-time freshness (the seam compares the live SrtData with srcSrt).
    bool stalePending=false;unsigned staleRearms=0;uint64_t staleSeen=0,orderShifted=0,skipped=0,freshUnreadable=0;
    // J-3b7e inline substitution outputs (written at the inserting draw) and the
    // persistently mapped private UPLOAD slot (mapped in the owner context).
    std::array<std::array<uint8_t,416>,gpu_copy::step3::blob_count> inlineBlobs{};uint8_t* uploadPtr=nullptr;
    std::string tableText;char tableName[96]{};char tableSha[65]{};bool tableOk=false;
    const char* freshness="not-checked";
    const char* firstRefusal=nullptr;gpu_copy::step3::Shape firstRefused{};
    // ---- J-3b7f whole-set mode ----
    bool wholeSet=false,continuous=false;
    struct SetMember {gpu_copy::step3::Shape shape{};uint64_t seq=0;gpu_copy::step3::CandidateEval eval{};const char* status="pending";};
    std::array<SetMember,gpu_copy::step3::set_cap> members{};
    unsigned memberCount=0,setNext=0,setRecorded=0,setSkipped=0,setExcluded=0;
    const char* setStop=nullptr;bool copiesRecorded=false,copiesAtClose=false;
    uint64_t setGroupList=0,setGroupGen=0;double setArea=0;
    std::vector<Candidate> visibleCands;std::vector<gpu_copy::step3::CandidateEval> visibleEvals;
    ID3D12Resource* rt2=nullptr;ID3D12Resource* ds2=nullptr;ID3D12DescriptorHeap* rtvHeap2=nullptr;ID3D12DescriptorHeap* dsvHeap2=nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv2{},dsv2{};HRESULT rt2Hr=E_PENDING,ds2Hr=E_PENDING;
    gpu_copy::step3::XcorrResult xcorr{};
    char pngSrgb[2][168]{},pngSrgbSmall[2][176]{},pngSrgbSha[2][65]{},pngSrgbSmallSha[2][65]{};
    // Draw arguments of the seam being evaluated (insertion or a set member).
    bool aIndexed=false;UINT aCount=0,aInstances=0,aStart=0,aStartInstance=0;INT aBase=0;
    char pngSmall[gpu_copy::step3::replay_count][168]{};char pngSmallSha[gpu_copy::step3::replay_count][65]{};
    uint64_t pngSmallBytes[gpu_copy::step3::replay_count]{};uint32_t smallW=0,smallH=0;
    gpu_copy::step3::Crop crop{};
    std::array<gpu_copy::step3::FloatStats,gpu_copy::step3::replay_count> f16{};
    unsigned observedTicks=0,armedTicks=0;uint64_t tFinished=0;
};
struct Step1State {
    std::mutex mutex;
    gpu_copy::owner::Registry registry;
    gpu_copy::owner::Timeline timeline;
    Step1Fence fence;Step1Map map;Step1Ledger ledger;
    ID3D12Device* device=nullptr;ID3D12CommandQueue* queue=nullptr; // borrowed; HookState owns
    UINT queueNodeMask=0;
    ID3D12Resource* upload=nullptr;ID3D12Resource* readback=nullptr;
    HRESULT uploadHr=E_PENDING,readbackHr=E_PENDING,writeMapHr=E_PENDING,removedHr=S_OK;
    std::array<uint8_t,gpu_copy::step1::copy_bytes> pattern{},observed{};
    char expectedSha[65]{},observedSha[65]{},dllSha[65]{},cfgSha[65]{};
    std::array<Step1Route,gpu_copy::owner::Timeline::batch_cap> routes{};
    char text[diagnostics::text_capacity]{};
    // Draw-seam qualification counters (refusal bit histogram, see emit).
    uint64_t matchedDraws=0,resets=0,resetsAccepted=0;
    std::array<uint32_t,14> drawRefusals{},resetRefusals{};
    // J-3b3 creation witnesses: [0] CreateCommandList, [1] CreateCommandList1,
    // [2] CreateCommandAllocator. Ready-phase Resets by the list's capture
    // registration path: [0] base CreateCommandList, [1] CreateCommandList1,
    // [2] registered at first Reset (creation never seen), [3] unobserved
    // placeholder state, [4] no capture state at all.
    std::array<Step1Creates,3> creates{};
    std::atomic<unsigned> list1Hook{0};
    // J-3b4 early device witness (default OFF). `early`: this owner was created
    // from the d3d12 proxy's device-created notification, before swap-chain
    // discovery; device creation itself is the device witness. Queue creation
    // witnesses: [0] CreateCommandQueue (slot 8), [1] CreateCommandQueue1
    // (ID3D12Device9 slot 75, shared base table only). queueHook: 0 not
    // attempted, 1 installed, 4 patch failed. queueWitness names how the
    // discovered queue was authenticated (written once at discovery).
    bool early=false,discoveryMismatch=false,discovered=false;
    std::array<Step1Creates,2> queueCreates{};
    std::atomic<unsigned> queueHook{0},queue1Hook{0};
    const char* queueWitness="proxy-retained-reference";
    std::array<uint32_t,5> resetAcceptedByPath{},resetRefusedByPath{};
    Step1Routes unseenLists,unseenAllocators;
    // Transaction facts (written once under the mutex).
    std::shared_ptr<gpu_copy::ObservationFeed> feed;
    Step1Route candidate=0,pso=0,executeQueue=0;
    gpu_copy::Identity identity{};gpu_copy::Key key{};
    uint64_t coverage=0,l3Draw=0,matchedAtInsert=0;
    bool indexed=false;UINT count=0,instances=0,start=0,startInstance=0;INT base=0;
    const char* armRefusal=nullptr;bool recorded=false;
    unsigned closes=0,resetsAfter=0;HRESULT closeHr=E_PENDING,resetHr=E_PENDING;
    uint64_t executesArmed=0,timelineTicket=0,witnessTicket=0,submission=0;
    unsigned executeCount=0,executeOrdinal=0,executeOccurrences=0;bool executeUnknown=false;
    bool signalCalled=false,signalOk=false,witnessJoined=false;HRESULT signalHr=E_PENDING;
    // Worker-only progression (Backend::poll runs on the worker).
    unsigned polls=0,firstPoll=0,completedPoll=0;uint64_t firstValue=0,completedValue=0;bool sawPending=false;
    bool timedOut=false;
    uint64_t tStart=0,tInsert=0,tSignal=0,tComplete=0,tClosed=0,tEnd=0;
    unsigned waitTicks=0,submitTicks=0,closureTicks=0;
    bool emittedInsert=false,emittedClose=false,emittedExecute=false,emittedSignal=false,emittedPoll=false,emittedReset=false;
    bool released=false;
    // ---- J-3b6 step-2 mode (latched at owner creation; heap storage only) ----
    bool step2=false;
    gpu_copy::step2::Plan plan{};      // written once at the Draw seam under this mutex
    std::array<std::array<uint8_t,gpu_copy::step2::region_bytes>,gpu_copy::step2::role_count> roleBytes{};
    std::array<bool,gpu_copy::step2::role_count> roleRead{};
    char roleSha[gpu_copy::step2::role_count][65]{};
    // CPU snapshot comparison per role (worker, under the hook mutex).
    const char* roleCpu[gpu_copy::step2::role_count]={"not-copied","not-copied","not-copied"};
    long long cpuFirstDiff[gpu_copy::step2::role_count]={-1,-1,-1};
    uint64_t cpuHash[gpu_copy::step2::role_count]{},cpuEventsAfter[gpu_copy::step2::role_count]{};
    uint64_t cpuList=0,cpuGen=0,cpuDraw=0,cpuMs=0;size_t cpuAttempts=0;const char* cpuReason="not-evaluated";
    const char* indexStatus="not-copied";
    bool cleanRetired=false; // step 2: retired after a clean transaction (PASS or partial)
    uint64_t closedGeneration=0; // J-3b7f: witness closed generation sampled at the first Close after insertion
    // ---- J-3b7 step-3 mode (latched at owner creation) ----
    bool step3=false;
    Step3Fields s3;
    Step1State() noexcept {ledger.owner=this;}
};
bool step2Read(Step1State&) noexcept;
bool step3Read(Step1State&) noexcept;
void step3ReleasePrivate(Step1State&) noexcept;
gpu_copy::FenceSample Step1Ledger::poll() noexcept {
    auto& st=*owner;
    const auto p=st.timeline.poll(); // native GetCompletedValue, no lock held
    if(p.sampled) {
        ++st.polls;
        if(!st.firstPoll){st.firstPoll=st.polls;st.firstValue=p.value;}
        if(p.value==0)st.sawPending=true;
        if(st.timeline.complete(st.timelineTicket)&&!st.completedPoll){st.completedPoll=st.polls;st.completedValue=p.value;}
    }
    if(p.timed_out)st.timedOut=true;
    return st.timeline.sample();
}
bool Step1Ledger::read() noexcept {
    auto& st=*owner;
    if(st.step2)return step2Read(st);
    if(st.step3)return step3Read(st);
    // Ledger already required authenticated completion AND recording closure.
    return gpu_copy::owner::read_back(st.timeline,st.timelineTicket,true,st.map,gpu_copy::step1::private_token,
        0,gpu_copy::step1::copy_bytes,st.observed.data(),st.observed.size());
}
void Step1Ledger::release_private(uint64_t) noexcept {
    auto& st=*owner;
    // Only reached through Session::retire: completed, closed, consumed.
    releaseOwned(st.readback);releaseOwned(st.upload);
    st.readback=nullptr;st.upload=nullptr;st.map.readback=nullptr;st.released=true;
    if(st.step3)step3ReleasePrivate(st);
}
gpu_copy::OwnershipProvider step1Provider() noexcept {
    auto* st=g_step1.load();
    return st&&step1Enabled()?st->registry.provider():gpu_copy::OwnershipProvider{};
}
#include "gpu_copy_step2_hooks.inl" // J-3b6 step-2 adapter (default OFF)
#include "gpu_copy_step3_hooks.inl" // J-3b7 step-3 adapter (default OFF)
#include "gpu_copy_step5_hooks.inl" // J-3b8 step-5 continuous producer (default OFF)
// ------------------------------------------------------------- witnesses
// Device and queue: the proxy's own retained references from swap-chain
// discovery (HookState::device/queue, never released while the game runs) are
// the lifetime authentication. Both objects predate the proxy's device hooks,
// so there is no creation witness; CreateCommandQueue is NOT hooked. Any
// other queue stays unwitnessed and every Execute on it is unauthenticated.
void step1EmitDiscovery(Step1State& st) noexcept;
void step1WitnessDeviceQueue(ID3D12Device* device,ID3D12CommandQueue* queue,const D3D12_COMMAND_QUEUE_DESC& desc) noexcept {
    if(!step1Enabled())return;
    if(auto* early=g_step1.load()) {
        // J-3b4: the owner already exists from the early device notification.
        // Join the discovered queue once. A queue witnessed at creation keeps
        // its creation record; otherwise the proxy's retained reference is the
        // authentication, exactly as without the early hook.
        if(!early->early)return;
        {std::lock_guard lock(early->mutex);
         if(early->discovered)return;
         early->discovered=true;
         if(device!=early->device)early->discoveryMismatch=true;
         else {
             early->queue=queue;early->queueNodeMask=desc.NodeMask;
             if(early->registry.live_token(step1Route(queue),gpu_copy::owner::Kind::queue))early->queueWitness="creation";
         }
         early->tStart=GetTickCount64();}
        if(!early->discoveryMismatch&&std::string_view(early->queueWitness)!="creation")
            early->registry.queue_created(true,step1Route(queue),step1Route(device),desc.NodeMask,desc.Type==D3D12_COMMAND_LIST_TYPE_DIRECT);
        step1EmitDiscovery(*early);
        return;
    }
    auto* st=new(std::nothrow) Step1State;
    if(!st)return;
    st->step2=step2Mode();st->step3=step3Mode();st->s3.wholeSet=st->step3&&step3WholeSetConfigured();
    st->s3.continuous=st->s3.wholeSet&&step5Configured();
    st->registry.enable();st->timeline.enable();
    st->device=device;st->queue=queue;st->queueNodeMask=desc.NodeMask;
    st->registry.device_created(true,step1Route(device));
    st->registry.queue_created(true,step1Route(queue),step1Route(device),desc.NodeMask,desc.Type==D3D12_COMMAND_LIST_TYPE_DIRECT);
    st->tStart=GetTickCount64();
    g_step1.store(st); // process lifetime; never destroyed (feeds borrow its registry)
}
// J-3b4: the owner from the early device notification (device creation is the
// device witness; the queue is joined at discovery). The caller retains its own
// device reference for the process lifetime (never released), like HookState's
// discovered device, so the device route cannot be reused while it is an identity.
Step1State* step1WitnessEarlyDevice(ID3D12Device* device) noexcept {
    if(!device||!step1EarlyEnabled()||g_step1.load())return nullptr;
    auto* st=new(std::nothrow) Step1State;
    if(!st)return nullptr;
    st->early=true;st->step2=step2Mode();st->step3=step3Mode();st->s3.wholeSet=st->step3&&step3WholeSetConfigured();
    st->s3.continuous=st->s3.wholeSet&&step5Configured();
    st->registry.enable();st->timeline.enable();
    st->device=device;
    st->registry.device_created(true,step1Route(device));
    st->tStart=GetTickCount64();
    Step1State* expected=nullptr;
    if(!g_step1.compare_exchange_strong(expected,st)){delete st;return nullptr;} // never published
    return st;
}
void step1CountCreate(Step1State& st,unsigned path,gpu_copy::owner::Created outcome,bool failed,
                      bool unregistered,bool direct) noexcept;
// CreateCommandQueue / CreateCommandQueue1 witness (early owner only). The queue
// route is the ID3D12CommandQueue interface pointer the game later executes on.
void step1QueueCreated(HRESULT hr,ID3D12Device* device,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID iid,void** out,
                       unsigned path) noexcept {
    auto* st=g_step1.load();if(!st||!st->early)return;
    if(!out||!*out){step1CountCreate(*st,3+path,gpu_copy::owner::Created::off,true,false,false);return;}
    const bool direct=desc&&desc->Type==D3D12_COMMAND_LIST_TYPE_DIRECT;
    const bool ok=SUCCEEDED(hr)&&desc&&iid==__uuidof(ID3D12CommandQueue);
    const auto outcome=st->registry.queue_created(ok,step1Route(*out),step1Route(device),desc?desc->NodeMask:0,direct);
    step1CountCreate(*st,3+path,outcome,!ok,false,direct);
}
void step1EmitDiscovery(Step1State& st) noexcept {
    char text[diagnostics::text_capacity]{};
    {std::lock_guard lock(st.mutex);
     const auto& q=st.queueCreates[0];const auto& q1=st.queueCreates[1];
     std::snprintf(text,sizeof text,"gpu-copy-step1;event=early-device-discovery;match=%u;queue=%llx;queue-witness=%s;"
        "CQ=%x:%x:%x:%x:%x;CQ1=%x:%x:%x:%x:%x;cq-hook=%u;cq1-hook=%u;cl1-hook=%u",unsigned(!st.discoveryMismatch),
        static_cast<unsigned long long>(step1Route(st.queue)),st.discoveryMismatch?"none":st.queueWitness,
        q.seen,q.recorded,q.nondirect,q.failed,q.collision,q1.seen,q1.recorded,q1.nondirect,q1.failed,q1.collision,
        st.queueHook.load(),st.queue1Hook.load(),st.list1Hook.load());}
    step1EmitStartup(text);
}
// Counts one creation outcome (step-1 mutex leaf; registry already returned).
void step1CountCreate(Step1State& st,unsigned path,gpu_copy::owner::Created outcome,bool failed,
                      bool unregistered,bool direct) noexcept {
    using gpu_copy::owner::Created;
    auto inc=[](uint32_t& v){if(v<UINT32_MAX)++v;};
    std::lock_guard lock(st.mutex);
    // 0..2: CL, CL1, CA; 3..4: J-3b4 CQ, CQ1.
    auto& c=path<3?st.creates[path]:st.queueCreates[path-3];inc(c.seen);
    if(failed){inc(c.failed);return;}
    if(unregistered){inc(c.unregistered);return;}
    switch(outcome) {
    case Created::recorded:inc(c.recorded);if(!direct)inc(c.nondirect);break;
    case Created::failed:inc(c.failed);break;
    case Created::collision:inc(c.collision);break;
    case Created::parent:inc(c.parent);break;
    case Created::exhausted:inc(c.exhausted);break;
    default:break;
    }
}
void step1AllocatorCreated(HRESULT hr,ID3D12Device* device,D3D12_COMMAND_LIST_TYPE type,REFIID iid,void** out) noexcept {
    auto* st=g_step1.load();if(!st)return;
    if(!out||!*out){step1CountCreate(*st,2,gpu_copy::owner::Created::off,true,false,false);return;}
    const bool ok=SUCCEEDED(hr)&&iid==__uuidof(ID3D12CommandAllocator);
    const auto outcome=st->registry.allocator_created(ok,step1Route(*out),step1Route(device),type==D3D12_COMMAND_LIST_TYPE_DIRECT);
    step1CountCreate(*st,2,outcome,!ok,false,type==D3D12_COMMAND_LIST_TYPE_DIRECT);
}
// `closed`: CreateCommandList1 (no allocator, created closed); else CreateCommandList.
void step1ListCreated(HRESULT hr,ID3D12Device* device,UINT node,D3D12_COMMAND_LIST_TYPE type,
                      ID3D12CommandAllocator* allocator,void** out,bool closed) noexcept {
    auto* st=g_step1.load();if(!st)return;
    const unsigned path=closed?1:0;
    if(!out||!*out){step1CountCreate(*st,path,gpu_copy::owner::Created::off,true,false,false);return;}
    void* key=*out;bool registered=false;
    {auto& s=state();std::lock_guard lock(s.mutex);
     auto alias=s.listKeys.find(*out);
     if(alias!=s.listKeys.end()) {
         key=alias->second;auto it=s.states.find(alias->second);
         registered=it!=s.states.end()&&it->second.interfaces_observed&&it->second.creation_observed;
     }}
    // Unregistered/failed outputs tombstone that route (D-171) or record a
    // non-DIRECT lifetime; neither can authenticate a Reset.
    const bool direct=type==D3D12_COMMAND_LIST_TYPE_DIRECT;
    const bool ok=SUCCEEDED(hr)&&(registered||!direct);
    const auto outcome=closed?
        st->registry.closed_list_created(ok,step1Route(key),step1Route(device),node,direct):
        st->registry.list_created(ok,step1Route(key),step1Route(device),step1Route(allocator),node,direct);
    step1CountCreate(*st,path,outcome,FAILED(hr),SUCCEEDED(hr)&&direct&&!registered,direct);
}
// Hook-install time, outside every lock, experiment only: is ID3D12Device4 the
// SAME object pointer (hence the same table) as the device being patched? Only
// then does slot 51 exist in this table. Native QI/Release via saved dispatch
// (balanced, bypassing the proxy's own device QI/Release observers). Decided
// once for the owner's device; any other device is never attempted.
Step1List1Hook step1SharesTable(ID3D12Device* device,REFIID extension,const std::atomic<unsigned>& cache) noexcept {
    auto* st=g_step1.load();
    if(!st||!device||device!=st->device)return Step1List1Hook::not_attempted;
    const auto known=static_cast<Step1List1Hook>(cache.load());
    if(known!=Step1List1Hook::not_attempted)return known==Step1List1Hook::installed?known:Step1List1Hook::not_attempted;
    using Q=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,REFIID,void**);
    using R=ULONG(STDMETHODCALLTYPE*)(IUnknown*);
    void* p=nullptr;HRESULT hr=E_FAIL;
    try {hr=reinterpret_cast<Q>(step1Native(device,0))(reinterpret_cast<IUnknown*>(device),extension,&p);}
    catch(...) {return Step1List1Hook::qi_failed;} // a published reference is retained, never guessed
    if(FAILED(hr)||!p)return Step1List1Hook::qi_failed;
    const bool same=p==static_cast<void*>(device)&&vtable(p)==vtable(device);
    try {reinterpret_cast<R>(step1Native(p,2))(static_cast<IUnknown*>(p));}catch(...) {}
    return same?Step1List1Hook::installed:Step1List1Hook::distinct_table;
}
Step1List1Hook step1Device4SharesTable(ID3D12Device* device) noexcept {
    auto* st=g_step1.load();if(!st)return Step1List1Hook::not_attempted;
    return step1SharesTable(device,__uuidof(ID3D12Device4),st->list1Hook);
}
// J-3b4: same rule for CreateCommandQueue1 (ID3D12Device9 slot 75), early owner only.
Step1List1Hook step1Device9SharesTable(ID3D12Device* device) noexcept {
    auto* st=g_step1.load();if(!st||!st->early)return Step1List1Hook::not_attempted;
    return step1SharesTable(device,__uuidof(ID3D12Device9),st->queue1Hook);
}
void step1NoteHook(std::atomic<unsigned>& cache,Step1List1Hook outcome) noexcept {
    unsigned expected=0;
    cache.compare_exchange_strong(expected,static_cast<unsigned>(outcome));
}
void step1NoteList1Hook(Step1List1Hook outcome) noexcept {
    auto* st=g_step1.load();if(!st)return;
    step1NoteHook(st->list1Hook,outcome);
}
void step1ListReleased(void* key) noexcept {
    if(auto* st=g_step1.load())st->registry.released(step1Route(key));
}
// ------------------------------------------------------------- Draw seam
void step1CountBits(std::array<uint32_t,14>& histogram,unsigned bits) noexcept {
    for(unsigned i=0;i<histogram.size();++i)if((bits>>i)&1u)if(histogram[i]<UINT32_MAX)++histogram[i];
}
// Qualified = DrawCPT-matched PSO (creation-time drawcpt_identity_matched, the
// same fact observeDraw uses for L3) on a list whose witness allows insertion
// (authenticated identity, all 29 coverage bits, open generation from an
// observed successful Reset, no predication/query/pass/bundle/indirect
// exclusion), no wrapped native list call in flight, same timeline tokens.
// Four register arguments only (x64: no stack-passed arguments), so the Draw
// hook roots do not grow an outgoing-argument area for this call.
constexpr uint64_t step1Pack(uint32_t low,uint32_t high) noexcept {return uint64_t(low)|(uint64_t(high)<<32);}
__declspec(noinline) void step1Insert(ID3D12GraphicsCommandList* self,uint64_t countInstances,uint64_t startBase,
                                      uint64_t firstIndexed) noexcept {
    auto* st=g_step1.load();if(!st)return;
    const UINT count=UINT(countInstances),instances=UINT(countInstances>>32),start=UINT(startBase);
    const INT base=INT(uint32_t(startBase>>32));const UINT startInstance=UINT(firstIndexed);
    const bool indexed=(firstIndexed>>32)!=0;
    try {
        std::shared_ptr<gpu_copy::ObservationFeed> feed;void* key=nullptr;ID3D12PipelineState* pso=nullptr;uint64_t l3draw=0;
        {auto& s=state();std::lock_guard lock(s.mutex);
         key=listKey_locked(self);auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));
         if(it==s.states.end())return;
         pso=it->second.pso.get();auto p=s.pipelines.find(pso);
         if(p==s.pipelines.end()||!p->second.drawcpt_identity_matched)return;
         feed=it->second.copyObservation;
         if(auto* l=l3_list_locked(it->second))l3draw=l->draws;}
        ID3D12Resource* upload=nullptr;ID3D12Resource* readback=nullptr;
        {std::lock_guard lock(st->mutex);
         if(g_step1Phase.load()!=static_cast<int>(Step1Phase::ready))return;
         if(st->matchedDraws<UINT64_MAX)++st->matchedDraws;
         gpu_copy::Identity id{};uint64_t generation=0;unsigned why=0;
         if(!feed){step1CountBits(st->drawRefusals,1u<<11);return;}
         if(!feed->insertion_candidate(id,generation,why)){step1CountBits(st->drawRefusals,why);return;}
         const auto facts=st->registry.timeline();
         if(!facts.bound||facts.lost||id.device!=facts.device||id.queue!=facts.queue||id.fence!=facts.fence) {
             step1CountBits(st->drawRefusals,1u<<12);return;
         }
         // J-3b7 step 3: observe first (nothing recorded), arm in the owner
         // context, consume the attempt only at a draw matching the observation.
         if(st->step3) {
             st->indexed=indexed;st->count=count;st->instances=instances;st->start=start;st->base=base;st->startInstance=startInstance;
             if(!step3Gate(*st,key))return;
         }
         // ---- the single attempt is consumed from here; never retried ----
         g_step1Phase.store(static_cast<int>(Step1Phase::inserted));
         st->tInsert=GetTickCount64();st->candidate=step1Route(key);st->pso=step1Route(pso);st->feed=feed;
         st->identity=id;st->key=gpu_copy::Key{id.device,id.queue,id.fence,id.list,generation};
         st->indexed=indexed;st->count=count;st->instances=instances;st->start=start;st->base=base;st->startInstance=startInstance;
         st->l3Draw=l3draw;st->matchedAtInsert=st->matchedDraws;st->coverage=feed->snapshot().coverage;
         // J-3b6 step 2: resolve the draw's bound game buffers (hook-state mutex
         // nested under this one, the documented order). No role copyable: refused,
         // nothing armed or recorded.
         const bool two=st->step2,three=st->step3;
         if(two&&!step2Resolve(*st,key))st->armRefusal="no-copyable-role";
         gpu_copy::Admission a{};a.key=st->key;
         a.allocation_bytes=three?st->s3.targets.charged:two?gpu_copy::step2::charged_bytes:gpu_copy::step1::charged_bytes;
         a.history_complete=true;a.retired_generation=false;a.private_fence=true;a.authenticated_queue=true;a.exclusive_signaller=true;
         auto& session=gpu_copy::diagnostic_session();
         using gpu_copy::Step1Owner;
         const gpu_copy::owner::CopyRecipe recipe{gpu_copy::step1::source_token,0,gpu_copy::step1::private_token,0,gpu_copy::step1::copy_bytes};
         const uint64_t privateToken=three?gpu_copy::step3::private_token:two?gpu_copy::step2::private_token:gpu_copy::step1::private_token;
         const uint64_t sourceToken=three?gpu_copy::step3::source_token:two?gpu_copy::step2::source_token:gpu_copy::step1::source_token;
         // Ledger reservation and creation receipt precede the possible recording.
         if(st->armRefusal) {}
         else if(!Step1Owner::arm(session,st->ledger,a))st->armRefusal="ledger-arm-refused";
         else if(!Step1Owner::reserve(session,a))st->armRefusal="ledger-reserve-refused";
         else if(!Step1Owner::created(session,privateToken,sourceToken))st->armRefusal="ledger-receipt-refused";
         else if(!st->timeline.arm(st->key))st->armRefusal="timeline-arm-refused";
         else if(three?!step3PlanValid(*st):two?!step2RecipesValid(*st):
                 !gpu_copy::owner::recipe_valid(recipe,gpu_copy::step1::copy_bytes,gpu_copy::step1::copy_bytes))
             st->armRefusal="recipe-invalid";
         else if(three?!step3PrivateReady(*st):two?!st->readback:(!st->upload||!st->readback))st->armRefusal="private-resources-missing";
         else if(!Step1Owner::possible_recording(session))st->armRefusal="ledger-mark-refused";
         else if(!feed->attach_transaction(session,st->key)) {
             Step1Owner::uncertain(session,"witness-attach-refused");st->armRefusal="witness-attach-refused"; // nothing recorded
         }
         if(st->armRefusal){st->timeline.stop();return;}
         st->recorded=true;upload=st->upload;readback=st->readback;}
        // Possibly recorded from here: the void command cannot be rolled back.
        if(st->step2){step2Record(self,*st,readback);return;} // up to three read-only regions
        if(st->step3){step3Record(self,*st);return;}          // four replays + exact restore
        using CopyFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64);
        reinterpret_cast<CopyFn>(step1Native(self,15))(self,readback,0,upload,0,gpu_copy::step1::copy_bytes);
    } catch(...) {
        // An exception after the phase change leaves the transaction quarantined.
        if(g_step1Phase.load()==static_cast<int>(Step1Phase::inserted)) {
            gpu_copy::Step1Owner::uncertain(gpu_copy::diagnostic_session(),"draw-seam-exception");
            std::lock_guard lock(st->mutex);if(!st->armRefusal)st->armRefusal="draw-seam-exception";
        }
    }
}
// ------------------------------------------------------------- Execute seam
struct Step1Submit {uint64_t ticket=0;bool witnessed=false;};
void step1BeforeExecute(ID3D12CommandQueue* self,UINT count,ID3D12CommandList* const* lists,Step1Submit& out) noexcept {
    auto* st=g_step1.load();if(!st||g_step1Phase.load()!=static_cast<int>(Step1Phase::inserted))return;
    std::lock_guard lock(st->mutex);
    if(!st->recorded)return;
    const unsigned n=count<st->routes.size()?count:static_cast<unsigned>(st->routes.size());
    {auto& s=state();std::lock_guard l(s.mutex);
     for(unsigned i=0;i<n;++i)st->routes[i]=lists&&lists[i]?step1Route(listKey_locked(lists[i])):0;}
    const auto w=st->feed->snapshot();
    gpu_copy::owner::BatchInput in{step1Route(self),count,lists?st->routes.data():nullptr,w.generation,w.closed_generation,true};
    out.ticket=st->timeline.begin(in);
    // The ledger join was registered by copyBeforeExecute just before this.
    out.witnessed=w.pending;
    if(st->executesArmed<UINT64_MAX)++st->executesArmed;
    if(out.ticket) {
        const auto b=st->timeline.batch();
        st->executeQueue=step1Route(self);st->executeCount=b.count;st->executeOrdinal=b.ordinal;
        st->executeOccurrences=b.occurrences;st->submission=b.submission;st->executeUnknown=b.unknown;
        st->timelineTicket=out.ticket;st->witnessTicket=st->feed->ticket();
    }
}
void step1AfterExecute(const Step1Submit& in) noexcept {
    auto* st=g_step1.load();if(!st)return;
    bool ok=false;
    if(in.ticket)ok=st->timeline.signal(in.ticket); // exactly one native Signal, no lock held
    bool joined=false;
    if(in.witnessed)joined=st->feed->signal_result(st->feed->ticket(),ok&&in.ticket);
    std::lock_guard lock(st->mutex);
    if(in.ticket&&!st->signalCalled) {
        st->signalCalled=true;st->signalOk=ok;st->signalHr=st->fence.signalHr;st->witnessJoined=joined;
        st->tSignal=GetTickCount64();
    }
}
// ------------------------------------------------------------- Close / Reset
void step1NoteClose(void* self,HRESULT hr) noexcept {
    auto* st=g_step1.load();if(!st||g_step1Phase.load()!=static_cast<int>(Step1Phase::inserted))return;
    void* key=nullptr;{auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);}
    std::lock_guard lock(st->mutex);
    if(!st->recorded||step1Route(key)!=st->candidate)return;
    if(!st->closes++){st->closeHr=hr;st->closedGeneration=st->feed?st->feed->snapshot().closed_generation:0;}
}
void step1NoteReset(void* self,ID3D12CommandAllocator* allocator,HRESULT hr) noexcept {
    auto* st=g_step1.load();if(!st)return;
    void* key=nullptr;unsigned path=4;
    {auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);
     auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));
     if(it!=s.states.end())path=it->second.registration_path<3?it->second.registration_path:3;}
    const auto phase=g_step1Phase.load();
    if(phase==static_cast<int>(Step1Phase::continuous)){if(SUCCEEDED(hr))step5NoteReset(self);return;}
    if(phase==static_cast<int>(Step1Phase::ready)) {
        const unsigned bits=SUCCEEDED(hr)?st->registry.reset_refusal(step1Route(key),step1Route(allocator)):(1u<<11);
        std::lock_guard lock(st->mutex);
        if(st->resets<UINT64_MAX)++st->resets;
        auto& byPath=bits?st->resetRefusedByPath:st->resetAcceptedByPath;
        if(byPath[path]<UINT32_MAX)++byPath[path];
        if(!bits){if(st->resetsAccepted<UINT64_MAX)++st->resetsAccepted;}else step1CountBits(st->resetRefusals,bits);
        if(bits&16u)st->unseenLists.add(step1Route(key));
        if(bits&64u)st->unseenAllocators.add(step1Route(allocator));
        return;
    }
    if(phase!=static_cast<int>(Step1Phase::inserted))return;
    std::lock_guard lock(st->mutex);
    if(!st->recorded||step1Route(key)!=st->candidate)return;
    if(!st->resetsAfter++){st->resetHr=hr;st->tClosed=GetTickCount64();}
}
// ------------------------------------------------------------- worker
const char* step1QuarantineReason(Step1State& st,const gpu_copy::Snapshot& ledger) noexcept {
    const auto b=st.timeline.batch();
    const auto w=st.feed?st.feed->snapshot():gpu_copy::WitnessSnapshot{};
    if(st.closes&&FAILED(st.closeHr))return "close-failed";
    if(b.duplicate)return "duplicate-in-batch";
    if(b.re_executed)return "re-executed-recording";
    if(b.reentrant)return "concurrent-or-reentrant-execute";
    if(b.cross_queue)return "cross-queue-execute";
    if(b.unauthenticated)return "unauthenticated-queue";
    if(b.unclosed)return "unclosed-generation";
    if(b.oversize)return "oversize-batch";
    if(b.unknown&&!st.timelineTicket)return "unknown-member";
    if(st.signalCalled&&!st.signalOk)return "signal-failed";
    if(st.fence.sawMax)return "device-lost-uint64max";
    if(st.timeline.lost())return "fence-timeline-lost";
    if(st.timedOut||(st.polls>=gpu_copy::owner::Timeline::poll_cap&&!ledger.gpu_complete))return "fence-poll-timeout";
    if(w.ambiguous)return "witness-ambiguity";
    if(ledger.state==gpu_copy::State::quarantined)return "ledger-quarantine";
    return "unclassified";
}
void step1EmitCounters(Step1State& st,const char* prefix) noexcept {
    auto& t=st.text;auto& d=st.drawRefusals;auto& r=st.resetRefusals;
    std::snprintf(t,sizeof t,"%s;matched=%llu;draw-refusals=%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x;resets=%llu;"
        "reset-accepted=%llu;reset-refusals=%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x;registry=%u/%u;tombstones=%u;exhausted=%u;"
        "executes-armed=%llu;polls=%u;ticks=%u,%u,%u",prefix,
        static_cast<unsigned long long>(st.matchedDraws),d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],d[9],d[10],d[11],d[12],
        static_cast<unsigned long long>(st.resets),static_cast<unsigned long long>(st.resetsAccepted),
        r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],r[8],r[9],r[10],r[11],
        st.registry.live_records(),gpu_copy::owner::Registry::capacity,st.registry.tombstones(),st.registry.exhausted(),
        static_cast<unsigned long long>(st.executesArmed),st.polls,st.waitTicks,st.submitTicks,st.closureTicks);
    step1Emit(t);
}
// J-3b3: which creation seams were witnessed, and which Reset routes were not.
// CL=CreateCommandList, CL1=CreateCommandList1, CA=CreateCommandAllocator, each
// seen:recorded:nondirect:unregistered:failed:collision:parent:exhausted (hex).
// cl1-hook: 0 not attempted, 1 installed, 2 distinct Device4 table (untouched),
// 3 Device4 QI failed, 4 patch failed. reset-paths: accepted/refused Resets by
// the list's capture registration path (cl, cl1, at-reset, unobserved, none).
void step1EmitCreation(Step1State& st,const char* prefix) noexcept {
    auto& t=st.text;
    int n=std::snprintf(t,sizeof t,st.early?"%s;witness-start=early-device;device-hook=CL:12,CA:9,CQ:8,CQ1:75;":
        "%s;witness-start=device-discovery;device-hook=CL:12,CA:9;",prefix);
    static constexpr const char* names[]={"CL","CL1","CA"};
    for(unsigned i=0;i<3&&n>0&&size_t(n)<sizeof t;++i) {
        const auto& c=st.creates[i];
        n+=std::snprintf(t+n,sizeof t-n,"%s=%x:%x:%x:%x:%x:%x:%x:%x;",names[i],c.seen,c.recorded,c.nondirect,
            c.unregistered,c.failed,c.collision,c.parent,c.exhausted);
    }
    if(n>0&&size_t(n)<sizeof t) {
        const auto& a=st.resetAcceptedByPath;const auto& r=st.resetRefusedByPath;
        n+=std::snprintf(t+n,sizeof t-n,"cl1-hook=%u;reset-paths=cl:%x/%x,cl1:%x/%x,at-reset:%x/%x,unobserved:%x/%x,none:%x/%x;"
            "unseen-lists=%u%s;unseen-allocators=%u%s",st.list1Hook.load(),a[0],r[0],a[1],r[1],a[2],r[2],a[3],r[3],a[4],r[4],
            st.unseenLists.count,st.unseenLists.overflow?"+":"",st.unseenAllocators.count,st.unseenAllocators.overflow?"+":"");
    }
    // J-3b4 queue witnesses, appended only for the early owner (OFF lines unchanged).
    if(st.early&&n>0&&size_t(n)<sizeof t) {
        const auto& q=st.queueCreates[0];const auto& q1=st.queueCreates[1];
        n+=std::snprintf(t+n,sizeof t-n,";CQ=%x:%x:%x:%x;CQ1=%x:%x:%x:%x;queue-witness=%s",q.seen,q.recorded,q.failed,q.collision,
            q1.seen,q1.recorded,q1.failed,q1.collision,st.queueWitness);
    }
    step1Emit(t);
}
void step1Verdict(Step1State& st,const char* result,const char* reason) noexcept {
    st.tEnd=GetTickCount64();
    const bool completed=std::string_view(result)=="PASS"||st.cleanRetired;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;EXPERIMENT_RESULT=%s;reason=%s;duration-ms=%llu;insert-to-end-ms=%llu;"
        "label=gpu-copy-completed?%s;draw-use-unproven;writer-exclusion-unproven;%s",result,reason,
        static_cast<unsigned long long>(st.tEnd-st.tStart),
        static_cast<unsigned long long>(st.tInsert?st.tEnd-st.tInsert:0),completed?"yes":"no",
        st.step3?(st.s3.continuous?"continuous-producer;per-eye-whole-cockpit-set;cockpit-draws-only;world-sky-hud-not-replayed;"
            "srt-live-at-draw;no-headset-output-proven":st.s3.wholeSet?"per-eye-whole-cockpit-set;cockpit-draws-only;world-sky-hud-not-replayed;game-state-restored-by-tracking;"
            "no-headset-output;srt-live-at-draw":"per-eye-replays-of-one-draw;game-state-restored-by-tracking;no-headset-output;srt-live-at-draw"):
        st.step2?"game-buffers-read-only;cpu-equality-not-coherence;capture=disabled":"no-game-data");
    step1Emit(st.text);
    g_step1Phase.store(static_cast<int>(Step1Phase::done));
}
void step1Pins(Step1State& st) noexcept {
    wchar_t path[1024]{};HMODULE module=nullptr;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&step1Pins),&module)) {
        const auto n=GetModuleFileNameW(module,path,static_cast<DWORD>(std::size(path)));
        if(!n||n>=std::size(path))path[0]=0;
    }
    step1FileSha256(path,st.dllSha);
    std::snprintf(st.cfgSha,sizeof st.cfgSha,"unavailable");
    try {const auto config=diagnostics::DefaultPaths().paths().config;step1FileSha256(config.c_str(),st.cfgSha);}catch(...) {}
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=pins;source=%s;build=%s %s;msc=%d;dll-sha256=%s;config-sha256=%s;"
        "config=log_enabled+diagnostic_ownership_pins+gpu_copy_witness_observation+%s%s",
        ACZ_VR_SOURCE_PIN,__DATE__,__TIME__,_MSC_FULL_VER,st.dllSha,st.cfgSha,
        st.step3?"gpu_copy_step3_experiment+gpu_copy_early_device_hook":
        st.step2?"gpu_copy_step2_experiment+gpu_copy_early_device_hook":"gpu_copy_step1_experiment",
        (st.step2||st.step3)?(st.early?";early-device=notified":";early-device=not-notified"):st.early?"+gpu_copy_early_device_hook":"");
    step1Emit(st.text);
}
bool step1CreateBuffer(Step1State& st,D3D12_HEAP_TYPE heapType,D3D12_RESOURCE_STATES initial,ID3D12Resource*& out,HRESULT& hr,
                       uint64_t width=gpu_copy::step1::copy_bytes) noexcept {
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=heapType;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=width;
    desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=DXGI_FORMAT_UNKNOWN;desc.SampleDesc.Count=1;
    desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_NONE;
    void* value=nullptr;
    // Saved original: the proxy's own buffers never enter the binding/L3 maps.
    hr=reinterpret_cast<Fn>(step1Native(st.device,27))(st.device,&heap,D3D12_HEAP_FLAG_NONE,&desc,initial,nullptr,
        __uuidof(ID3D12Resource),&value);
    out=static_cast<ID3D12Resource*>(value); // failed non-null output retained (never released)
    return SUCCEEDED(hr)&&out;
}
// Owner context only (worker thread, outside loader lock, never a Draw path).
bool step2Prepare(Step1State& st,const char*& why) noexcept;
bool step1Prepare(Step1State& st,const char*& why) noexcept {
    step1Pins(st);
    why=nullptr;
    if(st.step2){if(!step2Prepare(st,why))return false;} // one READBACK buffer, no UPLOAD/pattern
    else if(st.step3)step3Prepare(st); // targets are created after observation (owner context)
    else {
    gpu_copy::step1::pattern(st.pattern.data(),st.pattern.size());
    step1Sha256(st.pattern.data(),st.pattern.size(),st.expectedSha);
    if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,st.upload,st.uploadHr))why="upload-create-failed";
    else {
        using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
        using UnmapFn=void(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*);
        const D3D12_RANGE none{0,0},written{0,static_cast<SIZE_T>(gpu_copy::step1::copy_bytes)};
        void* p=nullptr;
        st.writeMapHr=reinterpret_cast<MapFn>(step1Native(st.upload,8))(st.upload,0,&none,&p);
        if(FAILED(st.writeMapHr)||!p)why="upload-map-failed";
        else {
            std::memcpy(p,st.pattern.data(),st.pattern.size()); // the only CPU write; never mapped again
            reinterpret_cast<UnmapFn>(step1Native(st.upload,9))(st.upload,0,&written);
        }
    }
    if(!why&&!step1CreateBuffer(st,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,st.readback,st.readbackHr))
        why="readback-create-failed";
    st.map.readback=st.readback;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=prepare;pattern=%s;bytes=%llu;expected-sha256=%s;"
        "upload=%llx,heap=UPLOAD,state=GENERIC_READ,width=4096,hr=%08lx,write-map=%08lx,written=0:4096,writes-ceased=1;"
        "readback=%llx,heap=READBACK,state=COPY_DEST,width=4096,hr=%08lx;charged=%llu;transitions=none;game-resources=none",
        gpu_copy::step1::pattern_name,static_cast<unsigned long long>(gpu_copy::step1::copy_bytes),st.expectedSha,
        static_cast<unsigned long long>(step1Route(st.upload)),static_cast<unsigned long>(st.uploadHr),
        static_cast<unsigned long>(st.writeMapHr),static_cast<unsigned long long>(step1Route(st.readback)),
        static_cast<unsigned long>(st.readbackHr),static_cast<unsigned long long>(gpu_copy::step1::charged_bytes));
    step1Emit(st.text);
    if(why)return false;
    }
    const bool bound=st.timeline.create(st.registry,st.fence,step1Route(st.queue));
    const auto facts=st.registry.timeline();
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=timeline;queue=%llx;queue-node-mask=%x;fence=%llx;"
        "create-hr=%08lx;initial=0;bound=%u;lost=%u;tokens=device:%llx,queue:%llx,fence:%llx,node:%llx;"
        "queue-witness=%s;CreateCommandQueue=%s",
        static_cast<unsigned long long>(step1Route(st.queue)),st.queueNodeMask,
        static_cast<unsigned long long>(st.timeline.fence_route()),static_cast<unsigned long>(st.fence.createHr),
        unsigned(bound),unsigned(facts.lost),static_cast<unsigned long long>(facts.device),
        static_cast<unsigned long long>(facts.queue),static_cast<unsigned long long>(facts.fence),
        static_cast<unsigned long long>(facts.node),st.queueWitness,st.early?"hooked":"not-hooked");
    step1Emit(st.text);
    if(!bound){why="private-fence-timeline-unavailable";return false;}
    return true;
}
void step1Finish(Step1State& st,const char* result,const char* reason) noexcept {
    // Quarantine: retain every owner (resources, fence, ledger charge). J-3b6:
    // a step-2 transaction that retired cleanly with some roles refused is
    // REFUSED by label but was never faulted; it is not re-quarantined.
    if(std::string_view(result)!="PASS"&&!st.cleanRetired) {
        gpu_copy::Step1Owner::uncertain(gpu_copy::diagnostic_session(),reason); // static verdict literal
        st.timeline.stop();
        // Diagnostic only (device slot 37, never patched): explains a loss.
        if(st.device)st.removedHr=st.device->GetDeviceRemovedReason();
    }
    const auto ledger=gpu_copy::diagnostic_session().snapshot();
    // J-3b5: the exact first rule on each side. witness-fault is the first
    // witness rule that reached the ledger while the transaction was attached,
    // @g current generation, r the transaction's recorded generation, c the last
    // successfully Closed generation, o whether it was open, n witness faults in
    // all; post-closure counts faults of later generations after detachment.
    // ledger-fault is the ledger's first quarantine rule (the Step-1 verdict
    // reason when the owner itself quarantined).
    const auto w=st.feed?st.feed->snapshot():gpu_copy::WitnessSnapshot{};
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=disposition;resources=%s;fence=retained;ledger-state=%u;"
        "records=%llu;charged=%llu;private-held=%u;source-lease=%u;closed=%u;complete=%u;consumed=%u;retired=%u;"
        "timeline-quarantined=%u;timeline-lost=%u;device-removed-reason=%08lx;witness-fault=%s@g:%llx,r:%llx,c:%llx,o:%u,n:%u;"
        "post-closure=%u;ledger-fault=%s;later-executes=%u;resets-after=%u",
        st.released?"released":"retained-quarantined",
        static_cast<unsigned>(ledger.state),static_cast<unsigned long long>(ledger.records),
        static_cast<unsigned long long>(ledger.charged_bytes),unsigned(ledger.private_held),unsigned(ledger.source_lease),
        unsigned(ledger.recording_closed),unsigned(ledger.gpu_complete),unsigned(ledger.consumed),unsigned(ledger.retired),
        unsigned(st.timeline.quarantined()),unsigned(st.timeline.lost()),static_cast<unsigned long>(st.removedHr),
        w.fault?w.fault:"none",static_cast<unsigned long long>(w.fault_generation),static_cast<unsigned long long>(w.recorded_generation),
        static_cast<unsigned long long>(w.fault_closed_generation),unsigned(w.fault_open),w.faults,w.post_closure_faults,
        ledger.fault?ledger.fault:"none",w.later_executes,st.resetsAfter);
    step1Emit(st.text);
    step1EmitCreation(st,"gpu-copy-step1;event=creation");
    step1EmitCounters(st,"gpu-copy-step1;event=counters");
    if(st.step2)step2EmitResult(st);
    if(st.step3)step3EmitResult(st);
    step1Verdict(st,result,reason);
}
void step1EmitProgress(Step1State& st) noexcept {
    if(!st.emittedInsert) {
        st.emittedInsert=true;
        // J-3b6: the only step-2 difference on this line is the copy field.
        char copy[96];
        if(st.step3)std::snprintf(copy,sizeof copy,"step3-replays:%u,planned-commands:%u",
            gpu_copy::step3::replay_count,st.s3.targets.commands);
        else if(st.step2)std::snprintf(copy,sizeof copy,"game-read-only,copies:%u,dst:readback+ib@0+vb@4096+srt@8192",st.plan.copyable);
        else std::snprintf(copy,sizeof copy,"dst:readback+0,src:upload+0,bytes:4096");
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=insert;list=%llx;tokens=list:%llx,creation:%llx,allocator:%llx,"
            "device:%llx,queue:%llx,fence:%llx,node:%llx;generation=%llx;coverage=%llx;required=%llx;pso=%llx;drawcpt=matched;"
            "matched-draws=%llu;l3-draw=%llu;draw=%u,%u,%u,%u,%d,%u;order=%s;"
            "copy=%s;recorded=%u;arm=%s",
            static_cast<unsigned long long>(st.candidate),static_cast<unsigned long long>(st.identity.list),
            static_cast<unsigned long long>(st.identity.creation),static_cast<unsigned long long>(st.identity.allocator),
            static_cast<unsigned long long>(st.identity.device),static_cast<unsigned long long>(st.identity.queue),
            static_cast<unsigned long long>(st.identity.fence),static_cast<unsigned long long>(st.identity.node),
            static_cast<unsigned long long>(st.key.generation),static_cast<unsigned long long>(st.coverage),
            static_cast<unsigned long long>(gpu_copy::required_coverage),static_cast<unsigned long long>(st.pso),
            static_cast<unsigned long long>(st.matchedAtInsert),static_cast<unsigned long long>(st.l3Draw),
            unsigned(st.indexed),st.count,st.instances,st.start,st.base,st.startInstance,
            st.step3?"private-replays-and-restore-then-original-Draw":"CopyBufferRegion-then-original-Draw",copy,unsigned(st.recorded),
            st.armRefusal?st.armRefusal:"ok");
        step1Emit(st.text);
        if(st.step2)step2EmitRoles(st); // three role lines: copyable or the exact refusal
    }
    if(!st.emittedClose&&st.closes) {
        st.emittedClose=true;
        // J-3b7: the list was Closed on the recording thread after the insertion,
        // so the issued-command count is final here.
        if(st.step3)step3EmitReplay(st); // command order and the captured restore state
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=close;hr=%08lx;closes-after-insert=%u;witness-closed-generation=%llx;"
            "recorded-generation=%llx",static_cast<unsigned long>(st.closeHr),st.closes,
            static_cast<unsigned long long>(st.closedGeneration),static_cast<unsigned long long>(st.key.generation));
        step1Emit(st.text);
    }
    if(!st.emittedExecute&&st.timelineTicket) {
        st.emittedExecute=true;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=execute;queue=%llx;bound-queue=%llx;count=%u;ordinal=%u;"
            "occurrences=%u;submission=%llu;unknown-members=%u;timeline-ticket=%llu;witness-ticket=%llu;executes-armed=%llu;"
            "forwarded=original-count-and-array-once",
            static_cast<unsigned long long>(st.executeQueue),static_cast<unsigned long long>(step1Route(st.queue)),
            st.executeCount,st.executeOrdinal,st.executeOccurrences,static_cast<unsigned long long>(st.submission),
            unsigned(st.executeUnknown),static_cast<unsigned long long>(st.timelineTicket),
            static_cast<unsigned long long>(st.witnessTicket),static_cast<unsigned long long>(st.executesArmed));
        step1Emit(st.text);
    }
    if(!st.emittedSignal&&st.signalCalled) {
        st.emittedSignal=true;
        const auto f=st.registry.timeline();
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=signal;hr=%08lx;ok=%u;fence=%llx;value=1;tuple=device:%llx,"
            "queue:%llx,fence:%llx;witness-join=%u;after=native-ExecuteCommandLists-return",
            static_cast<unsigned long>(st.signalHr),unsigned(st.signalOk),
            static_cast<unsigned long long>(st.timeline.fence_route()),static_cast<unsigned long long>(f.device),
            static_cast<unsigned long long>(f.queue),static_cast<unsigned long long>(f.fence),unsigned(st.witnessJoined));
        step1Emit(st.text);
    }
}
void step1EmitPoll(Step1State& st) noexcept {
    if(st.emittedPoll)return;
    st.emittedPoll=true;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=poll;first=%llu@%u;pending-seen=%u;completed=%llu@%u;polls=%u;"
        "signal-to-complete-ms=%llu;nonblocking=GetCompletedValue;cadence-ms=%u",
        static_cast<unsigned long long>(st.firstValue),st.firstPoll,unsigned(st.sawPending),
        static_cast<unsigned long long>(st.completedValue),st.completedPoll,st.polls,
        static_cast<unsigned long long>(st.tComplete&&st.tSignal?st.tComplete-st.tSignal:0),gpu_copy::step1::tick_ms);
    step1Emit(st.text);
}
// Returns false when the worker should exit (verdict emitted).
bool step1Progress(Step1State& st) noexcept {
    auto& session=gpu_copy::diagnostic_session();
    {std::lock_guard lock(st.mutex);
     step1EmitProgress(st);
     if(st.armRefusal) {
         const bool recorded=st.recorded;
         step1Finish(st,recorded?"FAIL":"REFUSED",st.armRefusal);return false;
     }
     const auto ledger=session.snapshot();
     if(ledger.state==gpu_copy::State::quarantined||st.timeline.quarantined()||st.timeline.lost()) {
         if(st.polls)step1EmitPoll(st);
         step1Finish(st,"FAIL",step1QuarantineReason(st,ledger));return false;
     }
     if(!st.signalCalled) {
         if(++st.submitTicks>=gpu_copy::step1::submit_ticks){step1Finish(st,"FAIL","no-execute-signal-within-bound");return false;}
         return true;
     }
     if(!st.signalOk){step1Finish(st,"FAIL","signal-failed");return false;}
     if(ledger.gpu_complete&&!ledger.recording_closed) {
         if(++st.closureTicks>=gpu_copy::step1::closure_ticks){step1EmitPoll(st);step1Finish(st,"FAIL","reset-closure-timeout");return false;}
         return true;
     }}
    auto ledger=session.snapshot();
    if(!ledger.gpu_complete) {
        const bool done=session.poll(); // -> Step1Ledger::poll -> Timeline::poll (no lock held)
        std::lock_guard lock(st.mutex);
        if(done&&!st.tComplete)st.tComplete=GetTickCount64();
        return true; // classification/closure on the next tick
    }
    if(!ledger.recording_closed)return true;
    // Completed on the same timeline AND the old recording discarded by an
    // observed successful Reset: map, copy out, unmap, then compare.
    const bool read=session.read();
    // J-3b7: pixel statistics, hashes and PNG files are produced here, with NO
    // lock held (game-thread Reset/Close/Execute seams take the step-1 mutex).
    if(st.step3&&read)step3Process(st);
    std::lock_guard lock(st.mutex);
    step1EmitPoll(st);
    if(!st.emittedReset) {
        st.emittedReset=true;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=reset-closure;hr=%08lx;resets-after-insert=%u;closed=%u;"
            "closure=observed-successful-Reset-of-recorded-list",static_cast<unsigned long>(st.resetHr),st.resetsAfter,
            unsigned(ledger.recording_closed));
        step1Emit(st.text);
    }
    if(!read){step1Finish(st,"FAIL","readback-map-failed");return false;}
    if(st.step2) {
        // Per-role bytes/hashes/CPU comparison/index decode. A CPU mismatch is
        // logged, never a failure; an unexpected index range stops (FAIL).
        if(const char* fail=step2Evaluate(st)){step1Finish(st,"FAIL",fail);return false;}
        return true; // retire on the next tick, outside this lock
    }
    if(st.step3){step3Evaluate(st);return true;} // content verdict applied after retirement
    const auto mismatch=gpu_copy::step1::first_mismatch(st.observed.data(),st.pattern.data(),st.observed.size());
    step1Sha256(st.observed.data(),st.observed.size(),st.observedSha);
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=map;subresource=0;read-range=0:4096;hr=%08lx;maps=%u;unmaps=%u;"
        "unmap-written=0:0;equal=%u;first-mismatch=%lld;observed-sha256=%s;expected-sha256=%s",
        static_cast<unsigned long>(st.map.mapHr),st.map.maps,st.map.unmaps,unsigned(mismatch<0),mismatch,st.observedSha,st.expectedSha);
    step1Emit(st.text);
    if(mismatch>=0){step1Finish(st,"FAIL","byte-mismatch");return false;}
    return true; // retire on the next tick, outside this lock
}
bool step1Tick() noexcept {
    auto* st=g_step1.load();if(!st)return false;
    try {
        const auto phase=g_step1Phase.load();
        if(phase==static_cast<int>(Step1Phase::preparing)) {
            const char* why=nullptr;const bool ok=step1Prepare(*st,why);
            std::lock_guard lock(st->mutex);
            if(!ok){step1Finish(*st,"REFUSED",why?why:"prepare-failed");return false;}
            g_step1Phase.store(static_cast<int>(Step1Phase::ready));return true;
        }
        if(phase==static_cast<int>(Step1Phase::ready)) {
            // J-3b7: observation refusal or target preparation (owner context).
            if(st->step3&&!step3ReadyTick(*st))return false;
            bool expire=false;
            {std::lock_guard lock(st->mutex);
             ++st->waitTicks;
             if(st->waitTicks%gpu_copy::step1::status_every==0) {
                 step1EmitCounters(*st,"gpu-copy-step1-status;event=waiting");
                 step1EmitCreation(*st,"gpu-copy-step1-status;event=creation");
             }
             if(st->waitTicks>=gpu_copy::step1::wait_ticks&&g_step1Phase.load()==static_cast<int>(Step1Phase::ready)) {
                 g_step1Phase.store(static_cast<int>(Step1Phase::done));expire=true; // Draw seam re-checks under this mutex
             }}
            if(expire) {
                // Never recorded: no GPU reference exists. Release outside every lock.
                releaseOwned(st->readback);releaseOwned(st->upload);
                if(st->step3)step3ReleasePrivate(*st);
                std::lock_guard lock(st->mutex);
                st->readback=nullptr;st->upload=nullptr;st->map.readback=nullptr;st->released=true;
                std::snprintf(st->text,sizeof st->text,"gpu-copy-step1;event=disposition;resources=released-never-recorded;fence=retained;arm=none");
                step1Emit(st->text);step1EmitCreation(*st,"gpu-copy-step1;event=creation");
                step1EmitCounters(*st,"gpu-copy-step1;event=counters");
                if(st->step3)step3EmitResult(*st);
                step1Verdict(*st,"REFUSED",st->step3?step3ExpiryReason(*st):"no-qualified-matched-draw-within-600s");
                return false;
            }
            return true;
        }
        if(phase==static_cast<int>(Step1Phase::continuous))return step5Tick(*st);
        if(phase==static_cast<int>(Step1Phase::inserted)) {
            auto& session=gpu_copy::diagnostic_session();
            const auto ledger=session.snapshot();
            if(ledger.consumed&&!ledger.retired) {
                const bool retired=session.retire(); // releases upload+readback via Step1Ledger
                std::lock_guard lock(st->mutex);
                if(st->step3) {
                    // Clean transaction: retired, never quarantined. The verdict is
                    // the content result (empty replay / PNG failure is FAIL).
                    st->cleanRetired=retired;
                    const char* fail=!retired?"retire-refused":st->s3.contentFail;
                    const bool partial=st->s3.wholeSet&&(st->s3.setRecorded<st->s3.memberCount);
                    step1Finish(*st,fail?"FAIL":partial?"PARTIAL":"PASS",
                        fail?fail:st->s3.wholeSet?(partial?"set-partial":"whole-set-replayed"):"per-eye-replays-read");
                    return false;
                }
                if(st->step2) {
                    // PASS only when all three roles were copied and read; a clean
                    // transaction with refused roles is REFUSED (never quarantined).
                    st->cleanRetired=retired;
                    const bool all=st->plan.copyable==gpu_copy::step2::role_count;
                    step1Finish(*st,!retired?"FAIL":all?"PASS":"REFUSED",
                        !retired?"retire-refused":all?"all-roles-read":"some-roles-refused");
                    return false;
                }
                step1Finish(*st,retired?"PASS":"FAIL",retired?"pattern-equal":"retire-refused");
                return false;
            }
            return step1Progress(*st);
        }
    } catch(...) {
        std::lock_guard lock(st->mutex);step1Finish(*st,"FAIL","worker-exception");
    }
    return false;
}
DWORD WINAPI step1Thread(void*) {
    while(step1Tick())Sleep(gpu_copy::step1::tick_ms);
    return 0;
}
// After swap-chain discovery accepted the device/queue (not loader lock, not Draw).
void step1Start() noexcept {
    auto* st=g_step1.load();
    if(!st||g_step1Phase.load()!=static_cast<int>(Step1Phase::off))return;
    g_step1Phase.store(static_cast<int>(Step1Phase::preparing));
    // J-3b4: the early owner's device was not the swap-chain device (or no
    // queue joined): never borrow another device's objects. One refusal.
    if(st->early&&(st->discoveryMismatch||!st->queue)) {
        std::lock_guard lock(st->mutex);step1Finish(*st,"REFUSED","early-device-discovery-mismatch");return;
    }
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1NoThread)return;
#endif
    HANDLE thread=CreateThread(nullptr,256*1024,&step1Thread,nullptr,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(thread){CloseHandle(thread);return;}
    std::lock_guard lock(st->mutex);step1Finish(*st,"REFUSED","worker-thread-unavailable");
}
// Explicit teardown veto: never drain while the experiment may use the device.
bool step1Quiescent() noexcept {
    const auto phase=g_step1Phase.load();
    return phase==static_cast<int>(Step1Phase::off)||phase==static_cast<int>(Step1Phase::done);
}
// Configured ON but an opt-in is missing: one explicit refusal, nothing armed.
void step1ReportMissingOptIn() noexcept {
    static std::atomic<bool> reported{false};
    auto* log=diagnostics::sink();
    if(!log||(!log->gpu_copy_step1_experiment()&&!log->gpu_copy_step2_experiment()&&!log->gpu_copy_step3_experiment())||
       step1Enabled()||reported.exchange(true))return;
    if(step1Mode()==Step1Mode::conflict)
        step1Emit(log->gpu_copy_step3_experiment()?
            "gpu-copy-step3;EXPERIMENT_RESULT=REFUSED;reason=multiple-experiments-configured;requires=exactly-one-experiment":
            "gpu-copy-step1;EXPERIMENT_RESULT=REFUSED;reason=step1-and-step2-both-configured;requires=exactly-one-experiment");
    else if(log->gpu_copy_step3_experiment())
        step1Emit("gpu-copy-step3;EXPERIMENT_RESULT=REFUSED;reason=missing-opt-in;requires=log_enabled+diagnostic_ownership_pins+"
            "gpu_copy_witness_observation+gpu_copy_early_device_hook");
    else if(log->gpu_copy_step2_experiment())
        step1Emit("gpu-copy-step2;EXPERIMENT_RESULT=REFUSED;reason=missing-opt-in;requires=log_enabled+diagnostic_ownership_pins+"
            "gpu_copy_witness_observation+gpu_copy_early_device_hook");
    else step1Emit("gpu-copy-step1;EXPERIMENT_RESULT=REFUSED;reason=missing-opt-in;requires=log_enabled+diagnostic_ownership_pins+gpu_copy_witness_observation");
}
