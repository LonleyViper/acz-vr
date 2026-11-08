// Standalone: compile this source only; it includes production interception.
// No DLL, real COM device, GPU, XR runtime, or game is loaded.
#define ACZ_VR_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)

struct Fake { void** table; ULONG refs=1; };
static int drawsA=0,drawsB=0,indexed=0,execA=0,execB=0;
static HRESULT resetResult=S_OK;
static ID3D12CommandAllocator* allocatorSeen=nullptr;
static ID3D12PipelineState* psoSeen=nullptr;
static UINT values[5]{};
static ID3D12CommandList* const* arraySeen=nullptr;
ULONG STDMETHODCALLTYPE releaseList(ID3D12GraphicsCommandList* p) {return --reinterpret_cast<Fake*>(p)->refs;}
ULONG STDMETHODCALLTYPE addPSO(ID3D12PipelineState* p) {return ++reinterpret_cast<Fake*>(p)->refs;}
ULONG STDMETHODCALLTYPE releasePSO(ID3D12PipelineState* p) {return --reinterpret_cast<Fake*>(p)->refs;}
HRESULT STDMETHODCALLTYPE resetList(ID3D12GraphicsCommandList*,ID3D12CommandAllocator* a,ID3D12PipelineState* p) {allocatorSeen=a;psoSeen=p;return resetResult;}
void STDMETHODCALLTYPE setList(ID3D12GraphicsCommandList*,ID3D12PipelineState* p) {psoSeen=p;}
void STDMETHODCALLTYPE drawA(ID3D12GraphicsCommandList*,UINT a,UINT b,UINT c,UINT d) {++drawsA;values[0]=a;values[1]=b;values[2]=c;values[3]=d;}
void STDMETHODCALLTYPE drawB(ID3D12GraphicsCommandList*,UINT a,UINT b,UINT c,UINT d) {++drawsB;values[0]=a;values[1]=b;values[2]=c;values[3]=d;}
void STDMETHODCALLTYPE drawIndexed(ID3D12GraphicsCommandList*,UINT a,UINT b,UINT c,INT d,UINT e) {++indexed;values[0]=a;values[1]=b;values[2]=c;values[3]=UINT(d);values[4]=e;}
void STDMETHODCALLTYPE executeA(ID3D12CommandQueue*,UINT n,ID3D12CommandList* const* p) {++execA;values[0]=n;arraySeen=p;}
void STDMETHODCALLTYPE executeB(ID3D12CommandQueue*,UINT n,ID3D12CommandList* const* p) {++execB;values[0]=n;arraySeen=p;}
HRESULT STDMETHODCALLTYPE queryList(ID3D12CommandList* p,REFIID iid,void** out) {
    if(!out) return E_POINTER;
    *out=nullptr;
    if(iid!=__uuidof(ID3D12GraphicsCommandList)) return E_NOINTERFACE;
    ++reinterpret_cast<Fake*>(p)->refs; *out=p; return S_OK;
}
static Fake* deviceFake=nullptr;
static HRESULT getDeviceResult=S_OK;
static bool supportsQueue=true;
static D3D12_COMMAND_LIST_TYPE queueType=D3D12_COMMAND_LIST_TYPE_DIRECT;
HRESULT STDMETHODCALLTYPE queryQueue(IUnknown* obj,REFIID iid,void** out) {
    *out=nullptr;
    if(!supportsQueue || iid!=__uuidof(ID3D12CommandQueue)) return E_NOINTERFACE;
    ++reinterpret_cast<Fake*>(obj)->refs;*out=obj;return S_OK;
}
ULONG STDMETHODCALLTYPE releaseUnknown(IUnknown* obj){return --reinterpret_cast<Fake*>(obj)->refs;}
HRESULT STDMETHODCALLTYPE getDevice(ID3D12CommandQueue*,REFIID iid,void** out) {
    *out=nullptr;
    if(FAILED(getDeviceResult)) return getDeviceResult;
    CHECK(iid==__uuidof(ID3D12Device));++deviceFake->refs;*out=deviceFake;return S_OK;
}
// MSVC's member-function ABI for struct return differs from a free function.
// Copy a real virtual member entry into the writable synthetic queue table.
struct DescDispatch {
    virtual D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() {
        D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=queueType;return desc;
    }
};
void sentinel() {}
static unsigned protectCalls=0, failProtectCall=0;
static DWORD protectionSeen=0;
BOOL WINAPI fakeProtect(LPVOID,SIZE_T,DWORD requested,DWORD* old) {
    protectionSeen=requested;
    *old=PAGE_READONLY;
    return ++protectCalls!=failProtectCall;
}
std::array<void*,40> listTable(void* draw) {
    std::array<void*,40> t{};
    t.fill(reinterpret_cast<void*>(&sentinel));
    t[0]=reinterpret_cast<void*>(&queryList); t[2]=reinterpret_cast<void*>(&releaseList);
    t[10]=reinterpret_cast<void*>(&resetList);t[11]=reinterpret_cast<void*>(&setList);
    t[12]=draw;t[13]=reinterpret_cast<void*>(&drawIndexed);t[25]=reinterpret_cast<void*>(&setList);
    return t;
}
int main() {
    acz::g_protect=fakeProtect;
    auto ta=listTable(reinterpret_cast<void*>(&drawA));
    auto tb=listTable(reinterpret_cast<void*>(&drawB));
    Fake a{ta.data()}, shared{ta.data()}, b{tb.data()};
    auto* la=reinterpret_cast<ID3D12GraphicsCommandList*>(&a);
    auto* ls=reinterpret_cast<ID3D12GraphicsCommandList*>(&shared);
    auto* lb=reinterpret_cast<ID3D12GraphicsCommandList*>(&b);
    acz::g_pinResult=false;
    CHECK(!acz::hookCommandList(la) && ta[12]==reinterpret_cast<void*>(&drawA));
    acz::g_pinResult=true;
    CHECK(!acz::hookCommandList(nullptr) && !acz::hookCommandQueue(nullptr));
    auto startCalls=protectCalls;
    CHECK(acz::hookCommandList(la));
    auto firstCalls=protectCalls-startCalls;startCalls=protectCalls;
    CHECK(acz::hookCommandList(ls));
    auto sharedCalls=protectCalls-startCalls;startCalls=protectCalls;
    CHECK(acz::hookCommandList(lb));
    std::printf("Foundation protection calls: first=%u shared=%u distinct=%u\n",firstCalls,sharedCalls,protectCalls-startCalls);
    CHECK(firstCalls==12 && sharedCalls==0 && protectCalls-startCalls==12);
    CHECK(acz::state().states.at(ls).table==ta.data());
    startCalls=protectCalls;
    CHECK(acz::hookCommandList(la) && acz::hookCommandList(lb));
    CHECK(protectCalls==startCalls);
    acz::g_pinResult=false;
    CHECK(!acz::hookCommandList(la) && protectCalls==startCalls);
    acz::g_pinResult=true;
    // An installed slot without retained dispatch is not proof of installation.
    auto& installed=acz::state().lists.at(ta.data());
    auto savedRelease=installed.originals[2];installed.originals[2]=nullptr;
    CHECK(!acz::hookCommandList(la) && protectCalls==startCalls);
    installed.originals[2]=savedRelease;
    // A missing lifetime prerequisite must be repaired before trusting draws.
    ta[2]=savedRelease;failProtectCall=protectCalls+2;
    CHECK(!acz::hookCommandList(la) && protectCalls==startCalls+2);
    CHECK(installed.pendingProtection[2]==PAGE_READONLY);
    // Installed draw slots do not bypass a lifetime-slot restoration retry.
    failProtectCall=protectCalls+1;
    CHECK(!acz::hookCommandList(ls) && protectCalls==startCalls+3);
    CHECK(protectionSeen==PAGE_READONLY && installed.pendingProtection[2]==PAGE_READONLY);
    failProtectCall=0;
    CHECK(acz::hookCommandList(la) && protectCalls==startCalls+4);
    CHECK(!installed.pendingProtection[2] && installed.originals[2]==savedRelease);
    startCalls=protectCalls;
    CHECK(acz::hookCommandList(ls) && protectCalls==startCalls);
    ta[10]=reinterpret_cast<void*>(&sentinel);
    CHECK(!acz::hookCommandList(la) && ta[10]==reinterpret_cast<void*>(&sentinel));
    CHECK(installed.originals[10]==reinterpret_cast<void*>(&resetList));
    ta[10]=installed.originals[10];
    CHECK(acz::hookCommandList(la) && protectCalls==startCalls+4);
    for(int i:{21,22,34}) CHECK(ta[i]==reinterpret_cast<void*>(&sentinel));
    auto failedTable=listTable(reinterpret_cast<void*>(&drawB));Fake failed{failedTable.data()};
    auto* failedList=reinterpret_cast<ID3D12GraphicsCommandList*>(&failed);
    failProtectCall=protectCalls+1;
    CHECK(!acz::hookCommandList(failedList));
    CHECK(failedTable[12]==reinterpret_cast<void*>(&drawB));
    failProtectCall=protectCalls+7; // state-binding prerequisite fails
    CHECK(!acz::hookCommandList(failedList));
    CHECK(failedTable[12]==reinterpret_cast<void*>(&drawB) && failedTable[13]==reinterpret_cast<void*>(&drawIndexed));
    failProtectCall=0;CHECK(acz::hookCommandList(failedList));CHECK(failedList->Release()==0);
    std::array<void*,9> pt{};pt[1]=reinterpret_cast<void*>(&addPSO);pt[2]=reinterpret_cast<void*>(&releasePSO);
    Fake p1{pt.data()},p2{pt.data()};
    auto* ps1=reinterpret_cast<ID3D12PipelineState*>(&p1);
    auto* ps2=reinterpret_cast<ID3D12PipelineState*>(&p2);
    acz::markPSOHasGS(ps1,true);
    la->SetPipelineState(ps1); ls->SetPipelineState(ps2);
    acz::DrawCallInfo last{};int callbacks=0;
    acz::setDrawCallback([&](const acz::DrawCallInfo& info){last=info;++callbacks;});
    la->DrawInstanced(17,3,9,4);
    CHECK(drawsA==1 && last.pso==ps1 && last.hasGeometryShader);
    CHECK(values[0]==17 && values[1]==3 && values[2]==9 && values[3]==4);
    CHECK(last.startVertexOrIndex==9 && last.startInstance==4);
    ls->DrawInstanced(2,1,0,0);CHECK(last.pso==ps2 && !last.hasGeometryShader);
    lb->DrawInstanced(5,6,7,8);CHECK(drawsB==1 && last.pso==nullptr);
    la->DrawIndexedInstanced(11,2,7,-13,19);
    CHECK(indexed==1 && last.indexed && last.baseVertex==-13 && last.startInstance==19);
    CHECK(values[0]==11 && values[1]==2 && values[2]==7 && INT(values[3])==-13 && values[4]==19);
    auto* allocator=reinterpret_cast<ID3D12CommandAllocator*>(0x1234);
    resetResult=E_FAIL;CHECK(la->Reset(allocator,ps2)==E_FAIL);
    la->DrawInstanced(1,1,0,0);CHECK(last.pso==ps1 && allocatorSeen==allocator && psoSeen==ps2);
    resetResult=S_OK;CHECK(la->Reset(allocator,ps2)==S_OK);
    // This foundation-only fake refuses canonical QI. Certification cannot
    // retain per-object state without published lifetime/alias routing.
    la->DrawInstanced(1,1,0,0);CHECK(last.pso==nullptr);
    CHECK(!acz::state().states.count(la));
    la->ClearState(nullptr);la->DrawInstanced(1,1,0,0);CHECK(last.pso==nullptr);
    CHECK(la->Reset(allocator,nullptr)==S_OK);
    ls->DrawInstanced(1,1,0,0);CHECK(last.pso==ps2); // reset did not affect shared-table peer
    int before=callbacks;int beforeDraws=drawsA;
    acz::setDrawCallback([&](const acz::DrawCallInfo&){++callbacks;la->DrawInstanced(1,1,0,0);});
    la->DrawInstanced(1,1,0,0);
    CHECK(callbacks==before+1 && drawsA==beforeDraws+2);
    acz::setDrawCallback([](const acz::DrawCallInfo&){throw std::runtime_error("fake");});
    beforeDraws=drawsA;la->DrawInstanced(1,1,0,0);CHECK(drawsA==beforeDraws+1);
    acz::setDrawCallback({});
    CHECK(ls->Release()==0 && p2.refs==1);
    CHECK(acz::state().states.find(ls)==acz::state().states.end());
    // Address reuse must not inherit recorded PSO.
    shared.refs=1;CHECK(acz::hookCommandList(ls));
    CHECK(!acz::state().states[ls].pso);
    // Two distinct and shared queue tables dispatch to their own originals.
    std::array<void*,26> qa{},qb{};qa[10]=reinterpret_cast<void*>(&executeA);qb[10]=reinterpret_cast<void*>(&executeB);
    Fake q1{qa.data()},qShared{qa.data()},q2{qb.data()};
    auto* queue1=reinterpret_cast<ID3D12CommandQueue*>(&q1);
    auto* queueShared=reinterpret_cast<ID3D12CommandQueue*>(&qShared);
    auto* queue2=reinterpret_cast<ID3D12CommandQueue*>(&q2);
    startCalls=protectCalls;
    CHECK(acz::hookCommandQueue(queue1) && protectCalls==startCalls+2);
    CHECK(acz::hookCommandQueue(queueShared) && protectCalls==startCalls+2);
    CHECK(acz::hookCommandQueue(queue2) && protectCalls==startCalls+4);
    startCalls=protectCalls;
    CHECK(acz::hookCommandQueue(queue1) && acz::hookCommandQueue(queue2) && protectCalls==startCalls);
    Fake discovered{ta.data()};auto discoveredList=reinterpret_cast<ID3D12GraphicsCommandList*>(&discovered);
    CHECK(!acz::state().states.count(discoveredList));
    ID3D12CommandList* lists[]={discoveredList,lb};
    int executeCallbacks=0;
    acz::setExecuteCallback([&](ID3D12CommandQueue* q,UINT n,ID3D12CommandList* const* p){
        ++executeCallbacks;CHECK(q==queue1 && n==2 && p==lists);q->ExecuteCommandLists(0,nullptr);
    });
    queue1->ExecuteCommandLists(2,lists);
    CHECK(executeCallbacks==1 && execA==2 && values[0]==2 && arraySeen==lists);
    CHECK(protectCalls==startCalls && discovered.refs==1);
    CHECK(acz::state().states.at(discoveredList).table==ta.data());
    CHECK(discoveredList->Release()==0 && !acz::state().states.count(discoveredList));
    acz::setExecuteCallback({});
    ID3D12CommandList* liveLists[]={la,lb};
    queueShared->ExecuteCommandLists(0,nullptr);queue2->ExecuteCommandLists(2,liveLists);
    CHECK(execA==3 && execB==1 && values[0]==2 && arraySeen==liveLists);
    // Initial failure leaves slots unchanged; retry succeeds. Restore failure
    // reports failure, but already-installed hook still calls its saved original.
    std::array<void*,26> qc{};qc[10]=reinterpret_cast<void*>(&executeA);Fake q3{qc.data()};
    auto* queue3=reinterpret_cast<ID3D12CommandQueue*>(&q3);
    failProtectCall=protectCalls+1;CHECK(!acz::hookCommandQueue(queue3));CHECK(qc[10]==reinterpret_cast<void*>(&executeA));
    failProtectCall=protectCalls+2;CHECK(!acz::hookCommandQueue(queue3));
    queue3->ExecuteCommandLists(0,nullptr);CHECK(execA==4);
    failProtectCall=protectCalls+1;CHECK(!acz::hookCommandQueue(queue3));
    CHECK(protectionSeen==PAGE_READONLY); // retries restore the saved protection
    startCalls=protectCalls;
    failProtectCall=0;CHECK(acz::hookCommandQueue(queue3));
    CHECK(protectCalls==startCalls+1 && !acz::state().queues.at(qc.data()).pendingProtection[10]);
    CHECK(acz::state().queues.at(qc.data()).originals[10]==reinterpret_cast<void*>(&executeA));
    startCalls=protectCalls;
    CHECK(acz::hookCommandQueue(queue3) && protectCalls==startCalls);
    // Competing patch is never overwritten.
    qc[10]=reinterpret_cast<void*>(&executeB);CHECK(!acz::hookCommandQueue(queue3));CHECK(qc[10]==reinterpret_cast<void*>(&executeB));
    acz::forgetPSO(ps1);CHECK(acz::state().gs.find(ps1)==acz::state().gs.end());
    CHECK(la->Release()==0);CHECK(lb->Release()==0);CHECK(ls->Release()==0);
    CHECK(p1.refs==1 && p2.refs==1);
    // Device capture owns exactly one queue/device reference, including errors.
    std::array<void*,26> captureTable{};std::array<void*,3> deviceTable{};
    captureTable[0]=reinterpret_cast<void*>(&queryQueue);
    captureTable[2]=reinterpret_cast<void*>(&releaseUnknown);
    captureTable[7]=reinterpret_cast<void*>(&getDevice);
    captureTable[10]=reinterpret_cast<void*>(&executeA);
    DescDispatch descDispatch;
    captureTable[18]=(*reinterpret_cast<void***>(&descDispatch))[0];
    deviceTable[2]=reinterpret_cast<void*>(&releaseUnknown);
    Fake capturedQueue{captureTable.data()},capturedDevice{deviceTable.data()};
    deviceFake=&capturedDevice;
    auto* input=reinterpret_cast<IUnknown*>(&capturedQueue);
    supportsQueue=false;acz::captureDeviceAndQueue(input);CHECK(capturedQueue.refs==1 && capturedDevice.refs==1);
    supportsQueue=true;queueType=D3D12_COMMAND_LIST_TYPE_COMPUTE;
    acz::captureDeviceAndQueue(input);CHECK(capturedQueue.refs==1 && capturedDevice.refs==1);
    queueType=D3D12_COMMAND_LIST_TYPE_DIRECT;getDeviceResult=E_FAIL;
    acz::captureDeviceAndQueue(input);CHECK(capturedQueue.refs==1 && capturedDevice.refs==1);
    getDeviceResult=S_OK;failProtectCall=protectCalls+1;
    acz::captureDeviceAndQueue(input);CHECK(capturedQueue.refs==1 && capturedDevice.refs==1);
    failProtectCall=0;int deviceCallbacks=0;
    acz::setDeviceReadyCallback([&](ID3D12Device* d,ID3D12CommandQueue* q){
        ++deviceCallbacks;CHECK(d==reinterpret_cast<ID3D12Device*>(&capturedDevice));
        CHECK(q==reinterpret_cast<ID3D12CommandQueue*>(&capturedQueue));
    });
    acz::captureDeviceAndQueue(input);
    CHECK(deviceCallbacks==1 && capturedQueue.refs==2 && capturedDevice.refs==2);
    acz::captureDeviceAndQueue(input);
    CHECK(deviceCallbacks==1 && capturedQueue.refs==2 && capturedDevice.refs==2);
    CHECK(acz::getHookedDevice()==reinterpret_cast<ID3D12Device*>(&capturedDevice));
    CHECK(acz::getHookedQueue()==reinterpret_cast<ID3D12CommandQueue*>(&capturedQueue));
    // Undo process-lifetime retention only in this fake executable.
    acz::state().device->Release();acz::state().queue->Release();
    acz::state().device=nullptr;acz::state().queue=nullptr;
    acz::setDeviceReadyCallback({});
    CHECK(capturedQueue.refs==1 && capturedDevice.refs==1);
    struct Frame {bool shouldRender;};
    struct Session {
        bool ready=false,eventsOk=true,start=true;int polls=0,ends=0,runningChecks=0;
        bool pollEvents(){++polls;ready=start;return eventsOk;}
        bool isSessionRunning(){++runningChecks;return ready;}
        bool endFrame(const Frame& f){++ends;CHECK(!f.shouldRender);return true;}
    } session;
    CHECK(!acz::pollReadySession(session,false) && session.polls==0);
    CHECK(acz::pollReadySession(session,true) && session.polls==1);
    session.start=false;CHECK(!acz::pollReadySession(session,true));
    CHECK(session.runningChecks==2);
    session.eventsOk=false;CHECK(!acz::pollReadySession(session,true));
    CHECK(session.runningChecks==2);
    CHECK(acz::endUnrenderedFrame(session,Frame{true}) && session.ends==1);
    std::puts("D3D12 fake dispatch/state/patch and XR policy regressions passed.");
}
