// SYNTHETIC ONLY (GPU-copy step 3b part 2). The REAL production hook wrappers
// (captureDeviceAndQueue, hookCaptureDevice, createList, the new allocator
// witness, Reset/Close/Draw/ExecuteCommandLists, observation feeds), the REAL
// owner registry/timeline, witness and diagnostic ledger session, driven with
// fake D3D12 objects whose vtables are writable arrays. A tiny "GPU" simulator
// applies recorded CopyBufferRegion commands and queued Signals only when the
// fixture calls advance(). No device, queue, fence, copy, Map, game or XR exists.
// Every scenario runs in its own process (CTest argument) so the one-attempt
// process singletons start fresh.
#define ACZ_VR_OFFLINE_TEST
#include "../../src/acz-vr/d3d12_hook.cpp"
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <set>
#include <atomic>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::fflush(stderr);std::abort();}}while(0)
using namespace acz;
static size_t allocations=0;
static bool traceAllocations=false;
static thread_local bool uncountedThread=false; // the step5-threads consumer (its own allocations are not the Draw path)
void* operator new(size_t n) {
    if(uncountedThread)return std::malloc(n?n:1);
    ++allocations;if(traceAllocations)std::fprintf(stderr,"Draw-path allocation of %zu bytes\n",n);
    if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new(size_t n,const std::nothrow_t&) noexcept {++allocations;return std::malloc(n?n:1);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}

using Table=std::array<void*,96>;
enum Kind {k_device,k_queue,k_list,k_allocator,k_resource,k_fence,k_pso};
struct Object {void** table=nullptr;Kind kind=k_device;int list=0;int alias=0;int index=0;ULONG refs=1;};
struct CopyCmd {Object* dst;UINT64 dstOffset;Object* src;UINT64 srcOffset;UINT64 bytes;};
// J-3b7: every step-3-relevant native list call, in recording order (POD, so
// recording on the Draw path never allocates). `bound*` is the fake native
// state at the time of a draw.
struct Op {
    enum T {om,clear_rtv,clear_dsv,srv,draw,barrier,copy_tex,pred,vb} t=om;
    UINT count=0;BOOL single=FALSE;std::array<SIZE_T,8> rtv{};bool dsvSet=false;SIZE_T dsv=0;
    UINT param=0;UINT64 va=0;float depth=0;UINT clearFlags=0;
    Object* consumedRtv=nullptr; // native OM consumes descriptor contents while recording
    Object* res=nullptr;UINT before=0,after=0;
    Object* dst=nullptr;UINT64 offset=0;UINT w=0,h=0,pitch=0,format=0;
    UINT dc=0,di=0,ds=0,dsi=0;INT db=0;bool indexed=false;
    bool box=false;UINT bl=0,bt=0,br=0,bb=0; // J-3b7b source box of a texture copy
    bool subres=false; // J-3b9 texture -> texture (subresource 0 -> 0) copy
    bool snapped=false;std::array<uint8_t,416> snap{}; // J-3b9: private SrtData bytes at submission (upload-retirement check)
    SIZE_T boundRtv=0;UINT boundCount=0;SIZE_T boundDsv=0;bool boundDsvSet=false;UINT64 boundT10=0,boundT5=0;
    UINT64 predOff=0;UINT predOp=0; // J-effects-layer: SetPredication (res = the predicate buffer, nullptr = off)
    void* boundPso=nullptr;         // J-effects-dual: the PSO bound at an indexed draw (the fake native SetPipelineState records it)
    UINT64 boundVb=0;UINT vbBytes=0,vbStride=0,vbStart=0,vbCount=0; // EFFECTS-OUTSIDE (F) fxmem:6: IASetVertexBuffers slot 0 (vb op) / bound at an indexed draw
};
struct ListSim {
    std::array<Object,11> aliases{};std::array<Table,11> tables{};
    std::vector<CopyCmd> recording;std::vector<std::string> events;
    std::vector<Op> ops;
    void* curPso=nullptr;unsigned psoSets=0; // J-effects-dual: the PSO the fake native list currently has bound, and the number of native SetPipelineState calls
    unsigned draws=0,copies=0,resets=0,closes=0;
    // Fake native list state (what the "GPU" sees).
    UINT omCount=0;BOOL omSingle=FALSE;std::array<SIZE_T,8> rtv{};bool dsvSet=false;SIZE_T dsv=0;std::array<UINT64,8> roots{};
    UINT viewports=0,scissors=0;
    UINT64 vb=0; // EFFECTS-OUTSIDE (F): the slot-0 vertex buffer the fake native list has bound
    // EYE-RING: every native RSSetViewports / RSSetScissorRects since the last Reset (values + the native draw count at the call). Fixed storage: the
    // producer's calls happen on the Draw path, which is allocation-checked.
    struct Rs {bool scissor=false;float x=0,y=0,w=0,h=0;LONG l=0,t=0,r=0,b=0;unsigned draws=0;};
    std::array<Rs,1024> rs{};unsigned rsCount=0;
    D3D12_VIEWPORT curVp{};D3D12_RECT curSc{}; // the fake native list's current viewport 0 / scissor 0
};
struct Resource {Object object;std::array<uint8_t,16384> memory{};unsigned maps=0,unmaps=0; // J-3b6: 12 KiB step-2 readback
    D3D12_HEAP_TYPE heap=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_STATES initial=D3D12_RESOURCE_STATE_COMMON;UINT64 width=0;
    UINT64 lastReadBegin=1,lastReadEnd=0,lastWrittenBegin=1,lastWrittenEnd=0;
    // J-3b7: textures and large buffers use `big`; `state` follows recorded barriers.
    std::vector<uint8_t> big;D3D12_RESOURCE_DIMENSION dim=D3D12_RESOURCE_DIMENSION_BUFFER;UINT height=1;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE;D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON;bool clear=false;UINT arraySize=1;
    uint8_t* data(){return big.empty()?memory.data():big.data();}
    size_t size()const{return big.empty()?memory.size():big.size();}};
struct World {
    Table worldTextureTable{},deviceTable{},queueTable{},allocatorTable{},resourceTable{},fenceTable{},psoTable{},otherQueueTable{};
    Object device,queue,otherQueue,fence,pso,unmatchedPso,otherDevice;
    std::array<Object,4> allocators{};unsigned allocatorsCreated=0;
    std::array<ListSim,2> lists{};
    std::array<Resource,48> resources{};unsigned resourcesCreated=0; // 22/23 are game textures; J-hud-capture adds 6 private ones (fxmem:6 fixture: 47)
    std::vector<Op> gpuOps;unsigned inconsistent=0,rasterised=0,worldCopies=0,uploadClobbered=0; // J-3b7 fake GPU
    unsigned predDrawn=0,predSkipped=0; // J-effects-layer: predicated draws executed / skipped by the fake GPU
    bool gamePredicate=false;unsigned gamePredDraws=0;Object* gamePredObj=nullptr; // J-effects-smoke-fix: the GAME has a predicate active (its draws are not the proxy's)
    Object fence2; // J-3b9: a second private fence object (fence-epoch test)
    Table heapTable{};std::array<Object,40> heaps{};unsigned heapsCreated=0;std::array<D3D12_DESCRIPTOR_HEAP_TYPE,40> heapTypes{}; // fxmem:6 fixture: 35
    std::array<std::pair<SIZE_T,Object*>,4096> views{};unsigned viewCount=0;
    std::array<std::pair<SIZE_T,SIZE_T>,4096> origins{};unsigned originCount=0; // J-3b9c: private descriptor copy -> source handle
    unsigned descriptorCopies=0;
    HRESULT createResourceHr=S_OK,signalHr=S_OK,closeHr=S_OK,resetHr=S_OK,removedHr=S_OK;
    uint64_t fenceValue=0,pendingSignal=0;bool signalQueued=false;unsigned signals=0,fenceCreates=0;
    std::vector<CopyCmd> gpu;bool corrupt=false;
    unsigned executes=0;UINT executeCount=0;ID3D12CommandList* const* executeArray=nullptr;
    std::function<void()> inPredication,duringExecute;
    std::vector<std::string> lines;
};
static World* w=nullptr;
static void STDMETHODCALLTYPE unused() {CHECK(false);}
// List aliases share one count (one COM object); every other fake has its own.
static Object* counted(IUnknown* self);
static ULONG STDMETHODCALLTYPE addRef(IUnknown* self){return ++counted(self)->refs;}
static std::function<void()> duringFinalResourceRelease;
static Object* creationTexture=nullptr;
static unsigned textureCreationCalls=0;
static ULONG STDMETHODCALLTYPE release(IUnknown* self){auto* o=counted(self);CHECK(o->refs);const auto refs=--o->refs;
    if(!refs&&o->kind==k_resource&&duringFinalResourceRelease)duringFinalResourceRelease();return refs;}
static Object* obj(void* p){return reinterpret_cast<Object*>(p);}
static Object* counted(IUnknown* self){auto* o=obj(self);return o->kind==k_list?&w->lists[o->list].aliases[0]:o;}
static ID3D12GraphicsCommandList* listAlias(int list,int alias=0){return reinterpret_cast<ID3D12GraphicsCommandList*>(&w->lists[list].aliases[alias]);}
static Object* viewOf(SIZE_T h);
static std::function<void()> duringClearRtv;
// ----- list
static HRESULT STDMETHODCALLTYPE listQuery(ID3D12GraphicsCommandList* self,REFIID iid,void** out) {
    const int l=obj(self)->list;*out=nullptr;
    if(iid==__uuidof(IUnknown)||iid==__uuidof(ID3D12CommandList)||iid==__uuidof(ID3D12DeviceChild)||iid==__uuidof(ID3D12Object))*out=listAlias(l);
    for(unsigned i=0;i<11;++i)if(iid==acz::listIids[i])*out=listAlias(l,i);
    if(!*out)return E_NOINTERFACE;++w->lists[l].aliases[0].refs;return S_OK;
}
static HRESULT STDMETHODCALLTYPE listDevice(ID3D12GraphicsCommandList*,REFIID,void** out){*out=nullptr;return E_NOINTERFACE;}
static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE listType(ID3D12GraphicsCommandList*){return D3D12_COMMAND_LIST_TYPE_DIRECT;}
static HRESULT STDMETHODCALLTYPE listReset(ID3D12GraphicsCommandList* self,ID3D12CommandAllocator*,ID3D12PipelineState*) {
    auto& l=w->lists[obj(self)->list];++l.resets;
    if(SUCCEEDED(w->resetHr)){l.recording.clear();l.ops.clear();l.events.push_back("reset");
        l.omCount=0;l.omSingle=FALSE;l.rtv={};l.dsvSet=false;l.dsv=0;l.roots={};l.viewports=l.scissors=0;l.vb=0;l.rsCount=0;l.curVp={};l.curSc={};}
    return w->resetHr;
}
static HRESULT STDMETHODCALLTYPE listClose(ID3D12GraphicsCommandList* self){++w->lists[obj(self)->list].closes;return w->closeHr;}
static void STDMETHODCALLTYPE listClear(ID3D12GraphicsCommandList*,ID3D12PipelineState*){}
static void STDMETHODCALLTYPE listSetPso(ID3D12GraphicsCommandList* self,ID3D12PipelineState* pso){auto& l=w->lists[obj(self)->list];l.curPso=pso;++l.psoSets;}
static void STDMETHODCALLTYPE listTopology(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY){}
static void STDMETHODCALLTYPE listDraw(ID3D12GraphicsCommandList* self,UINT count,UINT instances,UINT start,UINT first) {
    CHECK((count==3||count==4||count==6)&&instances==1&&start==2&&first==0);auto& l=w->lists[obj(self)->list];++l.draws;l.events.push_back("draw"); // 4 = a quad (J-hud-radar fixtures); 6 = (K) a non-indexed 6-vertex sprite strip
}
static void STDMETHODCALLTYPE listDrawIndexed(ID3D12GraphicsCommandList* self,UINT count,UINT instances,UINT start,INT base,UINT first) {
    CHECK(count>=3&&count<=9&&instances==1&&start==0&&base==0&&first==0);auto& l=w->lists[obj(self)->list];++l.draws;l.events.push_back("draw-indexed");
    Op o;o.t=Op::draw;o.indexed=true;o.dc=count;o.di=instances;o.ds=start;o.db=base;o.dsi=first;
    o.boundRtv=l.omCount?l.rtv[0]:0;o.boundCount=l.omCount;o.boundDsvSet=l.dsvSet;o.boundDsv=l.dsv;o.boundT10=l.roots[3];o.boundT5=l.roots[5];o.boundPso=l.curPso;
    o.boundVb=l.vb;l.ops.push_back(o);
}
// EFFECTS-OUTSIDE (F) fxmem:6: the fake native IASetVertexBuffers (slot 0 recorded as an op; the sprite replay binds the eye copies, then the game's view).
static void STDMETHODCALLTYPE listVb(ID3D12GraphicsCommandList* self,UINT start,UINT n,const D3D12_VERTEX_BUFFER_VIEW* v) {
    auto& l=w->lists[obj(self)->list];l.events.push_back("vb");
    Op o;o.t=Op::vb;o.vbStart=start;o.vbCount=n;if(v&&n){o.va=v[0].BufferLocation;o.vbBytes=v[0].SizeInBytes;o.vbStride=v[0].StrideInBytes;}
    l.ops.push_back(o);if(start==0&&n>=1)l.vb=v?v[0].BufferLocation:0;
}
// J-3b7 fake natives (state recorded as the runtime would hold it).
static void STDMETHODCALLTYPE listOm(ID3D12GraphicsCommandList* self,UINT n,const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,BOOL single,
                                     const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    auto& l=w->lists[obj(self)->list];l.events.push_back("om");
    Op o;o.t=Op::om;o.count=rtvs?n:0;o.single=single;for(UINT i=0;rtvs&&i<n&&i<(single?1u:n);++i)o.rtv[i]=rtvs[i].ptr;
    o.dsvSet=dsv!=nullptr;o.dsv=dsv?dsv->ptr:0;o.consumedRtv=o.count?viewOf(o.rtv[0]):nullptr;l.ops.push_back(o);
    l.omCount=o.count;l.omSingle=single;l.rtv=o.rtv;l.dsvSet=o.dsvSet;l.dsv=o.dsv;
}
static void STDMETHODCALLTYPE listViewports(ID3D12GraphicsCommandList* self,UINT n,const D3D12_VIEWPORT* v){
    auto& l=w->lists[obj(self)->list];l.viewports=n;if(v&&n)l.curVp=v[0];
    if(v&&n&&l.rsCount<l.rs.size()){auto& r=l.rs[l.rsCount++];r=ListSim::Rs{};r.x=v[0].TopLeftX;r.y=v[0].TopLeftY;r.w=v[0].Width;r.h=v[0].Height;r.draws=l.draws;}}
static void STDMETHODCALLTYPE listScissors(ID3D12GraphicsCommandList* self,UINT n,const D3D12_RECT* s){
    auto& l=w->lists[obj(self)->list];l.scissors=n;if(s&&n)l.curSc=s[0];
    if(s&&n&&l.rsCount<l.rs.size()){auto& r=l.rs[l.rsCount++];r=ListSim::Rs{};r.scissor=true;r.l=s[0].left;r.t=s[0].top;r.r=s[0].right;r.b=s[0].bottom;r.draws=l.draws;}}
static void STDMETHODCALLTYPE listClearRtv(ID3D12GraphicsCommandList* self,D3D12_CPU_DESCRIPTOR_HANDLE h,const FLOAT* c,UINT rects,const D3D12_RECT*) {
    if(duringClearRtv)duringClearRtv();
    CHECK(c&&c[0]==0&&c[1]==0&&c[2]==0&&c[3]==0&&rects==0);
    auto& l=w->lists[obj(self)->list];l.events.push_back("clear-rtv");Op o;o.t=Op::clear_rtv;o.rtv[0]=h.ptr;l.ops.push_back(o);
}
static void STDMETHODCALLTYPE listClearDsv(ID3D12GraphicsCommandList* self,D3D12_CPU_DESCRIPTOR_HANDLE h,D3D12_CLEAR_FLAGS f,FLOAT d,UINT8 st,UINT rects,const D3D12_RECT*) {
    CHECK(st==0&&rects==0);
    auto& l=w->lists[obj(self)->list];l.events.push_back("clear-dsv");Op o;o.t=Op::clear_dsv;o.dsv=h.ptr;o.depth=d;o.clearFlags=f;l.ops.push_back(o);
}
static void STDMETHODCALLTYPE listSrv(ID3D12GraphicsCommandList* self,UINT param,D3D12_GPU_VIRTUAL_ADDRESS va) {
    auto& l=w->lists[obj(self)->list];l.events.push_back("srv");Op o;o.t=Op::srv;o.param=param;o.va=va;l.ops.push_back(o);
    if(param<8)l.roots[param]=va;
}
static void STDMETHODCALLTYPE listBarrier(ID3D12GraphicsCommandList* self,UINT n,const D3D12_RESOURCE_BARRIER* b) {
    CHECK(n>=1&&b);auto& l=w->lists[obj(self)->list];
    for(UINT i=0;i<n;++i) {
        l.events.push_back("barrier");
        // The fake GPU applies only complete transitions of subresource 0/ALL (J-3b9 game barriers may be split/aliasing).
        if(b[i].Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION||(b[i].Flags&(D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY|D3D12_RESOURCE_BARRIER_FLAG_END_ONLY))||
           (b[i].Transition.Subresource!=0&&b[i].Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))continue;
        Op o;o.t=Op::barrier;o.res=obj(b[i].Transition.pResource);o.before=b[i].Transition.StateBefore;o.after=b[i].Transition.StateAfter;l.ops.push_back(o);
    }
}
static void STDMETHODCALLTYPE listCopyTex(ID3D12GraphicsCommandList* self,const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,
                                          const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box) {
    if(dst&&src&&dst->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) { // J-3b9 world snapshot
        CHECK(!x&&!y&&!z&&!box&&src->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&src->SubresourceIndex==0&&dst->SubresourceIndex==0);
        auto& l=w->lists[obj(self)->list];l.events.push_back("copy-tex-sub");
        Op o;o.t=Op::copy_tex;o.subres=true;o.res=obj(src->pResource);o.dst=obj(dst->pResource);l.ops.push_back(o);return;
    }
    CHECK(dst&&src&&!x&&!y&&!z&&dst->Type==D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT&&src->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX);
    CHECK(src->SubresourceIndex==0&&dst->PlacedFootprint.Footprint.Depth==1);
    auto& l=w->lists[obj(self)->list];l.events.push_back("copy-tex");
    Op o;o.t=Op::copy_tex;o.res=obj(src->pResource);o.dst=obj(dst->pResource);o.offset=dst->PlacedFootprint.Offset;
    o.w=dst->PlacedFootprint.Footprint.Width;o.h=dst->PlacedFootprint.Footprint.Height;o.pitch=dst->PlacedFootprint.Footprint.RowPitch;
    o.format=dst->PlacedFootprint.Footprint.Format;
    if(box){CHECK(box->front==0&&box->back==1);o.box=true;o.bl=box->left;o.bt=box->top;o.br=box->right;o.bb=box->bottom;}
    l.ops.push_back(o);
}
static void STDMETHODCALLTYPE listCopy(ID3D12GraphicsCommandList* self,ID3D12Resource* dst,UINT64 dstOffset,ID3D12Resource* src,UINT64 srcOffset,UINT64 bytes) {
    auto& l=w->lists[obj(self)->list];++l.copies;l.events.push_back("copy");
    l.recording.push_back({obj(dst),dstOffset,obj(src),srcOffset,bytes});
}
static void STDMETHODCALLTYPE listPredication(ID3D12GraphicsCommandList* self,ID3D12Resource* r,UINT64 off,D3D12_PREDICATION_OP op) {
    if(w->inPredication)w->inPredication();
    auto& l=w->lists[obj(self)->list];l.events.push_back("predication");
    Op o;o.t=Op::pred;o.res=r?obj(r):nullptr;o.predOff=off;o.predOp=UINT(op);l.ops.push_back(o);
}
static void STDMETHODCALLTYPE listQueryStub(ID3D12GraphicsCommandList*,ID3D12QueryHeap*,D3D12_QUERY_TYPE,UINT){}
// ----- queue
static HRESULT STDMETHODCALLTYPE queueQuery(ID3D12CommandQueue* self,REFIID iid,void** out) {
    *out=nullptr;if(iid!=__uuidof(ID3D12CommandQueue)&&iid!=__uuidof(IUnknown))return E_NOINTERFACE;
    *out=self;++obj(self)->refs;return S_OK;
}
static HRESULT STDMETHODCALLTYPE queueDevice(ID3D12CommandQueue*,REFIID iid,void** out) {
    CHECK(iid==__uuidof(ID3D12Device));*out=&w->device;++w->device.refs;return S_OK;
}
static D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE queueDesc(ID3D12CommandQueue*,D3D12_COMMAND_QUEUE_DESC* out) {
    *out=D3D12_COMMAND_QUEUE_DESC{};out->Type=D3D12_COMMAND_LIST_TYPE_DIRECT;return out;
}
static const uint8_t* blobAt(UINT64 va);
static void STDMETHODCALLTYPE queueExecute(ID3D12CommandQueue*,UINT count,ID3D12CommandList* const* lists) {
    CHECK(count==w->executeCount&&lists==w->executeArray);++w->executes;
    for(UINT i=0;i<count;++i)for(const auto& c:w->lists[obj(lists[i])->list].recording)w->gpu.push_back(c);
    for(UINT i=0;i<count;++i)for(const auto& o:w->lists[obj(lists[i])->list].ops) {
        w->gpuOps.push_back(o);auto& q=w->gpuOps.back();
        // J-3b9: the bytes a proxy draw's private SrtData holds when its list is SUBMITTED.
        if(q.t==Op::draw&&q.boundT10>=0x7000000000ull){std::memcpy(q.snap.data(),blobAt(q.boundT10),416);q.snapped=true;}
    }
    if(w->duringExecute)w->duringExecute(); // reentrant game call inside native Execute
}
static HRESULT STDMETHODCALLTYPE queueSignal(ID3D12CommandQueue* self,ID3D12Fence* fence,UINT64 value) {
    CHECK(obj(self)==&w->queue&&(obj(fence)==&w->fence||obj(fence)==&w->fence2)&&value>=1&&value>w->fenceValue&&(!w->signalQueued||value>w->pendingSignal));++w->signals; // monotonic
    if(SUCCEEDED(w->signalHr)){w->pendingSignal=value;w->signalQueued=true;}
    return w->signalHr;
}
// ----- device
static HRESULT STDMETHODCALLTYPE deviceAllocator(ID3D12Device*,D3D12_COMMAND_LIST_TYPE type,REFIID iid,void** out) {
    CHECK(type==D3D12_COMMAND_LIST_TYPE_DIRECT&&iid==__uuidof(ID3D12CommandAllocator));
    *out=&w->allocators[w->allocatorsCreated++];return S_OK;
}
static int createListIndex=0; // J-180: which fake list the next CreateCommandList returns (0 = the historical fixture)
static HRESULT STDMETHODCALLTYPE deviceList(ID3D12Device*,UINT node,D3D12_COMMAND_LIST_TYPE type,ID3D12CommandAllocator*,
                                            ID3D12PipelineState*,REFIID iid,void** out) {
    CHECK(node==0&&type==D3D12_COMMAND_LIST_TYPE_DIRECT);return listQuery(listAlias(createListIndex),iid,out);
}
// J-3b3 Device4 shape: 0 no Device4 (E_NOINTERFACE), 1 same object/table
// (production D3D12 shape), 2 distinct extension object/table (J-056 fixture shape).
static int device4Mode=0,device9Mode=0; // J-3b4: same 0/1/2 shapes for ID3D12Device9
static unsigned deviceQueries=0,list1Calls=0,queueCalls=0,queue1Calls=0;
static Table device9Table{};
static Object device9Object;
static Table device4Table{};
static Object device4Object;
// J-3b5: one of the two runtime-private IIDs DXGI's swap-chain creation queried
// on the device in live run 4 (in dxgi.dll/d3d12core.dll, not in the SDK headers).
static const GUID dxgiPrivateIid={0xde18ef3a,0x2089,0x4936,{0xa3,0xf3,0xec,0x78,0x7a,0xc6,0xa4,0x0d}};
static HRESULT STDMETHODCALLTYPE deviceQuery(ID3D12Device* self,REFIID iid,void** out) {
    ++deviceQueries;*out=nullptr;
    if(iid==__uuidof(IUnknown)||iid==__uuidof(ID3D12Device)||iid==dxgiPrivateIid)*out=self;
    else if(iid==__uuidof(ID3D12Device4)&&device4Mode==1)*out=self;
    else if(iid==__uuidof(ID3D12Device4)&&device4Mode==2)*out=&device4Object;
    else if(iid==__uuidof(ID3D12Device9)&&device9Mode==1)*out=self;
    else if(iid==__uuidof(ID3D12Device9)&&device9Mode==2)*out=&device9Object;
    if(!*out)return E_NOINTERFACE;++obj(*out)->refs;return S_OK;
}
// CreateCommandList1: list 1, created CLOSED, no allocator.
static HRESULT STDMETHODCALLTYPE deviceList1(ID3D12Device4*,UINT node,D3D12_COMMAND_LIST_TYPE type,D3D12_COMMAND_LIST_FLAGS flags,
                                             REFIID iid,void** out) {
    CHECK(node==0&&type==D3D12_COMMAND_LIST_TYPE_DIRECT&&flags==D3D12_COMMAND_LIST_FLAG_NONE);++list1Calls;
    return listQuery(listAlias(1),iid,out);
}
// J-3b4 queue creation: slot 8 returns the game queue, slot 75 the other queue.
static HRESULT STDMETHODCALLTYPE deviceQueue(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID iid,void** out) {
    CHECK(desc&&desc->Type==D3D12_COMMAND_LIST_TYPE_DIRECT&&iid==__uuidof(ID3D12CommandQueue));++queueCalls;
    *out=&w->queue;++w->queue.refs;return S_OK;
}
static HRESULT STDMETHODCALLTYPE deviceQueue1(ID3D12Device9*,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID creator,REFIID iid,void** out) {
    CHECK(desc&&creator==__uuidof(ID3D12Device)&&iid==__uuidof(ID3D12CommandQueue));++queue1Calls;
    *out=&w->otherQueue;++w->otherQueue.refs;return S_OK;
}
static HRESULT STDMETHODCALLTYPE devicePso(ID3D12Device*,const void*,REFIID,void**){CHECK(false);return E_FAIL;}
static HRESULT STDMETHODCALLTYPE deviceRoot(ID3D12Device*,UINT,const void*,SIZE_T,REFIID,void**){CHECK(false);return E_FAIL;}
static UINT fakeBytes(DXGI_FORMAT f){return f==DXGI_FORMAT_D32_FLOAT_S8X24_UINT||f==DXGI_FORMAT_R16G16B16A16_FLOAT?8:f==DXGI_FORMAT_D16_UNORM?2:4;}
static unsigned g_failResourceAt=UINT_MAX; // J-hud-markers: the private creation with this ordinal fails (UINT_MAX: none)
static HRESULT STDMETHODCALLTYPE deviceResource(ID3D12Device*,const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** out) {
    CHECK(flags==D3D12_HEAP_FLAG_NONE&&iid==__uuidof(ID3D12Resource));
    if(creationTexture) {
        CHECK(desc->Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        ++textureCreationCalls;*out=creationTexture;return S_OK;
    }
    if(desc->Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        // J-3b7 private targets: DEFAULT heap, one mip, single sample, optimized clear value of the same format.
        // J-3b9: a TYPELESS depth resource carries the typed DSV format as its clear value.
        const bool typelessDepth=(desc->Format==DXGI_FORMAT_R32G8X24_TYPELESS&&clear&&clear->Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT)||
            (desc->Format==DXGI_FORMAT_R32_TYPELESS&&clear&&clear->Format==DXGI_FORMAT_D32_FLOAT)||
            (desc->Format==DXGI_FORMAT_R24G8_TYPELESS&&clear&&clear->Format==DXGI_FORMAT_D24_UNORM_S8_UINT)||
            (desc->Format==DXGI_FORMAT_R16_TYPELESS&&clear&&clear->Format==DXGI_FORMAT_D16_UNORM);
        CHECK(heap->Type==D3D12_HEAP_TYPE_DEFAULT&&clear&&(clear->Format==desc->Format||typelessDepth)&&desc->MipLevels==1&&desc->DepthOrArraySize==1);
        if(typelessDepth)CHECK(desc->Flags==D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL); // no DENY_SHADER_RESOURCE
        CHECK(desc->SampleDesc.Count==1&&desc->SampleDesc.Quality==0&&desc->Layout==D3D12_TEXTURE_LAYOUT_UNKNOWN);
    } else CHECK(!clear&&desc->Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&desc->Layout==D3D12_TEXTURE_LAYOUT_ROW_MAJOR&&desc->Height==1);
    if(FAILED(w->createResourceHr)){*out=nullptr;return w->createResourceHr;}
    if(w->resourcesCreated==g_failResourceAt){*out=nullptr;return E_OUTOFMEMORY;} // J-hud-markers: fail ONE private creation (the nth)
    // Private objects land in 4.., skipping the fixed game textures 22/23 (J-hud-capture needs > 22).
    const size_t slot=w->resourcesCreated<22?w->resourcesCreated:w->resourcesCreated+2u;
    CHECK(slot<w->resources.size());++w->resourcesCreated;
    auto& r=w->resources[slot];r.heap=heap->Type;r.initial=initial;r.state=initial;r.width=desc->Width;
    r.dim=desc->Dimension;r.height=desc->Height;r.format=desc->Format;r.flags=desc->Flags;r.clear=clear!=nullptr;
    const uint64_t bytes=r.dim==D3D12_RESOURCE_DIMENSION_TEXTURE2D?desc->Width*desc->Height*fakeBytes(desc->Format):desc->Width;
    if(bytes>r.memory.size())r.big.assign(size_t(bytes),0);
    *out=&r.object;return S_OK;
}
// J-3b7 descriptor heaps and views (CPU handles are opaque fake addresses).
static D3D12_CPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE heapStart(ID3D12DescriptorHeap* self,D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    const auto i=unsigned(obj(self)-&w->heaps[0]);CHECK(i<w->heaps.size());
    out->ptr=SIZE_T(w->heapTypes[i]==D3D12_DESCRIPTOR_HEAP_TYPE_RTV?0x10000000:0x20000000)+i*0x100000;return out;
}
static HRESULT STDMETHODCALLTYPE deviceHeap(ID3D12Device*,const D3D12_DESCRIPTOR_HEAP_DESC* d,REFIID iid,void** out) {
    CHECK(d&&d->NumDescriptors>=1&&d->NumDescriptors<=4096&&d->Flags==D3D12_DESCRIPTOR_HEAP_FLAG_NONE&&iid==__uuidof(ID3D12DescriptorHeap));
    CHECK(d->Type==D3D12_DESCRIPTOR_HEAP_TYPE_RTV||d->Type==D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    const unsigned i=w->heapsCreated++;CHECK(i<w->heaps.size());w->heapTypes[i]=d->Type;
    auto& h=w->heaps[i];h.table=w->heapTable.data();h.kind=k_resource;h.refs=1;*out=&h;return S_OK;
}
static void setView(SIZE_T h,Object* o) { // a descriptor's contents (fake): the resource it names
    for(unsigned i=0;i<w->viewCount;++i)if(w->views[i].first==h){w->views[i].second=o;return;}
    CHECK(w->viewCount<w->views.size());w->views[w->viewCount++]={h,o};
}
static Object* viewOf(SIZE_T h){for(unsigned i=0;i<w->viewCount;++i)if(w->views[i].first==h)return w->views[i].second;return nullptr;}
static SIZE_T originOf(SIZE_T h){for(unsigned i=0;i<w->originCount;++i)if(w->origins[i].first==h)return w->origins[i].second;return h;}
static void STDMETHODCALLTYPE deviceView(ID3D12Device*,ID3D12Resource* r,const void* desc,D3D12_CPU_DESCRIPTOR_HANDLE h) {
    CHECK(r&&!desc);setView(h.ptr,obj(r));
}
constexpr UINT fakeDescriptorIncrement=0x20;
static UINT STDMETHODCALLTYPE deviceIncrement(ID3D12Device*,D3D12_DESCRIPTOR_HEAP_TYPE){return fakeDescriptorIncrement;}
// J-3b9c CopyDescriptorsSimple: copies the CONTENTS (the named resource) of `count` descriptors.
static void STDMETHODCALLTYPE deviceCopySimple(ID3D12Device*,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    CHECK(count>=1&&(type==D3D12_DESCRIPTOR_HEAP_TYPE_RTV||type==D3D12_DESCRIPTOR_HEAP_TYPE_DSV));++w->descriptorCopies;
    for(UINT k=0;k<count;++k) {
        const SIZE_T d=dst.ptr+k*fakeDescriptorIncrement,sh=src.ptr+k*fakeDescriptorIncrement;
        setView(d,viewOf(sh));
        bool found=false;for(unsigned i=0;i<w->originCount;++i)if(w->origins[i].first==d){w->origins[i].second=originOf(sh);found=true;}
        if(!found){CHECK(w->originCount<w->origins.size());w->origins[w->originCount++]={d,originOf(sh)};}
    }
}
// J-3b9 DSV: a null desc for a typed resource, a typed TEXTURE2D mip-0 view of a typeless one.
static unsigned dsvTyped=0;
static Resource& res(void* p);
static void STDMETHODCALLTYPE deviceDsv(ID3D12Device*,ID3D12Resource* r,const D3D12_DEPTH_STENCIL_VIEW_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE h) {
    CHECK(r);auto& x=res(r);
    const bool typeless=x.format==DXGI_FORMAT_R32G8X24_TYPELESS||x.format==DXGI_FORMAT_R32_TYPELESS||x.format==DXGI_FORMAT_R24G8_TYPELESS||x.format==DXGI_FORMAT_R16_TYPELESS;
    if(typeless){CHECK(d&&d->ViewDimension==D3D12_DSV_DIMENSION_TEXTURE2D&&d->Texture2D.MipSlice==0&&d->Flags==D3D12_DSV_FLAG_NONE);
        CHECK(d->Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT||d->Format==DXGI_FORMAT_D32_FLOAT||d->Format==DXGI_FORMAT_D24_UNORM_S8_UINT||d->Format==DXGI_FORMAT_D16_UNORM);++dsvTyped;}
    else CHECK(!d);
    setView(h.ptr,obj(r));
}
static UINT64 STDMETHODCALLTYPE resourceVa(ID3D12Resource* self) {
    for(size_t i=0;i<w->resources.size();++i)if(&w->resources[i].object==obj(self))return 0x7000000000ull+uint64_t(i)*0x1000000;
    CHECK(false);return 0;
}
static HRESULT STDMETHODCALLTYPE deviceFence(ID3D12Device*,UINT64 initial,D3D12_FENCE_FLAGS flags,REFIID iid,void** out) {
    CHECK(initial==0&&flags==D3D12_FENCE_FLAG_NONE&&iid==__uuidof(ID3D12Fence));++w->fenceCreates;*out=&w->fence;return S_OK;
}
static HRESULT STDMETHODCALLTYPE deviceRemoved(ID3D12Device*){return w->removedHr;}
// ----- resource / fence
static Resource& res(void* p){for(auto& r:w->resources)if(&r.object==p)return r;CHECK(false);return w->resources[0];}
// J-3b7b: ID3D12Resource::GetDesc (MSVC hidden return pointer) and GetHeapProperties.
static D3D12_RESOURCE_DESC* STDMETHODCALLTYPE resourceDesc(ID3D12Resource* self,D3D12_RESOURCE_DESC* out) {
    auto& r=res(self);*out=D3D12_RESOURCE_DESC{};out->Dimension=r.dim;out->Width=r.width;out->Height=r.height;out->Format=r.format;
    out->DepthOrArraySize=UINT16(r.arraySize);out->MipLevels=1;out->SampleDesc.Count=1;out->Flags=r.flags;return out;
}
static HRESULT STDMETHODCALLTYPE resourceHeap(ID3D12Resource* self,D3D12_HEAP_PROPERTIES* props,D3D12_HEAP_FLAGS* flags) {
    auto& r=res(self);if(props){*props=D3D12_HEAP_PROPERTIES{};props->Type=r.heap;}if(flags)*flags=D3D12_HEAP_FLAG_NONE;return S_OK;
}
static HRESULT STDMETHODCALLTYPE resourceMap(ID3D12Resource* self,UINT sub,const D3D12_RANGE* read,void** data) {
    auto& r=res(self);CHECK(sub==0&&read&&data);++r.maps;r.lastReadBegin=read->Begin;r.lastReadEnd=read->End;
    *data=r.data();return S_OK;
}
static void STDMETHODCALLTYPE resourceUnmap(ID3D12Resource* self,UINT sub,const D3D12_RANGE* written) {
    auto& r=res(self);CHECK(sub==0&&written);++r.unmaps;r.lastWrittenBegin=written->Begin;r.lastWrittenEnd=written->End;
}
static UINT64 STDMETHODCALLTYPE fenceValue(ID3D12Fence*){return w->fenceValue;}
static BOOL WINAPI protect(LPVOID,SIZE_T,DWORD,DWORD* old){*old=PAGE_READWRITE;return TRUE;}

static void gpuOps(); // J-3b7: replays the recorded step-3 ops (defined with the step-3 fixture)
static SIZE_T hudOutputHandleValue(){return 0x17100;} // == hudOutputHandle (declared with the HUD fixtures)
static void advance() { // the "GPU": execute recorded copies, then the queued Signal
    gpuOps();
    for(const auto& c:w->gpu) {
        auto& src=res(c.src);auto& dst=res(c.dst);
        CHECK(c.srcOffset+c.bytes<=src.memory.size()&&c.dstOffset+c.bytes<=dst.memory.size());
        std::memcpy(dst.memory.data()+c.dstOffset,src.memory.data()+c.srcOffset,size_t(c.bytes));
        if(w->corrupt)dst.memory[100]^=0x5a;
    }
    w->gpu.clear();
    if(w->signalQueued){w->fenceValue=w->pendingSignal;w->signalQueued=false;}
}
static void fillTable(Table& t){t.fill(reinterpret_cast<void*>(&unused));
    t[0]=reinterpret_cast<void*>(&queueQuery);t[1]=reinterpret_cast<void*>(&addRef);t[2]=reinterpret_cast<void*>(&release);}
static void build(bool enabled) {
    w=new World;
    acz::g_protect=protect;acz::g_pinResult=true;acz::g_copyObservationTest=true;
    acz::g_step1Test=enabled;acz::g_step1NoThread=true;
    acz::g_step1Capture=[](const char* text){w->lines.emplace_back(text);};
    fillTable(w->deviceTable);auto& d=w->deviceTable;
    d[9]=reinterpret_cast<void*>(&deviceAllocator);d[10]=reinterpret_cast<void*>(&devicePso);d[12]=reinterpret_cast<void*>(&deviceList);
    d[16]=reinterpret_cast<void*>(&deviceRoot);d[27]=reinterpret_cast<void*>(&deviceResource);
    d[36]=reinterpret_cast<void*>(&deviceFence);d[37]=reinterpret_cast<void*>(&deviceRemoved);
    d[0]=reinterpret_cast<void*>(&deviceQuery);d[51]=reinterpret_cast<void*>(&deviceList1);
    d[8]=reinterpret_cast<void*>(&deviceQueue);d[75]=reinterpret_cast<void*>(&deviceQueue1);
    device4Table=d;device4Object.table=device4Table.data();device4Object.kind=k_device;
    device9Table=d;device9Object.table=device9Table.data();device9Object.kind=k_device;
    w->device.table=d.data();w->device.kind=k_device;
    w->otherDevice.table=d.data();w->otherDevice.kind=k_device; // second device, same runtime table
    fillTable(w->queueTable);auto& q=w->queueTable;
    q[7]=reinterpret_cast<void*>(&queueDevice);q[10]=reinterpret_cast<void*>(&queueExecute);
    q[14]=reinterpret_cast<void*>(&queueSignal);q[18]=reinterpret_cast<void*>(&queueDesc);
    w->queue.table=q.data();w->queue.kind=k_queue;
    w->otherQueueTable=q;w->otherQueue.table=w->otherQueueTable.data();w->otherQueue.kind=k_queue;
    fillTable(w->allocatorTable);for(auto& a:w->allocators){a.table=w->allocatorTable.data();a.kind=k_allocator;}
    fillTable(w->resourceTable);w->resourceTable[8]=reinterpret_cast<void*>(&resourceMap);w->resourceTable[9]=reinterpret_cast<void*>(&resourceUnmap);
    w->resourceTable[11]=reinterpret_cast<void*>(&resourceVa);
    w->resourceTable[10]=reinterpret_cast<void*>(&resourceDesc);w->resourceTable[14]=reinterpret_cast<void*>(&resourceHeap);
    // Independent, initially UNPATCHED texture dispatch: buffer hooks cannot cover it.
    w->worldTextureTable=w->resourceTable;
    d[15]=reinterpret_cast<void*>(&deviceIncrement);d[24]=reinterpret_cast<void*>(&deviceCopySimple);
    d[14]=reinterpret_cast<void*>(&deviceHeap);d[20]=reinterpret_cast<void*>(&deviceView);d[21]=reinterpret_cast<void*>(&deviceDsv);
    fillTable(w->heapTable);w->heapTable[9]=reinterpret_cast<void*>(&heapStart);
    for(auto& r:w->resources){r.object.table=w->resourceTable.data();r.object.kind=k_resource;}
    fillTable(w->fenceTable);w->fenceTable[8]=reinterpret_cast<void*>(&fenceValue);w->fence.table=w->fenceTable.data();w->fence.kind=k_fence;
    w->fence2.table=w->fenceTable.data();w->fence2.kind=k_fence;
    fillTable(w->psoTable);w->pso.table=w->psoTable.data();w->pso.kind=k_pso;w->unmatchedPso.table=w->psoTable.data();w->unmatchedPso.kind=k_pso;
    for(int l=0;l<2;++l)for(int i=0;i<11;++i) {
        auto& t=w->lists[l].tables[i];t.fill(reinterpret_cast<void*>(&unused));
        t[0]=reinterpret_cast<void*>(&listQuery);t[1]=reinterpret_cast<void*>(&addRef);t[2]=reinterpret_cast<void*>(&release);
        t[7]=reinterpret_cast<void*>(&listDevice);t[8]=reinterpret_cast<void*>(&listType);
        t[9]=reinterpret_cast<void*>(&listClose);t[10]=reinterpret_cast<void*>(&listReset);t[11]=reinterpret_cast<void*>(&listClear);
        t[12]=reinterpret_cast<void*>(&listDraw);t[13]=reinterpret_cast<void*>(&listDrawIndexed);t[15]=reinterpret_cast<void*>(&listCopy);
        t[25]=reinterpret_cast<void*>(&listSetPso);t[20]=reinterpret_cast<void*>(&listTopology);
        t[52]=reinterpret_cast<void*>(&listQueryStub);t[53]=reinterpret_cast<void*>(&listQueryStub);
        t[16]=reinterpret_cast<void*>(&listCopyTex);t[21]=reinterpret_cast<void*>(&listViewports);t[22]=reinterpret_cast<void*>(&listScissors);
        t[26]=reinterpret_cast<void*>(&listBarrier);t[40]=reinterpret_cast<void*>(&listSrv);t[46]=reinterpret_cast<void*>(&listOm);t[44]=reinterpret_cast<void*>(&listVb);
        t[47]=reinterpret_cast<void*>(&listClearDsv);t[48]=reinterpret_cast<void*>(&listClearRtv);
        t[55]=reinterpret_cast<void*>(&listPredication);
        auto& o=w->lists[l].aliases[i];o.table=t.data();o.kind=k_list;o.list=l;o.alias=i;o.refs=i?0:1000;
    }
    // Fake bookkeeping storage is reserved up front so the Draw-path
    // allocation check measures the production hook path only.
    for(auto& l:w->lists){l.events.reserve(256);l.recording.reserve(16);l.ops.reserve(128);}
    w->gpu.reserve(64);w->lines.reserve(64);w->gpuOps.reserve(512);
    // Creation-time DrawCPT match fact (normally set by the hooked PSO creation).
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     s.pipelines[reinterpret_cast<ID3D12PipelineState*>(&w->pso)].drawcpt_identity_matched=true;
     s.pipelines[reinterpret_cast<ID3D12PipelineState*>(&w->unmatchedPso)].drawcpt_identity_matched=false;}
}
static ID3D12Device* device(){return reinterpret_cast<ID3D12Device*>(&w->device);}
static ID3D12CommandQueue* queue(){return reinterpret_cast<ID3D12CommandQueue*>(&w->queue);}
static ID3D12PipelineState* fakePso(bool matched=true){return reinterpret_cast<ID3D12PipelineState*>(matched?&w->pso:&w->unmatchedPso);}
static ID3D12CommandAllocator* allocator(int i){return reinterpret_cast<ID3D12CommandAllocator*>(&w->allocators[i]);}
static Resource& upload(){return w->resources[0];}
static Resource& readback(){return w->resources[1];}
static int phase(){return acz::g_step1Phase.load();}
static bool has(const char* text){for(const auto& l:w->lines)if(l.find(text)!=std::string::npos)return true;return false;}
static void dump(){for(const auto& l:w->lines)std::fprintf(stderr,"  %s\n",l.c_str());}
static bool verdict(const char* text) {
    for(const auto& l:w->lines)if(l.find("EXPERIMENT_RESULT=")!=std::string::npos)return l.find(text)!=std::string::npos;
    return false;
}
static unsigned verdicts(){unsigned n=0;for(const auto& l:w->lines)if(l.find("EXPERIMENT_RESULT=")!=std::string::npos)++n;return n;}
// Discovery, device hooks, owner preparation, game allocators and list A.
static void bootBuilt(bool enabled);
static void boot(bool enabled=true) {build(enabled);bootBuilt(enabled);}
static void bootBuilt(bool enabled) {
    acz::captureDeviceAndQueue(reinterpret_cast<IUnknown*>(&w->queue));
    CHECK(acz::getHookedQueue()==queue()&&acz::getHookedDevice()==device());
    CHECK(acz::hookCaptureDevice(device())); // offline: hookNativeBindingDevice is not compiled in
    if(enabled) {
        CHECK(phase()==int(acz::Step1Phase::preparing));
        CHECK(w->deviceTable[9]==reinterpret_cast<void*>(&acz::step1CreateAllocator));
    } else {
        CHECK(phase()==0&&!acz::g_step1.load()&&w->deviceTable[9]==reinterpret_cast<void*>(&deviceAllocator));
    }
    ID3D12CommandAllocator* a=nullptr;
    for(int i=0;i<2;++i)CHECK(SUCCEEDED(device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a))));
    ID3D12GraphicsCommandList* list=nullptr;
    CHECK(SUCCEEDED(device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator(0),nullptr,IID_PPV_ARGS(&list))));
    CHECK(list==listAlias(0));
    CHECK(list->Close()==S_OK); // created open; the creation recording is never eligible
}
static void prepare() {
    CHECK(acz::step1Tick());CHECK(phase()==int(acz::Step1Phase::ready));
    CHECK(upload().heap==D3D12_HEAP_TYPE_UPLOAD&&upload().initial==D3D12_RESOURCE_STATE_GENERIC_READ&&upload().width==4096);
    CHECK(readback().heap==D3D12_HEAP_TYPE_READBACK&&readback().initial==D3D12_RESOURCE_STATE_COPY_DEST&&readback().width==4096);
    CHECK(upload().maps==1&&upload().unmaps==1&&upload().lastReadBegin==0&&upload().lastReadEnd==0);
    CHECK(upload().lastWrittenBegin==0&&upload().lastWrittenEnd==4096&&readback().maps==0&&w->fenceCreates==1);
    std::array<uint8_t,4096> expected{};gpu_copy::step1::pattern(expected.data(),expected.size());
    CHECK(std::memcmp(expected.data(),upload().memory.data(),4096)==0);
    CHECK(has("event=pins;")&&has("event=prepare;")&&has("event=timeline;")&&has("bound=1"));
}
static void frame(int alloc=0,bool matched=true,bool indexed=false) {
    auto* l=listAlias(0);CHECK(l->Reset(allocator(alloc),fakePso(matched))==w->resetHr);
    if(indexed)l->DrawIndexedInstanced(9,1,0,0,0);else l->DrawInstanced(3,1,2,0);
}
static void execute(std::vector<ID3D12CommandList*> lists) {
    w->executeCount=UINT(lists.size());w->executeArray=lists.data();
    const auto before=w->executes;queue()->ExecuteCommandLists(w->executeCount,w->executeArray);
    CHECK(w->executes==before+1);w->executeArray=nullptr;
}
static ID3D12CommandList* cl(int list){return reinterpret_cast<ID3D12CommandList*>(listAlias(list));}
static void ticks(unsigned n){for(unsigned i=0;i<n&&acz::step1Tick();++i){}}
static void retained() {
    CHECK(upload().object.refs==1&&readback().object.refs==1&&w->fence.refs==1);
    const auto s=gpu_copy::diagnostic_session().snapshot();
    CHECK(s.private_held&&s.state==gpu_copy::State::quarantined&&!s.retired);
}
static void inserted(bool indexed=false) {
    const auto draws=w->lists[0].draws;
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);
    const auto before=allocations;traceAllocations=true;
    if(indexed)l0->DrawIndexedInstanced(9,1,0,0,0);else l0->DrawInstanced(3,1,2,0);traceAllocations=false;
    CHECK(allocations==before); // no heap allocation on the inserting Draw path
    CHECK(phase()==int(acz::Step1Phase::inserted));
    auto& l=w->lists[0];CHECK(l.copies==1&&l.draws==draws+1&&l.recording.size()==1);
    CHECK(l.events.size()>=3&&l.events[l.events.size()-2]=="copy"&&l.events.back()==(indexed?"draw-indexed":"draw"));
    const auto& c=l.recording[0];
    CHECK(c.dst==&readback().object&&c.src==&upload().object&&c.dstOffset==0&&c.srcOffset==0&&c.bytes==4096);
}

// ------------------------------------------------------------- scenarios
static void off() {
    // Default OFF: no owner, no allocator hook, empty provider, no native extra.
    boot(false);
    frame();CHECK(w->lists[0].copies==0&&w->lists[0].draws==1);
    CHECK(SUCCEEDED(listAlias(0)->Close()));execute({cl(0)});advance();
    CHECK(w->signals==0&&w->resourcesCreated==0&&w->fenceCreates==0&&w->lines.empty());
    CHECK(!acz::step1Tick()&&phase()==0);
    const auto feed=acz::state().states.at(listAlias(0)).copyObservation;
    CHECK(feed&&!feed->constructed()); // 3a: ownership unavailable while OFF
}
static void success() {
    boot();prepare();
    // A second, certified but UNWITNESSED list (discovered, not created through
    // the hook) appears in the batch: canonical routing makes it a non-alias.
    CHECK(acz::hookCaptureCommandList(listAlias(1)));
    CHECK(listAlias(1)->Reset(allocator(1),nullptr)==S_OK);CHECK(listAlias(1)->Close()==S_OK);
    inserted();
    CHECK(listAlias(0)->Close()==S_OK);
    CHECK(acz::step1Tick()); // insert/close lines; no Execute yet
    execute({cl(1),cl(0)});
    CHECK(w->signals==1&&w->fenceValue==0&&readback().maps==0);
    CHECK(acz::step1Tick()&&acz::step1Tick()); // pending polls: never Map before completion
    CHECK(readback().maps==0&&upload().maps==1);
    advance();CHECK(w->fenceValue==1);
    ticks(3); // completed, but recording not yet closed: still no Map
    CHECK(readback().maps==0);
    frame(1,false); // Reset with the ROTATED allocator closes the old recording
    ticks(5);
    CHECK(readback().maps==1&&readback().unmaps==1&&readback().lastReadBegin==0&&readback().lastReadEnd==4096);
    CHECK(readback().lastWrittenBegin==0&&readback().lastWrittenEnd==0);
    if(!verdict("EXPERIMENT_RESULT=PASS")){dump();CHECK(false);}
    CHECK(has("event=execute;")&&has("unknown-members=1")&&has("ordinal=1")&&has("event=signal;hr=00000000;ok=1"));
    CHECK(has("event=poll;")&&has("pending-seen=1")&&has("event=reset-closure;")&&has("equal=1;first-mismatch=-1"));
    CHECK(has("resources=released")&&has("order=CopyBufferRegion-then-original-Draw"));
    CHECK(upload().object.refs==0&&readback().object.refs==0&&w->fence.refs==1); // fence retained
    CHECK(gpu_copy::diagnostic_session().snapshot().retired);
    // One attempt only: later matched draws record nothing more.
    frame();frame(0,true,true);CHECK(w->lists[0].copies==1&&w->lists[0].draws==4&&verdicts()==1);
    CHECK(!acz::step1Tick());
    CHECK(w->lines.size()<=diagnostics::step1_records); // fits the reserved evidence partition
}
static void refusals_then_insert() {
    boot();prepare();
    frame(0,false);CHECK(w->lists[0].copies==0);                // unmatched PSO: not even counted
    auto* l=listAlias(0);CHECK(l->Reset(allocator(0),fakePso())==S_OK);
    l->SetPredication(reinterpret_cast<ID3D12Resource*>(&w->fence),0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    l->DrawInstanced(3,1,2,0);CHECK(w->lists[0].copies==0);     // active predication
    // Reentrant Draw from inside the native SetPredication: in-flight window.
    CHECK(l->Reset(allocator(0),fakePso())==S_OK);
    w->inPredication=[&]{l->DrawInstanced(3,1,2,0);};
    l->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);w->inPredication={};
    CHECK(w->lists[0].copies==0);
    {const auto& st=*acz::g_step1.load();CHECK(st.matchedDraws==2&&st.drawRefusals[6]==1&&st.drawRefusals[1]==1);}
    CHECK(phase()==int(acz::Step1Phase::ready)); // refusals never consume the attempt
    // Unwitnessed (discovered) list with a matched PSO: ownership unavailable.
    CHECK(acz::hookCaptureCommandList(listAlias(1)));
    CHECK(listAlias(1)->Reset(allocator(1),fakePso())==S_OK);listAlias(1)->DrawInstanced(3,1,2,0);
    CHECK(w->lists[1].copies==0&&acz::g_step1.load()->drawRefusals[0]==1);
    CHECK(acz::g_step1.load()->resetRefusals[4]>=1); // list-unwitnessed reset classification
    inserted();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();frame(1,false);ticks(10);
    if(!verdict("PASS")){dump();CHECK(false);}
    CHECK(has("matched=4"));
}
static void no_qualified() {
    boot();prepare();
    frame(0,false);ticks(gpu_copy::step1::wait_ticks+5);
    if(!verdict("REFUSED;reason=no-qualified-matched-draw-within-600s")){dump();CHECK(false);}
    CHECK(has("gpu-copy-step1-status;event=waiting")&&has("resources=released-never-recorded"));
    CHECK(upload().object.refs==0&&readback().object.refs==0&&w->lists[0].copies==0);
    frame();CHECK(w->lists[0].copies==0&&phase()==int(acz::Step1Phase::done));
}
static void prepare_fail() {
    boot();w->createResourceHr=E_OUTOFMEMORY;
    CHECK(!acz::step1Tick());
    if(!verdict("REFUSED;reason=upload-create-failed")){dump();CHECK(false);}
    frame();CHECK(w->lists[0].copies==0&&w->lists[0].draws==1&&w->fenceCreates==0);
}
static void close_fail() {
    boot();prepare();inserted();
    w->closeHr=E_FAIL;CHECK(listAlias(0)->Close()==E_FAIL);w->closeHr=S_OK;
    execute({cl(0)});CHECK(w->signals==0);advance();frame(1,false);ticks(5);
    if(!verdict("FAIL;reason=close-failed")){dump();CHECK(false);}
    CHECK(readback().maps==0);retained();
}
static void duplicate() {
    boot();prepare();inserted();CHECK(listAlias(0)->Close()==S_OK);
    execute({cl(0),cl(0)});CHECK(w->signals==0); // original count/array forwarded once
    advance();frame(1,false);ticks(5);
    if(!verdict("FAIL;reason=duplicate-in-batch")){dump();CHECK(false);}
    CHECK(readback().maps==0);retained();
}
static void reexecute() {
    boot();prepare();inserted();CHECK(listAlias(0)->Close()==S_OK);
    execute({cl(0)});CHECK(w->signals==1);
    execute({cl(0)});CHECK(w->signals==1); // second writer: no second Signal
    advance();frame(1,false);ticks(10);
    if(!verdict("FAIL;reason=re-executed-recording")){dump();CHECK(false);}
    CHECK(readback().maps==0);retained();
}
static void uint64max() {
    boot();prepare();inserted();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});
    w->fenceValue=(std::numeric_limits<uint64_t>::max)();w->removedHr=DXGI_ERROR_DEVICE_REMOVED;
    frame(1,false);ticks(10);
    if(!verdict("FAIL;reason=device-lost-uint64max")){dump();CHECK(false);}
    CHECK(has("device-removed-reason=887a0005")&&readback().maps==0);retained();
}
static void signal_fail() {
    boot();prepare();inserted();CHECK(listAlias(0)->Close()==S_OK);
    w->signalHr=DXGI_ERROR_DEVICE_REMOVED;execute({cl(0)});CHECK(w->signals==1);
    advance();frame(1,false);ticks(10);
    if(!verdict("FAIL;reason=signal-failed")){dump();CHECK(false);}
    CHECK(has("event=signal;hr=887a0005;ok=0")&&readback().maps==0);retained();
}
static void mismatch() {
    boot();prepare();inserted();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});
    w->corrupt=true;advance();frame(1,false);ticks(10);
    if(!verdict("FAIL;reason=byte-mismatch")){dump();CHECK(false);}
    CHECK(has("equal=0;first-mismatch=100")&&readback().maps==1&&readback().unmaps==1);retained();
}
static void closure_timeout() {
    boot();prepare();inserted();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();
    ticks(gpu_copy::step1::closure_ticks+10);
    if(!verdict("FAIL;reason=reset-closure-timeout")){dump();CHECK(false);}
    CHECK(readback().maps==0);retained();
}
static void no_execute() {
    boot();prepare();inserted();CHECK(listAlias(0)->Close()==S_OK);
    ticks(gpu_copy::step1::submit_ticks+10);
    if(!verdict("FAIL;reason=no-execute-signal-within-bound")){dump();CHECK(false);}
    CHECK(w->signals==0&&readback().maps==0);retained();
    // A late Execute of the retained recording still forwards; nothing signals.
    execute({cl(0)});CHECK(w->signals==0);
}
static void reset_before_complete() {
    boot();prepare();inserted(true); // indexed Draw seam
    ticks(1);CHECK(has("draw=1,9,1,0,0,0"));CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});
    frame(1,false); // closure first, GPU still pending
    ticks(3);CHECK(readback().maps==0&&phase()==int(acz::Step1Phase::inserted));
    advance();ticks(5);
    if(!verdict("PASS")){dump();CHECK(false);}
    CHECK(readback().maps==1);
}
static bool line(const char* a,const char* b);
// J-3b5: the live-run-4 sequence. The game Resets and re-records the candidate
// list every frame and executes it (a NEW generation) again, together with
// other lists, long before the private fence completes. Later generations do
// not contain the copy and must not make the old transaction ambiguous.
static void otherList() {
    CHECK(acz::hookCaptureCommandList(listAlias(1)));
    CHECK(listAlias(1)->Reset(allocator(1),nullptr)==S_OK);CHECK(listAlias(1)->Close()==S_OK);
}
static void laterFrame(bool queries=false) {
    // Next frame: the same list, a new generation (matched PSO, same indexed Draw).
    frame(1,true,true);
    if(queries) {
        auto* l=reinterpret_cast<ID3D12GraphicsCommandList*>(listAlias(0));
        l->BeginQuery(nullptr,D3D12_QUERY_TYPE_OCCLUSION,0);l->EndQuery(nullptr,D3D12_QUERY_TYPE_OCCLUSION,0);
    }
    CHECK(listAlias(0)->Close()==S_OK);
    CHECK(listAlias(1)->Reset(allocator(0),nullptr)==S_OK);CHECK(listAlias(1)->Close()==S_OK);
    execute({cl(1)});execute({cl(0)});execute({cl(1),cl(0)});
}
static void live_run4(bool queries) {
    boot();prepare();otherList();
    inserted(true);CHECK(listAlias(0)->Close()==S_OK);
    execute({cl(1)});                  // another list first (executes-armed=2 at the candidate)
    execute({cl(0)});CHECK(w->signals==1&&w->fenceValue==0);
    ticks(1);                          // insert/close/execute/signal lines
    for(int f=0;f<6;++f){laterFrame(queries);ticks(1);}
    if(!(w->signals==1&&readback().maps==0&&phase()==int(acz::Step1Phase::inserted))){dump();CHECK(false);} // still pending, no Map
    CHECK(w->lists[0].copies==1);      // one attempt: later matched draws recorded nothing
    advance();CHECK(w->fenceValue==1);ticks(5);
    if(!verdict("EXPERIMENT_RESULT=PASS")){dump();CHECK(false);}
    CHECK(readback().maps==1&&readback().unmaps==1&&has("equal=1;first-mismatch=-1"));
    CHECK(has("event=reset-closure;")&&has("pending-seen=1"));
    CHECK(line("event=disposition;resources=released","fault=none"));
    CHECK(line("event=disposition;resources=released","later-executes=12"));
    CHECK(upload().object.refs==0&&readback().object.refs==0&&gpu_copy::diagnostic_session().snapshot().retired);
    CHECK(w->lines.size()<=diagnostics::step1_records);
}
static void later_generations() {live_run4(false);}
static void later_generation_queries() {live_run4(true);}
// Negative variants of the same sequence: the real safety rules still fire.
static void later_reexecute_same() {
    // The SAME closed generation executed again before any Reset (other lists between).
    boot();prepare();otherList();inserted(true);CHECK(listAlias(0)->Close()==S_OK);
    execute({cl(0)});execute({cl(1)});execute({cl(1),cl(0)});CHECK(w->signals==1);
    laterFrame();advance();ticks(10);
    if(!verdict("FAIL;reason=re-executed-recording")){dump();CHECK(false);}
    CHECK(readback().maps==0);retained();
    CHECK(line("event=disposition;resources=retained-quarantined","ledger-fault=begin-execute-refused"));
}
static void later_reset_failed() {
    // A failed Reset of the candidate before closure: the old recording's fate is unknown.
    boot();prepare();otherList();inserted(true);CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});
    w->resetHr=E_FAIL;frame(1,true,true);w->resetHr=S_OK;advance();ticks(10);
    if(!verdict("FAIL;reason=witness-ambiguity")){dump();CHECK(false);}
    CHECK(line("event=disposition;resources=retained-quarantined","witness-fault=reset-failed"));
    CHECK(readback().maps==0);retained();
}
static void later_reset_in_join() {
    // A Reset of the candidate inside the native Execute (before the private Signal joined).
    boot();prepare();otherList();inserted(true);CHECK(listAlias(0)->Close()==S_OK);
    w->duringExecute=[]{CHECK(listAlias(0)->Reset(allocator(1),fakePso())==S_OK);};
    execute({cl(0)});w->duringExecute={};
    advance();ticks(10);
    if(!verdict("FAIL")){dump();CHECK(false);}
    CHECK(line("event=disposition;resources=retained-quarantined","ledger-fault=close-recording-refused"));
    CHECK(readback().maps==0);retained();
}
static void later_unclosed_execute() {
    // The candidate executed while its generation was never Closed.
    boot();prepare();otherList();inserted(true);execute({cl(0)});CHECK(w->signals==0);
    advance();frame(1,false);ticks(10);
    if(!verdict("FAIL;reason=unclosed-generation")){dump();CHECK(false);}
    CHECK(readback().maps==0);retained();
}
static void later_execute_open() {
    // After closure, a LATER generation submitted while still open (never Closed).
    boot();prepare();otherList();inserted(true);CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});
    frame(1,true,true);execute({cl(0)});advance();ticks(10);
    if(!verdict("FAIL;reason=witness-ambiguity")){dump();CHECK(false);}
    CHECK(line("event=disposition;resources=retained-quarantined","witness-fault=later-execute-unclosed"));
    CHECK(readback().maps==0);retained();
}
static void later_released_before_reset() {
    // Final Release of the candidate list before any observed Reset closure.
    boot();prepare();otherList();inserted(true);CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});
    w->lists[0].aliases[0].refs=1;CHECK(listAlias(0)->Release()==0);
    advance();ticks(10);
    if(!verdict("FAIL;reason=witness-ambiguity")){dump();CHECK(false);}
    CHECK(line("event=disposition;resources=retained-quarantined","witness-fault=list-released"));
    CHECK(readback().maps==0);retained();
}
static void poll_timeout() {
    boot();prepare();inserted();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});frame(1,false);
    ticks(80);
    if(!verdict("FAIL;reason=fence-poll-timeout")){dump();CHECK(false);}
    CHECK(readback().maps==0);retained();
}
static void cross_queue() {
    boot();prepare();inserted();CHECK(listAlias(0)->Close()==S_OK);
    CHECK(acz::hookCommandQueue(reinterpret_cast<ID3D12CommandQueue*>(&w->otherQueue)));
    std::vector<ID3D12CommandList*> lists{cl(0)};w->executeCount=1;w->executeArray=lists.data();
    reinterpret_cast<ID3D12CommandQueue*>(&w->otherQueue)->ExecuteCommandLists(1,lists.data());
    CHECK(w->signals==0);advance();frame(1,false);ticks(5);
    if(!verdict("FAIL;reason=unauthenticated-queue")){dump();CHECK(false);}
    CHECK(readback().maps==0);retained();
}
// ------------------------------------------------------------- J-3b3 creation witnesses
static acz::Step1State& st1(){return *acz::g_step1.load();}
static bool line(const char* a,const char* b) {
    for(const auto& l:w->lines)if(l.find(a)!=std::string::npos&&l.find(b)!=std::string::npos)return true;
    return false;
}
static void off_list1() {
    // Default OFF, even with the production (shared-table) Device4 shape:
    // no Device4 QI, slot 51 untouched, CreateCommandList1 forwarded unchanged.
    device4Mode=1;boot(false);
    CHECK(deviceQueries==0&&w->deviceTable[51]==reinterpret_cast<void*>(&deviceList1));
    ID3D12GraphicsCommandList* l=nullptr;
    CHECK(SUCCEEDED(reinterpret_cast<ID3D12Device4*>(device())->CreateCommandList1(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_LIST_FLAG_NONE,IID_PPV_ARGS(&l)))&&l==listAlias(1)&&list1Calls==1);
    CHECK(!acz::state().states.count(listAlias(1))&&w->lines.empty()); // discovered at Reset as before (D-86)
}
static void list1_witnessed() {
    // Shared table: slot 51 is wrapped while the experiment is on; the closed
    // list is certified at creation and authenticated at its first Reset.
    device4Mode=1;boot();
    CHECK(w->deviceTable[51]==reinterpret_cast<void*>(&acz::step1CreateList1)&&st1().list1Hook.load()==1);
    const auto refs=w->device.refs;
    prepare();
    ID3D12GraphicsCommandList* l=nullptr;
    CHECK(SUCCEEDED(reinterpret_cast<ID3D12Device4*>(device())->CreateCommandList1(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_LIST_FLAG_NONE,IID_PPV_ARGS(&l)))&&l==listAlias(1));
    CHECK(list1Calls==1&&w->device.refs==refs); // original forwarded exactly once; QI balanced
    {const auto& c=st1().creates[1];CHECK(c.seen==1&&c.recorded==1&&!c.unregistered&&!c.failed&&!c.collision);}
    {auto& s=acz::state();std::lock_guard lock(s.mutex);const auto& ls=s.states.at(listAlias(1));
     CHECK(ls.creation_observed&&ls.registration_path==1&&ls.interfaces_observed&&!ls.reset_observed);}
    CHECK(w->lists[1].resets==0&&w->lists[1].closes==0); // closed creation opens no recording
    // First recording: observed Reset on a witnessed allocator, matched Draw.
    CHECK(l->Reset(allocator(1),fakePso())==S_OK);
    CHECK(st1().resetAcceptedByPath[1]==1&&!st1().resetRefusedByPath[1]);
    const auto before=allocations;traceAllocations=true;
    l->DrawInstanced(3,1,2,0);traceAllocations=false;CHECK(allocations==before);
    CHECK(phase()==int(acz::Step1Phase::inserted)&&w->lists[1].copies==1&&w->lists[1].draws==1);
    CHECK(l->Close()==S_OK);execute({cl(1)});advance();
    CHECK(l->Reset(allocator(0),fakePso(false))==S_OK);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PASS")){dump();CHECK(false);}
    if(!line("gpu-copy-step1;event=creation","CL=1:1:0:0:0:0:0:0;CL1=1:1:0:0:0:0:0:0;CA=2:2:0:0:0:0:0:0;cl1-hook=1")){dump();CHECK(false);}
    CHECK(w->lines.size()<=diagnostics::step1_records);
}
static void list1_distinct() {
    // J-056 shape: Device4 is a different object/table. Nothing is patched.
    device4Mode=2;boot();
    CHECK(st1().list1Hook.load()==2&&w->deviceTable[51]==reinterpret_cast<void*>(&deviceList1));
    CHECK(device4Table[51]==reinterpret_cast<void*>(&deviceList1)&&device4Table[0]==reinterpret_cast<void*>(&deviceQuery));
    CHECK(device4Object.refs==1); // QI reference released through saved dispatch
    const auto q=deviceQueries;CHECK(acz::hookCaptureDevice(device())&&deviceQueries==q); // decided once
    prepare();frame(0,false);ticks(gpu_copy::step1::wait_ticks+5);
    if(!line("gpu-copy-step1;event=creation","cl1-hook=2")){dump();CHECK(false);}
}
static void prehook_discovery() {
    // Lists and allocators that predate the device hook (or any unhooked
    // creation seam): list 1 shares list 0's (already patched) class tables and
    // is first seen at Reset; allocators 2/3 were never created through slot 9.
    boot();prepare();
    for(int i=0;i<11;++i)w->lists[1].aliases[i].table=w->lists[0].tables[i].data();
    auto* late=listAlias(1);
    CHECK(late->Reset(allocator(3),fakePso())==S_OK);             // first sight: no capture state yet
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(s.states.at(listAlias(1)).registration_path==2);}
    CHECK(late->Reset(allocator(2),fakePso())==S_OK);             // registered at Reset since
    CHECK(late->Reset(allocator(0),fakePso())==S_OK);             // witnessed allocator, list still unseen
    late->DrawInstanced(3,1,2,0);CHECK(w->lists[1].copies==0&&st1().drawRefusals[0]==1);
    frame(3,false);                                               // witnessed list, unseen allocator
    frame(1,false);                                               // fully witnessed
    const auto& a=st1().resetAcceptedByPath;const auto& r=st1().resetRefusedByPath;
    CHECK(r[4]==1&&r[2]==2&&r[0]==1&&a[0]==1&&!a[2]&&!a[4]);
    CHECK(st1().unseenLists.count==1&&st1().unseenAllocators.count==2&&!st1().unseenLists.overflow);
    CHECK(st1().resetRefusals[4]==3&&st1().resetRefusals[6]==3&&st1().resetsAccepted==1);
    CHECK(st1().registry.tombstones()==0);
    ticks(gpu_copy::step1::wait_ticks+5);
    if(!verdict("REFUSED;reason=no-qualified-matched-draw-within-600s")){dump();CHECK(false);}
    if(!line("gpu-copy-step1;event=creation",
        "reset-paths=cl:1/1,cl1:0/0,at-reset:0/2,unobserved:0/0,none:0/1;unseen-lists=1;unseen-allocators=2")){dump();CHECK(false);}
    CHECK(line("gpu-copy-step1-status;event=creation","CA=2:2:0:0:0:0:0:0"));
}
static void release_recreate() {
    // Final Release witnessed, then a new list at the SAME address: fresh
    // tokens, no tombstone; the new lifetime qualifies on its own Reset.
    boot();prepare();
    const auto route=reinterpret_cast<gpu_copy::owner::Route>(listAlias(0));
    const auto old=st1().registry.live_token(route,gpu_copy::owner::Kind::list);CHECK(old);
    w->lists[0].aliases[0].refs=1;
    CHECK(listAlias(0)->Release()==0);
    CHECK(!st1().registry.live_token(route,gpu_copy::owner::Kind::list));
    ID3D12GraphicsCommandList* list=nullptr;
    CHECK(SUCCEEDED(device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator(1),nullptr,IID_PPV_ARGS(&list))));
    CHECK(list==listAlias(0)&&list->Close()==S_OK);
    const auto reborn=st1().registry.live_token(route,gpu_copy::owner::Kind::list);CHECK(reborn&&reborn!=old);
    {const auto& c=st1().creates[0];CHECK(c.seen==2&&c.recorded==2&&!c.collision);}
    CHECK(st1().registry.tombstones()==0);
    w->lists[0].aliases[0].refs=1000;
    inserted();CHECK(st1().resetAcceptedByPath[0]>=1);
    // Allocator created again at a still-live route (Release not hooked): collision.
    w->allocatorsCreated=0;ID3D12CommandAllocator* a=nullptr;
    CHECK(SUCCEEDED(device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)))&&a==allocator(0));
    CHECK(st1().creates[2].seen==3&&st1().creates[2].collision==1&&st1().registry.tombstones()==1);
}
// ------------------------------------------------------------- J-3b4 early device hook
// The dxgi export the d3d12.dll proxy calls after a successful D3D12CreateDevice.
static unsigned notifyDevice(Object& d) {
    return AczVrNotifyDeviceCreated(reinterpret_cast<IUnknown*>(&d),&__uuidof(ID3D12Device));
}
static void* hookAt(int i){return w->deviceTable[i];}
static size_t experimentLines() { // the reserved step-1 partition; early-device lines go to startup
    size_t n=0;for(const auto& l:w->lines)if(l.rfind("gpu-copy-step1;event=early-device",0)!=0)++n;return n;
}
static void early_off() {
    // Step-1 fully off: the notification does nothing (no QI, no reference, no patch).
    build(false);acz::g_step1EarlyTest=true;
    auto refs=w->device.refs;auto queries=deviceQueries;
    CHECK(notifyDevice(w->device)==0&&w->device.refs==refs&&deviceQueries==queries);
    // Step-1 on but gpu_copy_early_device_hook off: still nothing.
    acz::g_step1Test=true;acz::g_step1EarlyTest=false;
    CHECK(notifyDevice(w->device)==0&&w->device.refs==refs&&deviceQueries==queries);
    CHECK(!acz::g_step1.load()&&w->lines.empty()&&acz::state().devices.empty());
    CHECK(hookAt(8)==reinterpret_cast<void*>(&deviceQueue)&&hookAt(9)==reinterpret_cast<void*>(&deviceAllocator));
    CHECK(hookAt(12)==reinterpret_cast<void*>(&deviceList)&&hookAt(75)==reinterpret_cast<void*>(&deviceQueue1));
    // The ordinary swap-chain discovery path is unchanged.
    bootBuilt(true);prepare();
    CHECK(!st1().early&&has("queue-witness=proxy-retained-reference;CreateCommandQueue=not-hooked"));
    CHECK(hookAt(8)==reinterpret_cast<void*>(&deviceQueue)&&hookAt(75)==reinterpret_cast<void*>(&deviceQueue1));
    // A device notified after discovery (key on) is ignored: no hook, reference balanced.
    acz::g_step1EarlyTest=true;refs=w->device.refs;
    CHECK(notifyDevice(w->device)==3&&w->device.refs==refs&&hookAt(8)==reinterpret_cast<void*>(&deviceQueue));
    CHECK(line("gpu-copy-step1;event=early-device;outcome=after-discovery-ignored","call=1"));
}
static void early_device() {
    device4Mode=1;device9Mode=1;build(true);acz::g_step1EarlyTest=true;
    const auto refs=w->device.refs;
    CHECK(notifyDevice(w->device)==1);
    CHECK(w->device.refs==refs+1); // the owner's own reference, retained for the process lifetime
    CHECK(acz::g_step1.load()&&st1().early&&st1().device==device()&&!st1().queue&&phase()==0);
    CHECK(hookAt(8)==reinterpret_cast<void*>(&acz::step1CreateQueue)&&hookAt(9)==reinterpret_cast<void*>(&acz::step1CreateAllocator));
    CHECK(hookAt(12)==reinterpret_cast<void*>(&acz::createList)&&hookAt(51)==reinterpret_cast<void*>(&acz::step1CreateList1));
    CHECK(hookAt(75)==reinterpret_cast<void*>(&acz::step1CreateQueue1));
    CHECK(st1().queueHook.load()==1&&st1().queue1Hook.load()==1&&st1().list1Hook.load()==1);
    if(!line("gpu-copy-step1;event=early-device;outcome=hooked","cl1-hook=1;cq-hook=1;cq1-hook=1")){dump();CHECK(false);}
    // Game creations BEFORE the first swap chain are now witnessed.
    D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q=nullptr;
    CHECK(SUCCEEDED(device()->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)))&&q==queue()&&queueCalls==1);
    ID3D12CommandQueue* q1=nullptr;
    CHECK(SUCCEEDED(reinterpret_cast<ID3D12Device9*>(device())->CreateCommandQueue1(&qd,__uuidof(ID3D12Device),
        IID_PPV_ARGS(&q1)))&&q1==reinterpret_cast<ID3D12CommandQueue*>(&w->otherQueue)&&queue1Calls==1);
    using gpu_copy::owner::Kind;
    CHECK(st1().registry.live_token(reinterpret_cast<gpu_copy::owner::Route>(queue()),Kind::queue));
    ID3D12CommandAllocator* a=nullptr;
    for(int i=0;i<2;++i)CHECK(SUCCEEDED(device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a))));
    ID3D12GraphicsCommandList* list=nullptr;
    CHECK(SUCCEEDED(device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator(0),nullptr,IID_PPV_ARGS(&list))));
    CHECK(list==listAlias(0)&&list->Close()==S_OK);
    CHECK(st1().creates[2].recorded==2&&st1().creates[0].recorded==1&&st1().queueCreates[0].recorded==1&&st1().queueCreates[1].recorded==1);
    // Swap-chain discovery joins the queue and re-runs the same install: idempotent.
    acz::captureDeviceAndQueue(reinterpret_cast<IUnknown*>(&w->queue));
    CHECK(acz::getHookedQueue()==queue()&&acz::getHookedDevice()==device());
    CHECK(acz::hookCaptureDevice(device())); // production runs this inside discovery
    {auto& s=acz::state();std::lock_guard lock(s.mutex);const auto& h=s.devices.at(w->deviceTable.data());
     CHECK(h.originals[8]==reinterpret_cast<void*>(&deviceQueue)&&h.originals[9]==reinterpret_cast<void*>(&deviceAllocator));
     CHECK(h.originals[12]==reinterpret_cast<void*>(&deviceList)&&h.originals[51]==reinterpret_cast<void*>(&deviceList1));
     CHECK(h.originals[75]==reinterpret_cast<void*>(&deviceQueue1));}
    CHECK(hookAt(8)==reinterpret_cast<void*>(&acz::step1CreateQueue)&&hookAt(75)==reinterpret_cast<void*>(&acz::step1CreateQueue1));
    CHECK(queueCalls==1&&list1Calls==0&&w->device.refs==refs+2); // + HookState's discovery reference
    CHECK(st1().queue==queue()&&phase()==int(acz::Step1Phase::preparing)&&st1().registry.tombstones()==0);
    if(!line("gpu-copy-step1;event=early-device-discovery;match=1","queue-witness=creation;CQ=1:1:0:0:0;CQ1=1:1:0:0:0")){dump();CHECK(false);}
    prepare();
    CHECK(has("queue-witness=creation;CreateCommandQueue=hooked")&&has("+gpu_copy_early_device_hook"));
    inserted();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PASS")){dump();CHECK(false);}
    if(!line("gpu-copy-step1;event=creation","witness-start=early-device;device-hook=CL:12,CA:9,CQ:8,CQ1:75;CL=1:1:0:0:0:0:0:0;")){dump();CHECK(false);}
    CHECK(line("gpu-copy-step1;event=creation","CA=2:2:0:0:0:0:0:0;cl1-hook=1"));
    CHECK(line("gpu-copy-step1;event=creation",";CQ=1:1:0:0;CQ1=1:1:0:0;queue-witness=creation"));
    CHECK(experimentLines()<=diagnostics::step1_records); // early lines live in the startup partition
}
static void early_repeat() {
    build(true);acz::g_step1EarlyTest=true;
    const auto refs=w->device.refs,otherRefs=w->otherDevice.refs;
    CHECK(notifyDevice(w->device)==1&&notifyDevice(w->device)==3);
    CHECK(w->device.refs==refs+1); // the repeat's QI reference is released
    CHECK(line("gpu-copy-step1;event=early-device;outcome=repeated","call=2"));
    CHECK(notifyDevice(w->otherDevice)==3&&w->otherDevice.refs==otherRefs);
    CHECK(line("gpu-copy-step1;event=early-device;outcome=other-device-ignored","call=3"));
    using gpu_copy::owner::Kind;
    CHECK(st1().registry.live_token(reinterpret_cast<gpu_copy::owner::Route>(device()),Kind::device)&&st1().registry.tombstones()==0);
    CHECK(st1().device==device()&&queueCalls==0);
    for(int i=0;i<4;++i)(void)notifyDevice(w->device); // bounded: at most four early-device lines
    size_t n=0;for(const auto& l:w->lines)if(l.rfind("gpu-copy-step1;event=early-device;",0)==0)++n;
    CHECK(n==4&&w->device.refs==refs+1);
}
static void early_mismatch() {
    // The early owner's device is not the swap-chain device: one refusal, nothing borrowed.
    build(true);acz::g_step1EarlyTest=true;
    CHECK(notifyDevice(w->otherDevice)==1&&st1().device==reinterpret_cast<ID3D12Device*>(&w->otherDevice));
    acz::captureDeviceAndQueue(reinterpret_cast<IUnknown*>(&w->queue));
    if(!verdict("REFUSED;reason=early-device-discovery-mismatch")){dump();CHECK(false);}
    CHECK(line("gpu-copy-step1;event=early-device-discovery;match=0","queue-witness=none"));
    CHECK(phase()==int(acz::Step1Phase::done)&&!st1().queue&&w->resourcesCreated==0&&w->fenceCreates==0);
    CHECK(verdicts()==1&&experimentLines()<=diagnostics::step1_records);
}
static void early_distinct() {
    // ID3D12Device9 on a distinct object/table: slot 75 untouched (J-056/D-86 rule), slot 8 still witnessed.
    device9Mode=2;build(true);acz::g_step1EarlyTest=true;
    CHECK(notifyDevice(w->device)==1);
    CHECK(st1().queue1Hook.load()==2&&hookAt(75)==reinterpret_cast<void*>(&deviceQueue1));
    CHECK(device9Table[75]==reinterpret_cast<void*>(&deviceQueue1)&&device9Object.refs==1);
    CHECK(hookAt(8)==reinterpret_cast<void*>(&acz::step1CreateQueue)&&st1().list1Hook.load()==3); // no Device4 here
    const auto q=deviceQueries;CHECK(acz::hookCaptureDevice(device())&&deviceQueries==q); // decided once
}
static void early_binding() {
    // J-3b5: the early path installs creation witnesses only; the binding
    // observer's device QI/Release/resource hooks wait for swap-chain discovery.
    build(true);acz::g_step1EarlyTest=true;
    CHECK(notifyDevice(w->device)==1);
    CHECK(hookAt(12)==reinterpret_cast<void*>(&acz::createList)&&hookAt(9)==reinterpret_cast<void*>(&acz::step1CreateAllocator));
    CHECK(hookAt(0)==reinterpret_cast<void*>(&deviceQuery)&&hookAt(2)==reinterpret_cast<void*>(&release));
    CHECK(hookAt(27)==reinterpret_cast<void*>(&deviceResource));
    // DXGI's pre-discovery private-IID query no longer reaches the observer.
    const auto refs=w->device.refs;void* p=nullptr;
    CHECK(SUCCEEDED(device()->QueryInterface(dxgiPrivateIid,&p))&&p==device());w->device.refs=refs;
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(s.nativeBindings.enabled());}
    // The live-run-4 mechanism: with the binding hooks installed (what the J-3b4
    // early path did, and what discovery still does) the same query fails closed.
    CHECK(acz::hookNativeBindingDevice(device()));
    CHECK(hookAt(0)==reinterpret_cast<void*>(&acz::queryBindingDevice));
    CHECK(SUCCEEDED(device()->QueryInterface(dxgiPrivateIid,&p))&&p==device());w->device.refs=refs;
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(!s.nativeBindings.enabled());
     CHECK(std::string_view(s.nativeBindings.disable_reason())=="binding device interface unsupported: unknown");}
}
// ------------------------------------------------------------- J-3b6 step 2
// Game buffers are fakes 1..3 (never created through the device): IB in fake 1,
// VB and t10 SrtData in fake 2 (the live run shares one UPLOAD resource for
// both, L3f), fake 3 is an alternative VB resource. Their creation-time facts
// (L3 resource record, J-142 upload pin) and the list's setter-tracked views
// are injected into HookState exactly where the production setters and
// creation hooks write them; the REAL step-2 Draw seam then resolves them.
struct Step2Shape {
    unsigned ibStride=2;uint64_t vbBytes=512;int vbRes=2;
    D3D12_HEAP_TYPE vbHeap=D3D12_HEAP_TYPE_UPLOAD,ibHeap=D3D12_HEAP_TYPE_UPLOAD;
    bool cpu=true,cpuMismatch=false,t10Table=false;uint16_t lastIndex=6;
};
static Step2Shape shape2;
constexpr uint64_t ibOff=0x510,vbOff=0x1b80,srtOff=0x9e0,gameWidth=16384;
static uint64_t gameVa(int res){return 0x100000000ull*uint64_t(res);}
static uint64_t gameAddr(int res){return reinterpret_cast<uintptr_t>(&w->resources[res].object);}
// J-3b9: step-3 fixtures register the game buffers with the native binding observer
// and use ITS lifetime generation (the mapped-source rules correlate them).
static std::array<uint64_t,24> nativeLife{};static bool useNativeLife=false;
static uint64_t gameLife(int res){return nativeLife[size_t(res)]?nativeLife[size_t(res)]:0x40+uint64_t(res);}
static void* fakeMapTable[16]{}; // a resource vtable whose Map/Unmap slots hold the proxy's l3Map/l3Unmap
static std::array<std::array<uint8_t,16384>,4> gameBefore{};
static Resource& readback2(){return w->resources[0];}
static void gameResource(int res,D3D12_HEAP_TYPE heap) {
    auto& s=acz::state();std::lock_guard lock(s.mutex);auto& o=acz::l3_locked();CHECK(o.enabled);
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=gameWidth;d.Height=1;d.DepthOrArraySize=1;
    d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES p{};p.Type=heap;
    o.create(gameAddr(res),&d,1,&p);auto* r=o.resources.get(gameAddr(res));CHECK(r&&r->serial);r->gpu_va=gameVa(res);
    // pinUploadCreation pins committed UPLOAD buffers only (J-142).
    if(useNativeLife) {
        s.nativeBindings.resource(binding::ResourceEvent{gameAddr(res),gameVa(res),gameWidth,1,false});
        const auto t=s.nativeBindings.resource_token(gameAddr(res));CHECK(t.id==gameAddr(res)&&t.generation);nativeLife[size_t(res)]=t.generation;
    }
    if(heap==D3D12_HEAP_TYPE_UPLOAD) {
        const auto slot=s.uploadPins.reserve(gameAddr(res),r->serial,gameWidth);CHECK(slot<s.uploadPins.capacity);
        ++w->resources[res].object.refs;s.uploadPins.finish(slot,reinterpret_cast<ID3D12Resource*>(&w->resources[res].object));
        s.uploadPins.bind(gameAddr(res),r->serial,gameLife(res));
    }
}
static void bootStep2() {
    build(true);acz::g_step2Test=true;acz::g_l3Test=true;
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     s.pipelines[reinterpret_cast<ID3D12PipelineState*>(&w->pso)].drawcpt_ia_layout=true;}
    bootBuilt(true);
    CHECK(st1().step2);
    // Deterministic game bytes: 9 indices (max lastIndex), 8 vertices x 64 B, SrtData.
    const uint16_t idx[9]={0,1,2,2,1,3,4,5,shape2.lastIndex};
    std::memcpy(w->resources[1].memory.data()+ibOff,idx,sizeof idx);
    for(size_t i=0;i<8192;++i)w->resources[shape2.vbRes].memory[vbOff+i]=uint8_t(i*7+1);
    for(size_t i=0;i<416;++i)w->resources[2].memory[srtOff+i]=uint8_t(i*13+5);
    gameResource(1,shape2.ibHeap);gameResource(2,shape2.vbRes==2?shape2.vbHeap:D3D12_HEAP_TYPE_UPLOAD);
    if(shape2.vbRes==3)gameResource(3,shape2.vbHeap);
    for(int i=1;i<4;++i)gameBefore[i]=w->resources[i].memory;
}
static void prepare2() {
    CHECK(acz::step1Tick());CHECK(phase()==int(acz::Step1Phase::ready));
    CHECK(w->resourcesCreated==1&&readback2().heap==D3D12_HEAP_TYPE_READBACK&&readback2().initial==D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(readback2().width==gpu_copy::step2::readback_bytes&&readback2().maps==0&&w->fenceCreates==1);
    CHECK(has("gpu-copy-step2;event=pins;")&&has("gpu_copy_step2_experiment+gpu_copy_early_device_hook"));
    CHECK(has("gpu-copy-step2;event=prepare;mode=step2;")&&has("upload=none;pattern=none")&&has("bound=1"));
}
// The J-131 CPU snapshot as finishCpuSnapshot leaves it after a stable sample.
static void cpuSample() {
    auto& s=acz::state();std::lock_guard lock(s.mutex);auto& c=s.cpuSnapshot;
    auto& q=c.completed;q=diagnostics::CpuUploadSnapshot::Request{};q.accepted=true;q.list=0x4b;q.generation=0x2768;q.draw=1;
    const uint64_t offs[3]={srtOff,vbOff,ibOff},bytes[3]={416,shape2.vbBytes>2048-434?512:shape2.vbBytes,18};
    const int res[3]={2,shape2.vbRes,1};size_t at=0;
    for(int i=0;i<3;++i) {
        auto& x=q.ranges[i];x.resource=gameAddr(res[i]);x.lifetime=gameLife(res[i]);x.offset=offs[i];x.bytes=bytes[i];x.pinned=true;x.upload=true;
        std::memcpy(c.first.data()+at,w->resources[res[i]].memory.data()+offs[i],size_t(bytes[i]));
        c.hashes[size_t(i)]=diagnostics::CpuUploadSnapshot::hash(c.first.data()+at,size_t(bytes[i]));at+=size_t(bytes[i]);
    }
    if(shape2.cpuMismatch)c.first[416+5]^=0x40; // VB byte 5 differs from the GPU copy
    c.stable=true;c.attempts=1;c.reason="snapshot-stable";c.sampled_ms=GetTickCount64();
}
// The game's own setters for the selected draw (after its Reset, before Draw).
static void bindRoles() {
    auto& s=acz::state();std::lock_guard lock(s.mutex);auto& ls=s.states.at(listAlias(0));auto& b=ls.bindings;
    b.index=capture::BufferView{gameVa(1)+ibOff,0,18,shape2.ibStride};
    b.vertices[0]=capture::BufferView{gameVa(shape2.vbRes)+vbOff,0,shape2.vbBytes,64};
    b.topology=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;b.pipeline.strip_cut=0;
    auto* l=acz::l3_list_locked(ls);CHECK(l);
    l->sources[33]={gameAddr(1),gameVa(1)+ibOff,18};
    l->sources[1]={gameAddr(shape2.vbRes),gameVa(shape2.vbRes)+vbOff,shape2.vbBytes};
    l->sources[0]={shape2.t10Table?0ull:gameAddr(2),shape2.t10Table?0ull:gameVa(2)+srtOff,shape2.t10Table?0ull:416ull};
    l->t10_parameter=3;l->t10_table=shape2.t10Table;
}
static unsigned inserted2() {
    if(shape2.cpu)cpuSample();
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);
    bindRoles();
    const auto draws=w->lists[0].draws;const auto before=allocations;traceAllocations=true;
    l0->DrawIndexedInstanced(9,1,0,0,0);traceAllocations=false;
    CHECK(allocations==before); // no heap allocation on the inserting Draw path
    auto& l=w->lists[0];CHECK(l.draws==draws+1&&l.events.back()=="draw-indexed");
    const unsigned copies=st1().plan.copyable;
    CHECK(l.copies==copies&&l.recording.size()==copies);
    for(unsigned i=0;i<copies;++i)CHECK(l.events[l.events.size()-2-i]=="copy"); // all copies precede the Draw
    for(const auto& c:l.recording)CHECK(c.dst==&readback2().object&&c.src!=&readback2().object&&c.dstOffset%4096==0);
    return copies;
}
static void finish2(bool executeIt=true) {
    CHECK(listAlias(0)->Close()==S_OK);ticks(1);
    if(executeIt)execute({cl(0)});
    advance();frame(1,false);ticks(10);
}
static void gameUntouched() {
    for(int i=1;i<4;++i)CHECK(w->resources[i].memory==gameBefore[i]&&!w->resources[i].maps&&!w->resources[i].unmaps);
}
static void step2Lines() {
    size_t n=0;
    for(const auto& l:w->lines) {
        if(l.size()>440){std::fprintf(stderr,"overlong (%zu): %s\n",l.size(),l.c_str());CHECK(false);}
        CHECK(l.rfind("gpu-copy-step2",0)==0); // every shared-machinery line is relabelled
        if(l.rfind("gpu-copy-step2;event=early-device",0)!=0&&l.rfind("gpu-copy-step2-status;",0)!=0)++n;
    }
    if(n>diagnostics::step1_records){dump();CHECK(false);}
}
static void retained2() {
    CHECK(readback2().object.refs==1&&w->fence.refs==1);
    const auto s=gpu_copy::diagnostic_session().snapshot();
    CHECK(s.private_held&&s.state==gpu_copy::State::quarantined&&!s.retired);
}
static void step2_success() {
    bootStep2();prepare2();
    CHECK(inserted2()==3);
    const auto& rec=w->lists[0].recording;
    CHECK(rec[0].src==&w->resources[1].object&&rec[0].srcOffset==ibOff&&rec[0].bytes==18&&rec[0].dstOffset==0);
    CHECK(rec[1].src==&w->resources[2].object&&rec[1].srcOffset==vbOff&&rec[1].bytes==512&&rec[1].dstOffset==4096);
    CHECK(rec[2].src==&w->resources[2].object&&rec[2].srcOffset==srtOff&&rec[2].bytes==416&&rec[2].dstOffset==8192);
    CHECK(listAlias(0)->Close()==S_OK);ticks(1);execute({cl(0)});
    CHECK(acz::step1Tick()&&readback2().maps==0); // pending: never Map before completion
    advance();ticks(3);CHECK(readback2().maps==0); // completed, closure not yet observed
    frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PASS;reason=all-roles-read")){dump();CHECK(false);}
    CHECK(readback2().maps==3&&readback2().unmaps==3&&readback2().lastWrittenBegin==0&&readback2().lastWrittenEnd==0);
    CHECK(line("event=role;role=ib;status=copy;reason=ok","heap=UPLOAD;kind=1"));
    CHECK(line("event=role;role=srt;status=copy","read-mask=0-175,192-303,352-367,400-415"));
    CHECK(line("event=role;role=vb;status=copy","state-rule=upload-generic-read-has-copy-source;pin=held"));
    CHECK(line("event=role-data;role=ib;bytes=18;hex18=000001000200020001000300040005000600","cpu=equal;cpu-diff=-1"));
    CHECK(line("event=role-data;role=vb;bytes=512;hex64=01080f","cpu=equal"));
    CHECK(line("event=role-data;role=srt;bytes=416;","cpu=equal;cpu-diff=-1;cpu-fnv="));
    CHECK(line("event=index;count=9;stride=2;start=0;base=0;indices=0,1,2,2,1,3,4,5,6","vertices=8;"));
    CHECK(line("event=index;","range=verified:index-vertex-range;draw-use-unproven"));
    CHECK(line("STEP2_RESULT;ib=read;vb=read;srt=read;copied=3/3;read=3;index=verified","cpu=ib:equal,vb:equal,srt:equal"));
    CHECK(line("EXPERIMENT_RESULT=PASS","label=gpu-copy-completed?yes;draw-use-unproven;writer-exclusion-unproven;game-buffers-read-only"));
    CHECK(has("copy=game-read-only,copies:3")&&has("order=CopyBufferRegion-then-original-Draw"));
    CHECK(has("resources=released")&&readback2().object.refs==0&&gpu_copy::diagnostic_session().snapshot().retired);
    // The SHA-256 of the IB bytes is the hash of exactly the 18 copied bytes.
    {char sha[65];const uint16_t idx[9]={0,1,2,2,1,3,4,5,6};CHECK(acz::step1Sha256(idx,18,sha));
     CHECK(line("event=role-data;role=ib;",sha));}
    gameUntouched();
    // One attempt only.
    frame(0,true,true);CHECK(w->lists[0].copies==3&&verdicts()==1&&!acz::step1Tick());
    step2Lines();
}
static void step2_default_heap() {
    // VB in its own DEFAULT-heap resource: state unknown, so that role is refused
    // (no barrier is ever added); IB and t10 are still copied and read.
    shape2.vbRes=3;shape2.vbHeap=D3D12_HEAP_TYPE_DEFAULT;shape2.cpu=false;
    bootStep2();prepare2();CHECK(inserted2()==2);finish2();
    if(!verdict("EXPERIMENT_RESULT=REFUSED;reason=some-roles-refused")){dump();CHECK(false);}
    CHECK(line("event=role;role=vb;status=refused;reason=default-heap-state-unknown","heap=DEFAULT;kind=1"));
    CHECK(line("event=role;role=vb;","state-rule=default-state-untracked;pin=none"));
    CHECK(line("STEP2_RESULT;ib=read;vb=refused:default-heap-state-unknown;srt=read;copied=2/3;read=2;index=verified","cpu=ib:absent"));
    CHECK(line("EXPERIMENT_RESULT=REFUSED","gpu-copy-completed?yes")); // clean transaction, not quarantined
    CHECK(readback2().maps==2&&readback2().object.refs==0&&gpu_copy::diagnostic_session().snapshot().retired);
    gameUntouched();step2Lines();
}
static void step2_shape_changed() {
    shape2.ibStride=4; // R32 index view: the cited 9 x 16-bit shape changed
    bootStep2();prepare2();CHECK(inserted2()==2);finish2();
    if(!verdict("REFUSED;reason=some-roles-refused")){dump();CHECK(false);}
    CHECK(line("event=role;role=ib;status=refused;reason=ib-shape-changed","stride=4"));
    CHECK(line("STEP2_RESULT;ib=refused:ib-shape-changed;vb=read;srt=read","index=not-copied"));
    CHECK(!has("event=index;"));gameUntouched();step2Lines();
}
static void step2_over_cap() {
    shape2.vbBytes=8192; // above the 4096-byte region cap
    bootStep2();prepare2();CHECK(inserted2()==2);finish2();
    if(!verdict("REFUSED;reason=some-roles-refused")){dump();CHECK(false);}
    CHECK(line("event=role;role=vb;status=refused;reason=over-cap","view-bytes=2000"));
    CHECK(line("event=index;","vertices=128;")); // VB metadata still bounds the index check
    gameUntouched();step2Lines();
}
static void step2_cpu_mismatch() {
    shape2.cpuMismatch=true; // the CPU sample differs: logged as a mismatch, never a failure
    bootStep2();prepare2();CHECK(inserted2()==3);finish2();
    if(!verdict("EXPERIMENT_RESULT=PASS")){dump();CHECK(false);}
    CHECK(line("event=role-data;role=vb;","cpu=mismatch;cpu-diff=5"));
    CHECK(line("STEP2_RESULT;","cpu=ib:equal,vb:mismatch,srt:equal"));
    step2Lines();
}
static void step2_no_cpu() {
    shape2.cpu=false;
    bootStep2();prepare2();CHECK(inserted2()==3);finish2();
    if(!verdict("EXPERIMENT_RESULT=PASS")){dump();CHECK(false);}
    CHECK(line("STEP2_RESULT;","cpu=ib:absent,vb:absent,srt:absent;cpu-sample=not-evaluated,attempts:0"));
    step2Lines();
}
static void step2_all_refused() {
    // Every role refused: nothing is armed, recorded or submitted.
    shape2.ibHeap=D3D12_HEAP_TYPE_DEFAULT;shape2.vbHeap=D3D12_HEAP_TYPE_DEFAULT;shape2.t10Table=true;shape2.cpu=false;
    bootStep2();prepare2();CHECK(inserted2()==0);
    CHECK(listAlias(0)->Close()==S_OK);ticks(2);
    if(!verdict("EXPERIMENT_RESULT=REFUSED;reason=no-copyable-role")){dump();CHECK(false);}
    CHECK(line("event=role;role=srt;status=refused;reason=t10-descriptor-table-span-unknown",""));
    CHECK(line("STEP2_RESULT;ib=refused:default-heap-state-unknown;vb=refused:default-heap-state-unknown;srt=refused:","copied=0/3"));
    CHECK(line("EXPERIMENT_RESULT=REFUSED","gpu-copy-completed?no"));
    execute({cl(0)});CHECK(w->signals==0&&readback2().maps==0);
    gameUntouched();step2Lines();
}
static void step2_index_range() {
    shape2.lastIndex=9; // index 9 >= 8 vertices: unexpected range stops the experiment
    bootStep2();prepare2();CHECK(inserted2()==3);finish2();
    if(!verdict("FAIL;reason=index-range-unexpected")){dump();CHECK(false);}
    CHECK(line("event=index;","range=failed:index-vertex-range"));
    CHECK(line("STEP2_RESULT;ib=read","index=failed:index-vertex-range"));
    retained2();gameUntouched();step2Lines();
}
static void step2_signal_fail() {
    bootStep2();prepare2();CHECK(inserted2()==3);
    CHECK(listAlias(0)->Close()==S_OK);w->signalHr=DXGI_ERROR_DEVICE_REMOVED;execute({cl(0)});
    advance();frame(1,false);ticks(10);
    if(!verdict("FAIL;reason=signal-failed")){dump();CHECK(false);}
    CHECK(readback2().maps==0&&line("STEP2_RESULT;ib=recorded-unread","copied=3/3;read=0"));
    retained2();gameUntouched();step2Lines();
}
static void step2_duplicate() {
    bootStep2();prepare2();CHECK(inserted2()==3);CHECK(listAlias(0)->Close()==S_OK);
    execute({cl(0),cl(0)});CHECK(w->signals==0);advance();frame(1,false);ticks(5);
    if(!verdict("FAIL;reason=duplicate-in-batch")){dump();CHECK(false);}
    CHECK(readback2().maps==0);retained2();gameUntouched();step2Lines();
}
static void step2_no_qualified() {
    bootStep2();prepare2();frame(0,false);ticks(gpu_copy::step1::wait_ticks+5);
    if(!verdict("REFUSED;reason=no-qualified-matched-draw-within-600s")){dump();CHECK(false);}
    CHECK(has("gpu-copy-step2-status;event=waiting")&&has("resources=released-never-recorded")&&readback2().object.refs==0);
    step2Lines();
}
static void step2_rules() {
    // The pure per-role rule, independent of the hook fixture.
    using namespace gpu_copy::step2;
    Facts f;f.address=0x1000;f.va=0x20010;f.view_bytes=18;f.record=f.description=f.buffer=true;f.serial=4;f.width=0x400000;
    f.gpu_va=0x20000;f.kind=kind_committed;f.heap=heap_upload;f.pin=f.pin_finished=true;f.pin_creation=4;f.pin_held=0x1000;
    RolePlan p;evaluate(f,role_ib,0,18,p);CHECK(p.copy&&!p.reason&&p.offset==0x10&&p.dest_offset==0);
    auto refused=[&](Facts g,const char* why,uint64_t rel=0,uint64_t span=18) {
        RolePlan q;evaluate(g,role_ib,rel,span,q);if(q.copy||std::string_view(q.reason)!=why){std::fprintf(stderr,"%s\n",q.reason);CHECK(false);}
    };
    {auto g=f;g.heap=heap_default;refused(g,"default-heap-state-unknown");}
    {auto g=f;g.heap=heap_readback;refused(g,"readback-heap-copy-dest-only");}
    {auto g=f;g.heap=heap_custom;refused(g,"custom-heap-state-unknown");}
    {auto g=f;g.heap=0;refused(g,"heap-unknown");}
    {auto g=f;g.kind=kind_placed;refused(g,"placed-resource-unsupported");}
    {auto g=f;g.kind=kind_reserved;refused(g,"reserved-resource-unsupported");}
    {auto g=f;g.retired=true;refused(g,"resource-retired");}
    {auto g=f;g.record=false;refused(g,"no-creation-record");}
    {auto g=f;g.address=0;refused(g,"resource-unresolved");}
    {auto g=f;g.pin=false;refused(g,"no-ownership-pin");}
    {auto g=f;g.pin_creation=5;refused(g,"pin-generation-mismatch");}
    {auto g=f;g.pin_finished=false;refused(g,"pin-pending");}
    {auto g=f;g.pin_held=0;refused(g,"pin-not-held");}
    {auto g=f;g.pin_held=0x2000;refused(g,"pin-pointer-mismatch");}
    {auto g=f;refused(g,"span-exceeds-view",2,18);}
    {auto g=f;g.view_bytes=8192;refused(g,"over-cap",0,8192);}
    {auto g=f;g.gpu_va=0x30000;refused(g,"gpu-va-unknown");}
    {auto g=f;g.width=0x18;refused(g,"span-exceeds-resource");}
    {auto g=f;g.shape="ib-shape-changed";refused(g,"ib-shape-changed");}
    CHECK(!copy_source_rule(heap_upload)&&std::string_view(copy_source_rule(heap_default))=="default-heap-state-unknown");
}
// ------------------------------------------------------------- J-3b7 step 3
// SYNTHETIC: a fake cockpit (two quads at 30 units in front of a known camera)
// whose SrtData is built with cockpit_math from a known lens and pose. The fake
// "GPU" replays recorded ops: clears, barriers (state-checked), draws into the
// proxy RT through a tiny rasteriser that positions vertices with the bound
// blob's +0 matrix (as the GS does) and checks +64 agrees, and texture copies
// into the readback footprints. Game draws (game RTV) are not rasterised.
struct Step3Shape {
    float vpW=256,vpH=144;UINT samples=1;bool omSet=true,omGap=false,t10=true,snapshot=true,mapped=true;unsigned candidates=1;
    DXGI_FORMAT rtv=DXGI_FORMAT_R8G8B8A8_UNORM,dsv=DXGI_FORMAT_D32_FLOAT;D3D12_COMPARISON_FUNC func=D3D12_COMPARISON_FUNC_GREATER;
};
static Step3Shape shape3;
constexpr SIZE_T gameRtvHandle=0x9000,gameDsvHandle=0xa000;
constexpr double sceneDepth=30.0;
static UINT64 gameT10(){return gameVa(2)+srtOff;}
static const acz::Vec3 sceneEye{10.0,5.0,-3.0};
static void sceneMatrices(acz::Mat4& m0,acz::Mat4& m64,acz::Mat4& m256) {
    const acz::Pose pose{sceneEye,{0,0,0,1}};
    const acz::FovAngles fov{std::atan(-0.6152),std::atan(0.6152),std::atan(0.3460),std::atan(-0.3460)};
    const auto vp=acz::multiply(acz::viewMatrixFromPose(pose),acz::projectionFromFov(fov,2.0,1000.0));
    acz::Mat4 scale=acz::identity();scale[0]=scale[5]=scale[10]=16.0;
    m256=acz::multiply(scale,acz::translation(sceneEye[0],sceneEye[1],sceneEye[2]-sceneDepth));
    m64=acz::multiply(m256,vp);
    m0=acz::multiply(m64,acz::ps2ViewportMatrix(acz::PS2_SCALE_X,acz::PS2_SCALE_Y,acz::PS2_CENTRE,{1.0,0.0}));
}
static void putMatrix(uint8_t* p,const acz::Mat4& m){const auto f=acz::toFloat32(m);std::memcpy(p,f.data(),64);}
// SrtData +400 as the GS reads it: ndc = 2*(ps2/w - offset)/size (E11 GS lines 263-299).
static void putParams(uint8_t* srt) {
    const float p[4]={float(2*acz::PS2_SCALE_X),float(2*acz::PS2_SCALE_Y),float(acz::PS2_CENTRE),float(acz::PS2_CENTRE)};std::memcpy(srt+400,p,16);
}
static std::array<uint8_t,416> sceneBlob{};
static void writeScene() {
    acz::Mat4 m0,m64,m256;sceneMatrices(m0,m64,m256);
    auto* srt=w->resources[2].data()+srtOff;
    for(size_t i=0;i<416;++i)srt[i]=uint8_t(i*13+5); // +128/+192/+320.. bytes: arbitrary, must survive unchanged
    putMatrix(srt,m0);putMatrix(srt+64,m64);putMatrix(srt+256,m256);
    putParams(srt);
    std::memcpy(sceneBlob.data(),srt,416);
    const float quads[8][2]={{-0.40f,-0.20f},{-0.40f,0.20f},{-0.05f,-0.20f},{-0.05f,0.20f},{0.05f,-0.20f},{0.05f,0.20f},{0.40f,-0.20f},{0.40f,0.20f}};
    auto* vb=w->resources[2].data()+vbOff;std::memset(vb,0,512);
    for(int i=0;i<8;++i){const float v[4]={quads[i][0],quads[i][1],0.0f,1.0f};std::memcpy(vb+64*i,v,16);}
    const uint16_t idx[9]={0,1,2,3,0xffff,4,5,6,7};std::memcpy(w->resources[1].data()+ibOff,idx,sizeof idx);
}
static Resource* viewResource(SIZE_T h){for(unsigned i=0;i<w->viewCount;++i)if(w->views[i].first==h)return w->views[i].second?&res(w->views[i].second):nullptr;return nullptr;}
static const uint8_t* blobAt(UINT64 va) {
    if(va>=0x7000000000ull){auto& r=w->resources[size_t((va-0x7000000000ull)>>24)];return r.data()+(va&0xffffff);}
    for(int r=1;r<4;++r)if(va>=gameVa(r)&&va<gameVa(r)+16384)return w->resources[r].data()+(va-gameVa(r));
    CHECK(false);return nullptr;
}
static void rasterise(Resource& rt,const Op& d) {
    ++w->rasterised;
    const uint8_t* blob=blobAt(d.boundT10);float m0[16],m64[16];std::memcpy(m0,blob,64);std::memcpy(m64,blob+64,64);
    const uint8_t* vb=w->resources[2].data()+vbOff;uint16_t idx[9];std::memcpy(idx,w->resources[1].data()+ibOff,sizeof idx);
    const double W=double(rt.width),H=double(rt.height);
    auto project=[&](int i,double& x,double& y) {
        float v[4];std::memcpy(v,vb+64*i,16);v[3]=1.0f; // the GS uses ATTRIBUTE0.xyz with w=1
        double s[4]{},c[4]{};
        for(int k=0;k<4;++k)for(int j=0;j<4;++j){s[k]+=double(v[j])*m0[j*4+k];c[k]+=double(v[j])*m64[j*4+k];}
        const double nx=(s[0]/s[3]-acz::PS2_CENTRE)/acz::PS2_SCALE_X,ny=(s[1]/s[3]-acz::PS2_CENTRE)/acz::PS2_SCALE_Y;
        float p400[2];std::memcpy(p400,blob+400,8);
        const bool worldRecord=p400[0]==512.0f&&p400[1]==-448.0f; // DrawACM_v records: A only (C is never read; the replay leaves +64 stale)
        if(!worldRecord&&(std::fabs(nx-c[0]/c[3])>1e-3||std::fabs(ny-c[1]/c[3])>1e-3))++w->inconsistent; // +0 and +64 disagree
        x=(nx+1.0)*0.5*W;y=(1.0-ny)*0.5*H;
    };
    std::vector<int> run;
    auto clipOf=[&](int i,double (&c)[4]){float v[4];std::memcpy(v,vb+64*i,16);v[3]=1.0f;
        for(int k=0;k<4;++k){c[k]=0;for(int j=0;j<4;++j)c[k]+=double(v[j])*m64[j*4+k];}};
    auto fill=[&](int a,int b,int c) {
        double ca[4],cb[4],cc[4];clipOf(a,ca);clipOf(b,cb);clipOf(c,cc);
        std::array<double,4> A{ca[0],ca[1],ca[2],ca[3]},B{cb[0],cb[1],cb[2],cb[3]},C{cc[0],cc[1],cc[2],cc[3]};
        if(gpu_copy::step3::outcode(A)&gpu_copy::step3::outcode(B)&gpu_copy::step3::outcode(C))return; // GS trivial reject
        if(!(ca[3]>0&&cb[3]>0&&cc[3]>0))return; // the fake has no clipper
        double x[3],y[3];project(a,x[0],y[0]);project(b,x[1],y[1]);project(c,x[2],y[2]);
        auto edge=[](double ax,double ay,double bx,double by,double px,double py){return (bx-ax)*(py-ay)-(by-ay)*(px-ax);};
        for(int py=0;py<int(rt.height);++py)for(int px=0;px<int(rt.width);++px) {
            const double cx=px+0.5,cy=py+0.5;
            const double e0=edge(x[0],y[0],x[1],y[1],cx,cy),e1=edge(x[1],y[1],x[2],y[2],cx,cy),e2=edge(x[2],y[2],x[0],y[0],cx,cy);
            if((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)) {
                if(rt.format==DXGI_FORMAT_R16G16B16A16_FLOAT) { // halves 0.75, 0.5, 0.25, 1.0
                    const uint16_t hv[4]={0x3a00,0x3800,0x3400,0x3c00};std::memcpy(rt.data()+(size_t(py)*rt.width+px)*8,hv,8);
                } else {auto* p=rt.data()+(size_t(py)*rt.width+px)*4;p[0]=200;p[1]=100;p[2]=50;p[3]=255;}
            }
        }
    };
    auto flush=[&]{for(size_t i=2;i<run.size();++i)fill(run[i-2],run[i-1],run[i]);run.clear();};
    for(int i=0;i<9;++i){if(idx[i]==0xffff)flush();else run.push_back(idx[i]);}
    flush();
}
static void gpuOps() {
    Object* predObj=nullptr;UINT64 predOff=0;UINT predOp=0; // the list's active predication (J-effects-layer)
    for(const auto& o:w->gpuOps) {
        switch(o.t) {
        case Op::pred:predObj=o.res;predOff=o.predOff;predOp=o.predOp;
            if(predObj)CHECK(o.predOp==UINT(D3D12_PREDICATION_OP_EQUAL_ZERO)||o.predOp==UINT(D3D12_PREDICATION_OP_NOT_EQUAL_ZERO));break;
        case Op::clear_rtv:CHECK(!predObj||w->gamePredicate);if(auto* r=viewResource(o.rtv[0])){CHECK(r->state==D3D12_RESOURCE_STATE_RENDER_TARGET);std::fill(r->big.begin(),r->big.end(),0);}break;
        case Op::clear_dsv:CHECK(!predObj||w->gamePredicate);if(auto* r=viewResource(o.dsv))CHECK(r->state==D3D12_RESOURCE_STATE_DEPTH_WRITE);break;
        case Op::barrier:{auto& r=res(o.res);CHECK(r.state==D3D12_RESOURCE_STATES(o.before));r.state=D3D12_RESOURCE_STATES(o.after);break;}
        case Op::draw:
            if(predObj&&w->gamePredicate&&predObj==w->gamePredObj) { // a GAME predicate (Astra P1-2 fixture): the fake executes every draw under it
                if(originOf(o.boundRtv)==gameRtvHandle)++w->gamePredDraws;
            } else if(predObj) { // J-effects-layer: only the proxy's own replay draws are ever predicated; the game's draws never
                CHECK(originOf(o.boundRtv)!=gameRtvHandle&&o.boundRtv!=hudOutputHandleValue());
                uint64_t v=0;std::memcpy(&v,res(predObj).data()+predOff,sizeof v);
                // REAL D3D12 SEMANTICS (J-effects-smoke-fix): EQUAL_ZERO enables the predication (discards the draw) when all 64 bits are zero,
                // NOT_EQUAL_ZERO when any bit is set.
                const bool enabled=predOp==UINT(D3D12_PREDICATION_OP_NOT_EQUAL_ZERO)?v!=0:v==0;
                if(enabled){++w->predSkipped;break;}
                ++w->predDrawn;
            }
            // J-3b9: a queued draw must read the bytes it was submitted with (upload retirement).
            if(o.snapped&&std::memcmp(blobAt(o.boundT10),o.snap.data(),416))++w->uploadClobbered;
            if(o.boundCount)if(auto* r=viewResource(o.boundRtv)){CHECK(r->state==D3D12_RESOURCE_STATE_RENDER_TARGET);rasterise(*r,o);}break;
        case Op::copy_tex:{
            auto& src=res(o.res);auto& dst=res(o.dst);
            if(o.subres) { // same size/format textures, whole subresource 0
                CHECK(src.state==D3D12_RESOURCE_STATE_COPY_SOURCE&&dst.state==D3D12_RESOURCE_STATE_COPY_DEST);
                CHECK(src.width==dst.width&&src.height==dst.height&&src.format==dst.format&&src.size()==dst.size());
                std::memcpy(dst.data(),src.data(),src.size());++w->worldCopies;break;
            }
            CHECK(src.state==D3D12_RESOURCE_STATE_COPY_SOURCE&&dst.heap==D3D12_HEAP_TYPE_READBACK);
            CHECK(o.w<=src.width&&o.h<=src.height&&o.format==UINT(src.format)&&o.offset%512==0&&o.pitch%256==0);
            CHECK(o.offset+uint64_t(o.pitch)*o.h<=dst.size());
            // J-3b7b: the copy reads only the source box (always given by step 3).
            CHECK(o.box&&o.br-o.bl==o.w&&o.bb-o.bt==o.h&&o.br<=src.width&&o.bb<=src.height);
            const UINT bpp=fakeBytes(src.format);
            for(UINT y=0;y<o.h;++y)std::memcpy(dst.data()+o.offset+uint64_t(y)*o.pitch,
                src.data()+(uint64_t(y+o.bt)*src.width+o.bl)*bpp,size_t(o.w)*bpp);
            break;}
        default:break;
        }
    }
    CHECK(!predObj); // predication is balanced in every list
    w->gpuOps.clear();
}
static void pso3() {
    auto& s=acz::state();std::lock_guard lock(s.mutex);auto& p=s.pipelines[reinterpret_cast<ID3D12PipelineState*>(&w->pso)];
    p.drawcpt_ia_layout=true;auto& o=p.output;o=gpu_copy::step3::PsoOutput{};o.known=true;o.rt_count=1;o.rtv[0]=UINT(shape3.rtv);
    o.dsv=UINT(shape3.dsv);o.samples=shape3.samples;o.quality=0;o.depth_enable=true;o.depth_write=true;o.depth_func=UINT(shape3.func);
}
static std::wstring pngDir;
// Per-process private scratch directory: <TEMP>\<prefix>-<pid>-<tick>-<qpc>-<n>. A bare process id is NOT unique across ctest runs (Windows reuses ids
// quickly under parallel load), and the old fixed per-pid directory was never removed, so a later process with a recycled id inherited the earlier
// one's PNGs and failed its pngFiles() counts (abort -> 0xc0000409). The directory is created fresh (an existing one is a hard error),
// removed on normal exit, and stale siblings of this prefix older than an hour are pruned (best effort) so aborted runs cannot accumulate.
static std::vector<std::wstring>& scratchDirs(){static std::vector<std::wstring> v;return v;}
static void removeScratchDirs() {
    { // the dedicated feature logs keep their files open (no delete sharing) until process exit: close them first so the directory can go
        auto& fl=acz::feature_log::detail::state();
        for(int i=0;i<fl.count&&i<int(acz::feature_log::max_files);++i)if(fl.files[i].h!=INVALID_HANDLE_VALUE){CloseHandle(fl.files[i].h);fl.files[i].h=INVALID_HANDLE_VALUE;}
    }
    for(const auto& d:scratchDirs()){std::error_code ec;std::filesystem::remove_all(std::filesystem::path(d),ec);}}
static std::wstring makeScratchDir(const wchar_t* prefix) {
    wchar_t tmp[MAX_PATH]{};const DWORD n=GetTempPathW(MAX_PATH,tmp);CHECK(n>0&&n<MAX_PATH);
    {std::error_code ec;const auto now=std::filesystem::file_time_type::clock::now();
     for(std::filesystem::directory_iterator it(std::filesystem::path(tmp),ec),end;!ec&&it!=end;it.increment(ec)) {
         const auto name=it->path().filename().wstring();
         std::error_code e2;if(name.rfind(prefix,0)==0&&now-it->last_write_time(e2)>std::chrono::hours(1)&&!e2)std::filesystem::remove_all(it->path(),e2);
     }}
    static unsigned counter=0;
    LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
    const std::wstring dir=std::wstring(tmp)+prefix+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+
        std::to_wstring(unsigned(qpc.QuadPart&0xffffff))+L"-"+std::to_wstring(counter++);
    CHECK(CreateDirectoryW(dir.c_str(),nullptr)); // fails (aborts the scenario) if it already exists: never inherit another run's files
    if(scratchDirs().empty())std::atexit(removeScratchDirs);
    scratchDirs().push_back(dir);
    return dir;
}
// pngDir is the process's one scratch directory (created on first use, shared by every boot of the process like the old per-pid directory was).
// Scenarios that derive sub-directories from it BEFORE bootStep3 ran (the effects threshold / burst control fixtures) call this first: with an empty
// pngDir they used "\fxthr" and "\burstctl", i.e. the drive root of the working directory, shared by every parallel scenario process.
static void ensurePngDir(){if(pngDir.empty())pngDir=makeScratchDir(L"acz-step3-test");}
// J-3b9: the persistent Map as the l3Map/l3Unmap hooks leave it: covered table,
// one outstanding Map of subresource 0 in the native lifetime, pin correlated.
static void mapGame(int r,bool mapped=true) {
    auto& s=acz::state();std::lock_guard lock(s.mutex);auto* x=s.l3.resources.get(gameAddr(r));CHECK(x);
    fakeMapTable[8]=reinterpret_cast<void*>(&acz::l3Map);fakeMapTable[9]=reinterpret_cast<void*>(&acz::l3Unmap);
    {auto& h=s.objects[fakeMapTable];h.originals[8]=h.originals[9]=reinterpret_cast<void*>(&unused);}
    x->hooks=true;x->map_table=fakeMapTable;x->outstanding=1;x->mapped_lifetime=gameLife(r);if(!x->map_generation)x->map_generation=1;
    x->mapped=w->resources[size_t(r)].data();x->mapped_subresource=0;x->map_gap=false;x->map_inflight=0;
    s.uploadPins.map(gameAddr(r),x->serial,gameLife(r),x->map_generation,x->mapped);
    if(!mapped){x->invalidate_map();x->outstanding=0;} // covered by the hooks, but unmapped
}
static void bootStep3() {
    build(true);acz::g_step3Test=true;acz::g_l3Test=true;
    ensurePngDir();
    acz::g_step3PngDir=pngDir.c_str();
    acz::feature_log::set_directory(pngDir.c_str()); // J-3b9c dedicated logs land next to the PNGs
    pso3();w->resourcesCreated=4; // fakes 1..3 are game buffers; private objects land in 4..7
    bootBuilt(true);
    CHECK(st1().step3&&!st1().step2);
    writeScene();
    // J-3b7d: the game IB (fake 1) and VB+SrtData (fake 2) as the creation/map
    // hooks leave them: L3 records, held J-142 pins, persistently mapped.
    acz::g_step3CandidateCap=shape3.candidates;
    useNativeLife=true;gameResource(1,D3D12_HEAP_TYPE_UPLOAD);gameResource(2,D3D12_HEAP_TYPE_UPLOAD);
    for(int r=1;r<=2;++r)mapGame(r,shape3.mapped);
    // J-3b9: the game's RTV/DSV descriptors as the view-creation hooks record them.
    acz::step3NoteView(nullptr,gameRtvHandle,false);acz::step3NoteView(nullptr,gameDsvHandle,true);
}
static void prepare3() {
    CHECK(acz::step1Tick());CHECK(phase()==int(acz::Step1Phase::ready));
    CHECK(w->resourcesCreated==4&&w->fenceCreates==1); // targets wait for the observation
    CHECK(has("gpu-copy-step3;event=pins;")&&has("gpu_copy_step3_experiment+gpu_copy_early_device_hook"));
    CHECK(has("gpu-copy-step3;event=prepare;mode=step3;targets=deferred-until-observed")&&has("bound=1"));
}
static int recList=0; // J-180: the fake list the frame helpers (frameSet / bind3 / bindT10) record into (0 = the historical fixture; 1 for A -> B -> A)
static diagnostics::L3Observation::List* l3list0() {
    auto& ls=acz::state().states.at(listAlias(recList));return acz::l3_list_locked(ls);
}
// The game's own setters for the cockpit draw, through the proxy's hooks.
static void bind3(float vpW,float vpH) {
    auto* l0=listAlias(recList);
    if(shape3.t10){auto& s=acz::state();std::lock_guard lock(s.mutex);auto* l=l3list0();CHECK(l);l->t10_parameter=3;l->t10_table=false;}
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{gameRtvHandle},dsv{gameDsvHandle};
    if(shape3.omGap)l0->OMSetRenderTargets(1,nullptr,FALSE,nullptr);
    else if(shape3.omSet)l0->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
    const D3D12_VIEWPORT vp{0,0,vpW,vpH,0,1};l0->RSSetViewports(1,&vp);
    const D3D12_RECT sc{0,0,LONG(vpW),LONG(vpH)};l0->RSSetScissorRects(1,&sc);
    l0->SetGraphicsRootShaderResourceView(3,gameT10());
    // Production resolves the resource key through the binding observer (absent offline).
    if(shape3.t10){auto& s=acz::state();std::lock_guard lock(s.mutex);l3list0()->sources[0].address=gameAddr(2);}
    // The game's IA setters (IB 9 x 16-bit, VB slot 0 stride 64) as the capture tracking leaves them.
    {auto& s=acz::state();std::lock_guard lock(s.mutex);auto& b=s.states.at(l0).bindings;
     b.index=capture::BufferView{gameVa(1)+ibOff,0,18,2};b.vertices[0]=capture::BufferView{gameVa(2)+vbOff,0,512,64};
     auto* l=l3list0();l->sources[33]={gameAddr(1),gameVa(1)+ibOff,18};l->sources[1]={gameAddr(2),gameVa(2)+vbOff,512};}
}
// J-3b7d: point the bound t10 at another SrtData slot of fake 2 (a second draw).
static void bindT10(uint64_t off) {
    auto* l0=listAlias(recList);l0->SetGraphicsRootShaderResourceView(3,gameVa(2)+off);
    auto& s=acz::state();std::lock_guard lock(s.mutex);l3list0()->sources[0].address=gameAddr(2);
}
static void frame3(float vpW=-1,float vpH=-1) {
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);
    bind3(vpW<0?shape3.vpW:vpW,vpH<0?shape3.vpH:vpH);
    l0->DrawIndexedInstanced(9,1,0,0,0);
}
static void cpuSample3() {
    auto& s=acz::state();std::lock_guard lock(s.mutex);auto& c=s.cpuSnapshot;
    auto& q=c.completed;q=diagnostics::CpuUploadSnapshot::Request{};q.accepted=true;q.list=0x4b;q.generation=0x2768;
    q.draw=st1().s3.shape.draw_ordinal;
    auto& x=q.ranges[0];x.resource=gameAddr(2);x.lifetime=gameLife(2);x.offset=srtOff;x.bytes=416;x.va=gameT10();x.pinned=x.upload=true;
    std::memcpy(c.first.data(),w->resources[2].data()+srtOff,416);
    // J-3b7b: the VB range of the same sample (8 vertices x 64 B) feeds the crop.
    auto& v=q.ranges[1];v.resource=gameAddr(2);v.lifetime=gameLife(2);v.offset=vbOff;v.bytes=512;v.pinned=v.upload=true;
    q.vertex_stride=64;std::memcpy(c.first.data()+416,w->resources[2].data()+vbOff,512);
    auto& ix=q.ranges[2];ix.resource=gameAddr(1);ix.lifetime=gameLife(1);ix.offset=ibOff;ix.bytes=18;ix.pinned=ix.upload=true;
    q.index_stride=2;q.count=9;q.topology=D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;std::memcpy(c.first.data()+416+512,w->resources[1].data()+ibOff,18);
    c.stable=true;c.attempts=1;c.reason="snapshot-stable";c.sampled_ms=GetTickCount64();
}
static void step3Lines() {
    size_t n=0;
    for(const auto& l:w->lines) {
        if(l.size()>440){std::fprintf(stderr,"overlong (%zu): %s\n",l.size(),l.c_str());CHECK(false);}
        CHECK(l.rfind("gpu-copy-step3",0)==0);
        if(l.rfind("gpu-copy-step3;event=early-device",0)!=0&&l.rfind("gpu-copy-step3-status;",0)!=0&&
           l.rfind("gpu-copy-step3-table;",0)!=0)++n; // the candidate table goes to the ordinary log
    }
    if(n>diagnostics::step1_records){dump();CHECK(false);}
}
static std::string field(const char* a,const char* key) {
    for(const auto& l:w->lines)if(l.find(a)!=std::string::npos) {
        const auto at=l.find(key);if(at==std::string::npos)continue;
        const auto start=at+std::strlen(key);return l.substr(start,l.find(';',start)-start);
    }
    return {};
}
// Observation frame -> snapshot -> armed (the targets exist, nothing recorded).
static void observeAndArm() {
    frame3();
    CHECK(!st1().s3.observed&&st1().s3.recorded==1&&w->lists[0].events.back()=="draw-indexed"&&w->lists[0].ops.size()<=3);
    CHECK(phase()==int(acz::Step1Phase::ready)&&!st1().recorded);
    // One tick: drain + evaluate + choose (cap reached) + observe + arm.
    CHECK(acz::step1Tick());
    if(!(st1().s3.armed&&has("event=observe;status=accepted")&&has("event=select;outcome=chosen")&&w->resourcesCreated==8&&w->heapsCreated==4)){dump();CHECK(false);}
    CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();
}
struct Native3 {UINT omCount;std::array<SIZE_T,8> rtv;bool dsvSet;SIZE_T dsv;UINT64 t10;};
static Native3 native3(){const auto& l=w->lists[0];return {l.omCount,l.rtv,l.dsvSet,l.dsv,l.roots[3]};}
static bool sameTrack(const acz::ListState::Step3Track& a,const acz::ListState::Step3Track& b) {
    if(a.known!=b.known||a.gap!=b.gap||a.viewports!=b.viewports||a.scissors!=b.scissors||a.right!=b.right||a.bottom!=b.bottom)return false;
    if(a.omSet!=b.omSet||a.omCount!=b.omCount||a.omSingle!=b.omSingle||a.dsv!=b.dsv||a.dsvHandle.ptr!=b.dsvHandle.ptr)return false;
    for(int i=0;i<8;++i)if(a.rtv[size_t(i)].ptr!=b.rtv[size_t(i)].ptr)return false;
    return true;
}
// The inserting frame: exact command sequence, exact restore, unchanged tracking.
static void inserted3() {
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);
    bind3(shape3.vpW,shape3.vpH);
    acz::ListState::Step3Track trackBefore;uint64_t rootBefore=0;diagnostics::L3Observation::Source srcBefore{};
    {auto& s=acz::state();std::lock_guard lock(s.mutex);const auto& ls=s.states.at(l0);trackBefore=ls.step3;
     rootBefore=ls.bindings.roots.at(3).value;srcBefore=l3list0()->sources[0];}
    const auto nativeBefore=native3();
    auto& L=w->lists[0];const size_t eventsBefore=L.events.size(),opsBefore=L.ops.size();
    const auto before=allocations;traceAllocations=true;
    l0->DrawIndexedInstanced(9,1,0,0,0);traceAllocations=false;
    CHECK(allocations==before); // no heap allocation on the inserting Draw path
    CHECK(phase()==int(acz::Step1Phase::inserted)&&st1().recorded);
    // 1. Exact native sequence: OMSet, 4 replays, restore, then the original Draw.
    std::vector<std::string> expected{"om"};
    for(int i=0;i<4;++i) {
        expected.push_back("clear-rtv");expected.push_back("clear-dsv");if(i)expected.push_back("srv");
        expected.push_back("draw-indexed");expected.push_back("barrier");expected.push_back("copy-tex");expected.push_back("barrier");
    }
    expected.push_back("srv");expected.push_back("om");expected.push_back("draw-indexed");
    std::vector<std::string> got(L.events.begin()+std::ptrdiff_t(eventsBefore),L.events.end());
    if(got!=expected){for(auto& e:got)std::fprintf(stderr,"%s ",e.c_str());std::fprintf(stderr,"\n");CHECK(false);}
    CHECK(st1().s3.issued.load()==30&&st1().s3.targets.commands==30);
    // 2. Arguments: private targets during the replays, the game's exactly after.
    const auto& s3=st1().s3;std::vector<Op> ops(L.ops.begin()+std::ptrdiff_t(opsBefore),L.ops.end());
    CHECK(ops.front().t==Op::om&&ops.front().count==1&&ops.front().rtv[0]==s3.rtv.ptr&&ops.front().dsvSet&&ops.front().dsv==s3.dsv.ptr);
    unsigned draw=0;const UINT64 vas[4]={gameT10(),s3.uploadVa,s3.uploadVa+512,s3.uploadVa+1024};
    for(size_t i=0;i+1<ops.size();++i) {
        const auto& o=ops[i];
        if(o.t==Op::draw){CHECK(o.boundRtv==s3.rtv.ptr&&o.boundDsv==s3.dsv.ptr&&o.boundT10==vas[draw]);++draw;}
        if(o.t==Op::barrier)CHECK(o.res==obj(s3.rt)); // never a game resource
        if(o.t==Op::copy_tex)CHECK(o.res==obj(s3.rt)&&o.dst==obj(s3.readback)&&o.offset==(draw-1)*s3.targets.footprint_stride);
        if(o.t==Op::clear_dsv)CHECK(o.depth==0.0f&&o.clearFlags==(shape3.dsv==DXGI_FORMAT_D32_FLOAT_S8X24_UINT?
            D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL:D3D12_CLEAR_FLAG_DEPTH)); // GREATER: clear to 0 (+stencil 0)
        if(o.t==Op::copy_tex)CHECK(o.box&&o.bl==s3.targets.crop_left&&o.bt==s3.targets.crop_top&&o.w==s3.targets.crop_w&&o.h==s3.targets.crop_h);
    }
    CHECK(draw==4);
    const auto& restoreSrv=ops[ops.size()-3];const auto& restoreOm=ops[ops.size()-2];const auto& original=ops.back();
    CHECK(restoreSrv.t==Op::srv&&restoreSrv.param==3&&restoreSrv.va==gameT10());
    CHECK(restoreOm.t==Op::om&&restoreOm.count==1&&restoreOm.single==FALSE&&originOf(restoreOm.rtv[0])==gameRtvHandle&&restoreOm.dsvSet&&originOf(restoreOm.dsv)==gameDsvHandle);
    CHECK(original.t==Op::draw&&originOf(original.boundRtv)==gameRtvHandle&&originOf(original.boundDsv)==gameDsvHandle&&original.boundT10==gameT10());
    // 3. The fake native state and the proxy's own tracking are identical before/after.
    const auto nativeAfter=native3();
    // J-3b9c: the restore binds PRIVATE copies: other handle values, the same descriptor contents.
    {bool same=true;for(size_t i=0;i<nativeAfter.rtv.size();++i)same=same&&originOf(nativeAfter.rtv[i])==originOf(nativeBefore.rtv[i]);
     CHECK(nativeAfter.omCount==nativeBefore.omCount&&same&&nativeAfter.dsvSet==nativeBefore.dsvSet&&
          originOf(nativeAfter.dsv)==originOf(nativeBefore.dsv)&&nativeAfter.t10==nativeBefore.t10);}
    {auto& s=acz::state();std::lock_guard lock(s.mutex);const auto& ls=s.states.at(l0);CHECK(sameTrack(trackBefore,ls.step3));
     CHECK(ls.bindings.roots.at(3).value==rootBefore);const auto src=l3list0()->sources[0];
     CHECK(src.va==srcBefore.va&&src.address==srcBefore.address&&src.bytes==srcBefore.bytes);}
}
static bool readFile(const std::wstring& path,std::vector<uint8_t>& out) {
    FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"rb")||!f)return false;
    uint8_t b[65536];size_t n;while((n=std::fread(b,1,sizeof b,f))>0)out.insert(out.end(),b,b+n);std::fclose(f);return true;
}
// Independent bitwise CRC-32 / Adler-32 (not the production table code).
static uint32_t crcBits(const uint8_t* p,size_t n){uint32_t c=0xffffffffu;for(size_t i=0;i<n;++i){c^=p[i];for(int k=0;k<8;++k)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
static uint32_t be32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
static bool decodePng(const std::vector<uint8_t>& f,uint32_t& w,uint32_t& h,std::vector<uint8_t>& rgba) {
    static const uint8_t sig[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    if(f.size()<8||std::memcmp(f.data(),sig,8))return false;
    std::vector<uint8_t> z;size_t at=8;bool end=false;
    while(at+12<=f.size()&&!end) {
        const uint32_t n=be32(&f[at]);if(at+12+n>f.size())return false;
        const uint8_t* type=&f[at+4];if(crcBits(type,n+4)!=be32(&f[at+8+n]))return false;
        if(!std::memcmp(type,"IHDR",4)){w=be32(type+4);h=be32(type+8);if(type[12]!=8||type[13]!=6||type[14]||type[15]||type[16])return false;}
        else if(!std::memcmp(type,"IDAT",4))z.insert(z.end(),type+4,type+4+n);
        else if(!std::memcmp(type,"IEND",4))end=true;
        at+=12+n;
    }
    if(!end||z.size()<6||z[0]!=0x78||((z[0]<<8)|z[1])%31)return false;
    std::vector<uint8_t> raw;size_t p=2;bool last=false;
    while(!last) {
        if(p+5>z.size())return false;
        last=z[p]&1;if(z[p]&6)return false; // stored blocks only
        const unsigned len=z[p+1]|z[p+2]<<8,nlen=z[p+3]|z[p+4]<<8;if((len^nlen)!=0xffff)return false;
        p+=5;if(p+len>z.size())return false;raw.insert(raw.end(),z.begin()+std::ptrdiff_t(p),z.begin()+std::ptrdiff_t(p+len));p+=len;
    }
    uint32_t a=1,b=0;for(uint8_t c:raw){a=(a+c)%65521;b=(b+a)%65521;}
    if(p+4!=z.size()||be32(&z[p])!=((b<<16)|a))return false;
    if(raw.size()!=size_t(h)*(size_t(w)*4+1))return false;
    rgba.clear();for(uint32_t y=0;y<h;++y){if(raw[size_t(y)*(w*4+1)])return false;const auto* r=&raw[size_t(y)*(w*4+1)+1];rgba.insert(rgba.end(),r,r+size_t(w)*4);}
    return true;
}
static double number(const std::string& s){return s.empty()?-1e9:std::strtod(s.c_str(),nullptr);}
static void step3_success() {
    bootStep3();prepare3();observeAndArm();
    CHECK(has("event=srt-source;source=selection-sample;")&&has("event=srt-matrix;replay=left;offset=-1.2474")&&has("event=targets;rt="));
    CHECK(line("event=targets;","R8G8B8A8_UNORM,256x144,RENDER_TARGET")&&line("event=srt-checks;","untouched-bytes-equal=1"));
    {const auto& sub=st1().s3.sub;CHECK(!sub.error&&sub.control_m0_rel<1e-6&&sub.control_m64_rel<1e-6);
     for(double r:sub.eye_clip_residual)CHECK(r<1e-9);
     CHECK(std::fabs(sub.camera.right[0]-1.0)<1e-6&&std::fabs(sub.camera.eye[0]-sceneEye[0])<1e-3);}
    // Exactly the zero-offset control reproduces the original blob within float rounding; +128.. untouched.
    {const auto& b=st1().s3.sub.blobs;CHECK(std::memcmp(b[0].data()+128,sceneBlob.data()+128,288)==0&&std::memcmp(b[1].data()+128,sceneBlob.data()+128,288)==0);
     float a[32],c[32];std::memcpy(a,b[0].data(),128);std::memcpy(c,sceneBlob.data(),128);
     for(int i=0;i<32;++i)CHECK(std::fabs(a[i]-c[i])<=1e-5f*(1.0f+std::fabs(c[i])));}
    inserted3();
    CHECK(listAlias(0)->Close()==S_OK);ticks(1);
    CHECK(has("gpu-copy-step3;event=replay;")&&line("event=replay;","issued=30")&&line("event=insert;","private-replays-and-restore-then-original-Draw"));
    execute({cl(0)});
    CHECK(acz::step1Tick()&&res(st1().s3.readback).maps==0); // pending: never Map before completion
    advance();CHECK(w->rasterised==4&&w->inconsistent==0);
    ticks(3);CHECK(res(st1().s3.readback).maps==0); // completed, closure not yet observed
    const auto pixels=res(st1().s3.readback).big; // what the GPU wrote (readback contents)
    frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PASS;reason=per-eye-replays-read")){dump();CHECK(false);}
    CHECK(line("EXPERIMENT_RESULT=PASS","per-eye-replays-of-one-draw;game-state-restored-by-tracking;no-headset-output"));
    // Per-eye geometry: left-minus-right centroid shift has the expected sign and magnitude.
    const double expected=(2.0*gpu_copy::step3::half_ipd_units/sceneDepth)/0.6152*(shape3.vpW*0.5);
    const double dx=number(field("event=stereo;","disparity-px="));
    if(!(dx>0&&std::fabs(dx-expected)<1.0)){std::fprintf(stderr,"disparity %f expected %f\n",dx,expected);dump();CHECK(false);}
    CHECK(line("event=stereo;","sign=left-minus-right-positive")&&line("event=stereo;","control-vs-original=equal:0"));
    CHECK(std::fabs(number(field("event=stereo;","dy-px=")))<0.01);
    const double cxOriginal=number(field("event=pixels;replay=original;","centroid="));
    CHECK(std::fabs(cxOriginal-shape3.vpW*0.5)<0.6); // the scene is centred on the original camera
    CHECK(number(field("event=pixels;replay=left;","covered="))>1000&&number(field("event=pixels;replay=right;","covered="))>1000);
    CHECK(line("STEP3_RESULT;observed=1;observe=ok;armed=1;inserted=1;issued=30;read=1","control=equal:0;pngs=4/4;content=ok"));
    // PNG files decode (independent CRC/Adler) to exactly the readback footprints, RGBA raw bytes.
    const auto& t=st1().s3.targets;
    for(unsigned i=0;i<4;++i) {
        const std::string name=field((std::string("event=png;replay=")+gpu_copy::step3::replay_names[i]+";").c_str(),"png=");
        CHECK(!name.empty());std::vector<uint8_t> file;CHECK(readFile(pngDir+L"\\"+std::wstring(name.begin(),name.end()),file));
        uint32_t pw=0,ph=0;std::vector<uint8_t> rgba;CHECK(decodePng(file,pw,ph,rgba)&&pw==t.crop_w&&ph==t.crop_h);
        for(uint32_t y=0;y<ph;++y)CHECK(std::memcmp(rgba.data()+size_t(y)*pw*4,pixels.data()+i*t.footprint_stride+y*t.row_pitch,size_t(pw)*4)==0);
        char sha[65];CHECK(acz::step1Sha256(file.data(),file.size(),sha));
        CHECK(line((std::string("event=png;replay=")+gpu_copy::step3::replay_names[i]+";").c_str(),sha));
        // The small copy exists and decodes (256 wide: factor 1, same pixels).
        const std::string sm=field((std::string("event=png;replay=")+gpu_copy::step3::replay_names[i]+";").c_str(),"small=");
        std::vector<uint8_t> sf;CHECK(readFile(pngDir+L"\\"+std::wstring(sm.begin(),sm.end()),sf));
        uint32_t sw=0,sh=0;std::vector<uint8_t> srgba;CHECK(decodePng(sf,sw,sh,srgba)&&sw==pw&&sh==ph&&srgba==rgba);
    }
    // Clean retirement: every private object released; the fence is retained.
    CHECK(has("resources=released")&&gpu_copy::diagnostic_session().snapshot().retired);
    for(int i=4;i<8;++i)CHECK(w->resources[size_t(i)].object.refs==0);
    CHECK(w->heaps[2].refs==0&&w->heaps[3].refs==0&&w->fence.refs==1&&w->heaps[0].refs==1&&w->heaps[1].refs==1); // J-3b9c: the OM ring heaps (0,1) are retained
    // One attempt only.
    frame3();CHECK(verdicts()==1&&!acz::step1Tick()&&w->rasterised==4);
    step3Lines();
}
static void refusedAtObservation(const char* reason) {
    bootStep3();prepare3();frame3();
    CHECK(!acz::step1Tick()&&st1().s3.observed); // decided in the selection tick: finished immediately
    if(!verdict((std::string("EXPERIMENT_RESULT=REFUSED;reason=")+reason).c_str())){dump();CHECK(false);}
    CHECK(line("event=observe;status=refused",reason)&&line("STEP3_RESULT;observed=1;observe=",reason));
    CHECK(has("event=game-rt;rtv=")&&line("STEP3_RESULT;","observe-to-end-ms=")); // decided refusal: logged in the next tick
    CHECK(w->resourcesCreated==4&&w->heapsCreated==2&&!st1().recorded); // nothing created, nothing recorded
    for(const auto& e:w->lists[0].events)CHECK(e!="clear-rtv"&&e!="copy-tex"&&e!="barrier");
    frame3();CHECK(verdicts()==1);
    step3Lines();
}
static void step3_msaa(){shape3.samples=4;refusedAtObservation("msaa-unsupported");}
static void step3_om_unknown(){shape3.omSet=false;refusedAtObservation("om-targets-never-set");}
static void step3_om_gap(){shape3.omGap=true;refusedAtObservation("om-state-gap");}
static void step3_over_cap(){shape3.vpW=4097;shape3.vpH=2049;refusedAtObservation("rt-over-cap");}
static void step3_srv_unknown(){shape3.t10=false;refusedAtObservation("t10-parameter-unknown");}
static void step3_format(){shape3.rtv=DXGI_FORMAT_R10G10B10A2_UNORM;refusedAtObservation("rt-format-unsupported");}
static void step3_depth_equal(){shape3.func=D3D12_COMPARISON_FUNC_EQUAL;refusedAtObservation("depth-func-equality-unsupported");}
static void step3_mismatch() {
    // After arming, a draw with another viewport does not consume the attempt.
    bootStep3();prepare3();observeAndArm();
    frame3(200,144);CHECK(phase()==int(acz::Step1Phase::ready)&&!st1().recorded&&st1().s3.mismatches==1);
    for(const auto& e:w->lists[0].events)CHECK(e!="clear-rtv");
    CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();
    inserted3();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PASS")){dump();CHECK(false);}
    CHECK(line("STEP3_RESULT;","mismatch=viewport-changed:1")&&line("event=replay;","mismatched-draws=1"));
    step3Lines();
}
static void step3_no_snapshot() {
    // The game buffers are not CPU-mapped: no candidate can be read, none visible.
    shape3.mapped=false;
    bootStep3();prepare3();frame3();CHECK(!acz::step1Tick());
    if(!verdict("REFUSED;reason=no-visible-candidate")){dump();CHECK(false);}
    CHECK(line("gpu-copy-step3-table;seq=1;ordinal=1;","status=not-mapped")&&line("event=select;outcome=none-visible","unreadable=1"));
    CHECK(w->resourcesCreated==4&&!st1().recorded);
    step3Lines();
}
static void step3_create_fail() {
    bootStep3();prepare3();frame3();
    w->createResourceHr=E_OUTOFMEMORY;CHECK(!acz::step1Tick());
    if(!verdict("REFUSED;reason=rt-create-failed")){dump();CHECK(false);}
    CHECK(has("event=targets;rt=0,")&&!st1().recorded&&has("resources=released"));
    step3Lines();
}
static void step3_signal_fail() {
    bootStep3();prepare3();observeAndArm();inserted3();
    CHECK(listAlias(0)->Close()==S_OK);w->signalHr=DXGI_ERROR_DEVICE_REMOVED;execute({cl(0)});
    advance();frame(1,false);ticks(10);
    if(!verdict("FAIL;reason=signal-failed")){dump();CHECK(false);}
    // Quarantine: nothing mapped, every private object retained.
    CHECK(res(st1().s3.readback).maps==0&&line("STEP3_RESULT;","read=0"));
    for(int i=4;i<8;++i)CHECK(w->resources[size_t(i)].object.refs==1);
    CHECK(w->heaps[2].refs==1&&gpu_copy::diagnostic_session().snapshot().state==gpu_copy::State::quarantined);
    step3Lines();
}
static void step3_float() {
    // The live shape: R16G16B16A16_FLOAT + D32_FLOAT_S8X24_UINT with stencil, here at 1280x720.
    shape3.rtv=DXGI_FORMAT_R16G16B16A16_FLOAT;shape3.dsv=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;shape3.vpW=1280;shape3.vpH=720;
    bootStep3();
    // The game RT 0 view and its creation record, as the RTV/L3 creation hooks leave them.
    // (the production CreateRenderTargetView wrapper calls step3NoteView after the native call).
    {auto& g=w->resources[3];g.dim=D3D12_RESOURCE_DIMENSION_TEXTURE2D;g.width=1280;g.height=720;g.format=DXGI_FORMAT_R16G16B16A16_FLOAT;
     g.heap=D3D12_HEAP_TYPE_DEFAULT;acz::step3NoteView(reinterpret_cast<ID3D12Resource*>(&g.object),gameRtvHandle,false);
     acz::step3NoteView(nullptr,0x7777,false);} // a null-resource view is recorded undescribed
    prepare3();observeAndArm();
    if(!(line("event=game-rt;rtv=9000;status=resolved","size=1280x720;format=10;heap=DEFAULT")&&line("event=game-rt;","replay-stencil-clear=1"))){dump();CHECK(false);}
    CHECK(line("event=targets;","R16G16B16A16_FLOAT,1280x720")&&line("event=targets;","D32_FLOAT_S8X24_UINT"));
    const auto& t=st1().s3.targets;
    // J-3b7c: the FULL target is read back; the crop is diagnostic only.
    CHECK(t.crop_left==0&&t.crop_top==0&&t.crop_w==1280&&t.crop_h==720&&t.readback_bytes==4*t.footprint_stride&&t.row_pitch==1280*8);
    CHECK(res(st1().s3.readback).width==t.readback_bytes);
    CHECK(line("event=crop;diagnostic-only;status=ok;","gs-rejected=o:0,c:0,l:0,r:0;w<=0=o:0,c:0,l:0,r:0;front=w>0(gs-outcode)"));
    CHECK(line("event=crop;","triangles=4;")&&line("event=crop;","indices=9;"));
    CHECK(line("event=vertices;source=selection-sample-vb;attribute0-xyz=-0.4,-0.2,0;-0.4,0.2,0;",""));
    inserted3();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();CHECK(w->inconsistent==0&&w->rasterised==4);
    frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PASS;reason=per-eye-replays-read")){dump();CHECK(false);}
    CHECK(line("event=f16;replay=left;","r=0.75/0.75/0.75;g=0.5/0.5/0.5;b=0.25/0.25/0.25;a=1/1/1")&&line("event=f16;","png=clamp01-x255-linear-no-gamma"));
    const double expected=(2.0*gpu_copy::step3::half_ipd_units/sceneDepth)/0.6152*(shape3.vpW*0.5);
    const double dx=number(field("event=stereo;","disparity-px="));
    if(!(std::fabs(dx-expected)<1.0)){std::fprintf(stderr,"disparity %f expected %f\n",dx,expected);dump();CHECK(false);}
    CHECK(std::fabs(number(field("event=pixels;replay=original;","centroid="))-640.0)<0.6); // RT coordinates (crop origin added)
    CHECK(line("event=stereo;","control-vs-original=equal:0")&&line("event=stereo;","channels=raw-fp16-png-clamped-scaled"));
    // PNG: 8-bit conversion of the halves (0.75,0.5,0.25,1) -> (191,128,64,255).
    const std::string name=field("event=png;replay=left;","png=");std::vector<uint8_t> file;
    CHECK(readFile(pngDir+L"\\"+std::wstring(name.begin(),name.end()),file));
    uint32_t pw=0,ph=0;std::vector<uint8_t> rgba;CHECK(decodePng(file,pw,ph,rgba)&&pw==t.crop_w&&ph==t.crop_h);
    uint64_t coloured=0;for(size_t i=0;i<rgba.size();i+=4)if(rgba[i+3]){CHECK(rgba[i]==191&&rgba[i+1]==128&&rgba[i+2]==64&&rgba[i+3]==255);++coloured;}
    CHECK(coloured==uint64_t(number(field("event=pixels;replay=left;","covered="))));
    // The 640x360 box-filtered copy (factor 2): interior texels keep the colour, edges blend toward 0.
    {const std::string sm=field("event=png;replay=left;","small=");std::vector<uint8_t> sf;
     CHECK(readFile(pngDir+L"\\"+std::wstring(sm.begin(),sm.end()),sf)&&line("event=png;replay=left;","small-size=640x360"));
     uint32_t sw=0,sh=0;std::vector<uint8_t> s4;CHECK(decodePng(sf,sw,sh,s4)&&sw==640&&sh==360);
     uint64_t full=0;for(size_t i=0;i<s4.size();i+=4)if(s4[i+3]==255){CHECK(s4[i]==191&&s4[i+1]==128&&s4[i+2]==64);++full;}
     CHECK(full>0&&full*4<=coloured);}
    step3Lines();
}
static void armAndPass() {
    prepare3();observeAndArm();inserted3();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();
    frame(1,false);ticks(10);
}
static void step3_crop_clip() {
    // Vertex 7 behind the camera: logged (w<=0), never a refusal; the GS (and the
    // fake) drop the triangles that cannot be drawn, the rest render: PASS.
    bootStep3();const float z=2.5f;std::memcpy(w->resources[2].data()+vbOff+64*7+8,&z,4);
    armAndPass();
    if(!verdict("EXPERIMENT_RESULT=PASS")){dump();CHECK(false);}
    CHECK(line("event=crop;diagnostic-only;status=ok;","w<=0=o:1,c:1,l:1,r:1"));
    step3Lines();
}
static void step3_crop_offscreen() {
    // Every vertex off screen: the crop says so, the full readback is empty -> FAIL (clean, released).
    bootStep3();for(int i=0;i<8;++i){float x;std::memcpy(&x,w->resources[2].data()+vbOff+64*i,4);x+=6.0f;std::memcpy(w->resources[2].data()+vbOff+64*i,&x,4);}
    // J-3b7d: such a draw is never selected (all triangles fail one clip plane).
    prepare3();frame3();CHECK(!acz::step1Tick());
    if(!verdict("EXPERIMENT_RESULT=REFUSED;reason=no-visible-candidate")){dump();CHECK(false);}
    CHECK(line("gpu-copy-step3-table;seq=1;","status=gs-rejected;")&&w->resourcesCreated==4&&!st1().recorded);
    step3Lines();
}
static void step3_gs_rejected() {
    // The live D-222 shape: every vertex has w<0 under +64 (local origin behind
    // the camera): the GS rejects every triangle; logged, then the replays are
    // empty -> FAIL original-replay-empty (clean).
    bootStep3();for(int i=0;i<8;++i){const float z=2.5f;std::memcpy(w->resources[2].data()+vbOff+64*i+8,&z,4);}
    // J-3b7d: the live D-222 draw would now be logged in the table and never chosen.
    prepare3();frame3();CHECK(!acz::step1Tick());
    if(!verdict("EXPERIMENT_RESULT=REFUSED;reason=no-visible-candidate")){dump();CHECK(false);}
    CHECK(line("gpu-copy-step3-table;seq=1;ordinal=1;","status=gs-rejected;w=-10:-10;tris=0/4"));
    step3Lines();
}
static void step3_crop_cap() {
    // The live shape at full size: 2560x1440 FP16 + D32S8X24, four full-frame
    // footprints (4 x 28.1 MiB) read back, 640x360 small PNGs. No crop cap applies.
    shape3.rtv=DXGI_FORMAT_R16G16B16A16_FLOAT;shape3.dsv=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;shape3.vpW=2560;shape3.vpH=1440;
    bootStep3();armAndPass();
    if(!verdict("EXPERIMENT_RESULT=PASS;reason=per-eye-replays-read")){dump();CHECK(false);}
    const auto& t=st1().s3.targets;
    CHECK(t.crop_w==2560&&t.crop_h==1440&&t.row_pitch==2560*8&&t.readback_bytes==4*t.footprint_stride&&t.readback_bytes<=gpu_copy::step3::readback_byte_cap);
    CHECK(line("event=png;replay=right;","small-size=640x360")&&line("event=crop;diagnostic-only;status=ok;",""));
    const double expected=(2.0*gpu_copy::step3::half_ipd_units/sceneDepth)/0.6152*(shape3.vpW*0.5);
    CHECK(std::fabs(number(field("event=stereo;","disparity-px="))-expected)<1.0);
    step3Lines();
}
static void step3_arm_timeout() {
    // Armed, then no qualified draw: refused after the bounded 30 s, not 600 s.
    bootStep3();prepare3();observeAndArm();
    ticks(305);
    if(!verdict("EXPERIMENT_RESULT=REFUSED;reason=no-qualified-draw-within-30s-after-arm")){dump();CHECK(false);}
    for(int i=4;i<8;++i)CHECK(w->resources[size_t(i)].object.refs==0); // never recorded: released
    frame3();CHECK(!st1().recorded&&verdicts()==1);
    step3Lines();
}
// ------------------------------------------------------------- J-3b7d selection
constexpr uint64_t srtOffBehind=0x1400; // a second SrtData slot in fake 2
static void writeBehind() {
    // The live D-222 shape: the same mesh with its local origin 30 units BEHIND the camera.
    acz::Mat4 m0,m64,m256;sceneMatrices(m0,m64,m256);
    const acz::Pose pose{sceneEye,{0,0,0,1}};
    const acz::FovAngles fov{std::atan(-0.6152),std::atan(0.6152),std::atan(0.3460),std::atan(-0.3460)};
    const auto vp=acz::multiply(acz::viewMatrixFromPose(pose),acz::projectionFromFov(fov,2.0,1000.0));
    acz::Mat4 scale=acz::identity();scale[0]=scale[5]=scale[10]=16.0;
    m256=acz::multiply(scale,acz::translation(sceneEye[0],sceneEye[1],sceneEye[2]+sceneDepth));
    m64=acz::multiply(m256,vp);m0=acz::multiply(m64,acz::ps2ViewportMatrix(acz::PS2_SCALE_X,acz::PS2_SCALE_Y,acz::PS2_CENTRE,{1.0,0.0}));
    auto* srt=w->resources[2].data()+srtOffBehind;for(size_t i=0;i<416;++i)srt[i]=uint8_t(i*11+3);
    putMatrix(srt,m0);putMatrix(srt+64,m64);putMatrix(srt+256,m256);putParams(srt);
}
// A frame with two matched draws: ordinal 1 behind the camera, ordinal 2 visible.
static void frameTwo(bool expectInsert=false) {
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);
    bind3(shape3.vpW,shape3.vpH);bindT10(srtOffBehind);
    l0->DrawIndexedInstanced(9,1,0,0,0);
    if(expectInsert)CHECK(w->lists[0].events.back()=="draw-indexed"&&phase()==int(acz::Step1Phase::ready)); // ordinal 1: skipped
    bindT10(srtOff);
    const auto before=allocations;traceAllocations=true;
    l0->DrawIndexedInstanced(9,1,0,0,0);traceAllocations=false;CHECK(allocations==before);
}
static void step3_select() {
    shape3.candidates=2;bootStep3();writeBehind();prepare3();
    frameTwo();CHECK(st1().s3.recorded==2&&!st1().s3.observed);
    CHECK(acz::step1Tick()&&st1().s3.armed);
    if(!line("gpu-copy-step3-table;seq=1;ordinal=1;","status=gs-rejected")){dump();CHECK(false);}
    CHECK(line("gpu-copy-step3-table;seq=1;ordinal=1;","tris=0/4")&&line("gpu-copy-step3-table;seq=2;ordinal=2;","status=visible;"));
    CHECK(line("gpu-copy-step3-table;seq=2;ordinal=2;","tris=4/4")&&line("gpu-copy-step3-table;seq=1;","w=-30:-30"));
    CHECK(line("event=select;outcome=chosen;recorded=2;evaluated=2;dropped=0;unreadable=0;visible=1;table-lines=2","chosen=seq:2,ordinal:2,"));
    CHECK(st1().s3.shape.draw_ordinal==2&&st1().s3.shape.t10_va==gameVa(2)+srtOff);
    CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();
    // Insertion: the ordinal-1 draw is skipped, the chosen ordinal-2 draw gets the replays.
    frameTwo(true);
    if(!(phase()==int(acz::Step1Phase::inserted)&&st1().s3.orderShifted>=1&&w->lists[0].copies==0)) {
        std::fprintf(stderr,"phase=%d skipped=%llu shifted=%llu mismatch=%s fresh=%s armed=%d\n",phase(),
            (unsigned long long)st1().s3.skipped,(unsigned long long)st1().s3.orderShifted,st1().s3.mismatch?st1().s3.mismatch:"-",
            st1().s3.freshness,int(st1().s3.armed));dump();CHECK(false);
    }
    CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();CHECK(w->rasterised==4);
    frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PASS;reason=per-eye-replays-read")){dump();CHECK(false);}
    CHECK(line("event=replay;","srt=inline-live;srt-unreadable=0;same-signature-other-ordinal=1;skipped=0"));
    CHECK(line("event=srt-live;source=inline-at-draw;","control-bytes-differ=0/416"));
    // The candidate table file next to the PNGs: every table line, hashed.
    {const std::string name=field("event=table;","file=");CHECK(!name.empty()&&line("event=table;","written=1;lines=2;"));
     std::vector<uint8_t> f;CHECK(readFile(pngDir+L"\\"+std::wstring(name.begin(),name.end()),f));
     const std::string text(f.begin(),f.end());CHECK(text.find("gpu-copy-step3-table;seq=1;ordinal=1;")!=std::string::npos);
     CHECK(text.find("gpu-copy-step3-table;seq=2;ordinal=2;")!=std::string::npos&&std::count(text.begin(),text.end(),'\n')==2);
     char sha[65];CHECK(acz::step1Sha256(f.data(),f.size(),sha)&&line("event=table;",sha));}
    step3Lines();
}
static void step3_no_visible() {
    // Every candidate behind the camera: REFUSED immediately, with the table.
    shape3.candidates=1;bootStep3();writeBehind();prepare3();
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);bind3(shape3.vpW,shape3.vpH);bindT10(srtOffBehind);
    l0->DrawIndexedInstanced(9,1,0,0,0);
    CHECK(!acz::step1Tick());
    if(!verdict("EXPERIMENT_RESULT=REFUSED;reason=no-visible-candidate")){dump();CHECK(false);}
    CHECK(line("gpu-copy-step3-table;seq=1;","status=gs-rejected")&&line("event=select;outcome=none-visible","visible=0"));
    CHECK(w->resourcesCreated==4&&!st1().recorded);
    step3Lines();
}
constexpr uint64_t srtOffRing=0x1600; // the next frame's SrtData slot (a ring of upload memory)
static void step3_ring_srt() {
    // D-223 live shape: at the inserting frame the chosen draw's SrtData sits at
    // ANOTHER VA and its content changed (the camera moved). The match ignores the
    // t10 VA; the blobs are derived inline from THAT draw's live bytes.
    bootStep3();prepare3();observeAndArm();
    {acz::Mat4 m0,m64,m256;sceneMatrices(m0,m64,m256);
     const auto t=acz::translation(0.05,0.0,0.0);m256=acz::multiply(m256,t);m64=acz::multiply(t,m64);m0=acz::multiply(t,m0);
     auto* srt=w->resources[2].data()+srtOffRing;std::memcpy(srt,w->resources[2].data()+srtOff,416);
     putMatrix(srt,m0);putMatrix(srt+64,m64);putMatrix(srt+256,m256);}
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);bind3(shape3.vpW,shape3.vpH);bindT10(srtOffRing);
    const auto before=allocations;traceAllocations=true;
    l0->DrawIndexedInstanced(9,1,0,0,0);traceAllocations=false;CHECK(allocations==before);
    CHECK(phase()==int(acz::Step1Phase::inserted)&&st1().s3.mismatches==0);
    const auto& s3=st1().s3;const uint8_t* live=w->resources[2].data()+srtOffRing;
    CHECK(std::memcmp(s3.inlineBlobs[0].data(),live,416)==0); // control: the exact raw bytes
    for(unsigned b=1;b<3;++b)for(size_t i=0;i<416;++i)
        if(!((i>=48&&i<64)||(i>=112&&i<128)))CHECK(s3.inlineBlobs[b][i]==live[i]); // eyes: only the two translation rows
    CHECK(std::memcmp(res(s3.upload).data(),live,416)==0); // the one write into the private UPLOAD slot
    CHECK(listAlias(0)->Close()==S_OK);ticks(1);execute({cl(0)});advance();CHECK(w->rasterised==4&&w->inconsistent==0);
    frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PASS")){dump();CHECK(false);}
    char vaText[32];std::snprintf(vaText,sizeof vaText,"va=%llx;",static_cast<unsigned long long>(gameVa(2)+srtOffRing));
    CHECK(line("event=srt-live;source=inline-at-draw;",vaText)&&line("event=srt-live;","control-bytes-differ=0/416"));
    CHECK(line("event=srt-delta;replay=left;","")&&line("event=clip;source=live-srt;replay=right;xyzw=",""));
    CHECK(line("event=stereo;","control-vs-original=equal:0"));
    const double expected=(2.0*gpu_copy::step3::half_ipd_units/sceneDepth)/0.6152*(shape3.vpW*0.5);
    CHECK(std::fabs(number(field("event=stereo;","disparity-px="))-expected)<1.0);
    step3Lines();
}
static void step3_live_unreadable() {
    // At the matched draw the live SrtData is in a resource the proxy has no mapped
    // view of (fake 3, unmapped): never consumed; REFUSED after the bounded wait.
    bootStep3();gameResource(3,D3D12_HEAP_TYPE_UPLOAD);mapGame(3,false);prepare3();observeAndArm();
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);bind3(shape3.vpW,shape3.vpH);
    l0->SetGraphicsRootShaderResourceView(3,gameVa(3)+srtOff);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);l3list0()->sources[0].address=gameAddr(3);}
    l0->DrawIndexedInstanced(9,1,0,0,0);
    CHECK(phase()==int(acz::Step1Phase::ready)&&st1().s3.freshUnreadable==1&&std::string_view(st1().s3.freshness)=="not-mapped");
    ticks(305);
    if(!verdict("EXPERIMENT_RESULT=REFUSED;reason=live-srt-unreadable-within-30s-after-arm")){dump();CHECK(false);}
    CHECK(!st1().recorded&&w->lists[0].copies==0);
    step3Lines();
}
static void step3_ring_overflow() {
    // 70 matched draws in one recording before the worker runs: 64 recorded, 6
    // dropped and counted, the Draw path never allocates or waits; the 45 s
    // bound closes the selection; the lowest visible ordinal wins the tie.
    shape3.candidates=100;bootStep3();prepare3();
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);bind3(shape3.vpW,shape3.vpH);
    const auto before=allocations;traceAllocations=true;
    for(int i=0;i<70;++i)l0->DrawIndexedInstanced(9,1,0,0,0);
    traceAllocations=false;CHECK(allocations==before);
    CHECK(st1().s3.recorded==64&&st1().s3.dropped==6);
    CHECK(acz::step1Tick()&&!st1().s3.observed); // evaluated, selection still open (cap 100)
    ticks(gpu_copy::step3::selection_ticks+2);
    CHECK(st1().s3.observed&&st1().s3.armed&&st1().s3.shape.draw_ordinal==1);
    if(!line("event=select;outcome=chosen;recorded=64;evaluated=64;dropped=6","visible=64;table-lines=64")){dump();CHECK(false);}
    size_t table=0;for(const auto& l:w->lines)if(l.rfind("gpu-copy-step3-table;",0)==0)++table;CHECK(table==64);
    step3Lines();
}
static void step3_eval_rules() {
    using namespace gpu_copy::step3;
    // A generic draw: 6 x 32-bit indices, list topology, base vertex 2, stride 32.
    acz::Mat4 m0,m64,m256;sceneMatrices(m0,m64,m256);std::array<uint8_t,416> srt{};putMatrix(srt.data()+64,m64);
    std::vector<uint8_t> vb(6*32,0);const float q[6][3]={{-0.2f,-0.1f,0},{0.2f,-0.1f,0},{0,0.1f,0},{-0.2f,-0.1f,4},{0.2f,-0.1f,4},{0,0.1f,4}};
    for(int i=0;i<6;++i)std::memcpy(vb.data()+32*i,q[i],12);
    const uint32_t idx[6]={0,1,2,3,4,5};Shape d;d.indexed=true;d.count=6;d.base=2;d.topology=4;d.strip_cut=0;
    CandidateEval e;CHECK(!evaluate_draw(srt.data(),vb.data(),2,6,32,reinterpret_cast<const uint8_t*>(idx),4,d,e));
    CHECK(e.triangles==2&&e.visible==1&&std::string_view(e.status)=="visible"&&e.wmin<0&&e.wmax>0&&e.area>0); // 2nd triangle behind
    {CandidateEval x;Shape g=d;g.base=0;CHECK(std::string_view(evaluate_draw(srt.data(),vb.data(),2,6,32,reinterpret_cast<const uint8_t*>(idx),4,g,x))=="index-outside-sampled-vertices");}
    // Strip with a 16-bit cut: 0,1,2,cut,3,4,5 -> two triangles.
    const uint16_t s16[7]={0,1,2,0xffff,3,4,5};Shape st;st.indexed=true;st.count=7;st.topology=5;st.strip_cut=1;
    CHECK(!evaluate_draw(srt.data(),vb.data(),0,6,32,reinterpret_cast<const uint8_t*>(s16),2,st,e)&&e.triangles==2&&e.visible==1);
    // Non-indexed strip from vertex 0.
    Shape ni;ni.indexed=false;ni.count=3;ni.start=0;ni.topology=5;CHECK(!evaluate_draw(srt.data(),vb.data(),0,6,32,nullptr,0,ni,e)&&e.visible==1);
    // Policy: visible beats rejected; larger area; then lowest ordinal; then earliest.
    CandidateEval a,b;a.visible=1;a.area=0.1;b.visible=0;b.area=0.5;CHECK(better(a,5,9,b,1,1)&&!better(b,1,1,a,5,9));
    b.visible=1;CHECK(better(b,9,9,a,1,1));b.area=0.1;CHECK(better(a,1,9,b,2,1)&&better(a,3,1,b,3,2));
}
// ------------------------------------------------------------- J-3b7f whole set
static Object overlayPsoObj;
static ID3D12PipelineState* overlayPso(){return reinterpret_cast<ID3D12PipelineState*>(&overlayPsoObj);}
// A visible variant of the scene (the mesh moved by d in world units) at `off` in fake 2.
static void writeVariant(uint64_t off,double dx,double dy) {
    acz::Mat4 m0,m64,m256;sceneMatrices(m0,m64,m256);
    const auto vp=acz::multiply(acz::inverse(m256),m64);
    m256=acz::multiply(m256,acz::translation(dx,dy,0));m64=acz::multiply(m256,vp);
    m0=acz::multiply(m64,acz::ps2ViewportMatrix(acz::PS2_SCALE_X,acz::PS2_SCALE_Y,acz::PS2_CENTRE,{1.0,0.0}));
    auto* srt=w->resources[2].data()+off;for(size_t i=0;i<416;++i)srt[i]=uint8_t(i*5+off);
    putMatrix(srt,m0);putMatrix(srt+64,m64);putMatrix(srt+256,m256);putParams(srt);
}
constexpr uint64_t offB=0x2000,offC=0x2200,offD=0x2400,offE=0x2600;
static void bootSet() {
    shape3.candidates=8;acz::g_step3WholeSetTest=true;bootStep3();writeBehind();
    writeVariant(offB,-3,0);writeVariant(offC,3,0);writeVariant(offD,0,2);writeVariant(offE,0,-2);
    overlayPsoObj.table=w->psoTable.data();overlayPsoObj.kind=k_pso;
    {auto& s=acz::state();std::lock_guard lock(s.mutex);auto& o=s.pipelines[overlayPso()];
     o=s.pipelines.at(reinterpret_cast<ID3D12PipelineState*>(&w->pso));o.output.blend0=true;} // same targets, blending on
    CHECK(st1().s3.wholeSet);
}
// One frame of 8 DrawCPT draws: 1/3/8 behind the camera, 2/4/7 visible (main PSO),
// 5 a visible blended overlay (second PSO), 6 visible but under another viewport
// (its targets differ: excluded). `upto` < 8 closes the list early.
struct SetFrame {std::array<UINT64,9> va{};};
static bool worldPerFrame=false;
static SetFrame frameSet(unsigned upto=8,bool unreadable4=false) {
    SetFrame f;auto* l0=listAlias(recList);CHECK(l0->Reset(allocator(recList),fakePso())==S_OK);bind3(shape3.vpW,shape3.vpH);
    if(worldPerFrame) {
        D3D12_RESOURCE_BARRIER b[2]{};
        for(auto& x:b){x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition.pResource=reinterpret_cast<ID3D12Resource*>(&w->resources[23].object);x.Transition.Subresource=0;}
        b[0].Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b[0].Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[1].Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        l0->ResourceBarrier(2,b);
    }
    const uint64_t offs[9]={0,srtOffBehind,srtOff,srtOffBehind,offB,offC,offD,offE,srtOffBehind};
    for(unsigned i=1;i<=upto;++i) {
        if(i==5)l0->SetPipelineState(overlayPso());
        if(i==6){l0->SetPipelineState(fakePso());const D3D12_VIEWPORT vp{0,0,128,72,0,1};l0->RSSetViewports(1,&vp);}
        if(i==7){const D3D12_VIEWPORT vp{0,0,shape3.vpW,shape3.vpH,0,1};l0->RSSetViewports(1,&vp);}
        if(i==4&&unreadable4) {
            l0->SetGraphicsRootShaderResourceView(3,gameVa(3)+srtOff);
            {auto& s=acz::state();std::lock_guard lock(s.mutex);l3list0()->sources[0].address=gameAddr(3);}
            f.va[i]=gameVa(3)+srtOff;
        } else {bindT10(offs[i]);f.va[i]=gameVa(2)+offs[i];}
        // Only the Draw path is allocation-checked (the game's own setter
        // tracking, e.g. SetPipelineState's retained reference, may allocate).
        const auto before=allocations;traceAllocations=true;
        l0->DrawIndexedInstanced(9,1,0,0,0);traceAllocations=false;CHECK(allocations==before);
    }
    return f;
}
static void armSet() {
    frameSet();CHECK(acz::step1Tick());
    if(!(st1().s3.armed&&has("event=select;outcome=set-chosen"))){dump();CHECK(false);}
    CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();
}
static void step3_set() {
    bootSet();prepare3();armSet();
    if(!line("event=set;members=4;excluded=1;","ordinals=2,4,5,7")){dump();CHECK(false);}
    CHECK(line("event=set;","commands=42(clears+8/member+6);cap=600")&&st1().s3.targets.commands==42);
    CHECK(w->resourcesCreated==10&&w->heapsCreated==6); // two RT/DS pairs, four CPU-only heaps (+2 OM ring)
    auto& L=w->lists[0];
    const auto f=frameSet(); // each Draw is allocation-checked inside
    CHECK(phase()==int(acz::Step1Phase::inserted)&&st1().s3.setRecorded==4&&st1().s3.issued.load()==42&&st1().s3.copiesRecorded);
    // Exact call sequence of the whole frame (game calls + inserted ones).
    std::vector<std::string> want{"reset","om","srv"};
    const std::vector<std::string> member{"om","srv","draw-indexed","om","srv","draw-indexed","srv","om"};
    for(unsigned i=1;i<=8;++i) {
        want.push_back("srv");
        const bool m=i==2||i==4||i==5||i==7;
        if(i==2)for(const char* c:{"clear-rtv","clear-rtv","clear-dsv","clear-dsv"})want.push_back(c); // once
        if(m)want.insert(want.end(),member.begin(),member.end());
        if(i==7)for(int k=0;k<2;++k)for(const char* c:{"barrier","copy-tex","barrier"})want.push_back(c);
        want.push_back("draw-indexed");
    }
    size_t lastReset=0;for(size_t i=0;i<L.events.size();++i)if(L.events[i]=="reset")lastReset=i;
    const std::vector<std::string> got(L.events.begin()+std::ptrdiff_t(lastReset),L.events.end());
    if(got!=want){for(auto& e:got)std::fprintf(stderr,"%s ",e.c_str());std::fprintf(stderr,"\n");CHECK(false);}
    // Arguments: left/right private targets with the member's upload slots; the
    // game's targets and SrtData restored before every original draw.
    const auto& s3=st1().s3;unsigned proxyDraws=0,gameDraws=0,clears=0;
    for(const auto& o:L.ops) {
        if(o.t==Op::clear_rtv||o.t==Op::clear_dsv)++clears;
        if(o.t!=Op::draw)continue;
        if(originOf(o.boundRtv)==gameRtvHandle){++gameDraws;CHECK(o.boundT10==f.va[gameDraws]&&originOf(o.boundDsv)==gameDsvHandle);continue;}
        const unsigned idx=proxyDraws/2,eye=proxyDraws%2;++proxyDraws;
        CHECK(o.boundRtv==(eye?s3.rtv2.ptr:s3.rtv.ptr)&&o.boundDsv==(eye?s3.dsv2.ptr:s3.dsv.ptr));
        CHECK(o.boundT10==s3.uploadVa+(2*idx+eye)*gpu_copy::step3::blob_stride);
    }
    CHECK(proxyDraws==8&&gameDraws==8&&clears==4);
    CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();CHECK(w->rasterised==8&&w->inconsistent==0);
    frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PASS;reason=whole-set-replayed")){dump();CHECK(false);}
    CHECK(line("EXPERIMENT_RESULT=PASS","cockpit-draws-only;world-sky-hud-not-replayed")&&line("EXPERIMENT_RESULT=PASS","srt-live-at-draw"));
    CHECK(line("event=set-replay;members=4;recorded=4;skipped=0;not-reached=0;stop=none;copies=after-last-member;issued=42;planned=42",""));
    CHECK(line("event=set-members;ordinal:status=2:recorded,4:recorded,5:recorded,7:recorded",""));
    // Per-eye composed images and the stereo estimate (all members at 30 units).
    const double expected=(2.0*gpu_copy::step3::half_ipd_units/sceneDepth)/0.6152*(shape3.vpW*0.5);
    const double xs=number(field("event=stereo-set;","xcorr-shift="));
    if(!(std::fabs(xs-expected)<1.5)){std::fprintf(stderr,"xcorr %f expected %f\n",xs,expected);dump();CHECK(false);}
    CHECK(std::fabs(number(field("event=stereo-set;","centroid-dx="))-expected)<1.5&&line("event=stereo-set;","sign=left-minus-right-positive"));
    CHECK(number(field("event=pixels;eye=left;","covered="))>2000&&number(field("event=stereo-set;","diff-pixels="))>0);
    // PNGs: raw and sRGB, full and small, for both eyes.
    for(const char* eye:{"left","right"}) {
        const std::string tag=std::string("eye=")+eye+";";
        for(const char* ev:{"event=png;","event=png-srgb;"})for(const char* key:{"png=","small="}) {
            const std::string name=field((ev+tag).c_str(),key);CHECK(!name.empty());
            std::vector<uint8_t> file;CHECK(readFile(pngDir+L"\\"+std::wstring(name.begin(),name.end()),file));
            uint32_t pw=0,ph=0;std::vector<uint8_t> rgba;CHECK(decodePng(file,pw,ph,rgba)&&pw==256&&ph==144);
            if(std::string(ev)=="event=png-srgb;")
                for(size_t i=0;i<rgba.size();i+=4)if(rgba[i+3])CHECK(rgba[i]==gpu_copy::step3::srgb8(200/255.0f)&&rgba[i+1]==gpu_copy::step3::srgb8(100/255.0f));
        }
    }
    for(int i=4;i<10;++i)CHECK(w->resources[size_t(i)].object.refs==0); // clean release
    step3Lines();
}
static void step3_set_partial() {
    // The list is Closed after ordinal 5: members 2/4/5 recorded, 7 never reached.
    // The copies are recorded at Close (still inside the recording): PARTIAL.
    bootSet();prepare3();armSet();
    frameSet(5);CHECK(st1().s3.setRecorded==3&&!st1().s3.copiesRecorded&&acz::g_step3SetActive.load()==1);
    auto& L=w->lists[0];const size_t before=L.events.size();
    CHECK(listAlias(0)->Close()==S_OK);
    const std::vector<std::string> copies{"barrier","copy-tex","barrier","barrier","copy-tex","barrier"};
    CHECK(std::vector<std::string>(L.events.begin()+std::ptrdiff_t(before),L.events.end())==copies);
    CHECK(st1().s3.copiesAtClose&&acz::g_step3SetActive.load()==0&&std::string_view(st1().s3.setStop)=="list-closed-before-members");
    execute({cl(0)});advance();frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PARTIAL;reason=set-partial")){dump();CHECK(false);}
    CHECK(line("event=set-replay;members=4;recorded=3;skipped=0;not-reached=1;stop=list-closed-before-members;copies=at-close;issued=34",""));
    CHECK(line("event=set-members;","7:pending"));
    step3Lines();
}
static void step3_set_unreadable() {
    // Member 4's live SrtData is unreadable (unmapped resource): further members
    // stop, the game's draws are untouched, the copies still happen at Close.
    bootSet();gameResource(3,D3D12_HEAP_TYPE_UPLOAD);mapGame(3,false);prepare3();armSet();
    const auto f=frameSet(8,true);
    CHECK(st1().s3.setRecorded==1&&std::string_view(st1().s3.setStop)=="live-srt-unreadable");
    unsigned proxy=0;for(const auto& o:w->lists[0].ops)if(o.t==Op::draw&&originOf(o.boundRtv)!=gameRtvHandle)++proxy;CHECK(proxy==2);
    for(const auto& o:w->lists[0].ops)if(o.t==Op::draw&&originOf(o.boundRtv)==gameRtvHandle)CHECK(originOf(o.boundDsv)==gameDsvHandle);
    CHECK(listAlias(0)->Close()==S_OK);CHECK(st1().s3.copiesAtClose);
    execute({cl(0)});advance();frame(1,false);ticks(10);
    if(!verdict("EXPERIMENT_RESULT=PARTIAL;reason=set-partial")){dump();CHECK(false);}
    CHECK(line("event=set-members;","2:recorded,4:not-mapped"));
    step3Lines();
}
static void step3_set_rules() {
    using namespace gpu_copy::step3;
    CHECK(set_commands(4,true)==42&&set_commands(64,true)==522&&set_commands(73,true)==594&&set_commands(74,true)>set_command_cap);
    CHECK(set_commands(1,false)==2+8+6);
    // linear -> sRGB
    CHECK(srgb8(0)==0&&srgb8(1)==255&&srgb8(2)==255&&srgb8(-1)==0&&srgb8(0.5f)==188&&srgb8(0.0031308f)==10&&srgb8(std::nanf(""))==0);
    // Cross-correlation disparity of a synthetic shifted image (FP16): a block at
    // x 90..109 in the left image and x 80..99 in the right -> +10 px.
    ColorFormat f;CHECK(color_format(dxgi::r16g16b16a16_float,f));
    const uint32_t W=200,H=40;const uint64_t pitch=W*8;std::vector<uint8_t> Limg(pitch*H,0),Rimg(pitch*H,0);
    auto put=[&](std::vector<uint8_t>& img,uint32_t x0){const uint16_t one[4]={0x3c00,0x3c00,0x3c00,0x3c00};
        for(uint32_t y=0;y<H;++y)for(uint32_t x=x0;x<x0+20;++x)std::memcpy(img.data()+y*pitch+x*8,one,8);};
    put(Limg,90);put(Rimg,80);
    XcorrResult r;CHECK(profile_disparity(Limg.data(),Rimg.data(),W,H,pitch,f,50,r)&&r.valid&&std::fabs(r.shift-10)<0.25&&r.score>0.99);
    XcorrResult r2;CHECK(profile_disparity(Rimg.data(),Limg.data(),W,H,pitch,f,50,r2)&&std::fabs(r2.shift+10)<0.25);
    // sRGB PNG variant of FP16 halves.
    std::vector<uint8_t> o;const uint16_t half[4]={0x3800,0x3a00,0x3c00,0x3800};std::vector<uint8_t> px(8);std::memcpy(px.data(),half,8);
    CHECK(to_srgb8(px.data(),1,1,8,f,o)&&o[0]==srgb8(0.5f)&&o[1]==srgb8(0.75f)&&o[2]==255&&o[3]==128);
    // finalize for two pairs: two full footprints, the 64-slot upload, doubled targets.
    Targets t;t.width=2560;t.height=1440;CHECK(color_format(dxgi::r16g16b16a16_float,t.color));t.has_depth=true;CHECK(depth_format(dxgi::d32_s8x24,t.depth));
    t.rt_bytes=2560ull*1440*8;t.ds_bytes=t.rt_bytes;
    CHECK(!finalize(t,0,0,2560,1440,2,2,set_upload_bytes)&&t.readback_bytes==2ull*20480*1440&&t.charged<=gpu_copy::Session::byte_cap);
}
// ------------------------------------------------------------- J-3b8 continuous producer
static std::string featureLog(const char* name);
struct FakePose final:acz::stereo::PoseProvider {
    bool available=true;acz::stereo::EyePose eyes[2]{};uint64_t time=1000;unsigned reads=0;
    bool try_read(acz::stereo::EyePose out[2],uint64_t* t) override {++reads;if(!available)return false;out[0]=eyes[0];out[1]=eyes[1];if(t)*t=time;return true;}
};
static FakePose fakePose;
static acz::stereo::EyePose eyePose(float x,float y,float z,float yawDeg,const float fov[4]) {
    acz::stereo::EyePose e{};e.position[0]=x;e.position[1]=y;e.position[2]=z;
    const double h=yawDeg*3.14159265358979323846/360.0;e.orientation[0]=0;e.orientation[1]=float(std::sin(h));e.orientation[2]=0;e.orientation[3]=float(std::cos(h));
    for(int i=0;i<4;++i)e.fov[i]=fov[i];return e;
}
// The game's own symmetric lens (fixture camera): tan 0.6152 x 0.3460.
static const float gameFov[4]={float(std::atan(-0.6152)),float(std::atan(0.6152)),float(std::atan(0.3460)),float(std::atan(-0.3460))};
static DXGI_FORMAT typelessDepth(DXGI_FORMAT f) {
    switch(f){case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:return DXGI_FORMAT_R32G8X24_TYPELESS;case DXGI_FORMAT_D32_FLOAT:return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:return DXGI_FORMAT_R24G8_TYPELESS;case DXGI_FORMAT_D16_UNORM:return DXGI_FORMAT_R16_TYPELESS;default:return f;}
}
// Test-only consumer: acquire latest, require fence completion, release.
struct FakeConsumer {
    uint64_t last=0;std::vector<uint64_t> ids;acz::stereo::StereoFrame held{};bool holding=false;
    bool acquire() {
        auto* src=acz::stereo::frame_source();if(!src)return false;
        acz::stereo::StereoFrame f;if(!src->try_acquire_latest(last,f))return false;
        CHECK(f.frame_id>last&&f.eye[0]&&f.eye[1]&&f.eye[0]!=f.eye[1]&&f.fence==reinterpret_cast<ID3D12Fence*>(&w->fence)&&f.fence_value);
        CHECK(f.width==uint32_t(shape3.vpW)&&f.height==uint32_t(shape3.vpH)&&f.members>=1);
        // D-236: per-eye depth (typeless for the consumer's SRV), lens.
        CHECK(f.depth[0]&&f.depth[1]&&f.depth[0]!=f.depth[1]&&f.depth_format==uint32_t(typelessDepth(shape3.dsv)));
        CHECK(res(f.depth[0]).format==typelessDepth(shape3.dsv)&&!(res(f.depth[0]).flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE));
        CHECK(res(f.depth[0]).state==D3D12_RESOURCE_STATE_DEPTH_WRITE&&res(f.eye[0]).state==D3D12_RESOURCE_STATE_RENDER_TARGET);
        CHECK(f.lens_valid&&std::fabs(f.lens_tan[0]-0.6152f)<1e-5f&&f.lens_tan[1]==f.lens_tan[0]&&std::fabs(f.lens_tan[2]-0.3460f)<1e-5f&&f.lens_tan[3]==f.lens_tan[2]);
        last=f.frame_id;ids.push_back(f.frame_id);held=f;holding=true;return true;
    }
    void release(){if(holding){acz::stereo::frame_source()->release(held);holding=false;}}
};
static void bootContinuous(bool world=false,D3D12_RESOURCE_FLAGS worldFlags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,bool prime=true);
static Resource& gameRt(){return w->resources[23];}
// One continuous game frame: the 8 draws, Close, Execute, GPU, a worker tick.
static void contFrame(bool executeIt=true,bool duplicate=false) {
    frameSet();CHECK(listAlias(0)->Close()==S_OK);
    if(executeIt){if(duplicate)execute({cl(0),cl(0)});else execute({cl(0)});}
    advance();(void)acz::step1Tick();
}
// J-3b9 game scene RT (fake 23): the RTV at gameRtvHandle names it.

static UINT gameRtArray=1; // J-3b9c: DepthOrArraySize of the game scene RT
// Exercise the actual committed-resource wrapper and its saved native dispatch.
// A fixture native returns the prepared game texture; private allocations still use deviceResource.
static void createGameTextureWrapped() {
    auto* texture=reinterpret_cast<ID3D12Resource*>(&gameRt().object);
    D3D12_RESOURCE_DESC desc{};resourceDesc(texture,&desc);
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    void* savedSlot=nullptr;void* savedOriginal=nullptr;
    {auto& s=acz::state();std::lock_guard lock(s.mutex);auto& hooks=s.devices.at(w->deviceTable.data());
     savedSlot=w->deviceTable[27];savedOriginal=hooks.originals[27];
     hooks.originals[27]=reinterpret_cast<void*>(&deviceResource);
     w->deviceTable[27]=reinterpret_cast<void*>(&acz::createBindingResource);}
    const unsigned before=textureCreationCalls;creationTexture=&gameRt().object;
    ID3D12Resource* result=nullptr;
    CHECK(device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr,IID_PPV_ARGS(&result))==S_OK);
    creationTexture=nullptr;CHECK(result==texture&&textureCreationCalls==before+1);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     w->deviceTable[27]=savedSlot;s.devices.at(w->deviceTable.data()).originals[27]=savedOriginal;}
}
static void registerGameRt(D3D12_RESOURCE_FLAGS flags) {
    auto& g=gameRt();g.object.table=w->worldTextureTable.data();g.dim=D3D12_RESOURCE_DIMENSION_TEXTURE2D;g.width=UINT64(shape3.vpW);g.height=UINT(shape3.vpH);g.format=shape3.rtv;
    g.heap=D3D12_HEAP_TYPE_DEFAULT;g.flags=flags;g.state=D3D12_RESOURCE_STATE_RENDER_TARGET;g.arraySize=gameRtArray;
    g.big.assign(size_t(g.width)*g.height*fakeBytes(g.format),0);
    for(size_t i=0;i<g.big.size();++i)g.big[i]=uint8_t(i*31+7); // "sky/terrain" drawn before the cockpit
    CHECK(g.object.table!=w->resourceTable.data());
    CHECK(w->worldTextureTable[2]==reinterpret_cast<void*>(&release));
    createGameTextureWrapped();
    CHECK(w->worldTextureTable[2]==reinterpret_cast<void*>(&acz::releaseMetadata));
    // L3 still keeps buffers only. World identity additionally requires Release coverage.
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(!s.l3.resources.get(gameAddr(23)));}
    acz::step3NoteView(reinterpret_cast<ID3D12Resource*>(&g.object),gameRtvHandle,false);
    setView(gameRtvHandle,&g.object); // the fake GPU checks the game RT state at the game's own draws
}
static void bootContinuous(bool world,D3D12_RESOURCE_FLAGS worldFlags,bool prime) {
    worldPerFrame=false;acz::g_step5Test=true;bootSet();
    w->lists[0].events.reserve(1u<<16);w->lists[0].ops.reserve(4096); // the fake's logs never grow inside a Draw
    if(world)registerGameRt(worldFlags);
    prepare3();
    frameSet();CHECK(acz::step1Tick()); // select + ring + continuous phase
    if(!(phase()==int(acz::Step1Phase::continuous)&&has("event=ring;mode=continuous;slots=3"))){dump();CHECK(false);}
    const bool worldReady=world&&!(worldFlags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)&&gameRtArray==1;
    // J-hud-capture: with gpu_copy_step5_hud (and a world source) 3 x (HUD RT + HUD DS) and their 6 CPU heaps.
    const unsigned hud=acz::g_hudReplayTest&&worldReady?6u:0u;
    // J-hud-colour: gpu_copy_step5_png_once adds ONE readback buffer (FP16 scene with HUD capture only).
    const unsigned png=acz::g_pngOnceTest&&hud&&shape3.rtv==DXGI_FORMAT_R16G16B16A16_FLOAT?1u:0u;
    // J-hud-markers: gpu_copy_step5_hud_markers adds 3 marker RTs and 3 CPU heaps (a failure-injected boot only bounds them).
    const unsigned markers=acz::g_hudMarkersTest&&hud?3u:0u;
    // J-effects-layer: gpu_copy_step5_effects adds 3 effects RTs + 1 predicate buffer and 3 CPU heaps.
    const unsigned effects=acz::g_effectsTest&&hud?4u:0u,effectHeaps=effects?3u:0u;
    // EFFECTS-OUTSIDE (C) fxmem:4 requested at ring creation (with effects_dual): 6 eye smoke RTs + 1 UPLOAD block and 6 CPU heaps.
    const unsigned smoke=effects&&acz::g_effectsDualTest&&gpu_copy::step5::effects_smoke_gpu_flag().load()?7u:0u,smokeHeaps=smoke?6u:0u;
    // EFFECTS-OUTSIDE (F) fxmem:6 requested at ring creation: 6 eye sprite RTs + 1 UPLOAD block and 6 CPU heaps.
    const unsigned sprite=effects&&acz::g_effectsDualTest&&gpu_copy::step5::effects_sprite_gpu_flag().load()?7u:0u,spriteHeaps=sprite?6u:0u;
    if(g_failResourceAt==UINT_MAX)CHECK(w->resourcesCreated==4+12+1+(worldReady?3u:0u)+hud+png+markers+effects+smoke+sprite&&w->heapsCreated==14+hud+markers+effectHeaps+smokeHeaps+spriteHeaps&&acz::stereo::frame_source());
    else CHECK(w->resourcesCreated<4+12+1+(worldReady?3u:0u)+hud+png+markers+effects&&acz::stereo::frame_source());
    CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance(); // the selection frame itself is not replayed
    CHECK(acz::stereo::frame_source()->frames_published()==0);
    if(worldReady&&prime) {
        // J-3b9c: one executed game list whose tracked transitions leave the scene RT in
        // RENDER_TARGET: later generations inherit a PROVEN entry state.
        auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);
        D3D12_RESOURCE_BARRIER b[2]{};auto* g=reinterpret_cast<ID3D12Resource*>(&gameRt().object);
        for(auto& x:b){x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition.pResource=g;x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
        b[0].Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b[0].Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[1].Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        l0->ResourceBarrier(2,b);CHECK(l0->Close()==S_OK);execute({cl(0)});advance();
        worldPerFrame=true; // future frameSet recordings prove their own prerequisite
        auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(s.step5RtCarriedKnown&&s.step5RtCarried==uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET)&&s.step5RtPending==0);
    }
}
static unsigned slotOf(const acz::stereo::StereoFrame& f){return f.slot;}
static std::string hudLog(); // the dedicated step5-producer log (below)
static size_t lastReset(const ListSim& L){size_t r=0;for(size_t i=0;i<L.events.size();++i)if(L.events[i]=="reset")r=i;return r;}
static void step5_continuous() {
    bootContinuous();
    CHECK(line("event=ring;","world=selection-game-rt-unresolved")); // the fixture's RTV is not tracked here
    auto* src=acz::stereo::frame_source();FakeConsumer c;
    // Frame 1: exact per-frame recording. The frame starts in the cockpit ordinal
    // range [2,7]; 2,3,4,5,7,8 are replayed (3 and 8: DrawCPT draws outside the
    // selection, replayed with live args), 6 refused alone (another viewport).
    // Clears once, 8 per member, NO copies.
    contFrame();
    {const auto& L=w->lists[0];const size_t r=lastReset(L);
     unsigned clears=0,copies=0,barriers=0,proxyDraws=0;
     for(size_t i=r;i<L.events.size();++i){clears+=L.events[i]=="clear-rtv"||L.events[i]=="clear-dsv";copies+=L.events[i].rfind("copy-tex",0)==0;barriers+=L.events[i]=="barrier";}
     for(const auto& o:L.ops)if(o.t==Op::draw&&originOf(o.boundRtv)!=gameRtvHandle)++proxyDraws;
     CHECK(clears==4&&copies==0&&barriers==0&&proxyDraws==12);}
    if(!(src->frames_published()==1&&w->signals==1&&w->fenceValue==1&&w->rasterised==12)){dump();CHECK(false);}
    if(!has("event=continuous-first;frame=1;fence-value=1;members=6;commands=52;")){dump();CHECK(false);}
    // EYE-RING off (eyeres:0 / absent: nothing requested): only the game's own RS calls, no eye-ring state or line
    {const auto* s5=acz::g_step5.load();CHECK(s5&&w->lists[0].rsCount==4&&!s5->eyeRing&&!s5->eyeRingAsked&&s5->ringW==256&&s5->gameW==256&&s5->perMember==8);}
    CHECK(has("event=continuous-definitions;produced=published"));
    CHECK(line("event=continuous-first;","lens-valid=1;lens-tan=0.61520,0.61520,0.34600,0.34600;world-valid=0"));
    CHECK(line("event=continuous-mismatch;n=1;of=1;outcome=produced;what=extra-ordinal;ordinal=3;","members=frame:6,selected:4"));
    // Consumer holds frame 1; frames 2 and 3 use the other slots.
    CHECK(c.acquire()&&c.ids.back()==1);const unsigned slotA=slotOf(c.held);auto heldA=c.held;c.holding=false;
    CHECK(!heldA.world&&!heldA.world_valid&&!heldA.partial&&heldA.members==6&&heldA.eye_width==0&&heldA.eye_height==0);
    contFrame();CHECK(c.acquire()&&c.ids.back()==2);const unsigned slotB=slotOf(c.held);auto heldB=c.held;c.holding=false;
    CHECK(slotA!=slotB&&w->fenceValue==2&&heldB.fence_value==2);
    contFrame();CHECK(src->frames_published()==3);
    // Frame 4: A and B held, C is the latest published -> no slot: skipped (never waits).
    const auto skippedBefore=src->frames_skipped();
    contFrame();CHECK(src->frames_published()==3&&src->frames_skipped()==skippedBefore+1);
    CHECK(w->signals==3); // nothing was submitted for the skipped frame
    // Release A: frame 5 reuses it; frame 6 reuses the superseded (not held) published slot.
    src->release(heldA);contFrame();CHECK(src->frames_published()==4);
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(3,f)&&f.frame_id==4&&f.slot==slotA);src->release(f);}
    contFrame();CHECK(src->frames_published()==5);
    {acz::stereo::StereoFrame f;CHECK(!src->try_acquire_latest(5,f));CHECK(src->try_acquire_latest(4,f)&&f.frame_id==5);src->release(f);}
    src->release(heldB);
    {acz::stereo::StereoFrame f;CHECK(!src->try_acquire_latest(5,f));}
    // Status lines (ordinary log) after 5 s of worker ticks: the skip reason is honest.
    ticks(60);
    if(!line("gpu-copy-step3-status;event=continuous;mode=continuous;published=5;skipped=1;skip=mismatch:0,no-slot:1,unqualified:0,unreadable:0;mismatch-produced=5;","avg-members=6.00")){dump();CHECK(false);}
    CHECK(line("event=continuous-quality;pose-valid=0/5;lens-valid=5/5;","world=valid:0,refused:5,last-refusal:selection-game-rt-unresolved"));
    CHECK(line("event=continuous-quality;","last-skip=no-free-slot;last-member-refusal=viewport-size-changed"));
    CHECK(phase()==int(acz::Step1Phase::continuous)&&verdicts()==0);
    CHECK(hudLog().find("eye-ring")==std::string::npos); // EYE-RING off: no line at all
    step3Lines();
}
static void step5_discard() {
    // The recording is Reset without being executed: the slot is freed, nothing published.
    bootContinuous();
    frameSet();CHECK(listAlias(0)->Close()==S_OK);
    CHECK(listAlias(0)->Reset(allocator(0),fakePso())==S_OK);(void)acz::step1Tick();
    CHECK(acz::stereo::frame_source()->frames_published()==0&&w->signals==0);
    CHECK(listAlias(0)->Close()==S_OK);contFrame();CHECK(acz::stereo::frame_source()->frames_published()==1);
    ticks(60);CHECK(has("discarded=1;"));
    step3Lines();
}
static void step5_ambiguity() {
    // Three consecutive frames submitted twice in one batch: each is ambiguous
    // (never published, slot quarantined); after 3 the producer is disabled.
    bootContinuous();
    contFrame(true,true);contFrame(true,true);
    CHECK(acz::stereo::frame_source()->frames_published()==0&&phase()==int(acz::Step1Phase::continuous));
    contFrame(true,true);
    CHECK(phase()==int(acz::Step1Phase::done));
    if(!verdict("EXPERIMENT_RESULT=FAIL;reason=3-consecutive-ambiguous-frames")){dump();CHECK(false);}
    CHECK(line("event=continuous-final;","ambiguous=3")&&line("event=continuous-final;","quarantined:3")&&w->signals==0);
    CHECK(line("EXPERIMENT_RESULT=FAIL","continuous-producer;"));
    // Later frames record nothing more.
    const auto r=w->rasterised;frameSet();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();CHECK(w->rasterised==r);
    step3Lines();
}
// J-3b9: the eye keeps the GAME'S lens; only the pose's rotation/translation is applied.
static void expectedEye(const acz::stereo::EyePose& e,const float p0[3],acz::Mat4& vp) {
    // Game camera of the fixture: identity orientation at sceneEye; the XR->game
    // mapping is the identity, scale 39.6 units per metre, yaw0 = 0.
    const double s=gpu_copy::step3::units_per_metre;
    const acz::Pose pose{{sceneEye[0]+s*(e.position[0]-p0[0]),sceneEye[1]+s*(e.position[1]-p0[1]),sceneEye[2]+s*(e.position[2]-p0[2])},
        {e.orientation[0],e.orientation[1],e.orientation[2],e.orientation[3]}};
    const acz::FovAngles fov{std::atan(-0.6152),std::atan(0.6152),std::atan(0.3460),std::atan(-0.3460)};
    vp=acz::multiply(acz::viewMatrixFromPose(pose),acz::projectionFromFov(fov,2.0,1000.0));
}
static void step5_pose() {
    // Pose-driven eyes: frame 1 neutral (fixes p0/yaw0), frame 2 head yaw 10 deg +
    // 0.1 m translation; the left eye's (asymmetric) XR FOV is ignored: the game lens
    // is kept. The member blobs in the slot's upload memory match cockpit_math.
    const float p0[3]={0.0f,1.2f,0.0f};
    fakePose.eyes[0]=eyePose(-0.0315f,1.2f,0,0,gameFov);fakePose.eyes[1]=eyePose(0.0315f,1.2f,0,0,gameFov);
    acz::stereo::set_pose_provider(&fakePose);
    bootContinuous();
    contFrame();
    if(!has("event=continuous-first;frame=1;fence-value=1;members=6;commands=52;pose=pose;pose-valid=1;control-zero-pose-bytes-differ=0/416")){dump();CHECK(false);}
    CHECK(line("event=continuous-first;","eyes=pose-full-substitution-game-lens"));
    {acz::stereo::StereoFrame f;auto* src=acz::stereo::frame_source();CHECK(src->try_acquire_latest(0,f)&&f.pose_valid&&f.predicted_display_time==1000);
     CHECK(f.pose[0].position[0]==-0.0315f&&f.pose[1].position[0]==0.0315f&&f.lens_valid);src->release(f);}
    const float asym[4]={-0.70f,0.55f,0.40f,-0.35f};
    fakePose.eyes[0]=eyePose(-0.0315f+0.1f,1.2f,-0.05f,10.0f,asym);fakePose.eyes[1]=eyePose(0.0315f+0.1f,1.2f,-0.05f,10.0f,gameFov);
    fakePose.time=2000;
    contFrame();
    acz::stereo::StereoFrame f;auto* src=acz::stereo::frame_source();CHECK(src->try_acquire_latest(1,f)&&f.pose_valid&&f.frame_id==2);
    auto* s5=acz::g_step5.load();CHECK(s5);
    const auto& sl=s5->slot[f.slot];const uint8_t* up=res(s5->upload).data()+sl.uploadOffset; // member 0: srtOff scene
    acz::Mat4 m0,m64,m256;sceneMatrices(m0,m64,m256);
    for(unsigned e=0;e<2;++e) {
        acz::Mat4 vpE;expectedEye(fakePose.eyes[e],p0,vpE);
        const auto e64=acz::multiply(m256,vpE);
        const auto e0=acz::multiply(e64,acz::ps2ViewportMatrix(acz::PS2_SCALE_X,acz::PS2_SCALE_Y,acz::PS2_CENTRE,{1.0,0.0}));
        float g64[16],g0[16];std::memcpy(g64,up+e*512+64,64);std::memcpy(g0,up+e*512,64);
        double s64=0,s0=0;for(int i=0;i<16;++i){s64=std::max(s64,std::fabs(e64[size_t(i)]));s0=std::max(s0,std::fabs(e0[size_t(i)]));}
        for(int i=0;i<16;++i) {
            if(std::fabs(g64[i]-e64[size_t(i)])>2e-5*s64||std::fabs(g0[i]-e0[size_t(i)])>2e-5*s0) {
                std::fprintf(stderr,"eye %u i %d: got %g/%g want %g/%g\n",e,i,g64[i],g0[i],e64[size_t(i)],e0[size_t(i)]);CHECK(false);
            }
        }
        // Rotation changes the whole matrix, not only row 3.
        float raw64[16];std::memcpy(raw64,w->resources[2].data()+srtOff+64,64);
        unsigned changedRows=0;for(int r=0;r<3;++r)for(int c=0;c<4;++c)changedRows+=g64[r*4+c]!=raw64[r*4+c];CHECK(changedRows>0);
    }
    src->release(f);
    // No pose: fallback to the IPD row-3 delta, pose_valid=false; the lens is still published.
    fakePose.available=false;contFrame();
    CHECK(src->try_acquire_latest(2,f)&&!f.pose_valid&&f.lens_valid);src->release(f);
    acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
static void step5_threads() {
    // Thread-safety smoke test: the game-side producer and a consumer thread.
    bootContinuous();
    std::atomic<bool> done{false};std::atomic<unsigned> got{0};std::atomic<bool> bad{false};
    std::thread consumer([&]{
        uncountedThread=true;uint64_t last=0;auto* src=acz::stereo::frame_source();
        while(!done.load()) {
            acz::stereo::StereoFrame f;
            if(src->try_acquire_latest(last,f)){if(f.frame_id<=last||!f.eye[0]||!f.depth[0])bad=true;last=f.frame_id;++got;std::this_thread::yield();src->release(f);}
            else std::this_thread::yield();
        }
    });
    // The producer's slot pick is TRY-ONLY by design (a held slot or the busy metadata mutex skips the generation), so how many of N
    // frames publish depends on OS scheduling of the consumer (a descheduled consumer holding a slot while the producer fires 40
    // frames back to back skipped >10 once in ~100 runs, 0xc0000409 via CHECK->abort). Drive until 40 frames PUBLISHED (bounded by
    // 4000 attempts, yielding after a skip) instead of asserting a scheduling-dependent ratio of a fixed count.
    {auto* ps=acz::stereo::frame_source();int attempts=0;
     while(ps->frames_published()<40&&attempts<4000){w->lists[0].events.clear();w->lists[0].ops.clear();contFrame();++attempts;if(ps->frames_published()<unsigned(attempts)&&attempts>=1)std::this_thread::yield();}} // keep the fake's logs bounded (capacity retained)
    done=true;consumer.join();
    CHECK(!bad.load()&&got.load()>0);
    auto* src=acz::stereo::frame_source();
    CHECK(src->frames_published()>=40&&src->frames_published()+src->frames_skipped()>=40);
    // Every slot is free or published afterwards (none stuck held/producing).
    auto* s5=acz::g_step5.load();for(const auto& sl:s5->slot)CHECK(sl.state==gpu_copy::step5::slot_free||sl.state==gpu_copy::step5::slot_published);
}
// J-3b9 varying frames: one list generation of the given draws (ordinal = position).
struct VarDraw {uint64_t off;int pso;UINT count;float vpW,vpH;int topo=-1;};
static Object altPsoObj;
static ID3D12PipelineState* altPso(){return reinterpret_cast<ID3D12PipelineState*>(&altPsoObj);}
static Object bcPsoObj; // J-cockpit-fov F3: a "second class" PSO (pso index 3 in VarDraw)
static ID3D12PipelineState* bcPso(){return reinterpret_cast<ID3D12PipelineState*>(&bcPsoObj);}
static Object acmPsoObj; // J-world-objects: a DrawACM-identity PSO (pso index 4 in VarDraw)
static ID3D12PipelineState* acmPso(){return reinterpret_cast<ID3D12PipelineState*>(&acmPsoObj);}
static std::vector<VarDraw> standardDraws() {
    const float W=shape3.vpW,H=shape3.vpH;
    return {{srtOffBehind,0,9,W,H},{srtOff,0,9,W,H},{srtOffBehind,0,9,W,H},{offB,0,9,W,H},{offC,1,9,W,H},{offD,0,9,128,72},{offE,0,9,W,H},{srtOffBehind,0,9,W,H}};
}
static std::function<void()> afterBind; // J-3b9: game activity between the OM bind and the draws
static std::function<void(unsigned)> beforeDraw;
static void frameVar(const std::vector<VarDraw>& draws) {
    auto* l0=listAlias(0);CHECK(l0->Reset(allocator(0),fakePso())==S_OK);bind3(shape3.vpW,shape3.vpH);
    if(afterBind)afterBind();unsigned ordinal=0;
    for(const auto& d:draws) {
        l0->SetPipelineState(d.pso==1?overlayPso():d.pso==2?altPso():d.pso==3?bcPso():d.pso==4?acmPso():fakePso());
        if(d.topo>=0)l0->IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY(d.topo));
        const D3D12_VIEWPORT vp{0,0,d.vpW,d.vpH,0,1};l0->RSSetViewports(1,&vp);
        bindT10(d.off);
        if(beforeDraw)beforeDraw(++ordinal);
        const auto before=allocations;traceAllocations=true;
        l0->DrawIndexedInstanced(d.count,1,0,0,0);traceAllocations=false;CHECK(allocations==before);
    }
}
static void varFrame(const std::vector<VarDraw>& draws) {
    w->lists[0].events.clear(); // bounded fake log (capacity kept: no Draw-path growth)
    frameVar(draws);CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();(void)acz::step1Tick();
}
static unsigned proxyDraws(){const auto& L=w->lists[0];unsigned n=0;for(const auto& o:L.ops)if(o.t==Op::draw&&originOf(o.boundRtv)!=gameRtvHandle)++n;return n;}
static void step5_varying() {
    // The J-3b8 live failure: member draw arguments / PSO change between frames, the
    // member count changes. Every one of these frames is now produced with the
    // LIVE draw arguments (the old exact first-member match skipped them).
    bootContinuous();
    altPsoObj.table=w->psoTable.data();altPsoObj.kind=k_pso;
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.pipelines[altPso()]=s.pipelines.at(reinterpret_cast<ID3D12PipelineState*>(&w->pso));}
    auto* src=acz::stereo::frame_source();
    // (a) first member with other draw args (count 6) and another PSO of the alternate set
    // (the overlay PSO, observed for selected ordinal 5) with the same signature class.
    auto d=standardDraws();d[1].count=6;d[1].pso=1;
    varFrame(d);CHECK(src->frames_published()==1&&src->frames_skipped()==0&&proxyDraws()==12);
    {unsigned n6=0;for(const auto& o:w->lists[0].ops)if(o.t==Op::draw&&originOf(o.boundRtv)!=gameRtvHandle&&o.dc==6)++n6;CHECK(n6==2);} // replayed with the live count
    CHECK(line("event=continuous-mismatch;n=1;","what=pso-changed;ordinal=2;")); // the first difference of the frame
    CHECK(line("event=continuous-mismatch;n=1;","args=sel:i9,1,0,0,0,live:i6,1,0,0,0;members=frame:6,selected:4"));
    // (b) fewer members: the list ends after ordinal 4.
    d=standardDraws();d.resize(4);
    varFrame(d);CHECK(src->frames_published()==2&&proxyDraws()==6);
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(1,f)&&f.members==3&&!f.partial);src->release(f);}
    // (c) more members: two extra DrawCPT draws after the last selected ordinal.
    d=standardDraws();d.push_back({offB,0,9,shape3.vpW,shape3.vpH});d.push_back({offC,1,9,shape3.vpW,shape3.vpH});
    varFrame(d);CHECK(src->frames_published()==3&&proxyDraws()==16);
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(2,f)&&f.members==8);src->release(f);}
    // (d) the first SELECTED member under another viewport: refused alone, the frame
    // starts at the next in-range draw and is published as partial.
    d=standardDraws();d[1].vpW=128;d[1].vpH=72;
    varFrame(d);CHECK(src->frames_published()==4&&proxyDraws()==10);
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(3,f)&&f.members==5&&f.partial);src->release(f);}
    // (e) a selected member with an unsupported state mid-frame (viewport): refused alone.
    d=standardDraws();d[3].vpW=64;d[3].vpH=64;
    varFrame(d);CHECK(src->frames_published()==5&&proxyDraws()==10);
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(4,f)&&f.members==5&&f.partial);src->release(f);}
    // (f) J-3b9c: a PSO NOT observed at selection, with EVERY class fact equal to the selected
    // member at the same ordinal: admitted at run time (logged once) and replayed.
    d=standardDraws();d[1].pso=2;
    varFrame(d);CHECK(src->frames_published()==6&&proxyDraws()==12);
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(5,f)&&f.members==6&&!f.partial);src->release(f);}
    CHECK(line("event=alt-pso-admit;",";ordinal=2;")&&line("event=alt-pso-admit;","alternates=3/16;compared=root:"));
    // (g) J-3b9: another signature class (topology) at a selected ordinal: refused alone.
    d=standardDraws();d[3].topo=D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;d[4].topo=0;
    varFrame(d);CHECK(src->frames_published()==7&&proxyDraws()==10);
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(6,f)&&f.members==5&&f.partial);src->release(f);}
    ticks(60);
    if(!line("event=continuous;mode=continuous;published=7;skipped=0;skip=mismatch:0,no-slot:0,unqualified:0,unreadable:0;mismatch-produced=7;","")){dump();CHECK(false);}
    CHECK(line("event=continuous-quality;","members=extra:6,fewer:1")); // a/c/d/e/f/g: 6/8/5/5/5/5 replayed vs 4 selected; b: 3
    CHECK(has("event=member-classes;classes=1;alternate-psos=2;"));
    {auto& st=st1();auto* s5=acz::g_step5.load();gpu_copy::step3::Shape x=st.s3.members[1].shape;x.topology=D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
     std::lock_guard lock(st.mutex);CHECK(std::string_view(acz::step5MemberWhy(st,*s5,x))=="signature-class-differs");} // (g)
    {acz::Step1State& s=st1();(void)s;auto* s5=acz::g_step5.load();CHECK(s5->classCount==1&&s5->altCount==3&&s5->admitCount==1);}
    {auto& st=st1();auto* s5=acz::g_step5.load();gpu_copy::step3::Shape x=st.s3.members[0].shape;x.pso_route=uint64_t(reinterpret_cast<uintptr_t>(altPso()));
     std::lock_guard lock(st.mutex);CHECK(!acz::step5MemberWhy(st,*s5,x));} // (f): admitted
    CHECK(phase()==int(acz::Step1Phase::continuous)&&verdicts()==0);
    step3Lines();
}
static void step5_skip() {
    // Skipped vs produced: a generation whose every in-range draw is refused is ONE
    // skipped frame (reason mismatch), counted at its Close, with a detail line.
    bootContinuous();
    auto* src=acz::stereo::frame_source();
    auto d=standardDraws();for(auto& x:d){x.vpW=64;x.vpH=64;}
    varFrame(d);CHECK(src->frames_published()==0&&src->frames_skipped()==1&&w->signals==0&&proxyDraws()==0);
    CHECK(line("event=continuous-mismatch;n=1;of=1;outcome=skipped;what=viewport-size-changed;ordinal=","members=frame:0,selected:4"));
    // Draws outside the cockpit range only: nothing seen, nothing skipped.
    d=standardDraws();d.resize(1);
    varFrame(d);CHECK(src->frames_published()==0&&src->frames_skipped()==1);
    // A normal frame afterwards.
    varFrame(standardDraws());CHECK(src->frames_published()==1&&src->frames_skipped()==1);
    // Rate limit: further skipped frames within 5 s add no detail line.
    for(int i=0;i<3;++i){d=standardDraws();for(auto& x:d){x.vpW=64;x.vpH=64;}varFrame(d);}
    CHECK(src->frames_skipped()==4);
    unsigned lines=0;for(const auto& l:w->lines)lines+=l.find("event=continuous-mismatch;")!=std::string::npos;CHECK(lines==1); // the produced frame's detail waits for the 5 s window
    ticks(60);CHECK(line("event=continuous;","skipped=4;skip=mismatch:4,no-slot:0"));
    unsigned after=0;for(const auto& l:w->lines)after+=l.find("event=continuous-mismatch;")!=std::string::npos;CHECK(after==1); // shared: rate limited
    acz::feature_log::flush();CHECK(featureLog("step5-producer").find("outcome=produced;what=extra-ordinal")!=std::string::npos); // dedicated: every detail
    step3Lines();
}
static void step5_world() {
    // World snapshot: the game's scene RT (fake 23, tracked RTV) is copied into the
    // slot's world texture at the first member, with an exact restore.
    bootContinuous(true);
    if(!line("event=ring;","world=ready;world-source=")){dump();CHECK(false);}
    auto* src=acz::stereo::frame_source();
    const auto before=gameRt().big;
    contFrame();
    const auto& L=w->lists[0];const size_t r=lastReset(L);
    // Order: ... om/srv setters ... then the 5 world commands, then the clears.
    std::vector<std::string> ev;for(size_t i=r;i<L.events.size();++i)if(L.events[i]=="barrier"||L.events[i]=="copy-tex-sub"||L.events[i]=="clear-rtv")ev.push_back(L.events[i]);
    CHECK(ev.size()==9&&ev[0]=="barrier"&&ev[1]=="barrier"); // same-generation game prerequisite
    ev.erase(ev.begin(),ev.begin()+2);
    CHECK(ev[0]=="barrier"&&ev[1]=="barrier"&&ev[2]=="copy-tex-sub"&&ev[3]=="barrier"&&ev[4]=="barrier"&&ev[5]=="clear-rtv");
    std::vector<const Op*> bars;for(const auto& o:L.ops)if(o.t==Op::barrier)bars.push_back(&o);
    CHECK(bars.size()==6&&bars[0]->res==&gameRt().object&&bars[1]->res==&gameRt().object);
    bars.erase(bars.begin(),bars.begin()+2);
    CHECK(bars[0]->res==&gameRt().object&&bars[0]->before==D3D12_RESOURCE_STATE_RENDER_TARGET&&bars[0]->after==D3D12_RESOURCE_STATE_COPY_SOURCE);
    CHECK(bars[1]->res!=&gameRt().object&&bars[1]->before==D3D12_RESOURCE_STATE_RENDER_TARGET&&bars[1]->after==D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(bars[2]->res==&gameRt().object&&bars[2]->before==D3D12_RESOURCE_STATE_COPY_SOURCE&&bars[2]->after==D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(bars[3]->res==bars[1]->res&&bars[3]->before==D3D12_RESOURCE_STATE_COPY_DEST&&bars[3]->after==D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The world copy precedes the first member's replays and the game's own first cockpit draw.
    {size_t firstBarrier=0,firstGameCockpit=0;for(size_t i=0;i<L.ops.size();++i){if(L.ops[i].t==Op::barrier&&!firstBarrier)firstBarrier=i+1;}
     unsigned gd=0;for(size_t i=0;i<L.ops.size();++i)if(L.ops[i].t==Op::draw&&originOf(L.ops[i].boundRtv)==gameRtvHandle&&++gd==2){firstGameCockpit=i+1;break;}
     CHECK(firstBarrier&&firstGameCockpit&&firstBarrier<firstGameCockpit);}
    // GPU: exact restore, the world texture holds the pre-cockpit game RT contents.
    CHECK(gameRt().state==D3D12_RESOURCE_STATE_RENDER_TARGET&&w->worldCopies==1);
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.world&&f.world_valid);
    auto& wr=res(f.world);CHECK(wr.state==D3D12_RESOURCE_STATE_RENDER_TARGET&&wr.width==gameRt().width&&wr.height==gameRt().height&&wr.format==gameRt().format);
    CHECK(wr.big.size()>=before.size()&&std::equal(before.begin(),before.end(),wr.big.begin()));
    src->release(f);
    if(!has("event=continuous-first;frame=1;fence-value=1;members=6;commands=57;")){dump();CHECK(false);}
    CHECK(line("event=world-snapshot;outcome=copied;frame=1;","barrier-calls=4(")&&line("event=world-snapshot;","restored=recorded-before-the-game-draw"));
    // Every slot has its own world texture.
    contFrame();contFrame();
    {std::set<Object*> worlds;for(const auto& sl:acz::g_step5.load()->slot)worlds.insert(obj(sl.world));CHECK(worlds.size()==3);}
    CHECK(w->worldCopies==3&&gameRt().state==D3D12_RESOURCE_STATE_RENDER_TARGET);
    ticks(60);CHECK(line("event=continuous-quality;","world=valid:3,refused:0"));
    step3Lines();
}
static void step5_world_changed() {
    // The RTV handle now names another resource: no game barrier, world_valid=false,
    // the eyes are still published.
    {
        bootContinuous(true);
        auto& other=w->resources[22];other=Resource{};other.object.table=w->resourceTable.data();other.object.kind=k_resource;
        other.dim=D3D12_RESOURCE_DIMENSION_TEXTURE2D;other.width=gameRt().width;other.height=gameRt().height;other.format=gameRt().format;
        other.heap=D3D12_HEAP_TYPE_DEFAULT;other.state=D3D12_RESOURCE_STATE_RENDER_TARGET;
        acz::step3NoteView(reinterpret_cast<ID3D12Resource*>(&other.object),gameRtvHandle,false);
        contFrame();
        // J-155 fixtures: frameSet records the game's OWN two prerequisite transitions (old RT);
        // nothing else: no injected barrier on either RT or a world texture.
        std::vector<const Op*> bars;for(const auto& o:w->lists[0].ops)if(o.t==Op::barrier)bars.push_back(&o);
        CHECK(bars.size()==2&&w->worldCopies==0);
        CHECK(bars[0]->res==&gameRt().object&&bars[0]->after==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        CHECK(bars[1]->res==&gameRt().object&&bars[1]->after==D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto* src=acz::stereo::frame_source();acz::stereo::StereoFrame f;
        CHECK(src->frames_published()==1&&src->try_acquire_latest(0,f)&&!f.world&&!f.world_valid&&f.eye[0]);src->release(f);
        CHECK(line("event=world-snapshot;outcome=refused;reason=game-rt-resource-changed;world-valid=0;eyes=published",""));
    }
    step3Lines();
}
static void step5_world_simultaneous() {
    // Simultaneous-access game RT: refused at arming (no world textures, no barrier).
    {
        bootContinuous(true,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
        CHECK(line("event=ring;","world=game-rt-simultaneous-access"));
        contFrame();
        unsigned bars=0;for(const auto& o:w->lists[0].ops)bars+=o.t==Op::barrier;CHECK(bars==0);
        CHECK(acz::stereo::frame_source()->frames_published()==1);
        CHECK(line("event=world-snapshot;outcome=refused;reason=game-rt-simultaneous-access",""));
    }
    step3Lines();
}
static void step5_float_depth() {
    // The live formats: R16G16B16A16_FLOAT + D32_FLOAT_S8X24_UINT. The replay depth is a
    // TYPELESS R32G8X24 resource (19) with a typed D32_FLOAT_S8X24_UINT DSV (20).
    shape3.rtv=DXGI_FORMAT_R16G16B16A16_FLOAT;shape3.dsv=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    bootContinuous(true);
    CHECK(dsvTyped==6&&line("event=ring;","ds=one-pair-per-slot,resource-format:19,dsv-format:20;")&&line("event=ring;","world=ready"));
    contFrame();
    FakeConsumer c;CHECK(c.acquire()&&c.held.depth_format==19u&&c.held.dxgi_format==10u&&c.held.world_valid);
    CHECK(res(c.held.depth[0]).format==DXGI_FORMAT_R32G8X24_TYPELESS&&res(c.held.depth[0]).flags==D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    CHECK(res(c.held.world).format==DXGI_FORMAT_R16G16B16A16_FLOAT&&w->worldCopies==1);
    c.release();
    step3Lines();
}
static void varFrame0(){frameSet();CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});(void)acz::step1Tick();} // no GPU progress
// J-3b9 review (issue 29) regressions.
static void perturbSrt(uint32_t frame) { // the game's SrtData bytes +128..+131 change every frame (live bytes are replayed)
    for(uint64_t off:{srtOff,offB,offC,offD,offE,srtOffBehind})std::memcpy(w->resources[2].data()+off+128,&frame,4);
}
static void step5_upload_retire() {
    // P1: the GPU lags several frames (no fence completion). A slot whose previous
    // recording has not COMPLETED is never re-recorded: earlier queued draws keep the
    // bytes they were submitted with (checked at fake "GPU execution").
    bootContinuous();
    auto* src=acz::stereo::frame_source();
    for(uint32_t i=1;i<=6;++i){perturbSrt(i);varFrame0();}
    CHECK(w->fenceValue==0);const auto skipped=src->frames_skipped();
    advance(); // the GPU now executes all queued work
    if(w->uploadClobbered){std::fprintf(stderr,"clobbered %u\n",w->uploadClobbered);CHECK(false);} // queued draws kept their bytes
    CHECK(skipped==3&&w->fenceValue==3); // frames 1-3 recorded into A,B,C; 4-6 skipped
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.frame_id==3&&f.fence_value==3&&f.last_signalled==3);src->release(f);}
    // Several more wraps with the GPU lagging by one frame, a consumer holding frame 4.
    perturbSrt(7);varFrame0();advance();
    acz::stereo::StereoFrame held;CHECK(src->try_acquire_latest(3,held)&&held.frame_id==4);
    for(uint32_t i=8;i<=20;++i){perturbSrt(i);varFrame0();if(i%2)advance();}
    advance();CHECK(w->uploadClobbered==0);
    CHECK(acz::g_step5.load()->slot[held.slot].frameId==4); // the held slot was never re-recorded
    src->release(held);
    ticks(60);CHECK(line("event=continuous-quality;","gpu-busy=")&&!line("event=continuous-quality;","gpu-busy=0;"));
    CHECK(line("event=continuous;","no-slot:"));
    step3Lines();
}
static void step5_pose_drop() {
    // P2: a later member's pose substitution fails: the WHOLE frame is never published.
    fakePose.eyes[0]=eyePose(-0.0315f,1.2f,0,0,gameFov);fakePose.eyes[1]=eyePose(0.0315f,1.2f,0,0,gameFov);
    acz::stereo::set_pose_provider(&fakePose);
    bootContinuous();
    auto* src=acz::stereo::frame_source();
    acz::g_step5PoseFailOrdinal=4;contFrame();
    CHECK(src->frames_published()==0&&src->frames_skipped()==1&&w->signals==1); // executed and signalled, not published
    {acz::stereo::StereoFrame f;CHECK(!src->try_acquire_latest(0,f));}
    acz::g_step5PoseFailOrdinal=0;contFrame();CHECK(src->frames_published()==1);
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.pose_valid&&f.fence_value==2);src->release(f);}
    ticks(60);CHECK(line("event=continuous-quality;","pose-dropped=1;"));
    acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
// xr_look_drive_keys (look_drive_keys.h): the producer wiring. Key off: untouched. Key on: the frame's rendered look is measured from the
// camera vs the cockpit matrix (identity in this fixture), published in StereoFrame::look_*, the matrices equal the key-off ones to float
// rounding (zero look), and one key decision per frame reaches the injector (fake here; nothing is ever sent).
struct LdFakeInjector final:acz::look_drive::KeyInjector {
    bool fg=true,kill=false;uint8_t cur=0;unsigned frames=0,leftDowns=0,rightDowns=0,upDowns=0,releases=0;
    bool foreground() noexcept override {return fg;}
    bool kill_switch() noexcept override {return kill;}
    void apply(uint8_t mask,double) noexcept override {
        ++frames;const uint8_t want=fg?mask:uint8_t(0);
        if((want&acz::look_drive::key_left)&&!(cur&acz::look_drive::key_left))++leftDowns;
        if((want&acz::look_drive::key_right)&&!(cur&acz::look_drive::key_right))++rightDowns;
        if((want&acz::look_drive::key_up)&&!(cur&acz::look_drive::key_up))++upDowns;
        cur=want;
    }
    bool release_all() noexcept override {if(cur)++releases;cur=0;return true;}
    uint8_t down() const noexcept override {return cur;}
};
static void step5_look_drive() {
    const float p0[3]={0.0f,1.2f,0.0f};(void)p0;
    fakePose.eyes[0]=eyePose(-0.0315f,1.2f,0,0,gameFov);fakePose.eyes[1]=eyePose(0.0315f,1.2f,0,0,gameFov);
    acz::stereo::set_pose_provider(&fakePose);
    bootContinuous();
    auto* s5=acz::g_step5.load();CHECK(s5);
    auto* src=acz::stereo::frame_source();
    contFrame(); // frame 1 fixes the neutral (key off)
    fakePose.eyes[0]=eyePose(-0.0315f,1.2f,-0.05f,10.0f,gameFov);fakePose.eyes[1]=eyePose(0.0315f,1.2f,-0.05f,10.0f,gameFov);fakePose.time=2000;
    contFrame();
    acz::stereo::StereoFrame f;
    CHECK(src->try_acquire_latest(1,f)&&f.pose_valid&&f.frame_id==2);
    CHECK(!f.look_valid&&f.look_yaw==0.0f&&f.look_pitch==0.0f);   // key off: nothing published, driver never configured
    CHECK(!s5->lookDrive.active());
    uint8_t off[1024];std::memcpy(off,res(s5->upload).data()+s5->slot[f.slot].uploadOffset,sizeof off);
    src->release(f);
    // key on, fake injector, same head pose
    static LdFakeInjector inj;inj.fg=true;
    s5->lookDrive.set_injector(&inj);
    acz::g_lookDriveTest=true;acz::g_lookDriveClockMs=10000.0;
    for(int i=0;i<40;++i){acz::g_lookDriveClockMs+=18.0;fakePose.time+=18;w->lists[0].events.clear();w->lists[0].ops.clear();contFrame();}
    CHECK(s5->lookDrive.active()&&!s5->lookDrive.sweep());
    CHECK(s5->lookDrive.measured().valid&&std::fabs(s5->lookDrive.measured().yaw)<1e-5&&std::fabs(s5->lookDrive.measured().pitch)<1e-5);
    CHECK(src->try_acquire_latest(0,f)&&f.pose_valid);
    CHECK(f.look_valid&&std::fabs(f.look_yaw)<1e-5f&&std::fabs(f.look_pitch)<1e-5f);   // published: the game looks straight ahead
    {
        uint8_t on[1024];std::memcpy(on,res(s5->upload).data()+s5->slot[f.slot].uploadOffset,sizeof on);
        // member 0 +64 / +0 matrices per eye (floats): equal to the key-off frame to float rounding (a zero look cancels exactly)
        for(unsigned e=0;e<2;++e)for(unsigned off0:{0u,64u}){
            float a[16],b[16];std::memcpy(a,off+e*512+off0,64);std::memcpy(b,on+e*512+off0,64);
            double scale=0;for(int i=0;i<16;++i)scale=std::max(scale,double(std::fabs(a[i])));
            for(int i=0;i<16;++i)CHECK(std::fabs(a[i]-b[i])<=2e-5*scale);
        }
    }
    src->release(f);
    // head yaw 10 degrees left -> the controller presses LEFT (and never right / up); frames reached the injector
    if(!(inj.leftDowns>=2&&inj.rightDowns==0&&inj.upDowns==0))std::fprintf(stderr,"look-drive: frames %u left %u right %u up %u why=%s\n",inj.frames,inj.leftDowns,inj.rightDowns,inj.upDowns,s5->lookDrive.controller().why());
    CHECK(inj.leftDowns>=2&&inj.rightDowns==0&&inj.upDowns==0);
    // foreground lost: no key is pressed and a held key is released at once
    inj.fg=false;const unsigned downs=inj.leftDowns;
    for(int i=0;i<10;++i){acz::g_lookDriveClockMs+=18.0;fakePose.time+=18;w->lists[0].events.clear();w->lists[0].ops.clear();contFrame();}
    CHECK(inj.cur==0&&inj.leftDowns==downs);
    inj.fg=true;
    // the pose provider goes away: keys released, nothing pressed
    fakePose.available=false;
    for(int i=0;i<10;++i){acz::g_lookDriveClockMs+=18.0;w->lists[0].events.clear();w->lists[0].ops.clear();contFrame();}
    CHECK(inj.cur==0);
    // the log line is bounded and carries the fields
    ticks(60);
    acz::g_lookDriveTest=false;acz::g_lookDriveClockMs=-1;fakePose.available=true;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
static void step5_look_sweep() {
    fakePose.eyes[0]=eyePose(-0.0315f,1.2f,0,0,gameFov);fakePose.eyes[1]=eyePose(0.0315f,1.2f,0,0,gameFov);
    acz::stereo::set_pose_provider(&fakePose);
    bootContinuous();
    auto* s5=acz::g_step5.load();CHECK(s5);
    auto* src=acz::stereo::frame_source();
    static LdFakeInjector inj;inj.fg=true;s5->lookDrive.set_injector(&inj);
    acz::g_lookDriveTest=true;acz::g_lookSweepTest=true;acz::g_lookDriveClockMs=1000.0;
    contFrame(); // sweep t = 0: identity; fixes the neutral at yaw 0
    acz::stereo::StereoFrame f;
    CHECK(src->try_acquire_latest(0,f)&&f.pose_valid);
    CHECK(std::fabs(f.pose[0].orientation[1])<1e-6f&&std::fabs(f.pose[0].orientation[3]-1.0f)<1e-6f);
    src->release(f);
    CHECK(s5->lookDrive.sweep());
    // 3 s later the scripted head is at +60 degrees (left): the provider's static pose is rotated by it, the frame carries the rotated poses
    acz::g_lookDriveClockMs=4000.0;fakePose.time=2000;contFrame();
    CHECK(src->try_acquire_latest(1,f)&&f.pose_valid&&f.frame_id==2);
    {
        double R[9];CHECK(acz::gpu_copy::step5::quat_rot(f.pose[0].orientation,R));
        double y=0,p=0;CHECK(acz::look_drive::head_angles(R,0.0,y,p));
        CHECK(std::fabs(y-60.0*3.14159265358979323846/180.0)<2e-5&&std::fabs(p)<2e-5);
        // the eyes stay 63 mm apart (rotated about the head centre)
        double d=0;for(int k=0;k<3;++k){const double v=double(f.pose[1].position[k])-f.pose[0].position[k];d+=v*v;}
        CHECK(std::fabs(std::sqrt(d)-0.063)<1e-5);
    }
    src->release(f);
    // the controller was fed the sweep, not the (static) head: left key
    for(int i=0;i<40;++i){acz::g_lookDriveClockMs+=18.0;fakePose.time+=18;w->lists[0].events.clear();w->lists[0].ops.clear();contFrame();}
    CHECK(inj.leftDowns>=2&&inj.rightDowns==0);
    acz::g_lookDriveTest=false;acz::g_lookSweepTest=false;acz::g_lookDriveClockMs=-1;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
static diagnostics::L3Observation::Resource* l3res(int r){return acz::state().l3.resources.get(gameAddr(r));}
static void step5_source() {
    // P2: the live SrtData read uses the J-131/J-142 coverage/currentness rules.
    bootContinuous();
    auto* src=acz::stereo::frame_source();uint64_t published=0;
    auto expectRefused=[&](const char* why){
        contFrame();CHECK(src->frames_published()==published);
        if(std::string_view(st1().s3.freshness)!=why){std::fprintf(stderr,"freshness %s want %s\n",st1().s3.freshness,why);CHECK(false);}
    };
    auto expectOk=[&]{contFrame();CHECK(src->frames_published()==++published);};
    expectOk();
    void* unmap=fakeMapTable[9];fakeMapTable[9]=reinterpret_cast<void*>(&unused); // Unmap hook replaced
    expectRefused("map-unmap-coverage-incomplete");fakeMapTable[9]=unmap;expectOk();
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.objects[fakeMapTable].originals[9]=nullptr;} // Unmap never hooked
    expectRefused("map-unmap-coverage-incomplete");
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.objects[fakeMapTable].originals[9]=reinterpret_cast<void*>(&unused);}expectOk();
    {auto& s=acz::state();std::lock_guard lock(s.mutex);++l3res(2)->map_generation;} // a remap the pin did not see
    expectRefused("upload-pin-map-mismatch");
    {auto& s=acz::state();std::lock_guard lock(s.mutex);--l3res(2)->map_generation;}expectOk();
    {auto& s=acz::state();std::lock_guard lock(s.mutex);l3res(2)->mapped_lifetime=0x9999;} // mapping of another lifetime
    expectRefused("map-lifetime-mismatch");
    {auto& s=acz::state();std::lock_guard lock(s.mutex);l3res(2)->mapped_lifetime=gameLife(2);}expectOk();
    {auto& s=acz::state();std::lock_guard lock(s.mutex);l3res(2)->outstanding=2;} // an outstanding second Map
    expectRefused("outstanding-map-count");
    {auto& s=acz::state();std::lock_guard lock(s.mutex);l3res(2)->outstanding=1;}expectOk();
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.nativeBindings.disable();} // observer disabled (permanent)
    expectRefused("binding-observer-disabled");
    ticks(60);if(!(line("event=continuous;","unreadable:6;")&&line("event=continuous-quality;","last-source=binding-observer-disabled"))){dump();CHECK(false);}
    step3Lines();
}
static std::string featureLog(const char* name);
static void step5_producer_dead() {
    // J-compat-race (headset test 2): the binding observer disables itself after frames were
    // published; every later frame is skipped `live-srt-unreadable` with freshness
    // `binding-observer-disabled`. The producer stays fail-closed (nothing re-arms) but says so
    // loudly: one producer-dead line at once, then at most one per minute.
    bootContinuous();
    auto* src=acz::stereo::frame_source();
    auto dead=[]{unsigned n=0;for(const auto& l:w->lines)n+=l.find("event=producer-dead;")!=std::string::npos;return n;};
    contFrame();contFrame();CHECK(src->frames_published()==2);
    ticks(60);CHECK(dead()==0); // a live observer never reports the producer dead
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.nativeBindings.disable("overlap:descriptor_write/committed_create:cross");}
    contFrame();CHECK(src->frames_published()==2);
    CHECK(std::string_view(st1().s3.freshness)=="binding-observer-disabled");
    ticks(1);
    if(!line("event=producer-dead;","reason=binding-observer-disabled;since-frame=2;published=2;")){dump();CHECK(false);}
    CHECK(line("event=producer-dead;","observer-reason=overlap:descriptor_write/committed_create:cross;line=1/16;recovery=none"));
    CHECK(dead()==1);
    contFrame();contFrame();ticks(100);CHECK(dead()==1&&src->frames_published()==2); // rate limited
    ticks(600);CHECK(dead()==2&&line("event=producer-dead;","line=2/16"));
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(!s.nativeBindings.enabled());} // never re-armed
    acz::feature_log::flush();
    CHECK(featureLog("step5-producer").find("producer-dead;reason=binding-observer-disabled;since-frame=2")!=std::string::npos);
    step3Lines();
}
static void gameBarrier(D3D12_RESOURCE_BARRIER_TYPE type,UINT sub,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after,D3D12_RESOURCE_BARRIER_FLAGS flags);
static std::string featureLog(const char* name) { // the dedicated log as a reader sees it while the game runs
    std::wstring path=pngDir+L"\\";for(const char* c=name;*c;++c)path.push_back(wchar_t(*c));path+=L".log";
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return {};
    std::string out;char buf[4096];DWORD n=0;while(ReadFile(h,buf,sizeof buf,&n,nullptr)&&n)out.append(buf,n);CloseHandle(h);return out;
}
static void makeTexture(Resource& r) { // a game texture the fake GPU can render into
    r.object.table=w->resourceTable.data();r.object.kind=k_resource;r.dim=D3D12_RESOURCE_DIMENSION_TEXTURE2D;r.width=UINT64(shape3.vpW);r.height=UINT(shape3.vpH);
    r.format=shape3.rtv;r.heap=D3D12_HEAP_TYPE_DEFAULT;r.state=D3D12_RESOURCE_STATE_RENDER_TARGET;r.big.assign(size_t(r.width)*r.height*fakeBytes(r.format),0);
}
static const Op* lastGameRestoreOm() {
    const Op* last=nullptr;for(const auto& o:w->lists[0].ops)if(o.t==Op::om&&o.count==1&&originOf(o.rtv[0])==gameRtvHandle&&o.rtv[0]!=gameRtvHandle)last=&o;return last;
}
static void step5_om_descriptor() {
    // J-3b9c (review 2): the restore binds PRIVATE bind-time copies of the OM descriptors.
    bootContinuous();
    auto* src=acz::stereo::frame_source();
    CHECK(acz::hookCaptureCommandList(listAlias(1))); // a second game list (other binds advance the ring)
    auto& A=w->resources[21];makeTexture(A);auto& B=w->resources[22];makeTexture(B);
    setView(gameRtvHandle,&A.object); // the game's RTV names A when it binds
    // (a) a LATE WRITER: the game rewrites its RTV (now naming B) after the bind, before the draws.
    afterBind=[&]{setView(gameRtvHandle,&B.object);acz::step3NoteView(reinterpret_cast<ID3D12Resource*>(&B.object),gameRtvHandle,false);};
    varFrame(standardDraws());CHECK(src->frames_published()==1);
    {const Op* r=lastGameRestoreOm();CHECK(r&&viewOf(r->rtv[0])==&A.object);} // restored contents: the bind-time view (A), not B
    // (b) HEAP RETIREMENT: the game's descriptor (heap) disappears between bind and draw.
    setView(gameRtvHandle,&A.object);
    afterBind=[&]{setView(gameRtvHandle,nullptr);};
    varFrame(standardDraws());CHECK(src->frames_published()==2);
    {const Op* r=lastGameRestoreOm();CHECK(r&&viewOf(r->rtv[0])==&A.object);}
    // (c) RTV/DSV copies after the bind no longer matter (the restore never reads the game heap).
    setView(gameRtvHandle,&A.object);
    afterBind=[]{acz::step3NoteDescriptorCopy(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);};
    varFrame(standardDraws());CHECK(src->frames_published()==3);
    // (d) COPY LIFETIME: more than half the ring of other binds after this list's bind: the
    // private slot may be reused before the restore, so the restore is refused (frame skipped).
    afterBind=[]{auto* l1=listAlias(1);CHECK(l1->Reset(allocator(1),fakePso())==S_OK);const D3D12_CPU_DESCRIPTOR_HANDLE rtv{gameRtvHandle},dsv{gameDsvHandle};
        for(int i=0;i<140;++i)l1->OMSetRenderTargets(1,&rtv,FALSE,&dsv);CHECK(l1->Close()==S_OK);};
    varFrame(standardDraws());CHECK(src->frames_published()==3&&src->frames_skipped()==1);
    acz::feature_log::flush();CHECK(featureLog("step5-producer").find("what=om-descriptor-unproved;")!=std::string::npos);
    // (e) an UNKNOWN descriptor (never written through the observed view hooks): not copied, refused.
    afterBind=[]{const D3D12_CPU_DESCRIPTOR_HANDLE rtv{0x7777000},dsv{gameDsvHandle};listAlias(0)->OMSetRenderTargets(1,&rtv,FALSE,&dsv);};
    varFrame(standardDraws());CHECK(src->frames_published()==3&&src->frames_skipped()==2);
    afterBind=nullptr;varFrame(standardDraws());CHECK(src->frames_published()==4);
    CHECK(w->descriptorCopies>0&&has("event=om-ring;status=ready;binds=256;rtv=2048;dsv=256;restore=private-bind-time-copies"));
    step3Lines();
}
static void step5_alt_admit() {
    // J-3b9c run-time alternate PSO admission (unit, on the armed producer).
    bootContinuous();
    auto& st=st1();auto* s5=acz::g_step5.load();std::lock_guard lock(st.mutex);
    const auto base=st.s3.members[0].shape;const unsigned before=s5->altCount;
    auto why=[&](gpu_copy::step3::Shape x)->std::string{const char* r=acz::step5MemberWhy(st,*s5,x);return r?r:"";};
    {auto x=base;x.pso_route=0x1234000;CHECK(why(x).empty()&&s5->altCount==before+1&&s5->admitCount==1);} // class-identical: admitted
    {auto x=base;x.pso_route=0x1234000;CHECK(why(x).empty()&&s5->altCount==before+1&&s5->admitCount==1);} // once only
    {auto x=base;x.pso_route=0x1235000;x.root_route^=0x10;CHECK(why(x)=="signature-class-differs");}
    {auto x=base;x.pso_route=0x1235000;x.topology=D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;CHECK(why(x)=="signature-class-differs");}
    {auto x=base;x.pso_route=0x1235000;x.ib_stride=4;CHECK(why(x)=="signature-class-differs");}
    {auto x=base;x.pso_route=0x1235000;x.vb_stride=32;CHECK(why(x)=="signature-class-differs");}
    {auto x=base;x.pso_route=0x1235000;x.pso.depth_write=!x.pso.depth_write;CHECK(why(x)=="signature-class-differs");}
    {auto x=base;x.pso_route=0x1235000;x.draw_ordinal=3;CHECK(why(x)=="pso-not-in-alternate-set");} // not a selected ordinal
    CHECK(s5->altCount==before+1);
    for(uint64_t k=0;s5->altCount<16;++k){auto x=base;x.pso_route=0x2000000+k*0x100;CHECK(why(x).empty());}
    {auto x=base;x.pso_route=0x3000000;CHECK(why(x)=="alternate-set-full"&&s5->admitRefusedFull==1);} // cap
}
// ------------------------------------------------------------- J-180 (Codex review P1): recordings outlive the single remembered Execute identity
// A submitted recording keeps writing its slot (private images + UPLOAD span) every time the game executes it again, until it is Reset or destroyed.
// The old producer remembered ONE identity (lastList): A -> B -> A went undetected. Now every submitted recording is owned (bounded table).
static void createList1() { // list 1 created THROUGH the hooked CreateCommandList (witnessed, qualifying), then closed
    createListIndex=1;ID3D12GraphicsCommandList* l=nullptr;
    CHECK(SUCCEEDED(device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator(1),nullptr,IID_PPV_ARGS(&l))));
    createListIndex=0;CHECK(l==listAlias(1)&&l->Close()==S_OK);
    w->lists[1].events.reserve(1u<<16);w->lists[1].ops.reserve(4096); // the fake's logs never grow inside a Draw (list 0 does the same in bootContinuous)
}
static void contFrameOn(int list) {
    if(list){w->lists[list].events.clear();w->lists[list].ops.clear();}
    recList=list;frameSet();CHECK(listAlias(list)->Close()==S_OK);execute({cl(list)});advance();(void)acz::step1Tick();recList=0;
}
static bool ownerLine(const char* needle) { // the dedicated log's `-owners` status line contains `needle`
    acz::feature_log::flush();const std::string t=featureLog("step5-producer");const std::string tag="event=continuous-owners;";
    for(size_t at=t.find(tag);at!=std::string::npos;at=t.find(tag,at+1)) {
        const size_t e=t.find('\n',at);if(t.substr(at,e==std::string::npos?std::string::npos:e-at).find(needle)!=std::string::npos)return true;
    }
    return false;
}
static unsigned liveOwners(acz::Step5State& s5){auto& st=st1();std::lock_guard lock(st.mutex);return acz::step5OwnerLive_locked(s5);}
static void step5_owner_aba() {
    bootContinuous();createList1();
    auto* src=acz::stereo::frame_source();auto* s5=acz::g_step5.load();
    contFrame();CHECK(src->frames_published()==1);          // A (list 0) executes and publishes
    const unsigned slotA=s5->lastSlot;const auto routeA=s5->lastList;
    contFrameOn(1);CHECK(src->frames_published()==2);       // B (list 1) executes and publishes: the remembered identity is now B
    const unsigned slotB=s5->lastSlot;CHECK(slotA!=slotB&&s5->lastList!=routeA&&liveOwners(*s5)==2);
    {auto& st=st1();std::lock_guard lock(st.mutex);CHECK(acz::step5OwnedSlot_locked(*s5,slotA)&&acz::step5OwnedSlot_locked(*s5,slotB)&&!acz::step5OwnedSlot_locked(*s5,3-slotA-slotB));}
    // A executes AGAIN before its Reset, the GPU not having advanced past B's signal (delayed completion).
    const auto signals=w->signals;const auto amb=s5->ambiguous;const auto fenceBefore=s5->fenceCounter;
    execute({cl(0)});
    CHECK(s5->ownerReexec==1&&s5->ambiguous==amb+1&&std::string_view(s5->lastFault)=="re-executed-recording");
    CHECK(s5->slot[slotA].state==gpu_copy::step5::slot_quarantined&&s5->slot[slotB].state==gpu_copy::step5::slot_published); // A's images never published again; B untouched
    CHECK(w->signals==signals+1&&s5->fenceCounter==fenceBefore+1&&s5->slot[slotA].lastUse==s5->fenceCounter);               // one correlated Signal for the occurrence
    {auto& st=st1();std::lock_guard lock(st.mutex);auto* a=acz::step5OwnerFind_locked(*s5,routeA);auto* b=acz::step5OwnerFind_locked(*s5,s5->lastList);
     CHECK(a&&b&&a->executes==2&&a->lastUse==s5->fenceCounter&&b->executes==1);}
    CHECK(w->fenceValue<s5->fenceCounter); // completion is delayed: the occurrence's value is not complete yet
    advance();CHECK(w->fenceValue==s5->fenceCounter&&s5->lastSignalled.load()==s5->fenceCounter);
    // And again (a second occurrence), then Reset: the entry is retired and the occurrences stay counted.
    execute({cl(0)});CHECK(s5->ownerReexec==2&&s5->ambiguous==amb+2);
    {auto& st=st1();std::lock_guard lock(st.mutex);auto* a=acz::step5OwnerFind_locked(*s5,routeA);CHECK(a&&a->executes==3);}
    {auto& st=st1();std::lock_guard lock(st.mutex);std::lock_guard m(s5->meta);bool busy=false;
     for(int i=0;i<3;++i)CHECK(acz::step5PickSlot_locked(*s5,UINT64_MAX-1,busy)!=int(slotA));}
    CHECK(listAlias(0)->Reset(allocator(0),fakePso())==S_OK);CHECK(liveOwners(*s5)==1&&s5->ownerRetired==1);CHECK(listAlias(0)->Close()==S_OK);
    ticks(60);CHECK(ownerLine("added=2;retired=1;re-executions=2;re-executions-of-held-slot=0;"));
}
static void step5_owner_batch() {
    // A and B execute together again: one occurrence each, ONE Signal covering the batch.
    bootContinuous();createList1();
    auto* src=acz::stereo::frame_source();auto* s5=acz::g_step5.load();
    contFrame();contFrameOn(1);CHECK(src->frames_published()==2);
    const auto signals=w->signals;const auto fence=s5->fenceCounter;
    execute({cl(0),cl(1)});
    CHECK(s5->ownerReexec==2&&w->signals==signals+1&&s5->fenceCounter==fence+1);
    unsigned quarantined=0;for(const auto& sl:s5->slot)quarantined+=sl.state==gpu_copy::step5::slot_quarantined;
    CHECK(quarantined==2); // both submitted slots were published: both re-rendered images are never published again
    advance();CHECK(w->fenceValue==s5->fenceCounter);
}
static void step5_owner_pin() {
    // A slot written by a live closed recording is never picked, even when its frame is superseded and its first submission completed.
    bootContinuous();createList1();
    auto* src=acz::stereo::frame_source();auto* s5=acz::g_step5.load();
    contFrame();const unsigned slotA=s5->lastSlot;const auto frameA=s5->slot[slotA].frameId;CHECK(src->frames_published()==1&&frameA==1);
    for(int i=0;i<6;++i){contFrameOn(1);CHECK(s5->lastSlot!=slotA);}   // list 1 is Reset every frame (its entry retired); A is alive
    CHECK(src->frames_published()==7&&s5->slot[slotA].frameId==frameA&&s5->slot[slotA].state==gpu_copy::step5::slot_published&&s5->ownerBlocked>0);
    CHECK(liveOwners(*s5)==2);
    // Destruction of A's list (the Release hook) ends its ability to write: the slot becomes pickable again.
    acz::step5NoteListReleased(s5->owner[0].live&&s5->owner[0].slot==slotA?s5->owner[0].route:s5->owner[1].route);
    CHECK(liveOwners(*s5)==1);
    contFrameOn(1);CHECK(s5->lastSlot==slotA&&src->frames_published()==8); // the oldest superseded published slot is reused now
    ticks(60);CHECK(ownerLine("slot-picks-blocked="));
}
static void step5_owner_held() {
    // The consumer holds A's frame when A executes again: a held image cannot be revoked; the slot is quarantined and never freed by the release.
    bootContinuous();createList1();
    auto* src=acz::stereo::frame_source();auto* s5=acz::g_step5.load();
    contFrame();acz::stereo::StereoFrame held;CHECK(src->try_acquire_latest(0,held)&&held.frame_id==1);
    const unsigned slotA=held.slot;CHECK(s5->slot[slotA].state==gpu_copy::step5::slot_held);
    CHECK(src->images_unchanged(held));                                 // DECOUPLE review 2: held, nothing written since publication
    contFrameOn(1);CHECK(src->frames_published()==2);
    CHECK(src->images_unchanged(held));                                 // another recording's frame does not touch A's images
    const auto fence=s5->fenceCounter;execute({cl(0)});                 // delayed completion: no advance()
    CHECK(s5->ownerReexec==1&&s5->ownerHeld==1&&s5->slot[slotA].state==gpu_copy::step5::slot_quarantined&&s5->slot[slotA].lastUse==fence+1&&w->fenceValue<=fence);
    // DECOUPLE review 2: the re-execution was recorded BEFORE its native Execute; the held frame's metadata no longer authenticates A's images
    CHECK(!src->images_unchanged(held));
    {acz::stereo::StereoFrame other=held;other.frame_id=held.frame_id+1;CHECK(!src->images_unchanged(other));}   // never a different frame id
    src->release(held);CHECK(s5->slot[slotA].state==gpu_copy::step5::slot_quarantined); // the release neither frees nor republishes it
    advance();
    {auto& st=st1();std::lock_guard lock(st.mutex);std::lock_guard m(s5->meta);bool busy=false;for(int i=0;i<3;++i)CHECK(acz::step5PickSlot_locked(*s5,UINT64_MAX-1,busy)!=int(slotA));}
    ticks(60);CHECK(ownerLine("re-executions-of-held-slot=1;"));
}
static void step5_acquire_quarantine_race() {
    // DECOUPLE review 3 (Astra P2): the consumer has SELECTED A (meta held) when the producer observes a re-execution of A on another thread: its
    // try_lock on meta fails and it stores QUARANTINED atomically before the native Execute. The acquisition must not erase that store.
    bootContinuous();
    auto* src=acz::stereo::frame_source();auto* s5=acz::g_step5.load();
    contFrame();CHECK(src->frames_published()==1);const unsigned slotA=s5->lastSlot;
    CHECK(s5->slot[slotA].state==gpu_copy::step5::slot_published);
    static unsigned seamSlot=UINT_MAX;seamSlot=UINT_MAX;
    acz::g_step5AcquireSeamTest=[](unsigned slot) {   // between candidate selection and acquisition, meta held by this (consumer) thread
        acz::g_step5AcquireSeamTest=nullptr;seamSlot=slot;
        std::thread producer([]{uncountedThread=true;execute({cl(0)});});   // the real step5BeforeExecute path: try_lock fails -> atomic store
        producer.join();
    };
    acz::stereo::StereoFrame f;
    CHECK(!src->try_acquire_latest(0,f));                                                     // the quarantine won: nothing acquired
    CHECK(seamSlot==slotA&&acz::g_step5AcquireSeamTest==nullptr&&s5->ownerReexec==1&&s5->acquireLostRace==1);
    CHECK(s5->slot[slotA].state==gpu_copy::step5::slot_quarantined);                          // never HELD over the quarantine
    for(int i=0;i<3;++i)CHECK(!src->try_acquire_latest(0,f));                                 // and never acquired later
    {acz::stereo::StereoFrame forged{};forged.frame_id=1;forged.slot=slotA;CHECK(!src->images_unchanged(forged));}
    advance();
}
static void step5_untracked_before_acquire() {
    // DECOUPLE review 3 (Astra P2): A is published, then an Execute containing A's owning recording goes UNEXAMINED (producer disabled) BEFORE any
    // acquisition. The proof is bound to the publication: A is never acquired, and a frame held across the same call is never reported unchanged.
    bootContinuous();createList1();
    auto* src=acz::stereo::frame_source();auto* s5=acz::g_step5.load();
    contFrame();const unsigned slotA=s5->lastSlot;
    contFrameOn(1);CHECK(src->frames_published()==2);
    acz::stereo::StereoFrame heldB;CHECK(src->try_acquire_latest(1,heldB)&&heldB.frame_id==2&&src->images_unchanged(heldB));
    const auto untracked=s5->untrackedExec.load();
    s5->disabled=true;execute({cl(0)});s5->disabled=false;                                    // forwarded without examining A's re-execution
    CHECK(s5->untrackedExec.load()==untracked+1&&s5->ownerReexec==0&&s5->slot[slotA].state==gpu_copy::step5::slot_published);
    acz::stereo::StereoFrame f;
    for(int i=0;i<3;++i){CHECK(!src->try_acquire_latest(0,f));CHECK(!src->images_unchanged(heldB));}   // repeated attempts: never authenticated
    src->release(heldB);
    for(int i=0;i<3;++i)CHECK(!src->try_acquire_latest(0,f));
    // A frame SUBMITTED after the unexamined call carries the new baseline: acquirable and unchanged.
    advance();contFrameOn(1);CHECK(src->frames_published()==3);
    CHECK(src->try_acquire_latest(2,f)&&f.frame_id==3&&src->images_unchanged(f));src->release(f);
}
static void step5_unexamined_phase() {
    // DECOUPLE review 3 (Astra open risk 2): fail closed at the Execute boundary. An Execute forwarded while the phase is not continuous is not
    // examined by step5BeforeExecute (no exception or callback involved): a held frame stays HELD but is no longer provably unchanged, also after the
    // phase is continuous again, and a published frame from before it is never acquired.
    bootContinuous();createList1();
    auto* src=acz::stereo::frame_source();auto* s5=acz::g_step5.load();
    contFrame();acz::stereo::StereoFrame held;CHECK(src->try_acquire_latest(0,held)&&held.frame_id==1&&src->images_unchanged(held));
    contFrameOn(1);CHECK(src->frames_published()==2);
    const auto untracked=s5->untrackedExec.load();
    acz::g_step1Phase.store(int(acz::Step1Phase::ready));execute({cl(0)});acz::g_step1Phase.store(int(acz::Step1Phase::continuous));
    CHECK(s5->untrackedExec.load()==untracked+1&&s5->ownerReexec==0&&s5->slot[held.slot].state==gpu_copy::step5::slot_held);
    CHECK(!src->images_unchanged(held));
    acz::stereo::StereoFrame f;CHECK(!src->try_acquire_latest(1,f));
    src->release(held);advance();
}
static void step5_owner_table_full() {
    // A full ownership table refuses NEW replay insertion; no executable writer is evicted.
    bootContinuous();createList1();
    auto* src=acz::stereo::frame_source();auto* s5=acz::g_step5.load();
    contFrame();CHECK(src->frames_published()==1);
    {auto& st=st1();std::lock_guard lock(st.mutex);
     unsigned added=0;for(auto& o:s5->owner)if(!o.live){o.live=true;o.route=0x7000+added;o.slot=0;o.generation=777;++added;} // fakes: generation 777
     CHECK(acz::step5OwnerLive_locked(*s5)==acz::step5_owner_cap);}
    const auto skipped=src->frames_skipped();
    contFrameOn(1); // a NEW recording (list 1; list 0 keeps its live entry: its own Reset would retire it)
    CHECK(src->frames_published()==1&&src->frames_skipped()==skipped+1&&s5->ownerRefused>=1&&std::string_view(s5->lastSkipWhy)=="owner-table-full");
    CHECK(liveOwners(*s5)==acz::step5_owner_cap); // the real owner (slot of frame 1) is still there
    {auto& st=st1();std::lock_guard lock(st.mutex);for(auto& o:s5->owner)if(o.live&&o.generation==777)o=acz::Step5Owner{};}
    CHECK(liveOwners(*s5)==1);
    contFrameOn(1);CHECK(src->frames_published()==2);
    ticks(60);CHECK(ownerLine("insertions-refused-table-full="));
}
static void step5_fence_rollover_held() {
    // J-3b9c (review 2, P1): a consumer holds an UNFINISHED frame across a fence replacement.
    bootContinuous();
    auto* src=acz::stereo::frame_source();
    varFrame0();CHECK(src->frames_published()==1&&w->fenceValue==0); // signalled, NOT complete
    acz::stereo::StereoFrame held;CHECK(src->try_acquire_latest(0,held)&&held.fence_epoch==1);
    st1().fence.fence=reinterpret_cast<ID3D12Fence*>(&w->fence2);(void)acz::step1Tick();
    auto* s5=acz::g_step5.load();CHECK(s5->fenceEpoch.load()==2&&s5->slot[held.slot].state==gpu_copy::step5::slot_held);
    src->release(held); // a release from the OLD epoch quarantines the slot instead of freeing it
    CHECK(s5->slot[held.slot].state==gpu_copy::step5::slot_quarantined);
    advance();for(int i=0;i<5;++i)contFrame();
    CHECK(s5->slot[held.slot].state==gpu_copy::step5::slot_quarantined); // never reused against the new fence
    {auto& st=st1();std::lock_guard lock(st.mutex);std::lock_guard m(s5->meta);bool busy=false;
     for(int i=0;i<3;++i)CHECK(acz::step5PickSlot_locked(*s5,UINT64_MAX-1,busy)!=int(held.slot));}
}
static void step5_world_entry() {
    // J-3b9c (review 2): the world copy needs a SUPPORTED entry state.
    bootContinuous(true,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,false); // no executed list has proven the state
    CHECK(acz::hookCaptureCommandList(listAlias(1)));
    auto* src=acz::stereo::frame_source();
    auto world=[&](uint64_t id){acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);const bool v=f.world_valid;src->release(f);return v;};
    // (a) unknown inherited state (a bound RTV is not evidence): refused.
    varFrame(standardDraws());CHECK(!world(1));
    CHECK(line("event=world-snapshot;outcome=refused;reason=entry-state-unknown;",""));
    // J-world-hold diagnostics: the refusal is split into its sub-condition (no barrier to RENDER_TARGET was tracked), counted, and logged with the pitch proxies.
    {auto* s5=acz::g_step5.load();CHECK(s5->worldEntryTotal==1&&s5->worldEntrySub[0]==1&&s5->refDetailCount==1&&s5->pitchRefused.n==1&&s5->pitchAccepted.n==0);
     acz::feature_log::flush();const std::string ded=featureLog("step5-producer"); // dedicated log only
     auto has=[&](const char* a,const char* b){size_t at=0;while((at=ded.find(a,at))!=std::string::npos){const size_t e=ded.find('\n',at);if(ded.substr(at,e-at).find(b)!=std::string::npos)return true;++at;}return false;};
     CHECK(has("event=world-refusal-detail;n=1;frame=","reason=entry-state-unknown;sub=no-transition-tracked;"));
     CHECK(has("event=world-refusal-definitions;","sub=the-first-failing-test-of-step3Capture"));
     CHECK(has("-world-refusal;","refused=1;entry-state-unknown=1;sub=no-transition-tracked:1;bursts=1;"));
     CHECK(has("-world-pitch;","accepted=n=0,head=none;refused=n=1,"));
     CHECK(!line("-world-refusal;","")&&!line("event=world-refusal-detail;","")); // never mirrored into the shared log
    }
    // (b) a correlated transition TO RENDER_TARGET earlier in this generation: accepted.
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    varFrame(standardDraws());CHECK(world(2));
    CHECK(line("event=world-snapshot;outcome=copied;","state-evidence=tracked-transition-to-RENDER_TARGET-in-generation"));
    // (c) even proven carried state cannot authorize commands recorded before Execute.
    afterBind=nullptr;varFrame(standardDraws());CHECK(!world(3));
    for(const auto& o:w->lists[0].ops)CHECK(o.t!=Op::barrier&&o.t!=Op::copy_tex);
    // (d) another generation touched the RT and is recorded but NOT executed: the carried state
    // is no longer proof -> refused; resetting it still cannot authorize inherited-state copies.
    {auto* l1=listAlias(1);CHECK(l1->Reset(allocator(1),fakePso())==S_OK);
     D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=reinterpret_cast<ID3D12Resource*>(&gameRt().object);
     b.Transition.Subresource=0;b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
     l1->ResourceBarrier(1,&b);CHECK(l1->Close()==S_OK);}
    varFrame(standardDraws());CHECK(!world(4));
    CHECK(listAlias(1)->Reset(allocator(1),fakePso())==S_OK);CHECK(listAlias(1)->Close()==S_OK);
    varFrame(standardDraws());CHECK(!world(5));
    for(const auto& o:w->lists[0].ops)CHECK(o.t!=Op::barrier&&o.t!=Op::copy_tex);
    CHECK(gameRt().state==D3D12_RESOURCE_STATE_RENDER_TARGET&&w->worldCopies==1);
    // (e) J-155 fix: the scene RT is a TEXTURE with no L3 creation record (as live); the watched
    // address's own incarnation keys the same-generation facts. A new object observed at that
    // address between the transition and the cockpit draw invalidates them: refused...
    CHECK(!acz::state().l3.resources.get(gameAddr(23)));
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        void* out=&gameRt().object;D3D12_RESOURCE_DESC d{};resourceDesc(reinterpret_cast<ID3D12Resource*>(&gameRt().object),&d);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;acz::l3Created(S_OK,&out,__uuidof(ID3D12Resource),&d,1,&hp);};
    varFrame(standardDraws());CHECK(!world(6));
    for(const auto& o:w->lists[0].ops)CHECK(o.t!=Op::copy_tex);
    // ...and the next generation's own qualifying transition (new incarnation) is accepted.
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    varFrame(standardDraws());CHECK(world(7));afterBind=nullptr;
    CHECK(gameRt().state==D3D12_RESOURCE_STATE_RENDER_TARGET&&w->worldCopies==2);
    step3Lines();
}
// Regression: independent texture vtable, real creation wrapper, real final-Release
// dispatch. Even a qualifying game transition cannot authorize a retired texture.
static void step5_world_texture_retirement() {
    bootContinuous(true,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,false);
    CHECK(gameRt().object.table!=w->resourceTable.data());
    uint64_t before=0;{auto& s=acz::state();std::lock_guard lock(s.mutex);
        before=acz::step5WatchSerial_locked();CHECK(before);}
    afterBind=[]{
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        auto* texture=reinterpret_cast<ID3D12Resource*>(&gameRt().object);
        CHECK(gameRt().object.refs==1&&texture->Release()==0);
        auto& s=acz::state();std::lock_guard lock(s.mutex);
        CHECK(!acz::step5WatchSerial_locked()&&!s.step5WatchSerial);
        CHECK(!s.step5WorldLifetimes.count(gameAddr(23)));
    };
    varFrame(standardDraws());afterBind=nullptr;
    CHECK(w->worldCopies==0&&gameRt().object.refs==0);
    unsigned barriers=0;for(const auto& op:w->lists[0].ops){CHECK(op.t!=Op::copy_tex);barriers+=op.t==Op::barrier;}
    CHECK(barriers==2); // exactly the two native game barriers, no injected world transitions
    CHECK(line("event=world-snapshot;outcome=refused;reason=world-texture-lifetime-uncovered;",""));
}
static void recordWorldTransition(unsigned,D3D12_RESOURCE_STATES,D3D12_RESOURCE_STATES,ID3D12Resource*);
static void step5_world_texture_release_reentry() {
    bootContinuous(true);CHECK(acz::hookCaptureCommandList(listAlias(1)));
    recordWorldTransition(1,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr);
    uint64_t before=0;{auto& s=acz::state();std::lock_guard lock(s.mutex);before=acz::step5WatchSerial_locked();CHECK(before);}
    duringFinalResourceRelease=[]{
        gameRt().object.refs=1; // a synthetic new object at the same address
        createGameTextureWrapped();
    };
    CHECK(reinterpret_cast<ID3D12Resource*>(&gameRt().object)->Release()==0);
    duringFinalResourceRelease=nullptr;
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     CHECK(acz::step5WatchSerial_locked()&&acz::step5WatchSerial_locked()!=before);
     CHECK(s.step5WorldLifetimes.count(gameAddr(23)));}
    execute({cl(1)});CHECK(!acz::state().step5RtCarriedKnown);advance();
    recordWorldTransition(1,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr);
    execute({cl(1)});CHECK(acz::state().step5RtCarriedKnown);advance();
    // An RTV observation of a different resource table also retires old facts,
    // even without a creation callback or an observed Release.
    recordWorldTransition(1,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr);
    static Table replacementTable;replacementTable=w->worldTextureTable;
    replacementTable[2]=reinterpret_cast<void*>(&release);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);before=acz::step5WatchSerial_locked();CHECK(before);}
    gameRt().object.table=replacementTable.data();
    auto* texture=reinterpret_cast<ID3D12Resource*>(&gameRt().object);
    acz::step3NoteView(texture,gameRtvHandle,false);
    uint64_t changed=0;{auto& s=acz::state();std::lock_guard lock(s.mutex);
        changed=acz::step5WatchSerial_locked();CHECK(changed&&changed!=before);
        CHECK(s.step5WorldLifetimes.at(gameAddr(23)).table==replacementTable.data());}
    CHECK(replacementTable[2]==reinterpret_cast<void*>(&acz::releaseMetadata));
    acz::step3NoteView(texture,gameRtvHandle,false); // same-table observation preserves identity
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(acz::step5WatchSerial_locked()==changed);}
    execute({cl(1)});CHECK(!acz::state().step5RtCarriedKnown);advance();
    recordWorldTransition(1,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr);
    execute({cl(1)});CHECK(acz::state().step5RtCarriedKnown);advance();
}
static void step5_world_texture_uncovered() {
    bootContinuous(true,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,false);
    // Simulate an uninstalled dispatch without dropping the registry's optimistic
    // metadata: proof must test the actual installed Release slot on every capture.
    w->worldTextureTable[2]=reinterpret_cast<void*>(&release);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(!acz::step5WatchSerial_locked());}
    afterBind=[]{
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);
    };
    varFrame(standardDraws());afterBind=nullptr;
    unsigned barriers=0;for(const auto& op:w->lists[0].ops){CHECK(op.t!=Op::copy_tex);barriers+=op.t==Op::barrier;}
    CHECK(barriers==2&&w->worldCopies==0&&gameRt().object.refs==1);
    CHECK(line("event=world-snapshot;outcome=refused;reason=world-texture-lifetime-uncovered;",""));
    // A texture never registered for lifetime coverage is unknown as well.
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.step5WorldLifetimes.erase(gameAddr(23));}
    // Production emits only the first refusal. Rearm only that diagnostic latch
    // between fixture cases so the missing-record case must emit its own reason.
    {auto* s5=acz::g_step5.load();CHECK(s5&&s5->worldRefusalEmitted&&!s5->worldRefusalPending);
        s5->worldRefusalEmitted=false;s5->worldFirstRefusal=nullptr;}
    w->lines.clear(); // the next refusal must be emitted for the missing-record case itself
    varFrame(standardDraws());
    for(const auto& op:w->lists[0].ops)CHECK(op.t!=Op::copy_tex&&op.t!=Op::barrier);
    CHECK(w->worldCopies==0);
    CHECK(line("event=world-snapshot;outcome=refused;reason=world-texture-lifetime-uncovered;",""));
    w->worldTextureTable[2]=reinterpret_cast<void*>(&acz::releaseMetadata);
}
// J-155: each scenario is a separate process, with real hook dispatch to fake natives.
static void step5_om_interleave() {
    bootContinuous();CHECK(acz::hookCaptureCommandList(listAlias(1)));
    auto& A=w->resources[21];makeTexture(A);auto& B=w->resources[22];makeTexture(B);
    constexpr SIZE_T otherRtv=0x9191000;
    setView(gameRtvHandle,&A.object);setView(otherRtv,&B.object);
    acz::step3NoteView(reinterpret_cast<ID3D12Resource*>(&B.object),otherRtv,false);
    auto* other=listAlias(1);CHECK(other->Reset(allocator(1),fakePso())==S_OK);
    w->lists[1].events.reserve(4096);w->lists[1].ops.reserve(4096);
    std::atomic<int> phase{0};
    std::thread recorder([&] {
        uncountedThread=true;
        while(phase.load()!=1)std::this_thread::yield();
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{otherRtv},dsv{gameDsvHandle};
        for(unsigned i=0;i<768;++i)other->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
        phase.store(2);
    });
    bool ran=false;
    duringClearRtv=[&] {
        if(ran)return;ran=true;
        // The first private clear happens AFTER validation, BEFORE native restore.
        {auto& s=acz::state();std::lock_guard lock(s.mutex);
         auto* pin=acz::g_step3OmPin;CHECK(pin&&pin->slot!=UINT_MAX&&s.step3Om.pins[pin->slot]==1);}
        phase.store(1);while(phase.load()!=2)std::this_thread::yield();
    };
    varFrame(standardDraws());recorder.join();duringClearRtv=nullptr;
    CHECK(ran&&acz::stereo::frame_source()->frames_published()==1);
    unsigned restored=0;
    for(const auto& o:w->lists[0].ops)if(o.t==Op::om&&o.count&&o.rtv[0]!=gameRtvHandle&&originOf(o.rtv[0])==gameRtvHandle) {
        CHECK(o.consumedRtv==&A.object);++restored;
    }
    CHECK(restored>=1);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);for(auto pins:s.step3Om.pins)CHECK(!pins);}
}
static void recordWorldTransition(unsigned list,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after,ID3D12Resource* resource=nullptr) {
    auto* l=listAlias(int(list));CHECK(l->Reset(allocator(int(list)),fakePso())==S_OK);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=resource?resource:reinterpret_cast<ID3D12Resource*>(&gameRt().object);
    b.Transition.Subresource=0;b.Transition.StateBefore=before;b.Transition.StateAfter=after;
    l->ResourceBarrier(1,&b);CHECK(l->Close()==S_OK);
}
static void step5_world_reselect_identity() {
    bootContinuous(true);CHECK(acz::hookCaptureCommandList(listAlias(1)));
    recordWorldTransition(1,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto& B=w->resources[22];makeTexture(B);
    // J-159: B is a real render target (an RTV requires ALLOW_RENDER_TARGET) with its own
    // independent, still-native texture table. Its lifetime coverage comes ONLY from the
    // production RTV observation below, never from A's record.
    static Table bTable;bTable=w->worldTextureTable;bTable[2]=reinterpret_cast<void*>(&release);
    B.object.table=bTable.data();B.flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    uint64_t serialA=0;{auto& s=acz::state();std::lock_guard lock(s.mutex);serialA=acz::step5WatchSerial_locked();CHECK(serialA);
        CHECK(!s.step5WorldLifetimes.count(gameAddr(22)));} // A's coverage says nothing about B
    acz::step3NoteView(reinterpret_cast<ID3D12Resource*>(&B.object),gameRtvHandle,false);setView(gameRtvHandle,&B.object);
    CHECK(bTable[2]==reinterpret_cast<void*>(&acz::releaseMetadata));
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     CHECK(s.step5WorldLifetimes.count(gameAddr(22))&&s.step5WorldLifetimes.at(gameAddr(22)).table==bTable.data());
     CHECK(acz::step5WatchSerial_locked()==serialA);} // observing B does not disturb A's watch
    auto& st=st1();st.s3.shape.game_rt=gameAddr(22);CHECK(!acz::step5Arm(st));
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     CHECK(!s.step5RtCarriedKnown&&s.step5RtPending==0&&!s.states.at(listAlias(1)).step3.rtTransition);
     CHECK(acz::step5WatchSerial_locked()&&acz::step5WatchSerial_locked()!=serialA);} // a new selection is a new incarnation
    // A's previously closed recording is still forwarded, but cannot become B evidence.
    const unsigned before=w->executes;execute({cl(1)});CHECK(w->executes==before+1);
    CHECK(!w->gpuOps.empty()&&w->gpuOps.back().res==&gameRt().object);
    CHECK(!acz::state().step5RtCarriedKnown);advance();
    varFrame(standardDraws());
    for(const auto& o:w->lists[0].ops)CHECK(o.t!=Op::barrier&&o.t!=Op::copy_tex);
    CHECK(w->worldCopies==0);
    // Lifetime reuse at the same address invalidates same-watch recording facts.
    recordWorldTransition(1,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,reinterpret_cast<ID3D12Resource*>(&B.object));
    // A new object observed at the watched address through the creation seam (l3Created, as
    // every hooked creation calls it; textures included) is a new incarnation.
    uint64_t serialB=0;{auto& s=acz::state();std::lock_guard lock(s.mutex);serialB=acz::step5WatchSerial_locked();}
    {void* out=&B.object;D3D12_RESOURCE_DESC d{};resourceDesc(reinterpret_cast<ID3D12Resource*>(&B.object),&d);
     D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;acz::l3Created(S_OK,&out,__uuidof(ID3D12Resource),&d,1,&hp);}
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(acz::step5WatchSerial_locked()&&acz::step5WatchSerial_locked()!=serialB);}
    execute({cl(1)});CHECK(!acz::state().step5RtCarriedKnown);
    // The same transition facts recorded for the CURRENT incarnation are accepted again.
    recordWorldTransition(1,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,reinterpret_cast<ID3D12Resource*>(&B.object));
    execute({cl(1)});
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(s.step5RtCarriedKnown&&s.step5RtCarried==D3D12_RESOURCE_STATE_RENDER_TARGET);}
}
// J-159: re-selecting a texture whose Release lifetime is NOT covered yields no identity:
// A's coverage and recorded facts cannot qualify B, and the world snapshot is refused by name.
// The production RTV observation of B as a render target then covers B itself; it is a fresh
// incarnation, and facts recorded while B was uncovered still cannot qualify it.
static void step5_world_reselect_uncovered() {
    bootContinuous(true);CHECK(acz::hookCaptureCommandList(listAlias(1)));
    recordWorldTransition(1,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto& B=w->resources[22];makeTexture(B); // flags NONE: the RTV observation cannot cover it
    static Table bTable;bTable=w->worldTextureTable;bTable[2]=reinterpret_cast<void*>(&release);B.object.table=bTable.data();
    auto* texB=reinterpret_cast<ID3D12Resource*>(&B.object);
    uint64_t serialA=0;{auto& s=acz::state();std::lock_guard lock(s.mutex);serialA=acz::step5WatchSerial_locked();CHECK(serialA);}
    acz::step3NoteView(texB,gameRtvHandle,false);setView(gameRtvHandle,&B.object);
    CHECK(bTable[2]==reinterpret_cast<void*>(&release)); // no dispatch installed
    auto& st=st1();st.s3.shape.game_rt=gameAddr(22);CHECK(!acz::step5Arm(st));
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     CHECK(acz::g_step5WorldWatch.load()==gameAddr(22)&&!s.step5WorldLifetimes.count(gameAddr(22)));
     CHECK(!acz::step5WatchSerial_locked()&&!s.step5WatchSerial);}
    // A's closed recording is forwarded but is no B evidence; neither is an uncovered B recording.
    execute({cl(1)});CHECK(!acz::state().step5RtCarriedKnown);advance();
    varFrame(standardDraws());
    for(const auto& o:w->lists[0].ops)CHECK(o.t!=Op::copy_tex);
    CHECK(w->worldCopies==0);
    CHECK(line("event=world-snapshot;outcome=refused;reason=world-texture-lifetime-uncovered;",""));
    recordWorldTransition(1,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,texB);
    // The game's RTV for B (a real render target) is observed: B's own coverage, a new serial.
    B.flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;acz::step3NoteView(texB,gameRtvHandle,false);
    CHECK(bTable[2]==reinterpret_cast<void*>(&acz::releaseMetadata));
    uint64_t serialB=0;{auto& s=acz::state();std::lock_guard lock(s.mutex);serialB=acz::step5WatchSerial_locked();
     CHECK(serialB&&serialB!=serialA&&s.step5WorldLifetimes.at(gameAddr(22)).table==bTable.data());}
    execute({cl(1)});CHECK(!acz::state().step5RtCarriedKnown);advance(); // recorded while uncovered
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(acz::step5WatchSerial_locked()==serialB);}
    recordWorldTransition(1,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,texB);
    execute({cl(1)});
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(s.step5RtCarriedKnown&&s.step5RtCarried==D3D12_RESOURCE_STATE_RENDER_TARGET);}
}
static void step5_world_foreign_queue() {
    bootContinuous(true);CHECK(acz::hookCaptureCommandList(listAlias(1)));
    recordWorldTransition(1,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto* foreign=reinterpret_cast<ID3D12CommandQueue*>(&w->otherQueue);CHECK(acz::hookCommandQueue(foreign));
    ID3D12CommandList* batch[]={cl(1)};w->executeCount=1;w->executeArray=batch;
    const auto before=w->executes;foreign->ExecuteCommandLists(1,batch);w->executeArray=nullptr;
    CHECK(w->executes==before+1&&!acz::state().step5RtCarriedKnown);
    CHECK(!w->gpuOps.empty()&&w->gpuOps.back().t==Op::barrier&&w->gpuOps.back().after==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    advance();
}
static void step5_world_reexecute() {
    bootContinuous(true);CHECK(acz::hookCaptureCommandList(listAlias(1)));
    recordWorldTransition(1,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    execute({cl(1)});advance();CHECK(acz::state().step5RtCarried==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    recordWorldTransition(0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    execute({cl(0)});advance();CHECK(acz::state().step5RtCarried==D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto before=w->executes;execute({cl(1)});CHECK(w->executes==before+1);
    CHECK(acz::state().step5RtCarriedKnown&&acz::state().step5RtCarried==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CHECK(!w->gpuOps.empty()&&w->gpuOps.back().t==Op::barrier&&w->gpuOps.back().after==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    advance();CHECK(gameRt().state==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
static void step5_world_preinject() {
    bootContinuous(true);worldPerFrame=false;CHECK(acz::hookCaptureCommandList(listAlias(1)));
    CHECK(acz::state().step5RtCarriedKnown); // old code would inject based on this
    frameVar(standardDraws());CHECK(listAlias(0)->Close()==S_OK);
    unsigned gameDraws=0;
    for(const auto& o:w->lists[0].ops){CHECK(o.t!=Op::barrier&&o.t!=Op::copy_tex);gameDraws+=o.t==Op::draw&&originOf(o.boundRtv)==gameRtvHandle;}
    CHECK(gameDraws==8);
    // Another recording/submission changes the state AFTER frame recording, BEFORE Execute.
    recordWorldTransition(1,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    execute({cl(1)});advance();CHECK(gameRt().state==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const auto before=w->executes;execute({cl(0)});CHECK(w->executes==before+1);
    unsigned forwardedGameDraws=0;
    for(const auto& o:w->gpuOps){CHECK(o.t!=Op::barrier&&o.t!=Op::copy_tex);forwardedGameDraws+=o.t==Op::draw&&originOf(o.boundRtv)==gameRtvHandle;}
    CHECK(forwardedGameDraws==8&&w->worldCopies==0); // game's array/list forwarded unchanged
    // Do not run the fake GPU: the game's own draws now have an invalid entry state.
}
static void step5_world_array() {
    // J-3b9c (review 2): a NULL-description RTV of a 2-slice Texture2D array covers both slices:
    // not subresource 0 -> no world texture (the eyes still run).
    gameRtArray=2;
    bootContinuous(true,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,false);
    CHECK(line("event=ring;","world=game-rtv-not-subresource-0"));
    contFrame();CHECK(acz::stereo::frame_source()->frames_published()==1&&w->worldCopies==0);
    step3Lines();
}
static void feature_log_unit() {
    pngDir=makeScratchDir(L"acz-flog-test");
    acz::feature_log::set_directory(pngDir.c_str());
    // A previous run's file is kept as <name>.prev.log.
    {std::wstring p=pngDir+L"\\unit-c.log";HANDLE h=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);DWORD n=0;WriteFile(h,"old\n",4,&n,nullptr);CloseHandle(h);}
    const auto a=acz::feature_log::open("unit-a");CHECK(a!=acz::feature_log::invalid&&acz::feature_log::open("unit-a")==a);
    CHECK(acz::feature_log::open("bad/name")==acz::feature_log::invalid);
    acz::feature_log::write(a,"one");acz::feature_log::write(a,"two");
    CHECK(featureLog("unit-a").empty()); // nothing written before a flush (no I/O on write)
    acz::feature_log::flush();
    const auto t=featureLog("unit-a"); // readable while open (FILE_SHARE_READ)
    CHECK(t.find("|one\n")!=std::string::npos&&t.find("|two\n")!=std::string::npos&&acz::feature_log::stats(a).written_lines==2);
    // Periodic flush: tick() flushes once 5 s have passed.
    acz::feature_log::write(a,"three");acz::feature_log::tick(GetTickCount64());CHECK(featureLog("unit-a").find("|three")==std::string::npos);
    acz::feature_log::tick(GetTickCount64()+6000);CHECK(featureLog("unit-a").find("|three")!=std::string::npos);
    // Bounded queue: 600 lines without a flush -> 512 kept, 88 dropped and reported.
    const auto b=acz::feature_log::open("unit-b");
    char line[64];for(int i=0;i<600;++i){std::snprintf(line,sizeof line,"line-%d",i);acz::feature_log::write(b,line);}
    CHECK(acz::feature_log::stats(b).dropped==88);
    acz::feature_log::flush();
    {const auto x=featureLog("unit-b");CHECK(x.find("|line-511\n")!=std::string::npos&&x.find("|line-512\n")==std::string::npos&&x.find("event=dropped;n=88")!=std::string::npos);}
    // Cap: one marker line, then dropped (unrotated).
    acz::feature_log::set_cap(300);
    const auto c=acz::feature_log::open("unit-c");
    for(int i=0;i<40;++i)acz::feature_log::write(c,"0123456789012345678901234567890123456789");
    acz::feature_log::flush();
    { /* J-157: the cap leaves room for the fixed cap marker and one incomplete-evidence marker beyond the payload budget; the bound no longer depends on the tick-count digits */ const auto x=featureLog("unit-c");CHECK(x.size()<=300+40+256&&x.find("feature-log;event=cap-reached")!=std::string::npos&&acz::feature_log::stats(c).capped);}
    {std::wstring p=pngDir+L"\\unit-c.prev.log";CHECK(GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES);}
    acz::feature_log::set_cap(0);
    std::printf("feature-log: unit checks passed\n");
}
static void step5_feature_log() {
    // The dedicated step5-producer.log is the evidence of record (status lines every 5 s).
    bootContinuous();
    for(int i=0;i<3;++i)contFrame();
    ticks(60);acz::feature_log::flush();
    const auto x=featureLog("step5-producer");
    for(const char* k:{"|gpu-copy-step1;event=ring;","|gpu-copy-step1;event=member-classes;","event=continuous-definitions;",
            "|gpu-copy-step1-status;event=continuous;mode=continuous;published=3","event=continuous-quality;","event=continuous-mismatch;n=1;"})
        if(x.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    step3Lines();
}

// J-171: real wrapper dispatch with fake objects; unchanged native tail commands.
static bool hudSameOp(const Op& a,const Op& b) {
    return a.t==b.t&&a.count==b.count&&a.single==b.single&&a.rtv==b.rtv&&a.dsvSet==b.dsvSet&&a.dsv==b.dsv&&
        a.param==b.param&&a.va==b.va&&a.depth==b.depth&&a.clearFlags==b.clearFlags&&a.consumedRtv==b.consumedRtv&&
        a.res==b.res&&a.before==b.before&&a.after==b.after&&a.dst==b.dst&&a.offset==b.offset&&a.w==b.w&&a.h==b.h&&
        a.pitch==b.pitch&&a.format==b.format&&a.dc==b.dc&&a.di==b.di&&a.ds==b.ds&&a.dsi==b.dsi&&a.db==b.db&&
        a.indexed==b.indexed&&a.box==b.box&&a.bl==b.bl&&a.bt==b.bt&&a.br==b.br&&a.bb==b.bb&&a.subres==b.subres&&
        a.snapped==b.snapped&&a.snap==b.snap&&a.boundRtv==b.boundRtv&&a.boundCount==b.boundCount&&
        a.boundDsv==b.boundDsv&&a.boundDsvSet==b.boundDsvSet&&a.boundT10==b.boundT10;
}
static void STDMETHODCALLTYPE hudNativeDraw(ID3D12GraphicsCommandList* self,UINT count,UINT instances,UINT start,UINT first) {
    listDraw(self,count,instances,start,first);
    auto& l=w->lists[obj(self)->list];Op o;o.t=Op::draw;o.dc=count;o.di=instances;o.ds=start;o.dsi=first;
    o.boundRtv=l.omCount?l.rtv[0]:0;o.boundCount=l.omCount;o.boundDsvSet=l.dsvSet;o.boundDsv=l.dsv;o.boundT10=l.roots[3];o.boundT5=l.roots[5];l.ops.push_back(o);
}
static void step5_hud_observe() {
    bootContinuous(true);
    // Make the existing fake non-indexed native record an Op too, for equality.
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.lists.at(w->lists[0].tables[0].data()).originals[12]=reinterpret_cast<void*>(&hudNativeDraw);}
    constexpr SIZE_T outputHandle=0x17100;
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     auto& p=s.pipelines[fakePso(false)];p=s.pipelines.at(fakePso());p.drawcpt_identity_matched=false;
     std::strcpy(p.hudVs.data(),"f4cfd928c11d0a822baeb92e09767134");std::strcpy(p.hudPs.data(),"e1372ac10d7191c7e4d1c03b66465260");
     auto& v=s.step3Views[127];v={};v.handle=outputHandle;v.resource=gameAddr(22);v.described=true;v.format=24;v.serial=++s.step3ViewSerial;}
    auto tailScript=[&] {
        auto* l=listAlias(0);const auto begin=w->lists[0].ops.size();
        // Explicit game bind makes native handles identical across recordings;
        // producer restore descriptors otherwise rotate independently of this key.
        const D3D12_CPU_DESCRIPTOR_HANDLE scene{gameRtvHandle},depth{gameDsvHandle};
        l->OMSetRenderTargets(1,&scene,FALSE,&depth);l->SetPipelineState(fakePso(false));
        auto draw=[&](bool indexed) {const auto n=allocations;
            if(indexed)l->DrawIndexedInstanced(6,1,0,0,0);else l->DrawInstanced(3,1,2,0);
            CHECK(allocations==n);};
        draw(true);draw(false); // scene-target provisional tail, both draw wrappers
        const D3D12_CPU_DESCRIPTOR_HANDLE h{outputHandle};l->OMSetRenderTargets(1,&h,FALSE,nullptr);
        draw(true);l->OMSetRenderTargets(1,&h,FALSE,nullptr); // repeated bind must not duplicate events
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource=reinterpret_cast<ID3D12Resource*>(&gameRt().object);
        b.Transition.Subresource=0;b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        l->ResourceBarrier(1,&b);l->ResourceBarrier(1,&b);
        CHECK(l->Close()==S_OK);
        return std::vector<Op>(w->lists[0].ops.begin()+begin,w->lists[0].ops.end());
    };
    frameSet();const auto baseline=tailScript();
    CHECK(!acz::g_hud.frames&&!acz::g_hud.head&&!acz::g_hud.runCount);
    CHECK(featureLog("step5-producer").find("hud;")==std::string::npos);
    acz::g_hudTest=true;frameSet();const auto observed=tailScript();
    CHECK(observed.size()==baseline.size());
    for(size_t i=0;i<baseline.size();++i)CHECK(hudSameOp(baseline[i],observed[i]));
    unsigned tails=0,scene=0,other=0,change=0,r10=0,barriers=0,closes=0;
    {std::lock_guard lock(acz::g_hudMutex);for(auto i=acz::g_hud.tail;i<acz::g_hud.head;++i) {
        const auto& r=acz::g_hud.queue[i%acz::gpu_copy::hud::queue_records];
        using E=acz::gpu_copy::hud::Event;
        if(r.event==E::tail){++tails;scene+=r.rt_known&&r.rt==r.scene;other+=r.rt_known&&r.rt!=r.scene;CHECK(r.member==8);}
        change+=r.event==E::rt_change;r10+=r.event==E::r10;barriers+=r.event==E::scene_barrier;closes+=r.event==E::close;
    }}
    CHECK(tails==3&&scene==2&&other==1&&change==1&&r10==1&&barriers==1&&closes==1);
    acz::hudDrain(acz::g_step5.load()->log);acz::feature_log::flush();const auto text=featureLog("step5-producer");
    for(auto* k:{"hud;back_buffer=unknown;event=tail;","rt_class=scene;","rt_class=other;","rt_format=24;fmt_r10g10b10a2=1;",
                "hud-state;back_buffer=unknown;event=tail;","after_member=8;provisional=1;","hud;back_buffer=unknown;event=scene_barrier;","hud;back_buffer=unknown;event=close;"})CHECK(text.find(k)!=std::string::npos);
    // A later accepted DrawCPT member follows a provisional tail in this generation.
    frameSet();auto* l=listAlias(0);l->SetPipelineState(fakePso(false));l->DrawInstanced(3,1,2,0);
    l->SetPipelineState(fakePso());l->DrawIndexedInstanced(9,1,0,0,0);CHECK(l->Close()==S_OK);
    unsigned resumed=0;{std::lock_guard lock(acz::g_hudMutex);acz::gpu_copy::hud::Record r;while(acz::g_hud.pop(r))resumed+=r.event==acz::gpu_copy::hud::Event::member_resume;}
    CHECK(resumed==1);
}

// ------------------------------------------------------------- J-hud-capture: HUD replay producer
// The post-cockpit tail of the frame: after frameSet()'s 8 DrawCPT draws (members), the game binds
// its scene target again and issues `n` non-DrawCPT HUD draws (alternating indexed / non-indexed,
// PSO `hudPso`), then binds the output target (R10G10B10A2 view, fake 22) and draws there, then
// transitions the scene target RENDER_TARGET -> PIXEL_SHADER_RESOURCE (and back, for the fake GPU).
constexpr SIZE_T hudOutputHandle=0x17100;
static Object hudRootObj,hudPsoObj2;
static ID3D12PipelineState* hudPso2(){return reinterpret_cast<ID3D12PipelineState*>(&hudPsoObj2);}
static void hudPipeline(ID3D12PipelineState* pso,const std::function<void(gpu_copy::step3::PsoOutput&)>& edit) {
    auto& s=acz::state();std::lock_guard lock(s.mutex);
    auto& p=s.pipelines[pso];p=s.pipelines.at(fakePso());p.drawcpt_identity_matched=false;
    auto& o=p.output;o.depth_enable=true;o.depth_func=gpu_copy::step3::cmp::always;o.depth_write=false;o.blend0=true;o.write_mask0=15;
    o.stencil=true;o.stencil_read=o.stencil_write=255;o.front_func=o.back_func=gpu_copy::step3::cmp::always;o.front_pass=3;
    if(edit)edit(o);
}
static void hudSetup(bool key) {
    acz::g_hudReplayTest=key;bootContinuous(true);
    hudPsoObj2.table=w->psoTable.data();hudPsoObj2.kind=k_pso;
    hudPipeline(fakePso(false),nullptr);hudPipeline(hudPso2(),nullptr);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     // The non-indexed fake native records an Op too (game and replay draws alike).
     s.lists.at(w->lists[0].tables[0].data()).originals[12]=reinterpret_cast<void*>(&hudNativeDraw);
     auto& v=s.step3Views[127];v={};v.handle=hudOutputHandle;v.resource=gameAddr(22);v.described=true;v.format=24;v.serial=++s.step3ViewSerial;}
}
static void hudSetRoot() { // the game's bound root signature (opaque identity; never dereferenced)
    auto& s=acz::state();std::lock_guard lock(s.mutex);
    s.states.at(listAlias(0)).root=std::shared_ptr<ID3D12RootSignature>(reinterpret_cast<ID3D12RootSignature*>(&hudRootObj),[](ID3D12RootSignature*){});
}
// A later cockpit member (DrawCPT PSO, visible SrtData) inside the tail. The fixture's selected
// members were captured without a root signature: the member keeps that signature class.
static void hudLateMember() {
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.states.at(listAlias(0)).root.reset();}
    auto* l=listAlias(0);l->SetPipelineState(fakePso());bindT10(srtOff);l->DrawIndexedInstanced(9,1,0,0,0);
    hudSetRoot();
}
static void hudBindScene(ID3D12PipelineState* pso=nullptr) {
    auto* l=listAlias(0);const D3D12_CPU_DESCRIPTOR_HANDLE scene{gameRtvHandle},depth{gameDsvHandle};
    l->OMSetRenderTargets(1,&scene,FALSE,&depth);l->SetPipelineState(pso?pso:fakePso(false));
    bindT10(srtOff); // the HUD draws' own constants (here the visible scene blob, so the fake GPU rasterises them)
}
static void hudDraws(unsigned n,ID3D12PipelineState* pso=nullptr,unsigned badAt=UINT_MAX) {
    auto* l=listAlias(0);
    for(unsigned i=0;i<n;++i) {
        if(i==badAt)l->SetPipelineState(hudPso2());
        const auto before=allocations;traceAllocations=true;
        if(i%2)l->DrawInstanced(3,1,2,0);else l->DrawIndexedInstanced(i%4?5:6,1,0,0,0);
        traceAllocations=false;CHECK(allocations==before); // no heap allocation on the HUD Draw path
        if(i==badAt)l->SetPipelineState(pso?pso:fakePso(false));
    }
}
static void hudSceneBarrier(D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=reinterpret_cast<ID3D12Resource*>(&gameRt().object);
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=before;b.Transition.StateAfter=after;
    listAlias(0)->ResourceBarrier(1,&b);
}
static void hudOutput() {
    auto* l=listAlias(0);const D3D12_CPU_DESCRIPTOR_HANDLE h{hudOutputHandle};l->OMSetRenderTargets(1,&h,FALSE,nullptr);
    l->DrawIndexedInstanced(3,1,0,0,0); // the tone-map/copy into the output: never replayed
    hudSceneBarrier(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    hudSceneBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET); // fixture only: next frame's entry
}
// One whole frame: members, the HUD tail of n draws, the output pass; Close, Execute, GPU, tick.
// Returns the index of the first tail op.
static size_t hudFrame(unsigned n,const std::function<void()>& tail=nullptr) {
    w->lists[0].events.clear();
    frameSet();const size_t begin=w->lists[0].ops.size();
    hudSetRoot();
    if(tail)tail();else {hudBindScene();hudDraws(n);hudOutput();}
    CHECK(listAlias(0)->Close()==S_OK);execute({cl(0)});advance();(void)acz::step1Tick();
    return begin;
}
static acz::Step5State& s5ref(){auto* s5=acz::g_step5.load();CHECK(s5);return *s5;}
// The exact tail op sequence: game OM(scene), then per HUD draw [clears once] OM(hud), Draw(hud),
// OM(restore to the game's scene/depth), the game's own Draw (same arguments), then the output pass.
static void checkHudTail(size_t begin,unsigned n,unsigned slot,unsigned replayed=UINT_MAX,unsigned clearsExpected=1) {
    if(replayed==UINT_MAX)replayed=n;
    const auto& L=w->lists[0];const auto& sl=s5ref().slot[slot];
    std::vector<const Op*> ops;for(size_t i=begin;i<L.ops.size();++i)ops.push_back(&L.ops[i]);
    unsigned gameDraws=0,hudDrawsSeen=0,clears=0,clearDs=0,restores=0;size_t i=0;
    CHECK(ops.size()>=1&&ops[0]->t==Op::om&&originOf(ops[0]->rtv[0])==gameRtvHandle);
    for(i=1;i<ops.size();++i) {
        const Op& o=*ops[i];
        if(o.t==Op::clear_rtv){CHECK(o.rtv[0]==sl.hudRtv.ptr);++clears;continue;}
        if(o.t==Op::clear_dsv){CHECK(o.dsv==sl.hudDsv.ptr&&o.depth==0.0f&&o.clearFlags==D3D12_CLEAR_FLAG_DEPTH);++clearDs;continue;}
        if(o.t==Op::om&&o.rtv[0]==sl.hudRtv.ptr) {
            // OM(hud) -> Draw(hud) -> OM(restore) -> the game's Draw with the same arguments.
            CHECK(o.count==1&&o.dsvSet&&o.dsv==sl.hudDsv.ptr&&i+3<ops.size());
            const Op& d=*ops[i+1];const Op& r=*ops[i+2];const Op& g=*ops[i+3];
            CHECK(d.t==Op::draw&&d.boundRtv==sl.hudRtv.ptr&&d.boundDsv==sl.hudDsv.ptr);
            CHECK(r.t==Op::om&&r.count==1&&originOf(r.rtv[0])==gameRtvHandle&&r.dsvSet&&originOf(r.dsv)==gameDsvHandle);
            CHECK(g.t==Op::draw&&originOf(g.boundRtv)==gameRtvHandle&&originOf(g.boundDsv)==gameDsvHandle);
            CHECK(d.indexed==g.indexed&&d.dc==g.dc&&d.di==g.di&&d.ds==g.ds&&d.db==g.db&&d.dsi==g.dsi&&d.boundT10==g.boundT10);
            ++hudDrawsSeen;++restores;++gameDraws;i+=3;continue;
        }
        if(o.t==Op::draw&&originOf(o.boundRtv)==gameRtvHandle){++gameDraws;continue;} // a game draw that was not replayed
        if(o.t==Op::om&&o.rtv[0]==hudOutputHandle)break;
        if(o.t==Op::om&&originOf(o.rtv[0])==gameRtvHandle)continue; // a game bind inside the tail script
        if(o.t==Op::srv||(o.t==Op::barrier&&o.res==&gameRt().object))continue; // the game's own setters/barriers
        std::fprintf(stderr,"unexpected tail op %d at %zu\n",int(o.t),i);CHECK(false);
    }
    if(!(hudDrawsSeen==replayed&&restores==replayed&&clears==clearsExpected&&clearDs==clearsExpected)) {
        std::fprintf(stderr,"replayed %u/%u clears %u/%u game %u\n",hudDrawsSeen,replayed,clears,clearDs,gameDraws);CHECK(false);
    }
    // Nothing after the output bind is the proxy's (the output pass and the barriers are the game's own).
    for(;i<ops.size();++i){const Op& o=*ops[i];CHECK(o.t!=Op::clear_rtv&&o.t!=Op::clear_dsv&&!(o.t==Op::om&&o.rtv[0]==sl.hudRtv.ptr));
        if(o.t==Op::draw)CHECK(o.boundRtv==hudOutputHandle);}
    CHECK(gameDraws>=n);
}
static std::string hudLog(){(void)acz::step1Tick();acz::feature_log::flush();return featureLog("step5-producer");}
static unsigned hudCovered(ID3D12Resource* r) {
    auto& x=res(r);unsigned n=0;for(size_t p=0;p+3<x.big.size();p+=4)n+=x.big[p]==200&&x.big[p+1]==100&&x.big[p+2]==50&&x.big[p+3]==255;return n;
}
static void step5_hud_off() {
    // gpu_copy_step5_hud OFF (default): no HUD resources (bootContinuous counts them), no command
    // in the tail other than the game's own, StereoFrame HUD fields at their defaults, no new log lines.
    hudSetup(false);
    auto* src=acz::stereo::frame_source();
    const size_t begin=hudFrame(4);
    {const auto& L=w->lists[0];std::vector<Op::T> kinds;for(size_t i=begin;i<L.ops.size();++i)kinds.push_back(L.ops[i].t);
     const std::vector<Op::T> want{Op::om,Op::srv,Op::draw,Op::draw,Op::draw,Op::draw,Op::om,Op::draw,Op::barrier,Op::barrier};
     CHECK(kinds==want);
     for(size_t i=begin+2;i<begin+6;++i)CHECK(originOf(L.ops[i].boundRtv)==gameRtvHandle&&originOf(L.ops[i].boundDsv)==gameDsvHandle);}
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f));
    CHECK(!f.hud&&!f.hud_valid&&f.hud_format==0&&f.hud_draws==0&&f.hud_refused==0&&f.world_valid);src->release(f);
    {auto& s5=s5ref();CHECK(!s5.hudReady&&!s5.hudValidFrames&&!s5.hudUnarmedFrames);for(const auto& sl:s5.slot)CHECK(!sl.hudRt&&!sl.hudDs);}
    {std::lock_guard lock(acz::g_hudRegionMutex);CHECK(!acz::g_hudRegion.active&&!acz::g_hudRegion.list);}
    ticks(60);const auto text=hudLog();
    CHECK(text.find("hud-ring")==std::string::npos&&text.find("event=hud-")==std::string::npos&&text.find("-hud;")==std::string::npos);
    for(const auto& l:w->lines)CHECK(l.find("hud")==std::string::npos);
    // The same tail with the flag raised AFTER arming (no HUD targets exist): the tail command
    // stream is identical op for op to the OFF frame (the key never starts anything by itself).
    std::vector<Op> off(w->lists[0].ops.begin()+std::ptrdiff_t(begin),w->lists[0].ops.end());
    acz::g_hudReplayTest=true;const size_t b2=hudFrame(4);acz::g_hudReplayTest=false;
    std::vector<Op> on(w->lists[0].ops.begin()+std::ptrdiff_t(b2),w->lists[0].ops.end());
    CHECK(on.size()==off.size());for(size_t i=0;i<off.size();++i)CHECK(hudSameOp(off[i],on[i]));
    step3Lines();
}
static void step5_hud() {
    // Key ON: the n tail draws on the scene target are replayed into the slot's private HUD target.
    hudSetup(true);
    {auto& s5=s5ref();CHECK(s5.hudReady&&s5.hudFormat==uint32_t(shape3.rtv)&&s5.hudDsFormat==uint32_t(shape3.dsv));
     for(const auto& sl:s5.slot){CHECK(sl.hudRt&&sl.hudDs&&sl.hudRtv.ptr&&sl.hudDsv.ptr);
         auto& r=res(sl.hudRt);CHECK(r.format==shape3.rtv&&r.width==gameRt().width&&r.height==gameRt().height&&r.state==D3D12_RESOURCE_STATE_RENDER_TARGET);
         auto& d=res(sl.hudDs);CHECK(d.format==shape3.dsv&&d.state==D3D12_RESOURCE_STATE_DEPTH_WRITE&&(d.flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));}}
    auto* src=acz::stereo::frame_source();
    const auto gameBefore=gameRt().object.refs;
    const size_t begin=hudFrame(4);
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.frame_id==1);
    checkHudTail(begin,4,f.slot);
    if(!(f.hud&&f.hud_valid&&f.hud_format==uint32_t(shape3.rtv)&&f.hud_draws==4&&f.hud_refused==0)){
        std::fprintf(stderr,"hud=%p valid=%d fmt=%u draws=%u refused=%u\n",(void*)f.hud,int(f.hud_valid),f.hud_format,f.hud_draws,f.hud_refused);CHECK(false);}
    CHECK(f.hud==s5ref().slot[f.slot].hudRt&&f.hud!=f.world&&f.hud!=f.eye[0]&&f.world_valid&&f.members==6);
    CHECK(res(f.hud).state==D3D12_RESOURCE_STATE_RENDER_TARGET&&hudCovered(f.hud)>100); // the fake GPU rasterised the replays
    CHECK(gameRt().object.refs==gameBefore&&res(f.hud).object.refs==1); // never AddRef'd (game or private)
    // Slot reuse (4 frames over 3 slots): each frame clears its slot's HUD target first (transparent black).
    std::set<unsigned> used{f.slot};
    for(unsigned k=2;k<=4;++k) {
        const auto prev=f; // held while the next frame is produced: it must use another slot
        if(k==2)for(const auto& sl:s5ref().slot)std::fill(res(sl.hudRt).big.begin(),res(sl.hudRt).big.end(),uint8_t(0x77)); // stale content
        const size_t b=hudFrame(k==3?2:4);
        CHECK(src->try_acquire_latest(k-1,f)&&f.frame_id==k&&f.hud_valid&&f.hud_draws==(k==3?2u:4u));
        checkHudTail(b,k==3?2:4,f.slot);used.insert(f.slot);
        if(k==2){const auto& x=res(f.hud).big;CHECK(std::find(x.begin(),x.end(),uint8_t(0x77))==x.end());} // cleared to 0 before the replays
        CHECK(hudCovered(f.hud)>100&&f.hud==s5ref().slot[f.slot].hudRt&&f.slot!=prev.slot);
        src->release(prev);
    }
    CHECK(used.size()>=2);
    src->release(f);
    {const auto& s5=s5ref();CHECK(s5.hudValidFrames==4&&s5.hudInvalidFrames==0&&s5.hudDrawSum==14&&s5.hudRefusedSum==0&&s5.hudEnd[0]==4);}
    ticks(60);const auto text=hudLog();
    for(const char* k:{"|gpu-copy-step1;event=hud-ring;outcome=ready;reason=none;slots=3;","caps=draws:160,commands:600;",
            "|gpu-copy-step1;event=hud-frame;outcome=first-valid;frame=1;draws=4;refused=0(state:0,descriptor:0,format:0,cap:0);first-refusal=none;commands=14;resets=0;depth-tested=0;no-target=0;end=rt-change;",
            "hud-valid=1;premultiplied=1","event=continuous-hud;ready=1;frames=valid:4,invalid:0,unarmed:0;draws=14;",
            "end=rt-change:4,scene-barrier:0,close:0;last-refusal=none;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    for(const auto& l:w->lines)CHECK(l.find("hud")==std::string::npos); // HUD lines stay in the dedicated log
    step3Lines();
}
static void step5_hud_refused() {
    // Fail closed per draw: a refused draw is not replayed (the game's own draw still runs) and the
    // frame's HUD is not published (hud=nullptr, hud_valid=false), refusals counted by category.
    hudSetup(true);auto* src=acz::stereo::frame_source();
    auto expectFrame=[&](uint64_t id,unsigned draws,unsigned refused){acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);
        if(!(!f.hud&&!f.hud_valid&&f.hud_draws==draws&&f.hud_refused==refused&&f.hud_format==uint32_t(shape3.rtv))){
            std::fprintf(stderr,"frame %llu: hud=%p valid=%d draws=%u refused=%u\n",(unsigned long long)id,(void*)f.hud,int(f.hud_valid),f.hud_draws,f.hud_refused);CHECK(false);}
        const unsigned s=f.slot;src->release(f);return s;};
    // (a) state: stencil test LESS (depends on the GAME's stencil contents).
    hudPipeline(hudPso2(),[](auto& o){o.front_func=gpu_copy::step3::cmp::less;});
    size_t b=hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,2);hudOutput();});
    unsigned slot=expectFrame(1,3,1);checkHudTail(b,4,slot,3);
    // (b) format: the PSO writes another format than the private target.
    hudPipeline(hudPso2(),[](auto& o){o.rtv[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;});
    b=hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,1);hudOutput();});slot=expectFrame(2,3,1);checkHudTail(b,4,slot,3);
    // (b2) format: the PSO declares another DSV format than the private depth-stencil.
    hudPipeline(hudPso2(),[](auto& o){o.dsv=DXGI_FORMAT_D24_UNORM_S8_UINT;});
    b=hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,3);hudOutput();});slot=expectFrame(3,3,1);checkHudTail(b,4,slot,3);
    // (c) descriptor: no root signature tracked for the list (the replay keeps the game's root state).
    b=hudFrame(0,[]{auto& s=acz::state();{std::lock_guard lock(s.mutex);s.states.at(listAlias(0)).root.reset();}
        hudBindScene();hudDraws(2);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(3,f)&&f.frame_id==4&&!f.hud&&!f.hud_valid&&f.hud_draws==0&&f.hud_refused==2);src->release(f);}
    // (d) depth EQUAL (game depth contents): refused; GREATER (passes against the cleared private depth): replayed, counted.
    hudPipeline(hudPso2(),[](auto& o){o.depth_func=gpu_copy::step3::cmp::equal;});
    b=hudFrame(0,[]{hudBindScene();hudDraws(2,nullptr,0);hudOutput();});slot=expectFrame(5,1,1);
    hudPipeline(hudPso2(),[](auto& o){o.depth_func=gpu_copy::step3::cmp::greater;});
    b=hudFrame(0,[]{hudBindScene();hudDraws(2,nullptr,0);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(5,f)&&f.frame_id==6&&f.hud&&f.hud_valid&&f.hud_draws==2&&f.hud_refused==0);checkHudTail(b,2,f.slot);src->release(f);}
    // (e) a later clean frame is valid again.
    b=hudFrame(3);{acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(6,f)&&f.frame_id==7&&f.hud_valid&&f.hud_draws==3);src->release(f);}
    {const auto& s5=s5ref();CHECK(s5.hudValidFrames==2&&s5.hudInvalidFrames==5&&s5.hudReason[0]==2&&s5.hudReason[1]==2&&s5.hudReason[2]==2&&s5.hudReason[3]==0&&s5.hudDepthTested==1);}
    ticks(60);const auto text=hudLog();
    for(const char* k:{"event=hud-frame;outcome=first-refused;frame=0;draws=3;refused=1(state:1,descriptor:0,format:0,cap:0);first-refusal=stencil-test-not-always;",
            "hud-valid=0;","frames=valid:2,invalid:5,unarmed:0;","refused=6(state:2,descriptor:2,format:2,cap:0);","depth-tested=1;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    step3Lines();
}
static void step5_hud_rs_skip() {
    // HUD-RS-SKIP: the live sun-flare tuple (blend ONE,ONE,REV_SUBTRACT / ONE,ZERO,ADD, depth ALWAYS, 4 vertices) is skipped by class: the game's
    // draw runs, nothing is replayed for it, the frame's HUD stays valid with the other draws replayed; hudrs:0 (flag off) replays it again.
    using gpu_copy::step5::blend_op::add;using gpu_copy::step5::blend_op::rev_subtract;using gpu_copy::step5::blend_op::subtract;
    auto& sw=gpu_copy::step5::hud_rs_skip_flag();CHECK(sw.load());   // default ON
    hudSetup(true);auto* src=acz::stereo::frame_source();
    auto flare=[](auto& o){o.depth_func=gpu_copy::step3::cmp::always;o.depth_write=false;o.blend0=true;o.blend_op0=rev_subtract;o.blend_op_alpha0=add;o.rs_flare_factors=true;
        o.stencil=false;o.write_mask0=15;};
    // The HUD's own additive tuple (5,2,1/1,2,1: colour op ADD) on the default PSO; the flare PSO at draw 2.
    hudPipeline(fakePso(false),[&](auto& o){o.blend_op0=add;o.blend_op_alpha0=add;});
    hudPipeline(hudPso2(),flare);
    size_t b=hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,2);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.frame_id==1);
     if(!(f.hud&&f.hud_valid&&f.hud_draws==3&&f.hud_refused==0)){std::fprintf(stderr,"rs: valid=%d draws=%u refused=%u\n",int(f.hud_valid),f.hud_draws,f.hud_refused);CHECK(false);}
     checkHudTail(b,4,f.slot,3);src->release(f);}
    // Two flare quads in one frame (the live pair) among additive HUD draws: both skipped, the frame valid.
    b=hudFrame(0,[]{hudBindScene();hudDraws(2);auto* l=listAlias(0);l->SetPipelineState(hudPso2());l->DrawInstanced(4,1,2,0);l->DrawInstanced(4,1,2,0);
        l->SetPipelineState(fakePso(false));hudDraws(2);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(1,f)&&f.frame_id==2&&f.hud_valid&&f.hud_draws==4&&f.hud_refused==0);checkHudTail(b,6,f.slot,4);src->release(f);}
    // W10-1 P1#1: SUBTRACT (src - dest, positive content) is NOT the flare class: replayed (4 HUD draws); so are MIN/MAX/ADD and blend OFF.
    hudPipeline(hudPso2(),[&](auto& o){flare(o);o.blend_op0=subtract;});
    hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,1);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(2,f)&&f.frame_id==3&&f.hud_valid&&f.hud_draws==4);src->release(f);}
    hudPipeline(hudPso2(),[&](auto& o){flare(o);o.blend0=false;});
    hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,1);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(3,f)&&f.frame_id==4&&f.hud_valid&&f.hud_draws==4);src->release(f);}
    // A skipped draw that the rules would ALSO refuse (stencil LESS) stays a skip: the frame is not invalidated by it.
    hudPipeline(hudPso2(),[&](auto& o){flare(o);o.stencil=true;o.front_func=gpu_copy::step3::cmp::less;});
    hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,3);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(4,f)&&f.frame_id==5&&f.hud_valid&&f.hud_draws==3&&f.hud_refused==0);src->release(f);}
    // hudrs:0: the flare is replayed exactly as before the fix (counted as a HUD draw).
    hudPipeline(hudPso2(),flare);sw.store(false);
    b=hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,2);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(5,f)&&f.frame_id==6&&f.hud_valid&&f.hud_draws==4);checkHudTail(b,4,f.slot,4);src->release(f);}
    sw.store(true);
    // W10-1 P1#1: REV_SUBTRACT with other factors (not ONE/ONE, alpha ONE/ZERO) is not the observed class: replayed.
    hudPipeline(hudPso2(),[&](auto& o){flare(o);o.rs_flare_factors=false;});
    hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,1);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(6,f)&&f.frame_id==7&&f.hud_valid&&f.hud_draws==4);src->release(f);}
    // W10-1 P1#2: a skipped flare draw that does NOT write depth leaves later depth-TESTED (GREATER) HUD draws replayable.
    hudPipeline(fakePso(false),[&](auto& o){o.blend_op0=add;o.blend_op_alpha0=add;o.depth_func=gpu_copy::step3::cmp::greater;});
    hudPipeline(hudPso2(),flare);
    hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,1);hudOutput();});
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(7,f)&&f.frame_id==8&&f.hud_valid&&f.hud_draws==3&&f.hud_refused==0);src->release(f);}
    // ... but a skipped flare draw that WRITES depth leaves the private depth short of the game's: the later GREATER draws are refused
    // (depth-writer-skipped), the frame is invalid; the earlier draw before the skip was replayed.
    hudPipeline(hudPso2(),[&](auto& o){flare(o);o.depth_write=true;});
    hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,1);hudOutput();});
    {const auto& s5=s5ref();CHECK(s5.hudRsSkipSum==6&&s5.hudRsSkipFrames==5&&s5.hudRsDswSum==1&&s5.hudValidFrames==8&&s5.hudInvalidFrames==1&&s5.hudRefusedSum==2);
     CHECK(s5.hudLastWhy&&std::strcmp(s5.hudLastWhy,"depth-writer-skipped")==0);}
    ticks(60);auto text=hudLog();
    for(const char* k:{"event=continuous-hud-rs-skip;n=6;frames=5;ds-writers=1;on=1;rule=rev-subtract-one-one-flare-not-replayed(frame-stays-valid;skipped-depth-writer-refuses-depth-tested)",
            "frames=valid:8,invalid:1,unarmed:0;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    step3Lines();
}
static void step5_hud_cap() {
    // 161 HUD draws: 160 replayed, the 161st refused (cap) -> the frame's HUD is not published.
    hudSetup(true);auto* src=acz::stereo::frame_source();
    const size_t b=hudFrame(161);
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&!f.hud&&!f.hud_valid&&f.hud_draws==160&&f.hud_refused==1);
    checkHudTail(b,161,f.slot,160);src->release(f);
    {const auto& s5=s5ref();CHECK(s5.hudReason[3]==1&&s5.hudCapFrames==1);}
    // The command cap: commands accumulate across a member resume (re-clear), the draw count restarts.
    hudFrame(0,[]{hudBindScene();hudDraws(150);hudLateMember();hudBindScene();hudDraws(60);hudOutput();});
    // 2 + 150*3 = 452; after the resume 2 + 3k <= 148 -> 48 replayed, the 49th refused.
    CHECK(src->try_acquire_latest(1,f)&&f.frame_id==2&&!f.hud_valid&&f.hud_draws==48&&f.hud_refused==12);src->release(f);
    {const auto& s5=s5ref();CHECK(s5.hudReason[3]==13&&s5.hudCapFrames==2&&s5.hudResets==1);}
    step3Lines();
}
static void step5_hud_region() {
    hudSetup(true);auto* src=acz::stereo::frame_source();
    auto frameHud=[&](uint64_t id,unsigned draws,bool valid){acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);
        if(!(f.hud_draws==draws&&f.hud_valid==valid&&(f.hud!=nullptr)==valid)){std::fprintf(stderr,"frame %llu draws %u valid %d members %u last-member-refusal %s\n",
            (unsigned long long)id,f.hud_draws,int(f.hud_valid),f.members,s5ref().lastMemberWhy?s5ref().lastMemberWhy:"none");CHECK(false);}
        const unsigned s=f.slot;src->release(f);return s;};
    // (a) a render-target change ends the region: scene draws after it are not replayed (the game's run unchanged).
    size_t b=hudFrame(0,[]{hudBindScene();hudDraws(2);auto* l=listAlias(0);const D3D12_CPU_DESCRIPTOR_HANDLE h{hudOutputHandle};
        l->OMSetRenderTargets(1,&h,FALSE,nullptr);l->DrawIndexedInstanced(3,1,0,0,0);hudBindScene();hudDraws(3);hudOutput();});
    unsigned slot=frameHud(1,2,true);
    {const auto& L=w->lists[0];unsigned replays=0,game=0;const auto& sl=s5ref().slot[slot];
     for(size_t i=b;i<L.ops.size();++i){if(L.ops[i].t!=Op::draw)continue;replays+=L.ops[i].boundRtv==sl.hudRtv.ptr;game+=originOf(L.ops[i].boundRtv)==gameRtvHandle;}
     CHECK(replays==2&&game==5);}
    // (b) the scene target leaving RENDER_TARGET ends the region (even if it comes back).
    b=hudFrame(0,[]{hudBindScene();hudDraws(2);hudSceneBarrier(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hudSceneBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);hudDraws(2);hudOutput();});
    slot=frameHud(2,2,true);checkHudTail(b,4,slot,2);
    // (c) a later cockpit member restarts the region: only the draws after the LAST member count,
    // the private target is cleared again before them.
    b=hudFrame(0,[]{hudBindScene();hudDraws(2);hudLateMember();hudBindScene();hudDraws(3);hudOutput();});
    slot=frameHud(3,3,true);
    {const auto& L=w->lists[0];unsigned clears=0;for(size_t i=b;i<L.ops.size();++i)clears+=L.ops[i].t==Op::clear_rtv&&L.ops[i].rtv[0]==s5ref().slot[slot].hudRtv.ptr;CHECK(clears==2);}
    // (d) a draw with no render target bound cannot write the scene colour: not replayed, not refused.
    b=hudFrame(0,[]{hudBindScene();hudDraws(1);auto* l=listAlias(0);l->OMSetRenderTargets(0,nullptr,FALSE,nullptr);l->DrawIndexedInstanced(3,1,0,0,0);
        hudBindScene();hudDraws(1);hudOutput();});
    frameHud(4,2,true);
    // (e) a frame without any HUD draw publishes no HUD (nothing stale from the slot's earlier use).
    hudFrame(0,[]{hudOutput();});frameHud(5,0,false);
    {const auto& s5=s5ref();CHECK(s5.hudEnd[0]==4&&s5.hudEnd[1]==1&&s5.hudEnd[2]==0&&s5.hudResets==1);}
    ticks(60);const auto text=hudLog();
    CHECK(text.find("frames=valid:4,invalid:1,unarmed:0;")!=std::string::npos&&text.find("end=rt-change:4,scene-barrier:1,close:0;")!=std::string::npos);
    step3Lines();
}
static void step5_hud_faults() {
    // The HUD rides the slot lifecycle: duplicate submission (ambiguous, quarantined), Reset before
    // submission (discarded), a new fence epoch, and device loss publish no HUD.
    hudSetup(true);auto* src=acz::stereo::frame_source();
    // Duplicate in one batch: ambiguous, never published.
    frameSet();hudSetRoot();hudBindScene();hudDraws(2);hudOutput();CHECK(listAlias(0)->Close()==S_OK);
    execute({cl(0),cl(0)});advance();(void)acz::step1Tick();
    CHECK(src->frames_published()==0);{std::lock_guard lock(acz::g_hudRegionMutex);CHECK(!acz::g_hudRegion.active);}
    // Reset before submission: discarded, the next frame is clean and valid.
    frameSet();hudSetRoot();hudBindScene();hudDraws(2);CHECK(listAlias(0)->Close()==S_OK);
    CHECK(listAlias(0)->Reset(allocator(0),fakePso())==S_OK);CHECK(listAlias(0)->Close()==S_OK);
    {std::lock_guard lock(acz::g_hudRegionMutex);CHECK(!acz::g_hudRegion.active);}
    hudFrame(3);acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.hud_valid&&f.hud_draws==3);src->release(f);
    // A new fence epoch: earlier frames (and their HUD) are no longer acquirable; a new-epoch frame is.
    advance();st1().fence.fence=reinterpret_cast<ID3D12Fence*>(&w->fence2);(void)acz::step1Tick();
    CHECK(!src->try_acquire_latest(0,f));
    hudFrame(2);CHECK(src->try_acquire_latest(0,f)&&f.fence_epoch==2&&f.hud_valid&&f.hud_draws==2);src->release(f);
    // Device loss (completed value UINT64_MAX): the producer stops; no HUD (or any) replay is recorded.
    w->fenceValue=UINT64_MAX;const auto published=src->frames_published();
    const size_t b=hudFrame(3);
    for(size_t i=b;i<w->lists[0].ops.size();++i){const auto& o=w->lists[0].ops[i];CHECK(o.t!=Op::clear_rtv&&o.t!=Op::clear_dsv);
        if(o.t==Op::draw)CHECK(originOf(o.boundRtv)==gameRtvHandle||o.boundRtv==hudOutputHandle);}
    CHECK(src->frames_published()==published&&s5ref().disabled);
}
static void step5_hud_rules() {
    using namespace gpu_copy::step5;using namespace gpu_copy::step3;
    Shape s;s.tracking_complete=true;s.root_route=0x10;s.pso.known=true;s.pso.rt_count=1;s.pso.rtv[0]=10;s.pso.dsv=20;s.pso.samples=1;
    s.om_known=s.om_set=true;s.om_count=1;s.om_desc_proved=true;s.game_rt_status="resolved";s.game_rt=0x5000;s.game_rt_format=10;
    s.game_rt_width=2560;s.game_rt_height=1440;s.pso.depth_enable=true;s.pso.depth_func=cmp::always;s.pso.stencil=true;s.pso.front_func=s.pso.back_func=cmp::always;
    HudTarget t;t.format=10;t.ds_format=20;t.width=2560;t.height=1440;t.scene=0x5000;
    CHECK(!hud_decide(s,t).why);
    auto why=[&](Shape x,HudTarget y=HudTarget{10,20,2560,1440,0x5000}){const auto v=hud_decide(x,y);return std::pair<std::string,int>(v.why?v.why:"",v.category);};
    using P=std::pair<std::string,int>;
    {auto x=s;x.tracking_complete=false;CHECK(why(x)==P("binding-tracking-incomplete",hud_descriptor));}
    {auto x=s;x.root_route=0;CHECK(why(x)==P("root-signature-unknown",hud_descriptor));}
    {auto x=s;x.om_desc_proved=false;CHECK(why(x)==P("om-descriptor-unproved",hud_descriptor));}
    {auto x=s;x.pso.known=false;CHECK(why(x)==P("pso-output-unknown",hud_state));}
    {auto x=s;x.om_gap=true;CHECK(why(x)==P("om-state-gap",hud_state));}
    {auto x=s;x.om_count=2;CHECK(why(x)==P("om-rt-count-not-1",hud_state));}
    {auto x=s;x.pso.rt_count=2;CHECK(why(x)==P("pso-rt-count-not-1",hud_state));}
    {auto x=s;x.game_rt_status="unresolved:rtv-not-tracked";CHECK(why(x)==P("game-rt-unresolved",hud_state));}
    {auto x=s;x.game_rt=0x6000;CHECK(why(x)==P("not-scene-target",hud_state));}
    {auto x=s;x.pso.rtv[0]=28;CHECK(why(x)==P("rt-format-mismatch",hud_format));}
    {auto x=s;x.pso.samples=4;CHECK(why(x)==P("msaa",hud_format));}
    {auto x=s;x.game_rt_width=1280;CHECK(why(x)==P("scene-desc-changed",hud_format));}
    {auto x=s;x.pso.dsv=45;CHECK(why(x)==P("ds-format-mismatch",hud_format));}
    {auto x=s;x.pso.dsv=0;x.pso.depth_enable=false;x.pso.stencil=false;CHECK(why(x)==P("",hud_state));} // no DSV: replayed without one
    {auto x=s;x.pso.dsv=0;CHECK(why(x)==P("depth-enabled-without-dsv-format",hud_state));}
    {auto x=s;x.pso.dsv=0;x.pso.depth_enable=false;CHECK(why(x)==P("stencil-enabled-without-dsv-format",hud_state));}
    for(uint32_t f:{cmp::less,cmp::less_equal,cmp::equal,cmp::not_equal,0u}){auto x=s;x.pso.depth_func=f;CHECK(why(x)==P("depth-test-not-replayable",hud_state));}
    for(uint32_t f:{cmp::always,cmp::never}){auto x=s;x.pso.depth_func=f;const auto v=hud_decide(x,t);CHECK(!v.why&&!v.depth_tested);}
    for(uint32_t f:{cmp::greater,cmp::greater_equal}){auto x=s;x.pso.depth_func=f;const auto v=hud_decide(x,t);CHECK(!v.why&&v.depth_tested);}
    {auto x=s;x.pso.depth_enable=false;x.pso.depth_func=cmp::less;CHECK(!hud_decide(x,t).why);} // a disabled test ignores the function
    {auto x=s;x.pso.back_func=cmp::less;CHECK(why(x)==P("stencil-test-not-always",hud_state));}
    {auto x=s;x.pso.stencil=false;x.pso.front_func=cmp::less;CHECK(!hud_decide(x,t).why);}
    CHECK(!hud_frame_valid(false,4,0)&&!hud_frame_valid(true,0,0)&&!hud_frame_valid(true,4,1)&&hud_frame_valid(true,4,0));
    // HUD-RS-SKIP class: the live flare tuple (ONE,ONE,REV_SUBTRACT / ONE,ZERO,ADD) is replayable by hud_decide (that was the bug) but skipped by class;
    // SUBTRACT likewise; ADD (the HUD's 5,2,1/1,2,1), MIN, MAX, blend off, an unknown PSO or op 0 (not recorded) are not; the switch off never skips.
    {auto x=s;x.pso.blend0=true;x.pso.blend_op0=blend_op::rev_subtract;x.pso.blend_op_alpha0=blend_op::add;
     x.pso.rs_flare_factors=true;
     CHECK(!hud_decide(x,t).why&&hud_rs_subtract(x.pso)&&hud_rs_skip(x,true)&&!hud_rs_skip(x,false));
     x.pso.blend_op0=blend_op::subtract;CHECK(!hud_rs_skip(x,true)&&!hud_decide(x,t).why); // positive source-minus-dest content is replayed
     x.pso.blend_op0=blend_op::rev_subtract;x.pso.rs_flare_factors=false;CHECK(!hud_rs_skip(x,true)); // other factors: not the observed class
     x.pso.rs_flare_factors=true;x.pso.blend_op_alpha0=blend_op::subtract;CHECK(!hud_rs_skip(x,true)); // alpha op not ADD
     x.pso.blend_op_alpha0=blend_op::add;
     {auto y=s;y.pso.depth_func=cmp::greater;HudTarget g=t;g.depth_gap=true; // a skipped depth writer: depth-tested draws refused, ALWAYS draws not
      CHECK(!hud_decide(y,t).why&&std::strcmp(hud_decide(y,g).why,"depth-writer-skipped")==0);y.pso.depth_func=cmp::always;CHECK(!hud_decide(y,g).why);}
     x.pso.blend_op0=blend_op::subtract;
     for(uint32_t op:{0u,blend_op::add,blend_op::min,blend_op::max}){x.pso.blend_op0=op;CHECK(!hud_rs_skip(x,true));}
     x.pso.blend_op0=blend_op::rev_subtract;x.pso.blend0=false;CHECK(!hud_rs_skip(x,true));
     x.pso.blend0=true;x.pso.known=false;CHECK(!hud_rs_skip(x,true));
     x.pso.known=true;x.pso.blend_op0=blend_op::add;x.pso.blend_op_alpha0=blend_op::rev_subtract;CHECK(!hud_rs_skip(x,true));} // alpha op alone: not the class
    CHECK(hud_draw_cap*hud_per_draw+2<=hud_command_cap&&std::string(hud_category_name(hud_cap))=="cap"&&std::string(hud_category_name(9))=="?");
    // Config: default OFF, the usual boolean forms, duplicates/out-of-range rejected.
    using namespace acz::diagnostics;
    CHECK(!Config{}.gpu_copy_step5_hud()&&!parse_config("").config.gpu_copy_step5_hud());
    for(const auto* v:{"true","1"}){auto c=parse_config(std::string("gpu_copy_step5_hud=")+v);CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_hud()&&!c.config.gpu_copy_step5_hud_observe());}
    for(const auto* v:{"false","0"}){auto c=parse_config(std::string("gpu_copy_step5_hud=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.gpu_copy_step5_hud());}
    CHECK(parse_config("gpu_copy_step5_hud=on").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_hud=1\ngpu_copy_step5_hud=0").issue==ConfigIssue::duplicate);
    CHECK(!parse_config("gpu_copy_step5_hud_observe=1").config.gpu_copy_step5_hud());
}
// ------------------------------------------------------------- J-hud-radar helpers (gpu_copy_step5_hud_radar)
// Script tokens (space separated). Binds: @ scene target, o proven other target (the output view), z no render target, u an unresolved
// single target, M a later cockpit member. Draws: p plain (default HUD PSO, alternating indexed / not), R stencil REPLACE writer (ALWAYS,
// pass op REPLACE, write mask 255; indexed), k harmless ALWAYS/KEEP (indexed), E stencil EQUAL pass KEEP (indexed, read mask 255), N
// NOT_EQUAL pass KEEP (4 vertices), W NOT_EQUAL pass ZERO = a stencil writer (4 vertices), T the a3050-shaped triangle (3 vertices, depth
// LESS write, stencil EQUAL pass ZERO). A trailing number is the draw's vertex/index count (default 6).
static Object radarRepObj,radarKeepObj,radarEqObj,radarNeObj,radarNeWObj,radarTriObj,radarEqWObj,radarNarrowObj,radarBadObj;
static ID3D12PipelineState* rpso(Object& o){return reinterpret_cast<ID3D12PipelineState*>(&o);}
static void radarPipelines() {
    namespace s3=gpu_copy::step3;
    for(Object* o:{&radarRepObj,&radarKeepObj,&radarEqObj,&radarNeObj,&radarNeWObj,&radarTriObj,&radarEqWObj,&radarNarrowObj,&radarBadObj}){o->table=w->psoTable.data();o->kind=k_pso;}
    // The plain draw is harmless (ALWAYS / KEEP, no stencil write): the fixture default has pass op REPLACE, which would make every draw a REPLACE writer.
    hudPipeline(fakePso(false),[](auto& o){o.front_pass=1;o.stencil_write=0;o.stencil_ops_keep=true;});
    hudPipeline(rpso(radarNarrowObj),[](auto& o){o.front_pass=3;o.stencil_write=0x0f;o.stencil_ops_keep=false;});
    hudPipeline(rpso(radarBadObj),[](auto& o){o.front_func=o.back_func=s3::cmp::less;o.front_pass=3;o.stencil_write=255;o.stencil_ops_keep=false;});
    hudPipeline(rpso(radarRepObj),[](auto& o){o.front_pass=3;o.stencil_write=255;o.stencil_ops_keep=false;});
    hudPipeline(rpso(radarKeepObj),[](auto& o){o.front_pass=1;o.stencil_write=0;o.stencil_ops_keep=true;});
    hudPipeline(rpso(radarEqObj),[](auto& o){o.front_func=o.back_func=s3::cmp::equal;o.front_pass=1;o.stencil_read=255;o.stencil_write=0;o.stencil_ops_keep=true;});
    hudPipeline(rpso(radarEqWObj),[](auto& o){o.front_func=o.back_func=s3::cmp::equal;o.front_pass=2;o.stencil_read=255;o.stencil_write=255;o.stencil_ops_keep=false;});
    hudPipeline(rpso(radarNeObj),[](auto& o){o.front_func=o.back_func=s3::cmp::not_equal;o.front_pass=1;o.stencil_read=255;o.stencil_write=0;o.stencil_ops_keep=true;});
    hudPipeline(rpso(radarNeWObj),[](auto& o){o.front_func=o.back_func=s3::cmp::not_equal;o.front_pass=2;o.stencil_read=255;o.stencil_write=255;o.stencil_ops_keep=false;});
    hudPipeline(rpso(radarTriObj),[](auto& o){o.depth_func=s3::cmp::less;o.depth_write=true;o.front_func=o.back_func=s3::cmp::equal;o.front_pass=2;
        o.stencil_read=255;o.stencil_write=255;o.stencil_ops_keep=false;});
}
static void radarSetup(bool key,bool radar);
static void radarScript(const std::string& script) {
    auto* l=listAlias(0);std::istringstream in(script);std::string tok;unsigned plain=0;
    while(in>>tok) {
        const char c=tok[0];const UINT n=tok.size()>1?UINT(std::stoul(tok.substr(1))):6u;
        if(c=='@'){hudBindScene();continue;}
        if(c=='o'){const D3D12_CPU_DESCRIPTOR_HANDLE h{hudOutputHandle};l->OMSetRenderTargets(1,&h,FALSE,nullptr);continue;}
        if(c=='z'){l->OMSetRenderTargets(0,nullptr,FALSE,nullptr);continue;}
        if(c=='u'){const D3D12_CPU_DESCRIPTOR_HANDLE h{0x17300};l->OMSetRenderTargets(1,&h,FALSE,nullptr);continue;}
        if(c=='M'){hudLateMember();hudBindScene();continue;}
        ID3D12PipelineState* p=c=='R'?rpso(radarRepObj):c=='k'?rpso(radarKeepObj):c=='E'?rpso(radarEqObj):c=='e'?rpso(radarEqWObj):c=='N'?rpso(radarNeObj):
            c=='W'?rpso(radarNeWObj):c=='T'?rpso(radarTriObj):c=='r'?rpso(radarNarrowObj):c=='x'?rpso(radarBadObj):fakePso(false);
        const bool indexed=c=='R'||c=='k'||c=='E'||c=='e'||c=='r'||c=='x'||(c=='p'&&(plain++%2==0));
        const UINT count=(c=='N'||c=='W')?4u:c=='T'?3u:(tok.size()>1?n:(indexed?6u:3u));
        l->SetPipelineState(p);
        const auto before=allocations;traceAllocations=true;
        if(indexed)l->DrawIndexedInstanced(count,1,0,0,0);else l->DrawInstanced(count,1,2,0);
        traceAllocations=false;CHECK(allocations==before); // no heap allocation on the (radar) HUD Draw path
    }
}
static size_t radarFrame(const std::string& script) {
    return hudFrame(0,[&]{hudBindScene();radarScript(script);hudOutput();});
}
// Normalised op stream (handles replaced by their class) and counters, hashed: equal across processes and builds.
static std::string radarNorm(size_t begin,unsigned slot) {
    const auto& L=w->lists[0];const auto& sl=s5ref().slot[slot];std::string s;char b[160];
    auto cls=[&](SIZE_T h)->char{
        if(!h)return '0';if(h==hudOutputHandle)return 'O';if(h==sl.hudRtv.ptr)return 'H';if(h==sl.hudDsv.ptr)return 'D';if(originOf(h)==gameRtvHandle)return 'G';
        if(originOf(h)==gameDsvHandle)return 'g';return '?';
    };
    for(size_t i=begin;i<L.ops.size();++i) {
        const Op& o=L.ops[i];
        switch(o.t) {
        case Op::om:std::snprintf(b,sizeof b,"om:%u:%c:%d:%c;",o.count,cls(o.rtv[0]),int(o.dsvSet),cls(o.dsv));break;
        case Op::clear_rtv:std::snprintf(b,sizeof b,"crt:%c;",cls(o.rtv[0]));break;
        case Op::clear_dsv:std::snprintf(b,sizeof b,"cds:%c:%u:%g;",cls(o.dsv),o.clearFlags,double(o.depth));break;
        case Op::draw:std::snprintf(b,sizeof b,"dr:%c:%c:%d:%u:%u:%u:%u:%d;",cls(o.boundRtv),cls(o.boundDsv),int(o.indexed),o.dc,o.di,o.ds,o.dsi,o.db);break;
        case Op::srv:std::snprintf(b,sizeof b,"srv;");break;
        case Op::barrier:std::snprintf(b,sizeof b,"bar;");break;
        default:std::snprintf(b,sizeof b,"op%d;",int(o.t));break;
        }
        s+=b;
    }
    return s;
}
static std::string fnvHex(const std::string& s) {
    uint64_t h=1469598103934665603ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}
    char b[24];std::snprintf(b,sizeof b,"%016llx",(unsigned long long)h);return b;
}
// One frame: the digest of its ops, the published HUD facts and the counters that exist in every build.
static std::string radarDigest(const std::string& script,uint64_t id,std::string* raw=nullptr) {
    auto* src=acz::stereo::frame_source();
    const size_t b=radarFrame(script);acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);
    char t[200];std::snprintf(t,sizeof t,"|hud=%d,valid=%d,draws=%u,refused=%u",int(f.hud!=nullptr),int(f.hud_valid),f.hud_draws,f.hud_refused);
    std::string s=radarNorm(b,f.slot)+t;
    {const auto& s5=s5ref();std::snprintf(t,sizeof t,"|valid=%llu,invalid=%llu,sum=%llu,refused=%llu,r=%llu/%llu/%llu/%llu,cap=%llu,resets=%llu,end=%llu/%llu/%llu",
        (unsigned long long)s5.hudValidFrames,(unsigned long long)s5.hudInvalidFrames,(unsigned long long)s5.hudDrawSum,(unsigned long long)s5.hudRefusedSum,
        (unsigned long long)s5.hudReason[0],(unsigned long long)s5.hudReason[1],(unsigned long long)s5.hudReason[2],(unsigned long long)s5.hudReason[3],
        (unsigned long long)s5.hudCapFrames,(unsigned long long)s5.hudResets,(unsigned long long)s5.hudEnd[0],(unsigned long long)s5.hudEnd[1],(unsigned long long)s5.hudEnd[2]);s+=t;}
    src->release(f);if(raw)*raw=s;return fnvHex(s);
}
// Scripts whose key-off outcome is the legacy behaviour (and which the radar key changes). Golden digests recorded from the build WITHOUT the key.
static const std::vector<std::string>& radarOffScripts() {
    static const std::vector<std::string> v{
        "p p p o p @ p p",                 // excursion: the region ends at the proven other target, later scene draws are not replayed
        "R p E8 E8 p",                     // stencil EQUAL after a writer: refused (stencil-test-not-always) in the legacy build
        "R z T p N p",                     // no-target triangle (skipped), then a NOT_EQUAL quad (refused)
        "p R o T @ N R E5 E9 k",           // the whole radar shape with an excursion
    };
    return v;
}
// Golden digests of radarOffScripts() recorded from the build WITHOUT gpu_copy_step5_hud_radar (the commit before it); key off must reproduce them.
static const char* const radarGolden[]={"3f95b9e03c374d52","ee7cfae552cd8c57","2c503c098afd5062","72e5f076366a21c3"};
struct RadarSnap {
    uint64_t frames=0,exc=0,ret=0,rep=0,eq=0,eqIdx=0,late=0,win=0,a3050=0,ne=0,lateSk=0,awaySk=0,excW=0,gaps=0,eqRef=0,gapRef=0,valid=0,invalid=0,end0=0,end1=0,cap=0,resets=0;
};
static RadarSnap radarSnap() {
    const auto& s=s5ref();RadarSnap r;
    r.frames=s.radarFrames;r.exc=s.radarExcursions;r.ret=s.radarReturns;r.rep=s.radarReplace;r.eq=s.radarEqual;r.eqIdx=s.radarEqualIndices;r.late=s.radarLate;
    r.win=s.radarWindows;r.a3050=s.radarA3050;r.ne=s.radarNotEqual;r.lateSk=s.radarLateSkipped;r.awaySk=s.radarAwaySkipped;r.excW=s.radarExcWriters;r.gaps=s.radarGaps;
    r.eqRef=s.radarEqualRefused;r.gapRef=s.radarGapRefused;r.valid=s.hudValidFrames;r.invalid=s.hudInvalidFrames;r.end0=s.hudEnd[0];r.end1=s.hudEnd[1];r.cap=s.hudCapFrames;r.resets=s.hudResets;
    return r;
}
static RadarSnap operator-(const RadarSnap& a,const RadarSnap& b) {
    RadarSnap r;
    r.frames=a.frames-b.frames;r.exc=a.exc-b.exc;r.ret=a.ret-b.ret;r.rep=a.rep-b.rep;r.eq=a.eq-b.eq;r.eqIdx=a.eqIdx-b.eqIdx;r.late=a.late-b.late;r.win=a.win-b.win;
    r.a3050=a.a3050-b.a3050;r.ne=a.ne-b.ne;r.lateSk=a.lateSk-b.lateSk;r.awaySk=a.awaySk-b.awaySk;r.excW=a.excW-b.excW;r.gaps=a.gaps-b.gaps;r.eqRef=a.eqRef-b.eqRef;
    r.gapRef=a.gapRef-b.gapRef;r.valid=a.valid-b.valid;r.invalid=a.invalid-b.invalid;r.end0=a.end0-b.end0;r.end1=a.end1-b.end1;r.cap=a.cap-b.cap;r.resets=a.resets-b.resets;
    return r;
}
// One frame of `script` (or `tail`); the published StereoFrame (released again; its slot and HUD facts stay readable).
static acz::stereo::StereoFrame radarRun(uint64_t id,const std::string& script,size_t* begin=nullptr,const std::function<void()>& tail=nullptr) {
    auto* src=acz::stereo::frame_source();
    const size_t b=tail?hudFrame(0,tail):radarFrame(script);if(begin)*begin=b;
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);src->release(f);return f;
}
// The replayed draws of a frame, in order ("i6" = indexed 6, "n3" = unindexed 3), each verified as OM(hud) Draw OM(restore) game-Draw.
static std::string radarReplayed(size_t begin,unsigned slot,unsigned* clears=nullptr) {
    const auto& L=w->lists[0];const auto& sl=s5ref().slot[slot];std::string out;unsigned c=0;
    for(size_t i=begin;i<L.ops.size();++i) {
        const Op& o=L.ops[i];
        if(o.t==Op::clear_rtv&&o.rtv[0]==sl.hudRtv.ptr)++c;
        if(!(o.t==Op::om&&o.rtv[0]==sl.hudRtv.ptr))continue;
        CHECK(o.count==1&&o.dsvSet&&o.dsv==sl.hudDsv.ptr&&i+3<L.ops.size());
        const Op& d=L.ops[i+1];const Op& r=L.ops[i+2];const Op& g=L.ops[i+3];
        CHECK(d.t==Op::draw&&d.boundRtv==sl.hudRtv.ptr&&d.boundDsv==sl.hudDsv.ptr);
        CHECK(r.t==Op::om&&r.count==1&&originOf(r.rtv[0])==gameRtvHandle&&r.dsvSet&&originOf(r.dsv)==gameDsvHandle);
        CHECK(g.t==Op::draw&&originOf(g.boundRtv)==gameRtvHandle&&d.indexed==g.indexed&&d.dc==g.dc&&d.di==g.di&&d.ds==g.ds&&d.boundT10==g.boundT10);
        if(!out.empty())out+=' ';
        out+=(d.indexed?"i":"n")+std::to_string(d.dc);i+=3;
    }
    if(clears)*clears=c;
    return out;
}
static void radarWant(const std::string& got,const char* want,const char* what) {
    if(got!=want){std::fprintf(stderr,"%s: replays [%s], want [%s]\n",what,got.c_str(),want);CHECK(false);}
}
static void radarSetup(bool key,bool radar) {
    acz::g_hudRadarTest=radar;
    hudSetup(key);radarPipelines();
}
// Shared fixture for the pure rules: a replayable ALWAYS HUD draw on the scene target.
static gpu_copy::step3::Shape radarShape() {
    using namespace gpu_copy::step3;
    Shape s;s.tracking_complete=true;s.root_route=0x10;s.pso.known=true;s.pso.rt_count=1;s.pso.rtv[0]=10;s.pso.dsv=20;s.pso.samples=1;
    s.om_known=s.om_set=true;s.om_count=1;s.om_desc_proved=true;s.game_rt_status="resolved";s.game_rt=0x5000;s.game_rt_format=10;
    s.game_rt_width=2560;s.game_rt_height=1440;s.pso.depth_enable=true;s.pso.depth_func=cmp::always;s.pso.stencil=true;s.pso.front_func=s.pso.back_func=cmp::always;
    s.pso.front_pass=1;s.pso.stencil_read=255;s.pso.stencil_write=0;s.pso.stencil_ops_keep=true;s.indexed=true;s.count=6;s.instances=1;
    return s;
}
static void step5_hud_radar_rules() {
    using namespace gpu_copy::step5;using namespace gpu_copy::step3;
    const Shape base=radarShape();
    HudTarget t0;t0.format=10;t0.ds_format=20;t0.width=2560;t0.height=1440;t0.scene=0x5000;
    HudTarget tr=t0;tr.radar=true;tr.radar_replace=true;tr.radar_write_mask=255;
    auto eq=[&]{Shape s=base;s.pso.front_func=s.pso.back_func=cmp::equal;return s;};
    auto why=[&](const Shape& s,const HudTarget& t){const auto v=hud_decide(s,t);return std::string(v.why?v.why:"");};
    CHECK(why(base,t0)==""&&why(base,tr)==""); // ALWAYS stencil: unchanged by the radar fields
    // --- the EQUAL relaxation (hud_decide): only a pure EQUAL test, only with the proof
    CHECK(why(eq(),t0)=="stencil-test-not-always");                                      // radar off: the original refusal
    CHECK(why(eq(),tr)=="");                                                             // radar + replayed REPLACE writer + no gap + mask covered
    {auto t=tr;t.radar_replace=false;CHECK(why(eq(),t)=="radar-equal-without-writer");}  // MUTATION GUARD: EQUAL without a replayed writer is refused
    {auto t=tr;t.radar_gap=true;CHECK(why(eq(),t)=="stencil-writer-skipped");}
    {auto t=tr;t.radar_write_mask=0x0f;CHECK(why(eq(),t)=="radar-mask-not-covered");}
    {auto t=tr;t.radar_write_mask=0x1ff;CHECK(why(eq(),t)=="");}
    {auto s=eq();s.pso.stencil_read=0x0f;auto t=tr;t.radar_write_mask=0x0f;CHECK(why(s,t)=="");}
    {auto s=eq();s.pso.stencil_write=255;s.pso.stencil_ops_keep=false;CHECK(why(s,tr)=="radar-equal-writes-stencil");}
    {auto s=eq();s.pso.stencil_write=255;s.pso.stencil_ops_keep=true;CHECK(why(s,tr)=="");} // a write mask with all-KEEP ops writes nothing
    {auto s=eq();s.pso.back_func=cmp::always;CHECK(why(s,tr)=="stencil-test-not-always");} // one face only
    {auto s=eq();s.pso.back_func=cmp::not_equal;CHECK(why(s,tr)=="stencil-test-not-always");}
    for(uint32_t f:{cmp::not_equal,cmp::less,cmp::less_equal,cmp::greater,cmp::greater_equal,cmp::never}){auto s=base;s.pso.front_func=s.pso.back_func=f;CHECK(why(s,tr)=="stencil-test-not-always");}
    {auto s=eq();s.pso.stencil=false;CHECK(why(s,t0)==""&&why(s,tr)=="");} // stencil disabled ignores the function
    {auto s=eq();s.pso.depth_func=cmp::less;s.pso.depth_write=true;CHECK(why(s,tr)=="depth-test-not-replayable");} // depth rules are untouched
    {auto s=eq();s.pso.rtv[0]=28;CHECK(why(s,tr)=="rt-format-mismatch");}                                           // earlier rules still win
    {auto s=eq();s.om_count=0;CHECK(why(s,tr)=="om-rt-count-not-1");}
    {auto s=eq();s.pso.dsv=0;CHECK(why(s,tr)=="stencil-enabled-without-dsv-format"||why(s,tr)=="depth-enabled-without-dsv-format");}
    // The effects layer never gets the relaxation (its HudTarget carries no radar fields).
    {auto s=eq();s.pso.blend0=true;s.pso.write_mask0=15;s.pso.depth_func=cmp::greater;
     const auto v=effects_decide(s,t0,effects_open,0);CHECK(v.kind==effects_refused&&std::string(v.why)=="stencil-test-not-always");}
    // --- predicates
    {PsoOutput p=base.pso;
     CHECK(!hud_stencil_writer(p)&&!hud_replace_writer(p)&&!hud_equal_test(p));
     p.stencil_write=255;p.stencil_ops_keep=false;p.front_pass=3;CHECK(hud_stencil_writer(p)&&hud_replace_writer(p));
     p.front_pass=2;CHECK(hud_stencil_writer(p)&&!hud_replace_writer(p));                       // ZERO is a writer, not a REPLACE writer
     p.front_pass=3;p.front_func=cmp::equal;CHECK(!hud_replace_writer(p)&&hud_stencil_writer(p)); // REPLACE needs an ALWAYS test
     p.front_func=p.back_func=cmp::equal;p.stencil_write=0;CHECK(hud_equal_test(p)&&!hud_stencil_writer(p));
     p.stencil=false;CHECK(!hud_equal_test(p)&&!hud_stencil_writer(p)&&!hud_replace_writer(p));
     PsoOutput u;CHECK(!hud_stencil_writer(u)&&!hud_replace_writer(u)&&!hud_equal_test(u));}
    {Shape t=base;t.indexed=false;t.count=3;t.pso.depth_write=true;t.pso.depth_func=cmp::less;t.pso.front_func=t.pso.back_func=cmp::equal;t.pso.front_pass=2;
     t.pso.stencil_write=255;t.pso.stencil_ops_keep=false;
     CHECK(radar_a3050_shaped(t));
     {auto x=t;x.indexed=true;CHECK(!radar_a3050_shaped(x));}{auto x=t;x.count=4;CHECK(!radar_a3050_shaped(x));}{auto x=t;x.pso.depth_write=false;CHECK(!radar_a3050_shaped(x));}
     {auto x=t;x.pso.front_func=cmp::always;CHECK(!radar_a3050_shaped(x));}{auto x=t;x.instances=2;CHECK(!radar_a3050_shaped(x));}{auto x=t;x.pso.known=false;CHECK(!radar_a3050_shaped(x));}
     Shape n=base;n.indexed=false;n.count=4;n.pso.front_func=n.pso.back_func=cmp::not_equal;
     CHECK(radar_notequal_shaped(n));
     {auto x=n;x.indexed=true;CHECK(!radar_notequal_shaped(x));}{auto x=n;x.count=3;CHECK(!radar_notequal_shaped(x));}{auto x=n;x.pso.depth_write=true;CHECK(!radar_notequal_shaped(x));}
     {auto x=n;x.pso.back_func=cmp::always;CHECK(!radar_notequal_shaped(x));}{auto x=n;x.pso.stencil=false;CHECK(!radar_notequal_shaped(x));}}
    // --- proof bookkeeping
    {HudRadar r;Shape rep=base;rep.pso.stencil_write=0x3f;rep.pso.stencil_ops_keep=false;rep.pso.front_pass=3;
     Shape w=base;w.pso.stencil_write=255;w.pso.stencil_ops_keep=false;w.pso.front_pass=2;
     radar_note_not_replayed(r,base);CHECK(!r.gap&&!r.stencilGaps);                              // a non-writer skipped: no gap
     radar_note_not_replayed(r,w);CHECK(r.gap&&r.stencilGaps==1&&!r.replace);                   // a writer skipped: gap
     radar_note_replayed(r,rep,false);CHECK(r.replace&&!r.gap&&r.write_mask==0x3f&&r.replaceReplayed==1&&!r.lateReplayed); // a REPLACE writer resets the gap
     radar_note_replayed(r,w,true);CHECK(r.replace&&!r.gap&&r.write_mask==0x3f&&r.replaceReplayed==1&&r.lateReplayed==1); // other replayed writers change nothing
     Shape e=base;e.pso.front_func=e.pso.back_func=cmp::equal;e.count=9;
     radar_note_replayed(r,e,false);radar_note_replayed(r,e,true);CHECK(r.equalReplayed==2&&r.equalIndices==18&&r.lateReplayed==2);
     e.indexed=false;radar_note_replayed(r,e,false);CHECK(r.equalReplayed==3&&r.equalIndices==18);
     HudTarget t;radar_target(r,t);CHECK(t.radar&&t.radar_replace&&!t.radar_gap&&t.radar_write_mask==0x3f);}
    // --- hud_radar_decide
    {HudRadar r;r.replace=true;r.write_mask=255;
     auto act=[&](const Shape& s,bool late,bool window){return hud_radar_decide(s,t0,r,late,window).action;};
     Shape k=base;                                                                    // ALWAYS/KEEP
     CHECK(act(k,false,false)==radar_replay&&act(k,true,true)==radar_replay);
     CHECK(act(k,true,false)==radar_skip_late);                                       // late, no window: counted, never refused
     Shape rep=base;rep.pso.stencil_write=255;rep.pso.stencil_ops_keep=false;rep.pso.front_pass=3;
     CHECK(act(rep,true,false)==radar_replay);                                        // a REPLACE writer opens the window
     Shape e=eq();CHECK(act(e,false,false)==radar_replay&&act(e,true,false)==radar_replay);
     {HudRadar none;CHECK(hud_radar_decide(e,t0,none,false,false).action==radar_refuse&&std::string(hud_radar_decide(e,t0,none,false,false).v.why)=="radar-equal-without-writer");}
     {HudRadar gap=r;gap.gap=true;CHECK(hud_radar_decide(e,t0,gap,false,false).action==radar_refuse);}
     Shape n=base;n.indexed=false;n.count=4;n.pso.front_func=n.pso.back_func=cmp::not_equal;
     CHECK(act(n,false,false)==radar_skip_notequal&&act(n,true,false)==radar_skip_notequal&&act(n,true,true)==radar_skip_notequal);
     {auto x=n;x.indexed=true;CHECK(act(x,false,false)==radar_refuse);}                // a NOT_EQUAL draw of another shape stays refused
     Shape tri=base;tri.indexed=false;tri.count=3;tri.pso.depth_write=true;tri.pso.depth_func=cmp::less;tri.pso.front_func=tri.pso.back_func=cmp::equal;tri.pso.front_pass=2;
     tri.pso.stencil_write=255;tri.pso.stencil_ops_keep=false;
     {auto x=tri;x.om_count=0;CHECK(act(x,false,false)==radar_skip_a3050&&act(x,true,false)==radar_skip_a3050);}
     {auto x=tri;x.game_rt_status="unresolved:rtv-not-tracked";CHECK(act(x,false,false)==radar_skip_a3050);}
     {auto x=tri;x.game_rt=0x6000;CHECK(act(x,false,false)==radar_skip_a3050);}
     CHECK(act(tri,false,false)==radar_refuse); // on the scene target: its depth write keeps it refused
     CHECK(std::string(hud_radar_decide(tri,t0,r,false,false).v.why)=="depth-test-not-replayable");}
    // --- caps and budgets
    CHECK(hud_draw_cap==160&&hud_command_cap==600&&hud_per_draw==3);                  // key off: unchanged
    CHECK(hud_draw_cap_for(false)==160&&hud_command_cap_for(false)==600&&hud_draw_cap_for(true)==256&&hud_command_cap_for(true)==960);
    CHECK(!hud_radar_budget(hud_radar_draw_cap,hud_radar_command_cap,hud_radar_clears,hud_radar_command_bytes,hud_radar_memory_cap));
    static_assert(hud_radar_draw_cap*hud_per_draw+hud_radar_clears<=hud_radar_command_cap,"radar draws fit the radar command cap");
    static_assert(uint64_t(hud_radar_command_cap)*hud_radar_command_bytes<=hud_radar_memory_cap,"radar commands fit the memory estimate");
    CHECK(std::string(hud_radar_budget(0,960,3,64,1u<<20))=="radar-caps-zero"&&std::string(hud_radar_budget(256,0,3,64,1u<<20))=="radar-caps-zero");
    CHECK(std::string(hud_radar_budget(400,960,3,64,1u<<20))=="radar-commands-below-draws");
    CHECK(std::string(hud_radar_budget(256,960,3,64,1000))=="radar-command-memory-over-cap");
    CHECK(!hud_radar_budget(256,771,3,64,1u<<20)&&std::string(hud_radar_budget(256,770,3,64,1u<<20))=="radar-commands-below-draws");
    // --- RADAR-SCENE-RESUME: only a plain full barrier back resumes, at most hud_radar_scene_resume_cap times per region
    CHECK(hud_radar_scene_resume_cap==4&&!radar_scene_back(0,0xffffffffu,0)&&!radar_scene_back(0,0,3));
    CHECK(std::string(radar_scene_back(1,0xffffffffu,0))=="split-barrier"&&std::string(radar_scene_back(2,0,0))=="split-barrier");  // BEGIN_ONLY / END_ONLY
    CHECK(std::string(radar_scene_back(0,1,0))=="partial-subresource"&&std::string(radar_scene_back(0,0,4))=="resume-cap");
    {HudRadar r;CHECK(!r.sceneLeaves&&!r.sceneResumes&&!r.sceneBackRefused);}
    // --- diagnostics container
    {HudRadarDiag g;for(unsigned i=0;i<130;++i)g.add_draw(i%2?'E':'A',i%3?'r':'a');
     char f[radar_diag_draws+1],d[radar_diag_draws+1];const unsigned n=g.ordered(f,d);
     CHECK(n==128&&g.seen==130&&f[0]=='A'&&d[0]=='r'&&f[1]=='E'&&f[127]=='E'&&d[127]=='a'&&std::strlen(f)==128); // the LAST 128 draws, oldest first (draw 2 .. 129)
     HudRadarDiag h;h.add_draw('A','r');h.add_draw('N','q');const unsigned m=h.ordered(f,d);CHECK(m==2&&std::string(f)=="AN"&&std::string(d)=="rq");
     HudRadarDiag v;v.add_verdict(nullptr);v.add_verdict("a");v.add_verdict("a");v.add_verdict("stencil-test-not-always");
     CHECK(v.post==4&&std::string(v.verdictWhy[0])=="accept"&&v.verdictCount[0]==1&&v.verdictCount[1]==2&&v.verdictWhy[2]&&v.verdictCount[2]==1&&!v.verdictOther);
     static const char* const names[]={"v0","v1","v2","v3","v4","v5","v6","v7","v8","v9"};HudRadarDiag o;for(const char* x:names)o.add_verdict(x);
     CHECK(o.post==10&&o.verdictOther==2&&o.verdictCount[7]==1);}
    CHECK(radar_func_char(PsoOutput{})=='?'&&radar_func_char(base.pso)=='A');
    {PsoOutput p=base.pso;p.front_func=cmp::equal;CHECK(radar_func_char(p)=='E');p.front_func=cmp::not_equal;CHECK(radar_func_char(p)=='N');p.stencil=false;CHECK(radar_func_char(p)=='-');}
    // --- config: default OFF, the usual boolean forms, duplicates / out-of-range rejected, never implies the HUD key
    using namespace acz::diagnostics;
    CHECK(!Config{}.gpu_copy_step5_hud_radar()&&!parse_config("").config.gpu_copy_step5_hud_radar());
    for(const auto* v:{"true","1"}){auto c=parse_config(std::string("gpu_copy_step5_hud_radar=")+v);CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_hud_radar()&&!c.config.gpu_copy_step5_hud());}
    for(const auto* v:{"false","0"}){auto c=parse_config(std::string("gpu_copy_step5_hud_radar=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.gpu_copy_step5_hud_radar());}
    CHECK(parse_config("gpu_copy_step5_hud_radar=on").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_hud_radar=1\ngpu_copy_step5_hud_radar=0").issue==ConfigIssue::duplicate);
    CHECK(!parse_config("gpu_copy_step5_hud=1").config.gpu_copy_step5_hud_radar());
    {const char* keys[]={"log_enabled=true","diagnostic_ownership_pins=true","gpu_copy_witness_observation=true","gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true","gpu_copy_step3_whole_set=true","gpu_copy_step5_continuous=true","xr_stereo_output=true","xr_edge_feather=0.06",
        "gpu_copy_step5_members_by_class=true","gpu_copy_step5_hmd_fov=true","gpu_copy_step5_hud=true","xr_hud_layer=true","xr_hud_quad_distance_m=0.8",
        "xr_hud_scale=0.5","xr_world_quad_distance_m=500","gpu_copy_step5_units_per_metre=87","gpu_copy_step5_hud_markers=true","xr_hud_markers_layer=true",
        "xr_world_in_projection=true","gpu_copy_step5_world_objects=true","gpu_copy_step5_world_observe=true","xr_world_hold_last=true","xr_world_extend=true",
        "gpu_copy_step5_hud_radar=true","gpu_copy_step5_png_once=true","gpu_copy_step5_effects=true","xr_effects_layer=true","xr_world_objects_depth_fix=true",
        "xr_png_once=true","xr_look_drive_keys=true","gpu_copy_step5_phase_observe=true"};
     std::string cfg;for(const char* k:keys)cfg+=std::string(k)+"\n";
     const auto c=parse_config(cfg);
     CHECK(std::size(keys)==max_config_keys&&cfg.size()<=max_config_bytes&&c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_hud_radar()&&c.config.gpu_copy_step5_hud()); // 32 keys: the cap, still accepted
     CHECK(parse_config(cfg+"xr_look_drive_test_sweep=true").issue==ConfigIssue::oversized);}
}
// Key OFF (or ON without anything for it to do): byte-identical to the build without the key.
static void step5_hud_radar_off() {
    radarSetup(true,false); // the HUD key on, the radar key OFF
    auto* src=acz::stereo::frame_source();(void)src;
    CHECK(!acz::hudRadarConfigured());
    CHECK(radarOffScripts().size()==std::size(radarGolden));
    for(size_t i=0;i<radarOffScripts().size();++i) {
        std::string raw;const std::string d=radarDigest(radarOffScripts()[i],i+1,&raw);
        if(d!=radarGolden[i]){std::fprintf(stderr,"script %zu [%s]: digest %s, golden %s\n  %s\n",i,radarOffScripts()[i].c_str(),d.c_str(),radarGolden[i],raw.c_str());CHECK(false);}
    }
    {const auto& s=s5ref();CHECK(!s.radarFrames&&!s.radarDiagPending&&!s.radarDiagEmitted&&!s.radarEqual&&!s.radarExcursions);}
    {std::lock_guard lock(acz::g_hudRegionMutex);CHECK(!acz::g_hudRegion.radar&&!acz::g_hudRegion.away&&!acz::g_hudRegion.excursions);}
    ticks(60);const auto text=hudLog();
    CHECK(text.find("hud-radar")==std::string::npos&&text.find("event=hud-end")==std::string::npos);
    CHECK(text.find("caps=draws:160,commands:600;")!=std::string::npos);
    for(const auto& l:w->lines)CHECK(l.find("radar")==std::string::npos);
    // The legacy facts the golden digests encode, spelled out: the region ends at the first other target, EQUAL / NOT_EQUAL are refused.
    // (The golden digests above are the strict check; these name the behaviour.)
    {const auto& s=s5ref();CHECK(s.hudEnd[0]==4&&s.hudValidFrames+s.hudInvalidFrames==4);}
    // The key raised after arming changes nothing for a script with nothing for the radar to do: same ops, same HUD facts (counters excluded).
    for(const char* script:{"p p k p p","k p R p k"}) {
        std::string offRaw,onRaw;
        acz::g_hudRadarTest=false;const size_t b0=radarFrame(script);acz::stereo::StereoFrame f0;CHECK(src->try_acquire_latest(0,f0));const std::string off=radarNorm(b0,f0.slot);
        const unsigned offDraws=f0.hud_draws,offRef=f0.hud_refused;const bool offValid=f0.hud_valid;src->release(f0);
        acz::g_hudRadarTest=true;const size_t b1=radarFrame(script);acz::stereo::StereoFrame f1;CHECK(src->try_acquire_latest(0,f1));const std::string on=radarNorm(b1,f1.slot);
        CHECK(f1.hud_draws==offDraws&&f1.hud_refused==offRef&&f1.hud_valid==offValid&&offValid);
        // the normalised stream names the slot's own targets by class, so it is slot independent
        if(on!=off){std::fprintf(stderr,"script [%s]:\n  off %s\n  on  %s\n",script,off.c_str(),on.c_str());CHECK(false);}
        src->release(f1);
        acz::g_hudRadarTest=false;
    }
    step3Lines();
}
// Region handling: excursions (bind or draw on another target), return, window, cap, barrier, later member, unresolved a3050.
static void step5_hud_radar_region() {
    radarSetup(true,true);
    CHECK(acz::hudRadarConfigured());
    uint64_t id=0;
    auto run=[&](const char* script,const char* replays,unsigned draws,unsigned refused,bool valid,const RadarSnap& want,const char* what) {
        const auto before=radarSnap();size_t b=0;const auto f=radarRun(++id,script,&b);
        const auto d=radarSnap()-before;
        const std::string got=radarReplayed(b,f.slot);
        bool ok=got==replays&&f.hud_draws==draws&&f.hud_refused==refused&&f.hud_valid==valid&&(f.hud!=nullptr)==valid;
        ok=ok&&d.exc==want.exc&&d.ret==want.ret&&d.late==want.late&&d.win==want.win&&d.lateSk==want.lateSk&&d.awaySk==want.awaySk&&d.a3050==want.a3050&&d.ne==want.ne&&d.rep==want.rep&&d.eq==want.eq;
        if(!ok){std::fprintf(stderr,"%s [%s]: replays [%s] want [%s]; draws %u/%u refused %u/%u valid %d/%d; exc %llu/%llu ret %llu/%llu late %llu/%llu win %llu/%llu lateSk %llu/%llu awaySk %llu/%llu a3050 %llu/%llu ne %llu/%llu rep %llu/%llu eq %llu/%llu\n",
            what,script,got.c_str(),replays,f.hud_draws,draws,f.hud_refused,refused,int(f.hud_valid),int(valid),
            (unsigned long long)d.exc,(unsigned long long)want.exc,(unsigned long long)d.ret,(unsigned long long)want.ret,(unsigned long long)d.late,(unsigned long long)want.late,
            (unsigned long long)d.win,(unsigned long long)want.win,(unsigned long long)d.lateSk,(unsigned long long)want.lateSk,(unsigned long long)d.awaySk,(unsigned long long)want.awaySk,
            (unsigned long long)d.a3050,(unsigned long long)want.a3050,(unsigned long long)d.ne,(unsigned long long)want.ne,(unsigned long long)d.rep,(unsigned long long)want.rep,
            (unsigned long long)d.eq,(unsigned long long)want.eq);CHECK(false);}
        return f;
    };
    auto W=[](uint64_t exc,uint64_t ret,uint64_t late,uint64_t win,uint64_t lateSk,uint64_t awaySk,uint64_t a3050,uint64_t ne,uint64_t rep,uint64_t eq){
        RadarSnap r;r.exc=exc;r.ret=ret;r.late=late;r.win=win;r.lateSk=lateSk;r.awaySk=awaySk;r.a3050=a3050;r.ne=ne;r.rep=rep;r.eq=eq;return r;};
    // The final output pass of every script is one more excursion (a bind + one draw on it).
    // (a) a draw on another target is an excursion: not replayed, not refused; the scene target's return ends it; late draws outside a window are skipped.
    run("p p o k @ k k","i6 n3",2,0,true,W(2,1,0,0,2,2,0,0,0,0),"excursion, return, late draws without a window");
    // (b) a REPLACE writer after the return opens a window: the following draws replay; the next excursion closes it.
    run("p o @ R k o @ k","i6 i6 i6",3,0,true,W(3,2,2,1,1,1,0,0,1,0),"window opened by a REPLACE writer, closed by an excursion");
    // (c) an EQUAL draw can open the window too, but only with the proof (here the REPLACE writer before the excursion survives it).
    run("R o @ E6 k","i6 i6 i6",3,0,true,W(2,1,2,1,0,1,0,0,1,1),"EQUAL after an excursion with the proof");
    // (d) a bind-only excursion (no draw on the other target) is an excursion too.
    run("p o @ k","i6",1,0,true,W(2,1,0,0,1,1,0,0,0,0),"bind-only excursion");
    // (e) the excursion cap: the 9th excursion ends the region (why excursion-cap); the key-off region would have ended at the first.
    {std::string s="p";for(int i=0;i<9;++i)s+=" o @";s+=" R k";
     const auto before=radarSnap();const auto f=run(s.c_str(),"i6",1,0,true,W(9,8,0,0,0,0,0,0,0,0),"excursion cap");
     const auto d=radarSnap()-before;CHECK(d.end0==1);(void)f;}
    // (f) the scene target leaving RENDER_TARGET still ends the region for good (even with a radar window open).
    {const auto before=radarSnap();size_t b=0;
     const auto f=radarRun(++id,"",&b,[]{hudBindScene();radarScript("R o @ R k");hudSceneBarrier(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
         hudSceneBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);radarScript("R k E6");hudOutput();});
     const auto d=radarSnap()-before;
     radarWant(radarReplayed(b,f.slot),"i6 i6 i6","barrier");CHECK(f.hud_valid&&f.hud_draws==3&&f.hud_refused==0&&d.end1==1);}
    // (g) a later cockpit member restarts the region AND the stencil proof: EQUAL after it needs a new REPLACE writer.
    {const auto before=radarSnap();size_t b=0;
     auto f=radarRun(++id,"",&b,[]{hudBindScene();radarScript("R E6 M E6");hudOutput();});
     radarWant(radarReplayed(b,f.slot),"i6 i6","member reset, no writer");CHECK(!f.hud_valid&&f.hud_draws==0&&f.hud_refused==1);   // the pre-member draws are forgotten
     f=radarRun(++id,"",&b,[]{hudBindScene();radarScript("R E6 M R E6");hudOutput();});
     radarWant(radarReplayed(b,f.slot),"i6 i6 i6 i6","member reset, new writer");CHECK(f.hud_valid&&f.hud_draws==2&&f.hud_refused==0);
     const auto d=radarSnap()-before;CHECK(d.resets==2&&d.valid==1&&d.invalid==1);}
    // (h) an a3050-shaped triangle on an unresolved single target: skipped by class (counted), the frame stays valid; any other unresolved draw is still refused.
    run("p u T @ p","i6 n3",2,0,true,W(1,0,0,0,0,1,1,0,0,0),"unresolved a3050 triangle");
    run("p u p @ p","i6 i6",2,1,false,W(1,0,0,0,0,1,0,0,0,0),"unresolved plain draw stays refused");
    // (i) no target bound: the a3050 triangle is counted, never refused (the original no-target rule).
    run("p z T @ p","i6 n3",2,0,true,W(1,0,0,0,0,1,1,0,0,0),"no-target triangle");
    // (k) RADAR-SCENE-RESUME: the step39 shape. A bind of another target (excursion), then the scene leaves RENDER_TARGET while away (the radar's mid
    // pass reads it), then comes back: the region resumes (still away), the scene bind returns, the NOT_EQUAL quad is skipped, the REPLACE writer opens
    // the window and the EQUAL contact draw + the closing line replay. Before this change everything after the barrier was lost (end=scene-barrier).
    {const auto before=radarSnap();const auto r0=s5ref().radarSceneResumes,l0=s5ref().radarSceneLeaves,x0=s5ref().radarSceneBackRefused;size_t b=0;
     const auto f=radarRun(++id,"",&b,[]{hudBindScene();radarScript("R k o");
         hudSceneBarrier(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
         radarScript("z T");                                                    // the a3050 triangle with no colour target (skipped, counted)
         hudSceneBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
         radarScript("@ N R E6 k7");hudOutput();});
     const auto d=radarSnap()-before;
     radarWant(radarReplayed(b,f.slot),"i6 i6 i6 i6 i7","scene resume after a barrier taken while away");
     CHECK(f.hud_valid&&f.hud_draws==5&&f.hud_refused==0);
     // the triangle drawn while the scene is out of RENDER_TARGET is not seen by the (ended) HUD region: a3050 stays 0
     CHECK(d.exc==2&&d.ret==1&&d.ne==1&&d.rep==2&&d.eq==1&&d.win==1&&d.late==3&&d.a3050==0&&d.end1==1&&d.end0==0);
     // two leaves while away (the radar pass and the fixture's output pass), one resume each (the output pass's barrier back has nothing after it)
     CHECK(s5ref().radarSceneLeaves-l0==2&&s5ref().radarSceneResumes-r0==2&&s5ref().radarSceneBackRefused-x0==0);}
    // (l) split / partial barriers back and the resume cap refuse (pure rule radar_scene_back, step5-hud-radar-rules). (m) the scene leaving
    // RENDER_TARGET while NOT away is never resumed: test (f) above (its barrier back resumes nothing, "R k E6" after it stays unreplayed).
    // (j) key off for the same excursion script: the region ends at the first other target (legacy), nothing after it is replayed.
    acz::g_hudRadarTest=false;
    {size_t b=0;const auto f=radarRun(++id,"p p o k @ k k",&b);radarWant(radarReplayed(b,f.slot),"i6 n3","key off: legacy end");CHECK(f.hud_valid&&f.hud_draws==2);}
    // key off, the step39 shape: the legacy end at the bind; the barrier back never reopens anything.
    {size_t b=0;const auto f=radarRun(++id,"",&b,[]{hudBindScene();radarScript("R k o");
         hudSceneBarrier(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
         hudSceneBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);radarScript("@ N R E6 k7");hudOutput();});
     radarWant(radarReplayed(b,f.slot),"i6 i6","key off: step39 shape ends at the bind");
     std::lock_guard lock(acz::g_hudRegionMutex);CHECK(!acz::g_hudRegion.resumed&&!acz::g_hudRegion.resumes);}
    acz::g_hudRadarTest=true;
    step3Lines();
}
// The radar group: a frame with N contacts, EQUAL only with a replayed REPLACE writer, skips, diagnostics, command stream.
static void step5_hud_radar() {
    radarSetup(true,true);
    auto* src=acz::stereo::frame_source();(void)src;
    {auto& s5=s5ref();CHECK(s5.hudReady);}
    uint64_t id=0;
    // --- the hud-end diagnostic comes FIRST (only the first three frames with a region-ending event are logged within 5 s): k(A) on the scene,
    // a bind of the output (excursion), k on it, the scene again, R, E7 (accepted: the REPLACE writer was replayed), the output pass.
    {size_t b=0;const auto f=radarRun(++id,"k o k @ R E7",&b);radarWant(radarReplayed(b,f.slot),"i6 i6 i7","diagnostic frame");CHECK(f.hud_valid&&f.hud_draws==3&&f.hud_refused==0);}
    const std::string P="k4 k9 k9 R R z T @ N R R z T @ ";   // rings / cross / cone, mask quads, a3050 triangle (no target), NOT_EQUAL quad, mask again, triangle again
    // --- the replayed draw count and the index sum grow with the contacts
    uint64_t lastEq=0;
    for(unsigned contacts:{0u,3u,7u}) {
        std::string script=P;for(unsigned i=0;i<contacts;++i)script+="E6 ";script+="k7";
        const auto before=radarSnap();size_t b=0;unsigned clears=0;const auto f=radarRun(++id,script,&b);
        const auto d=radarSnap()-before;
        std::string want="i4 i9 i9 i6 i6 i6 i6";for(unsigned i=0;i<contacts;++i)want+=" i6";want+=" i7";
        radarWant(radarReplayed(b,f.slot,&clears),want.c_str(),"radar frame");
        if(!(f.hud_valid&&f.hud&&f.hud_draws==8+contacts&&f.hud_refused==0&&clears==1&&d.eq==contacts&&d.eqIdx==6*contacts&&d.rep==4&&d.a3050==2&&d.ne==1&&
             d.gaps==0&&d.eqRef==0&&d.gapRef==0&&d.excW==2&&d.valid==1)){
            std::fprintf(stderr,"contacts %u: valid %d draws %u refused %u clears %u eq %llu idx %llu rep %llu a3050 %llu ne %llu gaps %llu\n",contacts,int(f.hud_valid),f.hud_draws,
                f.hud_refused,clears,(unsigned long long)d.eq,(unsigned long long)d.eqIdx,(unsigned long long)d.rep,(unsigned long long)d.a3050,(unsigned long long)d.ne,(unsigned long long)d.gaps);CHECK(false);}
        CHECK(d.eq>=lastEq);lastEq=d.eq;
    }
    {const auto& s=s5ref(); // the diagnostic frame (R E7 after an excursion) + the three contact frames
     CHECK(s.radarFrames==4&&s.radarEqual==11&&s.radarEqualIndices==67&&s.radarMaxEqual==7&&s.radarEqualFrames==3);
     // group draws = REPLACE + EQUAL + late replays: 4 + {0,3,7} for the contact frames, 1 + 1 + 2 for the diagnostic frame
     CHECK(s.radarGroupSum==4*3+10+4&&s.radarMaxGroup==11);}
    // --- EQUAL only with a replayed REPLACE writer and no skipped writer; the mutation (accept without the writer) would fail the first rows
    struct Row {const char* script;const char* replays;unsigned draws,refused;bool valid;const char* why;};
    const std::vector<Row> rows{
        {"E6",        "",             0,1,false,"radar-equal-without-writer"},
        {"E6 R",      "i6",           1,1,false,"radar-equal-without-writer"},
        {"R E6",      "i6 i6",        2,0,true, nullptr},
        {"R N E6",    "i6 i6",        2,0,true, nullptr},                 // a NOT_EQUAL quad that writes nothing leaves the proof intact
        {"R W E6",    "i6",           1,1,false,"stencil-writer-skipped"}, // a skipped NOT_EQUAL WRITER leaves a gap
        {"R W R E6",  "i6 i6 i6",     3,0,true, nullptr},                 // a new REPLACE writer closes the gap
        {"r E6",      "i6",           1,1,false,"radar-mask-not-covered"}, // the writer's mask (0x0f) does not cover the read mask (255)
        {"R e6",      "i6",           1,1,false,"radar-equal-writes-stencil"},
        {"R x E6",    "i6",           1,2,false,"stencil-writer-skipped"}, // a REFUSED stencil writer (test LESS) is a skipped writer too
        {"R k E6 E6", "i6 i6 i6 i6",  4,0,true, nullptr},
    };
    for(const auto& r:rows) {
        const auto before=radarSnap();size_t b=0;const auto f=radarRun(++id,r.script,&b);const auto d=radarSnap()-before;
        const std::string got=radarReplayed(b,f.slot);
        if(!(got==r.replays&&f.hud_draws==r.draws&&f.hud_refused==r.refused&&f.hud_valid==r.valid&&(f.hud!=nullptr)==r.valid)){
            std::fprintf(stderr,"row [%s]: replays [%s] want [%s] draws %u/%u refused %u/%u valid %d/%d\n",r.script,got.c_str(),r.replays,f.hud_draws,r.draws,f.hud_refused,r.refused,int(f.hud_valid),int(r.valid));CHECK(false);}
        if(r.why)CHECK(std::string(s5ref().hudLastWhy)==r.why);
        if(std::string(r.script)=="R W E6")CHECK(d.ne==1&&d.gaps==1&&d.gapRef==1);
        if(std::string(r.script)=="r E6"||std::string(r.script)=="R e6"||std::string(r.script)=="E6")CHECK(d.eqRef==1);
    }
    // --- the hud-end diagnostic (first frames with an excursion) and the status lines
    ticks(60);const auto text=hudLog();
    for(const char* k:{"gpu-copy-step1;event=hud-radar-ring;key=gpu_copy_step5_hud_radar;caps=draws:256,commands:960;excursion-cap=8;","budget=ok",
            "event=hud-radar-legend;","stencil-ref=untracked","gpu-copy-step1;event=hud-ring;outcome=ready;reason=none;slots=3;","caps=draws:256,commands:960;",
            "event=hud-end;","end=scene-barrier;first-excursion=rt-change;","om-count=","post-excursion-draws=","event=hud-end-verdicts;","stencil-test-not-always:","event=hud-end-draws;",
            "stencil-func=","disposition=","hud-radar;frames=","replayed=replace:","skipped=a3050:",
            "hud-radar-resume;scene-leaves-away=","event=hud-end-resume;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    // the diagnostic line of the last frame: k(A,r) k-on-output(A,a) R(A,r) E7(E,r) + the output pass (E,a)
    CHECK(text.find("stencil-func=AAAEE;disposition=rarra")!=std::string::npos);
    CHECK(text.find("last=5/5;")!=std::string::npos);
    // At most three hud-end diagnostics in the first five seconds.
    {size_t n=0,pos=0;while((pos=text.find("event=hud-end;",pos))!=std::string::npos){++n;pos+=4;}CHECK(n>=1&&n<=3);}
    for(const auto& l:w->lines)CHECK(l.find("hud-end")==std::string::npos&&l.find("radar")==std::string::npos); // dedicated log only
    step3Lines();
}
// Caps 256 / 960 and the budgets.
static void step5_hud_radar_caps() {
    radarSetup(true,true);auto* src=acz::stereo::frame_source();
    // 257 draws: 256 replayed, the 257th refused (draw cap); the original key-off cap was 160.
    const size_t b=hudFrame(257);
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&!f.hud&&!f.hud_valid&&f.hud_draws==256&&f.hud_refused==1);
    {unsigned c=0;const std::string got=radarReplayed(b,f.slot,&c);CHECK(c==1);size_t n=0;for(char ch:got)n+=ch=='i'||ch=='n';CHECK(n==256);}
    src->release(f);
    {const auto& s5=s5ref();CHECK(s5.hudReason[3]==1&&s5.hudCapFrames==1&&std::string(s5.hudLastWhy)=="hud-cap");}
    // 256 exactly: valid (2 + 768 = 770 commands <= 960).
    hudFrame(256);CHECK(src->try_acquire_latest(1,f)&&f.frame_id==2&&f.hud_valid&&f.hud_draws==256&&f.hud_refused==0);src->release(f);
    // The command cap across a member resume: 2 + 3*250 = 752; after the resume 2 + 3k <= 208 -> 68 replayed, the other 12 refused.
    hudFrame(0,[]{hudBindScene();hudDraws(250);hudLateMember();hudBindScene();hudDraws(80);hudOutput();});
    CHECK(src->try_acquire_latest(2,f)&&f.frame_id==3&&!f.hud_valid&&f.hud_draws==68&&f.hud_refused==12);src->release(f);
    {const auto& s5=s5ref();CHECK(s5.hudReason[3]==13&&s5.hudCapFrames==2&&s5.hudResets==1);}
    // Key off keeps 160 / 600 (the original numbers), also when the other keys are on.
    acz::g_hudRadarTest=false;
    hudFrame(161);CHECK(src->try_acquire_latest(3,f)&&f.frame_id==4&&!f.hud_valid&&f.hud_draws==160&&f.hud_refused==1);src->release(f);
    hudFrame(0,[]{hudBindScene();hudDraws(150);hudLateMember();hudBindScene();hudDraws(60);hudOutput();});
    CHECK(src->try_acquire_latest(4,f)&&f.frame_id==5&&!f.hud_valid&&f.hud_draws==48&&f.hud_refused==12);src->release(f);
    step3Lines();
}
// ------------------------------------------------------------- J-hud-markers (gpu_copy_step5_hud_markers)
// Draw scripts: one letter per HUD draw. B = depth-GREATER box (indexed), L = text-PSO draw (indexed), I = instrument
// (non-indexed), J = other-PSO indexed draw, X = text-PSO non-indexed draw, Z = box the replay refuses (stencil != ALWAYS),
// Q = instrument the replay refuses. The expected target string uses H (HUD target) and M (marker target) per REPLAYED draw.
static Object boxPsoObj,boxBadPsoObj,textPsoObj,instPsoObj,instBadPsoObj;
static ID3D12PipelineState* boxPso(){return reinterpret_cast<ID3D12PipelineState*>(&boxPsoObj);}
static ID3D12PipelineState* boxBadPso(){return reinterpret_cast<ID3D12PipelineState*>(&boxBadPsoObj);}
static ID3D12PipelineState* textPso(){return reinterpret_cast<ID3D12PipelineState*>(&textPsoObj);}
static ID3D12PipelineState* instPso(){return reinterpret_cast<ID3D12PipelineState*>(&instPsoObj);}
static ID3D12PipelineState* instBadPso(){return reinterpret_cast<ID3D12PipelineState*>(&instBadPsoObj);}
static void markerSetup(bool markersKey) {
    acz::g_hudMarkersTest=markersKey;hudSetup(true);
    for(Object* o:{&boxPsoObj,&boxBadPsoObj,&textPsoObj,&instPsoObj,&instBadPsoObj}){o->table=w->psoTable.data();o->kind=k_pso;}
    hudPipeline(boxPso(),[](auto& o){o.depth_func=gpu_copy::step3::cmp::greater;});
    hudPipeline(boxBadPso(),[](auto& o){o.depth_func=gpu_copy::step3::cmp::greater;o.front_func=gpu_copy::step3::cmp::less;});
    hudPipeline(textPso(),nullptr);hudPipeline(instPso(),nullptr);
    hudPipeline(instBadPso(),[](auto& o){o.front_func=gpu_copy::step3::cmp::less;});
}
static void markerDraws(const std::string& seq) {
    auto* l=listAlias(0);
    for(char c:seq) {
        ID3D12PipelineState* p=c=='B'?boxPso():c=='Z'?boxBadPso():(c=='L'||c=='X')?textPso():c=='Q'?instBadPso():instPso();
        l->SetPipelineState(p);bindT10(srtOff);
        const auto before=allocations;traceAllocations=true;
        if(c=='B'||c=='Z'||c=='L'||c=='J')l->DrawIndexedInstanced(c=='L'?5:6,1,0,0,0);else l->DrawInstanced(3,1,2,0);
        traceAllocations=false;CHECK(allocations==before); // no heap allocation on the (classifying) HUD Draw path
    }
}
static size_t markerFrame(const std::string& seq) {
    return hudFrame(0,[&]{hudBindScene();markerDraws(seq);hudOutput();});
}
// The exact replay stream of a split frame: per replayed draw OM(target) Draw OM(restore) game-Draw, targets in script order,
// one ClearRTV per used target and one ClearDSV for the frame BEFORE the first replay (`segments` > 1: a late cockpit member
// restarted the region, every count repeats).
static void checkSplitTail(size_t begin,const std::string& want,unsigned slot,unsigned segments=1) {
    const auto& L=w->lists[0];const auto& sl=s5ref().slot[slot];
    std::string got;unsigned hudClears=0,markClears=0,dsClears=0;bool anyReplay=false;
    for(size_t i=begin;i<L.ops.size();++i) {
        const Op& o=L.ops[i];
        if(o.t==Op::clear_rtv){
            if(o.rtv[0]==sl.hudRtv.ptr)++hudClears;else if(o.rtv[0]==sl.hudMarkerRtv.ptr)++markClears;
            continue;}
        if(o.t==Op::clear_dsv){
            if(o.dsv==sl.hudDsv.ptr){CHECK(o.depth==0.0f&&o.clearFlags==D3D12_CLEAR_FLAG_DEPTH);if(segments==1)CHECK(!anyReplay);++dsClears;}
            continue;}
        if(o.t==Op::om&&(o.rtv[0]==sl.hudRtv.ptr||o.rtv[0]==sl.hudMarkerRtv.ptr)) {
            const bool marker=o.rtv[0]==sl.hudMarkerRtv.ptr;
            CHECK(o.count==1&&o.dsvSet&&o.dsv==sl.hudDsv.ptr&&i+3<L.ops.size()); // the HUD depth-stencil is shared by both targets
            const Op& d=L.ops[i+1];const Op& r=L.ops[i+2];const Op& g=L.ops[i+3];
            CHECK(d.t==Op::draw&&d.boundRtv==o.rtv[0]&&d.boundDsv==sl.hudDsv.ptr);
            CHECK(r.t==Op::om&&r.count==1&&originOf(r.rtv[0])==gameRtvHandle&&r.dsvSet&&originOf(r.dsv)==gameDsvHandle);
            CHECK(g.t==Op::draw&&originOf(g.boundRtv)==gameRtvHandle&&d.indexed==g.indexed&&d.dc==g.dc&&d.ds==g.ds&&d.boundT10==g.boundT10);
            got+=marker?'M':'H';anyReplay=true;i+=3;continue;
        }
        if(o.t==Op::om&&o.rtv[0]==hudOutputHandle)break;
    }
    if(got!=want){std::fprintf(stderr,"targets %s, want %s\n",got.c_str(),want.c_str());CHECK(false);}
    const bool usesH=want.find('H')!=std::string::npos,usesM=want.find('M')!=std::string::npos;
    const unsigned s=segments;
    if(!(hudClears==(usesH?s:0u)&&markClears==(usesM?s:0u)&&dsClears==((usesH||usesM)?s:0u))){
        std::fprintf(stderr,"clears hud %u marker %u ds %u (segments %u) for %s\n",hudClears,markClears,dsClears,s,want.c_str());CHECK(false);}
}
static uint64_t markerCharge(uint64_t width,uint32_t height){return 3*(gpu_copy::step3::align_up(width*height*fakeBytes(shape3.rtv),65536)+65536);}
static void step5_hud_markers_rules() {
    using namespace gpu_copy::step5;using namespace gpu_copy::step3;
    // Classification table over (depth-tested, indexed, PSO route) sequences. T = text route, O = another route.
    struct Row {const char* what;std::string seq;std::vector<int> want;};
    // seq letters: b box, t text indexed, o other indexed, n text-route non-indexed, i instrument non-indexed
    auto run=[](const std::string& seq){
        HudMarkerState st;std::vector<int> out;
        for(char c:seq){const bool box=c=='b';const bool idx=c=='b'||c=='t'||c=='o';const uint64_t route=(c=='t'||c=='n')?0x70:c=='b'?0x50:0x90;
            out.push_back(int(hud_classify(st,box,idx,route)));}
        return out;
    };
    enum {I=hud_instrument,B=hud_marker_box,L=hud_marker_label};
    const std::vector<Row> rows{
        {"empty",                         "",         {}},
        {"instruments only",              "iioi",     {I,I,I,I}},
        {"B L B L B L (run 11 frame 1)",  "btbtbt",   {B,L,B,L,B,L}},
        {"label learned from the first follower",  "ibtiit", {I,B,L,I,I,I}},
        {"B B L: the label follows the 2nd box",   "bbt",  {B,B,L}},
        {"text draw not after a box is an instrument", "tbtt", {I,B,L,I}},
        {"a non-indexed follower is no label",     "bnt",  {B,I,I}},
        {"first follower defines the text route (heuristic)", "botbt", {B,L,I,B,I}},
        {"box between: label route kept",          "btibt", {B,L,I,B,L}},
        {"box, instrument, text: text is an instrument", "bit", {B,I,I}},
        {"run 7 frame 1800 shape B L B L B L B B L B", "btbtbtbbtb", {B,L,B,L,B,L,B,B,L,B}},
    };
    for(const auto& r:rows){const auto got=run(r.seq);if(got!=r.want){std::fprintf(stderr,"classify %s: %s\n",r.what,r.seq.c_str());CHECK(false);}}
    // Shape-level depth test: only an ENABLED GREATER/GREATER_EQUAL test counts, independently of replayability.
    Shape s;s.pso.known=true;s.pso.depth_enable=true;s.pso.depth_func=cmp::greater;
    CHECK(hud_depth_tested(s));s.pso.depth_func=cmp::greater_equal;CHECK(hud_depth_tested(s));
    for(uint32_t f:{cmp::always,cmp::never,cmp::less,cmp::less_equal,cmp::equal,cmp::not_equal,0u}){s.pso.depth_func=f;CHECK(!hud_depth_tested(s));}
    s.pso.depth_func=cmp::greater;s.pso.depth_enable=false;CHECK(!hud_depth_tested(s));
    s.pso.depth_enable=true;s.pso.known=false;CHECK(!hud_depth_tested(s));
    // Memory accounting: 2560x1440 FP16 x 3 slots = 88.7 MB charged, refused above the per-target and total caps.
    uint64_t charged=7;
    CHECK(!hud_markers_budget(10,2560,1440,slots,charged)&&charged==3*(29491200ull+65536));
    CHECK(!hud_markers_budget(10,256,144,slots,charged)&&charged==3*(align_up(256ull*144*8,65536)+65536));
    CHECK(std::string(hud_markers_budget(10,4096,2160,slots,charged))=="hud-markers-over-slot-cap"&&charged==0);
    CHECK(std::string(hud_markers_budget(2,2560,1440,slots,charged))=="hud-markers-format-unsupported"&&charged==0); // FP32 scene targets: not a step-3 colour format, refused
    CHECK(std::string(hud_markers_budget(10,2560,1440,5,charged))=="hud-markers-over-total-cap"&&charged==0);
    CHECK(std::string(hud_markers_budget(0,2560,1440,slots,charged))=="hud-markers-format-unsupported");
    CHECK(std::string(hud_markers_budget(10,0,1440,slots,charged))=="hud-markers-size-unsupported"&&std::string(hud_markers_budget(10,2560,0,slots,charged))=="hud-markers-size-unsupported");
    CHECK(std::string(hud_markers_budget(10,20000,10,slots,charged))=="hud-markers-size-unsupported");
    CHECK(hud_marker_slot_cap==rt_byte_cap&&3*(29491200ull+65536)<=hud_marker_total_cap);
    // Config: defaults OFF, boolean forms, duplicates/out-of-range rejected, the producer key alone does not enable the HUD.
    using namespace acz::diagnostics;
    CHECK(!Config{}.gpu_copy_step5_hud_markers()&&!parse_config("").config.gpu_copy_step5_hud_markers());
    for(const auto* v:{"true","1"}){auto c=parse_config(std::string("gpu_copy_step5_hud_markers=")+v);CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_hud_markers()&&!c.config.gpu_copy_step5_hud());}
    for(const auto* v:{"false","0"}){auto c=parse_config(std::string("gpu_copy_step5_hud_markers=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.gpu_copy_step5_hud_markers());}
    CHECK(parse_config("gpu_copy_step5_hud_markers=on").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_hud_markers=1\ngpu_copy_step5_hud_markers=0").issue==ConfigIssue::duplicate);
    CHECK(!parse_config("gpu_copy_step5_hud=1").config.gpu_copy_step5_hud_markers());
}
static void step5_hud_markers_off() {
    // Key OFF (HUD on): marker-shaped draws are ordinary HUD draws in the HUD target; no marker resources, fields or lines.
    markerSetup(false);auto* src=acz::stereo::frame_source();
    {auto& s5=s5ref();CHECK(s5.hudReady&&!s5.hudMarkersReady&&std::string(s5.hudMarkersArmWhy)=="not-configured");for(const auto& sl:s5.slot)CHECK(!sl.hudMarkerRt&&!sl.hudMarkerRtvHeap);}
    const std::string seq="IBLIBLJ";
    const size_t begin=markerFrame(seq);
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f));
    checkSplitTail(begin,std::string(seq.size(),'H'),f.slot);
    CHECK(f.hud&&f.hud_valid&&f.hud_draws==7&&f.hud_refused==0&&hudCovered(f.hud)>100);
    CHECK(!f.hud_markers&&!f.hud_markers_valid&&f.hud_marker_draws==0&&f.hud_marker_refused==0&&!f.tac);src->release(f);
    {const auto& s5=s5ref();CHECK(s5.hudDepthTested==2&&s5.hudMarkerValidFrames==0&&s5.hudMarkerInvalidFrames==0&&s5.hudMarkerAbsentFrames==0&&s5.hudMarkerDrawSum==0);}
    ticks(60);const auto text=hudLog();
    CHECK(text.find("hud-markers")==std::string::npos&&text.find("marker")==std::string::npos);
    for(const auto& l:w->lines)CHECK(l.find("marker")==std::string::npos&&l.find("hud")==std::string::npos);
    // The exact pre-existing stream: ClearRTV(hud) immediately followed by ClearDSV(hud) before the first replay, one pair per frame.
    {const auto& L=w->lists[0];size_t first=SIZE_MAX;for(size_t i=begin;i<L.ops.size();++i)if(L.ops[i].t==Op::clear_rtv){first=i;break;}
     CHECK(first!=SIZE_MAX&&first+2<L.ops.size()&&L.ops[first+1].t==Op::clear_dsv&&L.ops[first+2].t==Op::om);}
    // The key raised AFTER arming (the second targets do not exist): still the pre-existing stream, no marker state or lines.
    acz::g_hudMarkersTest=true;const size_t b2=markerFrame(seq);acz::g_hudMarkersTest=false;
    CHECK(src->try_acquire_latest(1,f));checkSplitTail(b2,std::string(seq.size(),'H'),f.slot);
    CHECK(f.hud_valid&&f.hud_draws==7&&!f.hud_markers&&!f.hud_markers_valid&&!s5ref().slot[f.slot].hudMarkerRt);src->release(f);
    step3Lines();
}
static void step5_hud_markers() {
    markerSetup(true);auto* src=acz::stereo::frame_source();
    const auto& mg=gameRt();const auto gameBefore=gameRt().object.refs;
    {auto& s5=s5ref();
     CHECK(s5.hudReady);if(!s5.hudMarkersReady)std::fprintf(stderr,"markers arm: %s\n",s5.hudMarkersArmWhy?s5.hudMarkersArmWhy:"none");
     CHECK(s5.hudMarkersReady&&!s5.hudMarkersArmWhy);
     if(s5.hudMarkersCharged!=markerCharge(mg.width,mg.height))std::fprintf(stderr,"charged %llu vs %llu (%llux%u)\n",(unsigned long long)s5.hudMarkersCharged,(unsigned long long)markerCharge(mg.width,mg.height),(unsigned long long)mg.width,mg.height);
     CHECK(s5.hudMarkersCharged==markerCharge(mg.width,mg.height));
     for(const auto& sl:s5.slot){CHECK(sl.hudMarkerRt&&sl.hudMarkerRtv.ptr&&sl.hudMarkerRt!=sl.hudRt&&sl.hudMarkerRtv.ptr!=sl.hudRtv.ptr);
         auto& r=res(sl.hudMarkerRt);CHECK(r.format==shape3.rtv&&r.width==mg.width&&r.height==mg.height&&r.state==D3D12_RESOURCE_STATE_RENDER_TARGET&&(r.flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));}}
    struct Case {const char* seq;const char* want;unsigned boxes,labels;};
    const std::vector<Case> cases{
        {"IIBLII",  "HHMMHH",  1,1},   // markers amid instruments
        {"BLBLBL",  "MMMMMM",  3,3},   // markers only: nothing in the HUD target
        {"BBLIL",   "MMMHH",   2,1},   // the text draw after an instrument is an instrument
        {"LBLJ",    "HMMH",    1,1},
        {"IIII",    "HHHH",    0,0},   // no markers at all
        {"IBXJ",    "HMHH",    1,0},   // a non-indexed text draw is no label
        {"BJLBL",   "MMHMH",   2,1},   // documented heuristic: the first follower defines the text route
    };
    uint64_t id=0;unsigned markerFrames=0,markerAbsent=0,hudValid=0,hudInvalid=0;uint64_t boxSum=0,labelSum=0;
    for(const auto& c:cases) {
        const size_t b=markerFrame(c.seq);++id;
        acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);
        checkSplitTail(b,c.want,f.slot);
        const std::string want=c.want;
        const unsigned h=unsigned(std::count(want.begin(),want.end(),'H')),m=unsigned(std::count(want.begin(),want.end(),'M'));
        const auto& sl=s5ref().slot[f.slot];
        if(!(f.hud_draws==h&&f.hud_refused==0&&f.hud_valid==(h>0)&&(f.hud!=nullptr)==(h>0)&&f.hud_marker_draws==m&&f.hud_marker_refused==0&&
             f.hud_markers_valid==(m>0)&&(f.hud_markers!=nullptr)==(m>0))){
            std::fprintf(stderr,"%s: hud %u/%u valid %d markers %u/%u valid %d\n",c.seq,f.hud_draws,h,int(f.hud_valid),f.hud_marker_draws,m,int(f.hud_markers_valid));CHECK(false);}
        if(m){CHECK(f.hud_markers==sl.hudMarkerRt&&f.hud_markers!=f.hud&&f.hud_markers!=f.world&&f.hud_markers!=f.eye[0]&&f.hud_markers!=f.eye[1]&&
            res(f.hud_markers).state==D3D12_RESOURCE_STATE_RENDER_TARGET&&hudCovered(f.hud_markers)>100);++markerFrames;boxSum+=c.boxes;labelSum+=c.labels;}
        else ++markerAbsent;
        if(h)++hudValid;else ++hudInvalid;
        if(h)CHECK(f.hud==sl.hudRt&&hudCovered(f.hud)>100&&res(f.hud).state==D3D12_RESOURCE_STATE_RENDER_TARGET);
        CHECK(f.members==6&&f.world_valid&&gameRt().object.refs==gameBefore&&res(sl.hudMarkerRt).object.refs==1); // private targets never AddRef'd
        src->release(f);
    }
    // Slot reuse (7 frames over 3 slots): stale content in either target is cleared before the next frame's first draw into it.
    for(const auto& sl:s5ref().slot){std::fill(res(sl.hudRt).big.begin(),res(sl.hudRt).big.end(),uint8_t(0x77));
        std::fill(res(sl.hudMarkerRt).big.begin(),res(sl.hudMarkerRt).big.end(),uint8_t(0x66));}
    for(unsigned k=0;k<4;++k) {
        const size_t b=markerFrame("IBL");++id;
        acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id&&f.hud_valid&&f.hud_markers_valid);
        checkSplitTail(b,"HMM",f.slot);
        {const auto& x=res(f.hud).big;CHECK(std::find(x.begin(),x.end(),uint8_t(0x77))==x.end());}
        {const auto& x=res(f.hud_markers).big;CHECK(std::find(x.begin(),x.end(),uint8_t(0x66))==x.end());}
        ++markerFrames;++hudValid;boxSum+=1;labelSum+=1;src->release(f);
    }
    {const auto& s5=s5ref();
     CHECK(s5.hudMarkerValidFrames==markerFrames&&s5.hudMarkerAbsentFrames==markerAbsent&&s5.hudMarkerInvalidFrames==0&&s5.hudMarkerBoxSum==boxSum&&
           s5.hudMarkerLabelSum==labelSum&&s5.hudMarkerDrawSum==boxSum+labelSum&&s5.hudMarkerRefusedSum==0&&s5.hudValidFrames==hudValid&&s5.hudInvalidFrames==hudInvalid&&hudInvalid==1/*the all-marker frame has no HUD draw*/);}
    ticks(60);const auto text=hudLog();
    const std::string charged=std::to_string(markerCharge(mg.width,mg.height));
    for(const std::string& k:{std::string("|gpu-copy-step1;event=hud-markers-ring;outcome=ready;reason=none;slots=3;format=")+std::to_string(unsigned(shape3.rtv))+";size="+std::to_string(mg.width)+"x"+std::to_string(mg.height)+";charged="+charged+
            ";caps=slot:33554432,total:134217728;ds=shared-with-hud;per-draw=3(OMSet-marker,Draw,OMSet-restore);clear=once-per-frame(0,0,0,0);classify=depth-greater->box,next-text-draw->label;split=exclusive;fallback=hud-unchanged",
            std::string("|gpu-copy-step1;event=hud-markers-frame;outcome=first-valid;frame=1;marker-draws=2(boxes:1,labels:1);marker-refused=0(state:0,descriptor:0,format:0,cap:0);first-refusal=none;instrument-draws=4;instrument-refused=0;markers-valid=1;premultiplied=1"),
            std::string("event=continuous-hud-markers;ready=1;frames=valid:")+std::to_string(markerFrames)+",invalid:0,absent:"+std::to_string(markerAbsent)+";draws="+std::to_string(boxSum+labelSum)+"(boxes:"+
            std::to_string(boxSum)+",labels:"+std::to_string(labelSum)+");avg-boxes="})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k.c_str());CHECK(false);}
    CHECK(text.find("refused=0(state:0,descriptor:0,format:0,cap:0);last-refusal=none;arm=none")!=std::string::npos);
    for(const auto& l:w->lines)CHECK(l.find("marker")==std::string::npos&&l.find("hud")==std::string::npos); // dedicated log only
    {std::istringstream is(text);std::string line;while(std::getline(is,line))CHECK(line.size()<448);}
    step3Lines();
}
// TACTICAL (tac:1): with the producer flags raised the frame carries its facts (StereoFrame::tac: marker box/label draws seen, objects, wedge) and the
// replay stream is EXACTLY the key-off stream whenever a marker cannot be matched to a replayed world draw (here: no world draws in the frame, and
// the fixture's sprite vertices are not a pinned mapped UPLOAD buffer). Flags back to 0: no facts (nullptr), same stream.
static void step5_tac() {
    markerSetup(true);auto* src=acz::stereo::frame_source();
    acz::tac::set_producer_flags(acz::tac::kProducerOn|acz::tac::kProducerSuppress);
    uint64_t id=0;
    for(const char* seq:{"IBLI","BLBL","IIII"}) {
        const std::string s=seq;
        const size_t b=markerFrame(s);++id;
        acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);
        std::string want;for(char c:s)want+=(c=='B'||c=='L')?'M':'H';
        if(s=="BLBL")want="MMMM";
        checkSplitTail(b,want,f.slot);
        CHECK(f.tac&&f.tac==&s5ref().tacSlot[f.slot]&&f.tac->valid);
        const unsigned boxes=unsigned(std::count(s.begin(),s.end(),'B')),labels=unsigned(std::count(s.begin(),s.end(),'L'));
        CHECK(f.tac->box_draws==boxes&&f.tac->label_draws==labels&&f.tac->nobj==0&&!f.tac->a_valid&&f.tac->wedge==0);
        CHECK(f.tac->boxes_suppressed==0&&f.tac->labels_suppressed==0);
        // The fixture's sprite buffer IS a pinned mapped UPLOAD buffer (read without a fault), but its vertices are not GS screen coordinates
        // (outside 2048 +- 1024): no marker cluster, nothing to match, nothing suppressed.
        CHECK(f.tac->box_unreadable==0&&f.tac->label_unreadable==0&&f.tac->nmark==0&&f.tac->mark_dropped==0);
        for(unsigned k=0;k<f.tac->nmark;++k)CHECK(f.tac->mark[k].obj==0&&!f.tac->mark[k].suppressed);
        src->release(f);
    }
    acz::tac::set_producer_flags(0);
    {const size_t b=markerFrame("IBLI");++id;
     acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);
     checkSplitTail(b,"HMMH",f.slot);CHECK(!f.tac);src->release(f);}
    ticks(60);const auto text=hudLog();
    CHECK(text.find("-tac;frames=3;avg-objects=0.00;avg-markers=")!=std::string::npos);
    CHECK(text.find("box-draws=3;label-draws=3;suppressed=boxes:0,labels:0;")!=std::string::npos);
    step3Lines();
}
static void step5_hud_markers_refused() {
    // A refused draw is charged to the layer it belongs to: marker refusals invalidate only the marker layer,
    // instrument refusals only the HUD; the game's own draws are untouched.
    markerSetup(true);auto* src=acz::stereo::frame_source();
    uint64_t id=0;
    auto frame=[&](const char* seq,const char* want,unsigned hudDraws,unsigned hudRefused,unsigned mDraws,unsigned mRefused){
        const size_t b=markerFrame(seq);++id;
        acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);
        checkSplitTail(b,want,f.slot);
        const bool hv=hudDraws>0&&hudRefused==0,mv=mDraws>0&&mRefused==0;
        if(!(f.hud_draws==hudDraws&&f.hud_refused==hudRefused&&f.hud_valid==hv&&(f.hud!=nullptr)==hv&&f.hud_marker_draws==mDraws&&f.hud_marker_refused==mRefused&&
             f.hud_markers_valid==mv&&(f.hud_markers!=nullptr)==mv)){
            std::fprintf(stderr,"%s: hud %u/%u (%u) valid %d markers %u (%u) valid %d\n",seq,f.hud_draws,hudDraws,f.hud_refused,int(f.hud_valid),f.hud_marker_draws,f.hud_marker_refused,int(f.hud_markers_valid));CHECK(false);}
        src->release(f);};
    frame("IBLZL",  "HMMM",  1,0, 3,1);   // a refused box (and its labels stay markers): HUD valid, marker layer invalid
    frame("IQBL",   "HMM",   1,1, 2,0);   // a refused instrument: HUD invalid, markers valid
    frame("BL",     "MM",    0,0, 2,0);   // markers only: no HUD draws -> no HUD layer, markers valid
    frame("II",     "HH",    2,0, 0,0);   // no markers: HUD only
    frame("ZQ",     "",      0,1, 0,1);   // everything refused: both invalid
    // Frame accounting by category (state) and the first-refusal events.
    {const auto& s5=s5ref();
     CHECK(s5.hudMarkerValidFrames==2&&s5.hudMarkerInvalidFrames==2&&s5.hudMarkerAbsentFrames==1&&s5.hudMarkerRefusedSum==2&&s5.hudMarkerReason[0]==2&&s5.hudMarkerReason[1]==0&&
           s5.hudValidFrames==2&&s5.hudInvalidFrames==3&&s5.hudRefusedSum==2&&s5.hudReason[0]==2);}
    // Cap: draws are capped over BOTH targets (160): 80 instruments + 81 markers -> the 161st (a marker) is refused.
    {std::string seq;for(int i=0;i<80;++i)seq+="IB";seq+="B";
     const size_t b=markerFrame(seq);++id;(void)b;
     acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.hud_draws==80&&f.hud_marker_draws==80&&f.hud_marker_refused==1&&f.hud_valid&&!f.hud_markers_valid&&!f.hud_markers);src->release(f);
     const auto& s5=s5ref();CHECK(s5.hudMarkerReason[3]==1&&s5.hudCapFrames==1);}
    ticks(60);const auto text=hudLog();
    for(const char* k:{"event=hud-markers-frame;outcome=first-refused;frame=0;marker-draws=3(boxes:1,labels:2);marker-refused=1(state:1,descriptor:0,format:0,cap:0);first-refusal=stencil-test-not-always;instrument-draws=1;instrument-refused=0;markers-valid=0;",
            "event=hud-markers-frame;outcome=first-valid;frame=2;marker-draws=2(boxes:1,labels:1);",
            "frames=valid:2,invalid:3,absent:1;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    step3Lines();
}
static void step5_hud_markers_region() {
    // A later cockpit member restarts the region: both targets and the shared depth-stencil are cleared again, counts forgotten.
    markerSetup(true);auto* src=acz::stereo::frame_source();
    size_t b=hudFrame(0,[]{hudBindScene();markerDraws("IBL");hudLateMember();hudBindScene();markerDraws("IIBL");hudOutput();});
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.frame_id==1);
    checkSplitTail(b,"HMMHHMM",f.slot,2);
    CHECK(f.hud_valid&&f.hud_draws==2&&f.hud_markers_valid&&f.hud_marker_draws==2&&f.hud_refused==0&&f.hud_marker_refused==0);src->release(f);
    CHECK(s5ref().hudResets==1&&s5ref().hudMarkerBoxSum==1&&s5ref().hudMarkerLabelSum==1);
    // (The replays before the late member stay in the command stream; the frame's counts restart: "HMM" + "HHMM" recorded, 2+2 published.)
}
static void step5_hud_markers_create_refused() {
    // The second targets cannot be created (second slot fails): refused cleanly, the HUD runs exactly as without the key.
    g_failResourceAt=27; // 4 + 12 + 1 + 3 world + 6 HUD = 26 before; marker slot 0 = #26, slot 1 = #27 fails
    markerSetup(true);g_failResourceAt=UINT_MAX;auto* src=acz::stereo::frame_source();
    {auto& s5=s5ref();
     CHECK(s5.hudReady&&!s5.hudMarkersReady&&s5.hudMarkersArmWhy&&std::string(s5.hudMarkersArmWhy)=="hud-markers-rt-create-failed"&&s5.hudMarkersCharged==0);
     CHECK(w->resourcesCreated==27&&w->heapsCreated==14+6+1);}
    const std::string seq="IBLIBL";
    const size_t b=markerFrame(seq);
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f));
    checkSplitTail(b,std::string(seq.size(),'H'),f.slot); // marker-shaped draws stay in the HUD target
    CHECK(f.hud_valid&&f.hud_draws==6&&!f.hud_markers&&!f.hud_markers_valid&&f.hud_marker_draws==0);src->release(f);
    {const auto& s5=s5ref();CHECK(s5.hudValidFrames==1&&s5.hudMarkerValidFrames==0&&s5.hudMarkerAbsentFrames==0&&s5.hudMarkerInvalidFrames==0);}
    ticks(60);const auto text=hudLog();
    for(const char* k:{"|gpu-copy-step1;event=hud-markers-ring;outcome=refused;reason=hud-markers-rt-create-failed;slots=3;","charged=0;","fallback=hud-unchanged",
            "event=continuous-hud-markers;ready=0;frames=valid:0,invalid:0,absent:0;","arm=hud-markers-rt-create-failed"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    step3Lines();
}


// ------------------------------------------------------------- J-effects-layer (gpu_copy_step5_effects)
// Draw scripts over the scene target after the first member. Psos: G good effect (blended, depth GREATER, no writes), S stencil EQUAL (refused),
// C constant blend factor (refused), W depth write (refused), L depth LESS (refused), O opaque + no depth (ignored, not an effect).
static Object effPsoObj[8];
static ID3D12PipelineState* effPso(char c){const char* k="GSCWLOHX";return reinterpret_cast<ID3D12PipelineState*>(&effPsoObj[std::strchr(k,c)-k]);}
// J-effects-smoke-fix: H = the HUD sprite class (ps d34ee153, depth ALWAYS, blended), X = an unknown pixel shader in the effects state (GREATER, blended),
// G..O = the smoke pixel shader d2464ed2. effRootMode: 0 = a layout without UAVs, 1 = a layout with a descriptor-table UAV range, 2 = a root UAV parameter, 3 = no layout.
static int effRootMode=0;
static void effInjectRoot() {
    namespace m=acz::metadata;
    auto& s=acz::state();std::lock_guard lock(s.mutex);auto& ls=s.states.at(listAlias(0));
    if(effRootMode==3){ls.bindings.root.reset();return;}
    m::RootLayout x;
    m::RootParameter srv;srv.kind=m::ParameterKind::table;m::DescriptorRange r;r.kind=m::BindingKind::srv;srv.ranges.push_back(r);x.parameters.push_back(srv);
    if(effRootMode==1){m::RootParameter t;t.kind=m::ParameterKind::table;m::DescriptorRange u;u.kind=m::BindingKind::uav;t.ranges.push_back(u);x.parameters.push_back(t);}
    if(effRootMode==2){m::RootParameter u;u.kind=m::ParameterKind::uav;x.parameters.push_back(u);}
    ls.bindings.root=m::RootBindingMetadata::from_adapter(x);
}
static void effectsSetup(bool key) {
    acz::g_effectsTest=key;hudSetup(true);
    for(auto& o:effPsoObj){o.table=w->psoTable.data();o.kind=k_pso;}
    using namespace gpu_copy::step3;
    hudPipeline(effPso('G'),[](auto& o){o.depth_func=cmp::greater;});
    hudPipeline(effPso('S'),[](auto& o){o.depth_func=cmp::greater;o.front_func=cmp::equal;});
    hudPipeline(effPso('C'),[](auto& o){o.depth_func=cmp::greater;o.blend_constant=true;});
    hudPipeline(effPso('W'),[](auto& o){o.depth_func=cmp::greater;o.depth_write=true;});
    hudPipeline(effPso('L'),[](auto& o){o.depth_func=cmp::less;});
    hudPipeline(effPso('O'),[](auto& o){o.depth_enable=false;o.blend0=false;o.stencil=false;});
    hudPipeline(effPso('H'),[](auto&){});
    hudPipeline(effPso('X'),[](auto& o){o.depth_func=cmp::greater;});
    effRootMode=0;
    auto& s=acz::state();std::lock_guard lock(s.mutex);
    for(char c:{'G','S','C','W','L','O','X'})std::strcpy(s.pipelines.at(effPso(c)).hudPs.data(),"d2464ed2c0ffee00d2464ed2c0ffee00");
    std::strcpy(s.pipelines.at(effPso('X')).hudPs.data(),"12345678c0ffee0012345678c0ffee00");
    std::strcpy(s.pipelines.at(effPso('H')).hudPs.data(),"d34ee153c0ffee00d34ee153c0ffee00");
}
static void effDraws(const std::string& seq) {
    auto* l=listAlias(0);effInjectRoot();
    for(char c:seq) {
        l->SetPipelineState(effPso(c));bindT10(srtOff);
        const auto before=allocations;traceAllocations=true;
        l->DrawIndexedInstanced(6,1,0,0,0);
        traceAllocations=false;CHECK(allocations==before); // no heap allocation on the (classifying, speculative) Draw path
    }
}
struct EffCounts {unsigned predOn=0,predOff=0,clearsEff=0,clearsDs=0,replays=0;};
static EffCounts effCounts(size_t begin,unsigned slot) {
    EffCounts c;const auto& L=w->lists[0];const auto& sl=s5ref().slot[slot];
    for(size_t i=begin;i<L.ops.size();++i) {
        const Op& o=L.ops[i];
        if(o.t==Op::pred){if(o.res)++c.predOn;else ++c.predOff;continue;}
        if(o.t==Op::clear_rtv&&o.rtv[0]==sl.effRtv.ptr){++c.clearsEff;continue;}
        if(o.t==Op::clear_dsv&&o.dsv==sl.hudDsv.ptr){++c.clearsDs;continue;}
        if(o.t==Op::om&&o.rtv[0]==sl.effRtv.ptr) {
            // pred(on) precedes it; OM(eff) -> Draw(eff) -> pred(off) -> OM(restore) -> the game's Draw (same arguments, NOT predicated)
            CHECK(o.count==1&&i>begin&&L.ops[i-1].t==Op::pred&&L.ops[i-1].res&&i+4<L.ops.size());
            const Op& d=L.ops[i+1];const Op& po=L.ops[i+2];const Op& r=L.ops[i+3];
            size_t gi=i+4;while(gi<L.ops.size()&&!(L.ops[gi].t==Op::draw&&originOf(L.ops[gi].boundRtv)==gameRtvHandle))++gi; // (the HUD replay of the same draw may sit between)
            CHECK(gi<L.ops.size());const Op& g=L.ops[gi];
            CHECK(d.t==Op::draw&&d.boundRtv==sl.effRtv.ptr&&po.t==Op::pred&&!po.res&&r.t==Op::om&&originOf(r.rtv[0])==gameRtvHandle);
            CHECK(g.t==Op::draw&&originOf(g.boundRtv)==gameRtvHandle&&d.indexed==g.indexed&&d.dc==g.dc&&d.boundT10==g.boundT10);
            ++c.replays;
        }
    }
    return c;
}
static void step5_effects_rules() {
    using namespace gpu_copy::step5;using namespace gpu_copy::step3;
    Shape s;s.tracking_complete=true;s.root_route=0x10;s.pso.known=true;s.pso.rt_count=1;s.pso.rtv[0]=10;s.pso.dsv=20;s.pso.samples=1;
    s.om_known=s.om_set=true;s.om_count=1;s.om_desc_proved=true;s.game_rt_status="resolved";s.game_rt=0x5000;s.game_rt_format=10;
    s.game_rt_width=2560;s.game_rt_height=1440;s.pso.depth_enable=true;s.pso.depth_func=cmp::greater;s.pso.blend0=true;
    s.pso.stencil=true;s.pso.front_func=s.pso.back_func=cmp::always;s.pso.stencil_write=255;s.pso.stencil_ops_keep=true;s.pso.write_mask0=15;
    HudTarget t;t.format=10;t.ds_format=20;t.width=2560;t.height=1440;t.scene=0x5000;
    struct R {int kind;std::string why;int cat;};
    auto run=[&](Shape x,int phase=effects_open,unsigned draws=0){const auto v=effects_decide(x,t,phase,draws);return R{v.kind,v.why?v.why:"",v.category};};
    auto is=[&](const R& r,int kind,const char* why,int cat=hud_state){return r.kind==kind&&r.why==why&&(kind!=effects_refused||r.cat==cat);};
    // ---- classification table
    CHECK(is(run(s),effects_ok,""));
    CHECK(effects_decide(s,t,effects_open,0).depth_tested);
    // phase gating: nothing before the first member or after the scene target left RENDER_TARGET, whatever the draw is
    CHECK(is(run(s,effects_before_first_member),effects_ignored,"phase-before-first-member"));
    CHECK(is(run(s,effects_scene_closed),effects_ignored,"phase-scene-closed"));
    CHECK(is(run(s,7),effects_ignored,"phase-scene-closed"));
    // not an effect: neither blended nor depth-tested
    {auto x=s;x.pso.blend0=false;x.pso.depth_enable=false;x.pso.stencil=false;CHECK(is(run(x),effects_ignored,"not-effect-like"));}
    {auto x=s;x.pso.blend0=false;CHECK(is(run(x),effects_ok,""));}                          // depth-tested only: an effect candidate
    {auto x=s;x.pso.depth_enable=false;x.pso.stencil=false;CHECK(is(run(x),effects_ok,""));} // blended only
    // J-effects-content: a draw that writes no colour channel is never an effect (the live full-screen alpha-only pass: mask 0x8, blend off, depth ALWAYS) ...
    {auto x=s;x.pso.write_mask0=8;x.pso.blend0=false;x.pso.depth_func=cmp::always;CHECK(is(run(x),effects_ignored,"no-colour-write"));}
    {auto x=s;x.pso.write_mask0=8;CHECK(is(run(x),effects_ignored,"no-colour-write"));}   // even blended / depth-tested: it cannot show colour
    {auto x=s;x.pso.write_mask0=0;CHECK(is(run(x),effects_ignored,"no-colour-write"));}
    for(uint32_t m:{1u,2u,4u,7u,15u}){auto x=s;x.pso.write_mask0=m;CHECK(is(run(x),effects_ok,""));}
    // ... "depth-tested" means a real test: an opaque ALWAYS/NEVER draw is an always-pass draw, not an effect; blended ALWAYS and GREATER(_EQUAL) stay candidates.
    for(uint32_t f:{cmp::always,cmp::never}){auto x=s;x.pso.blend0=false;x.pso.depth_func=f;CHECK(is(run(x),effects_ignored,"not-effect-like"));}
    for(uint32_t f:{cmp::greater,cmp::greater_equal}){auto x=s;x.pso.blend0=false;x.pso.depth_func=f;CHECK(is(run(x),effects_ok,""));}
    {auto x=s;x.pso.depth_func=cmp::always;x.pso.blend0=true;CHECK(is(run(x),effects_ok,""));}
    {auto x=s;x.pso.known=false;CHECK(is(run(x),effects_refused,"pso-output-unknown"));} // an unknown PSO is still refused (never silently ignored)
    // everything hud_decide refuses is refused (same reason/category), stencil EQUAL included
    {auto x=s;x.pso.front_func=cmp::equal;CHECK(is(run(x),effects_refused,"stencil-test-not-always"));}
    {auto x=s;x.pso.back_func=cmp::equal;CHECK(is(run(x),effects_refused,"stencil-test-not-always"));}
    {auto x=s;x.tracking_complete=false;CHECK(is(run(x),effects_refused,"binding-tracking-incomplete",hud_descriptor));}
    {auto x=s;x.om_desc_proved=false;CHECK(is(run(x),effects_refused,"om-descriptor-unproved",hud_descriptor));}
    {auto x=s;x.game_rt_status="unresolved:rtv-not-tracked";CHECK(is(run(x),effects_refused,"game-rt-unresolved"));}
    {auto x=s;x.game_rt=0x6000;CHECK(is(run(x),effects_refused,"not-scene-target"));}
    {auto x=s;x.pso.rtv[0]=28;CHECK(is(run(x),effects_refused,"rt-format-mismatch",hud_format));}
    for(uint32_t f:{cmp::less,cmp::less_equal,cmp::equal,cmp::not_equal}){auto x=s;x.pso.depth_func=f;CHECK(is(run(x),effects_refused,"depth-test-not-replayable"));}
    for(uint32_t f:{cmp::always,cmp::never}){auto x=s;x.pso.depth_func=f;const auto v=effects_decide(x,t,effects_open,0);CHECK(v.kind==effects_ok&&!v.depth_tested);}
    // constant blend factor / alpha factor: refused only while blending is on; the factor values map is pinned by effects_blend_constant
    {auto x=s;x.pso.blend_constant=true;CHECK(is(run(x),effects_refused,"blend-constant-factor"));}
    {auto x=s;x.pso.blend_constant=true;x.pso.blend0=false;CHECK(is(run(x),effects_ok,""));} // blending off: the factors are not read
    for(uint32_t f:{14u,15u,20u,21u})CHECK(effects_blend_constant(f));
    for(uint32_t f:{0u,1u,2u,5u,6u,7u,16u,19u})CHECK(!effects_blend_constant(f));
    // shared depth-stencil safety: depth writers and stencil writers are refused
    {auto x=s;x.pso.depth_write=true;CHECK(is(run(x),effects_refused,"depth-write"));}
    {auto x=s;x.pso.depth_enable=false;x.pso.depth_write=true;x.pso.stencil=false;CHECK(is(run(x),effects_ok,""));} // a disabled depth test never writes
    {auto x=s;x.pso.stencil_ops_keep=false;CHECK(is(run(x),effects_refused,"stencil-write"));}
    {auto x=s;x.pso.stencil_ops_keep=false;x.pso.stencil_write=0;CHECK(is(run(x),effects_ok,""));} // write mask 0: nothing is written
    // cap: the 65th speculative replay of a frame is refused (category cap); the 64th is fine
    CHECK(is(run(s,effects_open,effects_draw_cap-1),effects_ok,""));
    CHECK(is(run(s,effects_open,effects_draw_cap),effects_refused,"effects-cap",hud_cap));
    CHECK(effects_draw_cap==256&&effects_epoch_cap==32&&effects_per_draw==5&&effects_draw_cap*effects_per_draw+2<=effects_command_cap);
    // ---- epoch bookkeeping: candidates between two members commit with the NEXT member; the open one at the close is the tail
    {
        EffectsEpochs e;
        CHECK(e.commit()==-1&&e.commits==0);                     // member with nothing since the previous one
        CHECK(e.open_draw()==0&&e.open_draw()==0);                // two candidates, epoch 0
        e.note_refused(hud_state,true);
        CHECK(e.commit()==0&&e.draws==2&&e.refused==1&&e.constBlend==1&&e.reason[hud_state]==1&&e.commits==1&&e.cur==-1);
        CHECK(e.open_draw()==1);                                  // epoch 1
        CHECK(e.commit()==1&&e.draws==3);
        e.note_refused(hud_format,false);                         // a refusal alone: counted, no predicate to set
        CHECK(e.commit()==-1&&e.refused==2&&e.reason[hud_format]==1&&e.draws==3&&e.commits==3);
        CHECK(e.open_draw()==2&&e.open_draw()==2);                // the tail
        e.note_refused(hud_cap,false);
        e.finish();
        CHECK(e.tailDraws==2&&e.tailRefused==1&&e.draws==3&&e.refused==2&&e.speculative==5&&e.used==3); // the tail is never added to the committed counts
        EffectsEpochs f;                                          // predicate slots run out at 32 epochs
        for(unsigned i=0;i<effects_epoch_cap;++i){CHECK(f.open_draw()==int(i));CHECK(f.commit()==int(i));}
        CHECK(f.open_draw()==-1&&f.used==effects_epoch_cap&&f.draws==effects_epoch_cap);
    }
    CHECK(!effects_frame_valid(false,3)&&!effects_frame_valid(true,0)&&effects_frame_valid(true,1)&&effects_frame_valid(true,5));
    // ---- memory: 3 slots of the 2560x1440 FP16 target + one 64 KiB predicate buffer; refused above the caps/unsupported formats
    uint64_t charged=7;
    CHECK(!effects_budget(10,2560,1440,slots,charged)&&charged==3*(29491200ull+65536)+65536);
    CHECK(std::string(effects_budget(10,4096,2160,slots,charged))=="effects-over-slot-cap"&&charged==0);
    CHECK(std::string(effects_budget(2,2560,1440,slots,charged))=="effects-format-unsupported"&&charged==0);
    CHECK(std::string(effects_budget(10,2560,1440,5,charged))=="effects-over-total-cap"&&charged==0);
    CHECK(std::string(effects_budget(10,0,1440,slots,charged))=="effects-size-unsupported"&&std::string(effects_budget(10,20000,10,slots,charged))=="effects-size-unsupported");
    CHECK(effects_slot_cap==rt_byte_cap&&3*(29491200ull+65536)+65536<=effects_total_cap);
    // ---- config: default OFF, boolean forms, duplicates/out-of-range rejected; neither key implies the other or the HUD
    using namespace acz::diagnostics;
    CHECK(!Config{}.gpu_copy_step5_effects()&&!Config{}.xr_effects_layer()&&!parse_config("").config.gpu_copy_step5_effects());
    for(const auto* v:{"true","1"}){auto c=parse_config(std::string("gpu_copy_step5_effects=")+v);CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_effects()&&!c.config.gpu_copy_step5_hud()&&!c.config.xr_effects_layer());}
    for(const auto* v:{"false","0"}){auto c=parse_config(std::string("gpu_copy_step5_effects=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.gpu_copy_step5_effects());}
    CHECK(parse_config("gpu_copy_step5_effects=on").issue==ConfigIssue::out_of_range&&parse_config("xr_effects_layer=2").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_effects=1\ngpu_copy_step5_effects=0").issue==ConfigIssue::duplicate);
    // ---- J-test8-fixes keys: default OFF / 0, forms, range, duplicates
    CHECK(!Config{}.xr_world_objects_depth_fix()&&!Config{}.xr_png_once()&&Config{}.xr_debug_view()==0);
    CHECK(parse_config("xr_world_objects_depth_fix=true").config.xr_world_objects_depth_fix()&&parse_config("xr_png_once=1").config.xr_png_once());
    CHECK(!parse_config("xr_world_objects_depth_fix=0").config.xr_world_objects_depth_fix()&&!parse_config("xr_png_once=false").config.xr_png_once());
    for(char v='0';v<='4';++v){auto c=parse_config(std::string("xr_debug_view=")+v);CHECK(c.issue==ConfigIssue::none&&c.config.xr_debug_view()==unsigned(v-'0'));}
    CHECK(parse_config("xr_debug_view=5").issue==ConfigIssue::out_of_range&&parse_config("xr_debug_view=10").issue==ConfigIssue::out_of_range&&
          parse_config("xr_debug_view=x").issue==ConfigIssue::out_of_range&&parse_config("xr_png_once=2").issue==ConfigIssue::out_of_range&&
          parse_config("xr_world_objects_depth_fix=on").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("xr_png_once=1\nxr_png_once=1").issue==ConfigIssue::duplicate&&parse_config("xr_debug_view=1\nxr_debug_view=2").issue==ConfigIssue::duplicate);
}
static void step5_effects_off() {
    // Key OFF (HUD on): scene draws between members are what they were; no effects resources, fields, predicate, or lines.
    effectsSetup(false);auto* src=acz::stereo::frame_source();
    {auto& s5=s5ref();CHECK(s5.hudReady&&!s5.effReady&&std::string(s5.effArmWhy)=="not-configured"&&!s5.effPred&&!s5.effPredPtr);for(const auto& sl:s5.slot)CHECK(!sl.effRt&&!sl.effRtvHeap);}
    const size_t begin=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();effDraws("G");hudOutput();});
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f));
    CHECK(!f.effects&&!f.effects_valid&&f.effects_draws==0&&f.effects_refused==0&&f.effects_tail==0&&f.hud_valid&&f.hud_draws==1);src->release(f);
    for(size_t i=begin;i<w->lists[0].ops.size();++i)CHECK(w->lists[0].ops[i].t!=Op::pred);
    CHECK(w->predDrawn==0&&w->predSkipped==0);
    {const auto& s5=s5ref();CHECK(s5.effValidFrames==0&&s5.effInvalidFrames==0&&s5.effAbsentFrames==0&&s5.effUnarmedFrames==0&&s5.effSpeculative==0);}
    ticks(60);const auto text=hudLog();
    CHECK(text.find("effects")==std::string::npos);
    for(const auto& l:w->lines)CHECK(l.find("effects")==std::string::npos);
    // The key raised AFTER arming (no effects resources exist): the command stream is identical op for op (the key never starts anything by itself).
    const size_t b1=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();effDraws("G");hudOutput();});
    {acz::stereo::StereoFrame g;CHECK(src->try_acquire_latest(1,g));src->release(g);} // free the slot: the next frame records into the same slot (same descriptors/addresses)
    std::vector<Op> off(w->lists[0].ops.begin()+std::ptrdiff_t(b1),w->lists[0].ops.end());
    acz::g_effectsTest=true;const size_t b2=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();effDraws("G");hudOutput();});acz::g_effectsTest=false;
    std::vector<Op> on(w->lists[0].ops.begin()+std::ptrdiff_t(b2),w->lists[0].ops.end());
    CHECK(on.size()==off.size());
    // The bind-time restore descriptors rotate per recording (a ring): compare the handles through their origin.
    auto loose=[](Op o){o.rtv[0]=originOf(o.rtv[0]);o.dsv=originOf(o.dsv);o.boundRtv=originOf(o.boundRtv);o.boundDsv=originOf(o.boundDsv);o.consumedRtv=nullptr;return o;};
    for(size_t i=0;i<off.size()&&i<on.size();++i){if(!hudSameOp(loose(off[i]),loose(on[i])))std::fprintf(stderr,"op %zu type %d rtv %zx/%zx va %llx/%llx bound %zx/%zx t10 %llx/%llx\n",i,int(off[i].t),size_t(off[i].rtv[0]),size_t(on[i].rtv[0]),(unsigned long long)off[i].va,(unsigned long long)on[i].va,size_t(off[i].boundRtv),size_t(on[i].boundRtv),(unsigned long long)off[i].boundT10,(unsigned long long)on[i].boundT10);CHECK(hudSameOp(loose(off[i]),loose(on[i])));}
    CHECK(w->predDrawn==0&&w->predSkipped==0);
    step3Lines();
}
static void step5_effects() {
    effectsSetup(true);auto* src=acz::stereo::frame_source();
    {auto& s5=s5ref();CHECK(s5.hudReady&&s5.effReady&&!s5.effArmWhy&&s5.effPred&&s5.effPredPtr);
     uint64_t charged=0;CHECK(!gpu_copy::step5::effects_budget(s5.hudFormat,s5.worldW,s5.worldH,gpu_copy::step5::slots,charged)&&s5.effCharged==charged);
     for(const auto& sl:s5.slot){CHECK(sl.effRt&&sl.effRtv.ptr&&sl.effRt!=sl.hudRt&&sl.effRt!=sl.world);
         auto& r=res(sl.effRt);CHECK(r.format==shape3.rtv&&r.width==gameRt().width&&r.height==gameRt().height&&r.state==D3D12_RESOURCE_STATE_RENDER_TARGET);}
     CHECK(res(s5.effPred).heap==D3D12_HEAP_TYPE_UPLOAD&&res(s5.effPred).initial==D3D12_RESOURCE_STATE_GENERIC_READ);}
    // (a) members, 2 effect draws (epoch A), a LATER member (commits A), 1 effect draw + 2 HUD-like draws (the tail, epoch B).
    const auto gameBefore=gameRt().object.refs;
    size_t b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();effDraws("G");hudDraws(2);hudOutput();});
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.frame_id==1);
    if(!(f.effects_valid&&f.effects&&f.effects_draws==2&&f.effects_refused==0&&f.effects_tail==3)){
        std::fprintf(stderr,"effects valid %d draws %u refused %u tail %u\n",int(f.effects_valid),f.effects_draws,f.effects_refused,f.effects_tail);CHECK(false);}
    {const auto& sl=s5ref().slot[f.slot];
     CHECK(f.effects==sl.effRt&&f.effects!=f.hud&&f.effects!=f.world&&f.effects!=f.eye[0]&&f.effects!=f.eye[1]&&res(f.effects).state==D3D12_RESOURCE_STATE_RENDER_TARGET);
     const auto c=effCounts(b,f.slot);
     CHECK(c.replays==5&&c.predOn==5&&c.predOff==5&&c.clearsEff==1&&c.clearsDs>=1);                // cleared ONCE per frame, every replay predicated and un-predicated
     CHECK(w->predDrawn==2&&w->predSkipped==3);                                                      // only the committed epoch executed; the tail stayed off
     CHECK(hudCovered(f.effects)>100);                                                               // the committed draws rasterised
     CHECK(f.hud_valid&&f.hud_draws==3&&f.hud_refused==0&&hudCovered(f.hud)>100);                   // the HUD is exactly what it was (only the draws after the last member)
     CHECK(gameRt().object.refs==gameBefore&&res(f.effects).object.refs==1);                         // never AddRef'd
     // predicate memory: epoch 0 committed (1), epoch 1 (tail) still 0, the rest 0
     const uint8_t* pr=res(s5ref().effPred).data()+uint64_t(f.slot)*gpu_copy::step5::effects_epoch_cap*8;
     uint64_t w0=0,w1=0;std::memcpy(&w0,pr,8);std::memcpy(&w1,pr+8,8);CHECK(w0==1&&w1==0);
     // ordering inside the recording: the effects clear precedes the first predicated replay
     size_t clearAt=SIZE_MAX,firstPred=SIZE_MAX;const auto& L=w->lists[0];
     for(size_t i=b;i<L.ops.size();++i){if(L.ops[i].t==Op::clear_rtv&&L.ops[i].rtv[0]==sl.effRtv.ptr&&clearAt==SIZE_MAX)clearAt=i;if(L.ops[i].t==Op::pred&&L.ops[i].res&&firstPred==SIZE_MAX)firstPred=i;}
     CHECK(clearAt<firstPred);}
    src->release(f);
    // (b) tail only: candidates after the LAST member are predicated off: no effects layer is published, the target holds nothing.
    b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudOutput();});
    CHECK(src->try_acquire_latest(1,f)&&f.frame_id==2&&!f.effects_valid&&!f.effects&&f.effects_draws==0&&f.effects_tail==2);
    CHECK(w->predDrawn==2&&w->predSkipped==5); // (cumulative: 2+0 drawn, 3+2 skipped)
    CHECK(hudCovered(s5ref().slot[f.slot].effRt)==0);src->release(f);
    // (c) no candidates at all: absent (the HUD-only frame of the earlier tests).
    hudFrame(0,[]{hudOutput();});CHECK(src->try_acquire_latest(2,f)&&f.frame_id==3&&!f.effects_valid&&f.effects_draws==0&&f.effects_tail==0&&!f.hud_valid);src->release(f);
    // (d) slot reuse (stale content is cleared before the first replay of the next frame into the slot's effects target)
    for(const auto& sl:s5ref().slot)std::fill(res(sl.effRt).big.begin(),res(sl.effRt).big.end(),uint8_t(0x55));
    for(unsigned k=0;k<4;++k) {
        const size_t bk=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
        CHECK(src->try_acquire_latest(3+k,f)&&f.effects_valid&&f.effects_draws==1&&f.effects_tail==0);
        {const auto& x=res(f.effects).big;CHECK(std::find(x.begin(),x.end(),uint8_t(0x55))==x.end());}
        (void)bk;src->release(f);
    }
    // (e) the effects target of an unrelated slot content is not touched by frames that never record a candidate (published invalid)
    {const auto& s5=s5ref();CHECK(s5.effValidFrames==5&&s5.effInvalidFrames==1&&s5.effAbsentFrames==1&&s5.effUnarmedFrames==0);
     CHECK(s5.effDrawSum==2+4&&s5.effTailSum==3+2&&s5.effRefusedSum==0&&s5.effConstBlend==0&&s5.effCommits==5);}
    // (f) log lines
    ticks(60);const auto text=hudLog();
    for(const char* k:{"|gpu-copy-step1;event=effects-ring;outcome=ready;reason=none;slots=3;","caps=slot:33554432,total:134217728,draws:256,epochs:32;ds=shared-with-hud(no-depth-stencil-writers);",
            "per-draw=5(SetPredication,OMSet-effects,Draw,SetPredication-null,OMSet-restore);","commit=later-cockpit-member;fallback=key-off",
            "event=effects-frame;outcome=first-valid;frame=1;draws=2;refused=0(state:0,descriptor:0,format:0,cap:0);const-blend=0;tail=3;speculative=5;epochs=2/32;commits=1;",
            "event=continuous-hud-effects;ready=1;frames=valid:5,tail-only:1,absent:1,unarmed:0;draws=6;","tail=5;","refused=0(state:0,descriptor:0,format:0,cap:0);const-blend=0;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    step3Lines();
}
static void step5_effects_refused() {
    effectsSetup(true);auto* src=acz::stereo::frame_source();
    acz::stereo::StereoFrame f;
    // (a) refusal categories inside a committed epoch: counted, the good draw still publishes the layer; constant blend counted apart.
    size_t b=hudFrame(0,[]{hudBindScene();effDraws("GSCWLO");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(0,f)&&f.frame_id==1);
    if(!(f.effects_valid&&f.effects_draws==1&&f.effects_refused==2&&f.effects_tail==0)){std::fprintf(stderr,"valid %d draws %u refused %u tail %u\n",int(f.effects_valid),f.effects_draws,f.effects_refused,f.effects_tail);CHECK(false);}
    {const auto c=effCounts(b,f.slot);CHECK(c.replays==1&&c.predOn==1&&c.predOff==1);CHECK(w->predDrawn==1&&w->predSkipped==0);}
    {const auto& s5=s5ref();CHECK(s5.effReason[0]==2&&s5.effReason[1]==0&&s5.effReason[2]==0&&s5.effReason[3]==0&&s5.effConstBlend==1&&s5.effIgnored>=1);
     CHECK(s5.effClassIgnored==2);                                   // W (depth write) and L (depth LESS) are not the effects state class: ignored, not refused
     CHECK(std::string(s5.effLastWhy)=="blend-constant-factor");}
    src->release(f);
    // (b) the refusal is charged to the epoch it is in: refused candidates of the TAIL epoch do not count (the HUD owns that region).
    hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();effDraws("SC");hudOutput();});
    CHECK(src->try_acquire_latest(1,f)&&f.effects_valid&&f.effects_draws==1&&f.effects_refused==0&&f.effects_tail==0);src->release(f);
    CHECK(s5ref().effRefusedSum==2&&s5ref().effConstBlend==1);
    // (c) per-frame cap (J-test8-fixes: 96; Q step174: 256): cap+4 candidates in one committed epoch -> cap replayed, 4 refused (cap), never an unbounded recording.
    b=hudFrame(0,[]{hudBindScene();effDraws(std::string(gpu_copy::step5::effects_draw_cap+4,'G'));hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(2,f)&&f.effects_valid&&f.effects_draws==gpu_copy::step5::effects_draw_cap&&f.effects_refused==4);
    {const auto c=effCounts(b,f.slot);CHECK(c.replays==gpu_copy::step5::effects_draw_cap&&c.predOn==gpu_copy::step5::effects_draw_cap&&c.predOff==gpu_copy::step5::effects_draw_cap&&c.clearsEff==1);}
    {const auto& s5=s5ref();CHECK(s5.effReason[3]==4);}src->release(f);
    // (d) 33 epochs: the 33rd has no predicate slot -> refused (cap), 32 committed.
    hudFrame(0,[]{hudBindScene();for(int i=0;i<33;++i){effDraws("G");hudLateMember();hudBindScene();}hudOutput();});
    CHECK(src->try_acquire_latest(3,f));
    if(!(f.effects_valid&&f.effects_draws==32&&f.effects_refused==1)){std::fprintf(stderr,"valid %d members %u draws %u refused %u\n",int(f.effects_valid),f.members,f.effects_draws,f.effects_refused);CHECK(false);}
    CHECK(s5ref().effEpochCapFrames>=1&&std::string(s5ref().effLastWhy)=="effects-epoch-cap");src->release(f);
    // (e) phase: after the scene target left RENDER_TARGET nothing is a candidate until the next member; a draw on another target is not one.
    b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();effDraws("G");
        hudSceneBarrier(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);hudSceneBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        hudBindScene();effDraws("G");
        auto* l=listAlias(0);const D3D12_CPU_DESCRIPTOR_HANDLE h{hudOutputHandle};l->OMSetRenderTargets(1,&h,FALSE,nullptr);l->DrawIndexedInstanced(3,1,0,0,0);
        hudOutput();});
    CHECK(src->try_acquire_latest(4,f));
    if(!(f.effects_valid&&f.effects_draws==1&&f.effects_tail==1&&f.effects_refused==0)){std::fprintf(stderr,"phase: valid %d draws %u tail %u refused %u members %u spec %llu ignored %llu refusedSum %llu last %s\n",int(f.effects_valid),f.effects_draws,f.effects_tail,f.effects_refused,f.members,(unsigned long long)s5ref().effSpeculative,(unsigned long long)s5ref().effIgnored,(unsigned long long)s5ref().effRefusedSum,s5ref().effLastWhy?s5ref().effLastWhy:"none");CHECK(false);}
    {const auto c=effCounts(b,f.slot);CHECK(c.replays==2);CHECK(s5ref().effIgnored>=1);}
    src->release(f);
    // (f) Q (step174 fxadd null runs): the densest measured frame (AGB2 f9673: 131 class-sprite + 58 tracer + 2 explosion candidates = 191) is replayed whole, no
    // `effects-cap` refusal: before Q the cap was 96 and 95 of them were refused (AGC1 refused=3631 over the dense seconds).
    {const unsigned peak=131+58+2;
     b=hudFrame(0,[peak]{hudBindScene();effDraws(std::string(peak,'G'));hudLateMember();hudBindScene();hudOutput();});
     CHECK(src->try_acquire_latest(5,f)&&f.effects_valid);
     if(!(f.effects_draws==peak&&f.effects_refused==0)){std::fprintf(stderr,"peak frame: draws %u refused %u (want %u/0)\n",f.effects_draws,f.effects_refused,peak);CHECK(false);}
     CHECK(gpu_copy::step5::effects_draw_cap*100>=peak*130);   // at least 30 % margin over the measured peak
     {const auto c=effCounts(b,f.slot);CHECK(c.replays==peak&&c.predOn==peak&&c.predOff==peak);}
     src->release(f);}
    ticks(60);const auto text=hudLog();
    CHECK(text.find("event=effects-frame;outcome=first-refused;")!=std::string::npos&&text.find("const-blend=1")!=std::string::npos);
    step3Lines();
}

// ------------------------------------------------------------- J-effects-dual (gpu_copy_step5_effects_dual)
static Object effCloneObj;
static void effectsDualSetup(bool dualKey,bool ready=true,bool clone=true) {
    effectsSetup(true);acz::g_effectsDualTest=dualKey;
    effCloneObj.table=w->psoTable.data();effCloneObj.kind=k_pso;
    auto& s=acz::state();std::lock_guard lock(s.mutex);
    auto& p=s.pipelines.at(effPso('G'));
    p.output.dual_ready=ready;p.blend_src=5;p.blend_dst=6;p.blend_op=1;p.blend_src_alpha=18;p.blend_dst_alpha=19;p.blend_op_alpha=1;
    p.effectsDualClone=clone?std::shared_ptr<ID3D12PipelineState>(reinterpret_cast<ID3D12PipelineState*>(&effCloneObj),[](ID3D12PipelineState*){}):nullptr;
}
static void step5_effects_dual_rules() {
    using namespace gpu_copy::step5;using namespace gpu_copy::step3;
    // the exact tuple: RGB SRC_ALPHA/INV_SRC_ALPHA/ADD, alpha SRC1_ALPHA/INV_SRC1_ALPHA/ADD, full write mask (D3D12_BLEND 5,6 / 18,19, op 1)
    CHECK(effects_dual_blend_tuple(5,6,1,18,19,1,15));
    CHECK(!effects_dual_blend_tuple(1,5,1,18,19,1,15)&&!effects_dual_blend_tuple(5,6,1,2,6,1,15)&&!effects_dual_blend_tuple(5,2,1,18,19,1,15)&&!effects_dual_blend_tuple(5,6,2,18,19,1,15));
    CHECK(!effects_dual_blend_tuple(5,6,1,18,19,2,15)&&!effects_dual_blend_tuple(5,6,1,18,19,1,7)&&!effects_dual_blend_tuple(5,6,1,19,18,1,15)&&!effects_dual_blend_tuple(5,6,1,18,19,1,0));
    CHECK(effects_dual_clone_src_alpha==2&&effects_dual_clone_dst_alpha==6&&effects_repr_legacy==0&&effects_repr_coverage_v1==1);
    Shape s;s.tracking_complete=true;s.root_route=0x10;s.pso.known=true;s.pso.rt_count=1;s.pso.rtv[0]=10;s.pso.dsv=20;s.pso.samples=1;
    s.om_known=s.om_set=true;s.om_count=1;s.om_desc_proved=true;s.game_rt_status="resolved";s.game_rt=0x5000;s.game_rt_format=10;
    s.game_rt_width=2560;s.game_rt_height=1440;s.pso.depth_enable=true;s.pso.depth_func=cmp::greater;s.pso.blend0=true;
    s.pso.stencil=true;s.pso.front_func=s.pso.back_func=cmp::always;s.pso.stencil_write=255;s.pso.stencil_ops_keep=true;s.pso.write_mask0=15;
    HudTarget t;t.format=10;t.ds_format=20;t.width=2560;t.height=1440;t.scene=0x5000;
    EffectsAllow allow=EffectsAllow::defaults();
    EffectsClassIn off;off.allow=&allow;off.ps8=0xd2464ed2u;
    EffectsClassIn on=off;on.dual=true;
    auto why=[&](const Shape& x,const EffectsClassIn& c){const auto v=effects_decide(x,t,effects_open,0,&c);return std::make_pair(v.kind,std::string(v.why?v.why:""));};
    // key off (dual=false): no new category, whatever dual_ready says
    CHECK(why(s,off)==std::make_pair(int(effects_ok),std::string()));
    // key on: only a PSO whose exact tuple has its private clone is a candidate; everything else is refused (fail closed)
    CHECK(why(s,on)==std::make_pair(int(effects_refused),std::string("dual-blend-unsupported")));
    {auto x=s;x.pso.dual_ready=true;CHECK(why(x,on)==std::make_pair(int(effects_ok),std::string()));}
    {auto x=s;x.pso.dual_ready=true;x.pso.blend_constant=true;CHECK(why(x,on)==std::make_pair(int(effects_refused),std::string("blend-constant-factor")));}  // the earlier rule still wins
    {auto x=s;x.pso.dual_ready=true;x.pso.depth_write=true;CHECK(why(x,on)==std::make_pair(int(effects_ignored),std::string("class-state-not-effect")));}  // the class gate (no depth writers) comes first
    // a draw that is not an effects candidate stays ignored, never refused by the dual rule
    {auto x=s;x.pso.blend0=false;x.pso.depth_enable=false;x.pso.stencil=false;CHECK(why(x,on)==std::make_pair(int(effects_ignored),std::string("not-effect-like")));}
    {EffectsClassIn h=on;h.ps8=effects_ps_deny;CHECK(why(s,h)==std::make_pair(int(effects_ignored),std::string("class-hud-sprite")));}
    {EffectsClassIn u=on;u.ps8=0x12345678u;CHECK(why(s,u)==std::make_pair(int(effects_ignored),std::string("class-ps-not-allowlisted")));}
    // no class input at all (the first-version rules table): unchanged
    CHECK(effects_decide(s,t,effects_open,0).kind==effects_ok);
}
static void step5_effects_dual() {
    effectsDualSetup(true);auto* src=acz::stereo::frame_source();
    auto* gamePso=effPso('G');auto* clonePso=reinterpret_cast<ID3D12PipelineState*>(&effCloneObj);
    acz::stereo::StereoFrame f;
    // (a) 2 candidates in the committed epoch, a later member, 1 tail candidate: every replay runs under the clone and the game's PSO is restored before its own draw.
    const unsigned setsBefore=w->lists[0].psoSets;
    size_t b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();effDraws("G");hudDraws(2);hudOutput();});
    CHECK(src->try_acquire_latest(0,f)&&f.frame_id==1);
    if(!(f.effects_valid&&f.effects&&f.effects_draws==2&&f.effects_refused==0&&f.effects_tail==3)){
        std::fprintf(stderr,"dual: valid %d draws %u refused %u tail %u\n",int(f.effects_valid),f.effects_draws,f.effects_refused,f.effects_tail);CHECK(false);}
    CHECK(f.effects_repr==acz::stereo::effects_repr_coverage_v1);
    {const auto& L=w->lists[0];unsigned cloneDraws=0,gameDraws=0;
     for(size_t i=b;i<L.ops.size();++i){const Op& o=L.ops[i];if(o.t!=Op::draw||!o.indexed)continue;
         if(o.boundPso==clonePso){++cloneDraws;CHECK(o.boundRtv==s5ref().slot[f.slot].effRtv.ptr);} // the clone only ever draws into the effects target
         else if(o.boundPso==gamePso&&originOf(o.boundRtv)==gameRtvHandle)++gameDraws;}               // the game's own draws: its PSO, its target
     CHECK(cloneDraws==4&&gameDraws==4); // indexed draws: 3 effect draws + the indexed HUD-like draw (hudDraws reuse the bound PSO)
     CHECK(L.psoSets-setsBefore==8/*the game and the frame scaffold own SetPipelineState calls*/+5*2/*clone then restore per replay*/);
     const auto c=effCounts(b,f.slot);CHECK(c.replays==5&&c.predOn==5&&c.predOff==5&&c.clearsEff==1);
     CHECK(w->predDrawn==2&&w->predSkipped==3);}                                                       // predication unchanged: committed executes, tail skipped
    {const auto& s5=s5ref();uint64_t charged=0;CHECK(!gpu_copy::step5::effects_budget(s5.hudFormat,s5.worldW,s5.worldH,gpu_copy::step5::slots,charged)&&s5.effCharged==charged);} // no extra target: the legacy budget
    CHECK(f.effects==s5ref().slot[f.slot].effRt&&res(f.effects).state==D3D12_RESOURCE_STATE_RENDER_TARGET);  // the same single private target, published in RENDER_TARGET
    src->release(f);
    // (c) key off while the clone exists: legacy exactly (no SetPipelineState swap, legacy tag).
    acz::g_effectsDualTest=false;
    const unsigned sets2=w->lists[0].psoSets;
    b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();effDraws("G");hudOutput();});
    CHECK(src->try_acquire_latest(1,f)&&f.effects_valid&&f.effects_draws==2&&f.effects_repr==acz::stereo::effects_repr_legacy);
    CHECK(w->lists[0].psoSets-sets2==8); // no swaps: the same game + scaffold calls as the key-on frame minus its 10 swaps
    for(size_t i=b;i<w->lists[0].ops.size();++i)CHECK(w->lists[0].ops[i].boundPso!=clonePso);
    src->release(f);
    acz::g_effectsDualTest=true;
    // (d) published tag only with a valid layer: a tail-only frame has no effects and no tag.
    hudFrame(0,[]{hudBindScene();effDraws("G");hudOutput();});
    CHECK(src->try_acquire_latest(2,f)&&!f.effects_valid&&!f.effects&&f.effects_repr==acz::stereo::effects_repr_legacy);src->release(f);
    step3Lines();
}
static void dualRefusedCase(int k) {
    acz::stereo::StereoFrame f;
    if(k==0)effectsDualSetup(true,false,false);else if(k==1)effectsDualSetup(true,true,false);else effectsDualSetup(true);
    auto* src=acz::stereo::frame_source(); // (obtained after the setup: it resets the source)
    auto* clonePso=reinterpret_cast<ID3D12PipelineState*>(&effCloneObj);
    size_t b=0;
    if(k==0) { // exact tuple but no clone (creation failed): dual_ready false -> every candidate refused, nothing recorded for it, the layer is absent.
        b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();hudOutput();});
        CHECK(src->try_acquire_latest(0,f)&&!f.effects_valid&&!f.effects&&f.effects_draws==0&&f.effects_refused==2&&f.effects_repr==acz::stereo::effects_repr_legacy);
        {const auto c=effCounts(b,f.slot);CHECK(c.replays==0&&c.predOn==0);}
        CHECK(std::string(s5ref().effLastWhy)=="dual-blend-unsupported");
        for(size_t i=b;i<w->lists[0].ops.size();++i)CHECK(w->lists[0].ops[i].boundPso!=clonePso);
        // (K, docs section 17) the refused candidates are drawn nowhere in the eye: the status line names the last one's blend tuple and shape
        {ticks(60);const auto text=hudLog();const auto at=text.find("continuous-hud-effects-dual-refused;count=");
         if(at==std::string::npos){std::fprintf(stderr,"no dual-refused line\n");CHECK(false);}
         else {const auto line=text.substr(at,text.find('\n',at)-at);std::fprintf(stderr,"  (K) %s\n",line.c_str());
             CHECK(line.find(";last=ps")!=std::string::npos&&line.find(";blend=5,6,1/18,19,1;dual-ready=0;")!=std::string::npos);}}
    } else if(k==1) { // dual_ready claimed but the record holds no clone for the bound PSO: refused before a predicate slot is used (fail closed).
        b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
        CHECK(src->try_acquire_latest(0,f)&&!f.effects_valid&&f.effects_refused==1&&std::string(s5ref().effLastWhy)=="dual-clone-missing");
        {const auto c=effCounts(b,f.slot);CHECK(c.replays==0);}
    } else { // a PSO that is not the dual tuple (C = constant blend, dual_ready false) is refused and counted; the good ones in the same epoch still publish.
        hudFrame(0,[]{hudBindScene();effDraws("GCG");hudLateMember();hudBindScene();hudOutput();});
        CHECK(src->try_acquire_latest(0,f)&&f.effects_valid&&f.effects_draws==2&&f.effects_refused==1&&f.effects_repr==acz::stereo::effects_repr_coverage_v1);
    }
    src->release(f);
    step3Lines();
}
// EFFECTS-OUTSIDE (P) fxadd (docs effects-outside-window.md section 21): the additive 4caf6e2d tuple 5,2,1/18,19,1 with its clone (additive_ready, dual_ready false).
// on=false: byte-identical to the clone-less refusal above (refused, drawn nowhere, the dual-refused line with dual-ready=0, no -additive line); on=true: admitted
// through the clone into the coverage layer and counted on the -additive status line.
static void additiveCase(bool on) {
    acz::stereo::StereoFrame f;
    effectsDualSetup(true);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);auto& p=s.pipelines.at(effPso('G'));
     p.output.dual_ready=false;p.output.additive_ready=true;p.blend_src=5;p.blend_dst=2;p.blend_op=1;p.blend_src_alpha=18;p.blend_dst_alpha=19;p.blend_op_alpha=1;}
    gpu_copy::step5::effects_additive_flag().store(on);
    auto* src=acz::stereo::frame_source();
    auto* clonePso=reinterpret_cast<ID3D12PipelineState*>(&effCloneObj);
    const size_t b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();hudOutput();});
    ticks(60);const auto text=hudLog();
    const bool addLine=text.find("continuous-hud-effects-additive;")!=std::string::npos;
    if(!on) {
        CHECK(src->try_acquire_latest(0,f)&&!f.effects_valid&&!f.effects&&f.effects_draws==0&&f.effects_refused==2&&f.effects_repr==acz::stereo::effects_repr_legacy);
        {const auto c=effCounts(b,f.slot);CHECK(c.replays==0&&c.predOn==0);}
        CHECK(std::string(s5ref().effLastWhy)=="dual-blend-unsupported");
        for(size_t i=b;i<w->lists[0].ops.size();++i)CHECK(w->lists[0].ops[i].boundPso!=clonePso);
        const auto at=text.find("continuous-hud-effects-dual-refused;count=");
        CHECK(at!=std::string::npos);
        if(at!=std::string::npos){const auto line=text.substr(at,text.find('\n',at)-at);std::fprintf(stderr,"  (P) off: %s\n",line.c_str());
            CHECK(line.find(";blend=5,2,1/18,19,1;dual-ready=0;tracer-ready=0;")!=std::string::npos);}
        CHECK(!addLine&&s5ref().effAddAdmitted==0);
    } else {
        CHECK(src->try_acquire_latest(0,f)&&f.effects_valid&&f.effects_draws==2&&f.effects_refused==0&&f.effects_repr==acz::stereo::effects_repr_coverage_v1);
        {const auto c=effCounts(b,f.slot);CHECK(c.replays==2);}
        bool cloneBound=false;for(size_t i=b;i<w->lists[0].ops.size();++i)cloneBound|=w->lists[0].ops[i].boundPso==clonePso;
        CHECK(cloneBound&&s5ref().effAddAdmitted==2&&s5ref().effAddPoints==0);
        CHECK(text.find("continuous-hud-effects-dual-refused;")==std::string::npos);
        const auto at=text.find("continuous-hud-effects-additive;");
        CHECK(addLine);
        if(at!=std::string::npos){const auto line=text.substr(at,text.find('\n',at)-at);std::fprintf(stderr,"  (P) on: %s\n",line.c_str());
            CHECK(line.find(";on=1;tuple=5,2,1/18,19,1;admitted=2;points=0;")!=std::string::npos);}
    }
    src->release(f);
    gpu_copy::step5::effects_additive_flag().store(false);
    step3Lines();
}
static void step5_effects_additive_off() {additiveCase(false);}
static void step5_effects_additive_on() {additiveCase(true);}
static void step5_effects_dual_refused() {dualRefusedCase(0);}
static void step5_effects_dual_clone_missing() {dualRefusedCase(1);}
static void step5_effects_dual_mixed() {dualRefusedCase(2);}

// ------------------------------------------------------------- J-effects-smoke-fix
static void step5_effects_smoke_rules() {
    using namespace gpu_copy::step5;using namespace gpu_copy::step3;
    // D3D12 predication truth table: an ENABLED predication discards the draw (EQUAL_ZERO: all 64 bits zero; NOT_EQUAL_ZERO: any bit set).
    CHECK(effects_pred_executes(effects_pred_eq0,1)&&!effects_pred_executes(effects_pred_eq0,0)&&effects_pred_executes(effects_pred_eq0,0x100000000ull));
    CHECK(!effects_pred_executes(effects_pred_ne0,1)&&effects_pred_executes(effects_pred_ne0,0)&&!effects_pred_executes(effects_pred_ne0,0x100000000ull));
    CHECK(effects_pred_executes(effects_pred_off,0)&&effects_pred_executes(effects_pred_off,1));
    CHECK(parse_effects_pred_mode("eq0",3)==effects_pred_eq0&&parse_effects_pred_mode(" NE0\r\n",6)==effects_pred_ne0&&parse_effects_pred_mode("off\n",4)==effects_pred_off);
    CHECK(parse_effects_pred_mode("",0)==-1&&parse_effects_pred_mode("on",2)==-1&&parse_effects_pred_mode("eq01",4)==-1&&parse_effects_pred_mode("e q0",4)==-1);
    // allowlist: built-in {d2464ed2, 4caf6e2d}; the file can only add 8-hex prefixes (comments, separators, duplicates, bad tokens counted)
    {EffectsAllow a=EffectsAllow::defaults();CHECK(a.count==2&&a.has(0xd2464ed2u)&&a.has(0x4caf6e2du)&&!a.has(0xd34ee153u));
     unsigned bad=0;const char* t="# comment 11111111\nABCDEF01, 0badf00d;d2464ed2\n123 xyz 123456789 0123456g\n";
     CHECK(parse_effects_ps_text(t,std::strlen(t),a,bad)==2&&bad==4&&a.count==4&&a.has(0xabcdef01u)&&a.has(0x0badf00du)&&!a.has(0x11111111u));
     EffectsAllow full;for(unsigned i=0;i<effects_allow_cap;++i)CHECK(full.add(0x1000u+i));CHECK(!full.add(0x9999u)&&full.count==effects_allow_cap);}
    Shape x;x.tracking_complete=true;x.root_route=0x10;x.pso.known=true;x.pso.rt_count=1;x.pso.rtv[0]=10;x.pso.dsv=20;x.pso.samples=1;
    x.om_known=x.om_set=true;x.om_count=1;x.om_desc_proved=true;x.game_rt_status="resolved";x.game_rt=0x5000;x.game_rt_format=10;
    x.game_rt_width=2560;x.game_rt_height=1440;x.pso.depth_enable=true;x.pso.depth_func=cmp::greater_equal;x.pso.blend0=true;
    x.pso.stencil=true;x.pso.front_func=x.pso.back_func=cmp::always;x.pso.stencil_write=255;x.pso.stencil_ops_keep=true;x.pso.write_mask0=15;
    HudTarget t;t.format=10;t.ds_format=20;t.width=2560;t.height=1440;t.scene=0x5000;
    const EffectsAllow allow=EffectsAllow::defaults();
    auto run=[&](const Shape& sh,uint32_t ps8,bool writable=false){EffectsClassIn c;c.allow=&allow;c.ps8=ps8;c.writable=writable;return effects_decide(sh,t,effects_open,0,&c);};
    auto is=[&](const EffectsVerdict& v,int kind,const char* why){return v.kind==kind&&std::string(v.why?v.why:"")==why;};
    CHECK(is(run(x,0xd2464ed2u),effects_ok,"")&&is(run(x,0x4caf6e2du),effects_ok,""));
    CHECK(is(run(x,0x12345678u),effects_ignored,"class-ps-not-allowlisted"));
    CHECK(is(run(x,effects_ps_deny),effects_ignored,"class-hud-sprite"));                       // the HUD sprite shader is never an effect
    {auto y=x;y.pso.depth_func=cmp::always;CHECK(is(run(y,0xd2464ed2u),effects_ignored,"class-state-not-effect"));}   // depth ALWAYS: the HUD state class
    {auto y=x;y.pso.depth_enable=false;CHECK(is(run(y,0xd2464ed2u),effects_ignored,"class-state-not-effect"));}
    {auto y=x;y.pso.depth_write=true;CHECK(is(run(y,0xd2464ed2u),effects_ignored,"class-state-not-effect"));}
    {auto y=x;y.pso.depth_func=cmp::less;CHECK(is(run(y,0xd2464ed2u),effects_ignored,"class-state-not-effect"));}
    {auto y=x;y.pso.blend0=false;CHECK(is(run(y,0xd2464ed2u),effects_ignored,"class-state-not-effect"));} // the class needs blending too
    {auto y=x;y.pso.blend0=false;y.pso.depth_func=cmp::always;CHECK(is(run(y,0xd2464ed2u),effects_ignored,"not-effect-like"));}
    CHECK(is(run(x,0xd2464ed2u,true),effects_refused,"shader-writable-binding"));              // a UAV-exposing root signature: refused even for an allowlisted shader
    {auto y=x;y.pso.front_func=cmp::equal;CHECK(is(run(y,0xd2464ed2u),effects_refused,"stencil-test-not-always"));} // allowlisted + non-replayable state: still refused, counted
    // no gate (nullptr): the first version's rules, bit for bit
    CHECK(effects_decide(x,t,effects_open,0).kind==effects_ok);
    EffectsPsTable tab;CHECK(tab.touch(1)&&tab.touch(2)&&tab.touch(1)==tab.touch(1)&&tab.count==2);
    for(unsigned i=0;i<effects_ps_table_cap;++i)tab.touch(100+i);
    CHECK(tab.count==effects_ps_table_cap&&tab.overflow>=2&&!tab.touch(999999));
}
// EFFECTS-OUTSIDE (A): gamecloud:0 = the game's cloud class (ps 4caf6e2d + depth GREATER_EQUAL) is not replayed; everything else is unchanged.
static void step5_effects_gamecloud_rules() {
    using namespace gpu_copy::step5;using namespace gpu_copy::step3;
    Shape x;x.tracking_complete=true;x.root_route=0x10;x.pso.known=true;x.pso.rt_count=1;x.pso.rtv[0]=10;x.pso.dsv=20;x.pso.samples=1;
    x.om_known=x.om_set=true;x.om_count=1;x.om_desc_proved=true;x.game_rt_status="resolved";x.game_rt=0x5000;x.game_rt_format=10;
    x.game_rt_width=2560;x.game_rt_height=1440;x.pso.depth_enable=true;x.pso.depth_func=cmp::greater_equal;x.pso.blend0=true;
    x.pso.stencil=true;x.pso.front_func=x.pso.back_func=cmp::always;x.pso.stencil_write=255;x.pso.stencil_ops_keep=true;x.pso.write_mask0=15;
    HudTarget t;t.format=10;t.ds_format=20;t.width=2560;t.height=1440;t.scene=0x5000;
    const EffectsAllow allow=EffectsAllow::defaults();
    auto run=[&](const Shape& sh,uint32_t ps8,bool drop){EffectsClassIn c;c.allow=&allow;c.ps8=ps8;c.dropClouds=drop;return effects_decide(sh,t,effects_open,0,&c);};
    auto is=[&](const EffectsVerdict& v,int kind,const char* why){return v.kind==kind&&std::string(v.why?v.why:"")==why;};
    CHECK(effects_ps_cloud==0x4caf6e2du&&effects_is_game_cloud(0x4caf6e2du,x.pso));
    {auto y=x;y.pso.depth_func=cmp::greater;CHECK(!effects_is_game_cloud(0x4caf6e2du,y.pso));}
    {auto y=x;y.pso.known=false;CHECK(!effects_is_game_cloud(0x4caf6e2du,y.pso));}
    CHECK(!effects_is_game_cloud(0xd2464ed2u,x.pso));
    // drop off: exactly the previous verdicts
    CHECK(is(run(x,0x4caf6e2du,false),effects_ok,"")&&is(run(x,0xd2464ed2u,false),effects_ok,""));
    // drop on: the cloud class is ignored (a class-* verdict: counted with the candidate table), the GREATER sprites and the smoke stay candidates
    CHECK(is(run(x,0x4caf6e2du,true),effects_ignored,"class-game-cloud"));
    {auto y=x;y.pso.depth_func=cmp::greater;CHECK(is(run(y,0x4caf6e2du,true),effects_ok,""));}
    CHECK(is(run(x,0xd2464ed2u,true),effects_ok,""));
    // the earlier class rules still win (HUD sprite, not allowlisted, state)
    CHECK(is(run(x,effects_ps_deny,true),effects_ignored,"class-hud-sprite")&&is(run(x,0x12345678u,true),effects_ignored,"class-ps-not-allowlisted"));
    {auto y=x;y.pso.depth_write=true;CHECK(is(run(y,0x4caf6e2du,true),effects_ignored,"class-state-not-effect"));}
    CHECK(!effects_drop_game_clouds_flag().load()); // the process default
}
static void step5_effects_gamecloud() {
    effectsSetup(true);auto* src=acz::stereo::frame_source();
    using namespace gpu_copy::step3;
    // X = the game cloud class (4caf6e2d, GREATER_EQUAL), L = a 4caf6e2d sprite with depth GREATER (kept), G = the smoke (d2464ed2).
    hudPipeline(effPso('X'),[](auto& o){o.depth_func=cmp::greater_equal;});
    hudPipeline(effPso('L'),[](auto& o){o.depth_func=cmp::greater;});
    {auto& s=acz::state();std::lock_guard lock(s.mutex);
     for(char c:{'X','L'})std::strcpy(s.pipelines.at(effPso(c)).hudPs.data(),"4caf6e2dc0ffee004caf6e2dc0ffee00");}
    acz::stereo::StereoFrame f;
    // (a) flag off (the default, terrain key off / gamecloud:1): all three replayed, byte-identical to before
    CHECK(!gpu_copy::step5::effects_drop_game_clouds_flag().load());
    size_t b=hudFrame(0,[]{hudBindScene();effDraws("GXL");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(0,f)&&f.effects_valid&&f.effects_draws==3&&f.effects_refused==0);
    CHECK(effCounts(b,f.slot).replays==3&&s5ref().effGameCloudIgnored==0);
    src->release(f);
    {const auto text=hudLog();CHECK(text.find("-gamecloud;")==std::string::npos);}   // no new line while the option is off
    // (b) flag on (gamecloud:0 / cloud:1 active): the cloud draw is not replayed, the game's own draw still runs, the others are unchanged
    gpu_copy::step5::effects_drop_game_clouds_flag().store(true);
    const unsigned draws0=unsigned(std::count_if(w->lists[0].ops.begin(),w->lists[0].ops.end(),[](const Op& o){return o.t==Op::draw;}));
    b=hudFrame(0,[]{hudBindScene();effDraws("GXL");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(1,f)&&f.effects_valid&&f.effects_draws==2&&f.effects_refused==0);
    CHECK(effCounts(b,f.slot).replays==2&&s5ref().effGameCloudIgnored==1);
    {unsigned gameDraws=0;for(size_t i=b;i<w->lists[0].ops.size();++i)gameDraws+=w->lists[0].ops[i].t==Op::draw&&w->lists[0].ops[i].boundPso==effPso('X')&&
        originOf(w->lists[0].ops[i].boundRtv)==gameRtvHandle;CHECK(gameDraws==1);(void)draws0;}
    src->release(f);
    // (c) only the cloud class: no committed effects draw -> no effects layer this frame (nothing else changes)
    b=hudFrame(0,[]{hudBindScene();effDraws("X");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(2,f)&&!f.effects_valid&&f.effects_draws==0&&effCounts(b,f.slot).replays==0&&s5ref().effGameCloudIgnored==2);
    src->release(f);
    ticks(60);
    {const auto text=hudLog();
     CHECK(text.find("continuous-hud-effects-gamecloud;replay=off;class=ps-4caf6e2d+depth-greater-equal;ignored=2;key=xr_terrain_stage(gamecloud:0|1)")!=std::string::npos);}
    gpu_copy::step5::effects_drop_game_clouds_flag().store(false);
    step3Lines();
}
static void step5_effects_smoke() {
    effectsSetup(true);auto* src=acz::stereo::frame_source();
    acz::stereo::StereoFrame f;uint64_t seen=0;
    auto pred=[&](size_t b){unsigned on=0,off=0;for(size_t i=b;i<w->lists[0].ops.size();++i)if(w->lists[0].ops[i].t==Op::pred){if(w->lists[0].ops[i].res)++on;else ++off;}return std::make_pair(on,off);};
    // (A) default mode eq0: smoke-class draws commit (word 1 = EXECUTE), the same class in the tail stays off (word 0 = DISCARD); the HUD sprite class (H) and an
    // unknown pixel shader (X) are never replayed; the candidate table sees all three.
    CHECK(s5ref().effPredMode==gpu_copy::step5::effects_pred_eq0&&std::string(s5ref().effPredSource)=="default"&&s5ref().effAllow.count==2);
    size_t b=hudFrame(0,[]{hudBindScene();effDraws("GXH");hudLateMember();hudBindScene();effDraws("GXH");hudOutput();});
    CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&f.effects_draws==1&&f.effects_refused==0&&f.effects_tail==1);seen=f.frame_id;
    CHECK(effCounts(b,f.slot).replays==2&&pred(b)==std::make_pair(2u,2u));
    CHECK(w->predDrawn==1&&w->predSkipped==1);                 // committed executed, tail discarded
    CHECK(hudCovered(f.effects)>100);
    src->release(f);
    {const auto& s5=s5ref();CHECK(s5.effClassIgnored==4&&s5.effPs.count==3);
     const auto& e0=s5.effPs.e[0];CHECK(e0.ps8==0xd2464ed2u&&e0.allowed&&e0.seen==2&&e0.replayed==2);
     const auto& e1=s5.effPs.e[1];CHECK(e1.ps8==0x12345678u&&!e1.allowed&&e1.seen==2&&e1.replayed==0);
     const auto& e2=s5.effPs.e[2];CHECK(e2.ps8==0xd34ee153u&&!e2.allowed&&e2.seen==2&&e2.replayed==0);}
    ticks(60);{const auto text=hudLog();
    for(const char* k:{"event=effects-replay;frame=1;set=early;epoch=0;pred-offset=","pred-word-at-close=1;pred-op=eq0;gpu-executes=1;phase=btw(committed);",
            "epoch=1;pred-offset=8;pred-word-at-close=0;pred-op=eq0;gpu-executes=0;phase=tail;","depth=greater;","allowlisted=1",
            "continuous-hud-effects-ps;pred-mode=eq0(source=default);allow=2(source=default,added=0,bad=0);class-ignored=4;pred-refused=0;uav-or-layout-refused=0;",
            "continuous-hud-effects-ps-candidates;entries=3;list=d2464ed2:A:","12345678:-:","d34ee153:-:",
            "event=effects-ring-config;predication=eq0(source=default;d3d12=enabled-predication-discards-the-draw;word0=discard,word1=execute);class-gate=ps-allowlist(2,source=default,added=0,bad=0):d2464ed2,4caf6e2d+blend"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}}
    // (B) the old inverted operation (ne0), reproduced: committed DISCARDED, tail EXECUTED (the live leak), unchanged class gate.
    s5ref().effPredMode=gpu_copy::step5::effects_pred_ne0;{const unsigned d0=w->predDrawn,s0=w->predSkipped;
    b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();effDraws("G");hudOutput();});
    CHECK(src->try_acquire_latest(seen,f));seen=f.frame_id;
    CHECK(w->predDrawn==d0+1&&w->predSkipped==s0+1);           // exactly one executed (the TAIL) and one discarded (the committed one)
    CHECK(hudCovered(f.effects)>100);                          // (the tail draw alone rasterised)
    src->release(f);}
    // (C) off: no predication commands at all; every allowlisted candidate executes.
    s5ref().effPredMode=gpu_copy::step5::effects_pred_off;{const unsigned d0=w->predDrawn,s0=w->predSkipped;
    b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();effDraws("G");hudOutput();});
    CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&f.effects_draws==1&&f.effects_tail==1);seen=f.frame_id;
    unsigned omEff=0;for(size_t i=b;i<w->lists[0].ops.size();++i)omEff+=w->lists[0].ops[i].t==Op::om&&w->lists[0].ops[i].rtv[0]==s5ref().slot[f.slot].effRtv.ptr;
    CHECK(pred(b)==std::make_pair(0u,0u)&&omEff==2&&w->predDrawn==d0&&w->predSkipped==s0);
    src->release(f);}
    s5ref().effPredMode=gpu_copy::step5::effects_pred_eq0;
    // (D) Astra P1-2: a GAME predicate active at the candidate: refused, no effects command, the game's predication is untouched (its on/off only).
    w->gamePredicate=true;w->gamePredDraws=0;{auto* gameRes=reinterpret_cast<ID3D12Resource*>(&w->fence);w->gamePredObj=obj(gameRes);const uint64_t r0=s5ref().effPredRefused;
    b=hudFrame(0,[gameRes]{hudBindScene();auto* l=listAlias(0);l->SetPredication(gameRes,0,D3D12_PREDICATION_OP_EQUAL_ZERO);effDraws("G");
        l->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&f.effects_draws==1&&f.effects_refused==1);seen=f.frame_id;   // the 2nd candidate (predicate off again) replays
    CHECK(s5ref().effPredRefused==r0+1&&std::string(s5ref().effLastWhy)=="game-predication-not-provably-off");
    CHECK(pred(b)==std::make_pair(2u,2u));                       // game on + our on ; game off + our off
    size_t gameOn=0,gameOff=0,ours=0;const auto& L=w->lists[0];
    for(size_t i=b;i<L.ops.size();++i)if(L.ops[i].t==Op::pred){if(L.ops[i].res==reinterpret_cast<Object*>(gameRes))++gameOn;else if(L.ops[i].res)++ours;else ++gameOff;}
    CHECK(gameOn==1&&ours==1&&gameOff==2);                       // the refused candidate recorded nothing: game on, [refused draw], game off, our on/Draw/off
    CHECK(w->gamePredDraws==1);                                  // the game's own draw of the refused candidate ran under the game's predicate (forwarded, untouched)
    src->release(f);w->gamePredicate=false;}
    // (E) Astra P1-3: UAV exposure / unknown root layout refused (even for an allowlisted shader)
    for(int mode:{1,2,3}) {
        effRootMode=mode;const uint64_t u0=s5ref().effUavRefused;
        hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
        CHECK(src->try_acquire_latest(seen,f)&&!f.effects_valid&&f.effects_draws==0&&f.effects_refused==1);seen=f.frame_id;
        CHECK(s5ref().effUavRefused==u0+1&&std::string(s5ref().effLastWhy)==(mode==3?"root-layout-unknown":"shader-writable-binding"));
        src->release(f);
    }
    effRootMode=0;
    step3Lines();
}
// EFFECTS-OUTSIDE (C) fxmem:1 (effects_smoke_capture_flag): capture + log ONLY. G gets the smoke VS (9eff1f92) so it has the smoke pipeline's identity.
// The fixture draws are INDEXED, which the real smoke draw never is: the capture refuses them before any memory read (smoke-draw-indexed, counted), so
// this proves the wiring, the fail-closed path, the no-allocation Draw path (effDraws checks it) and that the command stream does not change.
static void step5_effects_smoke_capture() {
    effectsSetup(true);auto* src=acz::stereo::frame_source();
    {auto& s=acz::state();std::lock_guard lock(s.mutex);std::strcpy(s.pipelines.at(effPso('G')).hudVs.data(),"9eff1f92c0ffee009eff1f92c0ffee00");}
    acz::stereo::StereoFrame f;uint64_t seen=0;
    auto drawsOf=[&](size_t b){unsigned n=0;for(size_t i=b;i<w->lists[0].ops.size();++i)n+=w->lists[0].ops[i].t==Op::draw;return n;};
    // (a) flag off (the default): nothing of the capture runs (a warm-up frame first, then the measured one)
    CHECK(!gpu_copy::step5::effects_smoke_capture_flag().load());
    size_t b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(seen,f));seen=f.frame_id;src->release(f);
    b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    const unsigned offDraws=drawsOf(b);
    CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&f.effects_draws==1);seen=f.frame_id;
    const unsigned offReplays=effCounts(b,f.slot).replays;src->release(f);
    CHECK(s5ref().fxDraws==0&&!s5ref().fxOpen&&!s5ref().fxPending&&s5ref().fxFrames==0);
    // (b) flag on: the same commands, the draw is seen and refused (counted), one fx-smoke-frame line + the totals line
    gpu_copy::step5::effects_smoke_capture_flag().store(true);
    b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    CHECK(drawsOf(b)==offDraws);
    CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&f.effects_draws==1);seen=f.frame_id;
    CHECK(effCounts(b,f.slot).replays==offReplays);src->release(f);
    {const auto& s5=s5ref();
     CHECK(s5.fxDraws==1&&s5.fxStored==0&&s5.fxUnread==1&&s5.fxLastUnread&&std::string(s5.fxLastUnread)=="smoke-draw-indexed"&&s5.fxFrames==1&&!s5.fxOpen);}
    // (c) a non-smoke draw (X: another pixel shader) is never captured
    b=hudFrame(0,[]{hudBindScene();effDraws("X");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(seen,f));seen=f.frame_id;src->release(f);
    CHECK(s5ref().fxDraws==1&&s5ref().fxFrames==1);
    ticks(60);
    {const auto text=hudLog();
     for(const char* k:{"gpu-copy-step1;event=fx-smoke-frame;frame=",";draws=0;segments=0;points=0;pts-in-window=0;pts-outside-window=0;pts-behind=0;a-differs=0;",
             ";overflow=0;refused=1;a-vs-obj=none;roots-why=none;last-why=smoke-draw-indexed;skipped=0",
             "gpu-copy-step1;event=fx-smoke-totals;smoke-frames=1;draws=1;stored=0;unread=1(last=smoke-draw-indexed);lines=1;store-cap=64;mode=capture-log-only(no-replay);over-cap=0;peak-draws=1"})
         if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
     CHECK(text.find("fx-smoke-frame",text.find("fx-smoke-frame")+1)==std::string::npos);} // exactly one frame line (one smoke frame)
    CHECK(hudLog().find("fx-smoke-replay-plan")==std::string::npos); // fxmem:1: no dry run
    // (d) fxmem:2 (replay flag too): still the same commands (the dry run is CPU only); the indexed draw is refused before any read, so nothing is prepared
    // and the plan line says so (draws=0), followed by the dry-run totals line. The replay flag alone (capture off) does nothing: the frame is not opened.
    gpu_copy::step5::effects_smoke_replay_flag().store(true);
    b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    CHECK(drawsOf(b)==offDraws);
    CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&f.effects_draws==1);seen=f.frame_id;
    CHECK(effCounts(b,f.slot).replays==offReplays);src->release(f);
    {const auto& s5=s5ref();CHECK(s5.fxDraws==2&&s5.fxStored==0&&s5.fxUnread==2&&s5.fxDryPrepared==0&&s5.fxDryRefused==0&&s5.fxFrames==2);}
    ticks(60);
    {const auto text=hudLog();
     for(const char* k:{"gpu-copy-step1;event=fx-smoke-replay-plan;frame=",";draws=0;prepared=0;refused=0;points=0;eye0-visible=0;eye1-visible=0;eye-only=0;lens-gain=0;",
             ";world=ok;last-why=none;mode=dry-run(cpu-only,no-gpu-replay)",
             "gpu-copy-step1;event=fx-smoke-replay-totals;prepared=0;refused=0;store-cap=64;store-bytes=204800;mode=dry-run(cpu-only,no-gpu-replay)"})
         if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}}
    gpu_copy::step5::effects_smoke_capture_flag().store(false);
    b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(seen,f));seen=f.frame_id;src->release(f);
    CHECK(s5ref().fxDraws==2&&!s5ref().fxDryOn&&!s5ref().fxDryPending);
    gpu_copy::step5::effects_smoke_replay_flag().store(false);
    step3Lines();
}
// EFFECTS-OUTSIDE (C) fxmem:4 (effects_smoke_gpu_flag; fx_smoke_gpu.h): (a) the per-eye smoke images + UPLOAD block exist after ring creation (fxmem:3
// requested then, effects_dual on) and the ring line says so; (b) a smoke draw the dry run could not prepare (the fixture draws are INDEXED: refused before any
// read) falls back to the window effects replay, byte-identical commands, no smoke fields, a named fx-smoke-gpu refusal; (c) the recorder itself on the fake
// list: clears, predication bracket, clone PSO, per eye OMSet(smoke image, eye objects DSV) + root SRV (eye copy) + Draw, the game's t10 / PSO / targets back;
// (d) fxmem:2 only (GPU flag off) with the resources present: nothing of the GPU path runs. The fixture cannot reach a PREPARED record (no world frame in the
// effects fixture): the glue from a prepared record to the recorder is first exercised live (docs 8.7 check list).
static void step5_effects_smoke_gpu() {
    using namespace gpu_copy::step5;
    effects_smoke_capture_flag().store(true);effects_smoke_replay_flag().store(true);effects_smoke_gpu_flag().store(true);
    acz::g_effectsDualTest=true;effectsDualSetup(true);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);std::strcpy(s.pipelines.at(effPso('G')).hudVs.data(),"9eff1f92c0ffee009eff1f92c0ffee00");}
    auto* src=acz::stereo::frame_source();auto* gamePso=effPso('G');auto* clonePso=reinterpret_cast<ID3D12PipelineState*>(&effCloneObj);
    namespace g=acz::fxsmoke::gpu;
    // (a) resources
    {const auto& s5=s5ref();
     if(!s5.smokeReady){std::fprintf(stderr,"smoke ring: %s\n",s5.smokeArmWhy?s5.smokeArmWhy:"?");CHECK(false);}
     CHECK(s5.effReady&&s5.smokeUpload&&s5.smokeUploadPtr&&s5.smokeUploadVa%65536==0&&s5.smokeCharged>0);
     for(const auto& sl:s5.slot){CHECK(sl.smokeRt[0]&&sl.smokeRt[1]&&sl.smokeRt[0]!=sl.smokeRt[1]&&sl.smokeRtv[0].ptr&&sl.smokeRtv[1].ptr&&sl.smokeRtv[0].ptr!=sl.smokeRtv[1].ptr);
         for(int e=0;e<2;++e)CHECK(res(sl.smokeRt[e]).state==D3D12_RESOURCE_STATE_RENDER_TARGET&&res(sl.smokeRt[e]).format==DXGI_FORMAT(s5.hudFormat));}
     CHECK(res(s5.smokeUpload).heap==D3D12_HEAP_TYPE_UPLOAD&&res(s5.smokeUpload).width==g::kSlotUpload*slots);}
    acz::stereo::StereoFrame f;uint64_t seen=0;
    // (b) one smoke draw in the committed epoch: not prepared -> the window replay exactly as without fxmem:4, counted as a fallback
    size_t b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&f.effects_draws==1);seen=f.frame_id;
    CHECK(!f.smoke_valid&&!f.smoke[0]&&!f.smoke[1]&&f.smoke_draws==0&&f.smoke_refused==1);
    {const auto c=effCounts(b,f.slot);CHECK(c.replays==1&&c.predOn==1&&c.predOff==1);
     const auto& sl=s5ref().slot[f.slot];
     for(size_t i=b;i<w->lists[0].ops.size();++i){const Op& o=w->lists[0].ops[i];
         CHECK(!(o.t==Op::om&&(o.rtv[0]==sl.smokeRtv[0].ptr||o.rtv[0]==sl.smokeRtv[1].ptr))&&!(o.t==Op::clear_rtv&&(o.rtv[0]==sl.smokeRtv[0].ptr||o.rtv[0]==sl.smokeRtv[1].ptr)));}}
    src->release(f);
    {const auto& s5=s5ref();CHECK(s5.smokeFrames==1&&s5.smokeValidFrames==0&&s5.smokeReplays==0&&s5.smokeRefused==1&&s5.smokeFallback==1&&
        s5.smokeLastWhy&&std::string(s5.smokeLastWhy)=="smoke-not-prepared");}
    ticks(60);
    {const auto text=hudLog();
     for(const char* k:{"gpu-copy-step1;event=fx-smoke-ring;outcome=ready;reason=none;slots=3;","per-slot=2x(smoke-rt)+upload:262144;dsv=eye-objects-depth(test-only);per-draw=12(+2-clears-once);lifetime=process(ring);created=ring",
             "gpu-copy-step1;event=fx-smoke-gpu;frame=",";replays=0;committed=0;refused=1;window-fallback=1;cleared=0;valid=0;last-why=smoke-not-prepared;mode=gpu-replay(",
             "gpu-copy-step1;event=fx-smoke-gpu-totals;frames=1;valid-frames=0;replays=0;committed=0;refused=1;window-fallback=1;last-why=smoke-not-prepared"})
         if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}}
    // (c) the recorder on the fake list (inside a frame's recording): the exact command sequence
    {const auto& s5=s5ref();const auto& sl=s5.slot[0];
     acz::Step5EffCall c;c.predRes=s5.effPred;c.predOffset=8;c.predOp=D3D12_PREDICATION_OP_EQUAL_ZERO;c.clone=clonePso;c.game=gamePso;
     c.count=3;c.instances=1;c.start=2;c.startInstance=0;c.omCount=1;c.omRtv[0].ptr=gameRtvHandle;c.omDsv=true;c.omDsvHandle.ptr=gameDsvHandle;
     acz::Step5SmokeCall s;s.clear=true;for(int e=0;e<2;++e){s.rtv[e]=sl.smokeRtv[e];s.dsv[e]=sl.dsv[e];}
     const uint64_t gameT10=gameVa(2)+srtOff;
     CHECK(acz::fxsmoke::plan_root_swap(3,true,gameT10,s5.smokeUploadVa,g::kSlotUpload,4,s.swap)==nullptr);
     unsigned sets0=0;size_t b2=0;
     hudFrame(0,[&]{hudBindScene(gamePso);sets0=w->lists[0].psoSets;b2=w->lists[0].ops.size();acz::step5RecordSmoke(listAlias(0),c,s);
         CHECK(w->lists[0].curPso==gamePso&&w->lists[0].psoSets==sets0+2);hudOutput();});
     std::vector<Op> ops;for(size_t i=b2;i<w->lists[0].ops.size();++i){if(w->lists[0].ops[i].t==Op::om&&w->lists[0].ops[i].rtv[0]==hudOutputHandle)break;ops.push_back(w->lists[0].ops[i]);}
     if(ops.size()!=12){std::fprintf(stderr,"smoke recorder ops %zu\n",ops.size());CHECK(false);}   // = kCmdsPerDraw - 2 native PSO sets (not Ops) + 2 clears
     else {
        CHECK(ops[0].t==Op::clear_rtv&&ops[0].rtv[0]==sl.smokeRtv[0].ptr&&ops[1].t==Op::clear_rtv&&ops[1].rtv[0]==sl.smokeRtv[1].ptr);
        CHECK(ops[2].t==Op::pred&&ops[2].res==&res(s5.effPred).object&&ops[2].predOff==8);
        for(unsigned e=0;e<2;++e){const Op& om=ops[3+3*e];const Op& sv=ops[4+3*e];const Op& d=ops[5+3*e];
            CHECK(om.t==Op::om&&om.count==1&&om.rtv[0]==sl.smokeRtv[e].ptr&&om.dsvSet&&om.dsv==sl.dsv[e].ptr);
            CHECK(sv.t==Op::srv&&sv.param==3&&sv.va==s.swap.eye_va[e]);
            CHECK(d.t==Op::draw&&!d.indexed&&d.dc==3&&d.di==1&&d.ds==2&&d.boundRtv==sl.smokeRtv[e].ptr&&d.boundDsv==sl.dsv[e].ptr&&d.boundT10==s.swap.eye_va[e]);}
        CHECK(s.swap.eye_va[0]==s5.smokeUploadVa+acz::fxsmoke::ReplayStore::offset(4,0)&&s.swap.eye_va[1]==s5.smokeUploadVa+acz::fxsmoke::ReplayStore::offset(4,1));
        CHECK(ops[9].t==Op::srv&&ops[9].param==3&&ops[9].va==gameT10);
        CHECK(ops[10].t==Op::pred&&!ops[10].res);
        CHECK(ops[11].t==Op::om&&ops[11].count==1&&originOf(ops[11].rtv[0])==gameRtvHandle&&ops[11].dsvSet&&originOf(ops[11].dsv)==gameDsvHandle);
     }}
    CHECK(src->try_acquire_latest(seen,f));seen=f.frame_id;src->release(f);
    // (d) fxmem:2 only: the resources exist but nothing of the GPU path runs (no refusal, no line)
    effects_smoke_gpu_flag().store(false);
    {const uint64_t fr=s5ref().smokeFrames,fb=s5ref().smokeFallback;
     b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
     CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&!f.smoke_valid&&f.smoke_refused==0);seen=f.frame_id;src->release(f);
     CHECK(s5ref().smokeFrames==fr&&s5ref().smokeFallback==fb&&effCounts(b,f.slot).replays==1);}
    effects_smoke_capture_flag().store(false);effects_smoke_replay_flag().store(false);
    step3Lines();
}
// Live run R2 ordering: the step-5 ring is created BEFORE the proxy sets the fxmem:4 flag (VR start). (a) ring with fxmem:2 only: no smoke resources, no
// fx-smoke-ring line, frames latch nothing; (b) the flag arrives: the next worker tick creates them once (6 RTs + 1 UPLOAD, 6 heaps), logs
// fx-smoke-ring;...;created=late(worker,option-after-ring), and the NEXT armed frame latches the GPU path (here: the fixture's unprepared draw -> the named
// window fallback); (c) further ticks never create again; (d) the flag cleared and set again: no second creation (process lifetime).
static void step5_effects_smoke_gpu_late() {
    using namespace gpu_copy::step5;
    effects_smoke_capture_flag().store(true);effects_smoke_replay_flag().store(true);effects_smoke_gpu_flag().store(false);
    acz::g_effectsDualTest=true;effectsDualSetup(true);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);std::strcpy(s.pipelines.at(effPso('G')).hudVs.data(),"9eff1f92c0ffee009eff1f92c0ffee00");}
    auto* src=acz::stereo::frame_source();acz::stereo::StereoFrame f;uint64_t seen=0;
    const unsigned res0=w->resourcesCreated,heaps0=w->heapsCreated;
    // (a)
    {const auto& s5=s5ref();CHECK(!s5.smokeReady&&!s5.smokeTried&&!s5.smokeUpload);for(const auto& sl:s5.slot)CHECK(!sl.smokeRt[0]&&!sl.smokeRt[1]);}
    hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(seen,f)&&!f.smoke_valid&&f.smoke_refused==0);seen=f.frame_id;src->release(f);
    CHECK(s5ref().smokeFrames==0&&s5ref().smokeFallback==0);
    ticks(5);CHECK(hudLog().find("fx-smoke-ring")==std::string::npos&&w->resourcesCreated==res0);
    // (b) the option arrives after the ring
    effects_smoke_gpu_flag().store(true);
    ticks(1);
    {const auto& s5=s5ref();
     if(!s5.smokeReady){std::fprintf(stderr,"late smoke: %s\n",s5.smokeArmWhy?s5.smokeArmWhy:"?");CHECK(false);}
     CHECK(s5.smokeTried&&s5.smokeLate&&s5.smokeUploadPtr&&w->resourcesCreated==res0+7&&w->heapsCreated==heaps0+6);
     for(const auto& sl:s5.slot)CHECK(sl.smokeRt[0]&&sl.smokeRt[1]&&sl.smokeRtv[0].ptr&&sl.smokeRtv[1].ptr);}
    {const auto text=hudLog();
     CHECK(text.find("gpu-copy-step1;event=fx-smoke-ring;outcome=ready;reason=none;slots=3;")!=std::string::npos&&
           text.find(";created=late(worker,option-after-ring)")!=std::string::npos&&text.find(";created=ring")==std::string::npos);}
    hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&!f.smoke_valid&&f.smoke_refused==1);seen=f.frame_id;src->release(f);
    CHECK(s5ref().smokeFrames==1&&s5ref().smokeFallback==1&&std::string(s5ref().smokeLastWhy)=="smoke-not-prepared");
    // (c) + (d)
    effects_smoke_gpu_flag().store(false);ticks(3);effects_smoke_gpu_flag().store(true);ticks(3);
    {const auto text=hudLog();const size_t at=text.find("fx-smoke-ring");CHECK(at!=std::string::npos&&text.find("fx-smoke-ring",at+1)==std::string::npos);}
    CHECK(w->resourcesCreated==res0+7&&w->heapsCreated==heaps0+6);
    effects_smoke_gpu_flag().store(false);effects_smoke_capture_flag().store(false);effects_smoke_replay_flag().store(false);
    step3Lines();
}
// EFFECTS-OUTSIDE (F) fxmem:6 (effects_sprite_gpu_flag; fx_sprite_gpu.h; docs section 11): (a) the per-eye sprite images + UPLOAD block exist after ring
// creation (fxmem:6 requested then, effects_dual on: the smoke set is created too) and the ring line says so; (b) a GREATER 4caf6e2d indexed draw (class 1)
// whose indices / vertices ARE read from the fake mapped upload memory but which the CPU plan cannot prepare (no world frame in this fixture) falls back to the window effects replay, identical
// commands, no sprite fields, a named fx-sprite-gpu refusal; (c) the recorder itself on the fake list: clears, predication bracket, clone PSO, per eye
// OMSet(sprite image, eye objects DSV) + IASetVertexBuffers(0, eye copy) + DrawIndexedInstanced (game arguments, base -lo), the game's VB view / PSO /
// targets back; (d) fxmem:5 (sprite GPU flag off) with the resources present: nothing of the GPU path runs; (e) the late creation path (option after the
// ring) is the smoke's, exercised by step5-effects-smoke-gpu-late; here: a second tick never creates a second set. A PREPARED draw cannot be reached in
// this fixture (no world frame): the glue from a prepared draw to the recorder is first exercised live (docs 11.6 check list).
static void step5_effects_sprite_gpu() {
    using namespace gpu_copy::step5;
    effects_smoke_capture_flag().store(true);effects_smoke_replay_flag().store(true);effects_smoke_gpu_flag().store(true);
    effects_explosion_flag().store(true);effects_sprite_gpu_flag().store(true);
    acz::g_effectsDualTest=true;effectsDualSetup(true);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);std::strcpy(s.pipelines.at(effPso('G')).hudPs.data(),"4caf6e2dc0ffee004caf6e2dc0ffee00");}
    auto* src=acz::stereo::frame_source();auto* gamePso=effPso('G');auto* clonePso=reinterpret_cast<ID3D12PipelineState*>(&effCloneObj);
    namespace g=acz::fxsprite::gpu;
    // (a) resources
    const unsigned res0=w->resourcesCreated;
    {const auto& s5=s5ref();
     if(!s5.spriteReady){std::fprintf(stderr,"sprite ring: %s\n",s5.spriteArmWhy?s5.spriteArmWhy:"?");CHECK(false);}
     CHECK(s5.smokeReady&&s5.spriteTried&&!s5.spriteLate&&s5.spriteUpload&&s5.spriteUploadPtr&&s5.spriteUploadVa%65536==0&&s5.spriteCharged>0&&s5.spriteUpload!=s5.smokeUpload);
     for(const auto& sl:s5.slot){CHECK(sl.spriteRt[0]&&sl.spriteRt[1]&&sl.spriteRt[0]!=sl.spriteRt[1]&&sl.spriteRtv[0].ptr&&sl.spriteRtv[1].ptr&&sl.spriteRtv[0].ptr!=sl.spriteRtv[1].ptr);
         for(int e=0;e<2;++e){CHECK(res(sl.spriteRt[e]).state==D3D12_RESOURCE_STATE_RENDER_TARGET&&res(sl.spriteRt[e]).format==DXGI_FORMAT(s5.hudFormat));
             CHECK(sl.spriteRt[e]!=sl.smokeRt[0]&&sl.spriteRt[e]!=sl.smokeRt[1]&&sl.spriteRt[e]!=sl.effRt);}}
     CHECK(res(s5.spriteUpload).heap==D3D12_HEAP_TYPE_UPLOAD&&res(s5.spriteUpload).width==g::kSlotStride*slots);}
    acz::stereo::StereoFrame f;uint64_t seen=0;
    // (b) one GREATER 4caf6e2d indexed draw in the committed epoch: not prepared -> the window replay exactly as with fxmem:5, counted as a fallback
    size_t b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
    CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&f.effects_draws==1);seen=f.frame_id;
    CHECK(!f.sprite_valid&&!f.sprite[0]&&!f.sprite[1]&&f.sprite_draws==0&&f.sprite_refused==1);
    CHECK(!f.smoke_valid&&f.smoke_refused==0); // not a smoke draw
    {const auto c=effCounts(b,f.slot);CHECK(c.replays==1&&c.predOn==1&&c.predOff==1);
     const auto& sl=s5ref().slot[f.slot];
     for(size_t i=b;i<w->lists[0].ops.size();++i){const Op& o=w->lists[0].ops[i];
         CHECK(o.t!=Op::vb);
         CHECK(!(o.t==Op::om&&(o.rtv[0]==sl.spriteRtv[0].ptr||o.rtv[0]==sl.spriteRtv[1].ptr))&&!(o.t==Op::clear_rtv&&(o.rtv[0]==sl.spriteRtv[0].ptr||o.rtv[0]==sl.spriteRtv[1].ptr)));}}
    src->release(f);
    {const auto& s5=s5ref();CHECK(s5.spriteFrames==1&&s5.spriteValidFrames==0&&s5.spriteReplays==0&&s5.spriteRefused==1&&s5.spriteFallback==1&&
        s5.spriteLastWhy&&std::string(s5.spriteLastWhy)=="sprite-not-prepared"&&s5.fxSprRead[1]==1&&s5.fxSprRefused[1]==0&&!s5.fxSprLastPrepared);
     // the draw WAS read from the fake mapped upload memory; the plan refused it (no world frame in this fixture), so the GPU gate says not-prepared
     CHECK(s5.fxSpr[1].draws==1&&s5.fxSpr[1].refused==1&&s5.fxSpr[1].prepared==0&&std::string(s5.fxSpr[1].last_why)=="sprite-world-no-object-record");}
    ticks(60);
    {const auto text=hudLog();
     for(const char* k:{"gpu-copy-step1;event=fx-sprite-ring;outcome=ready;reason=none;slots=3;",
             "caps=image:33554432,total:209715200,draws:256,vtx:512;per-slot=2x(sprite-rt)+upload:1048576;dsv=eye-objects-depth(test-only);per-draw=12(+2-clears-once);lifetime=process(ring);created=ring",
             "gpu-copy-step1;event=fx-sprite-gpu;frame=",";replays=0(explosion:0,sprite:0);committed=0;refused=1;window-fallback=1;cleared=0;valid=0;last-why=sprite-not-prepared;xpso=light:0,shared:none;mode=gpu-replay(",
             "gpu-copy-step1;event=fx-sprite-gpu-totals;frames=1;valid-frames=0;replays=0;committed=0;refused=1;window-fallback=1;last-why=sprite-not-prepared",
             "gpu-copy-step1;event=fx-sprite-plan;frame=",";class=sprite(tracer-cand);draws=1;read=1;prep=0;refused=1;",";last-why=sprite-world-no-object-record;",
             "gpu-copy-step1;event=fx-sprite-totals;class=sprite(tracer-cand);frames=1;read=1;refused=0;caps=idx:1536,vtx:512;mode=plan+gpu-replay(per-eye,fx-sprite-gpu-lines)"})
         if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
     CHECK(text.find("fx-sprite-ring",text.find("fx-sprite-ring")+1)==std::string::npos);}
    CHECK(w->resourcesCreated==res0); // (e) ticks never create a second set
    // (c) the recorder on the fake list (inside a frame's recording): the exact command sequence
    {const auto& s5=s5ref();const auto& sl=s5.slot[0];
     acz::Step5EffCall c;c.predRes=s5.effPred;c.predOffset=16;c.predOp=D3D12_PREDICATION_OP_EQUAL_ZERO;c.clone=clonePso;c.game=gamePso;c.indexed=true;
     c.count=6;c.instances=1;c.start=0;c.base=0;c.startInstance=0;c.omCount=1;c.omRtv[0].ptr=gameRtvHandle;c.omDsv=true;c.omDsvHandle.ptr=gameDsvHandle;
     acz::Step5SpriteCall s;s.clear=true;
     g::EyeViews v;CHECK(g::eye_views(s5.spriteUploadVa,3*g::replay_bytes(40),40,0,v)&&v.base==0&&v.bytes==40*64&&v.va[0]==s5.spriteUploadVa+g::offset(3*g::replay_bytes(40),40,0)&&v.va[1]==s5.spriteUploadVa+g::offset(3*g::replay_bytes(40),40,1)&&v.used==g::replay_bytes(40)); // (H) packed span cursor
     for(int e=0;e<2;++e){s.rtv[e]=sl.spriteRtv[e];s.dsv[e]=sl.dsv[e];s.eye[e]={v.va[e],v.bytes,v.stride};}
     const UINT64 gameVbVa=0x7123450000ull;s.game={gameVbVa,4096,64};s.base=v.base;
     unsigned sets0=0;size_t b2=0;
     hudFrame(0,[&]{hudBindScene(gamePso);sets0=w->lists[0].psoSets;b2=w->lists[0].ops.size();acz::step5RecordSprite(listAlias(0),c,s);
         CHECK(w->lists[0].curPso==gamePso&&w->lists[0].psoSets==sets0+2&&w->lists[0].vb==gameVbVa);hudOutput();});
     std::vector<Op> ops;for(size_t i=b2;i<w->lists[0].ops.size();++i){if(w->lists[0].ops[i].t==Op::om&&w->lists[0].ops[i].rtv[0]==hudOutputHandle)break;ops.push_back(w->lists[0].ops[i]);}
     if(ops.size()!=12){std::fprintf(stderr,"sprite recorder ops %zu\n",ops.size());CHECK(false);}   // = kCmdsPerDraw - 2 native PSO sets (not Ops) + 2 clears
     else {
        CHECK(ops[0].t==Op::clear_rtv&&ops[0].rtv[0]==sl.spriteRtv[0].ptr&&ops[1].t==Op::clear_rtv&&ops[1].rtv[0]==sl.spriteRtv[1].ptr);
        CHECK(ops[2].t==Op::pred&&ops[2].res==&res(s5.effPred).object&&ops[2].predOff==16);
        for(unsigned e=0;e<2;++e){const Op& om=ops[3+3*e];const Op& vb=ops[4+3*e];const Op& d=ops[5+3*e];
            CHECK(om.t==Op::om&&om.count==1&&om.rtv[0]==sl.spriteRtv[e].ptr&&om.dsvSet&&om.dsv==sl.dsv[e].ptr);
            CHECK(vb.t==Op::vb&&vb.vbStart==0&&vb.vbCount==1&&vb.va==v.va[e]&&vb.vbBytes==40*64&&vb.vbStride==64);
            CHECK(d.t==Op::draw&&d.indexed&&d.dc==6&&d.di==1&&d.ds==0&&d.db==0&&d.dsi==0&&d.boundRtv==sl.spriteRtv[e].ptr&&d.boundDsv==sl.dsv[e].ptr&&d.boundVb==v.va[e]&&d.boundPso==clonePso);}
        CHECK(ops[9].t==Op::vb&&ops[9].vbStart==0&&ops[9].vbCount==1&&ops[9].va==gameVbVa&&ops[9].vbBytes==4096&&ops[9].vbStride==64);
        CHECK(ops[10].t==Op::pred&&!ops[10].res);
        CHECK(ops[11].t==Op::om&&ops[11].count==1&&originOf(ops[11].rtv[0])==gameRtvHandle&&ops[11].dsvSet&&originOf(ops[11].dsv)==gameDsvHandle);
     }
     // (I) the explosion's light PSO (Step5SpriteCall::pso): the eye draws run under it, the game's PSO is bound back; the shared clone is not bound
     {static Object lightObj;lightObj.table=w->psoTable.data();lightObj.kind=k_pso;auto* lightPso=reinterpret_cast<ID3D12PipelineState*>(&lightObj);
      acz::Step5SpriteCall sl2=s;sl2.clear=false;sl2.pso=lightPso;size_t b3=0;unsigned sets1=0;
      hudFrame(0,[&]{hudBindScene(gamePso);sets1=w->lists[0].psoSets;b3=w->lists[0].ops.size();acz::step5RecordSprite(listAlias(0),c,sl2);
          CHECK(w->lists[0].curPso==gamePso&&w->lists[0].psoSets==sets1+2&&w->lists[0].vb==gameVbVa);hudOutput();});
      unsigned draws=0;for(size_t i=b3;i<w->lists[0].ops.size();++i){const Op& o=w->lists[0].ops[i];if(o.t==Op::om&&o.rtv[0]==hudOutputHandle)break;
          if(o.t==Op::draw){++draws;CHECK(o.boundPso==lightPso&&o.boundPso!=clonePso);}}
      CHECK(draws==2);}
    }
    CHECK(src->try_acquire_latest(seen,f));seen=f.frame_id;src->release(f);
    // (J, headset 2026-10-29 items 5/7) a NON-indexed GREATER 4caf6e2d quad (DrawInstanced(4, 1, 2, 0)) is class 1 with fxmem:6: captured and planned for the per-eye
    // replay (here the fixture has no world record, so the plan refuses and the window replay runs as the named fallback); before, it was never captured.
    {const auto& s5=s5ref();const uint32_t d0=s5.fxSpr[1].draws;(void)d0; /* fxSpr is reset per frame */const uint64_t fb0=s5.spriteFallback,fr0=s5.spriteFrames;
     b=hudFrame(0,[]{hudBindScene();auto* l=listAlias(0);effInjectRoot();l->SetPipelineState(effPso('G'));bindT10(srtOff);l->DrawInstanced(4,1,2,0);
         hudLateMember();hudBindScene();hudOutput();});
     CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&f.effects_draws==1);seen=f.frame_id;src->release(f);
     if(!(s5.fxSpr[1].draws==1&&s5.fxSpr[1].nonindexed==1)){std::fprintf(stderr,"non-indexed class 1: draws %u->%u nonidx %u why %s\n",d0,s5.fxSpr[1].draws,
         s5.fxSpr[1].nonindexed,s5.fxSpr[1].last_why?s5.fxSpr[1].last_why:"-");CHECK(false);}
     CHECK(s5.spriteFrames==fr0+1&&s5.spriteFallback==fb0+1&&effCounts(b,f.slot).replays==1);}
    // (K, headset 2026-10-30 m03k; docs section 17) a NON-indexed GREATER 4caf6e2d TRIANGLESTRIP of 6 vertices (not a 4-vertex quad list) is captured for
    // the per-eye replay too (one run, collapsed whole when a vertex is unusable); before (K) it was refused as `sprite-nonindexed-shape` and, accepted by the
    // effects layer, fell back to the WINDOW image as `sprite-not-prepared` (the user's m03k flight: 20,672 such refusals, 27 per dense frame).
    {const auto& s5=s5ref(); // (the fixture's members bind no topology: it is reset to UNDEFINED after the strip so the late member stays in its class)
     b=hudFrame(0,[]{hudBindScene();auto* l=listAlias(0);effInjectRoot();l->SetPipelineState(effPso('G'));bindT10(srtOff);
         l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);l->DrawInstanced(6,1,2,0);l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);hudLateMember();hudBindScene();hudOutput();});
     {const bool got=src->try_acquire_latest(seen,f);if(!(got&&f.effects_valid&&f.effects_draws==1)){std::fprintf(stderr,"(K) got %d valid %d draws %u refused %u why %s\n",int(got),int(got&&f.effects_valid),got?f.effects_draws:0u,got?f.effects_refused:0u,s5.effLastWhy?s5.effLastWhy:"-");std::fprintf(stderr,"(K) gate %s\n",s5.fxCenGate?s5.fxCenGate:"-");CHECK(false);}seen=f.frame_id;src->release(f);}
     const char* why=s5.fxSpr[1].last_why?s5.fxSpr[1].last_why:"-";
     std::fprintf(stderr,"  (K) non-indexed 6-vertex strip: draws %u nonidx %u read %u why %s\n",s5.fxSpr[1].draws,s5.fxSpr[1].nonindexed,s5.fxSpr[1].read,why);
     CHECK(s5.fxSpr[1].draws==1&&s5.fxSpr[1].nonindexed==1&&s5.fxSpr[1].read==1&&std::strcmp(why,"sprite-nonindexed-shape")!=0&&std::strcmp(why,"sprite-nonindexed-topology")!=0);}
    // (d) fxmem:5 (the sprite GPU flag off): the resources exist but nothing of the GPU path runs (no refusal, no GPU line); the window replay is unchanged
    effects_sprite_gpu_flag().store(false);
    {const uint64_t fr=s5ref().spriteFrames,fb=s5ref().spriteFallback;
     b=hudFrame(0,[]{hudBindScene();effDraws("G");hudLateMember();hudBindScene();hudOutput();});
     CHECK(src->try_acquire_latest(seen,f)&&f.effects_valid&&!f.sprite_valid&&f.sprite_refused==0);seen=f.frame_id;src->release(f);
     CHECK(s5ref().spriteFrames==fr&&s5ref().spriteFallback==fb&&effCounts(b,f.slot).replays==1);}
    effects_explosion_flag().store(false);effects_smoke_gpu_flag().store(false);effects_smoke_capture_flag().store(false);effects_smoke_replay_flag().store(false);
    step3Lines();
}
// ---- EYE-RING (eye_ring.h, docs/world-render/eye-resolution.md 5.1 (a)): the per-eye producer targets at the eye swapchain size.
// The fixture's game viewport is 256x144 (scissor 256x144); the "eye swapchain" is 320x320.
// Index of the first native RS call after the last Reset whose values match (scissor or viewport), from `from`; rsCount if none.
static unsigned rsFind(const ListSim& L,unsigned from,bool scissor,float wv,float hv) {
    for(unsigned i=from;i<L.rsCount;++i){const auto& r=L.rs[i];if(r.scissor!=scissor)continue;
        if(!scissor&&r.w==wv&&r.h==hv&&r.x==0&&r.y==0)return i;if(scissor&&r.l==0&&r.t==0&&r.r==LONG(wv)&&r.b==LONG(hv))return i;}
    return L.rsCount;
}
static unsigned rsCountOf(const ListSim& L,bool scissor,float wv,float hv){unsigned n=0;for(unsigned i=0;i<L.rsCount;++i){const auto& r=L.rs[i];
    if(r.scissor==scissor&&(scissor?(r.l==0&&r.t==0&&r.r==LONG(wv)&&r.b==LONG(hv)):(r.x==0&&r.y==0&&r.w==wv&&r.h==hv)))++n;}return n;}
// (a) eyeres + eyeobj requested and the eye extent published BEFORE the ring (vsearly): the six eye RT/DS are 320x320, the game-screen images stay at
// the game size; (b) every replayed member: the eye viewport/scissor (scaled 320/256 x 320/144 = the full eye) right before its two eye draws, the game's
// own viewport/scissor set back right after them (before the game's draw); the refused member (another viewport) records nothing; (c) the published frame
// says game 256x144, per-eye 320x320; (d) commands 4 + 12/member; (e) the eye-ring and summary-eye-ring lines.
static void step5_eye_ring() {
    namespace er=acz::eyering;
    er::reset_for_tests();er::request(true,true);er::publish_extent(320,320);
    bootContinuous();
    {const auto& s5=s5ref();
     CHECK(s5.eyeRing&&s5.ringW==320&&s5.ringH==320&&s5.gameW==256&&s5.gameH==144&&s5.perMember==12&&std::string(s5.eyeRingWhy)=="eye");
     CHECK(s5.commandCap==gpu_copy::step3::set_command_cap+4*s5.memberCap);
     for(const auto& sl:s5.slot)for(int e=0;e<2;++e)
         CHECK(res(sl.rt[e]).width==320&&res(sl.rt[e]).height==320&&res(sl.ds[e]).width==320&&res(sl.ds[e]).height==320);}
    {const auto text=hudLog();   // the eye-ring line is in the dedicated step5-producer log only
     for(const char* k:{"gpu-copy-step1;event=eye-ring;mode=eye;why=eye;game=256x144;per-eye=320x320;eye-extent=320x320;eyeobj=1;",
             "rs=scaled(eye/game)+restored-per-replay;","extra-commands-per-replay=4;fallback=eyeobj:0"})
         if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}}
    CHECK(line("event=ring;mode=continuous;"," 320x320 + DS "));
    contFrame();
    auto* src=acz::stereo::frame_source();
    CHECK(src->frames_published()==1&&w->rasterised==12);
    if(!has("event=continuous-first;frame=1;fence-value=1;members=6;commands=76;")){dump();CHECK(false);}   // 4 clears + 6 x (8 + 4 RS)
    {const auto& L=w->lists[0];
     // the game's own: bind3 (vp + sc 256x144), ordinal 6 (vp 128x72), ordinal 7 (vp 256x144); the producer: per member eye vp + sc, game vp + sc
     CHECK(rsCountOf(L,false,320,320)==6&&rsCountOf(L,true,320,320)==6);
     CHECK(rsCountOf(L,false,256,144)==2+6&&rsCountOf(L,true,256,144)==1+6&&rsCountOf(L,false,128,72)==1);
     unsigned i=0,members=0;
     while((i=rsFind(L,i,false,320,320))<L.rsCount) {
         CHECK(i+3<L.rsCount);
         const auto& ev=L.rs[i];const auto& es=L.rs[i+1];const auto& gv=L.rs[i+2];const auto& gs=L.rs[i+3];
         CHECK(es.scissor&&es.r==320&&es.b==320&&es.draws==ev.draws);                                   // eye scissor with the eye viewport
         CHECK(!gv.scissor&&gv.w==256&&gv.h==144&&gv.draws==ev.draws+2);                                 // the game's back after exactly the two eye draws
         CHECK(gs.scissor&&gs.r==256&&gs.b==144&&gs.draws==ev.draws+2);
         ++members;i+=4;
     }
     CHECK(members==6);
     CHECK(L.curVp.Width==256&&L.curVp.Height==144&&L.curSc.right==256&&L.curSc.bottom==144);   // the list ends with the game's own state
     unsigned proxyDraws=0;for(const auto& o:L.ops)if(o.t==Op::draw&&originOf(o.boundRtv)!=gameRtvHandle)++proxyDraws;
     CHECK(proxyDraws==12);}
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f));
     CHECK(f.width==256&&f.height==144&&f.eye_width==320&&f.eye_height==320&&f.members==6);
     CHECK(acz::stereo::eye_image_width(f)==320&&acz::stereo::eye_image_height(f)==320);
     CHECK(res(f.eye[0]).width==320&&res(f.depth[0]).height==320);src->release(f);}
    ticks(60);
    if(hudLog().find("gpu-copy-step1;event=summary-eye-ring;mode=eye;why=eye;per-eye=320x320;game=256x144;member-draws=6;fx-replays=0;vp-sub=6;scissor-sub=6;vp-restore=6;rs-refused=0;")==std::string::npos){
        dump();CHECK(false);}
    CHECK(line("event=continuous-quality;","last-member-refusal=viewport-size-changed")); // the GAME size decides membership, as before
    er::reset_for_tests();
    step3Lines();
}
// The fallback eyeres:100;eyeobj:0 (and the no-extent case): the ring stays at the game size, no RS call is added, the frame carries no eye size, the
// commands are the old ones; the eye-ring line and the summary say why.
static void step5_eye_ring_fallback(bool objects) {
    namespace er=acz::eyering;
    er::reset_for_tests();er::request(true,objects);if(!objects)er::publish_extent(320,320);   // objects on + no extent = the ring before the swapchains
    bootContinuous();
    {const auto& s5=s5ref();
     CHECK(!s5.eyeRing&&s5.ringW==256&&s5.ringH==144&&s5.gameW==256&&s5.perMember==8&&s5.commandCap==gpu_copy::step3::set_command_cap);
     for(const auto& sl:s5.slot)for(int e=0;e<2;++e)CHECK(res(sl.rt[e]).width==256&&res(sl.rt[e]).height==144);}
    const char* want=objects?"gpu-copy-step1;event=eye-ring;mode=game-size;why=no-eye-extent;game=256x144;per-eye=256x144;eye-extent=0x0;eyeobj=1;"
                            :"gpu-copy-step1;event=eye-ring;mode=game-size;why=eyeobj-off;game=256x144;per-eye=256x144;eye-extent=320x320;eyeobj=0;";
    if(hudLog().find(want)==std::string::npos){dump();CHECK(false);}
    contFrame();
    CHECK(acz::stereo::frame_source()->frames_published()==1&&w->rasterised==12);
    CHECK(has("event=continuous-first;frame=1;fence-value=1;members=6;commands=52;"));   // the old count
    {const auto& L=w->lists[0];CHECK(L.rsCount==4&&rsCountOf(L,false,320,320)==0);}      // the game's own RS calls only
    {acz::stereo::StereoFrame f;auto* src=acz::stereo::frame_source();CHECK(src->try_acquire_latest(0,f));
     CHECK(f.width==256&&f.height==144&&f.eye_width==0&&f.eye_height==0&&acz::stereo::eye_image_width(f)==256);src->release(f);}
    ticks(60);
    CHECK(hudLog().find(objects?"event=summary-eye-ring;mode=game-size;why=no-eye-extent;per-eye=256x144;game=256x144;member-draws=0;fx-replays=0;vp-sub=0;"
                               :"event=summary-eye-ring;mode=game-size;why=eyeobj-off;per-eye=256x144;game=256x144;member-draws=0;fx-replays=0;vp-sub=0;")!=std::string::npos);
    er::reset_for_tests();
    step3Lines();
}
static void step5_eye_ring_eyeobj_off(){step5_eye_ring_fallback(false);}
static void step5_eye_ring_no_extent(){step5_eye_ring_fallback(true);}
// The per-eye smoke / sprite images on the eye ring: created at the eye size under the eye caps (the window effects image stays at the game size), and
// the recorders bracket their two eye draws with the eye viewport/scissor and set the game's back before the game's own targets (Step5EyeRs).
static void step5_eye_ring_fx() {
    using namespace gpu_copy::step5;namespace er=acz::eyering;
    er::reset_for_tests();er::request(true,true);er::publish_extent(320,320);
    effects_smoke_capture_flag().store(true);effects_smoke_replay_flag().store(true);effects_smoke_gpu_flag().store(true);
    effects_explosion_flag().store(true);effects_sprite_gpu_flag().store(true);
    acz::g_effectsDualTest=true;effectsDualSetup(true);
    auto* gamePso=effPso('G');auto* clonePso=reinterpret_cast<ID3D12PipelineState*>(&effCloneObj);
    {const auto& s5=s5ref();
     if(!s5.smokeReady||!s5.spriteReady){std::fprintf(stderr,"eye fx: smoke %s sprite %s\n",s5.smokeArmWhy?s5.smokeArmWhy:"?",s5.spriteArmWhy?s5.spriteArmWhy:"?");CHECK(false);}
     CHECK(s5.eyeRing&&s5.ringW==320&&s5.ringH==320&&s5.effReady&&s5.hudReady);
     for(const auto& sl:s5.slot){
         for(int e=0;e<2;++e)CHECK(res(sl.smokeRt[e]).width==320&&res(sl.smokeRt[e]).height==320&&res(sl.spriteRt[e]).width==320&&res(sl.spriteRt[e]).height==320);
         CHECK(res(sl.effRt).width==256&&res(sl.effRt).height==144&&res(sl.hudRt).width==256&&res(sl.world).width==256);}}   // game-screen images: game size
    ticks(1);
    {const auto text=hudLog();CHECK(text.find("event=fx-smoke-ring;outcome=ready;")!=std::string::npos&&text.find("event=fx-sprite-ring;outcome=ready;")!=std::string::npos&&
        text.find("event=eye-ring;mode=eye;why=eye;")!=std::string::npos);}
    // the recorders with an eye-ring RS plan (what step5EyeRsPlan builds for a full game viewport / scissor)
    const auto& s5=s5ref();const auto& sl=s5.slot[0];
    acz::Step5EyeRs rs;rs.on=true;rs.eyeVp={0,0,320,320,0,1};rs.eyeSc={0,0,320,320};rs.gameVp={0,0,256,144,0,1};rs.gameSc={0,0,256,144};
    {gpu_copy::step3::Shape d{};d.rs_known=true;d.viewports=1;d.scissors=1;d.vp0={0,0,256,144,0,1};d.sc0={0,0,256,144};
     acz::Step5EyeRs p;CHECK(acz::step5EyeRsPlan(s5,d,p)==nullptr&&p.on&&p.eyeVp.Width==320.0f&&p.eyeVp.Height==320.0f&&p.eyeSc.right==320&&p.eyeSc.bottom==320&&
         p.gameVp.Width==256.0f&&p.gameSc.bottom==144);
     d.scissors=0;CHECK(std::string(acz::step5EyeRsPlan(s5,d,p))=="eye-ring-scissor-count"&&!p.on);}
    acz::Step5EffCall c;c.predRes=s5.effPred;c.predOffset=8;c.predOp=D3D12_PREDICATION_OP_EQUAL_ZERO;c.clone=clonePso;c.game=gamePso;
    c.count=3;c.instances=1;c.start=2;c.startInstance=0;c.omCount=1;c.omRtv[0].ptr=gameRtvHandle;c.omDsv=true;c.omDsvHandle.ptr=gameDsvHandle;
    auto bracket=[&](unsigned rs0,unsigned draws0){
        const auto& L=w->lists[0];
        CHECK(L.rsCount==rs0+4);
        const auto& ev=L.rs[rs0];const auto& es=L.rs[rs0+1];const auto& gv=L.rs[rs0+2];const auto& gs=L.rs[rs0+3];
        CHECK(!ev.scissor&&ev.w==320&&ev.h==320&&ev.draws==draws0&&es.scissor&&es.r==320&&es.b==320&&es.draws==draws0);
        CHECK(!gv.scissor&&gv.w==256&&gv.h==144&&gv.draws==draws0+2&&gs.scissor&&gs.r==256&&gs.b==144&&gs.draws==draws0+2);
        CHECK(L.curVp.Width==256&&L.curSc.bottom==144);
    };
    {acz::Step5SmokeCall s;s.clear=false;s.rs=rs;for(int e=0;e<2;++e){s.rtv[e]=sl.smokeRtv[e];s.dsv[e]=sl.dsv[e];}
     CHECK(acz::fxsmoke::plan_root_swap(3,true,gameVa(2)+srtOff,s5.smokeUploadVa,acz::fxsmoke::gpu::kSlotUpload,4,s.swap)==nullptr);
     hudFrame(0,[&]{hudBindScene(gamePso);const unsigned rs0=w->lists[0].rsCount,d0=w->lists[0].draws;acz::step5RecordSmoke(listAlias(0),c,s);bracket(rs0,d0);hudOutput();});}
    {acz::Step5SpriteCall s;s.clear=false;s.rs=rs;c.indexed=true;c.count=6;c.start=0;
     namespace g=acz::fxsprite::gpu;g::EyeViews v;CHECK(g::eye_views(s5.spriteUploadVa,0,40,0,v));
     for(int e=0;e<2;++e){s.rtv[e]=sl.spriteRtv[e];s.dsv[e]=sl.dsv[e];s.eye[e]={v.va[e],v.bytes,v.stride};}
     s.game={0x7123450000ull,4096,64};s.base=v.base;
     hudFrame(0,[&]{hudBindScene(gamePso);const unsigned rs0=w->lists[0].rsCount,d0=w->lists[0].draws;acz::step5RecordSprite(listAlias(0),c,s);bracket(rs0,d0);hudOutput();});}
    // the same recorders with rs off (every frame without the eye ring): no RS call at all
    {acz::Step5SmokeCall s;s.clear=false;for(int e=0;e<2;++e){s.rtv[e]=sl.smokeRtv[e];s.dsv[e]=sl.dsv[e];}c.indexed=false;c.count=3;c.start=2;
     CHECK(acz::fxsmoke::plan_root_swap(3,true,gameVa(2)+srtOff,s5.smokeUploadVa,acz::fxsmoke::gpu::kSlotUpload,4,s.swap)==nullptr);
     hudFrame(0,[&]{hudBindScene(gamePso);const unsigned rs0=w->lists[0].rsCount;acz::step5RecordSmoke(listAlias(0),c,s);CHECK(w->lists[0].rsCount==rs0);hudOutput();});}
    effects_explosion_flag().store(false);effects_sprite_gpu_flag().store(false);effects_smoke_gpu_flag().store(false);effects_smoke_capture_flag().store(false);
    effects_smoke_replay_flag().store(false);er::reset_for_tests();
    step3Lines();
}
static void step5_effects_create_refused() {
    // The 2nd slot's effects target cannot be created: refused cleanly (no predicate buffer), the HUD runs exactly as without the key.
    g_failResourceAt=27; // 4 + 12 + 1 + 3 world + 6 HUD = 26 before; effects slot 0 = #26, slot 1 = #27 fails
    effectsSetup(true);g_failResourceAt=UINT_MAX;auto* src=acz::stereo::frame_source();
    {auto& s5=s5ref();
     CHECK(s5.hudReady&&!s5.effReady&&s5.effArmWhy&&std::string(s5.effArmWhy)=="effects-rt-create-failed"&&s5.effCharged==0&&!s5.effPred&&!s5.effPredPtr);
     CHECK(w->resourcesCreated==27&&w->heapsCreated==14+6+1);}
    const size_t b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();effDraws("G");hudOutput();});
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f));
    CHECK(f.hud_valid&&f.hud_draws==1&&!f.effects&&!f.effects_valid&&f.effects_draws==0&&f.effects_tail==0);src->release(f);
    for(size_t i=b;i<w->lists[0].ops.size();++i)CHECK(w->lists[0].ops[i].t!=Op::pred);
    {const auto& s5=s5ref();CHECK(s5.effValidFrames==0&&s5.effUnarmedFrames==1&&s5.effSpeculative==0);}
    ticks(60);const auto text=hudLog();
    for(const char* k:{"|gpu-copy-step1;event=effects-ring;outcome=refused;reason=effects-rt-create-failed;slots=3;","charged=0;","fallback=key-off",
            "event=continuous-hud-effects;ready=0;frames=valid:0,tail-only:0,absent:0,unarmed:1;","arm=effects-rt-create-failed"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    step3Lines();
}
static void step5_effects_pred_refused() {
    // Only the predicate buffer cannot be created (the 3 targets exist): refused as a whole, nothing recorded.
    g_failResourceAt=29;
    effectsSetup(true);g_failResourceAt=UINT_MAX;auto* src=acz::stereo::frame_source();
    {auto& s5=s5ref();CHECK(s5.hudReady&&!s5.effReady&&s5.effArmWhy&&std::string(s5.effArmWhy)=="effects-predicate-create-failed"&&!s5.effPredPtr);
     CHECK(w->resourcesCreated==29&&w->heapsCreated==14+6+3);}
    const size_t b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();hudOutput();});
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&!f.effects&&!f.effects_valid);src->release(f);
    for(size_t i=b;i<w->lists[0].ops.size();++i)CHECK(w->lists[0].ops[i].t!=Op::pred);
    ticks(60);const auto text=hudLog();
    CHECK(text.find("event=effects-ring;outcome=refused;reason=effects-predicate-create-failed;")!=std::string::npos);
    step3Lines();
}

// ------------------------------------------------------------- J-hud-colour (gpu_copy_step5_png_once)
// A minimal decoder for the encoder's own output (8-bit RGBA, one IDAT of STORED deflate blocks,
// filter 0 rows): signature, IHDR fields, chunk CRCs, zlib header and Adler-32 all checked.
static bool decodeStoredPng(const std::vector<uint8_t>& f,uint32_t& w,uint32_t& h,std::vector<uint8_t>& rgba) {
    auto be=[&](size_t at){return uint32_t(f[at])<<24|uint32_t(f[at+1])<<16|uint32_t(f[at+2])<<8|uint32_t(f[at+3]);};
    static const uint8_t sig[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    if(f.size()<8||std::memcmp(f.data(),sig,8))return false;
    std::vector<uint8_t> z;size_t at=8;bool ihdr=false,end=false;
    while(at+12<=f.size()&&!end) {
        const uint32_t n=be(at);const std::string type(reinterpret_cast<const char*>(&f[at+4]),4);
        if(at+12+n>f.size())return false;
        if(gpu_copy::step3::crc32(&f[at+4],n+4)!=be(at+8+n))return false;
        const uint8_t* d=&f[at+8];
        if(type=="IHDR"){if(n!=13||d[8]!=8||d[9]!=6||d[10]||d[11]||d[12])return false;w=be(at+8);h=be(at+12);ihdr=true;}
        else if(type=="IDAT")z.insert(z.end(),d,d+n);
        else if(type=="IEND")end=true;
        at+=12+n;
    }
    if(!ihdr||!end||z.size()<6||z[0]!=0x78||z[1]!=0x01)return false;
    std::vector<uint8_t> raw;size_t p=2;bool last=false;
    while(!last) {
        if(p+5>z.size())return false;
        last=z[p]&1;if((z[p]>>1)&3)return false; // stored blocks only
        const uint16_t len=uint16_t(z[p+1]|z[p+2]<<8),nlen=uint16_t(z[p+3]|z[p+4]<<8);
        if(uint16_t(~len)!=nlen||p+5+len>z.size())return false;
        raw.insert(raw.end(),z.begin()+std::ptrdiff_t(p+5),z.begin()+std::ptrdiff_t(p+5+len));p+=5+len;
    }
    if(p+4!=z.size()||gpu_copy::step3::adler32(raw.data(),raw.size())!=(uint32_t(z[p])<<24|uint32_t(z[p+1])<<16|uint32_t(z[p+2])<<8|z[p+3]))return false;
    if(raw.size()!=size_t(h)*(size_t(w)*4+1))return false;
    rgba.clear();
    for(uint32_t y=0;y<h;++y){const uint8_t* row=&raw[size_t(y)*(size_t(w)*4+1)];if(row[0])return false;rgba.insert(rgba.end(),row+1,row+1+size_t(w)*4);}
    return true;
}
static uint16_t toHalf(float v) { // exact for the values used here (0, 0.25, 0.5, 0.75, 1, 2, -1)
    if(v==0.0f)return 0;uint32_t b;std::memcpy(&b,&v,4);
    const uint32_t sign=(b>>16)&0x8000;const int e=int((b>>23)&0xff)-127+15;return uint16_t(sign|uint32_t(e)<<10|((b>>13)&0x3ff));
}
static void putTexel(std::vector<uint8_t>& img,size_t i,float r,float g,float b,float a) {
    const uint16_t hv[4]={toHalf(r),toHalf(g),toHalf(b),toHalf(a)};std::memcpy(&img[i*8],hv,8);
}
static std::vector<uint8_t> readAll(const std::wstring& path) {
    std::vector<uint8_t> out;HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return out;
    uint8_t buf[65536];DWORD n=0;while(ReadFile(h,buf,sizeof buf,&n,nullptr)&&n)out.insert(out.end(),buf,buf+n);CloseHandle(h);return out;
}
static void step5_png_rules() {
    namespace png=gpu_copy::png;namespace s3=gpu_copy::step3;
    // Footprints (D3D12: row pitch multiple of 256, offsets multiple of 512): the live 2560x1440 FP16 image.
    CHECK(png::row_pitch(2560)==20480&&png::footprint_stride(2560,1440)==29491200&&png::readback_bytes(2560,1440)==88473600);
    CHECK(png::row_pitch(257)==2304&&png::footprint_stride(257,3)%512==0&&png::footprint_stride(257,3)>=2304*3);
    CHECK(png::commands_per_sample==9&&png::samples==2&&png::image_count==3&&png::out_count==5);
    CHECK(std::string(png::image_name(0))=="hud"&&std::string(png::image_name(2))=="eye0"&&std::string(png::output_name(2))=="hud-over-world");
    // Known pixels, 2x1, CHANNEL ORDER: every source texel is R,G,B,A halves; every PNG is R,G,B,A bytes.
    const uint32_t W=2,H=1;const uint64_t pitch=png::row_pitch(W);
    std::vector<uint8_t> hud(static_cast<size_t>(pitch)),world(static_cast<size_t>(pitch)),eye(static_cast<size_t>(pitch));
    putTexel(hud,0,1.0f,0.0f,0.0f,1.0f);putTexel(hud,1,0.0f,0.5f,0.0f,0.25f);   // red opaque; dim green, alpha 0.25
    putTexel(world,0,0.0f,0.0f,1.0f,1.0f);putTexel(world,1,0.25f,0.25f,0.25f,1.0f);
    putTexel(eye,0,0.0f,1.0f,0.0f,1.0f);putTexel(eye,1,2.0f,1.0f,-1.0f,0.0f);   // overbright / negative: clamp
    const uint8_t* src[3]={hud.data(),world.data(),eye.data()};
    const uint8_t g5=s3::srgb8(0.5f),q=s3::srgb8(0.125f),g625=s3::srgb8(0.625f),w25=s3::srgb8(0.25f);
    CHECK(g5==188&&w25==137&&s3::srgb8(1.0f)==255&&s3::srgb8(0.0f)==0);
    const std::vector<std::vector<uint8_t>> want={
        {255,0,0,255, 0,g5,0,255},                 // hud-on-black: premultiplied HUD over black, sRGB
        {255,255,255,255, 64,64,64,255},           // hud-alpha-raw: stored alpha 1.0 and 0.25 (linear)
        {255,0,0,255, q,g625,q,255},               // hud-over-world: a=max(0.25,0.5)=0.5 -> (0,.5,0)+(.25,.25,.25)*.5
        {0,0,255,255, w25,w25,w25,255},            // world
        {0,255,0,255, 255,255,0,255}};             // eye0 (clamp 2->1, -1->0)
    for(unsigned o=0;o<png::out_count;++o) {
        std::vector<uint8_t> rgba,file,back;uint32_t dw=0,dh=0;
        CHECK(png::build(src,W,H,pitch,o,rgba));
        if(rgba!=want[o]){std::fprintf(stderr,"output %s:",png::output_name(o));for(auto b:rgba)std::fprintf(stderr," %u",b);std::fprintf(stderr,"\n");CHECK(false);}
        CHECK(s3::png_encode(rgba.data(),W,H,uint64_t(W)*4,false,file)&&decodeStoredPng(file,dw,dh,back)&&dw==W&&dh==H&&back==want[o]);
    }
    {std::vector<uint8_t> unused;CHECK(!png::build(src,W,H,pitch,png::out_count,unused)&&!png::build(src,W,H,7,0,unused));}
    // The HUD shading mirror (exact with the consumer, cross-checked in xr_stereo_output_offline_tests).
    {const float in[4]={0.2f,0.9f,0.1f,0.0f};float o[4];png::shade_hud(in,0,1.0f,o);CHECK(o[0]==0.2f&&o[1]==0.9f&&o[2]==0.1f&&o[3]==0.9f);}
    {const float in[4]={0.0f,0.0f,0.0f,0.7f};float o[4];png::shade_hud(in,0,1.0f,o);CHECK(o[0]==0&&o[3]==0.7f);} // dark panel keeps its alpha
    {const float nan=std::numeric_limits<float>::quiet_NaN();const float in[4]={nan,3.0f,-2.0f,nan};float o[4];png::shade_hud(in,0,1.0f,o);CHECK(o[0]==0&&o[1]==1&&o[2]==0&&o[3]==1);}
    // Raw statistics: every texel, finite values only, alpha > 0 and lit fractions.
    {std::vector<uint8_t> img(size_t(png::row_pitch(3)));putTexel(img,0,1,0,0,1);putTexel(img,1,0,0,0,0);
     const uint16_t nan[4]={0x7e00,0,0,0x3c00};std::memcpy(&img[16],nan,8);
     png::Stats s;png::stats(img.data(),3,1,png::row_pitch(3),s);
     CHECK(s.texels==3&&s.nonfinite==1&&s.alpha_positive==2&&s.lit==1&&s.max[0]==1.0&&s.min[0]==0.0&&s.mean[0]==0.5&&s.mean[3]==2.0/3.0);}
    // Config: default OFF, boolean forms, rejected values/duplicates; independent of the HUD keys.
    using namespace acz::diagnostics;
    CHECK(!Config{}.gpu_copy_step5_png_once()&&!parse_config("").config.gpu_copy_step5_png_once());
    for(const auto* v:{"true","1"}){auto c=parse_config(std::string("gpu_copy_step5_png_once=")+v);CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_png_once()&&!c.config.gpu_copy_step5_hud());}
    for(const auto* v:{"false","0"}){auto c=parse_config(std::string("gpu_copy_step5_png_once=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.gpu_copy_step5_png_once());}
    CHECK(parse_config("gpu_copy_step5_png_once=yes").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_png_once=1\ngpu_copy_step5_png_once=1").issue==ConfigIssue::duplicate);
    CHECK(!parse_config("gpu_copy_step5_hud=1").config.gpu_copy_step5_png_once());
}
// Live-like formats (FP16 scene, D32S8 depth), HUD capture on, png_once per `key`. The game's
// scene target (the world snapshot source) holds the known colour (0.25, 0.5, 1.0, 1.0).
static void pngSetup(bool key) {
    shape3.rtv=DXGI_FORMAT_R16G16B16A16_FLOAT;shape3.dsv=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    acz::g_pngOnceTest=key;hudSetup(true);
    auto& g=gameRt().big;for(size_t i=0;i+8<=g.size();i+=8)putTexel(g,i/8,0.25f,0.5f,1.0f,1.0f);
}
static std::vector<Op> opsFrom(size_t begin){return std::vector<Op>(w->lists[0].ops.begin()+std::ptrdiff_t(begin),w->lists[0].ops.end());}
static unsigned pngCopies(const std::vector<Op>& ops){unsigned n=0;for(const auto& o:ops)n+=o.t==Op::copy_tex&&!o.subres;return n;}
static std::wstring pngOutDir(){return pngDir+L"\\png";}
static unsigned pngFiles() {
    WIN32_FIND_DATAW d;HANDLE h=FindFirstFileW((pngOutDir()+L"\\acz-vr-png-*.png").c_str(),&d);if(h==INVALID_HANDLE_VALUE)return 0;
    unsigned n=0;do ++n;while(FindNextFileW(h,&d));FindClose(h);return n;
}
static void step5_png_off() {
    // Key OFF (default): no readback resource (bootContinuous counts the resources), no Close-seam call,
    // no copy, no png line; raising the flag after arming (nothing armed) leaves the stream op for op.
    pngSetup(false);
    CHECK(!acz::g_step5PngWanted.load()&&!s5ref().png.readback&&s5ref().png.stage.load()==acz::png_off);
    const size_t b1=hudFrame(4);const auto off=opsFrom(b1);
    CHECK(pngCopies(off)==0); // (the world snapshot's own sub-resource copy is not a readback copy)
    acz::g_pngOnceTest=true;const size_t b2=hudFrame(4);const auto on=opsFrom(b2);acz::g_pngOnceTest=false;
    // (the two frames use different ring slots, so the HUD/eye handles differ: same ops, same kinds)
    CHECK(on.size()==off.size());for(size_t i=0;i<off.size();++i)CHECK(on[i].t==off[i].t);
    CHECK(pngCopies(on)==0&&!acz::g_step5PngWanted.load()&&s5ref().png.stage.load()==acz::png_off);
    ticks(60);const auto text=hudLog();
    CHECK(text.find("png-once")==std::string::npos&&text.find("png-stats")==std::string::npos&&text.find("-png;")==std::string::npos);
    CHECK(pngFiles()==0);
    step3Lines();
}
static void step5_png() {
    namespace png=gpu_copy::png;namespace s3=gpu_copy::step3;
    pngSetup(true);
    auto& p=s5ref().png;auto* src=acz::stereo::frame_source();
    CHECK(p.readback&&p.stage.load()==acz::png_armed&&acz::g_step5PngWanted.load());
    auto& rb=res(p.readback);CHECK(rb.heap==D3D12_HEAP_TYPE_READBACK&&rb.state==D3D12_RESOURCE_STATE_COPY_DEST&&rb.width==png::readback_bytes(256,144));
    // Frame 1: the sample. The ONLY difference from an unsampled frame is 9 trailing commands at the
    // list's Close: per image Barrier(RT->COPY_SOURCE), Copy(readback footprint i), Barrier(back).
    const size_t b1=hudFrame(4);const auto f1=opsFrom(b1);
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.hud_valid&&f.world_valid);
    const auto& sl=s5ref().slot[f.slot];
    CHECK(f1.size()>=9);
    const Object* want[3]={&res(sl.hudRt).object,&res(sl.world).object,&res(sl.rt[0]).object};
    for(unsigned i=0;i<3;++i) {
        const Op& a=f1[f1.size()-9+3*i];const Op& c=f1[f1.size()-8+3*i];const Op& z=f1[f1.size()-7+3*i];
        CHECK(a.t==Op::barrier&&a.res==want[i]&&a.before==D3D12_RESOURCE_STATE_RENDER_TARGET&&a.after==D3D12_RESOURCE_STATE_COPY_SOURCE);
        CHECK(c.t==Op::copy_tex&&!c.subres&&c.res==want[i]&&c.dst==&rb.object&&c.offset==uint64_t(i)*p.stride&&c.pitch==p.pitch&&
              c.w==256&&c.h==144&&c.format==UINT(DXGI_FORMAT_R16G16B16A16_FLOAT)&&c.box&&c.bl==0&&c.bt==0);
        CHECK(z.t==Op::barrier&&z.res==want[i]&&z.before==D3D12_RESOURCE_STATE_COPY_SOURCE&&z.after==D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    CHECK(pngCopies(f1)==3);
    for(const auto* o:want)CHECK(res(const_cast<Object*>(o)).state==D3D12_RESOURCE_STATE_RENDER_TARGET); // restored after the GPU ran it
    CHECK(rb.maps==0); // nothing mapped on the game thread or before completion
    // The HUD/eye images the readback must hold (the slot is held: unchanged until released).
    const std::vector<uint8_t> hudBytes=res(sl.hudRt).big,eyeBytes=res(sl.rt[0]).big;
    src->release(f);
    // Frame 2: no sample armed yet (the first one is still being read): identical stream minus the 9 commands.
    const size_t b2=hudFrame(4);const auto f2=opsFrom(b2);
    if(!(f2.size()+9==f1.size())){std::fprintf(stderr,"f1 %zu f2 %zu\n",f1.size(),f2.size());CHECK(false);}
    for(size_t i=0;i<f2.size();++i)CHECK(f2[i].t==f1[i].t);
    CHECK(pngCopies(f2)==0);
    ticks(2);CHECK(rb.maps==1&&rb.unmaps==1&&rb.lastReadBegin==0&&rb.lastReadEnd==rb.width&&rb.lastWrittenBegin==0&&rb.lastWrittenEnd==0);
    CHECK(p.read==1&&p.stage.load()==acz::png_armed&&p.notBefore>=p.publishedAt+png::second_gap_frames);
    CHECK(pngFiles()==5);
    // The written PNGs: decode and compare with the images at copy time (row 0 = top, R,G,B,A).
    auto load=[&](const char* output,std::vector<uint8_t>& rgba) {
        WIN32_FIND_DATAW d;const std::wstring pat=pngOutDir()+L"\\acz-vr-png-*-s1-"+std::wstring(output,output+std::strlen(output))+L".png";
        HANDLE h=FindFirstFileW(pat.c_str(),&d);CHECK(h!=INVALID_HANDLE_VALUE);FindClose(h);
        uint32_t dw=0,dh=0;CHECK(decodeStoredPng(readAll(pngOutDir()+L"\\"+d.cFileName),dw,dh,rgba)&&dw==256&&dh==144);
    };
    std::vector<uint8_t> img;unsigned lit=0,litWant=0;
    const uint8_t r75=s3::srgb8(0.75f),g5=s3::srgb8(0.5f),b25=s3::srgb8(0.25f);
    load("hud-on-black",img);
    for(size_t i=0;i<size_t(256)*144;++i) {
        uint16_t hv[4];std::memcpy(hv,&hudBytes[i*8],8);const bool covered=hv[0]==0x3a00; // the fake rasteriser's (0.75,0.5,0.25,1)
        litWant+=covered;
        const uint8_t* q=&img[i*4];
        if(covered){CHECK(q[0]==r75&&q[1]==g5&&q[2]==b25&&q[3]==255);++lit;}
        else CHECK(hv[0]==0&&hv[3]==0&&q[0]==0&&q[1]==0&&q[2]==0&&q[3]==255);
    }
    CHECK(lit==litWant&&lit>100);
    load("world",img);for(size_t i=0;i<img.size();i+=4)CHECK(img[i]==s3::srgb8(0.25f)&&img[i+1]==g5&&img[i+2]==255&&img[i+3]==255);
    load("hud-alpha-raw",img);{unsigned white=0;for(size_t i=0;i<img.size();i+=4)white+=img[i]==255;CHECK(white==litWant);}
    load("hud-over-world",img); // covered: HUD alpha 1 hides the world; elsewhere the world shows
    {unsigned hudPx=0,worldPx=0;for(size_t i=0;i<img.size();i+=4){hudPx+=img[i]==r75&&img[i+1]==g5&&img[i+2]==b25;worldPx+=img[i+2]==255;}
     CHECK(hudPx==litWant&&worldPx==256u*144u-litWant);}
    load("eye0",img);{unsigned px=0;for(size_t i=0;i<img.size();i+=4)px+=img[i]==r75&&img[i+1]==g5&&img[i+2]==b25;
     unsigned eyeLit=0;for(size_t i=0;i<size_t(256)*144;++i){uint16_t h0;std::memcpy(&h0,&eyeBytes[i*8],2);eyeLit+=h0==0x3a00;}CHECK(px==eyeLit&&eyeLit>100);}
    // Frames until the second sample (>= 10 published frames after the first was read); one more sample, then done.
    unsigned sampled=0,frames=0;
    while(p.stage.load()==acz::png_armed&&frames<30){++frames;const size_t b=hudFrame(4);sampled+=pngCopies(opsFrom(b))?1:0;}
    CHECK(sampled==1&&frames>=9);
    ticks(3);CHECK(p.read==2&&p.stage.load()==acz::png_done&&!acz::g_step5PngWanted.load()&&rb.maps==2&&pngFiles()==10);
    for(unsigned k=0;k<5;++k){const size_t b=hudFrame(4);CHECK(pngCopies(opsFrom(b))==0);} // never again in this process
    ticks(60);const auto text=hudLog();
    for(const char* k:{"|gpu-copy-step1;event=png-once;outcome=armed;images=hud,world,eye0;format=10;size=256x144;pitch=2048;",
            "commands=9-at-close;outputs=hud-on-black,hud-alpha-raw,hud-over-world,world,eye0;",
            "event=png-stats;sample=1;image=hud;size=256x144;format=10(R16G16B16A16_FLOAT,RGBA);texels=36864;nonfinite=0;",
            "event=png-stats;sample=1;image=world;size=256x144;format=10(R16G16B16A16_FLOAT,RGBA);texels=36864;nonfinite=0;r=0.25/0.25/0.25;g=0.5/0.5/0.5;b=1/1/1;a=1/1/1(min/max/mean,finite);alpha-gt0=1.00000;rgb-gt0=1.00000",
            "event=png-file;sample=1;output=hud-over-world;","ok=1;","event=png-once;outcome=read;sample=1;fence-value=1;","files=5/5;",
            "event=png-once;outcome=read;sample=2;","event=png-once;outcome=done;samples=2;",
            "event=continuous-png;key=gpu_copy_step5_png_once;stage=done;read=2/2;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    for(const auto& l:w->lines)CHECK(l.find("png-once")==std::string::npos&&l.find("png-stats")==std::string::npos&&l.find("png-file")==std::string::npos); // dedicated log only
    step3Lines();
}
static void step5_png_faults() {
    pngSetup(true);auto& p=s5ref().png;
    // (a) a frame whose HUD replay is refused is not sampled (waited, still armed).
    hudPipeline(hudPso2(),[](auto& o){o.front_func=gpu_copy::step3::cmp::less;});
    size_t b=hudFrame(0,[]{hudBindScene();hudDraws(4,nullptr,2);hudOutput();});
    CHECK(pngCopies(opsFrom(b))==0&&p.stage.load()==acz::png_armed&&p.waitedFrames==1&&std::string(p.lastWait)=="hud-invalid");
    // (b) Reset before submission: the recorded copies never execute -> armed again; the next frame samples.
    frameSet();hudSetRoot();hudBindScene();hudDraws(2);hudOutput();CHECK(listAlias(0)->Close()==S_OK);
    CHECK(p.stage.load()==acz::png_recorded&&!acz::g_step5PngWanted.load());
    CHECK(listAlias(0)->Reset(allocator(0),fakePso())==S_OK);CHECK(listAlias(0)->Close()==S_OK);
    CHECK(p.stage.load()==acz::png_armed&&acz::g_step5PngWanted.load());
    // (c) the sampled frame is submitted twice in one batch (ambiguous): the readback is never reused.
    frameSet();hudSetRoot();hudBindScene();hudDraws(2);hudOutput();CHECK(listAlias(0)->Close()==S_OK);
    CHECK(p.stage.load()==acz::png_recorded);
    execute({cl(0),cl(0)});advance();(void)acz::step1Tick();
    CHECK(p.stage.load()==acz::png_failed&&!acz::g_step5PngWanted.load());
    for(unsigned k=0;k<4;++k){b=hudFrame(3);CHECK(pngCopies(opsFrom(b))==0);}
    ticks(60);CHECK(res(p.readback).maps==0&&pngFiles()==0);
    const auto text=hudLog();
    for(const char* k:{"event=png-once;outcome=failed;sample=1;reason=duplicate-in-batch;no-further-sample",
            "event=continuous-png;key=gpu_copy_step5_png_once;stage=failed;read=0/2;","last-wait=hud-invalid;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    step3Lines();
}
// ------------------------------------------------------------- J-effects-content: png_once + the effects layer (4th image, burst trigger)
static void step5_png_fx_rules() {
    namespace png=gpu_copy::png;namespace s3=gpu_copy::step3;
    CHECK(png::out_count==5&&png::out_count_fx==9&&png::image_count==3&&png::image_count_fx==4&&png::image_effects==3);
    CHECK(png::readback_bytes_fx(2560,1440)==4*29491200ull&&png::readback_bytes(2560,1440)==3*29491200ull);
    CHECK(png::burst_samples==3&&png::burst_gap_frames==30&&png::threshold_default==6&&png::threshold_max==96);
    CHECK(std::string(png::image_name(3))=="effects"&&std::string(png::output_name(5))=="effects"&&std::string(png::output_name(6))=="effects-on-black"&&
          std::string(png::output_name(7))=="effects-alpha-raw"&&std::string(png::output_name(8))=="effects-nonzero"&&std::string(png::output_name(9))=="scene-end"&&std::string(png::output_name(10))=="?");
    // J-scene-end: the 5th footprint / 10th output
    CHECK(png::image_scene_end==4&&png::image_count_se==5&&png::out_scene_end==9&&png::out_count_se==10&&std::string(png::image_name(4))=="scene-end"&&std::string(png::image_name(5))=="?");
    CHECK(png::readback_bytes_se(2560,1440)==5*29491200ull&&png::readback_bytes_se(2560,1440)==147456000ull);
    // Known pixels, 3x1: effects texel 0 opaque dim red (premultiplied), texel 1 alpha only (0.25), texel 2 empty.
    const uint32_t W=3,H=1;const uint64_t pitch=png::row_pitch(W);
    const size_t pitchN=static_cast<size_t>(pitch);
    std::vector<uint8_t> hudI(pitchN),worldI(pitchN),eyeI(pitchN),fxI(pitchN);
    putTexel(worldI,0,0.0f,0.0f,1.0f,1.0f);putTexel(worldI,1,0.25f,0.25f,0.25f,1.0f);putTexel(worldI,2,1.0f,1.0f,1.0f,1.0f);
    putTexel(fxI,0,0.5f,0.0f,0.0f,1.0f);putTexel(fxI,1,0.0f,0.0f,0.0f,0.25f);putTexel(fxI,2,0.0f,0.0f,0.0f,0.0f);
    const uint8_t* src[4]={hudI.data(),worldI.data(),eyeI.data(),fxI.data()};
    const uint8_t g5=s3::srgb8(0.5f),a1875=s3::srgb8(0.1875f);
    const std::vector<std::vector<uint8_t>> want={
        {g5,0,0,255, a1875,a1875,a1875,255, 255,255,255,255},   // effects over world: e + (1 - max(a, max rgb)) * world
        {g5,0,0,255, 0,0,0,255, 0,0,0,255},                     // premultiplied rgb on black
        {255,255,255,255, 64,64,64,255, 0,0,0,255},             // stored alpha (linear grey)
        {255,255,255,255, 255,255,255,255, 0,0,0,255}};         // any channel non-zero
    for(unsigned k=0;k<4;++k) {
        std::vector<uint8_t> rgba,file,back;uint32_t dw=0,dh=0;const unsigned o=png::out_effects+k;
        CHECK(png::build(src,W,H,pitch,o,rgba,png::image_count_fx));
        if(rgba!=want[k]){std::fprintf(stderr,"output %s:",png::output_name(o));for(auto b:rgba)std::fprintf(stderr," %u",b);std::fprintf(stderr,"\n");CHECK(false);}
        CHECK(s3::png_encode(rgba.data(),W,H,uint64_t(W)*4,false,file)&&decodeStoredPng(file,dw,dh,back)&&dw==W&&dh==H&&back==want[k]);
    }
    {std::vector<uint8_t> unused;CHECK(!png::build(src,W,H,pitch,png::out_effects,unused)&&!png::build(src,W,H,pitch,png::out_effects,unused,png::image_count)&&
        !png::build(src,W,H,pitch,png::out_count_fx,unused,png::image_count_fx));} // effects outputs need the 4th image
    {std::vector<uint8_t> a,b;CHECK(png::build(src,W,H,pitch,png::out_hud_over_world,a)&&png::build(src,W,H,pitch,png::out_hud_over_world,b,png::image_count_fx)&&a==b);}
    // Coverage statistics: 3 texels (opaque red, alpha only, empty).
    {png::FxStats s;png::fx_stats(fxI.data(),W,H,pitch,s);
     CHECK(s.texels==3&&s.nonzero==2&&s.alpha_positive==2&&s.rgb_positive==1&&s.nonfinite==0&&s.alpha_max==1.0&&s.rgb_max==0.5&&
           s.alpha_mean==(1.0+0.25+0.0)/3.0&&s.x0==0&&s.x1==1&&s.y0==0&&s.y1==0);}
    {const uint32_t w2=5,h2=4;const uint64_t p2=png::row_pitch(w2);std::vector<uint8_t> img(size_t(p2)*h2);png::FxStats s;
     png::fx_stats(img.data(),w2,h2,p2,s);CHECK(s.nonzero==0&&s.x0==-1&&s.y0==-1&&s.x1==-1&&s.y1==-1&&s.alpha_max==0.0&&s.alpha_mean==0.0&&s.rgb_max==0.0);
     putTexel(img,size_t(p2/8)*1+2,0.0f,0.5f,0.0f,0.0f);putTexel(img,size_t(p2/8)*3+4,0.0f,0.0f,0.0f,0.5f); // (2,1) and (4,3)
     png::fx_stats(img.data(),w2,h2,p2,s);CHECK(s.nonzero==2&&s.x0==2&&s.y0==1&&s.x1==4&&s.y1==3&&s.rgb_positive==1&&s.alpha_positive==1&&s.alpha_max==0.5);
     const uint16_t nan[4]={0x7e00,0,0,0};std::memcpy(&img[size_t(p2)*2],nan,8);                                                                  // (0,2) non-finite
     png::fx_stats(img.data(),w2,h2,p2,s);CHECK(s.nonzero==3&&s.nonfinite==1&&s.x0==0&&s.y0==1&&s.x1==4&&s.y1==3);}
    // The threshold file text.
    auto thr=[](const char* t){return png::parse_threshold(t,std::strlen(t));};
    CHECK(thr("6")==6&&thr(" 12\r\n")==12&&thr("96")==96&&thr("1")==1&&thr("\t7 ")==7);
    CHECK(thr("0")==0&&thr("97")==0&&thr("")==0&&thr("abc")==0&&thr("5x")==0&&thr("1 2")==0&&thr("12345")==0&&thr("-3")==0&&thr("   ")==0);
}
// The live game's full-screen alpha-only pass (render-target write mask 0x8, blend off, depth ALWAYS): an HUD/effects-region draw the effects layer ignores.
static Object alphaPsoObj;
static ID3D12PipelineState* alphaPsoSetup() {
    alphaPsoObj.table=w->psoTable.data();alphaPsoObj.kind=k_pso;
    auto* pso=reinterpret_cast<ID3D12PipelineState*>(&alphaPsoObj);
    hudPipeline(pso,[](auto& o){o.write_mask0=8;o.blend0=false;o.depth_enable=true;o.depth_func=gpu_copy::step3::cmp::always;});
    return pso;
}
static std::wstring fxThrDir;
// A temporary directory standing in for %LOCALAPPDATA%\acz-vr with an optional threshold file (content nullptr: no file).
static void fxThresholdDir(const char* content) {
    ensurePngDir();fxThrDir=pngDir+L"\\fxthr";CreateDirectoryW(fxThrDir.c_str(),nullptr);acz::g_pngFxDirTest=fxThrDir;
    DeleteFileW((fxThrDir+L"\\png-effects.threshold").c_str()); // (a leftover of an earlier scenario run)
    if(!content)return;
    HANDLE h=CreateFileW((fxThrDir+L"\\png-effects.threshold").c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(h!=INVALID_HANDLE_VALUE);DWORD n=0;WriteFile(h,content,DWORD(std::strlen(content)),&n,nullptr);CloseHandle(h);
}
static void pngFxSetup(const char* thrFile,bool pngKey=true) {
    shape3.rtv=DXGI_FORMAT_R16G16B16A16_FLOAT;shape3.dsv=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    fxThresholdDir(thrFile);acz::g_pngOnceTest=pngKey;effectsSetup(true);
    auto& g=gameRt().big;for(size_t i=0;i+8<=g.size();i+=8)putTexel(g,i/8,0.25f,0.5f,1.0f,1.0f);
}
// One frame: `n` effect draws between two members (committed), then one effect draw + 2 HUD draws after the last member (the tail).
static size_t fxFrame(unsigned n) {
    const std::string seq(n,'G');
    return hudFrame(0,[seq]{hudBindScene();effDraws(seq);hudLateMember();hudBindScene();effDraws("G");hudDraws(2);hudOutput();});
}
struct FxImage {unsigned nonzero=0,x0=UINT_MAX,y0=UINT_MAX,x1=0,y1=0;unsigned covered=0;};
static FxImage fxScan(const std::vector<uint8_t>& big,unsigned w) {
    FxImage r;
    for(size_t i=0;i*8+8<=big.size();++i) {
        bool nz=false;for(size_t k=0;k<8;++k)nz=nz||big[i*8+k]!=0;
        uint16_t h0;std::memcpy(&h0,&big[i*8],2);r.covered+=h0==0x3a00;
        if(!nz)continue;
        ++r.nonzero;const unsigned x=unsigned(i%w),y=unsigned(i/w);
        r.x0=std::min(r.x0,x);r.y0=std::min(r.y0,y);r.x1=std::max(r.x1,x);r.y1=std::max(r.y1,y);
    }
    return r;
}
static unsigned pngFilesNamed(const std::wstring& part) {
    WIN32_FIND_DATAW d;HANDLE h=FindFirstFileW((pngOutDir()+L"\\acz-vr-png-*"+part+L"*.png").c_str(),&d);if(h==INVALID_HANDLE_VALUE)return 0;
    unsigned n=0;do ++n;while(FindNextFileW(h,&d));FindClose(h);return n;
}
// Drives the two base samples to completion with plain effects frames; returns with the machinery in burst mode.
static void fxFinishBase(unsigned n=2) {
    auto& p=s5ref().png;unsigned frames=0;
    while(p.read<2&&frames<80){++frames;(void)fxFrame(n);ticks(3);}
    CHECK(p.read==2);
}
static void step5_png_effects() {
    namespace png=gpu_copy::png;namespace s3=gpu_copy::step3;
    pngFxSetup(nullptr);
    auto& p=s5ref().png;auto* src=acz::stereo::frame_source();
    if(!(p.readback&&p.fxCapable&&p.thr==6&&std::string(p.thrSource)=="default"&&p.stage.load()==acz::png_armed&&!p.burst))
        std::fprintf(stderr,"png: readback %d fxCapable %d thr %u src %s stage %d burst %d refusal %s\n",int(p.readback!=nullptr),int(p.fxCapable),p.thr,p.thrSource,p.stage.load(),int(p.burst),p.refusal?p.refusal:"none");
    CHECK(p.readback&&p.fxCapable&&p.thr==6&&std::string(p.thrSource)=="default"&&p.stage.load()==acz::png_armed&&!p.burst);
    auto& rb=res(p.readback);CHECK(rb.width==png::readback_bytes_se(256,144)&&rb.heap==D3D12_HEAP_TYPE_READBACK);
    // ---- sample 1: a frame WITHOUT effect candidates (only alpha-only HUD draws: ignored by the effects layer, so the effects target was not cleared and
    // holds nothing of this frame) -> 3 images, 5 files, no effects image.
    auto* alphaPso=alphaPsoSetup();
    const size_t b1=hudFrame(0,[alphaPso]{hudBindScene(alphaPso);hudDraws(2);hudOutput();});const auto f1=opsFrom(b1);
    CHECK(pngCopies(f1)==3&&!p.fx);
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f));src->release(f);
    ticks(3);
    if(!(p.read==1&&rb.maps==1&&pngFiles()==5&&pngFilesNamed(L"effects")==0))
        std::fprintf(stderr,"sample1: read %u maps %u files %u effects-files %u fx %d stage %d fail %s\n",p.read,unsigned(rb.maps),pngFiles(),pngFilesNamed(L"effects"),int(p.fx),p.stage.load(),p.failWhy?p.failWhy:"none");
    CHECK(p.read==1&&rb.maps==1&&pngFiles()==5&&pngFilesNamed(L"effects")==0);
    // ---- sample 2: a frame with 2 committed + 1 tail effect draws: 4 images, 9 files.
    unsigned frames=0;size_t b2=0;
    while(p.stage.load()==acz::png_armed&&frames<40){++frames;b2=fxFrame(2);if(pngCopies(opsFrom(b2)))break;}
    const auto f2=opsFrom(b2);CHECK(frames>=9&&pngCopies(f2)==4&&p.fx&&p.fxDraws==2);
    CHECK(src->try_acquire_latest(1,f)&&f.effects_valid&&f.effects_draws==2&&f.effects_tail==3);
    const auto& sl=s5ref().slot[f.slot];
    CHECK(f2.size()>=12);
    const Object* want[4]={&res(sl.hudRt).object,&res(sl.world).object,&res(sl.rt[0]).object,&res(sl.effRt).object};
    for(unsigned i=0;i<4;++i) {
        const Op& a=f2[f2.size()-12+3*i];const Op& c=f2[f2.size()-11+3*i];const Op& z=f2[f2.size()-10+3*i];
        CHECK(a.t==Op::barrier&&a.res==want[i]&&a.before==D3D12_RESOURCE_STATE_RENDER_TARGET&&a.after==D3D12_RESOURCE_STATE_COPY_SOURCE);
        CHECK(c.t==Op::copy_tex&&!c.subres&&c.res==want[i]&&c.dst==&rb.object&&c.offset==uint64_t(i)*p.stride&&c.pitch==p.pitch&&
              c.w==256&&c.h==144&&c.format==UINT(DXGI_FORMAT_R16G16B16A16_FLOAT)&&c.box&&c.bl==0&&c.bt==0);
        CHECK(z.t==Op::barrier&&z.res==want[i]&&z.before==D3D12_RESOURCE_STATE_COPY_SOURCE&&z.after==D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    for(const auto* o:want)CHECK(res(const_cast<Object*>(o)).state==D3D12_RESOURCE_STATE_RENDER_TARGET);
    const std::vector<uint8_t> fxBytes=res(sl.effRt).big;const FxImage scan=fxScan(fxBytes,256);
    CHECK(scan.nonzero>100&&scan.covered==scan.nonzero); // the committed draws rasterised (the fake rasteriser writes 0.75,0.5,0.25,1.0)
    src->release(f);
    ticks(3);CHECK(p.read==2&&pngFiles()==5+9&&pngFilesNamed(L"-s2-effects")==4);
    // The effects PNGs against the target at copy time.
    auto load=[&](const char* output,std::vector<uint8_t>& rgba) {
        WIN32_FIND_DATAW d;const std::wstring pat=pngOutDir()+L"\\acz-vr-png-*-s2-"+std::wstring(output,output+std::strlen(output))+L".png";
        HANDLE h=FindFirstFileW(pat.c_str(),&d);CHECK(h!=INVALID_HANDLE_VALUE);FindClose(h);
        uint32_t dw=0,dh=0;CHECK(decodeStoredPng(readAll(pngOutDir()+L"\\"+d.cFileName),dw,dh,rgba)&&dw==256&&dh==144);
    };
    std::vector<uint8_t> img;const uint8_t r75=s3::srgb8(0.75f),g5=s3::srgb8(0.5f),b25=s3::srgb8(0.25f);
    load("effects-on-black",img);{unsigned px=0;for(size_t i=0;i<size_t(256)*144;++i){const uint8_t* q=&img[i*4];px+=q[0]==r75&&q[1]==g5&&q[2]==b25&&q[3]==255;}CHECK(px==scan.covered&&px>100);}
    load("effects-nonzero",img);{unsigned white=0;for(size_t i=0;i<img.size();i+=4)white+=img[i]==255&&img[i+1]==255&&img[i+2]==255;CHECK(white==scan.nonzero);}
    load("effects-alpha-raw",img);{unsigned white=0;for(size_t i=0;i<img.size();i+=4)white+=img[i]==255;CHECK(white==scan.covered);}
    load("effects",img);{unsigned fxPx=0,worldPx=0;for(size_t i=0;i<img.size();i+=4){fxPx+=img[i]==r75&&img[i+1]==g5&&img[i+2]==b25;worldPx+=img[i+2]==255;}
        CHECK(fxPx>=scan.covered&&worldPx>0&&worldPx<=256u*144u-scan.covered);} // (the world snapshot also holds earlier frames' game draws)
    // ---- burst phase: the base samples are done; the next set waits for >= 6 COMMITTED draws (the tail's draws do not count) and >= 30 published frames.
    CHECK(p.burst&&p.burstRead==0&&p.stage.load()==acz::png_armed&&acz::g_step5PngWanted.load());
    const uint64_t waited0=p.waitedFrames;unsigned copies=0;
    for(unsigned k=0;k<80;++k){const size_t b=fxFrame(5);copies+=pngCopies(opsFrom(b))?1:0;}      // 5 committed + 1 tail = 6 speculative, 5 committed
    CHECK(copies==0&&p.burstBelow>=40&&p.waitedFrames==waited0&&p.stage.load()==acz::png_armed);
    for(unsigned k=0;k<3;++k){const size_t b=hudFrame(4);copies+=pngCopies(opsFrom(b))?1:0;}   // no effects at all
    CHECK(copies==0);
    unsigned bursts=0;
    for(unsigned round=0;round<3;++round) {
        unsigned fr=0;size_t bb=0;bool got=false;
        while(fr<80){++fr;bb=fxFrame(round==0?6:7+round);if(pngCopies(opsFrom(bb))){got=true;break;}}
        CHECK(got&&(round==0||fr>=29)); // after the first, the gap of 30 published frames holds the next one back
        const auto fb=opsFrom(bb);CHECK(pngCopies(fb)==4&&p.burst&&p.fx&&p.fxDraws==(round==0?6u:7u+round));
        ticks(3);++bursts;CHECK(p.burstRead==bursts);
        CHECK(pngFilesNamed(std::wstring(L"-b")+std::to_wstring(bursts)+L"-")==7); // world, eye0, hud-over-world + the 4 effects outputs
    }
    CHECK(p.stage.load()==acz::png_done&&!acz::g_step5PngWanted.load()&&p.burstRead==3&&p.read==2);
    CHECK(pngFiles()==14+21&&rb.maps==2+3&&pngFilesNamed(L"-b1-")+pngFilesNamed(L"-b2-")+pngFilesNamed(L"-b3-")==21);
    for(const wchar_t* o:{L"-b1-world",L"-b1-eye0",L"-b1-hud-over-world",L"-b3-effects-nonzero"})CHECK(pngFilesNamed(o)==1);
    for(unsigned k=0;k<5;++k){const size_t b=fxFrame(9);CHECK(pngCopies(opsFrom(b))==0);}      // never again in this process
    ticks(60);const auto text=hudLog();
    for(const char* k:{"|gpu-copy-step1;event=png-once;outcome=armed;images=hud,world,eye0,effects;format=10;size=256x144;pitch=2048;","commands=9-at-close(12-with-effects);",
            "outputs=hud-on-black,hud-alpha-raw,hud-over-world,world,eye0,effects,effects-on-black,effects-alpha-raw,effects-nonzero;",
            "event=png-effects-config;trigger=first-frame-with-committed-effects-draws>=threshold-after-the-base-samples;threshold=6;source=default;",
            "max-bursts=3;min-gap=30-frames;","event=png-once;outcome=read;sample=1;fence-value=1;","files=5/5;",
            "event=png-stats;sample=2;image=effects;size=256x144;format=10(R16G16B16A16_FLOAT,RGBA);texels=36864;nonfinite=0;",
            "event=png-effects;sample=2;kind=base;committed-draws=2;threshold=6;texels=36864;",
            "event=png-once;outcome=read;sample=2;","kind=base;effects=draws:2,nonzero:","files=9/9;",
            "event=png-effects;sample=3;kind=burst;committed-draws=6;threshold=6;","event=png-effects;sample=5;kind=burst;committed-draws=9;threshold=6;",
            "event=png-once;outcome=read;sample=4;","kind=burst;effects=draws:8,nonzero:","files=7/7;",
            "event=png-burst;sample=3;set=1/3;reason=threshold;file-requests=0;request-to-capture-ms=n/a;","world-nonblack=","effects-image=1;","event=png-burst;sample=5;set=3/3;reason=threshold;",
            "event=png-burst-phases;sample=3;observer=none(","event=png-stats;sample=3;image=world;",
            "event=png-effects-tails;sample=3;kind=burst;effects-nonzero=","smoke-bbox=none(no-scene-end-or-no-change);","hud-bbox=",
            "event=png-once;outcome=done;samples=2;effects-bursts=3;readback=retained(process-lifetime)",
            "key=gpu_copy_step5_png_once;stage=done;read=2/2;","effects-bursts=3/3;threshold=6;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    {const auto pos=text.find("event=png-effects;sample=2;kind=base;");CHECK(pos!=std::string::npos);const std::string line=text.substr(pos,text.find('\n',pos)-pos);
     char want1[96];std::snprintf(want1,sizeof want1,"nonzero=%u(",scan.nonzero);
     char want2[96];std::snprintf(want2,sizeof want2,"bbox=%u,%u,%u,%u(x0,y0,x1,y1)",scan.x0,scan.y0,scan.x1,scan.y1);
     if(line.find(want1)==std::string::npos||line.find(want2)==std::string::npos||line.find("alpha-max=1;")==std::string::npos){std::fprintf(stderr,"png-effects line: %s\n",line.c_str());CHECK(false);}}
    for(const auto& l:w->lines)CHECK(l.find("png-once")==std::string::npos&&l.find("png-effects")==std::string::npos&&l.find("png-stats")==std::string::npos);
    step3Lines();
}
static void fxThresholdScenario(const char* file,unsigned wantThr,const char* wantSrc) {
    pngFxSetup(file);
    auto& p=s5ref().png;
    CHECK(p.fxCapable&&p.thr==wantThr&&std::string(p.thrSource)==wantSrc);
    fxFinishBase(2);
    CHECK(p.burst&&p.stage.load()==acz::png_armed);
    // Exactly thr-1 committed draws never trigger (40 frames), thr does.
    unsigned copies=0;
    if(wantThr>1)for(unsigned k=0;k<40;++k){const size_t b=fxFrame(wantThr-1);copies+=pngCopies(opsFrom(b))?1:0;}
    CHECK(copies==0);
    const size_t b=fxFrame(wantThr);CHECK(pngCopies(opsFrom(b))==4&&p.fxDraws==wantThr);
    ticks(3);const auto text=hudLog();
    char k1[200];std::snprintf(k1,sizeof k1,"event=png-effects-config;trigger=first-frame-with-committed-effects-draws>=threshold-after-the-base-samples;threshold=%u;source=%s;",wantThr,wantSrc);
    char k2[200];std::snprintf(k2,sizeof k2,"event=png-effects;sample=3;kind=burst;committed-draws=%u;threshold=%u;",wantThr,wantThr);
    for(const char* k:{k1,k2})if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    step3Lines();
}
static void step5_png_effects_threshold() {fxThresholdScenario("4\r\n",4,"file");}
static void step5_png_effects_invalid_threshold() {fxThresholdScenario("seven",6,"file-invalid");}
// ---- burst protocol v2: the file trigger %LOCALAPPDATA%\acz-vr\png-burst.now (a temporary directory stands in) + world/eye0/hud-over-world in every burst set
static std::wstring burstCtlSetup(const char* thrFile,bool staleFile) {
    pngFxSetup(thrFile);ensurePngDir();
    const std::wstring ctl=pngDir+L"\\burstctl";CreateDirectoryW(ctl.c_str(),nullptr);
    DeleteFileW((ctl+L"\\png-burst.now").c_str());
    if(staleFile){std::ofstream f(std::filesystem::path(ctl)/"png-burst.now",std::ios::trunc);f<<"left over";}
    acz::g_phControlDirTest=ctl;acz::g_phaseObserveTest=true;
    return ctl;
}
static void burstCtlTouch(const std::wstring& ctl,const char* text) {
    std::this_thread::sleep_for(std::chrono::milliseconds(60)); // a distinct last-write stamp
    std::ofstream f(std::filesystem::path(ctl)/"png-burst.now",std::ios::trunc);f<<text;
}
static void burstCtlEnd() {acz::g_phControlDirTest.clear();acz::g_phaseObserveTest=false;}
static void checkSceneEndOps(const std::vector<Op>& f,const acz::Step5Png& p,bool beforeBarrier); // (below)
static void step5_png_burst_file() {
    namespace png=gpu_copy::png;
    const std::wstring ctl=burstCtlSetup("96\r\n",false); // threshold 96: the threshold trigger never fires, only the file does
    auto& p=s5ref().png;
    CHECK(p.fxCapable&&p.thr==96);
    fxFinishBase(2);
    CHECK(p.burst&&p.fileBaselined&&p.fileTrig.load()==0);
    unsigned copies=0;for(unsigned k=0;k<45;++k){const size_t b=fxFrame(2);copies+=pngCopies(opsFrom(b))?1:0;ticks(1);}
    CHECK(copies==0&&p.burstRead==0&&p.stage.load()==acz::png_armed);              // no file, no burst (quiet frames, below the threshold)
    for(unsigned round=1;round<=3;++round) {
        burstCtlTouch(ctl,round==1?"go":round==2?"again":"third");
        ticks(1);CHECK(p.fileTrig.load()==round);
        const size_t b=fxFrame(2);                                                   // the NEXT valid frame, no 30-frame gap
        const auto f=opsFrom(b);
        CHECK(pngCopies(f)==5&&p.burst&&p.burstFile&&p.fx&&p.se&&p.fileBursts==round); // 4 images at the Close + the scene-end copy at the HUD region end
        checkSceneEndOps(f,p,false);
        ticks(3);CHECK(p.burstRead==round);
        CHECK(pngFilesNamed(std::wstring(L"-b")+std::to_wstring(round)+L"-")==8);
        CHECK(pngFilesNamed(std::wstring(L"-b")+std::to_wstring(round)+L"-scene-end")==1);
        if(round==1)for(const wchar_t* o:{L"-b1-world",L"-b1-eye0",L"-b1-hud-over-world",L"-b1-effects-on-black",L"-b1-effects-alpha-raw",L"-b1-effects-nonzero",L"-b1-scene-end"})CHECK(pngFilesNamed(o)==1);
    }
    CHECK(p.stage.load()==acz::png_done&&!acz::g_step5PngWanted.load()&&p.fileBursts==3);
    burstCtlTouch(ctl,"fourth");ticks(2);                                           // all sets taken: logged, ignored
    for(unsigned k=0;k<3;++k){const size_t b=fxFrame(2);CHECK(pngCopies(opsFrom(b))==0);}
    ticks(60);const auto text=hudLog();
    for(const char* k:{"event=png-burst-config;file-trigger=%LOCALAPPDATA%\\acz-vr\\png-burst.now;","file-present-at-first-poll=0",
            "event=png-burst-request;request=1;","status=pending-next-valid-frame","event=png-burst-request;request=4;","status=ignored-all-burst-sets-taken",
            "event=png-burst;sample=3;set=1/3;reason=file;file-requests=1;request-to-capture-ms=","event=png-burst;sample=4;set=2/3;reason=file;file-requests=2;",
            "event=png-burst;sample=5;set=3/3;reason=file;file-requests=3;","world-nonblack=","eye0-nonblack=","hud-nonblack=","effects-image=1;",
            "event=png-stats;sample=3;image=hud;","event=png-stats;sample=3;image=world;","event=png-stats;sample=3;image=eye0;",
            "event=png-burst-phases;sample=3;observer-frame=","draws-per-phase=pre:0,mem:","event=png-scene-end;sample=3;region-end=rt-change;copies=1;declined=0;scene-end-nonblack=","changed-vs-world=","event=png-burst-phase-shapes;sample=3;","event=png-burst-ps;sample=3;observer-frame=","ps-d2464ed2=pre:","ps-4caf6e2d=","ps-d34ee153=",
            "event=png-effects-tails;sample=3;kind=burst;effects-nonzero=","smoke-bbox=","first-member-ord=",
            "event=png-once;outcome=done;samples=2;effects-bursts=3;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    // the world image of the capture frame carries the fixture's game content (0.25,0.5,1.0): non-black
    {const auto pos=text.find("event=png-burst;sample=3;");CHECK(pos!=std::string::npos);const std::string line=text.substr(pos,text.find('\n',pos)-pos);
     if(line.find("world-nonblack=0(")!=std::string::npos||line.find("world-nonblack=")==std::string::npos){std::fprintf(stderr,"png-burst line: %s\n",line.c_str());CHECK(false);}
     if(line.find(";scene-end=1;files=8/8")==std::string::npos){std::fprintf(stderr,"png-burst line (scene-end): %s\n",line.c_str());CHECK(false);}}
    // the scene-end image: the fake GPU copied the game target (0.25,0.5,1.0 everywhere) into footprint 4: PNG pixels = srgb8 of it
    {WIN32_FIND_DATAW d;HANDLE h=FindFirstFileW((pngOutDir()+L"\\acz-vr-png-*-b1-scene-end.png").c_str(),&d);CHECK(h!=INVALID_HANDLE_VALUE);
     if(h!=INVALID_HANDLE_VALUE) {
         FindClose(h);std::vector<uint8_t> rgba;uint32_t dw=0,dh=0;
         CHECK(decodeStoredPng(readAll(pngOutDir()+L"\\"+d.cFileName),dw,dh,rgba)&&dw==256&&dh==144);
         CHECK(rgba.size()>=4&&rgba[0]==gpu_copy::step3::srgb8(0.25f)&&rgba[1]==gpu_copy::step3::srgb8(0.5f)&&rgba[2]==255&&rgba[3]==255);
     }}
    {const auto pos=text.find("event=png-burst-config;");CHECK(pos!=std::string::npos);const std::string line=text.substr(pos,text.find('\n',pos)-pos);
     if(line.find("file-present-at-first-poll=0")==std::string::npos||line.find("scene-end(file-triggered-sets)")==std::string::npos){std::fprintf(stderr,"config line: %s\n",line.c_str());CHECK(false);}}
    burstCtlEnd();step3Lines();
}
// J-scene-end: the scene-end copy in the frame's ops: Barrier(game RT RENDER_TARGET -> COPY_SOURCE), Copy(game RT -> readback footprint 4, full box),
// Barrier(back), recorded BEFORE the four Close-time copies. `beforeBarrier`: it must also precede the game's own scene-target barrier (scene-barrier path).
static void checkSceneEndOps(const std::vector<Op>& f,const acz::Step5Png& p,bool beforeBarrier) {
    size_t at=SIZE_MAX;for(size_t i=0;i<f.size();++i)if(f[i].t==Op::copy_tex&&!f[i].subres&&f[i].offset==4*p.stride){at=i;break;}
    CHECK(at!=SIZE_MAX&&at>=1&&at+1<f.size());
    if(at==SIZE_MAX||at<1||at+1>=f.size())return;
    const Op& a=f[at-1];const Op& c=f[at];const Op& z=f[at+1];
    CHECK(a.t==Op::barrier&&a.res==&gameRt().object&&a.before==D3D12_RESOURCE_STATE_RENDER_TARGET&&a.after==D3D12_RESOURCE_STATE_COPY_SOURCE);
    CHECK(c.res==&gameRt().object&&c.dst==&res(p.readback).object&&c.pitch==p.pitch&&c.w==256&&c.h==144&&c.format==UINT(DXGI_FORMAT_R16G16B16A16_FLOAT)&&c.box&&c.bl==0&&c.bt==0);
    CHECK(z.t==Op::barrier&&z.res==&gameRt().object&&z.before==D3D12_RESOURCE_STATE_COPY_SOURCE&&z.after==D3D12_RESOURCE_STATE_RENDER_TARGET);
    unsigned later=0;for(size_t i=at+2;i<f.size();++i)later+=f[i].t==Op::copy_tex&&!f[i].subres;
    CHECK(later==4);
    if(beforeBarrier) { // the game's transition of the scene target out of RENDER_TARGET follows the copy
        bool game=false;for(size_t i=at+2;i<f.size();++i)game=game||(f[i].t==Op::barrier&&f[i].res==&gameRt().object&&f[i].before==D3D12_RESOURCE_STATE_RENDER_TARGET&&f[i].after==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        CHECK(game);
    }
    CHECK(res(const_cast<Object*>(&gameRt().object)).state==D3D12_RESOURCE_STATE_RENDER_TARGET);
}
// The HUD region ends through the scene target's own barrier (no OMSetRenderTargets change): the copy is recorded BEFORE that barrier.
static void step5_png_scene_end_barrier() {
    const std::wstring ctl=burstCtlSetup("96\r\n",false);
    auto& p=s5ref().png;
    fxFinishBase(2);
    CHECK(p.burst&&p.fileBaselined);
    burstCtlTouch(ctl,"go");ticks(1);CHECK(p.fileTrig.load()==1&&acz::g_step5SceneEndWanted.load());
    const size_t b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();hudDraws(2);
        hudSceneBarrier(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hudSceneBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);});
    const auto f=opsFrom(b);
    CHECK(pngCopies(f)==5&&p.burstFile&&p.se&&std::string(p.seWhy)=="scene-barrier");
    checkSceneEndOps(f,p,true);
    ticks(3);CHECK(p.burstRead==1&&pngFilesNamed(L"-b1-scene-end")==1);
    ticks(60);const auto text=hudLog();
    if(text.find("event=png-scene-end;sample=3;region-end=scene-barrier;copies=1;")==std::string::npos){std::fprintf(stderr,"missing scene-barrier line\n");CHECK(false);}
    CHECK(!acz::g_step5SceneEndWanted.load()); // no request pending any more
    burstCtlEnd();step3Lines();
}
// A request that arrives AFTER the frame's HUD region ended waits for a frame with a copy (declined once), then is served with the scene-end image.
static void step5_png_scene_end_declined() {
    const std::wstring ctl=burstCtlSetup("96\r\n",false);
    auto& p=s5ref().png;
    fxFinishBase(2);
    CHECK(p.burst&&p.fileBaselined);
    const size_t b1=hudFrame(0,[&]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();hudDraws(2);hudOutput();
        burstCtlTouch(ctl,"late");ticks(1);});                       // the request arrives after the region ended
    CHECK(pngCopies(opsFrom(b1))==0&&p.fileTrig.load()==1&&p.seDeclined==1&&p.stage.load()==acz::png_armed&&acz::g_step5SceneEndWanted.load());
    const size_t b2=fxFrame(2);
    CHECK(pngCopies(opsFrom(b2))==5&&p.se&&p.burstFile&&p.seDeclinedRun==0);
    ticks(3);CHECK(p.burstRead==1&&pngFilesNamed(L"-b1-")==8);
    ticks(60);const auto text=hudLog();
    if(text.find("event=png-scene-end;sample=3;region-end=rt-change;copies=1;declined=1;")==std::string::npos){std::fprintf(stderr,"missing declined line\n");CHECK(false);}
    burstCtlEnd();step3Lines();
}
// No frame ever ends its HUD region through a hooked call: after 2 declined frames the set is served WITHOUT the scene-end image (7 files, scene-end=0).
static void step5_png_scene_end_absent() {
    const std::wstring ctl=burstCtlSetup("96\r\n",false);
    auto& p=s5ref().png;
    fxFinishBase(2);
    burstCtlTouch(ctl,"go");ticks(1);CHECK(p.fileTrig.load()==1);
    for(unsigned k=0;k<2;++k) {
        const size_t b=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();hudDraws(2);});   // the region is stopped by the Close only
        CHECK(pngCopies(opsFrom(b))==0&&p.stage.load()==acz::png_armed&&p.seDeclined==k+1);
    }
    const size_t b3=hudFrame(0,[]{hudBindScene();effDraws("GG");hudLateMember();hudBindScene();hudDraws(2);});
    CHECK(pngCopies(opsFrom(b3))==4&&p.burstFile&&!p.se);
    ticks(3);CHECK(p.burstRead==1&&pngFilesNamed(L"-b1-")==7&&pngFilesNamed(L"-b1-scene-end")==0);
    ticks(60);const auto text=hudLog();
    CHECK(text.find("event=png-scene-end;")==std::string::npos&&text.find(";scene-end=0;files=7/7")!=std::string::npos);
    burstCtlEnd();step3Lines();
}
// A file left over from an earlier session is no order; a later rewrite is. The threshold trigger still works beside it (reason=threshold).
// J-hud-radar + J-scene-end: with the radar key the region does not end at the first render-target change, but the scene-end copy still fires THERE
// (original rt-change semantics): exactly one copy per frame, before the later scene-barrier end (which must not copy again).
static void step5_png_scene_end_radar() {
    const std::wstring ctl=burstCtlSetup("96\r\n",false);
    auto& p=s5ref().png;
    fxFinishBase(2);
    CHECK(p.burst&&p.fileBaselined);
    acz::g_hudRadarTest=true; // read at every frame arm
    burstCtlTouch(ctl,"go");ticks(1);CHECK(p.fileTrig.load()==1&&acz::g_step5SceneEndWanted.load());
    const size_t b=fxFrame(2);
    const auto f=opsFrom(b);
    CHECK(pngCopies(f)==5&&p.burstFile&&p.se&&std::string(p.seWhy)=="rt-change");
    checkSceneEndOps(f,p,false);
    ticks(3);CHECK(p.burstRead==1&&pngFilesNamed(L"-b1-scene-end")==1);
    ticks(60);const auto text=hudLog();
    if(text.find("event=png-scene-end;sample=3;region-end=rt-change;copies=1;")==std::string::npos){std::fprintf(stderr,"missing radar rt-change line\n");CHECK(false);}
    CHECK(!acz::g_step5SceneEndWanted.load());
    acz::g_hudRadarTest=false;burstCtlEnd();step3Lines();
}
static void step5_png_burst_file_stale() {
    const std::wstring ctl=burstCtlSetup("4",true);
    auto& p=s5ref().png;
    fxFinishBase(2);
    CHECK(p.burst&&p.fileBaselined&&p.fileTrig.load()==0);
    unsigned copies=0;for(unsigned k=0;k<45;++k){const size_t b=fxFrame(2);copies+=pngCopies(opsFrom(b))?1:0;ticks(1);}
    CHECK(copies==0&&p.fileRequests==0);
    const size_t t=fxFrame(4);CHECK(pngCopies(opsFrom(t))==4&&p.burstFile==false);   // the threshold trigger (4 committed draws)
    ticks(3);CHECK(p.burstRead==1);
    for(unsigned k=0;k<35;++k){const size_t b=fxFrame(2);CHECK(pngCopies(opsFrom(b))==0);ticks(1);}
    burstCtlTouch(ctl,"fresh");ticks(1);CHECK(p.fileTrig.load()==1);
    const size_t b=fxFrame(2);CHECK(pngCopies(opsFrom(b))==5&&p.burstFile&&p.se);
    ticks(3);CHECK(p.burstRead==2);
    ticks(60);const auto text=hudLog();
    for(const char* k:{"file-present-at-first-poll=1","event=png-burst;sample=3;set=1/3;reason=threshold;file-requests=0;","event=png-burst;sample=4;set=2/3;reason=file;file-requests=1;"})
        if(text.find(k)==std::string::npos){std::fprintf(stderr,"missing %s\n",k);CHECK(false);}
    burstCtlEnd();step3Lines();
}
static void step5_png_effects_off() {
    // png_once ON, effects layer OFF, a threshold file present: nothing of the effects extension exists (3-image readback, no threshold read,
    // no extra line) and the machinery ends after the two base samples exactly as before.
    namespace png=gpu_copy::png;
    shape3.rtv=DXGI_FORMAT_R16G16B16A16_FLOAT;shape3.dsv=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    fxThresholdDir("3");acz::g_pngOnceTest=true;hudSetup(true);
    auto& g=gameRt().big;for(size_t i=0;i+8<=g.size();i+=8)putTexel(g,i/8,0.25f,0.5f,1.0f,1.0f);
    auto& p=s5ref().png;
    CHECK(p.readback&&!p.fxCapable&&p.thr==png::threshold_default&&std::string(p.thrSource)=="default"&&res(p.readback).width==png::readback_bytes(256,144));
    unsigned frames=0;while(p.stage.load()!=acz::png_done&&frames<80){++frames;(void)hudFrame(4);ticks(3);}
    CHECK(p.stage.load()==acz::png_done&&p.read==2&&p.burstRead==0&&!p.burst&&!acz::g_step5PngWanted.load()&&pngFiles()==10&&pngFilesNamed(L"effects")==0);
    ticks(60);const auto text=hudLog();
    CHECK(text.find("png-effects")==std::string::npos&&text.find("effects-bursts")==std::string::npos&&text.find("outputs=hud-on-black,hud-alpha-raw,hud-over-world,world,eye0;")!=std::string::npos&&
          text.find("event=png-once;outcome=done;samples=2;readback=retained(process-lifetime)")!=std::string::npos);
    step3Lines();
}
// An alpha-only full-screen pass between members (mask 0x8, blend off, depth ALWAYS: the live game's) is ignored by the effects layer: no replay, no predicate.
static void step5_effects_alpha_only() {
    effectsSetup(true);auto* src=acz::stereo::frame_source();
    auto* alphaPso=alphaPsoSetup();
    const auto ign0=s5ref().effIgnored;
    const size_t b=hudFrame(0,[alphaPso]{
        hudBindScene();auto* l=listAlias(0);
        l->SetPipelineState(alphaPso);bindT10(srtOff);l->DrawInstanced(3,1,2,0);                     // the alpha-only pass (ignored)
        effDraws("G");                                                                                 // one real effect candidate (committed)
        l->SetPipelineState(alphaPso);bindT10(srtOff);l->DrawInstanced(3,1,2,0);                     // ... and another alpha-only pass
        hudLateMember();hudBindScene();hudDraws(2);hudOutput();});
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.effects_valid&&f.effects_draws==1&&f.effects_refused==0&&f.effects_tail==0);
    const auto c=effCounts(b,f.slot);
    CHECK(c.replays==1&&c.predOn==1&&c.predOff==1&&c.clearsEff==1);                                    // only the real candidate: never the alpha passes, never the HUD-class tail draws
    CHECK(s5ref().effIgnored==ign0+4&&s5ref().effSpeculative==1);
    src->release(f);
    // A frame with ONLY the alpha-only pass between members publishes no effects layer (no committed candidate; the tail draws are predicated off).
    const size_t b2=hudFrame(0,[alphaPso]{
        hudBindScene();auto* l=listAlias(0);l->SetPipelineState(alphaPso);bindT10(srtOff);l->DrawInstanced(3,1,2,0);
        hudLateMember();hudBindScene();hudDraws(2);hudOutput();});
    CHECK(src->try_acquire_latest(1,f)&&!f.effects_valid&&f.effects_draws==0&&f.effects_tail==0);
    const auto c2=effCounts(b2,f.slot);CHECK(c2.replays==0&&c2.predOn==0&&c2.clearsEff==0);src->release(f);
    ticks(60);const auto text=hudLog();
    CHECK(text.find("hud-ds-writers=0")!=std::string::npos&&text.find("ignored=7;")!=std::string::npos);
    step3Lines();
}
static void step5_png_refused() {
    // 8-bit scene format (the fixture default): refused at arming, no readback, no command, one line.
    acz::g_pngOnceTest=true;hudSetup(true);auto& p=s5ref().png;
    CHECK(!p.readback&&p.stage.load()==acz::png_off&&!acz::g_step5PngWanted.load()&&std::string(p.refusal)=="format-not-fp16");
    const size_t b=hudFrame(4);CHECK(pngCopies(opsFrom(b))==0);
    ticks(60);const auto text=hudLog();
    CHECK(text.find("event=png-once;outcome=refused;reason=format-not-fp16;")!=std::string::npos&&text.find("outcome=armed")==std::string::npos);
    step3Lines();
}

struct FakePoseEx final:acz::stereo::PoseProvider {
    uint32_t gen=7;bool available=true;
    bool try_read(acz::stereo::EyePose out[2],uint64_t* t) override {out[0]=fakePose.eyes[0];out[1]=fakePose.eyes[1];if(t)*t=500;return available;}
    bool try_read_ex(acz::stereo::EyePose out[2],uint64_t* t,uint32_t* g) override {if(g)*g=gen;return try_read(out,t);}
};
static void step5_neutral_generation() {
    static FakePoseEx pose;pose.gen=7;acz::stereo::set_pose_provider(&pose);
    fakePose.eyes[0]=eyePose(-0.0315f,1.2f,0,0,gameFov);fakePose.eyes[1]=eyePose(0.0315f,1.2f,0,0,gameFov);
    bootContinuous();contFrame();
    const auto before=acz::g_step5.load()->pf;
    const double yaw=60.0*3.14159265358979323846/180.0;
    for(unsigned e=0;e<2;++e){const double x=e?0.0315:-0.0315;
        fakePose.eyes[e]=eyePose(float(2+std::cos(yaw)*x),2.7f,float(-3-std::sin(yaw)*x),60,gameFov);}
    pose.gen=9;contFrame();const auto& after=acz::g_step5.load()->pf;
    CHECK(after.valid&&acz::g_step5.load()->neutral.generation==9);
    for(unsigned e=0;e<2;++e)for(unsigned i=0;i<16;++i)CHECK(std::fabs(before.D[e][i]-after.D[e][i])<1e-5);
    acz::stereo::StereoFrame f;auto* src=acz::stereo::frame_source();
    CHECK(src->try_acquire_latest(1,f)&&f.space_generation==9&&f.pose_valid&&f.pose[0].position[0]>1.9f);src->release(f);
    // Submitted native replay contains newly computed blobs, not just new metadata.
    unsigned replayed=0;
    // advance() consumes gpuOps, so inspect the persistent upload referenced by native calls.
    for(const auto& o:w->lists[0].ops)if(o.t==Op::draw&&o.boundT10>=0x7000000000ull) {
        std::array<uint8_t,416> expected{};gpu_copy::step5::Scratch scratch;
        const auto* raw=w->resources[2].data()+srtOff; // first visible member
        if(!replayed){CHECK(!gpu_copy::step5::pose_substitute(raw,after,0,expected.data(),scratch));
            CHECK(std::memcmp(blobAt(o.boundT10),expected.data(),416)==0);}++replayed;
    }
    CHECK(replayed>0);acz::stereo::set_pose_provider(nullptr);
}
static void step5_neutral_export() {
    // J-quad-yaw: StereoFrame::neutral_valid/neutral_pos/neutral_yaw carry the producer's seated-
    // recentering neutral (p0 = eye midpoint, yaw0 from the left-eye forward) of the frame's own
    // space generation; invalid without the pose path; rebased with the generation.
    static FakePoseEx pose;pose.gen=7;pose.available=true;acz::stereo::set_pose_provider(&pose);
    const double pi=3.14159265358979323846;
    auto place=[&](double cx,double cy,double cz,double yawDeg){const double a=yawDeg*pi/180.0;
        for(unsigned e=0;e<2;++e){const double x=e?0.0315:-0.0315; // eyes along the turned right axis Ry(a)(1,0,0) = (cos a, 0, -sin a)
            fakePose.eyes[e]=eyePose(float(cx+std::cos(a)*x),float(cy),float(cz-std::sin(a)*x),float(yawDeg),gameFov);}};
    place(0.4,1.3,-0.2,30.0);
    bootContinuous();auto* src=acz::stereo::frame_source();
    auto grab=[&](uint64_t after,acz::stereo::StereoFrame& f){CHECK(src->try_acquire_latest(after,f));src->release(f);};
    contFrame();
    acz::stereo::StereoFrame f;grab(0,f);
    CHECK(f.pose_valid&&f.neutral_valid&&f.space_generation==7);
    CHECK(std::fabs(f.neutral_pos[0]-0.4f)<1e-6f&&std::fabs(f.neutral_pos[1]-1.3f)<1e-6f&&std::fabs(f.neutral_pos[2]+0.2f)<1e-6f);
    CHECK(std::fabs(double(f.neutral_yaw)-30.0*pi/180.0)<1e-6);
    // Sign convention: the cockpit forward Ry(yaw0)(0,0,-1) = (-sin yaw0, 0, -cos yaw0) equals the
    // first left eye's forward R(0,0,-1) (here exactly horizontal).
    {double R[9];CHECK(gpu_copy::step5::quat_rot(fakePose.eyes[0].orientation,R));
     CHECK(std::fabs(-R[2]+std::sin(double(f.neutral_yaw)))<1e-6&&std::fabs(-R[8]+std::cos(double(f.neutral_yaw)))<1e-6);}
    // A later head motion in the same generation keeps the neutral.
    place(0.5,1.25,-0.1,-20.0);contFrame();grab(1,f);
    CHECK(f.neutral_valid&&std::fabs(f.neutral_pos[0]-0.4f)<1e-6f&&std::fabs(double(f.neutral_yaw)-30.0*pi/180.0)<1e-6);
    // A new generation: the neutral is rebased from that generation's first pose (never the stale one).
    pose.gen=9;place(2.0,2.7,-3.0,-90.0);contFrame();grab(2,f);
    CHECK(f.neutral_valid&&f.space_generation==9&&std::fabs(f.neutral_pos[0]-2.0f)<1e-6f&&std::fabs(f.neutral_pos[1]-2.7f)<1e-6f&&
          std::fabs(f.neutral_pos[2]+3.0f)<1e-6f&&std::fabs(double(f.neutral_yaw)+90.0*pi/180.0)<1e-6);
    CHECK(acz::g_step5.load()->neutral.generation==9);
    // 180 degrees: atan2 lands on +-pi (both describe the same heading).
    pose.gen=11;place(0,1.2,0,180.0);contFrame();grab(3,f);
    CHECK(f.neutral_valid&&std::fabs(std::fabs(double(f.neutral_yaw))-pi)<1e-6);
    // No pose: no neutral (invalid, zeros), and no generation.
    pose.available=false;contFrame();grab(4,f);
    CHECK(!f.pose_valid&&!f.neutral_valid&&f.space_generation==0&&f.neutral_pos[0]==0.0f&&f.neutral_pos[1]==0.0f&&f.neutral_pos[2]==0.0f&&f.neutral_yaw==0.0f);
    // Pure helper: not set / pose path not used -> invalid.
    {gpu_copy::step5::Neutral n;CHECK(!gpu_copy::step5::neutral_export(n,true).valid);n.set=true;n.p0[1]=1.2;n.yaw0=0.5;
     CHECK(!gpu_copy::step5::neutral_export(n,false).valid);auto e=gpu_copy::step5::neutral_export(n,true);
     CHECK(e.valid&&e.pos[1]==1.2f&&e.yaw==0.5f);n.yaw0=std::nan("");CHECK(!gpu_copy::step5::neutral_export(n,true).valid);}
    pose.available=true;acz::stereo::set_pose_provider(nullptr);
}
static void step5_consumer_rev5() {
    // Consumer rev 5: producer fence registration (epoch == StereoFrame::fence_epoch) and the
    // pose snapshot space generation carried per slot.
    fakePose.eyes[0]=eyePose(-0.0315f,1.2f,0,0,gameFov);fakePose.eyes[1]=eyePose(0.0315f,1.2f,0,0,gameFov);
    static FakePoseEx pose;acz::stereo::set_pose_provider(&pose);
    bootContinuous();
    CHECK(acz::g_step5Registrations==1&&acz::g_step5RegisteredFence==st1().fence.fence&&acz::g_step5RegisteredEpoch==1);
    auto* src=acz::stereo::frame_source();
    contFrame();
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.fence_epoch==acz::g_step5RegisteredEpoch&&f.space_generation==7&&f.pose_valid);src->release(f);}
    pose.gen=9;contFrame();
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(1,f)&&f.space_generation==9);src->release(f);}
    pose.available=false;contFrame();
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(2,f)&&f.space_generation==0&&!f.pose_valid);src->release(f);} // no pose: unknown
    advance();st1().fence.fence=reinterpret_cast<ID3D12Fence*>(&w->fence2);(void)acz::step1Tick();
    CHECK(acz::g_step5Registrations==2&&acz::g_step5RegisteredFence==&w->fence2&&acz::g_step5RegisteredEpoch==2);
    acz::stereo::set_pose_provider(nullptr);
}
static void gameBarrier(D3D12_RESOURCE_BARRIER_TYPE type,UINT sub,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after,D3D12_RESOURCE_BARRIER_FLAGS flags) {
    D3D12_RESOURCE_BARRIER b{};b.Type=type;b.Flags=flags;
    auto* g=reinterpret_cast<ID3D12Resource*>(&gameRt().object);
    if(type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION){b.Transition.pResource=g;b.Transition.Subresource=sub;b.Transition.StateBefore=before;b.Transition.StateAfter=after;}
    else if(type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){b.Aliasing.pResourceBefore=nullptr;b.Aliasing.pResourceAfter=g;}
    else b.UAV.pResource=g;
    listAlias(0)->ResourceBarrier(1,&b);
}
static void step5_world_barriers() {
    // P2: the world snapshot uses tracked barrier facts; refused on anything it cannot account for.
    bootContinuous(true);
    auto* src=acz::stereo::frame_source();
    auto world=[&](uint64_t id){acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);const bool v=f.world_valid;src->release(f);return v;};
    // (a) the game's own complete transitions RT -> PSR -> RT before the cockpit: tracked evidence.
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    varFrame(standardDraws());CHECK(world(1)&&gameRt().state==D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(line("event=world-snapshot;outcome=copied;","state-evidence=tracked-transition-to-RENDER_TARGET"));
    // (b) a split barrier (BEGIN_ONLY) on the RT in this list: refused (eyes still published).
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);};
    varFrame(standardDraws());CHECK(!world(2));
    CHECK(line("event=world-snapshot;outcome=refused;reason=split-barrier;",""));
    // (c) a subresource other than ALL/0.
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,3,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    varFrame(standardDraws());CHECK(!world(3));
    // (d) an aliasing barrier naming it; (e) a UAV barrier on it.
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_ALIASING,0,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    varFrame(standardDraws());CHECK(!world(4));
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_UAV,0,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    varFrame(standardDraws());CHECK(!world(5));
    // A clean generation afterwards (the facts are per list generation; the carried state is
    // unknown after the refused lists, so this one proves it with its own transitions).
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    varFrame(standardDraws());CHECK(world(6));afterBind=nullptr;
    CHECK(src->frames_published()==6&&w->worldCopies==2);
    // Rules that the fake GPU cannot execute: unit-checked on the live shape.
    {auto& st=st1();auto* s5=acz::g_step5.load();std::lock_guard lock(st.mutex);auto x=st.s3.live;
     CHECK(!acz::step5WorldWhy(*s5,x));
     auto y=x;y.world_rt_transition=true;y.world_rt_after=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
     CHECK(std::string_view(acz::step5WorldWhy(*s5,y))=="last-transition-not-render-target");
     y=x;y.world_enhanced=true;CHECK(std::string_view(acz::step5WorldWhy(*s5,y))=="enhanced-barrier-in-list");
     y=x;y.world_barriers_tracked=false;CHECK(std::string_view(acz::step5WorldWhy(*s5,y))=="barrier-tracking-unavailable");
     y=x;y.world_barrier_why="aliasing-barrier";CHECK(std::string_view(acz::step5WorldWhy(*s5,y))=="aliasing-barrier");}
    ticks(60);CHECK(line("event=continuous-quality;","world=valid:2,refused:4"));
    step3Lines();
}
static void step5_fence_epoch() {
    // Issue-29 consumer rev 4: fence_epoch + last_signalled bookkeeping.
    bootContinuous();
    auto* src=acz::stereo::frame_source();
    contFrame();
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.fence_epoch==1&&f.fence_value==1&&f.last_signalled==1);src->release(f);
    // A failed Signal never advances last_signalled (and the frame is not published).
    w->signalHr=DXGI_ERROR_DEVICE_REMOVED;contFrame();w->signalHr=S_OK;
    {auto* s5=acz::g_step5.load();CHECK(s5->lastSignalled.load()==1&&src->frames_published()==1);}
    // A value the producer has not signalled is never published.
    {auto& st=st1();auto* s5=acz::g_step5.load();std::lock_guard lock(st.mutex);
     acz::Step5Frame x{};x.slot=0;std::lock_guard m(s5->meta);s5->slot[0].state=gpu_copy::step5::slot_producing;
     const auto before=s5->published.load();acz::step5Publish_locked(*s5,x,s5->lastSignalled.load()+5);
     CHECK(s5->published.load()==before&&s5->publishRefused==1&&s5->slot[0].state==gpu_copy::step5::slot_free);}
    contFrame();CHECK(src->frames_published()==2);
    {CHECK(src->try_acquire_latest(1,f)&&f.fence_epoch==1&&f.fence_value<=f.last_signalled);src->release(f);}
    // The private fence object changes: a new epoch; old-epoch frames are no longer acquirable.
    advance();st1().fence.fence=reinterpret_cast<ID3D12Fence*>(&w->fence2);(void)acz::step1Tick();
    {auto* s5=acz::g_step5.load();CHECK(s5->fenceEpoch.load()==2&&s5->lastSignalled.load()==0);}
    CHECK(!src->try_acquire_latest(0,f));
    contFrame();
    // Slots used on the old fence were quarantined; a frame on the new epoch needs a clean slot.
    {auto* s5=acz::g_step5.load();unsigned q=0;for(const auto& sl:s5->slot)q+=sl.state==gpu_copy::step5::slot_quarantined;CHECK(q>=2);}
    step3Lines();
}

static void step5_rules() {
    using namespace gpu_copy::step5;
    acz::Mat4 m0,m64,m256;sceneMatrices(m0,m64,m256);
    std::array<uint8_t,416> blob{};for(size_t i=0;i<416;++i)blob[i]=uint8_t(i*7);
    putMatrix(blob.data(),m0);putMatrix(blob.data()+64,m64);putMatrix(blob.data()+256,m256);
    {float nz=-0.0f;std::memcpy(blob.data()+64+4*1,&nz,4);} // a negative zero must survive a zero delta
    Neutral n;PoseFrame f;Scratch sc;Lens lens;
    acz::stereo::EyePose e[2]={eyePose(0,1.2f,0,0,gameFov),eyePose(0,1.2f,0,0,gameFov)};
    CHECK(!setup_frame(blob.data(),e,n,f,sc,&lens)&&f.valid&&lens.valid);
    // Recovered camera and lens equal the fixture camera.
    CHECK(std::fabs(f.cam.eye[0]-sceneEye[0])<1e-6&&std::fabs(f.cam.tanX-0.6152)<1e-6&&std::fabs(f.cam.right[0]-1)<1e-9&&std::fabs(f.cam.fwd[2]+1)<1e-9);
    CHECK(std::fabs(lens.tan[0]-0.6152f)<1e-6f&&lens.tan[1]==lens.tan[0]&&std::fabs(lens.tan[2]-0.3460f)<1e-6f&&lens.tan[3]==lens.tan[2]);
    // Neutral pose: the game's lens is kept, so D == 0 EXACTLY and the bytes are raw (-0.0 kept).
    for(int i=0;i<16;++i)CHECK(f.D[0][i]==0.0&&f.D[1][i]==0.0);
    std::array<uint8_t,416> out{};
    CHECK(!pose_substitute(blob.data(),f,0,out.data(),sc)&&out==blob&&!pose_substitute(blob.data(),f,1,out.data(),sc)&&out==blob);
    // The neutral pose with ANY XR FOV still gives D == 0 (the XR FOV is not applied).
    {const float asym[4]={-0.70f,0.55f,0.40f,-0.35f};acz::stereo::EyePose a[2]={eyePose(0,1.2f,0,0,asym),eyePose(0,1.2f,0,0,asym)};
     Neutral na;CHECK(!setup_frame(blob.data(),a,na,f,sc));for(int i=0;i<16;++i)CHECK(f.D[0][i]==0.0&&f.D[1][i]==0.0);}
    // VP_ref equals the game VP (recovered) to double precision.
    CHECK(!setup_frame(blob.data(),e,n,f,sc));
    acz::Mat4 vp=acz::multiply(acz::inverse(m256),m64);for(int i=0;i<16;++i){CHECK(std::fabs(f.vpRef[i]-f.vp[i])<1e-12*(1+std::fabs(f.vp[i])));CHECK(std::fabs(f.vp[i]-vp[size_t(i)])<1e-6*(1+std::fabs(vp[size_t(i)])));}
    // Yaw 10 deg + 0.1 m (asymmetric XR FOV ignored): VP_e equals cockpit_math's view x the game projection.
    const float asym[4]={-0.70f,0.55f,0.40f,-0.35f};const float p0[3]={0,1.2f,0};
    acz::stereo::EyePose m[2]={eyePose(0.1f,1.2f,-0.05f,10.0f,asym),eyePose(-0.1f,1.25f,0.02f,-5.0f,gameFov)};
    CHECK(!setup_frame(blob.data(),m,n,f,sc)&&f.valid);
    for(unsigned k=0;k<2;++k){acz::Mat4 want;expectedEye(m[k],p0,want);
        double sc=0,err=0;for(int i=0;i<16;++i){sc=std::max(sc,std::fabs(want[size_t(i)]));err=std::max(err,std::fabs(f.vpEye[k][i]-want[size_t(i)]));}
        CHECK(err<1e-6*sc);} // float SrtData source: ~3e-8 relative measured
    // Invalid poses fall back (the lens stays valid: it comes from the game).
    acz::stereo::EyePose badPose[2]={m[0],m[1]};badPose[0].position[1]=std::nanf("");CHECK(setup_frame(blob.data(),badPose,n,f,sc,&lens)&&!f.valid&&lens.valid);
    badPose[0]=m[0];badPose[0].orientation[3]=std::nanf("");CHECK(setup_frame(blob.data(),badPose,n,f,sc));
    // A degenerate camera: lens invalid.
    {std::array<uint8_t,416> z{};CHECK(setup_frame(z.data(),e,n,f,sc,&lens)&&!lens.valid);}
    // Recentering: a neutral captured at yaw 30 deg makes a later 30 deg head pose the neutral view.
    Neutral n2;acz::stereo::EyePose y30[2]={eyePose(0,1.2f,0,30.0f,gameFov),eyePose(0,1.2f,0,30.0f,gameFov)};
    CHECK(!setup_frame(blob.data(),y30,n2,f,sc)&&std::fabs(n2.yaw0-30.0*3.14159265358979323846/180.0)<1e-6);
    for(int i=0;i<16;++i)CHECK(std::fabs(f.D[0][i])<1e-9*(1+std::fabs(f.vpRef[i])));
}
// ------------------------------------------------------------- J-cockpit-fov (F1 hmd_fov, F2 precockpit_observe, F3 members_by_class)
static std::vector<std::string> frameEvents(){const auto& L=w->lists[0];return std::vector<std::string>(L.events.begin()+std::ptrdiff_t(lastReset(L)),L.events.end());}
static std::vector<Op> gameDrawOps(){std::vector<Op> v;for(const auto& o:w->lists[0].ops)if(o.t==Op::draw&&originOf(o.boundRtv)==gameRtvHandle)v.push_back(o);return v;}
// The game's own draws as the GPU sees them (the restore re-binds the bind-time private copies of the
// game's descriptors, whose handle values rotate: compared by the source descriptor they copy).
static bool sameGameDraw(const Op& a,const Op& b) {
    return a.t==b.t&&a.dc==b.dc&&a.di==b.di&&a.ds==b.ds&&a.dsi==b.dsi&&a.db==b.db&&a.indexed==b.indexed&&a.boundCount==b.boundCount&&
        originOf(a.boundRtv)==originOf(b.boundRtv)&&a.boundDsvSet==b.boundDsvSet&&originOf(a.boundDsv)==originOf(b.boundDsv)&&a.boundT10==b.boundT10;
}
static std::string dedicated(){acz::feature_log::flush();return featureLog("step5-producer");}
static bool hasText(const std::string& text,const char* a,const char* b="") {
    size_t at=0;
    while((at=text.find(a,at))!=std::string::npos){const size_t end=text.find('\n',at);if(text.substr(at,end-at).find(b)!=std::string::npos)return true;++at;}
    return false;
}
// Row-vector clip of a world point under a VP (double).
static void clipOf(const double* vp,const double p[3],double c[4]){for(int k=0;k<4;++k)c[k]=p[0]*vp[k]+p[1]*vp[4+k]+p[2]*vp[8+k]+vp[12+k];}
static void step5_cockpit_keys() {
    // Defaults OFF; parse like the other step-5 keys.
    using namespace acz::diagnostics;
    CHECK(!Config{}.gpu_copy_step5_hmd_fov()&&!Config{}.gpu_copy_step5_precockpit_observe()&&!Config{}.gpu_copy_step5_members_by_class());
    {auto c=parse_config("");CHECK(!c.config.gpu_copy_step5_hmd_fov()&&!c.config.gpu_copy_step5_precockpit_observe()&&!c.config.gpu_copy_step5_members_by_class());}
    for(const char* k:{"gpu_copy_step5_hmd_fov","gpu_copy_step5_precockpit_observe","gpu_copy_step5_members_by_class"}) {
        auto get=[&](const Config& c){const std::string_view n(k);return n=="gpu_copy_step5_hmd_fov"?c.gpu_copy_step5_hmd_fov():
            n=="gpu_copy_step5_precockpit_observe"?c.gpu_copy_step5_precockpit_observe():c.gpu_copy_step5_members_by_class();};
        for(const char* v:{"true","1"}){auto c=parse_config(std::string(k)+"="+v);CHECK(c.issue==ConfigIssue::none&&get(c.config));}
        for(const char* v:{"false","0"}){auto c=parse_config(std::string(k)+"="+v);CHECK(c.issue==ConfigIssue::none&&!get(c.config));}
        CHECK(parse_config(std::string(k)+"=on").issue==ConfigIssue::out_of_range);
        CHECK(parse_config(std::string(k)+"=1\n"+k+"=0").issue==ConfigIssue::duplicate);
        CHECK(!parse_config(std::string(k)+"=1").config.gpu_copy_step5_continuous()&&!parse_config(std::string(k)+"=1").config.gpu_copy_step5_hud());
    }
    // All three keys OFF (the default): the producer's ring, command stream and log lines are those of the build without them.
    bootContinuous();
    CHECK(line("event=ring;","upload=196608;members=4;commands-per-frame=36"));
    CHECK(s5ref().memberCap==64&&s5ref().commandCap==600&&s5ref().slotUpload==65536);
    contFrame();
    {const std::vector<std::string> member{"om","srv","draw-indexed","om","srv","draw-indexed","srv","om"};
     std::vector<std::string> want{"reset","om","srv"};
     for(unsigned i=1;i<=8;++i){want.push_back("srv");const bool m=i!=1&&i!=6;
        if(i==2)for(const char* c:{"clear-rtv","clear-rtv","clear-dsv","clear-dsv"})want.push_back(c);
        if(m)want.insert(want.end(),member.begin(),member.end());want.push_back("draw-indexed");}
     const auto got=frameEvents();if(got!=want){for(auto& e:got)std::fprintf(stderr,"%s ",e.c_str());std::fprintf(stderr,"\n");CHECK(false);}}
    acz::stereo::StereoFrame f;auto* src=acz::stereo::frame_source();CHECK(src->try_acquire_latest(0,f)&&!f.cockpit_fov_valid&&f.members==6);
    for(int e=0;e<2;++e)for(int i=0;i<4;++i)CHECK(f.cockpit_fov[e][i]==0.0f);
    src->release(f);
    ticks(60);const auto text=dedicated();
    for(const char* k:{"continuous-fov","continuous-members","continuous-precockpit","precockpit;","precockpit-srt;","hmd-fov","class-family-admit",
            "members-by-class-definitions","world-objects","continuous-status-world;","continuous-status-world-camera"})if(text.find(k)!=std::string::npos){std::fprintf(stderr,"unexpected %s\n",k);CHECK(false);}
    CHECK(!acz::g_pc.frames&&!acz::g_pc.records&&s5ref().classFrames==0&&s5ref().fovFrames==0&&s5ref().familyCount==0);
    step3Lines();
}
static void step5_hmd_fov_rules() {
    using namespace gpu_copy::step5;
    acz::Mat4 m0,m64,m256;sceneMatrices(m0,m64,m256);
    std::array<uint8_t,416> blob{};for(size_t i=0;i<416;++i)blob[i]=uint8_t(i*7);
    putMatrix(blob.data(),m0);putMatrix(blob.data()+64,m64);putMatrix(blob.data()+256,m256);putParams(blob.data());
    const acz::stereo::EyePose neutral[2]={eyePose(-0.0315f,1.2f,0,0,gameFov),eyePose(0.0315f,1.2f,0,0,gameFov)};
    const acz::stereo::EyePose moved[2]={eyePose(0.1f-0.0315f,1.2f,-0.05f,10.0f,gameFov),eyePose(0.1f+0.0315f,1.25f,0.02f,-5.0f,gameFov)};
    // (1) FOV == the game lens: hmd ON and OFF give the SAME D and byte-identical substitution (neutral and moved).
    for(const auto* poses:{neutral,moved}) {
        Neutral na,nb;PoseFrame fa,fb;Scratch sc;
        CHECK(!setup_frame(blob.data(),neutral,na,fa,sc,nullptr,false)&&!setup_frame(blob.data(),neutral,nb,fb,sc,nullptr,true)); // neutral fixed first
        CHECK(!setup_frame(blob.data(),poses,na,fa,sc,nullptr,false)&&!setup_frame(blob.data(),poses,nb,fb,sc,nullptr,true));
        CHECK(!fa.fovHmd&&std::string_view(fa.fovWhy)=="not-requested"&&fb.fovHmd&&std::string_view(fb.fovWhy)=="hmd");
        CHECK(std::memcmp(fa.D,fb.D,sizeof fa.D)==0&&std::memcmp(fa.vpRef,fb.vpRef,sizeof fa.vpRef)==0&&std::memcmp(fa.vpEye,fb.vpEye,sizeof fa.vpEye)==0);
        for(unsigned e=0;e<2;++e){std::array<uint8_t,416> oa{},ob{};CHECK(!pose_substitute(blob.data(),fa,e,oa.data(),sc)&&!pose_substitute(blob.data(),fb,e,ob.data(),sc)&&oa==ob);}
        for(unsigned e=0;e<2;++e)for(int i=0;i<4;++i)CHECK(std::fabs(fb.fov[e].angle[i]-gameFov[i])<1e-6f); // snapped to the lens
    }
    // (2) Asymmetric per-eye FOVs (radians): each eye's frustum edges land on NDC +-1 (x) and -+1 (y, the game's negative clip-y).
    const float fovL[4]={-0.90f,0.75f,0.80f,-0.85f},fovR[4]={-0.70f,0.95f,0.78f,-0.88f};
    const acz::stereo::EyePose asym[2]={eyePose(-0.0315f,1.2f,0,0,fovL),eyePose(0.0315f,1.2f,0,0,fovR)};
    Neutral n;PoseFrame f;Scratch sc;
    CHECK(!setup_frame(blob.data(),asym,n,f,sc,nullptr,true)&&f.valid&&f.fovHmd&&!f.fovClamped&&std::string_view(f.fovWhy)=="hmd");
    for(int i=0;i<4;++i)CHECK(f.fov[0].angle[i]==fovL[i]&&f.fov[1].angle[i]==fovR[i]); // published unchanged (not clamped/snapped)
    // The reference stays on the game lens (VP_ref reproduces the game's VP).
    {Neutral n2;PoseFrame g;CHECK(!setup_frame(blob.data(),asym,n2,g,sc,nullptr,false));CHECK(std::memcmp(g.vpRef,f.vpRef,sizeof g.vpRef)==0);
     bool differs=false;for(int i=0;i<16;++i)differs=differs||g.D[0][i]!=f.D[0][i];CHECK(differs);}
    const acz::Mat4 inv256=acz::inverse(m256);
    for(unsigned e=0;e<2;++e) {
        const float* fv=e?fovR:fovL;double T[4];for(int i=0;i<4;++i)T[i]=std::tan(double(fv[i]));
        std::array<uint8_t,416> out{};CHECK(!pose_substitute(blob.data(),f,e,out.data(),sc));
        float s64[16],s0[16],P[4];std::memcpy(s0,out.data(),64);std::memcpy(s64,out.data()+64,64);std::memcpy(P,out.data()+400,16);
        CHECK(std::memcmp(out.data()+128,blob.data()+128,416-128)==0); // only +0 and +64 change
        auto edge=[&](double tx,double ty,double wantX,double wantY) {
            const double d=30.0;double p[3];
            for(int k=0;k<3;++k)p[k]=f.eyeWorld[e][k]+d*(f.cam.fwd[k]+tx*f.cam.right[k]+ty*f.cam.up[k]);
            double c[4];clipOf(f.vpEye[e],p,c);
            CHECK(c[3]>0);
            if(!std::isnan(wantX))CHECK(std::fabs(c[0]/c[3]-wantX)<1e-9);
            if(!std::isnan(wantY))CHECK(std::fabs(c[1]/c[3]-wantY)<1e-9);
            // Through the substituted float bytes, as the game's GS sees them (+64 clip, +0 PS2 screen and +400).
            const auto lp=acz::transformPoint(inv256,{p[0],p[1],p[2]});
            double q[4]{},s[4]{};for(int k=0;k<4;++k)for(int j=0;j<4;++j){const double v=j<3?lp[size_t(j)]:1.0;q[k]+=v*s64[j*4+k];s[k]+=v*s0[j*4+k];}
            if(!std::isnan(wantX))CHECK(std::fabs(q[0]/q[3]-wantX)<2e-4);
            if(!std::isnan(wantY))CHECK(std::fabs(q[1]/q[3]-wantY)<2e-4);
            const double nx=2.0*(s[0]/s[3]-double(P[2]))/double(P[0]),ny=2.0*(s[1]/s[3]-double(P[3]))/double(P[1]);
            CHECK(std::fabs(nx-q[0]/q[3])<1e-3&&std::fabs(ny-q[1]/q[3])<1e-3);
        };
        const double nan=std::nan("");
        edge(T[0],0,-1,nan);edge(T[1],0,1,nan);edge(0,T[2],nan,-1);edge(0,T[3],nan,1);
        edge(T[0],T[2],-1,-1);edge(T[1],T[3],1,1);
        // The centre direction is off-centre for an asymmetric FOV (not the symmetric game lens).
        {double p[3];for(int k=0;k<3;++k)p[k]=f.eyeWorld[e][k]+30.0*f.cam.fwd[k];double c[4];clipOf(f.vpEye[e],p,c);CHECK(std::fabs(c[0]/c[3])>0.01);}
    }
    // (3) Unusable FOVs: the game lens for BOTH eyes, counted reason; the pose path still runs (D == the key-off D).
    auto fallback=[&](const float a[4],const float b[4],const char* why) {
        const acz::stereo::EyePose p[2]={eyePose(-0.0315f,1.2f,0,0,a),eyePose(0.0315f,1.2f,0,0,b)};
        Neutral n1,n2;PoseFrame g,h;
        CHECK(!setup_frame(blob.data(),p,n1,g,sc,nullptr,true)&&g.valid&&!g.fovHmd);
        if(std::string_view(g.fovWhy)!=why){std::fprintf(stderr,"fovWhy %s want %s\n",g.fovWhy,why);CHECK(false);}
        CHECK(!setup_frame(blob.data(),p,n2,h,sc,nullptr,false)&&std::memcmp(g.D,h.D,sizeof g.D)==0);
        for(unsigned e=0;e<2;++e)for(int i=0;i<4;++i)CHECK(g.fov[e].t[i]==0&&g.fov[e].angle[i]==0);
    };
    const float zero[4]={0,0,0,0},nanf4[4]={-0.8f,std::nanf(""),0.7f,-0.7f},reversed[4]={0.8f,-0.8f,0.7f,-0.7f},over[4]={-1.6f,0.8f,0.7f,-0.7f},
        tiny[4]={-0.0002f,0.0002f,0.7f,-0.7f};
    fallback(zero,zero,"fov-zero");fallback(fovL,zero,"fov-zero");fallback(zero,fovL,"fov-zero"); // one eye unusable -> both fall back
    fallback(nanf4,fovR,"fov-nonfinite");fallback(reversed,fovR,"fov-not-ordered");fallback(over,fovR,"fov-over-90-degrees");
    fallback(fovL,tiny,"fov-span-too-small");
    // (4) Clamp: |tan| > 4 is clamped (counted) and the PUBLISHED angle is the clamped one (what was rendered).
    {const float wide[4]={-1.50f,0.8f,1.45f,-0.7f};const acz::stereo::EyePose p[2]={eyePose(-0.0315f,1.2f,0,0,wide),eyePose(0.0315f,1.2f,0,0,fovR)};
     Neutral n1;PoseFrame g;CHECK(!setup_frame(blob.data(),p,n1,g,sc,nullptr,true)&&g.fovHmd&&g.fovClamped&&std::string_view(g.fovWhy)=="hmd-clamped");
     CHECK(g.fov[0].t[0]==-fov_tan_max&&g.fov[0].t[2]==fov_tan_max&&g.fov[0].angle[0]==float(std::atan(-4.0))&&g.fov[0].angle[1]==0.8f);
     for(int i=0;i<4;++i)CHECK(g.fov[1].angle[i]==fovR[i]);}
    // (5) No pose: no FOV either (IPD fallback path, fovHmd false).
    {acz::stereo::EyePose bad[2]={asym[0],asym[1]};bad[0].position[0]=std::nanf("");Neutral n1;PoseFrame g;
     CHECK(setup_frame(blob.data(),bad,n1,g,sc,nullptr,true)&&!g.valid&&!g.fovHmd);}
    // hmd_tangents directly: the game lens within 1e-5 is snapped, a larger difference is kept.
    {Camera c;c.tanX=0.6152;c.tanY=0.3460;EyeFov o;bool cl=false;
     const float near5[4]={float(std::atan(-0.6152*(1+5e-6))),float(std::atan(0.6152)),float(std::atan(0.3460)),float(std::atan(-0.3460))};
     CHECK(!hmd_tangents(near5,c,o,cl)&&o.t[0]==-0.6152&&o.t[1]==0.6152&&!cl);
     const float far5[4]={float(std::atan(-0.6152*(1+1e-3))),float(std::atan(0.6152)),float(std::atan(0.3460)),float(std::atan(-0.3460))};
     CHECK(!hmd_tangents(far5,c,o,cl)&&o.t[0]!=-0.6152&&o.angle[0]==far5[0]);}
}
// F1 live path: per-frame FOV from the pose snapshot, StereoFrame fields, byte identity with the game lens.
static void step5_hmd_fov() {
    const float p0[3]={0,1.2f,0};(void)p0;
    fakePose.available=true;fakePose.eyes[0]=eyePose(-0.0315f,1.2f,0,0,gameFov);fakePose.eyes[1]=eyePose(0.0315f,1.2f,0,0,gameFov);
    acz::stereo::set_pose_provider(&fakePose);
    bootContinuous();
    auto* src=acz::stereo::frame_source();auto* s5=acz::g_step5.load();
    auto grab=[&](uint64_t after,acz::stereo::StereoFrame& f,std::array<std::array<uint8_t,416>,2>& blobs) {
        CHECK(src->try_acquire_latest(after,f)&&f.frame_id==after+1&&f.pose_valid);
        const uint8_t* up=res(s5->upload).data()+s5->slot[f.slot].uploadOffset; // member 0 (srtOff scene)
        for(int e=0;e<2;++e)std::memcpy(blobs[size_t(e)].data(),up+e*512,416);
        src->release(f);
    };
    std::array<std::array<uint8_t,416>,2> b1{},b2{},b3{},b4{},b5{};acz::stereo::StereoFrame f;
    contFrame();grab(0,f,b1);CHECK(!f.cockpit_fov_valid); // frame 1, key OFF: the neutral
    CHECK(line("event=continuous-first;","eyes=pose-full-substitution-game-lens"));
    const auto moved0=eyePose(0.1f-0.0315f,1.2f,-0.05f,10.0f,gameFov),moved1=eyePose(0.1f+0.0315f,1.2f,-0.05f,10.0f,gameFov);
    fakePose.eyes[0]=moved0;fakePose.eyes[1]=moved1;
    contFrame();const auto ev2=frameEvents();const auto g2=gameDrawOps();grab(1,f,b2);CHECK(!f.cockpit_fov_valid); // frame 2: key OFF, moved
    // Frame 3: key ON, the same pose, FOV == the game lens: byte-identical blobs, identical command stream.
    acz::g_hmdFovTest=true;w->lists[0].ops.clear();
    contFrame();CHECK(frameEvents()==ev2);
    {const auto g3=gameDrawOps();CHECK(g3.size()==g2.size());for(size_t i=0;i<g3.size();++i)CHECK(sameGameDraw(g2[i],g3[i]));}
    grab(2,f,b3);CHECK(b3==b2&&b2!=b1);
    CHECK(f.cockpit_fov_valid);for(int e=0;e<2;++e)for(int i=0;i<4;++i)CHECK(std::fabs(f.cockpit_fov[e][i]-gameFov[i])<1e-6f);
    // Frame 4: asymmetric per-eye FOVs: published exactly, and the blobs are the header's hmd substitution.
    const float fovL[4]={-0.90f,0.75f,0.80f,-0.85f},fovR[4]={-0.70f,0.95f,0.78f,-0.88f};
    for(int i=0;i<4;++i){fakePose.eyes[0].fov[i]=fovL[i];fakePose.eyes[1].fov[i]=fovR[i];}
    contFrame();grab(3,f,b4);
    CHECK(f.cockpit_fov_valid&&f.lens_valid&&b4!=b3);for(int i=0;i<4;++i)CHECK(f.cockpit_fov[0][i]==fovL[i]&&f.cockpit_fov[1][i]==fovR[i]);
    CHECK(std::fabs(f.lens_tan[0]-0.6152f)<1e-5f); // the game lens is still published (world quad, HUD)
    {using namespace gpu_copy::step5;const uint8_t* raw=w->resources[2].data()+srtOff;
     const acz::stereo::EyePose n0[2]={eyePose(-0.0315f,1.2f,0,0,gameFov),eyePose(0.0315f,1.2f,0,0,gameFov)};
     Neutral n;PoseFrame pf;Scratch sc;CHECK(!setup_frame(raw,n0,n,pf,sc,nullptr,true));
     CHECK(!setup_frame(raw,fakePose.eyes,n,pf,sc,nullptr,true)&&pf.fovHmd);
     for(unsigned e=0;e<2;++e){std::array<uint8_t,416> want{};CHECK(!pose_substitute(raw,pf,e,want.data(),sc)&&want==b4[e]);}}
    // Frame 5: one eye's FOV unusable (zeros): both eyes on the game lens, cockpit_fov_valid=0, counted.
    for(int i=0;i<4;++i)fakePose.eyes[1].fov[i]=0;
    contFrame();grab(4,f,b5);CHECK(!f.cockpit_fov_valid&&f.pose_valid&&b5==b2); // same pose as frames 2/3, game lens
    for(int e=0;e<2;++e)for(int i=0;i<4;++i)CHECK(f.cockpit_fov[e][i]==0.0f);
    // Frame 6: no pose: neither FOV nor a fallback count (not a posed frame).
    fakePose.available=false;contFrame();CHECK(src->try_acquire_latest(5,f)&&!f.pose_valid&&!f.cockpit_fov_valid);src->release(f);
    ticks(60);
    CHECK(line("event=hmd-fov;outcome=applied;frame=3;why=hmd;","reference=game-lens(0.61520,0.61520,0.34600,0.34600);rt=256x144;stereo-frame=cockpit_fov_valid=1"));
    CHECK(line("event=hmd-fov;outcome=fallback;reason=fov-zero;","eyes=game-lens"));
    const auto text=dedicated();
    if(!hasText(text,"event=continuous-fov;key=gpu_copy_step5_hmd_fov;hmd=2;fallback=1;clamped=0;posed=5/6;last-fallback=fov-zero;","reference=game-lens;rt=256x144"))
        {std::fprintf(stderr,"%s\n",text.c_str());CHECK(false);}
    CHECK(text.find("event=hmd-fov-definitions;")==std::string::npos); // printed once at the first tick, and the key was off then
    acz::g_hmdFovTest=false;fakePose.available=true;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
// F2: the read-only pre-cockpit observer (fixture ordinal 1 precedes the selected members 2,4,5,7).
static void step5_precockpit_observe() {
    namespace pc=gpu_copy::precockpit;
    {   // Bounded state (unit): 32 records per generation + one marker; the queue counts losses.
        pc::State s;for(unsigned i=0;i<40;++i){if(s.admit(1,7)){pc::Record r;r.ordinal=i;s.push(r);}}
        CHECK(s.sampled==1&&s.records==33&&s.truncated==1);
        unsigned markers=0,draws=0;pc::Record r;while(s.pop(r)){markers+=r.marker;draws+=!r.marker;}CHECK(markers==1&&draws==32);
        for(uint64_t g=8;g<11;++g)CHECK(s.admit(1,g)==(g<10)); // generations 2 and 3 are sampled, the 4th waits for `due`
        s.due=true;CHECK(s.admit(1,11)&&!s.due&&s.sampled==4);
        for(unsigned i=0;i<200;++i)s.push(pc::Record{});CHECK(s.lost==200-pc::queue_records);
        // Line bounds: a worst-case record stays inside the 479-character payload (or is marked).
        pc::Record big;big.frame=big.list=big.generation=big.ordinal=big.first_member=big.pso=big.id=big.root=big.t10_va=UINT64_MAX;
        big.count=big.instances=big.start=big.start_instance=UINT32_MAX;big.base=INT32_MIN;big.state="binding-tracking-incomplete";
        big.klass="out-of-class-root-signature";big.srt="map-unmap-coverage-incomplete";big.vb=64;big.vb_bytes=UINT64_MAX;
        pc::Derived d;d.camera="recovered";d.ok=true;for(int k=0;k<3;++k){d.eye[k]=-1e9;d.t[k]=1e9;d.rel[k]=-2e9;d.relCam[k]=-2e9;}d.dist=3e9;d.scale=1e9;
        for(auto& c:d.clip)c=-1e30;char a[pc::line_bytes],b[pc::line_bytes];pc::format(big,d,a,b);
        CHECK(std::strlen(a)<pc::line_bytes&&std::strlen(b)<pc::line_bytes);
        pc::Record m;m.marker=true;pc::format(m,d,a,b);CHECK(std::string_view(a).rfind("precockpit;event=truncated;",0)==0&&!b[0]);
    }
    bootContinuous();
    contFrame();const auto ev0=frameEvents();const auto g0=gameDrawOps();CHECK(!acz::g_pc.frames);
    acz::g_precockpitTest=true;w->lists[0].ops.clear();
    contFrame();CHECK(frameEvents()==ev0); // the command stream is unchanged
    {const auto g1=gameDrawOps();CHECK(g1.size()==g0.size()&&g1.size()==8);for(size_t i=0;i<g1.size();++i)CHECK(sameGameDraw(g0[i],g1[i]));}
    CHECK(acz::g_pc.sampled==1&&acz::g_pc.records==1);
    contFrame();contFrame();CHECK(acz::g_pc.sampled==3&&acz::g_pc.records==3);
    contFrame();CHECK(acz::g_pc.sampled==3&&acz::g_pc.records==3&&acz::g_pc.frames==4); // the 4th sampled generation waits for the 5 s period
    ticks(60);contFrame();CHECK(acz::g_pc.sampled==4&&acz::g_pc.records==4);
    auto* src=acz::stereo::frame_source();CHECK(src->frames_published()==6);
    {acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(5,f)&&f.members==6&&!f.cockpit_fov_valid);src->release(f);}
    ticks(60);const auto text=dedicated();
    if(!hasText(text,"precockpit;event=draw;frame=1;","ordinal=1;first-member=2;")){std::fprintf(stderr,"%s\n",text.c_str());CHECK(false);}
    CHECK(hasText(text,"precockpit;event=draw;frame=4;","state=ok;class=selected;class-index=0;topo=0;ib=2;vb=64;t10=3;"));
    CHECK(hasText(text,"precockpit;event=draw;frame=1;","draw=i9,1,0,0,0;vb-view-vertices=8;"));
    CHECK(hasText(text,"precockpit-srt;frame=1;","ordinal=1;srt=read;camera=recovered;eye=10.000,5.000,-3.000;fwd=0.0000,0.0000,-1.0000;lens-tan=0.61520,0.34600;"));
    CHECK(hasText(text,"precockpit-srt;frame=1;","rel=0.000,0.000,30.000;rel-cam=right:0.000,up:0.000,fwd:-30.000;dist=30.000;m256-scale=16;"));
    CHECK(hasText(text,"precockpit-srt;frame=1;","verdict=origin-behind-camera;tris=not-read"));
    CHECK(hasText(text,"event=continuous-precockpit;key=gpu_copy_step5_precockpit_observe;","sampled=4;records=4;lost=0;truncated-frames=0;"));
    unsigned n=0;for(size_t at=0;(at=text.find("precockpit;event=draw;",at))!=std::string::npos;++at)++n;CHECK(n==4);
    for(const auto& l:w->lines)CHECK(l.find("precockpit")==std::string::npos); // dedicated log only
    acz::g_precockpitTest=false;
    step3Lines();
}
// F3: membership by class.
static void writeOtherCamera(uint64_t off) { // a visible draw rendered by a DIFFERENT camera (eye moved 5 units)
    const acz::Vec3 eye2{sceneEye[0]+5.0,sceneEye[1],sceneEye[2]};
    const acz::Pose pose{eye2,{0,0,0,1}};
    const acz::FovAngles fov{std::atan(-0.6152),std::atan(0.6152),std::atan(0.3460),std::atan(-0.3460)};
    const auto vp=acz::multiply(acz::viewMatrixFromPose(pose),acz::projectionFromFov(fov,2.0,1000.0));
    acz::Mat4 scale=acz::identity();scale[0]=scale[5]=scale[10]=16.0;
    const auto m256=acz::multiply(scale,acz::translation(eye2[0],eye2[1],eye2[2]-sceneDepth));const auto m64=acz::multiply(m256,vp);
    const auto m0=acz::multiply(m64,acz::ps2ViewportMatrix(acz::PS2_SCALE_X,acz::PS2_SCALE_Y,acz::PS2_CENTRE,{1.0,0.0}));
    auto* srt=w->resources[2].data()+off;for(size_t i=0;i<416;++i)srt[i]=uint8_t(i*3+1);
    putMatrix(srt,m0);putMatrix(srt+64,m64);putMatrix(srt+256,m256);putParams(srt);
}
static void bootByClass(bool gameRt=false) {
    acz::g_byClassTest=true;bootContinuous(gameRt);
    for(auto* o:{&altPsoObj,&bcPsoObj}){o->table=w->psoTable.data();o->kind=k_pso;}
    {auto& s=acz::state();std::lock_guard lock(s.mutex);const auto& base=s.pipelines.at(reinterpret_cast<ID3D12PipelineState*>(&w->pso));
     s.pipelines[altPso()]=base;auto& b=s.pipelines[bcPso()];b=base;
     b.output.depth_write=false;b.output.depth_func=UINT(D3D12_COMPARISON_FUNC_GREATER_EQUAL);} // the "second class": depth facts differ
    writeOtherCamera(0x2800);
}
static void bcFrame(const std::vector<VarDraw>& d){w->lists[0].ops.clear();varFrame(d);}
static acz::stereo::StereoFrame bcLatest(uint64_t id) {
    acz::stereo::StereoFrame f;auto* src=acz::stereo::frame_source();CHECK(src->try_acquire_latest(id-1,f)&&f.frame_id==id);src->release(f);return f;
}
static void step5_members_by_class() {
    bootByClass();
    auto* src=acz::stereo::frame_source();auto& s5=s5ref();
    CHECK(line("event=ring;","upload=393216;members=4;")&&s5.memberCap==128&&s5.commandCap==1100&&s5.slotUpload==131072);
    const float W=shape3.vpW,H=shape3.vpH;
    // (a) The standard frame: it starts at ordinal 1 (behind the camera, never replayed before); 6 (another
    // viewport: another target) is refused alone WITHOUT making the frame partial.
    bcFrame(standardDraws());{const auto f=bcLatest(1);CHECK(f.members==7&&!f.partial&&proxyDraws()==14);}
    // (b) The live ordinal shift: a SECOND-CLASS draw (other depth facts) first. By ordinal (key off for one
    // frame) the selected ordinal 7 now holds the other-viewport draw -> partial, and ordinal 1 is never
    // replayed; by class: every class draw is replayed, full frame.
    std::vector<VarDraw> shifted={{srtOffBehind,3,9,W,H}};for(const auto& x:standardDraws())shifted.push_back(x);
    acz::g_byClassTest=false;bcFrame(shifted);{const auto f=bcLatest(2);CHECK(f.members==7&&f.partial&&proxyDraws()==14);}
    acz::g_byClassTest=true;bcFrame(shifted);{const auto f=bcLatest(3);CHECK(f.members==8&&!f.partial&&proxyDraws()==16);}
    CHECK(s5.familyCount==1&&s5.family[0].pso==uint64_t(reinterpret_cast<uintptr_t>(bcPso()))&&s5.family[0].ordinal==1);
    // (c) The second class at a SELECTED ordinal (2): by ordinal 'signature-class-differs' -> partial; by class: full.
    std::vector<VarDraw> second={{srtOffBehind,0,9,W,H},{srtOffBehind,3,9,W,H}};{auto s=standardDraws();second.insert(second.end(),s.begin()+1,s.end());}
    acz::g_byClassTest=false;bcFrame(second);{const auto f=bcLatest(4);CHECK(f.partial&&f.members==6);}
    acz::g_byClassTest=true;bcFrame(second);{const auto f=bcLatest(5);CHECK(!f.partial&&f.members==8);}
    {auto& st=st1();std::lock_guard lock(st.mutex);auto x=st.s3.members[0].shape;x.pso.depth_write=false;x.pso.depth_func=UINT(D3D12_COMPARISON_FUNC_GREATER_EQUAL);
     acz::g_byClassTest=false;CHECK(std::string_view(acz::step5MemberWhy(st,s5,x))=="signature-class-differs");
     acz::g_byClassTest=true;CHECK(!acz::step5MemberWhy(st,s5,x));}
    // (d) Out of class (topology): refused alone mid-frame (not partial); at the start it is no frame start and
    // leaves no pending skip; a generation of out-of-class draws only is neither produced nor skipped.
    auto d=standardDraws();d[3].topo=D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;d[4].topo=0;
    const auto classBefore=s5.classReason[gpu_copy::step5::member_class];
    bcFrame(d);{const auto f=bcLatest(6);CHECK(f.members==6&&!f.partial);}
    CHECK(s5.classReason[gpu_copy::step5::member_class]==classBefore+1); // ordinal 4: out-of-class-topology (6: viewport, state)
    d=standardDraws();d[0].topo=D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;d[1].topo=0;
    const auto outBefore=s5.classOutStarts;const auto skippedBefore=src->frames_skipped();
    bcFrame(d);{const auto f=bcLatest(7);CHECK(f.members==6&&!f.partial);}CHECK(s5.classOutStarts==outBefore+1);
    d=standardDraws();for(auto& x:d)x.topo=D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    bcFrame(d);CHECK(src->frames_published()==7&&src->frames_skipped()==skippedBefore&&proxyDraws()==0);
    {auto& st=st1();std::lock_guard lock(st.mutex);auto x=st.s3.members[0].shape;x.topology=0;
     auto why=[&](gpu_copy::step3::Shape y)->std::string{const char* r=acz::step5MemberWhy(st,s5,y);return r?r:"";};
     {auto y=x;y.root_route^=0x10;CHECK(why(y)=="out-of-class-root-signature");}
     {auto y=x;y.ib_stride=4;CHECK(why(y)=="out-of-class-ib-stride");}
     {auto y=x;y.vb_stride=32;CHECK(why(y)=="out-of-class-vb-stride");}
     {auto y=x;y.pso_route=0x7777000;y.draw_ordinal=99;CHECK(why(y).empty());} // PSO object and ordinal are not class facts
     CHECK(gpu_copy::step5::member_category("out-of-class-topology")==gpu_copy::step5::member_class&&gpu_copy::step5::member_category("camera-eye-differs")==gpu_copy::step5::member_camera&&
         gpu_copy::step5::member_category("viewport-size-changed")==gpu_copy::step5::member_state&&gpu_copy::step5::member_category("rt-format-changed")==gpu_copy::step5::member_format&&
         gpu_copy::step5::member_category("om-descriptor-unproved")==gpu_copy::step5::member_descriptor&&gpu_copy::step5::member_category("member-cap")==gpu_copy::step5::member_cap);}
    // (e) A member rendered by ANOTHER camera: refused alone, the frame is partial (its pixels would be wrong).
    d=standardDraws();d[4].off=0x2800;
    bcFrame(d);{const auto f=bcLatest(8);CHECK(f.members==6&&f.partial);}
    CHECK(s5.classReason[gpu_copy::step5::member_camera]==1);
    {auto& st=st1();std::lock_guard lock(st.mutex);gpu_copy::step5::Scratch sc;
     CHECK(std::string_view(gpu_copy::step5::camera_differs(w->resources[2].data()+0x2800,s5.pf,sc))=="camera-eye-differs");
     CHECK(!gpu_copy::step5::camera_differs(w->resources[2].data()+offB,s5.pf,sc)&&!gpu_copy::step5::camera_differs(w->resources[2].data()+srtOffBehind,s5.pf,sc));}
    // (f) Caps: 130 class draws -> 128 members (commands 4 + 8 x 128 = 1028 <= 1100), stop at the cap, partial, counted.
    std::vector<VarDraw> many(130,VarDraw{srtOff,0,9,W,H});
    bcFrame(many);{const auto f=bcLatest(9);CHECK(f.members==128&&f.partial);}CHECK(proxyDraws()==256&&s5.classCapFrames==1);
    CHECK(s5.frame.commands==4+8*128);
    // The upload slot holds all 128 members' blobs (the last one is the member's substituted bytes).
    ticks(60);
    const auto text=dedicated();
    if(!hasText(text,"event=continuous-members;mode=by-class;frames=7;partial=2;cap-frames=1;","")){std::fprintf(stderr,"%s\n",text.c_str());CHECK(false);}
    CHECK(hasText(text,"event=continuous-members;mode=by-class;","camera:1,unreadable:0;out-of-class-starts=")&&hasText(text,"event=continuous-members;","caps=members:128,commands:1100,upload-per-slot:131072"));
    CHECK(hasText(text,"event=members-by-class-definitions;","admitted=selected-classes+family"));
    CHECK(line("event=class-family-admit;",";ordinal=1;")&&line("event=class-family-admit;","depth=1/0/7(selected:1/1/5)"));
    CHECK(phase()==int(acz::Step1Phase::continuous)&&verdicts()==0);
    acz::g_byClassTest=false;
    step3Lines();
}
// ------------------------------------------------------------- J-world-objects (gpu_copy_step5_world_objects)
// SYNTHETIC. Pure maths: the DrawACM_v SrtData record of stored-draw-manifest-2.md (6-digit published values,
// ABSOLUTE world coordinates) and mutations of it. Integration: a by-class frame with DrawACM-identity draws.
static void worldManifestRecord(uint8_t* srt) {
    const float A[16]={1770.20f,1488.73f,-184.332f,0.722849f, 1.27356f,-660.583f,1.02658f,-0.0040257f,
        -1112.04f,-1402.64f,176.209f,-0.690994f, 1.62439e8f,1.96515e8f,-7.5852e6f,95537.0f};
    const float C[16]={1.12308f,0.0369045f,0.722880f,0.722849f, 0.0368853f,-2.88911f,-0.0040259f,-0.0040257f,
        1.17464f,0.0554376f,-0.691024f,-0.690994f, -128738.4f,3787.21f,95539.1f,95537.0f};
    const float B[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 12120.1f,512.18f,99382.5f,1};
    for(size_t i=0;i<416;++i)srt[i]=uint8_t(i*7+3);
    std::memcpy(srt,A,64);std::memcpy(srt+64,C,64);std::memcpy(srt+256,B,64);
    const float p[4]={512.0f,-448.0f,2048.0f,2048.0f};std::memcpy(srt+400,p,16);
    const float one=1.0f;std::memcpy(srt+360,&one,4); // depth scale (+360) of the stored ACM records
}
static void step5_world_keys() {
    using namespace acz::diagnostics;
    CHECK(!Config{}.gpu_copy_step5_world_objects()&&!parse_config("").config.gpu_copy_step5_world_objects());
    for(const char* v:{"true","1"}){auto c=parse_config(std::string("gpu_copy_step5_world_objects=")+v);
        CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_world_objects()&&!c.config.gpu_copy_step5_continuous()&&!c.config.gpu_copy_step5_members_by_class());}
    for(const char* v:{"false","0"}){auto c=parse_config(std::string("gpu_copy_step5_world_objects=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.gpu_copy_step5_world_objects());}
    CHECK(parse_config("gpu_copy_step5_world_objects=on").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_world_objects=1\ngpu_copy_step5_world_objects=0").issue==ConfigIssue::duplicate);
    // The headset-test key set (17 keys) + the new one stays within the key cap (24) and the 2048-byte config limit.
    const char* keys[]={"log_enabled=true","diagnostic_ownership_pins=true","gpu_copy_witness_observation=true","gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true","gpu_copy_step3_whole_set=true","gpu_copy_step5_continuous=true","xr_stereo_output=true","xr_edge_feather=0.06",
        "gpu_copy_step5_members_by_class=true","gpu_copy_step5_hmd_fov=true","gpu_copy_step5_hud=true","xr_hud_layer=true","xr_hud_quad_distance_m=0.8",
        "xr_hud_scale=0.5","xr_world_quad_distance_m=500","gpu_copy_step5_units_per_metre=87","gpu_copy_step5_world_objects=true"};
    std::string cfg;for(const char* k:keys)cfg+=std::string(k)+"\n";
    {auto c=parse_config(cfg);CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_world_objects()&&c.config.gpu_copy_step5_members_by_class()&&
        c.config.gpu_copy_step5_continuous()&&c.config.gpu_copy_step5_units_per_metre()==87.0f);}
    CHECK(std::size(keys)<=max_config_keys&&cfg.size()<=max_config_bytes);
}
static void step5_world_rules() {
    using namespace gpu_copy::step5;
    std::array<uint8_t,416> raw{};worldManifestRecord(raw.data());
    // Provenance numbers of the manifest: the record passes the layout check; mutations are refused with their reason.
    CHECK(!world::record_check(raw.data()));
    auto mutated=[&](auto edit)->std::string{auto r=raw;edit(r.data());const char* why=world::record_check(r.data());return why?why:"";};
    CHECK(mutated([](uint8_t* p){const float nan=std::nanf("");std::memcpy(p+8,&nan,4);})=="world-record-nonfinite");
    CHECK(mutated([](uint8_t* p){const float inf=INFINITY;std::memcpy(p+256+4,&inf,4);})=="world-record-nonfinite");
    CHECK(mutated([](uint8_t* p){const float v=0.5f;std::memcpy(p+256+12,&v,4);})=="world-model-not-affine");   // B column 3
    CHECK(mutated([](uint8_t* p){const float v=2.0f;std::memcpy(p+256+60,&v,4);})=="world-model-not-affine");   // B[3][3] != 1
    CHECK(mutated([](uint8_t* p){const float v=512.001f;std::memcpy(p+400,&v,4);})=="world-screen-params");
    CHECK(mutated([](uint8_t* p){const float v=-447.0f;std::memcpy(p+404,&v,4);})=="world-screen-params");
    CHECK(mutated([](uint8_t* p){const float inf=INFINITY;std::memcpy(p+16,&inf,4);})=="world-record-nonfinite");        // A Inf
    CHECK(mutated([](uint8_t* p){const float v=1e13f;std::memcpy(p+48,&v,4);})=="world-record-nonfinite");              // A implausible
    // C (+64..+127) is NOT validated: NaN, Inf, a different w column or all zeros are accepted (the VS never reads it for these draws).
    CHECK(mutated([](uint8_t* p){const float nan=std::nanf("");std::memcpy(p+64+4,&nan,4);const float inf=INFINITY;std::memcpy(p+64+8,&inf,4);}).empty());
    CHECK(mutated([](uint8_t* p){const float v=0.8f;std::memcpy(p+64+12,&v,4);const float u=95538.0f;std::memcpy(p+64+60,&u,4);}).empty());
    CHECK(mutated([](uint8_t* p){std::memset(p+64,0,64);}).empty());
    // Depth scale (+360): 1.0 only (16.0 would also select the [-1,1] clamp); 0, other values and NaN are refused.
    for(float v:{16.0f,0.0f,2.0f,0.5f,std::nanf("")})
        CHECK(mutated([&](uint8_t* p){std::memcpy(p+360,&v,4);})=="world-record-depth-scale");
    // cb10 flag bit 8 (value 8) selects the fade branch, the only reader of +64: the pure rule (flags 258, 2, 7 observed; never 8).
    CHECK(!world::flags_check(258)&&!world::flags_check(2)&&!world::flags_check(7)&&std::string_view(world::flags_check(8))=="world-flags-fade-reads-c"&&world::flags_check(10));
    CHECK(mutated([](uint8_t* p){float a;std::memcpy(&a,p+12,4);a+=1e-7f;std::memcpy(p+12,&a,4);}).empty()); // 1e-7: inside the tolerance
    // Camera recovery from A alone, in absolute world units (the 6-digit published A quantises the 1.6e8 translation entries at ~500:
    // eye within 1 unit of the numpy value from the published C: -14359.85, 3491.80, 123218.06; the stored bytes give 0.015 units, see step5_world_stored).
    Scratch sc;world::Frame w;
    CHECK(!world::camera_setup(raw.data(),nullptr,w,sc)&&w.camera);
    CHECK(std::fabs(w.pf.cam.tanX-0.6152)<2e-3&&std::fabs(w.pf.cam.tanY-0.3460)<1e-3&&std::fabs(w.pf.cam.tanX/w.pf.cam.tanY-16.0/9.0)<2e-3);
    CHECK(std::fabs(w.pf.cam.eye[0]+14359.85)<1.0&&std::fabs(w.pf.cam.eye[1]-3491.80)<1.0&&std::fabs(w.pf.cam.eye[2]-123218.06)<1.0);
    CHECK(std::fabs(dot3(w.pf.cam.fwd,w.pf.cam.fwd)-1)<1e-12&&std::fabs(w.pf.cam.fwd[0]-0.722849)<1e-5&&std::fabs(w.pf.cam.fwd[2]+0.690994)<1e-5);
    CHECK(std::fabs(w.model[0]-12120.1)<1e-3&&std::fabs(w.eyeAbs[2]-123218.06)<1.0);
    // S is the analytic clip -> PS2-screen map (258.048 / 225.792 / centre 2048, z_s = b w with b = -255); no inverse(C).
    CHECK(w.pf.S[0]==258.048&&w.pf.S[5]==225.792&&w.pf.S[12]==2048.0&&w.pf.S[13]==2048.0&&std::fabs(w.pf.S[14]+255.0)<0.01&&w.pf.S[15]==1.0&&w.pf.S[10]==0.0);
    // Against the cockpit camera: the same orientation/lens passes, a rotated (1 degree) or re-lensed camera is refused.
    {Camera ck=w.pf.cam;world::Frame x;CHECK(!world::camera_setup(raw.data(),&ck,x,sc));
     Camera rot=ck;const double a=0.0175;
     for(int k=0;k<3;++k)rot.right[k]=ck.right[k]*std::cos(a)+ck.fwd[k]*std::sin(a);
     CHECK(std::string_view(world::camera_setup(raw.data(),&rot,x,sc))=="world-camera-axes-differ-from-cockpit");
     Camera lens=ck;lens.tanX*=1.001;CHECK(std::string_view(world::camera_setup(raw.data(),&lens,x,sc))=="world-camera-lens-differs-from-cockpit");
     auto deg=raw;std::memset(deg.data(),0,64);const char* why=world::camera_setup(deg.data(),nullptr,x,sc);CHECK(why&&std::strncmp(why,"world-camera-",13)==0&&!x.camera);
     auto noC=raw;std::memset(noC.data()+64,0,64);CHECK(!world::camera_setup(noC.data(),nullptr,x,sc)&&x.camera);   // C is irrelevant to the camera
     auto badZ=raw;{float z;std::memcpy(&z,badZ.data()+4*6,4);z+=5.0f;std::memcpy(badZ.data()+4*6,&z,4);}
     CHECK(std::string_view(world::camera_setup(badZ.data(),nullptr,x,sc))=="world-camera-depth-column"&&!x.camera);} // z column no longer b x w
    // A later record must carry the same A (bit-identical in the evidence; 1e-6 relative accepted, more refused). C is not compared.
    CHECK(!world::record_matches(raw.data(),w));
    {auto r=raw;float c;std::memcpy(&c,r.data()+48,4);c*=1.0f+5e-7f;std::memcpy(r.data()+48,&c,4);CHECK(!world::record_matches(r.data(),w));
     c*=1.0f+1e-3f;std::memcpy(r.data()+48,&c,4);CHECK(std::string_view(world::record_matches(r.data(),w))=="world-vp-differs");
     r=raw;float a;std::memcpy(&a,r.data()+8,4);a+=1.0f;std::memcpy(r.data()+8,&a,4);CHECK(std::string_view(world::record_matches(r.data(),w))=="world-vp-differs");
     r=raw;std::memset(r.data()+64,0xff,64);CHECK(!world::record_matches(r.data(),w));}
    // Poses: neutral -> D == 0 -> the raw bytes (also -0.0); a head offset moves the eye in ABSOLUTE world units along the camera axes.
    Neutral n;const acz::stereo::EyePose neutral[2]={eyePose(0,1.2f,0,0,gameFov),eyePose(0,1.2f,0,0,gameFov)};
    const double units=39.6;
    CHECK(!setup_poses(neutral,n,w.pf,sc,false,units)&&w.pf.valid);
    {auto z=raw;const float nz=-0.0f;std::memcpy(z.data()+64+4,&nz,4);std::array<uint8_t,416> out{};
     for(unsigned e=0;e<2;++e){CHECK(!world::substitute(z.data(),w.pf,e,out.data())&&out==z);}}
    for(int e=0;e<2;++e)for(int i=0;i<16;++i)CHECK(w.pf.D[e][i]==0.0);
    { // J-test8-fixes xr_world_objects_depth_fix: only +360 of the REPLAYED copy changes (2^-10); key off / non-float depth = the raw bytes
      using namespace acz::gpu_copy::step5;
      CHECK(world::depth_fix_scale==1.0f/1024.0f&&world::depth_fix_scale_for(false,40)==1.0f&&world::depth_fix_scale_for(true,40)==world::depth_fix_scale&&
            world::depth_fix_scale_for(true,20)==world::depth_fix_scale&&world::depth_fix_scale_for(true,45)==1.0f&&world::depth_fix_scale_for(true,55)==1.0f&&
            world::depth_fix_scale_for(true,0)==1.0f);
      for(unsigned e=0;e<2;++e){
        std::array<uint8_t,416> a{},b{};
        CHECK(!world::substitute(raw.data(),w.pf,e,a.data())&&!world::substitute(raw.data(),w.pf,e,b.data(),world::depth_fix_scale));
        float s;std::memcpy(&s,b.data()+360,4);
        CHECK(s==world::depth_fix_scale&&std::memcmp(a.data(),b.data(),360)==0&&std::memcmp(a.data()+364,b.data()+364,416-364)==0&&std::memcmp(a.data()+360,raw.data()+360,4)==0);
        CHECK(!world::record_check(raw.data())); // the live record keeps 1.0: the check still passes
      }
      // The compressed depth stays > 0 for every unclipped fragment and below any cockpit depth: depth = 2^-10 * (1/w - 255*2^-24) vs the cockpit 1/w - 15.9*2^-20.
      for(double wv:{50.0,600.0,5000.0,35680.0,65000.0}) {
        const double obj=world::depth_fix_scale*(1.0/wv-255.0*std::ldexp(1.0,-24));
        CHECK(obj>0.0&&obj<(1.0/600.0-15.94*std::ldexp(1.0,-20))*0.8);
      }
      // J-180 (Codex review P2): the compression CHANGES the homogeneous clip volume (clip z = h.z*s*2^-24 against clip w = h.w): the plane z <= w moves.
      // The depth model of A (Z0, b) is recovered per frame; the clip decision is a function of the view depth w alone. Tested on BOTH sides of z = w.
      CHECK(w.b==w.pf.cam.A&&w.z0>0&&w.b<0);
      const double Z0=16777216.0,B=-255.0; // the measured model: depth = 1/w - 255 * 2^-24
      const double fixedScale=double(world::depth_fix_scale);
      CHECK(world::clip_inside(Z0,B,2.0,1.0)&&world::clip_inside(Z0,B,2.0,fixedScale)&&!world::clip_changes(Z0,B,2.0,fixedScale));         // z < w both ways
      CHECK(!world::clip_inside(Z0,B,0.5,1.0)&&world::clip_inside(Z0,B,0.5,fixedScale)&&world::clip_changes(Z0,B,0.5,fixedScale));          // z/w ~ 2: original clipped, compressed inside
      CHECK(!world::clip_inside(Z0,B,0.9,1.0)&&world::clip_inside(Z0,B,0.9,fixedScale)&&world::clip_changes(Z0,B,0.9,fixedScale));
      CHECK(world::clip_inside(Z0,B,1.1,1.0)&&!world::clip_changes(Z0,B,1.1,fixedScale));
      CHECK(!world::clip_inside(Z0,B,-3.0,1.0)&&!world::clip_inside(Z0,B,-3.0,fixedScale));                                               // behind the eye: clipped both ways
      CHECK(!world::clip_inside(Z0,B,70000.0,1.0)&&!world::clip_inside(Z0,B,70000.0,fixedScale)&&!world::clip_changes(Z0,B,70000.0,fixedScale)); // beyond the near plane z >= 0 (w > Z0/255): both clip
      {const auto band=world::depth_fix_clip_band(Z0,B,fixedScale);
       CHECK(band.valid&&std::fabs(band.hi-1.0)<1e-3&&std::fabs(band.lo-fixedScale)<1e-4&&band.lo<band.hi);
       CHECK(world::clip_changes(Z0,B,band.hi*0.999,fixedScale)&&!world::clip_changes(Z0,B,band.hi*1.001,fixedScale));                   // the upper edge of the band
       CHECK(world::clip_changes(Z0,B,band.lo*1.001,fixedScale)&&!world::clip_changes(Z0,B,band.lo*0.999,fixedScale));                   // the lower edge
       CHECK(!world::depth_fix_clip_band(-1.0,B,fixedScale).valid&&!world::depth_fix_clip_band(Z0,1.0,fixedScale).valid&&!world::depth_fix_clip_band(Z0,B,1.0).valid);
       const auto fx=world::depth_fix_clip_band(w.z0,w.b,fixedScale);CHECK(fx.valid&&fx.lo<fx.hi);} // the recovered model of the fixture's own record
    }
    { // J-180 (Codex review P2): losses of ELIGIBLE scene draws vs out-of-scope (offscreen) refusals.
      using namespace acz::gpu_copy::step5;
      gpu_copy::step3::Shape scene{};scene.om_count=1;scene.game_rt_status="resolved";scene.game_rt=0x1000;scene.game_rt_width=1280;scene.game_rt_height=720;scene.game_rt_format=10;
      auto other=scene;other.game_rt=0x2000;
      auto two=scene;two.om_count=2;
      auto L=[&](const char* why,const gpu_copy::step3::Shape& s,bool ready=true){return world::loss_eligible(why,s,ready,0x1000,1280,720,10);};
      for(const char* eligible:{"world-cap","live-srt-unreadable","world-record-nonfinite","world-model-not-affine","world-camera-depth-column","world-vp-differs","world-pose-unavailable",
                                "world-flags-fade-reads-c","world-cb10-unknown","world-class-topology","binding-tracking-incomplete","world-rt-count-unsupported"})
          CHECK(L(eligible,scene));
      CHECK(!L("world-offscreen-target",scene)&&!L("world-offscreen-desc",scene)&&!L("world-offscreen-om-count",scene)); // out-of-scope categories
      CHECK(!L("viewport-size-changed",scene)&&!L("rt-format-changed",scene)&&!L("ds-format-changed",scene));           // another pass (the cockpit members' rule)
      CHECK(!L("world-cap",other)&&!L("live-srt-unreadable",other)&&!L("world-record-nonfinite",two)); // not the scene RT: out of scope whatever the refusal says
      CHECK(!L(nullptr,scene)&&L("world-cap",other,false)); // accepted draws are no loss; without a known scene RT nothing can be proven out of scope
      // WORLD-4 propdedupe: a record at a prop placement is drawn by the props pass: out of scope (target category), never a loss
      CHECK(world::category("world-dedupe-prop")==world::cat_target&&!L("world-dedupe-prop",scene));
      std::array<uint8_t,416> rec{};const float tr[3]={12120.092f,512.18f,132150.48f};std::memcpy(rec.data()+4*76,tr,12);
      CHECK(::acz::props::dedupe_record_why(rec.data())==nullptr);                   // nothing published: every record kept
      ::acz::props::publish_dedupe({12120.092f,512.1821f,132150.48f, -5.0f,0.0f,7.0f},2.0f);
      CHECK(std::strcmp(::acz::props::dedupe_record_why(rec.data()),"world-dedupe-prop")==0);
      const float tr2[3]={12120.092f,512.18f,132153.0f};std::memcpy(rec.data()+4*76,tr2,12);
      CHECK(::acz::props::dedupe_record_why(rec.data())==nullptr);                   // 2.5 units off on z: an aircraft/unit there is kept
      ::acz::props::clear_dedupe();
    }
    const acz::stereo::EyePose moved[2]={eyePose(0.1f-0.0315f,1.2f,0,0,gameFov),eyePose(0.1f+0.0315f,1.2f,0,0,gameFov)};
    CHECK(!setup_poses(moved,n,w.pf,sc,false,units));
    const Camera cam=w.pf.cam;
    for(unsigned e=0;e<2;++e) {
        std::array<uint8_t,416> out{};CHECK(!world::substitute(raw.data(),w.pf,e,out.data()));
        CHECK(std::memcmp(out.data()+64,raw.data()+64,416-64)==0&&std::memcmp(out.data(),raw.data(),64)!=0); // only A (+0..+63) changes: C, B (+256), params (+400) and every other byte unchanged
        float a32[16];std::memcpy(a32,out.data(),64);
        const double dx=(e?0.1+0.0315:0.1-0.0315)*units;
        double eyeE[3];for(int k=0;k<3;++k)eyeE[k]=cam.eye[k]+dx*cam.right[k];
        auto project=[&](const float* m,const double p[3],double c[4]){for(int k=0;k<4;++k)c[k]=double(m[12+k])+p[0]*double(m[k])+p[1]*double(m[4+k])+p[2]*double(m[8+k]);};
        // h = (p,1) x A': the shifted eye is the projection centre (h.x = h.y = h.w = 0; float32 at 1.6e8: w ~0.01, x/y a few tens of screen units).
        double h[4];project(a32,eyeE,h);
        CHECK(std::fabs(h[3])<0.5&&std::fabs(h[0])<200&&std::fabs(h[1])<200);
        // x_c/w = (h.x/h.w - 2048)/258.048 (the clip ndc); y likewise.
        auto clipNdc=[&](const double p[3],double& xc,double& yc,double& wv){double q[4];project(a32,p,q);wv=q[3];xc=(q[0]/q[3]-2048.0)/258.048;yc=(q[1]/q[3]-2048.0)/225.792;};
        double ahead[3];for(int k=0;k<3;++k)ahead[k]=eyeE[k]+60.0*cam.fwd[k];
        double xc,yc,wv;clipNdc(ahead,xc,yc,wv);CHECK(wv>50&&std::fabs(xc)<6e-3&&std::fabs(yc)<6e-3);
        // A point 60 units ahead of the GAME camera shifts by -dx/(60 tanX) in clip x (parallax); a far point barely moves.
        double p60[3],pfar[3];for(int k=0;k<3;++k){p60[k]=cam.eye[k]+60.0*cam.fwd[k];pfar[k]=cam.eye[k]+60000.0*cam.fwd[k];}
        clipNdc(p60,xc,yc,wv);CHECK(std::fabs(xc-(-dx/(60.0*cam.tanX)))<6e-3);
        clipNdc(pfar,xc,yc,wv);CHECK(std::fabs(xc-(-dx/(60000.0*cam.tanX)))<3e-3);
    }
    // Failures: an invalid PoseFrame and an out-of-range eye are refused without writing a usable result.
    {PoseFrame bad;std::array<uint8_t,416> out{};CHECK(std::string_view(world::substitute(raw.data(),bad,0,out.data()))=="world-pose-unavailable");
     CHECK(std::string_view(world::substitute(raw.data(),w.pf,2,out.data()))=="world-pose-unavailable");}
    // FOV agreement with the cockpit frame (both on the game lens, or the same HMD tangents).
    {PoseFrame ck,wf;CHECK(!world::fov_matches(wf,ck));ck.fovHmd=true;CHECK(std::string_view(world::fov_matches(wf,ck))=="world-fov-mode-differs-from-cockpit");
     wf.fovHmd=true;for(int e=0;e<2;++e)for(int i=0;i<4;++i)wf.fov[e].t[i]=ck.fov[e].t[i]=0.5;CHECK(!world::fov_matches(wf,ck));
     wf.fov[1].t[2]=0.5025;CHECK(std::string_view(world::fov_matches(wf,ck))=="world-fov-differs-from-cockpit");}
    // Categories (counted in the -world status line).
    CHECK(world::category("world-record-nonfinite")==world::cat_record&&world::category("world-model-not-affine")==world::cat_record&&
          world::category("world-screen-params")==world::cat_record&&world::category("world-record-depth-scale")==world::cat_record&&
          world::category("world-flags-fade-reads-c")==world::cat_cb10&&world::category("world-camera-depth-column")==world::cat_camera&&
          world::category("world-camera-degenerate")==world::cat_camera&&world::category("world-vp-differs")==world::cat_camera&&
          world::category("world-fov-differs-from-cockpit")==world::cat_camera&&world::category("world-pose-unavailable")==world::cat_pose&&
          world::category("world-class-topology")==world::cat_class&&world::category("live-srt-unreadable")==world::cat_unreadable&&
          world::category("world-cap")==world::cat_cap&&world::category("viewport-size-changed")==world::cat_state&&world::category(nullptr)==world::cat_state);
}
// ------------------------------------------------------------- J-world-objects F1-F4: stored DrawACM_v records (acm-shader-dataflow.md)
// The record bytes are the STORED ones of capture 11906 (draw 3 = pass 0 ordinal 3; pass 6 ordinal 35 whose +64..+127 is NOT a matrix).
// The oracle is the VS dataflow derived from the DXIL: h = (local,1) x B x A, w = h.w, ndc = ((h.xy/w) - 2048) * 2 / (512, -448),
// depth = (h.z/w) * rec[+360] * 2^-24. Nothing here was measured live.
#include "fixtures/acm_stored_records.h"
struct AcmVsOut {double nx=0,ny=0,w=0,depth=0;};
static AcmVsOut acmVsOracle(const double* A,const double* B,double scale360,const double p[3]) {
    double N[16];gpu_copy::step5::mul44(B,A,N);                       // N = B x A
    double h[4];for(int j=0;j<4;++j)h[j]=p[0]*N[j]+p[1]*N[4+j]+p[2]*N[8+j]+N[12+j];
    AcmVsOut o;o.w=h[3];
    o.nx=(h[0]/h[3]-2048.0)*2.0/512.0;o.ny=(h[1]/h[3]-2048.0)*2.0/-448.0;
    const double sc=(scale360==16.0)?std::ldexp(1.0,-24):scale360*std::ldexp(1.0,-24);
    double d=(h[2]/h[3])*sc;if(scale360==16.0&&h[3]>0)d=std::fmax(-1.0,std::fmin(1.0,d));
    o.depth=d;return o;
}
// The exact rigid map of the WORLD that makes the OLD camera (A) see what the NEW camera (e2, axes a2 = r2,u2,f2) sees:
// p' = e + sum_k ((p - e2) . a2_k) a_k. Row-vector 4x4 M: (p,1) x M = p'. A_exact = M x A.
static void acmRigid(const gpu_copy::step5::Camera& c,const double e2[3],const double a2[3][3],double M[16]) {
    const double* a[3]={c.right,c.up,c.fwd};
    double L[9];for(int i=0;i<3;++i)for(int j=0;j<3;++j){double s=0;for(int k=0;k<3;++k)s+=a2[k][i]*a[k][j];L[i*3+j]=s;}
    for(int i=0;i<3;++i){for(int j=0;j<3;++j)M[i*4+j]=L[i*3+j];M[i*4+3]=0;}
    for(int j=0;j<3;++j){double s=c.eye[j];for(int i=0;i<3;++i)s-=e2[i]*L[i*3+j];M[12+j]=s;}
    M[15]=1;
}
static void acmUseCamera(gpu_copy::step5::PoseFrame& pf,const double e2[3],const double a2[3][3]) {
    using namespace gpu_copy::step5;
    const Camera& c=pf.cam;
    build_vp(c.eye,c.right,c.up,c.fwd,-c.tanX,c.tanX,c.tanY,-c.tanY,c,pf.vpRef);
    build_vp(e2,a2[0],a2[1],a2[2],-c.tanX,c.tanX,c.tanY,-c.tanY,c,pf.vpEye[0]);
    for(int i=0;i<16;++i)pf.D[0][i]=pf.D[1][i]=pf.vpEye[0][i]-pf.vpRef[i];
    pf.valid=true;
}
static void step5_world_stored() {
    using namespace gpu_copy::step5;
    const auto d3=acm_fixture::acm_stored_record(acm_fixture::acm_stored_draw3_record_hex);
    const auto p6=acm_fixture::acm_stored_record(acm_fixture::acm_stored_pass6_ord35_record_hex);
    auto fl=[](const std::array<uint8_t,416>& r,unsigned i){return world::rf(r.data(),i);};
    // Fixture pins (the SHA-256 of the records is in the fixture header, computed at extraction).
    CHECK(std::fabs(fl(d3,0)-1770.203f)<1e-3f&&std::fabs(fl(d3,3)-0.7228491f)<1e-6f&&std::fabs(fl(d3,12)-1.62439e8f)<1e4f&&std::fabs(fl(d3,15)-95536.96f)<0.01f);
    CHECK(std::fabs(fl(d3,76)-12120.09f)<0.01f&&std::fabs(fl(d3,77)-512.1821f)<0.001f&&std::fabs(fl(d3,78)-99382.48f)<0.01f&&fl(d3,90)==1.0f);
    CHECK(fl(d3,100)==512.0f&&fl(d3,101)==-448.0f&&fl(d3,102)==2048.0f&&fl(d3,103)==2048.0f&&fl(p6,90)==1.0f);
    CHECK(std::memcmp(d3.data(),p6.data(),64)==0&&std::memcmp(d3.data()+64,p6.data()+64,64)!=0);   // A bit-identical, C is not
    CHECK(std::isnan(fl(p6,21))&&std::isnan(fl(p6,22))&&std::fabs(fl(p6,16)+2.9516f)<1e-3f);        // pass-6 +64..: garbage with NaNs
    // (1) both records pass the layout check (pass 6 despite NaN in C) and give the SAME camera; the draw-3 numbers match the document.
    CHECK(!world::record_check(d3.data())&&!world::record_check(p6.data()));
    Scratch sc;world::Frame w3,w6;
    CHECK(!world::camera_setup(d3.data(),nullptr,w3,sc)&&w3.camera);
    CHECK(!world::camera_setup(p6.data(),nullptr,w6,sc)&&w6.camera);
    const Camera& cam=w3.pf.cam;
    CHECK(std::fabs(cam.eye[0]+14359.693)<0.05&&std::fabs(cam.eye[1]-3491.785)<0.05&&std::fabs(cam.eye[2]-123218.140)<0.05);   // A-only eye of the document
    CHECK(std::fabs(cam.tanX-0.61517318)<1e-6&&std::fabs(cam.tanY-0.34603493)<1e-6&&std::fabs(w3.pf.S[14]+255.00778)<1e-4);
    for(int k=0;k<3;++k)CHECK(w3.pf.cam.eye[k]==w6.pf.cam.eye[k]&&w3.pf.cam.fwd[k]==w6.pf.cam.fwd[k]);
    // WORLD-1 (terrain registration from the game camera): the decomposed axes ARE the terrain's camera_frame convention (right-handed, +X east, -Z north, Y up):
    // the capture flew level on the mission-02 NE track, so fwd ~ (+0.72, small, -0.69), right = the horizontal (-fz, 0, fx) direction, up ~ +Y, right x up = -fwd.
    {const double* r=cam.right;const double* u=cam.up;const double* f=cam.fwd;
     const double cx=r[1]*u[2]-r[2]*u[1],cy=r[2]*u[0]-r[0]*u[2],cz=r[0]*u[1]-r[1]*u[0];
     std::printf("  world camera axes (stored draw 3): right %.4f %.4f %.4f up %.4f %.4f %.4f fwd %.4f %.4f %.4f\n",r[0],r[1],r[2],u[0],u[1],u[2],f[0],f[1],f[2]);
     CHECK(cx*f[0]+cy*f[1]+cz*f[2]<-0.999);
     const double hn=std::sqrt(f[0]*f[0]+f[2]*f[2]);
     CHECK(std::fabs(r[0]+f[2]/hn)<0.05&&std::fabs(r[2]-f[0]/hn)<0.05&&std::fabs(r[1])<0.05&&u[1]>0.95);}
    // The cockpit camera comparison at adoption: the C-derived camera of draw 3 (the old source, 0.015 units away in the eye) is the same
    // orientation and lens within the tolerances (axes dot >= 1-1e-5, lens 1e-4); the eye is not compared.
    {Scratch s2;double c64[16];load16(d3.data()+64,c64);Camera ck;CHECK(decompose(c64,ck));
     const double ex=cam.eye[0]-ck.eye[0],ey=cam.eye[1]-ck.eye[1],ez=cam.eye[2]-ck.eye[2];
     CHECK(std::sqrt(ex*ex+ey*ey+ez*ez)<0.05);                                  // measured 0.015
     CHECK(dot3(cam.right,ck.right)>=1-world::axes_dot_tol&&dot3(cam.up,ck.up)>=1-world::axes_dot_tol&&dot3(cam.fwd,ck.fwd)>=1-world::axes_dot_tol);
     CHECK(std::fabs(cam.tanX-ck.tanX)<=world::lens_rel_tol*ck.tanX&&std::fabs(cam.tanY-ck.tanY)<=world::lens_rel_tol*ck.tanY);
     world::Frame x;CHECK(!world::camera_setup(p6.data(),&ck,x,s2)&&x.camera);   // adoption of the pass-6 record against the cockpit camera
     Camera rot=ck;const double a=0.0175;for(int k=0;k<3;++k)rot.right[k]=ck.right[k]*std::cos(a)+ck.fwd[k]*std::sin(a);
     CHECK(std::string_view(world::camera_setup(d3.data(),&rot,x,s2))=="world-camera-axes-differ-from-cockpit");}
    // A later record shares A with the frame's reference even though C differs (pass 6 against draw 3): accepted; a different A refused.
    CHECK(!world::record_matches(p6.data(),w3)&&!world::record_matches(d3.data(),w6));
    {auto r=p6;float v=fl(r,4)+1.0f;std::memcpy(r.data()+16,&v,4);CHECK(std::string_view(world::record_matches(r.data(),w3))=="world-vp-differs");}
    // (2) the VS oracle reproduces the document's numbers for the first stored vertex of draw 3.
    double A3[16],B3[16],A6[16],B6[16];load16(d3.data(),A3);load16(d3.data()+256,B3);load16(p6.data(),A6);load16(p6.data()+256,B6);
    const double v0[3]={76.44140625,32.0457763671875,-2.1875};
    {const AcmVsOut o=acmVsOracle(A3,B3,1.0,v0);
     CHECK(std::fabs(o.w-35679.8196)<1e-3&&std::fabs(o.nx-0.048456)<1e-6&&std::fabs(o.ny+0.230936)<1e-6&&std::fabs(o.depth-1.28278e-5)<1e-9);}
    // (3) the substitution against the exact world transform, through the VS oracle. Points: the first stored vertices of draw 3 (w ~ 35,600)
    // and aircraft-sized local points for the pass-6 record (w ~ 50..70, about 70 units from the camera).
    const std::vector<std::array<double,3>> pts3={{76.44140625,32.0457763671875,-2.1875},{78.08984375,-3.53875732421875,66.546875},
        {-76.44140625,32.0457763671875,2.1875},{-74.79296875,-3.53875732421875,70.921875}};
    const std::vector<std::array<double,3>> pts6={{0,0,0},{5,2,-3},{-8,1,6},{3,-2,9}};
    struct Pose {const char* name;double move[3];double yaw,pitch;};   // move along the camera's right/up/forward, then yaw about up, pitch about right
    const Pose poses[]={{"right",{1,0,0},0,0},{"up",{0,1,0},0,0},{"fwd",{0,0,1},0,0},{"mixed",{2.5,-1,3},0,0},{"yaw",{0,0,0},0.0174533,0},
                        {"yaw5-pitch3-right",{1,0,0},5*3.14159265358979/180,3*3.14159265358979/180},{"identity",{0,0,0},0,0}};
    struct Rec {const char* name;const std::array<uint8_t,416>* raw;const double* A;const double* B;const std::vector<std::array<double,3>>* pts;world::Frame* w;};
    const Rec recs[]={{"draw3",&d3,A3,B3,&pts3,&w3},{"pass6",&p6,A6,B6,&pts6,&w6}};
    for(const Rec& R:recs) {
        PoseFrame pf=R.w->pf;const Camera& c=pf.cam;
        for(const Pose& P:poses) {
            // new camera: yaw about the old up axis, then pitch about the new right axis; eye moved along the OLD axes
            double r2[3],u2[3],f2[3],e2[3];
            const double cy=std::cos(P.yaw),sy_=std::sin(P.yaw),cp=std::cos(P.pitch),sp=std::sin(P.pitch);
            double r1[3],f1[3];for(int k=0;k<3;++k){r1[k]=c.right[k]*cy+c.fwd[k]*sy_;f1[k]=c.fwd[k]*cy-c.right[k]*sy_;}
            for(int k=0;k<3;++k){r2[k]=r1[k];u2[k]=c.up[k]*cp+f1[k]*sp;f2[k]=f1[k]*cp-c.up[k]*sp;
                e2[k]=c.eye[k]+P.move[0]*c.right[k]+P.move[1]*c.up[k]+P.move[2]*c.fwd[k];}
            const double a2[3][3]={{r2[0],r2[1],r2[2]},{u2[0],u2[1],u2[2]},{f2[0],f2[1],f2[2]}};
            acmUseCamera(pf,e2,a2);
            std::array<uint8_t,416> out{};CHECK(!world::substitute(R.raw->data(),pf,0,out.data()));
            CHECK(std::memcmp(out.data()+64,R.raw->data()+64,416-64)==0);   // B, C and everything else untouched
            if(std::string_view(P.name)=="identity"){CHECK(out==*R.raw);continue;} // identity pose: A' == A bit for bit
            double M[16],Aex[16],Af[16],Ad[16];acmRigid(c,e2,a2,M);mul44(M,R.A,Aex);load16(out.data(),Af);
            for(int i=0;i<16;++i){double s=0;for(int k=0;k<4;++k)s+=pf.D[0][(i/4)*4+k]*pf.S[k*4+(i%4)];Ad[i]=R.A[i]+s;}
            double eN=0,eNf=0,eW=0,eWf=0,eD=0,eDf=0;
            for(const auto& q:*R.pts) {
                const AcmVsOut ex=acmVsOracle(Aex,R.B,1.0,q.data()),ad=acmVsOracle(Ad,R.B,1.0,q.data()),af=acmVsOracle(Af,R.B,1.0,q.data());
                eN=std::max(eN,std::max(std::fabs(ad.nx-ex.nx),std::fabs(ad.ny-ex.ny)));eW=std::max(eW,std::fabs(ad.w-ex.w)/std::fabs(ex.w));eD=std::max(eD,std::fabs(ad.depth-ex.depth));
                eNf=std::max(eNf,std::max(std::fabs(af.nx-ex.nx),std::fabs(af.ny-ex.ny)));eWf=std::max(eWf,std::fabs(af.w-ex.w)/std::fabs(ex.w));eDf=std::max(eDf,std::fabs(af.depth-ex.depth));
            }
            // Measured (offline): the DOUBLE formulation A + D x S matches the exact transform to ndc <= 4.8e-7, w rel <= 1.4e-8 (draw 3) / 9e-8
            // (pass 6), depth <= 4e-13 / 2.6e-9 (the stored z column is b x w only to 1.7e-8). The FLOAT32 record (A' rounded: 1.6e8
            // translation entries quantised at 16) is exact to ndc 1.2e-6 for draw 3 (w ~ 35,600) but only ~1e-3 NDC (~1.3 px at 2560)
            // for the pass-6 aircraft ~70 units from the camera: an intrinsic limit of the float32 record, unrelated to this maths.
            const bool far3=R.raw==&d3;
            CHECK(eN<1e-6&&eW<2e-7&&eD<(far3?1e-11:1e-8));
            CHECK(eNf<(far3?3e-6:3e-3)&&eWf<(far3?1e-6:2e-4)&&eDf<(far3?1e-11:1e-5));
        }
    }
    // (4) via setup_poses (a real head offset about the neutral): the eyes are the exact camera moves along the camera axes.
    // (5) C independence: the corrupted-C record gives a bit-identical A' (and +64.. is copied untouched).
    // (6) refusals on the stored bytes.
    {auto bad=d3;const float nan=std::nanf("");std::memcpy(bad.data()+20,&nan,4);CHECK(std::string_view(world::record_check(bad.data()))=="world-record-nonfinite");
     bad=p6;const float inf=INFINITY;std::memcpy(bad.data()+60,&inf,4);CHECK(std::string_view(world::record_check(bad.data()))=="world-record-nonfinite");
     for(float v:{16.0f,0.0f,2.0f,std::nanf("")}){bad=d3;std::memcpy(bad.data()+360,&v,4);CHECK(std::string_view(world::record_check(bad.data()))=="world-record-depth-scale");}
     bad=d3;const float one=1.0f;std::memcpy(bad.data()+360,&one,4);CHECK(!world::record_check(bad.data()));}
}

static void writeWorldRecord(uint64_t off,const acz::Vec3& eye,double yaw=0.0,uint8_t fill=9) {
    auto* srt=w->resources[2].data()+off;for(size_t i=0;i<416;++i)srt[i]=uint8_t(i*5+fill);
    const acz::Pose pose{eye,{0,std::sin(yaw/2),0,std::cos(yaw/2)}};
    const acz::FovAngles fov{std::atan(-0.6152),std::atan(0.6152),std::atan(0.3460),std::atan(-0.3460)};
    const auto c=acz::multiply(acz::viewMatrixFromPose(pose),acz::projectionFromFov(fov,2.0,1000.0));
    // A = C x (PS2 screen map with the stored depth structure z_s = -255 w: the z-clip column is ignored by the real S)
    const auto a=acz::multiply(c,acz::ps2ViewportMatrix(acz::PS2_SCALE_X,acz::PS2_SCALE_Y,acz::PS2_CENTRE,{0.0,-255.0}));
    putMatrix(srt,a);putMatrix(srt+64,c);putMatrix(srt+256,acz::translation(0,0,0));
    const float p[4]={512.0f,-448.0f,2048.0f,2048.0f};std::memcpy(srt+400,p,16);
    const float one=1.0f;std::memcpy(srt+360,&one,4);
}
constexpr uint64_t offW=0x3000,offW2=0x3200,offWbad=0x3400,offWrot=0x3600,offWdiff=0x3800;
static const acz::Vec3 worldEye{0.0,0.0,30.0}; // ABSOLUTE camera of the world records (the cockpit camera sits at sceneEye)
static void bootWorld(bool gameRt=false) {
    acz::g_worldObjectsTest=true;
    acz::g_wevCb10Fixed=true;acz::g_wevCb10FixedValue=258;acz::g_wevCb10FixedUnknown=false;acz::wevReset(); // fixtures have no root-constant witness: a witnessed flags word (258) unless a test says otherwise
    fakePose.available=true;fakePose.eyes[0]=eyePose(0,1.2f,0,0,gameFov);fakePose.eyes[1]=eyePose(0,1.2f,0,0,gameFov);
    acz::stereo::set_pose_provider(&fakePose);
    bootByClass(gameRt);
    acmPsoObj.table=w->psoTable.data();acmPsoObj.kind=k_pso;
    {auto& s=acz::state();std::lock_guard lock(s.mutex);auto& b=s.pipelines[acmPso()];b=s.pipelines.at(reinterpret_cast<ID3D12PipelineState*>(&w->pso));
     b.drawcpt_identity_matched=false;b.acm_world_matched=true;b.output.depth_write=false;b.output.depth_func=UINT(D3D12_COMPARISON_FUNC_GREATER_EQUAL);}
    writeWorldRecord(offW,worldEye);writeWorldRecord(offW2,worldEye);
    writeWorldRecord(offWbad,worldEye);{const float v=511.0f;std::memcpy(w->resources[2].data()+offWbad+400,&v,4);}            // wrong +400 constants
    writeWorldRecord(offWrot,worldEye,0.3);                                                                                 // another camera orientation
    writeWorldRecord(offWdiff,{0.0,0.0,31.0});                                                                              // same orientation/lens, other eye: A|C differ
}
static void worldTail(std::vector<VarDraw>& d,std::initializer_list<uint64_t> offs) {for(uint64_t o:offs)d.push_back({o,4,9,shape3.vpW,shape3.vpH});}
static std::vector<Op> proxyOps() {std::vector<Op> v;for(const auto& o:w->lists[0].ops)if(o.t==Op::draw&&originOf(o.boundRtv)!=gameRtvHandle)v.push_back(o);return v;}
static void expectedWorldEye(const acz::Vec3& eye,double dxMetres,acz::Mat4& vp) {
    const double s=gpu_copy::step3::units_per_metre;
    const acz::Pose pose{{eye[0]+s*dxMetres,eye[1],eye[2]},{0,0,0,1}};
    const acz::FovAngles fov{std::atan(-0.6152),std::atan(0.6152),std::atan(0.3460),std::atan(-0.3460)};
    vp=acz::multiply(acz::viewMatrixFromPose(pose),acz::projectionFromFov(fov,2.0,1000.0));
}
// Astra issue-31 P2-8: the scene-target qualification of a world member is mandatory; a missing world snapshot (allocation / source failure) no longer drops it.
static void step5_world_scene_identity() {
    bootWorld();
    auto& s5=s5ref();
    CHECK(!s5.worldReady&&!s5.worldSource);                       // no snapshot in this fixture (the same state a failed allocation leaves)
    acz::g_wobjSceneOptionalTest=false;
    auto d=standardDraws();worldTail(d,{offW,offW2});
    bcFrame(d);{const auto f=bcLatest(1);CHECK(f.members==7&&proxyDraws()==14);} // cockpit only: both world draws refused (counted as lost), none replayed
    CHECK(std::string(s5.wobjLastWhy)=="world-offscreen-no-scene-source");
    acz::g_wobjSceneOptionalTest=true;
    acz::g_worldObjectsTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
}
static void step5_world_objects() {
    bootWorld();
    auto& s5=s5ref();auto* src=acz::stereo::frame_source();
    CHECK(s5.wobjFrames==0&&s5.wobjSeen==0);
    // (a) neutral pose: DrawACM draws after the cockpit members are replayed per eye with D == 0 -> the raw record bytes.
    // 7 cockpit members + 2 world members; the cockpit members and the game's own draws are untouched.
    auto d=standardDraws();worldTail(d,{offW,offW2});
    bcFrame(d);{const auto f=bcLatest(1);CHECK(f.members==9&&!f.partial&&proxyDraws()==18&&!f.world_incomplete&&f.world_lost==0);}
    CHECK(s5.wobjFrames==1&&s5.wobjRecordedSum==2&&s5.wobjSeen==2&&s5.wobjRefusedSum==0&&s5.wobjBeforeFrame==0&&s5.wobjLostSum==0&&s5.wobjIncompleteFrames==0);
    {const auto ops=proxyOps();CHECK(ops.size()==18);
     const uint8_t* raw=w->resources[2].data()+offW;
     for(size_t i=14;i<18;++i){const uint8_t* b=blobAt(ops[i].boundT10);CHECK(std::memcmp(b,raw,416)==0);CHECK(ops[i].boundT10!=gameVa(2)+offW);} // private upload copies, not the game's record
     CHECK(w->inconsistent==0);}
    // (b) a head offset (+-0.0315 m about the neutral p0): eye positions move in ABSOLUTE world units along the camera right axis.
    fakePose.eyes[0]=eyePose(-0.0315f,1.2f,0,0,gameFov);fakePose.eyes[1]=eyePose(0.0315f,1.2f,0,0,gameFov);
    w->lists[0].ops.clear();
    bcFrame(d);{const auto f=bcLatest(2);CHECK(f.members==9&&!f.partial&&f.pose_valid);}
    {const auto ops=proxyOps();CHECK(ops.size()==18);
     const uint8_t* raw=w->resources[2].data()+offW;
     for(unsigned e=0;e<2;++e) {
        const uint8_t* b=blobAt(ops[14+e].boundT10);
        CHECK(std::memcmp(b+64,raw+64,416-64)==0&&std::memcmp(b,raw,64)!=0); // only A changes; C (+64) is left as the game wrote it
        float a32[16];std::memcpy(a32,b,64);
        acz::Mat4 want;expectedWorldEye(worldEye,e?0.0315:-0.0315,want);
        // A' = (the eye's VP) x (the fixture's clip -> PS2 map, z_s = -255 w) to float32 accuracy
        const auto wantA=acz::multiply(want,acz::ps2ViewportMatrix(acz::PS2_SCALE_X,acz::PS2_SCALE_Y,acz::PS2_CENTRE,{0.0,-255.0}));
        double scaleA=0,errA=0;for(int i=0;i<16;++i){scaleA=std::max(scaleA,std::fabs(wantA[size_t(i)]));errA=std::max(errA,std::fabs(double(a32[i])-wantA[size_t(i)]));}
        CHECK(errA<2e-3*scaleA);
        // the shifted eye is the projection centre of A' (h.x = h.y = h.w = 0)
        const double ex=worldEye[0]+gpu_copy::step3::units_per_metre*(e?0.0315:-0.0315);
        double cl[4];for(int k=0;k<4;++k)cl[k]=double(a32[12+k])+ex*double(a32[k])+worldEye[2]*double(a32[8+k]);
        CHECK(std::fabs(cl[0])<1e-2&&std::fabs(cl[1])<1e-2&&std::fabs(cl[3])<1e-3);
     }
     CHECK(w->inconsistent==0); // the fake GPU's +0 / +64 agreement check on every replayed draw (cockpit and world)
    }
    // (c) refusals alone, never partial: wrong +400 constants (record), another camera orientation, another A|C; the good records around them are kept.
    s5.wobjLastWhy=nullptr;
    d=standardDraws();worldTail(d,{offWbad,offW,offWrot,offWdiff,offW});
    // J-180 (Codex review P2): the three refused eligible scene draws are LOSSES: not `partial` (the cockpit is complete), but the published frame says
    // world_incomplete with the count; the accepted frames around it say nothing.
    bcFrame(d);{const auto f=bcLatest(3);CHECK(f.members==9&&!f.partial&&proxyDraws()==18&&f.world_incomplete&&f.world_lost==3);}
    CHECK(s5.wobjLostSum==3&&s5.wobjIncompleteFrames==1);
    CHECK(s5.wobjReason[gpu_copy::step5::world::cat_record]==1&&s5.wobjReason[gpu_copy::step5::world::cat_camera]==2&&s5.wobjRefusedSum==3&&s5.wobjRecordedSum==6);
    CHECK(std::string_view(s5.wobjLastWhy)=="world-vp-differs");
    // (d) a draw BEFORE the first cockpit member of a generation is never a frame start (counted); a generation of world draws only
    // is neither produced nor skipped and records nothing.
    d.clear();worldTail(d,{offW});for(const auto& x:standardDraws())d.push_back(x);
    {const auto before=s5.wobjBeforeFrame;bcFrame(d);const auto f=bcLatest(4);CHECK(f.members==7&&!f.partial&&proxyDraws()==14&&s5.wobjBeforeFrame==before+1);}
    {d.clear();worldTail(d,{offW,offW2});const auto pub=src->frames_published(),skp=src->frames_skipped();const auto before=s5.wobjBeforeFrame;
     bcFrame(d);CHECK(src->frames_published()==pub&&src->frames_skipped()==skp&&proxyDraws()==0&&s5.wobjBeforeFrame==before+2);}
    // (e) cap (J-test8-fixes: 96): 110 world draws -> 96 recorded (world::draw_cap), 14 refused as `world-cap`.
    CHECK(gpu_copy::step5::world::draw_cap==96&&gpu_copy::step5::world::draw_cap+gpu_copy::step5::world::cockpit_reserve<=gpu_copy::step5::class_member_cap);
    d=standardDraws();for(int i=0;i<110;++i)worldTail(d,{offW});
    // J-180: the 14 cap losses are published as world_incomplete (the old fixture expected a silent complete-looking frame).
    bcFrame(d);{const auto f=bcLatest(5);CHECK(f.members==7+96&&!f.partial&&f.world_incomplete&&f.world_lost==14);}
    CHECK(s5.wobjReason[gpu_copy::step5::world::cat_cap]==14&&std::string_view(s5.wobjLastWhy)=="world-cap"&&s5.frame.commands==4+8*(7+96));
    // (f) no pose: the IPD row-3 fallback is cockpit-specific -> world draws refused (pose), cockpit frame unchanged.
    fakePose.available=false;
    d=standardDraws();worldTail(d,{offW,offW2});
    {const auto before=s5.wobjReason[gpu_copy::step5::world::cat_pose];bcFrame(d);const auto f=bcLatest(6);
     CHECK(f.members==7&&!f.pose_valid&&s5.wobjReason[gpu_copy::step5::world::cat_pose]==before+2&&f.world_incomplete&&f.world_lost==2);}
    fakePose.available=true;
    // (g) not provable state: the world draw's own viewport is another target -> state refusal, frame not partial.
    d=standardDraws();d.push_back({offW,4,9,128,72});worldTail(d,{offW});
    {const auto before=s5.wobjReason[gpu_copy::step5::world::cat_state];bcFrame(d);const auto f=bcLatest(7);CHECK(f.members==8&&!f.partial&&s5.wobjReason[gpu_copy::step5::world::cat_state]==before+1&&!f.world_incomplete&&f.world_lost==0);} // another viewport = another pass: out of scope, no loss
    // (h) a different class (strip topology off): refused as class; a record read outside the mapped span: unreadable.
    d=standardDraws();d.push_back({offW,4,9,shape3.vpW,shape3.vpH,D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP});d.push_back({offW,4,9,shape3.vpW,shape3.vpH,0});
    {const auto before=s5.wobjReason[gpu_copy::step5::world::cat_class];bcFrame(d);const auto f=bcLatest(8);CHECK(f.members==8&&s5.wobjReason[gpu_copy::step5::world::cat_class]==before+1&&f.world_incomplete&&f.world_lost>=1);}
    d=standardDraws();d.push_back({0x3f00,4,9,shape3.vpW,shape3.vpH});
    {const auto before=s5.wobjReason[gpu_copy::step5::world::cat_unreadable];bcFrame(d);const auto f=bcLatest(9);CHECK(f.members==7&&s5.wobjReason[gpu_copy::step5::world::cat_unreadable]==before+1&&f.world_incomplete&&f.world_lost==1);}
    // (i) KEY OFF (the default): DrawACM-identity draws are ignored entirely: no count, no command, no log line, no HUD interplay.
    acz::g_worldObjectsTest=false;
    {const auto seen=s5.wobjSeen,rec=s5.wobjRecordedSum,ref=s5.wobjRefusedSum,bf=s5.wobjBeforeFrame;
     d=standardDraws();worldTail(d,{offW,offW2});
     w->lists[0].ops.clear();bcFrame(d);const auto f=bcLatest(10);CHECK(f.members==7&&!f.partial&&proxyDraws()==14);
     CHECK(s5.wobjSeen==seen&&s5.wobjRecordedSum==rec&&s5.wobjRefusedSum==ref&&s5.wobjBeforeFrame==bf);}
    acz::g_worldObjectsTest=true;
    // (j) status / definitions / first-publication lines (dedicated log only).
    ticks(60);
    const auto text=dedicated();
    if(!hasText(text,"event=continuous-status-world;","key=gpu_copy_step5_world_objects;frames=")){std::fprintf(stderr,"%s\n",text.c_str());CHECK(false);}
    CHECK(hasText(text,"event=continuous-status-world;","refused=state:"));
    CHECK(hasText(text,"event=world-objects-definitions;part=1;","identity=DrawACM_v(VS hash+full sha256)")&&hasText(text,"event=world-objects-definitions;part=2;","no-ipd-fallback"));
    CHECK(hasText(text,"event=world-objects;outcome=first-published;frame=1;world-members=2;","0.0,30.0;model-translation=0.0,0.0,0.0;")&&
          hasText(text,"event=world-objects;outcome=first-published;","lens-tan=0.61520,0.34600;eye-offset-units="));
    for(const auto& l:w->lines)CHECK(l.find("world-objects")==std::string::npos&&l.find("status-world")==std::string::npos); // not mirrored to the shared log
    CHECK(phase()==int(acz::Step1Phase::continuous)&&verdicts()==0);
    acz::g_worldObjectsTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
// J-180 (Codex review P2, t10 selection): a DrawACM_v draw reads its record through the VERTEX-stage binding when the root has one of its own
// (here parameter 5), not through the cockpit's general binding (parameter 3, the GS-stage one); a cockpit draw never does.
static void step5_world_vs_binding() {
    bootWorld();
    auto& s5=s5ref();
    auto d=standardDraws();worldTail(d,{offW});const unsigned worldOrdinal=unsigned(d.size());
    auto bindVs=[&](unsigned ord){
        if(ord!=worldOrdinal)return;
        {auto& s=acz::state();std::lock_guard lock(s.mutex);auto* l=l3list0();l->t10_vs_parameter=5;l->t10_vs_table=false;}
        listAlias(0)->SetGraphicsRootShaderResourceView(5,gameVa(2)+offWdiff); // the VS binding names ANOTHER record than the general binding (offW)
        {auto& s=acz::state();std::lock_guard lock(s.mutex);l3list0()->vs_source.address=gameAddr(2);}
    };
    // (a) with the vertex-stage binding the world draw replays the record of parameter 5
    beforeDraw=bindVs;bcFrame(d);beforeDraw=nullptr;
    {const auto f=bcLatest(1);CHECK(f.members==8&&!f.partial&&!f.world_incomplete);}
    {const auto ops=proxyOps();CHECK(ops.size()==16);
     const uint8_t* viaVs=w->resources[2].data()+offWdiff;const uint8_t* viaGeneral=w->resources[2].data()+offW;CHECK(std::memcmp(viaVs,viaGeneral,416)!=0);
     for(size_t i=14;i<16;++i)CHECK(std::memcmp(blobAt(ops[i].boundT5),viaVs,416)==0);}
    // (b) without it (a root with a single t10 binding: the previous behaviour) the general binding is used
    w->lists[0].ops.clear();bcFrame(d);
    {const auto f=bcLatest(2);CHECK(f.members==8&&!f.world_incomplete);}
    {const auto ops=proxyOps();CHECK(ops.size()==16);for(size_t i=14;i<16;++i)CHECK(std::memcmp(blobAt(ops[i].boundT10),w->resources[2].data()+offW,416)==0);}
    CHECK(s5.wobjRecordedSum==2&&s5.wobjRefusedSum==0);
    acz::g_worldObjectsTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
// HUD interplay: a DrawACM-identity draw in the tail is a HUD candidate with the key off (the game's PSO is just another
// non-DrawCPT PSO) and is excluded from the HUD replay with the key on (it belongs to the world seam, accepted or refused).
static void step5_world_hud() {
    hudSetup(true);
    acmPsoObj.table=w->psoTable.data();acmPsoObj.kind=k_pso;
    hudPipeline(acmPso(),nullptr);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.pipelines[acmPso()].acm_world_matched=true;}
    writeWorldRecord(offW,worldEye);
    auto* src=acz::stereo::frame_source();
    const auto tail=[&]{
        hudBindScene();hudDraws(2);
        auto* l=listAlias(0);l->SetPipelineState(acmPso());bindT10(offW);
        l->DrawIndexedInstanced(9,1,0,0,0);l->DrawIndexedInstanced(9,1,0,0,0);
        l->SetPipelineState(fakePso(false));bindT10(srtOff);
        hudDraws(2);hudOutput();
    };
    // KEY OFF: the two DrawACM-identity draws are replayed as HUD draws like the other four.
    hudFrame(0,tail);
    acz::stereo::StereoFrame f;CHECK(src->try_acquire_latest(0,f)&&f.frame_id==1&&f.hud_valid&&f.hud_draws==6&&f.hud_refused==0);src->release(f);
    CHECK(s5ref().wobjSeen==0);
    // KEY ON (this fixture is not by-class: the world draws are refused as `world-class-needs-members-by-class`, still not HUD draws).
    acz::g_worldObjectsTest=true;
    hudFrame(0,tail);
    CHECK(src->try_acquire_latest(1,f)&&f.frame_id==2&&f.hud_valid&&f.hud_draws==4&&f.hud_refused==0);src->release(f);
    CHECK(s5ref().wobjSeen==2&&s5ref().wobjRefusedSum==2&&s5ref().wobjRecordedSum==0&&std::string_view(s5ref().wobjLastWhy)=="world-class-needs-members-by-class");
    acz::g_worldObjectsTest=false;
}
// ------------------------------------------------------------- step-5 member flags equivalence (clear / snapshot / HUD-arm split)
// The `first` flag of step5RecordMember (and the HUD arm) was split into three flags. The recorded command stream of
// whole frames must be IDENTICAL to the one recorded before the split: a digest of every recorded op (handles by origin,
// private objects by role) and event, captured from the unsplit code and compared here.
static uint64_t flagsDigest() {
    uint64_t h=1469598103934665603ull;
    auto mix=[&](uint64_t v){for(int i=0;i<8;++i){h^=(v>>(8*i))&0xffu;h*=1099511628211ull;}};
    const auto& L=w->lists[0];
    for(const auto& e:L.events)for(unsigned char c:e)mix(c);
    for(const auto& o:L.ops) {
        mix(unsigned(o.t));mix(o.count);mix(o.single);mix(originOf(o.rtv[0]));mix(o.dsvSet);mix(originOf(o.dsv));mix(o.param);mix(o.va);
        uint32_t d;std::memcpy(&d,&o.depth,4);mix(d);mix(o.clearFlags);mix(o.res==&gameRt().object);mix(o.res!=nullptr);mix(o.before);mix(o.after);
        mix(o.dc);mix(o.di);mix(o.ds);mix(o.dsi);mix(uint32_t(o.db));mix(o.indexed);mix(o.box);mix(o.subres);mix(o.boundCount);
        mix(originOf(o.boundRtv));mix(o.boundDsvSet);mix(originOf(o.boundDsv));mix(o.boundT10);
    }
    return h;
}
static void flagsCheck(uint64_t want,unsigned barriers,unsigned copies,unsigned clearRtv,unsigned clearDsv) {
    unsigned b=0,c=0,cr=0,cd=0;for(const auto& o:w->lists[0].ops){b+=o.t==Op::barrier;c+=o.t==Op::copy_tex;cr+=o.t==Op::clear_rtv;cd+=o.t==Op::clear_dsv;}
    const uint64_t got=flagsDigest();
    if(!want){std::fprintf(stderr,"flags digest=%016llx barriers=%u copies=%u clear-rtv=%u clear-dsv=%u (capture)\n",(unsigned long long)got,b,c,cr,cd);return;}
    if(got!=want||b!=barriers||c!=copies||cr!=clearRtv||cd!=clearDsv)
        std::fprintf(stderr,"flags digest=%016llx barriers=%u copies=%u clear-rtv=%u clear-dsv=%u\n",(unsigned long long)got,b,c,cr,cd);
    CHECK(got==want&&b==barriers&&c==copies&&cr==clearRtv&&cd==clearDsv);
}
static void step5_flags_by_class() { // clears only (no world source), three frames, with and without world draws
    bootWorld();auto d=standardDraws();
    bcFrame(d);flagsCheck(0xd002a6dad1216d6eull,0,0,2,2);
    d=standardDraws();worldTail(d,{offW,offW2});
    bcFrame(d);flagsCheck(0x3e0e87c707a8ddb8ull,0,0,2,2);
    CHECK(s5ref().wobjRecordedSum==2);
    acz::g_worldObjectsTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
}
static void step5_flags_world() { // world snapshot + clears at the first member only
    bootContinuous(true);
    contFrame();flagsCheck(0xb4bd97c6e29478dfull,6,1,2,2);
}
static void step5_flags_hud() { // HUD armed at the first member, region restarted by a later member
    hudSetup(true);
    hudFrame(3);flagsCheck(0x1f1a4cfd1deafb77ull,8,1,3,3);
    hudFrame(2,[&]{hudBindScene();hudDraws(2);hudLateMember();hudBindScene();hudDraws(1);hudOutput();});flagsCheck(0x9d1e6f69c96189dcull,8,1,4,4);
    acz::g_hudReplayTest=false;
}
// ------------------------------------------------------------- J-world-objects v2 step 1: gpu_copy_step5_world_observe
namespace wo=acz::gpu_copy::wobs;
static wo::Obs woObs(wo::Kind k,uint32_t ordinal,uint64_t list=0x10,uint64_t gen=7,uint64_t id=0) {
    wo::Obs o;o.kind=k;o.list=list;o.generation=gen;o.l3Ordinal=ordinal;o.id=id?id:ordinal;o.pso=0x500+ordinal;o.vs8=0xA0000000u+ordinal;return o;
}
static void step5_world_observe_rules() {
    using namespace acz::diagnostics;
    // ---- key: default OFF, parse like the other step-5 keys, independent of the others
    CHECK(!Config{}.gpu_copy_step5_world_observe()&&!parse_config("").config.gpu_copy_step5_world_observe());
    for(const char* v:{"true","1"}){auto c=parse_config(std::string("gpu_copy_step5_world_observe=")+v);
        CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_world_observe()&&!c.config.gpu_copy_step5_continuous()&&!c.config.gpu_copy_step5_world_objects()&&
              !c.config.gpu_copy_step5_members_by_class());}
    for(const char* v:{"false","0"}){auto c=parse_config(std::string("gpu_copy_step5_world_observe=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.gpu_copy_step5_world_observe());}
    CHECK(parse_config("gpu_copy_step5_world_observe=on").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_world_observe=1\ngpu_copy_step5_world_observe=0").issue==ConfigIssue::duplicate);
    // ---- State: positions of the other draw kinds relative to the ACM draws of one list recording
    {   // ordinals: other1 other2 ACM3 CPT4 ACM5 other6 CPT7 ACM8 other9 other10
        auto stBox=std::make_unique<wo::State>();auto& st=*stBox;unsigned idx=0;
        const wo::Kind seq[]={wo::kind_other,wo::kind_other,wo::kind_acm,wo::kind_cpt,wo::kind_acm,wo::kind_other,wo::kind_cpt,wo::kind_acm,wo::kind_other,wo::kind_other};
        unsigned want[10],n=0;for(unsigned i=0;i<10;++i){const bool srt=st.draw(woObs(seq[i],i+1),idx);if(seq[i]==wo::kind_acm)want[n++]=idx;CHECK(srt==(seq[i]==wo::kind_acm));}
        const auto* t=st.find(0x10,7);CHECK(t&&n==3&&want[0]==0&&want[1]==1&&want[2]==2);
        const auto& s=t->s;
        CHECK(s.draws==10&&s.acm==3&&s.cpt==2&&s.other==5&&s.firstAcm==3&&s.lastAcm==8&&s.firstCpt==4&&s.lastCpt==7);
        CHECK(s.cptBefore==0&&s.cptBetween==2&&s.sinceCpt==0&&s.otherBefore==2&&s.otherBetween==1&&s.sinceOther==2);
        CHECK(s.otherPsoBefore==2&&s.otherVsBefore==0xA0000002u&&s.otherPsoAfter==9&&s.otherVsAfter==0xA0000009u); // nearest other before the first ACM / first after the last
        CHECK(t->detailCount==3&&t->detail[1].ordinal==5&&t->detail[1].acmIndex==1);
        // a 4th ACM draw wants no SrtData copy (first 3 only); a later ACM draw resets the "after" facts
        CHECK(!st.draw(woObs(wo::kind_acm,11),idx)&&idx==3);CHECK(t->s.sinceOther==0&&t->s.otherPsoAfter==0&&t->s.otherBetween==3);
        CHECK(st.acmDraws==4&&st.cptDraws==2&&st.otherDraws==5&&st.acmNoFrame==4&&st.acmActive==0);
    }
    {   // the producer-frame facts: same list vs another list
        auto stBox=std::make_unique<wo::State>();auto& st=*stBox;unsigned idx=0;auto a=woObs(wo::kind_acm,1);a.frameActive=true;a.inFrameList=true;st.draw(a,idx);
        auto b=woObs(wo::kind_acm,1,0x20,3);b.frameActive=true;b.inFrameList=false;st.draw(b,idx);st.draw(b,idx);
        CHECK(st.acmActive==3&&st.acmActiveSameList==1&&st.acmActiveOtherList==2&&st.find(0x20,3)->s.otherListAcm==2&&st.find(0x10,7)->s.inFrameAcm==1);
    }
    {   // target identity changes within a list
        auto stBox=std::make_unique<wo::State>();auto& st=*stBox;unsigned idx=0;auto a=woObs(wo::kind_acm,1);a.target.rtv=0x9000;a.target.rt=0xAAA;a.target.dsv=0xA000;a.target.ds=0xBBB;st.draw(a,idx);
        auto b=a;st.draw(b,idx);auto c=a;c.target.rt=0xCCC;st.draw(c,idx);auto d=a;d.target.ds=0xDDD;d.target.sceneRt=true;st.draw(d,idx);
        const auto& s=st.find(0x10,7)->s;CHECK(s.rtChanges==1&&s.dsChanges==1&&s.sceneAcm==1&&s.firstTarget.rt==0xAAA);
    }
    // ---- windows: a carrier list (ACM only) executed before the cockpit list; first 3 windows sampled, then waits for `due`
    {
        auto stBox=std::make_unique<wo::State>();auto& st=*stBox;unsigned idx=0;
        auto rec=[&](uint64_t list,uint64_t gen,unsigned acm,unsigned cpt){
            for(unsigned i=0;i<acm;++i){st.draw(woObs(wo::kind_acm,i+1,list,gen),idx);}for(unsigned i=0;i<cpt;++i)st.draw(woObs(wo::kind_cpt,acm+i+1,list,gen),idx);};
        auto exec=[&](std::initializer_list<std::pair<uint64_t,uint64_t>> lists,uint64_t queue=0x77){
            st.begin_call();unsigned i=0;for(const auto& l:lists){st.execute(l.first,l.second,queue,i,unsigned(lists.size()),false);++i;}};
        auto count=[&](wo::RecordType t){unsigned n=0;for(uint64_t i=st.tail;i<st.head;++i)n+=st.queue[i%wo::queue_records].type==t;return n;};
        rec(0x30,1,4,0);rec(0x31,1,2,6);exec({{0x30,1},{0x31,1}});
        CHECK(st.windows==1&&st.sampledWindows==1&&st.acmLists==2&&st.cockpitLists==1&&st.carrierLists==1&&st.windowsWithCarriers==1&&!st.open);
        CHECK(count(wo::rec_list)==2&&count(wo::rec_frame)==1&&count(wo::rec_draw)==6&&count(wo::rec_srt)==3);
        wo::Record r;wo::Record frame;bool gotFrame=false;while(st.pop(r)){if(r.type==wo::rec_frame){frame=r;gotFrame=true;}}
        CHECK(gotFrame&&frame.list==0x31&&frame.listsSince==2&&frame.acmLists==2&&frame.carrierLists==1&&frame.acmTotal==6&&frame.acmInCockpit==2&&frame.listCount==2&&
              frame.lists[0].list==0x30&&frame.lists[0].acm==4&&frame.lists[1].cpt==6&&frame.sameQueue==2&&frame.prevCockpitSeq==0&&frame.execSeq==1);
        // the same-list ACM-before-CPT case (no carrier): the degenerate window
        rec(0x32,1,3,5);exec({{0x32,1}});CHECK(st.windows==2&&st.carrierLists==1&&st.windowsWithCarriers==1&&st.sampledWindows==2);
        // a carrier on ANOTHER queue is visible in the summary
        rec(0x33,1,1,0);rec(0x34,1,0,4);st.begin_call();st.execute(0x33,1,0x55,0,1,false);st.begin_call();st.execute(0x34,1,0x77,0,1,false);
        CHECK(st.windows==3&&st.sampledWindows==3&&st.windowsWithCarriers==2);
        unsigned w3=0;while(st.pop(r)){if(r.type==wo::rec_frame&&r.window==3){++w3;CHECK(r.sameQueue==1&&r.listsSince==2&&r.carrierLists==1);}}CHECK(w3==1);
        // window 4 is not sampled until `due`: lifetime totals still count, no records
        const auto recs=st.records;rec(0x35,1,2,2);exec({{0x35,1}});CHECK(st.windows==4&&st.sampledWindows==3&&st.records==recs&&st.acmLists==5);
        st.due=true;rec(0x36,1,2,2);exec({{0x36,1}});CHECK(st.windows==5&&st.sampledWindows==4&&!st.due&&st.records>recs);
        // an untracked or non-matching execute is counted; lists with neither ACM nor CPT draws leave no record and no window
        const auto before=st.records,windows=st.windows;st.begin_call();st.execute(0x99,9,0x77,0,1,false);CHECK(st.untracked==1);
        rec(0x37,1,0,0);st.draw(woObs(wo::kind_other,1,0x37,1),idx);st.begin_call();st.execute(0x37,1,0x77,0,1,false);CHECK(st.records==before&&st.windows==windows&&!st.find(0x37,1));
        // Reset discards unexecuted recordings of that list
        rec(0x38,1,1,0);rec(0x38,2,1,0);st.discard(0x38);CHECK(!st.find(0x38,1)&&!st.find(0x38,2)&&st.discarded==2);
    }
    // ---- bounds: 12 list lines + one marker per sampled window, 8 draw lines, 3 srt lines, losses counted, tally table evicts oldest
    {
        auto stBox=std::make_unique<wo::State>();auto& st=*stBox;unsigned idx=0;
        for(unsigned l=0;l<15;++l){st.draw(woObs(wo::kind_acm,1,0x100+l,1),idx);for(unsigned k=0;k<3;++k)st.draw(woObs(wo::kind_acm,2+k,0x100+l,1),idx);}
        st.begin_call();for(unsigned l=0;l<15;++l)st.execute(0x100+l,1,0x77,l,15,false);
        unsigned lists=0,draws=0,srts=0,markers=0,frames=0;wo::Record r;while(st.pop(r)){lists+=r.type==wo::rec_list;draws+=r.type==wo::rec_draw;srts+=r.type==wo::rec_srt;markers+=r.type==wo::rec_marker;frames+=r.type==wo::rec_frame;}
        CHECK(lists==wo::list_lines&&draws==wo::draw_lines&&srts==wo::srt_lines&&markers==1&&frames==0&&st.truncated==1&&st.open); // no cockpit list yet: the window stays open
        for(unsigned i=0;i<wo::queue_records+40;++i)st.push(wo::Record{});CHECK(st.lost==40);
        auto evBox=std::make_unique<wo::State>();auto& ev=*evBox;for(unsigned l=0;l<wo::list_cap+5;++l)ev.draw(woObs(wo::kind_acm,1,0x200+l,1),idx);CHECK(ev.evicted==5&&!ev.find(0x200,1)&&ev.find(0x200+wo::list_cap+4,1));
    }
    // ---- decoding and formatting: the manifest record, worst-case line bounds
    {
        std::array<uint8_t,416> raw{};worldManifestRecord(raw.data());
        wo::SrtCopy c;c.status="read";c.bytes=raw;wo::Derived d;acz::gpu_copy::step5::Scratch sc;wo::derive(c,d,sc);
        CHECK(d.ok&&std::string_view(d.record)=="ok"&&std::string_view(d.camera)=="recovered"&&std::fabs(d.eye[0]+14359.85)<1.0&&std::fabs(d.eye[2]-123218.06)<1.0&&
              std::fabs(d.tan[0]-0.6152)<2e-3&&std::fabs(d.model[0]-12120.1)<1e-3&&d.ac==wo::fnv32(raw.data(),128));
        c.status="map-unmap-coverage-incomplete";wo::derive(c,d,sc);CHECK(!d.ok&&std::string_view(d.camera)=="map-unmap-coverage-incomplete");
        c.status="read";c.bytes={};wo::derive(c,d,sc);CHECK(!d.ok&&std::string_view(d.record)!="ok"&&std::string_view(d.camera)!="recovered");
        wo::Record r;char a[wo::line_bytes],b[wo::line_bytes];
        r.type=wo::rec_srt;r.window=3;r.list=0x10;r.generation=7;r.acmIndex=1;r.srt.status="read";r.srt.bytes=raw;wo::derive(r.srt,d,sc);wo::format(r,d,a,b);
        CHECK(std::string_view(a).rfind("world-observe;event=acm-srt;win=3;list=10;gen=7;acm-index=1;srt=read;record=ok;camera=recovered;eye=-14360.",0)==0&&!b[0]&&!std::strstr(a,"truncated=1"));
        // worst case: every field at its maximum stays inside the line buffer, marked when it did not fit
        const uint64_t M=UINT64_MAX;
        wo::Record big;big.window=M;big.list=M;big.generation=M;big.execSeq=M;big.queue=M;big.draw.ordinal=UINT32_MAX;big.draw.acmIndex=UINT32_MAX;big.draw.pso=M;big.draw.id=M;
        big.draw.t10Va=M;big.draw.frameList=M;big.draw.frameGeneration=M;auto& t=big.draw.target;t.rtv=t.rt=t.dsv=t.ds=t.rtWidth=M;t.rtHeight=t.rtFormat=t.dsFormat=t.omCount=UINT32_MAX;
        t.vpW=t.vpH=1e30f;t.sceneRt=t.rtKnown=t.dsKnown=true;
        for(auto ty:{wo::rec_draw,wo::rec_list,wo::rec_frame,wo::rec_marker,wo::rec_srt}) {
            big.type=ty;auto& s=big.sum;s.draws=s.acm=s.cpt=s.other=s.firstAcm=s.lastAcm=s.firstCpt=s.lastCpt=s.cptBefore=s.cptBetween=s.sinceCpt=UINT32_MAX;
            s.otherBefore=s.otherBetween=s.sinceOther=s.rtChanges=s.dsChanges=s.sceneAcm=s.activeAcm=s.otherListAcm=s.inFrameAcm=s.distinctPso=UINT32_MAX;
            s.otherPsoBefore=s.otherPsoAfter=M;s.otherVsBefore=s.otherVsAfter=UINT32_MAX;s.psoSeen[0]=M;s.firstTarget=t;big.batch=big.batchCount=UINT32_MAX;
            big.prevCockpitSeq=M;big.listsSince=big.acmLists=big.carrierLists=big.acmTotal=big.acmInCockpit=big.sameQueue=big.droppedLists=UINT32_MAX;
            big.listCount=wo::summary_lists;for(auto& l:big.lists){l.list=l.generation=l.execSeq=l.queue=M;l.acm=l.cpt=l.other=l.batch=UINT32_MAX;}
            wo::Derived dd;dd.camera="recovered";dd.record="ok";for(int k=0;k<3;++k){dd.eye[k]=-1e12;dd.right[k]=dd.up[k]=dd.fwd[k]=-1.5;dd.model[k]=-1e12;}dd.tan[0]=dd.tan[1]=1e9;dd.ac=UINT32_MAX;
            wo::format(big,dd,a,b);CHECK(std::strlen(a)<wo::line_bytes&&std::strlen(b)<wo::line_bytes&&a[0]);
        }
        // typical values fit without truncation
        auto stBox=std::make_unique<wo::State>();auto& st=*stBox;unsigned idx=0;for(unsigned i=0;i<6;++i)st.draw(woObs(wo::kind_acm,i+1,0x7ff6a0b4c8,0x1f3),idx);
        auto cp=woObs(wo::kind_cpt,7,0x7ff6a0b4c8,0x1f3);st.draw(cp,idx);st.begin_call();st.execute(0x7ff6a0b4c8,0x1f3,0x1c8a0e30,0,2,true);
        while(st.pop(r)) {
            wo::format(r,d,a,b);CHECK(a[0]&&!std::strstr(a,"truncated=1")&&!std::strstr(b,"truncated=1"));
            if(r.type==wo::rec_frame)CHECK(std::string_view(a).find("lists-since-last-cockpit=1;acm-lists=1;carrier-lists=0;acm-total=6;acm-in-cockpit-list=6;lists-on-cockpit-queue=1;per-list=L7ff6a0b4c8.g1f3:a6/c1/o0@1.0")!=std::string::npos);
        }
    }
    // ---- the member phase flags (plan step 3): first_member() sets all three, nothing sets them for later members
    CHECK(acz::MemberPhase::first_member(true).clear&&acz::MemberPhase::first_member(true).snapshot&&acz::MemberPhase::first_member(true).hudArm);
    CHECK(!acz::MemberPhase::first_member(false).clear&&!acz::MemberPhase::first_member(false).snapshot&&!acz::MemberPhase::first_member(false).hudArm);
    CHECK(!acz::MemberPhase{}.clear&&!acz::MemberPhase{}.snapshot&&!acz::MemberPhase{}.hudArm);
    // ---- the headset key set (22 keys with the observer; +2 for xr_world_hold_last / xr_world_extend = 24, the cap) stays inside the 24-key cap
    const char* keys[]={"log_enabled=true","diagnostic_ownership_pins=true","gpu_copy_witness_observation=true","gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true","gpu_copy_step3_whole_set=true","gpu_copy_step5_continuous=true","xr_stereo_output=true","xr_edge_feather=0.06",
        "gpu_copy_step5_members_by_class=true","gpu_copy_step5_hmd_fov=true","gpu_copy_step5_hud=true","xr_hud_layer=true","xr_hud_quad_distance_m=0.8",
        "xr_hud_scale=0.5","xr_world_quad_distance_m=500","gpu_copy_step5_units_per_metre=87","gpu_copy_step5_hud_markers=true","xr_hud_markers_layer=true",
        "xr_world_in_projection=true","gpu_copy_step5_world_objects=true","gpu_copy_step5_world_observe=true","xr_world_hold_last=true","xr_world_extend=true"};
    std::string cfg;for(const char* k:keys)cfg+=std::string(k)+"\n";
    {auto c=parse_config(cfg);CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_world_observe()&&c.config.gpu_copy_step5_world_objects()&&c.config.gpu_copy_step5_continuous());}
    CHECK(std::size(keys)==24&&std::size(keys)<=max_config_keys&&cfg.size()<=max_config_bytes);
}
// Single list: ACM draws AFTER the cockpit draws of the cockpit list (the case the v1 replay handles). Key ON must not change one
// recorded command or one published frame; the log shows the list, its ACM draws, the decoded camera and the window summary.
static void step5_world_observe() {
    bootWorld(true);
    auto& s5=s5ref();auto* src=acz::stereo::frame_source();
    // Ordinal 5 (PSO index 1) is NOT a DrawCPT-identity draw here: another draw type inside the list (an "other" draw).
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.pipelines.at(overlayPso()).drawcpt_identity_matched=false;}
    auto d=standardDraws();worldTail(d,{offW,offW2});
    // key OFF reference frames
    bcFrame(d);const auto digOff=flagsDigest();const auto evOff=frameEvents();const auto fOff=bcLatest(1);
    CHECK(fOff.members==8&&proxyDraws()==16&&g_wo.acmDraws==0&&g_wo.executed==0);
    // key ON: the same frame
    acz::g_worldObserveTest=true;
    bcFrame(d);const auto digOn=flagsDigest();const auto evOn=frameEvents();const auto fOn=bcLatest(2);
    CHECK(fOn.members==8&&proxyDraws()==16&&digOn==digOff&&evOn==evOff&&!fOn.partial);
    CHECK(s5.wobjRecordedSum==4&&s5.wobjRefusedSum==0);
    ticks(60);
    {std::lock_guard lock(g_woMutex);
     CHECK(g_wo.acmDraws==2&&g_wo.cptDraws==7&&g_wo.otherDraws==1&&g_wo.acmLists==1&&g_wo.cockpitLists==1&&g_wo.carrierLists==0&&g_wo.executed==1&&g_wo.windows==1);
     CHECK(g_wo.acmActive==2&&g_wo.acmActiveSameList==2&&g_wo.acmActiveOtherList==0&&g_wo.acmNoFrame==0&&g_wo.noList==0&&g_wo.untracked==0);}
    const auto text=dedicated();
    if(!hasText(text,"world-observe;event=list;win=1;","role=cockpit;")){std::fprintf(stderr,"%s\n",text.c_str());CHECK(false);}
    if(std::getenv("ACZ_WO_DUMP")){size_t at=0;while((at=text.find("world-observe",at))!=std::string::npos){const size_t e=text.find('\n',at);std::fprintf(stderr,"%s\n",text.substr(at,e-at).c_str());at=e==std::string::npos?text.size():e;}}
    CHECK(hasText(text,"world-observe;event=list;win=1;","draws=10;acm=2;cpt=7;other=1;first-acm=9;last-acm=10;first-cpt=1;last-cpt=8;cpt-around-acm=before:7,between:0,after:0;"
                       "other-around-acm=before:1,between:0,after:0;"));
    CHECK(hasText(text,"world-observe;event=list;win=1;","producer-list=1;")); // the Execute seam runs before the producer's own accounting: its frame is still active
    CHECK(hasText(text,"world-observe;event=list-targets;win=1;","acm=2;rt-changes=0;ds-changes=0;"));
    CHECK(hasText(text,"world-observe;event=acm-draw;win=1;","ordinal=9;l3-ordinal=7;acm-index=0;")&&hasText(text,"world-observe;event=acm-draw;win=1;","ordinal=10;l3-ordinal=7;acm-index=1;"));
    CHECK(hasText(text,"world-observe;event=acm-draw;win=1;","frame-active=1;in-frame-list=1;"));
    CHECK(hasText(text,"world-observe;event=acm-srt;win=1;","acm-index=0;srt=read;record=ok;camera=recovered;eye=-0.000,0.000,30.000;right=1.0000,0.0000,0.0000;"));
    CHECK(hasText(text,"world-observe;event=acm-srt;win=1;","lens-tan=0.61520,0.34600;model=0.000,0.000,0.000;"));
    CHECK(hasText(text,"world-observe;event=frame;win=1;","lists-since-last-cockpit=1;acm-lists=1;carrier-lists=0;acm-total=2;acm-in-cockpit-list=2;"));
    CHECK(hasText(text,"event=world-observe-definitions;part=2;","window=lists-executed-since-the-previous-list-with-DrawCPT-draws"));
    CHECK(hasText(text,"event=continuous-world-observe;key=gpu_copy_step5_world_observe;acm-draws=2;cpt-draws=7;other-draws=1;lists-executed=1;acm-lists=1;cockpit-lists=1;carrier-lists=0;"));
    CHECK(hasText(text,"event=continuous-world-observe-activity;","acm-seen-with-frame-active=2(same-list:2,other-list:0);acm-seen-without-frame=0;"));
    for(const auto& l:w->lines)CHECK(l.find("world-observe")==std::string::npos); // dedicated log only
    // the world status line: camera recovery TOTALS across frames (this run recovered the world camera in both frames)
    CHECK(hasText(text,"event=continuous-world-camera;","camera=evaluated:2,recovered:2,failed:0,last-failure:none;seen-in-other-list-while-frame-active=0;"));
    CHECK(!hasText(text,"camera=not-recovered"));
    // a window beyond the first 3 (and without `due`) is counted but adds no records
    uint64_t recs=0,wins=0;
    {std::lock_guard lock(g_woMutex);g_wo.sampledWindows=wo::sampled_full;g_wo.due=false;recs=g_wo.records;wins=g_wo.windows;}
    bcFrame(d);
    {std::lock_guard lock(g_woMutex);CHECK(g_wo.windows==wins+1&&g_wo.records==recs&&g_wo.acmDraws==4);}
    // key OFF again: the tallies stay as they were
    acz::g_worldObserveTest=false;
    bcFrame(d);
    {std::lock_guard lock(g_woMutex);CHECK(g_wo.windows==wins+1&&g_wo.acmDraws==4&&g_wo.executed==2);}
    CHECK(src->frames_skipped()==0&&phase()==int(acz::Step1Phase::continuous)&&verdicts()==0);
    acz::g_worldObjectsTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
// t10-va=0 hunt: the root-signature diagnostics of the world observer (layout once per signature seen with an ACM PSO, the raw root-parameter state of the
// first draws). Direct calls with a fake ListState: an ACM-like root signature (manifest 2), with and without DENY flags, and a signature whose layout is missing.
static void step5_world_observe_root() {
    namespace wo=acz::gpu_copy::wobs;namespace m=acz::metadata;
    acz::woRootDiagReset();
    m::RootLayout x;x.flags=1|4|8|16;x.parameters.resize(4);
    x.parameters[0].kind=m::ParameterKind::table;x.parameters[0].visibility=m::Visibility::pixel;
    {m::DescriptorRange rg;rg.kind=m::BindingKind::srv;rg.count=1;rg.base_register=1;x.parameters[0].ranges.push_back(rg);}
    x.parameters[1].kind=m::ParameterKind::table;x.parameters[1].visibility=m::Visibility::pixel;
    {m::DescriptorRange rg;rg.kind=m::BindingKind::sampler;rg.count=1;rg.base_register=0;x.parameters[1].ranges.push_back(rg);}
    x.parameters[2].kind=m::ParameterKind::srv;x.parameters[2].visibility=m::Visibility::all;x.parameters[2].shader_register=10;
    x.parameters[3].kind=m::ParameterKind::constants;x.parameters[3].visibility=m::Visibility::all;x.parameters[3].shader_register=10;x.parameters[3].constant_count=2;
    auto acm=m::RootBindingMetadata::from_adapter(x);CHECK(acm.valid());
    auto lsBox=std::make_unique<acz::ListState>();auto& ls=*lsBox;
    ls.bindings.set_root(0xAAA,std::optional<m::RootBindingMetadata>(acm),5);
    ls.bindings.set_value(0,m::ParameterKind::table,0x111);
    ls.bindings.set_value(2,m::ParameterKind::srv,0xabc200);
    const uint32_t words[2]={1,2};ls.bindings.set_constants(3,0,words,2);
    wo::Obs o;o.list=0x77;o.generation=0x9;o.pso=0x4242;o.t10Va=0;o.t10Address=0;o.l3Ordinal=3;
    diagnostics::L3Observation::List l3{};l3.t10_parameter=UINT32_MAX;
    // key on: the signature's layout and the t10 rule's verdict
    acz::g_worldObserveTest=true;
    wo::RootDiagOut out;
    acz::woRootDiag_locked(ls,&l3,o,out);
    CHECK(out.root&&out.state);
    CHECK(out.rf.key==0xAAA&&out.rf.identity==5&&out.rf.layoutPresent&&out.rf.layoutValid&&out.rf.flags==29&&out.rf.paramCount==4&&out.rf.staticSamplers==0);
    CHECK(out.rf.t10Parameter==2&&!out.rf.t10Table&&out.rf.matches==1&&out.rf.worldMatches==1&&out.rf.cockpitMatches==0&&out.rf.worldKeyOn);
    CHECK(out.rf.p[2].kind==uint8_t(m::ParameterKind::srv)&&out.rf.p[2].vis==uint8_t(m::Visibility::all)&&out.rf.p[2].reg==10&&out.rf.p[0].r0base==1);
    CHECK(out.rs.rootKey==0xAAA&&out.rs.setCount==3&&out.rs.set[0].index==0&&out.rs.set[1].index==2&&out.rs.set[1].value==0xabc200&&out.rs.set[2].index==3&&
          out.rs.set[2].kind==uint8_t(m::ParameterKind::constants)&&out.rs.set[2].words==2&&out.rs.t10Parameter==UINT32_MAX&&out.rs.layoutPresent&&out.rs.pso==0x4242&&out.rs.ordinal==3);
    {auto st=std::make_unique<wo::State>();st->push_root(out);
     CHECK(st->records==0&&st->head==2); // queued, but not counted in the window / list record totals
     wo::Record r;wo::Derived d;char a[wo::line_bytes],b[wo::line_bytes];
     CHECK(st->pop(r)&&r.type==wo::rec_root);wo::format(r,d,a,b);
     CHECK(std::string_view(a).find("world-observe;event=acm-root;root=aaa;identity=5;layout=found;valid=1;rejection=none;flags=0x1d(ia:1,deny-vs:0,hs:1,ds:1,gs:1,ps:0);"
           "static-samplers=0;params=4;t10-param=2;t10-table=0;t10-offset=0;rule-matches=1(cockpit-rule:0,world-rule:1);world-key-on=1")==0);
     CHECK(std::string_view(b).find("world-observe;event=acm-root-params;root=aaa;params=4;0=table:ps:1r(t1x1s0),1=table:ps:1r(s0x1s0),2=srv:all:t10s0,3=constants:all:b10s0x2")==0);
     CHECK(!std::strstr(a,"truncated=1")&&!std::strstr(b,"truncated=1"));
     CHECK(st->pop(r)&&r.type==wo::rec_rootstate);wo::format(r,d,a,b);
     CHECK(std::string_view(a).find("world-observe;event=acm-root-state;n=0;list=77;gen=9;ordinal=3;pso=4242;root=aaa;layout=found;t10-param=-1;t10-table=0;t10-va=0;t10-address=0;"
           "set-parameters=3;set=0:table:111,2:srv:abc200,3:constants:2w")==0&&!std::strstr(a,"truncated=1"));}
    // the same signature again: no new layout record, state records for its first draws only (state_per_root)
    unsigned states=1;
    for(unsigned i=0;i<20;++i){wo::RootDiagOut again;acz::woRootDiag_locked(ls,&l3,o,again);CHECK(!again.root);states+=again.state;}
    CHECK(states==wo::state_per_root);
    // key off: the same layout, the verdict of the cockpit rule only (the ALL-visible t10 behind DENY_GS is not found)
    acz::woRootDiagReset();acz::g_worldObserveTest=false;
    {wo::RootDiagOut off;acz::woRootDiag_locked(ls,&l3,o,off);CHECK(off.root&&!off.rf.worldKeyOn&&off.rf.t10Parameter==UINT32_MAX&&off.rf.matches==0);}
    // a signature whose layout the tracking does not have: reported as MISSING (the cause would be a rejected / unobserved CreateRootSignature)
    acz::woRootDiagReset();
    {auto l2Box=std::make_unique<acz::ListState>();auto& l2=*l2Box;l2.bindings.set_root(0xBBB,std::nullopt,0);
     wo::RootDiagOut miss;acz::woRootDiag_locked(l2,nullptr,o,miss);
     CHECK(miss.root&&!miss.rf.layoutPresent&&!miss.rf.layoutValid&&miss.rs.t10Parameter==UINT32_MAX&&!miss.rs.layoutPresent);
     auto st=std::make_unique<wo::State>();st->push_root(miss);wo::Record r;wo::Derived d;char a[wo::line_bytes],b[wo::line_bytes];
     CHECK(st->pop(r));wo::format(r,d,a,b);CHECK(std::string_view(a).find("layout=MISSING;valid=0;")!=std::string::npos&&std::string_view(a).find("flags=0x0(")!=std::string::npos);}
    // an invalid layout keeps its rejection text
    acz::woRootDiagReset();
    {m::RootLayout bad;bad.static_sampler_count=1;auto invalid=m::RootBindingMetadata::from_adapter(bad);CHECK(!invalid.valid());
     auto l3Box=std::make_unique<acz::ListState>();auto& l4=*l3Box;l4.bindings.set_root(0xCCC,std::optional<m::RootBindingMetadata>(invalid),0);
     wo::RootDiagOut rej;acz::woRootDiag_locked(l4,nullptr,o,rej);CHECK(rej.root&&rej.rf.layoutPresent&&!rej.rf.layoutValid&&std::string_view(rej.rf.rejection).find("static samplers")==0);}
    // table full: the first root_cap signatures are recorded, later ones are not
    acz::woRootDiagReset();
    unsigned roots=0,statesTotal=0;
    for(unsigned i=0;i<wo::root_cap+4;++i) {
        auto lBox=std::make_unique<acz::ListState>();auto& lx=*lBox;lx.bindings.set_root(0x1000+i,std::nullopt,0);
        for(unsigned k=0;k<3;++k){wo::RootDiagOut r;acz::woRootDiag_locked(lx,nullptr,o,r);roots+=r.root;statesTotal+=r.state;}
    }
    CHECK(roots==wo::root_cap&&statesTotal==wo::state_total_cap);
    acz::woRootDiagReset();
}
// Carrier lists: DrawACM_v draws recorded in ANOTHER list while the cockpit frame is active (the case the v1 replay never sees).
// The observer must count them, name the list as a carrier and its Execute order, and change nothing the producer records.
static void recordCarrier(unsigned acm,unsigned other) {
    auto* l1=listAlias(1);CHECK(l1->Reset(allocator(1),acmPso())==S_OK);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);auto* l=acz::l3_list_locked(s.states.at(l1));CHECK(l);l->t10_parameter=3;l->t10_table=false;}
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{gameRtvHandle},dsv{gameDsvHandle};l1->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
    const D3D12_VIEWPORT vp{0,0,shape3.vpW,shape3.vpH,0,1};l1->RSSetViewports(1,&vp);
    l1->SetGraphicsRootShaderResourceView(3,gameVa(2)+offW);
    {auto& s=acz::state();std::lock_guard lock(s.mutex);acz::l3_list_locked(s.states.at(l1))->sources[0].address=gameAddr(2);}
    for(unsigned i=0;i<acm;++i)l1->DrawIndexedInstanced(9,1,0,0,0);
    if(other){l1->SetPipelineState(overlayPso());for(unsigned i=0;i<other;++i)l1->DrawIndexedInstanced(9,1,0,0,0);}
    CHECK(l1->Close()==S_OK);
}
static void step5_world_observe_carrier() {
    bootWorld(true);acz::g_worldObserveTest=true;
    auto& s5=s5ref();auto* src=acz::stereo::frame_source();
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.pipelines.at(overlayPso()).drawcpt_identity_matched=false;}
    CHECK(acz::hookCaptureCommandList(listAlias(1)));
    // the reference: the same cockpit list alone (no carrier)
    auto d=standardDraws();
    bcFrame(d);const auto fRef=bcLatest(1);const unsigned refDraws=proxyDraws();
    // mode 0: one batch {carrier, cockpit}; 1: two calls, carrier first; 2: cockpit first, then the carrier.
    // list 1 records 3 ACM + 1 other draw WHILE list 0's frame is active (before list 0's 4th draw).
    auto carrierFrame=[&](int mode){
        {std::lock_guard lock(g_woMutex);g_wo.due=true;} // sample this window
        w->lists[0].events.clear();w->lists[0].ops.clear();
        beforeDraw=[&](unsigned ordinal){if(ordinal==4)recordCarrier(3,1);};
        frameVar(d);beforeDraw=nullptr;
        CHECK(listAlias(0)->Close()==S_OK);
        if(mode==0)execute({cl(1),cl(0)});else if(mode==1){execute({cl(1)});execute({cl(0)});}else{execute({cl(0)});execute({cl(1)});}
        advance();(void)acz::step1Tick();
    };
    // (a) the producer is not affected: same published frames and replayed draws as the reference; the carrier's ACM draws are seen, counted as
    // 'another list while a frame was active', never replayed (the v1 world path is list-bound).
    uint64_t seq0=0,wins0=0;
    {std::lock_guard lock(g_woMutex);seq0=g_wo.execSeq;wins0=g_wo.windows;}
    carrierFrame(0);
    CHECK(src->frames_published()==2&&proxyDraws()==refDraws);
    {std::lock_guard lock(g_woMutex);
     CHECK(g_wo.acmDraws==3&&g_wo.acmActive==3&&g_wo.acmActiveOtherList==3&&g_wo.acmActiveSameList==0&&g_wo.carrierLists==1&&g_wo.windows==wins0+1&&
           g_wo.windowsWithCarriers==1&&g_wo.execSeq==seq0+1&&!g_wo.open);}
    CHECK(s5.wobjOtherList==3&&s5.wobjSeen==3&&s5.wobjRecordedSum==0&&s5.wobjRefusedSum==0);
    ticks(60);
    {const auto text=dedicated();
     if(std::getenv("ACZ_WO_DUMP")){size_t at=0;while((at=text.find("world-observe",at))!=std::string::npos){const size_t e=text.find('\n',at);std::fprintf(stderr,"%s\n",text.substr(at,e-at).c_str());at=e==std::string::npos?text.size():e;}}
     CHECK(hasText(text,"world-observe;event=list;win=2;","role=carrier;exec-seq=2;batch=0/2;"));
     CHECK(hasText(text,"world-observe;event=list;win=2;","role=cockpit;exec-seq=2;batch=1/2;"));
     CHECK(hasText(text,"world-observe;event=list;win=2;","draws=4;acm=3;cpt=0;other=1;first-acm=1;last-acm=3;"));
     CHECK(hasText(text,"world-observe;event=list;win=2;","other-around-acm=before:0,between:0,after:1;"));
     CHECK(hasText(text,"world-observe;event=list-targets;win=2;","acm=3;rt-changes=0;ds-changes=0;scene-rt-acm=3;acm-with-frame-active=3;acm-in-frame-list=0;acm-in-other-list-while-active=3;"));
     CHECK(hasText(text,"world-observe;event=acm-draw;win=2;","acm-index=0;")&&hasText(text,"world-observe;event=acm-draw;win=2;","frame-active=1;in-frame-list=0;"));
     CHECK(hasText(text,"world-observe;event=acm-srt;win=2;","srt=read;record=ok;camera=recovered;"));
     CHECK(hasText(text,"world-observe;event=frame;win=2;","lists-since-last-cockpit=2;acm-lists=1;carrier-lists=1;acm-total=3;acm-in-cockpit-list=0;lists-on-cockpit-queue=2;"));
     CHECK(hasText(text,"world-observe;event=frame;win=2;","per-list=L")&&hasText(text,"world-observe;event=frame;win=2;",":a3/c0/o1@2.0,L"));
     CHECK(hasText(text,"event=continuous-world-observe-activity;","acm-seen-with-frame-active=3(same-list:0,other-list:3);"));
     CHECK(hasText(text,"event=continuous-world-camera;","seen-in-other-list-while-frame-active=3;")); }
    // (b) two Execute calls, carrier first: the same window, the carrier's exec-seq is earlier than the cockpit's
    {std::lock_guard lock(g_woMutex);wins0=g_wo.windows;}
    carrierFrame(1);
    {std::lock_guard lock(g_woMutex);CHECK(g_wo.windows==wins0+1&&g_wo.windowsWithCarriers==2&&!g_wo.open&&g_wo.acmActiveOtherList==6);}
    ticks(1);
    {const auto text=dedicated();
     CHECK(hasText(text,"world-observe;event=list;win=3;","role=carrier;exec-seq=3;batch=0/1;")&&hasText(text,"world-observe;event=list;win=3;","role=cockpit;exec-seq=4;batch=0/1;"));
     CHECK(hasText(text,"world-observe;event=frame;win=3;","exec-seq=4;prev-cockpit-exec-seq=2;")&&hasText(text,"world-observe;event=frame;win=3;","lists-since-last-cockpit=2;acm-lists=1;carrier-lists=1;"));}
    // (c) the cockpit list executes FIRST, then the carrier: the carrier opens the NEXT window (attributed to the next cockpit list; the
    // exec-seq order in the log makes the ordering visible)
    {std::lock_guard lock(g_woMutex);wins0=g_wo.windows;}
    carrierFrame(2);
    {std::lock_guard lock(g_woMutex);CHECK(g_wo.windows==wins0+2&&g_wo.open&&g_wo.windowsWithCarriers==2&&g_wo.carrierLists==3);}
    CHECK(src->frames_published()==4&&src->frames_skipped()==0&&phase()==int(acz::Step1Phase::continuous)&&verdicts()==0);
    // (d) a Reset of list 1 before it executes discards its recording
    recordCarrier(2,0);{auto* l1=listAlias(1);CHECK(l1->Reset(allocator(1),acmPso())==S_OK);}
    {std::lock_guard lock(g_woMutex);CHECK(g_wo.discarded==1);}
    CHECK(listAlias(1)->Close()==S_OK);
    // (e) key OFF: ACM draws of another list are not observed at all
    acz::g_worldObserveTest=false;
    {uint64_t before=0;{std::lock_guard lock(g_woMutex);before=g_wo.acmDraws;}
     const auto world=s5.wobjOtherList;
     carrierFrame(0);{std::lock_guard lock(g_woMutex);CHECK(g_wo.acmDraws==before);}
     CHECK(s5.wobjOtherList==world+3);} // the world counter keeps counting with the world key
    acz::g_worldObjectsTest=false;acz::g_worldObserveTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
}
// ------------------------------------------------------------- J-world-objects v2: same-list early frame start
// DrawACM_v draws that write the scene RT come BEFORE the cockpit's DrawCPT draws in the SAME list (live step30). The first one opens the
// producer frame (slot, pose, eye clears, world camera from the record's +64 matrix); the list's first cockpit member adopts it (cockpit
// camera/lens verified, world snapshot, HUD region). Offline fakes only; nothing here was measured live.
static unsigned earlyProxyDraws(){const auto& L=w->lists[0];unsigned n=0;for(const auto& o:L.ops)if(o.t==Op::draw&&originOf(o.boundRtv)!=gameRtvHandle&&originOf(o.boundRtv)!=0x9100)++n;return n;}
static std::vector<VarDraw> acmFirst(std::initializer_list<uint64_t> offs) {
    std::vector<VarDraw> d;worldTail(d,offs);for(const auto& x:standardDraws())d.push_back(x);return d;
}
static void step5_world_early() {
    bootWorld(true);
    auto& s5=s5ref();auto* src=acz::stereo::frame_source();
    auto latest=[&]{return bcLatest(src->frames_published());};
    // the world snapshot needs a SUPPORTED entry state: the game's own transitions to RENDER_TARGET earlier in every generation
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    // (a) ACM, ACM, then the 7 cockpit members: the first ACM draw opens the frame, the first cockpit member adopts it.
    bcFrame(acmFirst({offW,offW2}));
    {const auto f=latest();CHECK(src->frames_published()==1&&f.members==2+7&&!f.partial&&f.world_valid&&f.pose_valid&&f.lens_valid&&earlyProxyDraws()==2*(2+7)&&!f.world_incomplete&&f.world_lost==0);}
    CHECK(s5.wobjEarlyOpened==1&&s5.wobjEarlyAdopted==1&&s5.wobjEarlyAbandoned==0&&s5.wobjEarlyMismatch==0&&s5.wobjFrames==1&&s5.wobjRecordedSum==2);
    CHECK(s5.wobjSeen==2&&s5.wobjBeforeFrame==0&&s5.wobjCamEval==1&&s5.wobjCamOk==1&&s5.wobjRefusedSum==0&&src->frames_skipped()==0);
    {const auto& L=w->lists[0];size_t firstClear=SIZE_MAX,copy=SIZE_MAX,firstProxy=SIZE_MAX;unsigned beforeCopy=0,clears=0;
     for(size_t i=0;i<L.ops.size();++i) {
         const auto& o=L.ops[i];
         if(o.t==Op::clear_rtv||o.t==Op::clear_dsv){++clears;firstClear=std::min(firstClear,i);}
         if(o.t==Op::copy_tex&&copy==SIZE_MAX)copy=i;
         if(o.t==Op::draw&&originOf(o.boundRtv)!=gameRtvHandle){firstProxy=std::min(firstProxy,i);if(copy==SIZE_MAX)++beforeCopy;}
     }
     // one set of clears (2 RTV + 2 DSV) BEFORE the first replay; the world snapshot after the 2 world members (4 eye draws), at the cockpit's first member
     CHECK(clears==4&&firstClear<firstProxy&&copy!=SIZE_MAX&&beforeCopy==4&&w->worldCopies==1);}
    {const auto ops=proxyOps();const uint8_t* raw=w->resources[2].data()+offW;
     CHECK(ops.size()==18&&std::memcmp(blobAt(ops[0].boundT10),raw,416)==0&&std::memcmp(blobAt(ops[1].boundT10),raw,416)==0); // neutral pose: raw bits, world members first
     CHECK(w->inconsistent==0);}
    // (b) a non-scene-RT ACM draw never opens a frame and is never replayed (same viewport size, other render target); a later scene-RT draw opens it.
    auto& other=w->resources[22];other=Resource{};other.object.table=w->resourceTable.data();other.object.kind=k_resource;
    other.dim=D3D12_RESOURCE_DIMENSION_TEXTURE2D;other.width=128;other.height=72;other.format=gameRt().format;other.heap=D3D12_HEAP_TYPE_DEFAULT;
    other.state=D3D12_RESOURCE_STATE_RENDER_TARGET;other.arraySize=1;other.big.assign(size_t(other.width)*other.height*fakeBytes(other.format),0);
    acz::step3NoteView(reinterpret_cast<ID3D12Resource*>(&other.object),0x9100,false);setView(0x9100,&other.object);
    const D3D12_CPU_DESCRIPTOR_HANDLE offRtv{0x9100},gameRtv{gameRtvHandle},dsv{gameDsvHandle};
    auto onTarget=[&](unsigned offscreenOrdinal){beforeDraw=[&,offscreenOrdinal](unsigned ord){listAlias(0)->OMSetRenderTargets(1,ord==offscreenOrdinal?&offRtv:&gameRtv,FALSE,&dsv);};};
    auto d=acmFirst({offW,offW2});d[0].vpW=128;d[0].vpH=72;
    onTarget(1);bcFrame(d);beforeDraw=nullptr;
    {const auto f=latest();CHECK(src->frames_published()==2&&f.members==1+7&&!f.partial&&earlyProxyDraws()==2*(1+7)&&!f.world_incomplete&&f.world_lost==0);} // offscreen: out of scope, no loss
    CHECK(s5.wobjEarlyOpened==2&&s5.wobjEarlyAdopted==2&&s5.wobjEarlyReason[gpu_copy::step5::world::cat_target]==1&&s5.wobjBeforeFrame==1&&
          std::string_view(s5.wobjEarlyLastWhy)=="world-offscreen-target");
    // the same on the member path: an offscreen ACM draw inside an open frame is refused alone as `target` (not replayed, frame not partial)
    d=acmFirst({offW,offW2});d[1].vpW=128;d[1].vpH=72;
    onTarget(2);bcFrame(d);beforeDraw=nullptr;
    {const auto f=latest();CHECK(src->frames_published()==3&&f.members==1+7&&!f.partial&&earlyProxyDraws()==2*(1+7)&&!f.world_incomplete&&f.world_lost==0);}
    CHECK(s5.wobjReason[gpu_copy::step5::world::cat_target]==1&&s5.wobjEarlyOpened==3&&s5.wobjEarlyAdopted==3);
    // an ACM-only list on the offscreen target alone: nothing opens, nothing is published or skipped
    d.clear();worldTail(d,{offW,offW2});d[0].vpW=d[1].vpW=128;d[0].vpH=d[1].vpH=72;
    beforeDraw=[&](unsigned){listAlias(0)->OMSetRenderTargets(1,&offRtv,FALSE,&dsv);};
    bcFrame(d);beforeDraw=nullptr;
    CHECK(src->frames_published()==3&&src->frames_skipped()==0&&s5.wobjEarlyOpened==3&&earlyProxyDraws()==0&&!s5.frame.active);
    // (c) an unreadable record (outside the mapped span) is refused alone at the start (lookup refusal kept); the next good record opens the frame
    d=acmFirst({0x3f00,offW});
    {const auto before=s5.wobjEarlyReason[gpu_copy::step5::world::cat_unreadable];bcFrame(d);
     // J-180 (Codex review P2): the unreadable draw was lost BEFORE the opener; the frame the next good record opens inherits that loss.
     const auto f=latest();CHECK(src->frames_published()==4&&f.members==1+7&&s5.wobjEarlyReason[gpu_copy::step5::world::cat_unreadable]==before+1&&s5.wobjEarlyOpened==4&&
                                 f.world_incomplete&&f.world_lost==1);}
    // (d) a world record with another camera orientation: the frame opens (nothing to compare yet) and the cockpit member's adoption REFUSES it:
    // the frame executes but is never published (the world members were recorded with the wrong camera); counted as adopt-camera-mismatch
    d=acmFirst({offWrot});
    {const auto drop=s5.poseDropped;bcFrame(d);
     CHECK(src->frames_published()==4&&src->frames_skipped()==1&&s5.poseDropped==drop+1&&s5.wobjEarlyMismatch==1&&s5.wobjEarlyOpened==5&&s5.wobjEarlyAdopted==4&&
           earlyProxyDraws()==2&&std::string_view(s5.wobjAdoptLastWhy)=="world-camera-axes-differ-from-cockpit"&&!s5.frame.active);}
    // the slot returns to the ring: the next frame is published
    bcFrame(acmFirst({offW}));CHECK(src->frames_published()==5&&latest().members==1+7);
    // (e) Reset before submission: the recording is discarded, the slot freed, nothing published
    {const auto disc=s5.discarded;
     w->lists[0].ops.clear();frameVar(acmFirst({offW}));
     CHECK(s5.frame.active&&s5.frame.early&&s5.frame.adopted);
     CHECK(listAlias(0)->Close()==S_OK);
     CHECK(listAlias(0)->Reset(allocator(0),fakePso())==S_OK);
     CHECK(!s5.frame.active&&s5.discarded==disc+1&&src->frames_published()==5);
     CHECK(listAlias(0)->Close()==S_OK);}
    // (f) no pose provider data: the early start is refused as `pose` (once per list), the cockpit starts the frame as before (world draws before it are not replayed)
    fakePose.available=false;
    {const auto before=s5.wobjEarlyReason[gpu_copy::step5::world::cat_pose];bcFrame(acmFirst({offW,offW2}));
     const auto f=latest();CHECK(src->frames_published()==6&&f.members==7&&!f.pose_valid&&s5.wobjEarlyReason[gpu_copy::step5::world::cat_pose]==before+1&&
                                 s5.wobjEarlyBlocked>=1&&s5.wobjEarlyOpened==7&&f.world_incomplete&&f.world_lost==2);} // both world draws lost before the cockpit opened the frame
    fakePose.available=true;
    // (g) caps (J-test8-fixes: 96): 110 world draws -> 96 recorded (the opener included), 14 refused as `world-cap`; commands = clears + 8 per member + the world snapshot
    {std::vector<VarDraw> big;for(int i=0;i<110;++i)worldTail(big,{offW});for(const auto& x:standardDraws())big.push_back(x);
     const auto cap=s5.wobjReason[gpu_copy::step5::world::cat_cap];const auto pn=s5.pitchAccepted.n+s5.pitchRefused.n;bcFrame(big);
     CHECK(s5.pitchAccepted.n+s5.pitchRefused.n==pn+1); // J-test8-fixes: the pitch/sub-condition diagnostics also run on the EARLY adopt path (once per adopted frame)
     const auto f=latest();CHECK(src->frames_published()==7&&f.members==96+7&&!f.partial&&s5.wobjReason[gpu_copy::step5::world::cat_cap]==cap+14&&
                                 f.world_incomplete&&f.world_lost==14); // J-180: the cap losses are published (the cockpit is still complete: !partial)
     CHECK(s5.frame.commands==4+8*(96+7)+5);}
    // (h) KEY OFF (world_objects): ACM draws before the cockpit are ignored entirely (no count, no frame start, same recording); world_observe alone does not open frames either
    {const auto opened=s5.wobjEarlyOpened,seen=s5.wobjSeen;
     acz::g_worldObjectsTest=false;
     bcFrame(acmFirst({offW,offW2}));{const auto f=latest();CHECK(src->frames_published()==8&&f.members==7&&earlyProxyDraws()==14);}
     const auto dig=flagsDigest();
     acz::g_worldObserveTest=true;
     bcFrame(acmFirst({offW,offW2}));{const auto f=latest();CHECK(src->frames_published()==9&&f.members==7&&earlyProxyDraws()==14&&flagsDigest()==dig);}
     acz::g_worldObserveTest=false;
     CHECK(s5.wobjEarlyOpened==opened&&s5.wobjSeen==seen);
     acz::g_worldObjectsTest=true;}
    // (i) ACM-only lists (no cockpit member adopts): executed, never published, the slot retired; 3 in a row back the early start off
    {const auto pub=src->frames_published();
     for(int i=0;i<3;++i){d.clear();worldTail(d,{offW,offW2});bcFrame(d);CHECK(src->frames_published()==pub&&earlyProxyDraws()==4&&!s5.frame.active);}
     CHECK(s5.wobjEarlyAbandoned==3&&s5.wobjEarlyHold==120&&std::string_view(s5.wobjEarlyOff)=="early-never-adopted");
     const auto opened=s5.wobjEarlyOpened;bcFrame(acmFirst({offW,offW2}));
     const auto f=latest();CHECK(src->frames_published()==pub+1&&f.members==7&&s5.wobjEarlyOpened==opened&&s5.wobjEarlyHold==119);}
    // status line (dedicated log only)
    ticks(60);
    {const auto text=dedicated();
     if(!hasText(text,"event=continuous-status-world-early;","key=gpu_copy_step5_world_objects;opened=")){std::fprintf(stderr,"%s\n",text.c_str());CHECK(false);}
     // (the status lines are emitted on the first 5 s tick of the run; the final counters are asserted on the state above)
     CHECK(hasText(text,"event=continuous-status-world-early;","adopt-camera-mismatch=")&&hasText(text,"event=continuous-status-world-early-refused;","offscreen-target:")&&
           hasText(text,"event=continuous-status-world;","target:"));}
    CHECK(phase()==int(acz::Step1Phase::continuous)&&verdicts()==0);
    afterBind=nullptr;
    acz::g_worldObjectsTest=false;acz::g_worldObserveTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
// J-179 finding 4: under xr_look_drive_keys a world-first (early-open) frame must not use the PREVIOUS frame's look. It derives the look from
// the world camera against the previous valid cockpit measurement (the aircraft reference); without one the early open is refused; the
// adoption refuses the frame when the cockpit's own measurement differs from what the world members used. (The fixture's cockpit look is
// zero; the changing nonzero look comparison of world and cockpit eye axes is in look_drive_keys_offline test_world_first_look.)
static void step5_world_early_look() {
    bootWorld(true);
    auto& s5=s5ref();auto* src=acz::stereo::frame_source();
    auto latest=[&]{return bcLatest(src->frames_published());};
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    static LdFakeInjector inj;inj.fg=true;s5.lookDrive.set_injector(&inj);
    acz::g_lookDriveTest=true;acz::g_lookDriveClockMs=10000.0;
    // (a) no aircraft reference yet: the early open is refused (pose category), the cockpit members still produce the frame
    acz::g_lookDriveClockMs+=18.0;bcFrame(acmFirst({offW,offW2}));
    CHECK(s5.wobjEarlyOpened==0&&s5.wobjEarlyReason[gpu_copy::step5::world::cat_pose]>=1&&std::string_view(s5.wobjEarlyLastWhy)=="world-pose-look-reference");
    CHECK(src->frames_published()==1&&latest().members==7&&s5.lookDrive.measured().valid);
    // (b) the cockpit of that frame measured the look: the next world-first frame opens, derives the look and the adoption accepts it
    acz::g_lookDriveClockMs+=18.0;bcFrame(acmFirst({offW,offW2}));
    CHECK(s5.wobjEarlyOpened==1&&s5.wobjEarlyAdopted==1&&s5.wobjEarlyMismatch==0&&src->frames_published()==2);
    {const auto f=latest();CHECK(f.members==2+7&&!f.partial&&f.world_valid&&f.pose_valid&&f.look_valid);}
    // (c) the aircraft reference is wrong (rotated by 0.35 rad about the vertical): the world members' look disagrees with the cockpit's
    // own measurement, the adoption poisons the frame (executed, never published)
    {
        double m[16];std::memcpy(m,s5.scratch.m256,sizeof m);
        const double c=std::cos(0.35),sn=std::sin(0.35);
        for(int r=0;r<3;++r){const double x=m[r*4],z=m[r*4+2];m[r*4]=c*x+sn*z;m[r*4+2]=-sn*x+c*z;}
        s5.lookDrive.note_camera(s5.pf.cam.right,s5.pf.cam.up,s5.pf.cam.fwd,m);
        CHECK(s5.lookDrive.measured().valid);
        const auto drop=s5.poseDropped;
        acz::g_lookDriveClockMs+=18.0;bcFrame(acmFirst({offW,offW2}));
        CHECK(s5.wobjEarlyMismatch==1&&src->frames_published()==2&&src->frames_skipped()==1&&s5.poseDropped==drop+1&&
              std::string_view(s5.wobjAdoptLastWhy)=="adopt-look-compensation-mismatch");
    }
    // (d) key off again: the early open needs no reference (behaviour of the build without the key)
    acz::g_lookDriveTest=false;acz::g_lookDriveClockMs=-1;
    {const auto opened=s5.wobjEarlyOpened;bcFrame(acmFirst({offW,offW2}));CHECK(s5.wobjEarlyOpened==opened+1&&src->frames_published()==3);}
    afterBind=nullptr;
    acz::g_worldObjectsTest=false;acz::g_worldObserveTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
// ------------------------------------------------------------- J-world-evidence (Astra reply 5): RT slot 1 metadata, record snapshots, live cb10 rule
namespace wev=acz::gpu_copy::wev;
static void step5_world_evidence_rules() {
    namespace world=acz::gpu_copy::step5::world;
    using W=acz::diagnostics::RootConstantWitness;
    // refusal categories: rt-count and cb10 are distinct and appended (older positions unchanged)
    CHECK(world::categories==10&&world::cat_target==7&&std::string_view(world::category_name(8))=="rt-count"&&std::string_view(world::category_name(9))=="cb10");
    CHECK(world::category("multiple-render-targets-unsupported")==world::cat_rt_count&&world::category("world-rt-count-unsupported")==world::cat_rt_count&&
          world::category("world-rt-count-zero")==world::cat_rt_count&&world::category("no-render-target")==world::cat_rt_count&&
          world::category("om-rt-count-below-pso")==world::cat_rt_count);
    CHECK(world::category("world-cb10-unknown")==world::cat_cb10&&world::category("world-cb10-unresolved")==world::cat_cb10&&
          world::category("world-flags-fade-reads-c")==world::cat_cb10);
    CHECK(world::category("world-record-nonfinite")==world::cat_record&&world::category("world-offscreen-target")==world::cat_target&&
          world::category("something-else")==world::cat_state);
    {acz::gpu_copy::step3::Shape s;
     CHECK(!world::rt_count_why(s)); // PSO output unknown: decide() reports it
     s.pso.known=true;s.pso.rt_count=1;CHECK(!world::rt_count_why(s));
     s.pso.rt_count=2;CHECK(std::string_view(world::rt_count_why(s))=="world-rt-count-unsupported");
     s.pso.rt_count=0;CHECK(std::string_view(world::rt_count_why(s))=="world-rt-count-zero");}
    // cb10_check: only a witnessed dword 0 with bit 8 clear is accepted
    {world::Cb10 c;CHECK(std::string_view(world::cb10_check(c))=="world-cb10-unresolved");
     c.resolved=true;CHECK(std::string_view(world::cb10_check(c))=="world-cb10-unknown");
     c.known=true;c.value=258;CHECK(!world::cb10_check(c));
     c.value=2;CHECK(!world::cb10_check(c));c.value=7;CHECK(!world::cb10_check(c));
     c.value=258|8;CHECK(std::string_view(world::cb10_check(c))=="world-flags-fade-reads-c");}
    // the witness: dword 0 of THIS draw (draw interval still open), judged word by word
    {const unsigned T=77;W w;w.bind(1,2,3,4,3,2,3,true);
     auto write=[&](unsigned off,const uint32_t* d,unsigned n){auto c=w.begin(1,2,T);w.returned(c);w.write(c,1,2,3,off,d,n);w.end(c);};
     auto atDraw=[&](uint64_t key,unsigned tid,bool complete,uint32_t& out)->unsigned{
         auto d=w.begin(1,2,T);const unsigned r=unsigned(w.word_at_draw(1,2,key,4,tid,complete,0,out));w.returned(d);w.end(d);return r;};
     uint32_t v=0;
     CHECK(atDraw(3,T,true,v)==W::partial);                                   // nothing written yet
     const uint32_t second=1;write(1,&second,1);CHECK(atDraw(3,T,true,v)==W::partial); // word 1 alone does not make word 0 known
     const uint32_t both[2]={258,1};write(0,both,2);
     CHECK(atDraw(3,T,true,v)==W::covered&&v==258&&!world::flags_check(v));
     CHECK(atDraw(3,T,false,v)==W::history);                                  // the list's binding tracking is not complete
     CHECK(atDraw(3,T+1,true,v)==W::thread);                                  // another thread
     CHECK(atDraw(9,T,true,v)==W::root_changed);                              // another root signature than the one the words were written under
     const uint32_t fade[2]={266,1};write(0,fade,2);
     CHECK(atDraw(3,T,true,v)==W::covered&&v==266&&world::flags_check(v));
     {uint32_t u=0;CHECK(w.word_at_draw(1,2,3,4,T,true,0,u)==W::overlap);}    // outside a Draw interval (no interval is open)
     w.bind(1,2,9,4,3,2,3,true);                                              // the game bound another root signature: the words are gone
     CHECK(atDraw(9,T,true,v)==W::root_changed);
     w.fail(W::bundle);CHECK(atDraw(9,T,true,v)==W::bundle);
     CHECK(std::string_view(W::reason_name(W::covered))=="covered"&&std::string_view(W::reason_name(W::root_changed))=="root-changed");}
    // Run H: the ACM draws' cb10 (param 3, stages 3, words 0x102,0x3f008081) read witness-reason=history. The provenance of the bound root is a pure rule.
    {bool inf=true;
     CHECK(!world::cb10_provenance(nullptr,true,true,true,53,53,inf)&&!inf);                              // PSO root == bound root: complete, not inferred
     CHECK(!world::cb10_provenance(nullptr,true,true,true,53,0,inf)&&inf);                                // PSO root identity unknown (run H's previous failure: 53 != 0): bound root layout used, inferred
     CHECK(std::string_view(world::cb10_provenance(nullptr,true,true,true,53,54,inf))=="pso-root-differs-from-bound-root"&&!inf); // a KNOWN different root is refused
     CHECK(std::string_view(world::cb10_provenance(nullptr,true,true,true,0,0,inf))=="bound-root-identity-unknown");
     CHECK(std::string_view(world::cb10_provenance(nullptr,true,true,false,53,53,inf))=="pipeline-identity-differs");
     CHECK(std::string_view(world::cb10_provenance(nullptr,true,false,true,53,53,inf))=="no-pipeline-record");
     CHECK(std::string_view(world::cb10_provenance(nullptr,false,true,true,53,53,inf))=="tracking-incomplete");
     CHECK(std::string_view(world::cb10_provenance("rejected:list generation mismatch",true,true,true,53,53,inf))=="rejected:list generation mismatch");}
    // The ACM scenarios end to end through the witness (the parameter-3 constants, vertex+pixel visible): same recording before the draw -> resolved;
    // one single-constant call per word -> resolved; bit 8 -> refused by cb10_check; never set / only word 1 / set in a previous recording -> unknown -> refused.
    {const unsigned T=88;
     auto resolve=[&](W& w,uint64_t list,uint64_t gen,bool complete,uint32_t& out)->world::Cb10 {
         world::Cb10 c;c.resolved=true;c.size=2;c.parameter=3;c.stages=3;
         auto d=w.begin(list,gen,T);c.reason=unsigned(w.word_at_draw(list,gen,3,4,T,complete,0,out));w.returned(d);w.end(d);
         if(c.reason==W::covered){c.known=true;c.value=out;}
         return c;};
     auto setN=[&](W& w,unsigned off,const uint32_t* d,unsigned n){auto c=w.begin(1,2,T);w.returned(c);w.write(c,1,2,3,off,d,n);w.end(c);};
     uint32_t v=0;
     {W w;w.bind(1,2,3,4,3,2,3,true);const uint32_t both[2]={0x102,0x3f008081u};setN(w,0,both,2);   // SetGraphicsRoot32BitConstants(3, 2, ...)
      const auto c=resolve(w,1,2,true,v);CHECK(c.known&&c.value==0x102&&!world::cb10_check(c));}
     {W w;w.bind(1,2,3,4,3,2,3,true);const uint32_t a=0x102,b=0x3f008081u;setN(w,0,&a,1);setN(w,1,&b,1); // SetGraphicsRoot32BitConstant per word
      const auto c=resolve(w,1,2,true,v);CHECK(c.known&&c.value==0x102&&!world::cb10_check(c));}
     {W w;w.bind(1,2,3,4,3,2,3,true);const uint32_t a=0x102;setN(w,0,&a,1);                              // word 0 alone is enough for dword 0
      const auto c=resolve(w,1,2,true,v);CHECK(c.known&&c.value==0x102);}
     {W w;w.bind(1,2,3,4,3,2,3,true);const uint32_t fade[2]={0x10a,1};setN(w,0,fade,2);
      const auto c=resolve(w,1,2,true,v);CHECK(c.known&&std::string_view(world::cb10_check(c))=="world-flags-fade-reads-c");}
     {W w;w.bind(1,2,3,4,3,2,3,true);const auto c=resolve(w,1,2,true,v);                                  // never set
      CHECK(!c.known&&c.reason==W::partial&&std::string_view(world::cb10_check(c))=="world-cb10-unknown");}
     {W w;w.bind(1,2,3,4,3,2,3,true);const uint32_t b=7;setN(w,1,&b,1);const auto c=resolve(w,1,2,true,v); // only word 1
      CHECK(!c.known&&std::string_view(world::cb10_check(c))=="world-cb10-unknown");}
     {W w;w.bind(1,2,3,4,3,2,3,true);const uint32_t a=0x102;setN(w,0,&a,1);
      w.bind(1,3,3,4,3,2,3,true);                                                                          // the next recording of the list (Reset): the words do not carry over
      const auto c=resolve(w,1,3,true,v);CHECK(!c.known&&c.reason==W::partial&&std::string_view(world::cb10_check(c))=="world-cb10-unknown");}
     {W w;w.bind(1,2,3,4,3,2,3,true);const uint32_t a=0x102;setN(w,0,&a,1);
      const auto c=resolve(w,1,3,true,v);CHECK(!c.known&&c.reason==W::stale);}                              // a draw of another generation than the words
     {W w;w.bind(1,2,3,4,3,2,3,true);const uint32_t a=0x102;setN(w,0,&a,1);
      const auto c=resolve(w,1,2,false,v);CHECK(!c.known&&c.reason==W::history);}                          // provenance incomplete (the run H symptom)
    }
    // RT records: first occurrence, change of (PSO, OM epoch), caps
    {wev::State st;
     CHECK(st.rt_wanted(10,111)&&!st.rt_wanted(10,111)&&st.rt_wanted(10,222)&&st.rt_wanted(11,111)&&!st.rt_wanted(11,111));
     for(uint64_t k=0;k<20;++k)(void)st.rt_wanted(10,1000+k);
     CHECK(st.rtRecords==wev::rt_changes_per_pso+1&&st.rtSuppressed>0);
     for(uint64_t p=100;p<130;++p)(void)st.rt_wanted(p,1);
     CHECK(st.rtTableFull>0&&st.rtRecords<=wev::rt_record_cap);
     wev::RtFacts f;for(unsigned i=0;i<wev::rt_queue+5;++i)st.rt_push(f);
     CHECK(st.rtLost==5);wev::RtFacts o;unsigned n=0;while(st.rt_pop(o))++n;CHECK(n==wev::rt_queue);
     // all-draw ordinals per recording
     CHECK(st.draw_ordinal(1,1)==1&&st.draw_ordinal(1,1)==2&&st.draw_ordinal(1,2)==1);st.discard(1);CHECK(st.draw_ordinal(1,1)==1);}
    // OM epoch key: changes with the bound handles, the view serial and whether the views are still current
    {wev::RtFacts a;a.omSet=true;a.omCount=2;a.handles[0]=0x100;a.handles[1]=0x108;a.slot0.kind=wev::sk_resource;a.slot0.serial=4;a.slot1.kind=wev::sk_null_descriptor;a.slot1.serial=5;
     const auto k0=wev::om_key(a);auto b=a;CHECK(wev::om_key(b)==k0);b.handles[1]=0x110;CHECK(wev::om_key(b)!=k0);
     b=a;b.slot1.serial=6;CHECK(wev::om_key(b)!=k0);b=a;b.slot1.current=true;CHECK(wev::om_key(b)!=k0);b=a;b.omSingle=true;CHECK(wev::om_key(b)!=k0);}
    // record snapshots: <= 3 per window, <= 8 windows per process, drops counted, >= 2 s between windows
    {wev::State s;wev::SrtMeta m;m.list=5;m.generation=1;m.hasBytes=true;uint8_t bytes[416];for(unsigned i=0;i<416;++i)bytes[i]=uint8_t(i);
     CHECK(s.srt_wanted(5,1,1000));
     for(unsigned i=0;i<3;++i)CHECK(s.srt_offer(m,bytes,1000)==wev::State::accept_record);
     CHECK(!s.srt_wanted(5,1,1000)&&s.srt_offer(m,bytes,1000)==wev::State::drop_full&&s.dropWindowFull==1);
     m.generation=2;CHECK(s.srt_offer(m,bytes,1500)==wev::State::drop_spacing&&s.dropSpacing==1); // another recording, 0.5 s later
     wev::Srt out;CHECK(!s.srt_pop(out,1100));                                                    // not executed, not old enough
     s.note_execute(5,1,99);CHECK(s.srt_pop(out,1100)&&out.execSeq==99&&out.window==1&&out.rec==0&&std::memcmp(out.bytes,bytes,416)==0);
     CHECK(s.srt_pop(out,1100)&&s.srt_pop(out,1100)&&!s.srt_pop(out,1100));
     uint64_t t=1000;
     for(unsigned k=2;k<=wev::window_cap;++k){t+=2100;m.generation=1;CHECK(s.srt_offer(m,bytes,t)==wev::State::accept_record);while(s.srt_pop(out,t+1000)){}}
     t+=2100;CHECK(!s.srt_wanted(5,1,t)&&s.srt_offer(m,bytes,t)==wev::State::drop_cap&&s.dropWindowCap==1&&s.totals().windows==wev::window_cap);
     // a failed read stores the metadata without bytes
     wev::State s2;wev::SrtMeta m2;m2.list=1;m2.generation=1;m2.hasBytes=true;CHECK(s2.srt_offer(m2,nullptr,1)==wev::State::accept_record);
     CHECK(s2.slot[0].used&&!s2.slot[0].m.hasBytes);}
    // formatting: hex of the saved copy, hash, decision and cb10 provenance
    {wev::Srt s;s.used=true;s.window=1;s.rec=0;s.m.list=0x10;s.m.generation=7;s.m.ordinal=12;s.m.t10Param=2;s.m.t10Vis=0;s.m.t10Va=0x1000;s.m.t10Address=0x2000;
     s.m.t10ResSerial=4;s.m.t10Offset=0xec4d0;s.m.t10OffsetKnown=true;s.m.hasBytes=true;s.m.accepted=true;std::snprintf(s.m.path,sizeof s.m.path,"member");
     std::snprintf(s.m.read,sizeof s.m.read,"read");std::snprintf(s.m.vs,sizeof s.m.vs,"%s","00112233445566778899aabbccddeeff");
     for(unsigned i=0;i<416;++i)s.bytes[i]=uint8_t(i);
     s.m.cb10.resolved=true;s.m.cb10.known=true;s.m.cb10.value=258;s.m.cb10.mask=3;s.m.cb10.words[0]=258;s.m.cb10.words[1]=1;s.m.cb10.size=2;s.m.cb10.parameter=3;
     char lines[wev::srt_lines][wev::line_bytes];
     CHECK(wev::format_srt(s,lines)==6);
     char want[64];std::snprintf(want,sizeof want,"fnv1a64=%016llx",(unsigned long long)wev::fnv64(s.bytes,416));
     CHECK(std::strstr(lines[0],"event=acm-record;win=1;rec=1/3;")&&std::strstr(lines[0],"path=member;")&&std::strstr(lines[0],"recording-gen=7;all-draw-ordinal=12;exec-seq=unknown;")&&
           std::strstr(lines[0],"vs=00112233445566778899aabbccddeeff;")&&std::strstr(lines[0],"t10-param=2;t10-vis=0;")&&std::strstr(lines[0],"t10-offset=ec4d0;")&&
           std::strstr(lines[0],want)&&std::strstr(lines[0],"build="));
     CHECK(std::strstr(lines[1],"event=acm-record-decision;")&&std::strstr(lines[1],"decision=accepted;reason=none;cb10=known;cb10-dword0=00000102;flags-bit8-fade=0;")&&
           std::strstr(lines[1],"cb10-mask=3;cb10-words=00000102,00000001;")&&std::strstr(lines[1],"label=original-game-record;"));
     CHECK(std::strstr(lines[2],"part=1/4;bytes=0-127;hex=000102030405")&&std::strstr(lines[5],"part=4/4;bytes=384-415;hex=808182"));
     CHECK(std::strlen(std::strstr(lines[2],"hex="))==4+256&&std::strlen(std::strstr(lines[5],"hex="))==4+64);
     for(unsigned i=0;i<6;++i)CHECK(!std::strstr(lines[i],"truncated=1"));
     // refused, cb10 unknown: the value stays unknown and the witness reason is shown
     s.m.accepted=false;std::snprintf(s.m.decision,sizeof s.m.decision,"world-cb10-unknown");s.m.cb10.known=false;s.m.cb10.reason=unsigned(W::partial);s.m.cb10.mask=0;s.m.cb10.note="x";s.execSeq=5;
     CHECK(wev::format_srt(s,lines)==6);
     CHECK(std::strstr(lines[0],"exec-seq=5;")&&std::strstr(lines[1],"decision=refused;reason=world-cb10-unknown;cb10=UNKNOWN;cb10-dword0=unknown;witness-reason=partial;note=x;")&&
           std::strstr(lines[1],"substituted=none(draw-refused)")&&std::strstr(lines[1],"cb10-words=????????,????????;"));
     // unreadable: no bytes, no hex lines
     s.m.hasBytes=false;std::snprintf(s.m.read,sizeof s.m.read,"not-mapped");CHECK(wev::format_srt(s,lines)==2&&std::strstr(lines[0],"read=not-mapped;bytes=none;fnv1a64=0000000000000000"));
     // observer-only
     s.m.observed=true;CHECK(wev::format_srt(s,lines)==2&&std::strstr(lines[1],"decision=not-evaluated;")&&std::strstr(lines[1],"substituted=none(observer-only)"));}
    {wev::RtFacts f;f.list=0x10;f.generation=7;f.ordinal=9;f.pso=0x42;f.psoId=15;f.psoKnown=true;f.rtCount=2;f.rtv[0]=10;f.rtv[1]=28;f.independent=true;f.mask0=15;f.blend1=true;f.mask1=7;
     f.omSet=true;f.omCount=2;f.omSingle=false;f.handles[0]=0x100;f.handles[1]=0x108;f.bindEpoch=3;f.curEpoch=3;f.omProof=true;
     f.slot0.kind=wev::sk_resource;f.slot0.resource=0xabc;f.slot0.serial=4;f.slot0.bindSerial=4;f.slot0.current=true;f.slot0.resFormat=10;f.slot0.width=2560;f.slot0.height=1440;
     f.slot0.viewDesc=true;f.slot0.viewFormat=10;f.slot0.viewDim=4;
     f.slot1.kind=wev::sk_null_descriptor;f.slot1.handle=0x108;f.slot1.serial=5;f.slot1.bindSerial=5;f.slot1.current=true;f.key=wev::om_key(f);
     char a[wev::line_bytes],b[wev::line_bytes],c[wev::line_bytes];wev::format_rt(f,1,a,b,c);
     CHECK(std::strstr(a,"event=acm-rt;n=1;")&&std::strstr(a,"pso-rt-count=2;pso-rtv=10,28,0,0,0,0,0,0;")&&std::strstr(a,"independent-blend=1;slot1-active=1;slot1-blend=1;slot1-write-mask=0x7")&&
           std::strstr(a,"pso-write-mask0=0xf"));
     CHECK(std::strstr(b,"event=acm-rt-om;")&&std::strstr(b,"label=original-game-binding;om-set=1;om-count=2;form=handle-array;handles=100,108;")&&std::strstr(b,"injected-eye=none(draw-refused:rt-count!=1,nothing-replayed)"));
     CHECK(std::strstr(c,"slot0=RESOURCE(res=abc,view-serial=4,bind-serial=4,still-current=1,res-fmt=10,2560x1440,view-desc=yes,view-fmt=10,view-dim=4,mip=0,first-slice=0)")&&
           std::strstr(c,"slot1=NULL-DESCRIPTOR(handle=108,view-serial=5,bind-serial=5,still-current=1)"));
     // contiguous-range form: slot 1 is range-unresolved, never UNBOUND
     f.omSingle=true;f.slot1=wev::SlotFacts{};f.slot1.kind=wev::sk_range_unresolved;f.slot1.detail="contiguous-range-form:only-the-base-handle-is-stored";f.rtCount=1;
     wev::format_rt(f,2,a,b,c);
     CHECK(std::strstr(b,"form=contiguous-range(")&&std::strstr(b,"injected-eye=OMSetRenderTargets(count=1,private-eye-target,slot1=none)")&&std::strstr(c,"slot1=range-unresolved(contiguous-range-form")&&
           !std::strstr(c,"UNBOUND")&&std::strstr(a,"slot1-active=0")&&std::strstr(a,"slot1-blend=not-active"));}
    // decision summary
    {wev::State s;for(int i=0;i<3;++i)s.note_decision(nullptr,i==2);s.note_decision("world-cb10-unknown",false);s.note_decision("world-cb10-unknown",true);
     for(unsigned i=0;i<wev::reason_cap+3;++i){char r[32];std::snprintf(r,sizeof r,"r%u",i);s.note_decision(r,false);}
     CHECK(s.reasonCount==wev::reason_cap&&s.reasonOverflow>0);
     char lines[wev::summary_lines][wev::line_bytes];const unsigned n=wev::format_summary(s,lines);
     CHECK(n==wev::summary_lines&&std::strstr(lines[0],"event=decision-summary;part=1;")&&std::strstr(lines[0],"accepted:3(member:2,early:1)")&&std::strstr(lines[0],"world-cb10-unknown:2(member:1,early:1)"));}
}
// Hook-level: the live cb10 rule on every ACM acceptance path, the rt-count refusal category, the saved evidence in the dedicated log.
static void step5_world_evidence() {
    namespace world=acz::gpu_copy::step5::world;
    bootWorld();
    auto& s5=s5ref();
    // (a) a witnessed flags word 258: accepted as before; the evidence log carries the PSO facts, the original OM binding and the record snapshot
    auto d=standardDraws();worldTail(d,{offW,offW2});
    bcFrame(d);{const auto f=bcLatest(1);CHECK(f.members==9&&!f.partial);}
    CHECK(s5.wobjRecordedSum==2&&s5.wobjRefusedSum==0&&s5.wobjCb10.resolved&&s5.wobjCb10.known&&s5.wobjCb10.value==258);
    // (b) flags bit 8 set (fade branch reads C): refused as cb10, nothing replayed
    acz::g_wevCb10FixedValue=258|8;
    bcFrame(d);{const auto f=bcLatest(2);CHECK(f.members==7&&!f.partial);}
    CHECK(s5.wobjReason[world::cat_cb10]==2&&s5.wobjRefusedSum==2&&std::string_view(s5.wobjLastWhy)=="world-flags-fade-reads-c");
    // (c) unknown value: refused too (a log of the value is not enforcement)
    acz::g_wevCb10FixedUnknown=true;
    bcFrame(d);{const auto f=bcLatest(3);CHECK(f.members==7);}
    CHECK(s5.wobjReason[world::cat_cb10]==4&&std::string_view(s5.wobjLastWhy)=="world-cb10-unknown");
    // (d) the REAL resolver (no root-constant witness in the fixture, ownership pins off): unknown -> refused, with the reason in the log
    acz::g_wevCb10Fixed=false;
    bcFrame(d);{const auto f=bcLatest(4);CHECK(f.members==7);}
    CHECK(s5.wobjReason[world::cat_cb10]==6&&std::string_view(s5.wobjLastWhy)=="world-cb10-unknown"&&s5.wobjCb10.resolved&&!s5.wobjCb10.known);
    acz::g_wevCb10Fixed=true;acz::g_wevCb10FixedUnknown=false;acz::g_wevCb10FixedValue=258;
    // (e) a two-target PSO is refused and counted as rt-count (not hidden behind another category); the record is still shown
    {auto& s=acz::state();std::lock_guard lock(s.mutex);auto& p=s.pipelines[acmPso()];p.output.rt_count=2;p.output.rtv[1]=uint32_t(shape3.rtv);p.blend1=true;p.write_mask1=7;}
    bcFrame(d);{const auto f=bcLatest(5);CHECK(f.members==7&&!f.partial);}
    CHECK(s5.wobjReason[world::cat_rt_count]==2&&std::string_view(s5.wobjLastWhy)=="world-rt-count-unsupported");
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.pipelines[acmPso()].output.rt_count=1;}
    bcFrame(d);{const auto f=bcLatest(6);CHECK(f.members==9);}
    // status + evidence lines (dedicated log only); the records are old enough after a short wait
    Sleep(900);ticks(120);
    const auto text=dedicated();
    if(std::getenv("ACZ_WEV_DUMP")){size_t at=0;while((at=text.find("world-evidence",at))!=std::string::npos){const size_t e=text.find('\n',at);std::fprintf(stderr,"%s\n",text.substr(at,e-at).c_str());at=e==std::string::npos?text.size():e;}}
    CHECK(hasText(text,"world-categories;","rt-count:2,cb10:6"));
    CHECK(hasText(text,"world-evidence;event=acm-rt;n=1;","pso-rt-count=1;pso-rtv="));
    CHECK(hasText(text,"world-evidence;event=acm-rt;","pso-rt-count=2;")&&hasText(text,"world-evidence;event=acm-rt;","slot1-active=1;slot1-blend=1;slot1-write-mask=0x7"));
    CHECK(hasText(text,"world-evidence;event=acm-rt-om;","label=original-game-binding;om-set=1;om-count=1;form=handle-array;"));
    CHECK(hasText(text,"world-evidence;event=acm-rt-slots;","slot1=UNBOUND(om-count-not-above-slot)"));
    CHECK(hasText(text,"world-evidence;event=acm-record;win=1;rec=1/3;","path=member;")&&hasText(text,"world-evidence;event=acm-record-decision;win=1;rec=1;","decision=accepted;reason=none;cb10=known;cb10-dword0=00000102;"));
    CHECK(hasText(text,"world-evidence;event=acm-record-hex;win=1;rec=1;part=1/4;bytes=0-127;hex="));
    CHECK(hasText(text,"world-evidence;event=decision-summary;","accepted:")&&hasText(text,"world-evidence;event=decision-summary;","world-flags-fade-reads-c:"));
    CHECK(hasText(text,"-world-evidence;keys=","windows-opened=1(cap 8);"));
    CHECK(hasText(text,"event=world-evidence-definitions;part=3;","range-unresolved=contiguous-range-form"));
    for(const auto& l:w->lines)CHECK(l.find("world-evidence")==std::string::npos); // dedicated log only
    CHECK(phase()==int(acz::Step1Phase::continuous)&&verdicts()==0);
    acz::g_wevCb10Fixed=false;acz::g_worldObjectsTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
// The same-list early start applies the same rules (rt-count before the scene-target rule, live cb10).
static void step5_world_evidence_early() {
    namespace world=acz::gpu_copy::step5::world;
    bootWorld(true);
    auto& s5=s5ref();auto* src=acz::stereo::frame_source();
    auto latest=[&]{return bcLatest(src->frames_published());};
    afterBind=[]{gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_BARRIER_FLAG_NONE);
        gameBarrier(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,0,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_BARRIER_FLAG_NONE);};
    bcFrame(acmFirst({offW,offW2}));
    {const auto f=latest();CHECK(src->frames_published()==1&&f.members==2+7&&s5.wobjEarlyOpened==1&&s5.wobjEarlyAdopted==1);}
    // flags bit 8: neither ACM draw opens a frame; the cockpit starts it as before
    acz::g_wevCb10FixedValue=258|8;
    {const auto before=s5.wobjEarlyReason[world::cat_cb10];bcFrame(acmFirst({offW,offW2}));
     const auto f=latest();CHECK(src->frames_published()==2&&f.members==7&&s5.wobjEarlyReason[world::cat_cb10]==before+2&&s5.wobjEarlyOpened==1&&
                                 std::string_view(s5.wobjEarlyLastWhy)=="world-flags-fade-reads-c");}
    acz::g_wevCb10FixedUnknown=true;
    {const auto before=s5.wobjEarlyReason[world::cat_cb10];bcFrame(acmFirst({offW}));
     const auto f=latest();CHECK(src->frames_published()==3&&f.members==7&&s5.wobjEarlyReason[world::cat_cb10]==before+1&&std::string_view(s5.wobjEarlyLastWhy)=="world-cb10-unknown");}
    acz::g_wevCb10FixedUnknown=false;acz::g_wevCb10FixedValue=258;
    // a two-target PSO: counted as rt-count even though the OM binding of the draw also differs from the scene target rule
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.pipelines[acmPso()].output.rt_count=2;}
    {const auto before=s5.wobjEarlyReason[world::cat_rt_count];bcFrame(acmFirst({offW,offW2}));
     const auto f=latest();CHECK(src->frames_published()==4&&f.members==7&&s5.wobjEarlyReason[world::cat_rt_count]==before+2&&s5.wobjEarlyOpened==1&&
                                 std::string_view(s5.wobjEarlyLastWhy)=="world-rt-count-unsupported");}
    {auto& s=acz::state();std::lock_guard lock(s.mutex);s.pipelines[acmPso()].output.rt_count=1;}
    bcFrame(acmFirst({offW}));{const auto f=latest();CHECK(src->frames_published()==5&&f.members==1+7&&s5.wobjEarlyOpened==2);}
    Sleep(900);ticks(120);
    const auto text=dedicated();
    CHECK(hasText(text,"world-evidence;event=decision-summary;","world-flags-fade-reads-c:")&&hasText(text,"world-evidence;event=decision-summary;","world-rt-count-unsupported:"));
    CHECK(hasText(text,"world-categories;","s=state:"));
    afterBind=nullptr;
    acz::g_wevCb10Fixed=false;acz::g_worldObjectsTest=false;acz::g_worldObserveTest=false;acz::g_byClassTest=false;acz::stereo::set_pose_provider(nullptr);
    step3Lines();
}
static void step3_off() {
    // Default OFF and step-1/2 modes: the RS/OM tracking slots are never patched.
    boot(false);
    for(int i=0;i<11;++i) {
        auto& t=w->lists[0].tables[size_t(i)];
        CHECK(t[21]==reinterpret_cast<void*>(&listViewports)&&t[22]==reinterpret_cast<void*>(&listScissors)&&t[46]==reinterpret_cast<void*>(&listOm));
    }
    {auto& s=acz::state();std::lock_guard lock(s.mutex);CHECK(!s.states.at(listAlias(0)).step3.known);}
    // The raw vtable slots step 3 dispatches through (list 16/26/40/46/47/48,
    // device 14/20/21, resource 11, heap 9) are the SDK's: calls made through
    // the d3d12.h C++ interfaces land on the fakes installed at those indices.
    auto* l=listAlias(0);auto& L=w->lists[0];CHECK(l->Reset(allocator(0),nullptr)==S_OK);
    const D3D12_CPU_DESCRIPTOR_HANDLE h{0x10000};const FLOAT c[4]={0,0,0,0};
    l->OMSetRenderTargets(1,&h,FALSE,nullptr);l->ClearRenderTargetView(h,c,0,nullptr);
    l->ClearDepthStencilView(h,D3D12_CLEAR_FLAG_DEPTH,0.0f,0,0,nullptr);l->SetGraphicsRootShaderResourceView(3,0x1234);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=reinterpret_cast<ID3D12Resource*>(&w->resources[5].object);
    l->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint.Depth=1;
    src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    const std::vector<std::string> want{"reset","om","clear-rtv","clear-dsv","srv","barrier","copy-tex"};
    CHECK(std::vector<std::string>(L.events.end()-7,L.events.end())==want&&L.roots[3]==0x1234);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;ID3D12DescriptorHeap* heap=nullptr;
    CHECK(SUCCEEDED(device()->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)))&&heap==reinterpret_cast<ID3D12DescriptorHeap*>(&w->heaps[0]));
    CHECK(heap->GetCPUDescriptorHandleForHeapStart().ptr==0x10000000);
    auto* r=reinterpret_cast<ID3D12Resource*>(&w->resources[6].object);
    device()->CreateRenderTargetView(r,nullptr,h);device()->CreateDepthStencilView(r,nullptr,D3D12_CPU_DESCRIPTOR_HANDLE{0x20000});
    CHECK(w->viewCount==2&&viewResource(0x20000)==&w->resources[6]&&r->GetGPUVirtualAddress()==0x7006000000ull);
}
static void step3_off_step2() {
    build(true);acz::g_step2Test=true;acz::g_l3Test=true;bootBuilt(true);CHECK(st1().step2&&!st1().step3);
    for(int i=0;i<11;++i)CHECK(w->lists[0].tables[size_t(i)][46]==reinterpret_cast<void*>(&listOm));
}
static void step3_rules() {
    using namespace gpu_copy::step3;
    Shape s;s.tracking_complete=true;s.pso.known=true;s.pso.rt_count=1;s.pso.rtv[0]=dxgi::b8g8r8a8_srgb;s.pso.dsv=dxgi::d24_s8;
    s.pso.samples=1;s.pso.depth_enable=true;s.pso.depth_func=cmp::less_equal;s.om_known=s.om_set=true;s.om_count=1;s.om_dsv=true;s.om_desc_proved=true;
    s.rs_known=true;s.viewports=1;s.vp_right=640.5f;s.vp_bottom=448;s.t10_parameter=2;s.t10_va=0x1000;s.t10_root_value=true;s.count=9;s.instances=1;
    {auto g=s;g.om_desc_proved=false;Targets u;CHECK(decide(g,u)&&std::string_view(decide(g,u))=="om-descriptor-unproved");} // J-3b9
    Targets t;CHECK(!decide(s,t)&&t.width==641&&t.height==448&&t.depth_clear==1.0f&&t.depth.stencil&&t.color.bgra&&t.color.srgb);
    CHECK(t.commands==30);
    CHECK(!finalize(t,0,0,641,448)&&t.row_pitch==2816&&t.footprint==2816*448&&t.footprint_stride%512==0&&t.charged<=gpu_copy::Session::byte_cap);
    auto refused=[&](Shape g,const char* why){Targets u;const char* r=decide(g,u);if(!r||std::string_view(r)!=why){std::fprintf(stderr,"%s\n",r?r:"null");CHECK(false);}};
    {auto g=s;g.tracking_complete=false;refused(g,"binding-tracking-incomplete");}
    {auto g=s;g.pso.known=false;refused(g,"pso-output-unknown");}
    {auto g=s;g.pso.samples=2;refused(g,"msaa-unsupported");}
    {auto g=s;g.pso.quality=1;refused(g,"msaa-unsupported");}
    {auto g=s;g.pso.rt_count=2;refused(g,"multiple-render-targets-unsupported");}
    {auto g=s;g.pso.rt_count=0;refused(g,"no-render-target");}
    {auto g=s;g.pso.rtv[0]=24;refused(g,"rt-format-unsupported");}
    {auto g=s;g.pso.dsv=99;refused(g,"ds-format-unsupported");}
    {auto g=s;g.pso.so_entries=1;refused(g,"stream-output-unsupported");}
    {auto g=s;g.pso.depth_func=cmp::not_equal;refused(g,"depth-func-equality-unsupported");}
    {auto g=s;g.pso.dsv=0;refused(g,"depth-enabled-without-dsv-format");}
    {auto g=s;g.om_known=false;refused(g,"om-state-unknown");}
    {auto g=s;g.om_dsv=false;refused(g,"om-dsv-unbound");}
    {auto g=s;g.rs_known=false;refused(g,"rs-state-unknown");}
    {auto g=s;g.viewports=0;refused(g,"viewport-unset");}
    {auto g=s;g.vp_left=-1;refused(g,"viewport-negative-origin");}
    {auto g=s;g.vp_right=4097;g.vp_bottom=2049;refused(g,"rt-over-cap");}
    {auto g=s;g.pso.rtv[0]=dxgi::r16g16b16a16_float;g.pso.dsv=dxgi::d32_s8x24;g.vp_right=2560;g.vp_bottom=1440;Targets u;
     CHECK(!decide(g,u)&&u.color.half&&u.color.bytes==8&&u.rt_bytes==2560ull*1440*8&&u.ds_bytes==2560ull*1440*8&&u.depth.stencil);
     // finalize: four FULL 2560x1440 FP16 footprints (112.5 MiB) fit the 160 MiB ceiling and the ledger cap.
     CHECK(!finalize(u,0,0,2560,1440)&&u.row_pitch==20480&&u.readback_bytes==4ull*20480*1440&&u.readback_bytes<=readback_byte_cap);
     CHECK(u.charged<=gpu_copy::Session::byte_cap&&std::string_view(finalize(u,2000,0,600,10))=="crop-outside-target");
     {Targets v=u;v.width=v.height=4096;CHECK(std::string_view(finalize(v,0,0,4096,4096))=="readback-over-cap");}
     g.vp_right=3000;refused(g,"rt-over-cap");}
    {auto g=s;g.t10_parameter=UINT32_MAX;refused(g,"t10-parameter-unknown");}
    {auto g=s;g.t10_table=true;refused(g,"t10-descriptor-table-unsupported");}
    {auto g=s;g.t10_va=0;refused(g,"t10-unbound");}
    {auto g=s;g.t10_root_value=false;refused(g,"t10-trackers-disagree");}
    {auto g=s;g.pso.depth_enable=false;g.pso.dsv=0;g.om_dsv=false;Targets u;CHECK(!decide(g,u)&&!u.has_depth&&u.commands==26);}
    // Same-shape rule.
    {auto g=s;CHECK(!same_shape(s,g));g.vp_right=100;CHECK(std::string_view(same_shape(s,g))=="viewport-changed");
     g=s;g.t10_va=0x2000;CHECK(!same_shape(s,g));g=s;g.count=8;CHECK(std::string_view(same_shape(s,g))=="draw-args-changed");}
    // Substitution: zero offset reproduces, eyes move by +-half_ipd along right, untouched bytes equal.
    acz::Mat4 m0,m64,m256;sceneMatrices(m0,m64,m256);
    std::array<uint8_t,416> blob{};for(size_t i=0;i<416;++i)blob[i]=uint8_t(i*7);
    putMatrix(blob.data(),m0);putMatrix(blob.data()+64,m64);putMatrix(blob.data()+256,m256);putParams(blob.data());
    Substitution sub;CHECK(substitute(blob.data(),sub)&&sub.untouched_equal&&sub.control_m64_rel<1e-6);
    // J-3b7e: control == raw bytes; the delta form equals the full M256*T(-o)*inv(M256)*M64 form.
    CHECK(sub.control_bytes_differ==0&&std::memcmp(sub.blobs[0].data(),blob.data(),416)==0);
    for(unsigned b=1;b<3;++b) {
        const double k=blob_sign[b]*half_ipd_units;const auto& r=sub.camera.right;
        const auto full64=acz::multiply(acz::multiply(acz::multiply(m256,acz::translation(-k*r[0],-k*r[1],-k*r[2])),acz::inverse(m256)),m64);
        const auto full0=acz::multiply(full64,acz::multiply(acz::inverse(m64),m0));
        for(int i=0;i<16;++i){
            CHECK(std::fabs(double(sub.m64f[b][size_t(i)])-full64[size_t(i)])<=1e-5*(1+std::fabs(full64[size_t(i)])));
            CHECK(std::fabs(double(sub.m0f[b][size_t(i)])-full0[size_t(i)])<=1e-4*(1+std::fabs(full0[size_t(i)])));
            if(i<12)CHECK(sub.m64f[b][size_t(i)]==float(m64[size_t(i)])&&sub.m0f[b][size_t(i)]==float(m0[size_t(i)])); // rows 0..2 untouched
        }
    }
    {auto na=blob;const float x=0.5f;std::memcpy(na.data()+256+12,&x,4);Substitution s2;
     CHECK(!substitute(na.data(),s2)&&std::string_view(s2.error)=="m256-not-affine");}
    CHECK(std::fabs(sub.eye[1][0]-(sceneEye[0]-half_ipd_units))<1e-6&&std::fabs(sub.eye[2][0]-(sceneEye[0]+half_ipd_units))<1e-6);
    CHECK(std::fabs(expected_disparity_px(sub,30.0,256)-(2*half_ipd_units/30.0)/0.6152*128)<1e-3);
    {auto bad=blob;const float inf=std::numeric_limits<float>::infinity();std::memcpy(bad.data()+64,&inf,4);Substitution s2;
     CHECK(!substitute(bad.data(),s2)&&std::string_view(s2.error)=="srt-matrix-nonfinite");}
    {auto bad=blob;std::memset(bad.data()+256,0,48);Substitution s2;CHECK(!substitute(bad.data(),s2)&&std::string_view(s2.error)=="srt-matrix-singular");}
    // PNG: >65535 raw bytes (several stored blocks), BGRA reordered to RGBA; decodes with independent checks.
    const uint32_t pw=300,ph=70,pitch=1280;std::vector<uint8_t> img(size_t(pitch)*ph);
    for(size_t i=0;i<img.size();++i)img[i]=uint8_t(i*31+7);
    std::vector<uint8_t> png;CHECK(png_encode(img.data(),pw,ph,pitch,true,png));
    uint32_t dw=0,dh=0;std::vector<uint8_t> rgba;CHECK(decodePng(png,dw,dh,rgba)&&dw==pw&&dh==ph);
    for(uint32_t y=0;y<ph;++y)for(uint32_t x=0;x<pw;++x) {
        const uint8_t* a=&img[size_t(y)*pitch+x*4];const uint8_t* b=&rgba[(size_t(y)*pw+x)*4];
        CHECK(b[0]==a[2]&&b[1]==a[1]&&b[2]==a[0]&&b[3]==a[3]);
    }
    CHECK(crc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926u&&adler32(reinterpret_cast<const uint8_t*>("Wikipedia"),9)==0x11e60398u);
    // Pixel statistics.
    std::vector<uint8_t> px(16*4*4,0);px[(1*4+2)*4+3]=9;px[(3*4+2)*4]=1;Pixels st;analyse(px.data(),4,4,16,4,st);
    CHECK(st.count==2&&st.minx==2&&st.maxx==2&&st.miny==1&&st.maxy==3&&st.cx==2.5&&st.cy==2.5&&st.alpha_max==9&&st.alpha_min==0);
    int64_t first=0;auto px2=px;px2[0]=1;CHECK(differing(px.data(),px2.data(),4,4,16,4,first)==1&&first==0);
    // FP16: exact conversions, unorm8 clamp, statistics over covered texels.
    CHECK(half_to_float(0x3c00)==1.0f&&half_to_float(0xc000)==-2.0f&&std::isinf(half_to_float(0x7c00))&&std::isnan(half_to_float(0x7e00)));
    CHECK(half_to_float(0x0001)==std::ldexp(1.0f,-24)&&half_to_float(0x3a00)==0.75f&&half_to_float(0x8000)==0.0f);
    CHECK(unorm8(-1)==0&&unorm8(2)==255&&unorm8(0.5f)==128&&unorm8(std::nanf(""))==0);
    {std::vector<uint8_t> f(2*8,0);const uint16_t a[4]={0x3a00,0x3800,0x4000,0x3c00};std::memcpy(f.data()+8,a,8);
     FloatStats fs;float_stats(f.data(),2,1,16,fs);CHECK(fs.count==1&&fs.max[0]==0.75&&fs.max[2]==2.0&&fs.mean[1]==0.5);
     ColorFormat cf;CHECK(color_format(dxgi::r16g16b16a16_float,cf));std::vector<uint8_t> o;CHECK(to_rgba8(f.data(),2,1,16,cf,o));
     CHECK(o.size()==8&&o[4]==191&&o[5]==128&&o[6]==255&&o[7]==255&&o[0]==0);
     Pixels pf;analyse(f.data(),2,1,16,8,pf);CHECK(pf.count==1&&pf.minx==1&&pf.alpha_max==0);}
    // Projection (diagnostic): GS-faithful screen positions, outcodes, clamping.
    {Shape g=s;g.vp0={0,0,256,144,0,1};g.topology=5;g.indexed=true;g.count=9;g.start=0;g.base=0;g.strip_cut=1;Targets u;u.width=256;u.height=144;
     std::array<uint8_t,8*64> vbuf{};const float q[8][2]={{-0.4f,-0.2f},{-0.4f,0.2f},{-0.05f,-0.2f},{-0.05f,0.2f},{0.05f,-0.2f},{0.05f,0.2f},{0.4f,-0.2f},{0.4f,0.2f}};
     for(int i=0;i<8;++i){const float v[4]={q[i][0],q[i][1],0,0};std::memcpy(vbuf.data()+64*i,v,16);} // w=0 like live: forced to 1
     const uint16_t idx[9]={0,1,2,3,0xffff,4,5,6,7};
     Projection cr;CHECK(std::string_view(project(g,u,vbuf.data(),0,8,64,reinterpret_cast<const uint8_t*>(idx),2,sub,cr))=="ok");
     CHECK(cr.vertices==8&&cr.triangles==4&&cr.clamped&&cr.top==0&&cr.bottom==144&&cr.culled[0]==0&&cr.w_nonpositive[0]==0);
     CHECK(std::fabs(cr.screen[0][0][0]-(128-0.4*16/30/0.6152*128))<1e-3&&cr.extent[2][0]>cr.extent[3][0]); // left eye shifted right
     CHECK(std::fabs(cr.clip[0][0][3]-30.0)<1e-3); // w = depth in front (> 0)
     auto behind=vbuf;const float z=2.5f;std::memcpy(behind.data()+64*7+8,&z,4);
     CHECK(std::string_view(project(g,u,behind.data(),0,8,64,reinterpret_cast<const uint8_t*>(idx),2,sub,cr))=="ok"&&cr.w_nonpositive[0]==1);
     auto all=vbuf;for(int i=0;i<8;++i)std::memcpy(all.data()+64*i+8,&z,4);
     CHECK(std::string_view(project(g,u,all.data(),0,8,64,reinterpret_cast<const uint8_t*>(idx),2,sub,cr))=="original-all-triangles-gs-rejected"&&cr.culled[0]==4);
     auto off=vbuf;for(int i=0;i<8;++i){const float x=q[i][0]+6.0f;std::memcpy(off.data()+64*i,&x,4);}
     CHECK(std::string_view(project(g,u,off.data(),0,8,64,reinterpret_cast<const uint8_t*>(idx),2,sub,cr))!="ok");
     CHECK(std::string_view(project(g,u,vbuf.data(),0,4097,64,nullptr,2,sub,cr))=="vertex-count-over-cap");
     CHECK(std::string_view(project(g,u,nullptr,0,0,64,nullptr,2,sub,cr))=="no-vertices");
     // outcode bits as the GS: w+x<0 16, w+y<0 8, w+z<0 4, w-x<0 2, w-y<0 1
     CHECK(outcode({0,0,0,1})==0&&outcode({0,0,0,-1})==31&&outcode({2,0,0,1})==2&&outcode({-2,0,0,1})==16);}
    // Box-filter downscale.
    {std::vector<uint8_t> in(4*2*4,0);for(int i=0;i<4;++i){in[size_t(i)*4]=uint8_t(i*40);in[size_t(i)*4+3]=255;}
     std::vector<uint8_t> out;uint32_t ow=0,oh=0;CHECK(downscale_factor(2560,640)==4&&downscale_factor(256,640)==1&&downscale_factor(1280,640)==2);
     CHECK(downscale_rgba8(in,4,2,2,out,ow,oh)&&ow==2&&oh==1&&out[0]==10&&out[3]==128&&out[4]==50);}
}
#include <dbghelp.h>
#pragma comment(lib,"dbghelp.lib")
static void traceTerminate(){void* fr[48];const USHORT n=CaptureStackBackTrace(0,48,fr,nullptr);HANDLE p=GetCurrentProcess();SymInitialize(p,nullptr,TRUE);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO)+256];auto* si=reinterpret_cast<SYMBOL_INFO*>(buf);
    for(USHORT i=0;i<n;++i){si->SizeOfStruct=sizeof(SYMBOL_INFO);si->MaxNameLen=255;DWORD64 d=0;if(SymFromAddr(p,DWORD64(fr[i]),&d,si))std::fprintf(stderr,"  %s\n",si->Name);}
    std::fflush(stderr);std::abort();}
// ------------------------------------------------------------- J-phase-observe: gpu_copy_step5_phase_observe
namespace po=acz::gpu_copy::phobs;
struct PoRt {uint64_t rt;uint64_t w;uint32_t h,fmt;float vw,vh;};
static const PoRt poScene{0xAAA,2560,1440,10,2560,1440};   // the first member's target (FP16 scene)
static const PoRt poSide{0xBBB,2560,1440,28,2560,1440};    // another target of the scene's size, same viewport
static const PoRt poTm{0xCCC,2405,1352,24,2405,1352};      // the R10G10B10A2 output (tone-map)
static const PoRt poOff{0xDDD,1280,720,28,1280,720};       // an offscreen target
static const PoRt poNone{0,0,0,0,2560,1440};               // a render target the tracking could not resolve
static po::Draw poDraw(po::Kind k,uint64_t id,const PoRt& r,uint32_t count=9,bool indexed=true,uint32_t inst=1) {
    po::Draw d;d.kind=uint8_t(k);d.id=id;d.pso=0x500+id;d.rt=r.rt;d.rtKnown=r.rt!=0;d.rtWidth=r.w;d.rtHeight=r.h;d.rtFormat=r.fmt;d.vpW=r.vw;d.vpH=r.vh;
    d.vpKnown=true;d.indexed=indexed;d.count=count;d.instances=inst;d.psoKnown=true;d.omCount=1;return d;
}
// The reference frame: pre(2) mem(3) btw(1) hud(3, one under an unresolved target) end(1) tm(3).
static std::vector<po::Draw> poBase() {
    return {poDraw(po::kind_other,1,poScene),poDraw(po::kind_acm,2,poScene),
            poDraw(po::kind_cpt,10,poScene),poDraw(po::kind_cpt,10,poScene),poDraw(po::kind_other,3,poScene),poDraw(po::kind_cpt,10,poScene),
            poDraw(po::kind_other,4,poScene,5,false),poDraw(po::kind_other,4,poScene),poDraw(po::kind_other,5,poNone),
            poDraw(po::kind_other,6,poSide),poDraw(po::kind_other,7,poTm),poDraw(po::kind_other,8,poScene),poDraw(po::kind_other,8,poScene)};
}
static void poFrame(po::State& st,const std::vector<po::Draw>& d,uint64_t list=0x10) {
    for(const auto& x:d)st.draw(list,7,x);
    CHECK(st.close(list));
}
static unsigned poCount(po::State& st,po::RecordType t) {unsigned n=0;for(uint64_t i=st.tail;i<st.head;++i)n+=st.queue[i%po::queue_records].type==t;return n;}
static void poDrain(po::State& st) {po::Record r;while(st.pop(r)){}}
static void step5_phase_observe_rules() {
    using namespace acz::diagnostics;
    // ---- key: default OFF, parses like the other step-5 keys, independent of the others
    CHECK(!Config{}.gpu_copy_step5_phase_observe()&&!parse_config("").config.gpu_copy_step5_phase_observe());
    for(const char* v:{"true","1"}){auto c=parse_config(std::string("gpu_copy_step5_phase_observe=")+v);
        CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_phase_observe()&&!c.config.gpu_copy_step5_continuous()&&!c.config.gpu_copy_step5_world_observe()&&
              !c.config.gpu_copy_step5_png_once());}
    for(const char* v:{"false","0"}){auto c=parse_config(std::string("gpu_copy_step5_phase_observe=")+v);CHECK(c.issue==ConfigIssue::none&&!c.config.gpu_copy_step5_phase_observe());}
    CHECK(parse_config("gpu_copy_step5_phase_observe=on").issue==ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_phase_observe=1\ngpu_copy_step5_phase_observe=0").issue==ConfigIssue::duplicate);
    // ---- classification: phases, target classes, region end and tone-map, per-phase sums
    {
        auto stBox=std::make_unique<po::State>();auto& st=*stBox;
        for(const auto& x:poBase())st.draw(0x10,7,x);
        auto* b=st.find(0x10);CHECK(b&&b->count==13&&b->cpt==3&&b->cptStored==3&&b->first==2&&b->last==5);
        CHECK(st.close(0x10)&&!st.find(0x10)&&st.cockpitLists==1&&st.lists==1);
        const auto& s=st.last;
        CHECK(s.total==13&&s.stored==13&&s.overflow==0&&s.members==3&&s.firstMember==3&&s.lastMember==6&&s.sceneKnown&&s.sceneRt==0xAAA&&s.sceneW==2560&&s.sceneH==1440);
        CHECK(s.p[po::ph_before].draws==2&&s.p[po::ph_member].draws==3&&s.p[po::ph_between].draws==1&&s.p[po::ph_hud].draws==3&&
              s.p[po::ph_after_region].draws==1&&s.p[po::ph_after_tonemap].draws==3);
        CHECK(s.regionEndOrd==10&&s.tonemapOrd==11); // the first proven target change (the side target), the first R10 draw
        // the unresolved target did not end the region; it is counted
        CHECK(s.p[po::ph_hud].unknownRt==1&&st.unknownRtDraws==1&&s.p[po::ph_hud].onScene==2);
        CHECK(s.p[po::ph_hud].indexed==2&&s.p[po::ph_hud].idx==18&&s.p[po::ph_hud].vtx==5&&s.p[po::ph_hud].inst==3);
        CHECK(s.p[po::ph_after_tonemap].onScene==2&&st.tailSceneDraws==2&&s.p[po::ph_after_tonemap].offVp==1); // the tone-map pass has its own viewport
        CHECK(s.p[po::ph_after_region].onScene==0&&s.p[po::ph_after_region].offVp==0);
        // every phase and class as the records carry them (summary + 14 draws on frame 1: all phases incl. members)
        std::vector<po::Record> recs;po::Record r;while(st.pop(r))recs.push_back(r);
        CHECK(recs.size()==14&&recs[0].type==po::rec_summary&&recs[0].reason==po::reason_first&&recs[0].frame==1&&recs[0].dumpDraws==13&&recs[0].omitted==0);
        const uint8_t wantPhase[13]={0,0,1,1,2,1,3,3,3,4,5,5,5};
        const uint8_t wantClass[13]={1,1,1,1,1,1,1,1,0,2,3,1,1};
        for(unsigned i=0;i<13;++i)CHECK(recs[1+i].draw.phase==wantPhase[i]&&recs[1+i].draw.rtClass==wantClass[i]&&recs[1+i].draw.ordinal==i+1);
        CHECK(recs[1].draw.psoNew&&recs[3].draw.psoNew&&!recs[4].draw.psoNew); // the second draw of an id is not new
    }
    // a viewport different from the first member's, with a known same-size target, starts the tone-map; without a proven target change the region never ends
    {
        auto stBox=std::make_unique<po::State>();auto& st=*stBox;
        std::vector<po::Draw> d={poDraw(po::kind_cpt,10,poScene),poDraw(po::kind_other,4,poNone),poDraw(po::kind_other,7,poNone)};d[2].vpW=2405;d[2].vpH=1352;
        poFrame(st,d);
        CHECK(st.last.p[po::ph_hud].draws==2&&st.last.p[po::ph_after_tonemap].draws==0&&st.last.regionEndOrd==0&&st.last.p[po::ph_hud].offVp==1);
        auto st2Box=std::make_unique<po::State>();auto& st2=*st2Box;
        d={poDraw(po::kind_cpt,10,poScene),poDraw(po::kind_other,4,poOff),poDraw(po::kind_other,5,poScene)}; // an offscreen target of another viewport: region end + tone-map class at once
        poFrame(st2,d);
        CHECK(st2.last.p[po::ph_after_tonemap].draws==2&&st2.last.p[po::ph_after_region].draws==0&&st2.last.regionEndOrd==2&&st2.last.tonemapOrd==2);
    }
    // ---- logging decisions: first 3 frames, then only a new signature / new PSO / a change (budgeted) / the 5-s sample
    {
        auto stBox=std::make_unique<po::State>();auto& st=*stBox;
        poFrame(st,poBase());
        CHECK(poCount(st,po::rec_summary)==1&&poCount(st,po::rec_draw)==13);
        poFrame(st,poBase());poFrame(st,poBase());
        CHECK(poCount(st,po::rec_summary)==3&&poCount(st,po::rec_draw)==13+10+10&&st.dumps==3&&st.dumpReason[po::reason_first]==3);
        poDrain(st);
        poFrame(st,poBase());CHECK(st.head==st.tail&&st.dumps==3&&st.cockpitLists==4); // identical: counted, not logged
        auto extra=poBase();extra.insert(extra.begin()+8,poDraw(po::kind_other,9,poScene,24)); // a new draw (new PSO) inside the HUD region
        poFrame(st,extra);
        {po::Record r;CHECK(st.pop(r)&&r.type==po::rec_summary&&r.reason==po::reason_new_signature&&r.newSig&&r.changed==(1u<<po::ph_hud)&&r.dumpDraws==4&&r.omitted==0&&
              r.sum.p[po::ph_hud].draws==4&&r.sum.p[po::ph_hud].newPso==1&&r.prevSig!=r.sum.sig&&r.frame==5);
         unsigned n=0;while(st.pop(r)){CHECK(r.type==po::rec_draw&&r.draw.phase==po::ph_hud);n+=r.draw.psoNew;}CHECK(n==1);}
        poFrame(st,extra);CHECK(st.head==st.tail&&st.newSigs==2); // same again: nothing
        // back to the base signature: seen before, but changed versus the previous frame -> a budgeted change dump
        poFrame(st,poBase());
        {po::Record r;CHECK(st.pop(r)&&r.reason==po::reason_change&&!r.newSig&&r.changed==(1u<<po::ph_hud)&&r.dumpDraws==3);poDrain(st);}
        CHECK(st.budget==po::dump_budget-1);
        unsigned changes=1;for(unsigned i=0;i<12;++i){poFrame(st,(i%2)?poBase():extra);poDrain(st);}
        changes=unsigned(st.dumpReason[po::reason_change]);
        CHECK(changes==po::dump_budget&&st.suppressed==1+6&&st.budget==0); // the budget is spent; the rest is counted, not logged
        // the sample: due, any frame that logs nothing else
        st.due=true;poFrame(st,poBase());
        {po::Record r;CHECK(st.pop(r)&&r.reason==po::reason_sample&&!st.due);unsigned members=0,draws=0;while(st.pop(r)){++draws;members+=r.draw.phase==po::ph_member;}CHECK(members==0&&draws==10);}
        st.due=true;st.budget=po::dump_budget;poFrame(st,extra);CHECK(poCount(st,po::rec_summary)==1&&!st.due); // a logged change counts as the sample
        poDrain(st);
        // the lifetime caps: signature table (64), then new PSOs still flag a dump
        for(unsigned k=1;k<=70;++k){auto x=poBase();for(unsigned j=0;j<k;++j)x.insert(x.begin()+8,poDraw(po::kind_other,100,poScene,24));poFrame(st,x);poDrain(st);}
        CHECK(st.sigCount==po::sig_cap&&st.sigFull>0);
        {auto x=poBase();x.insert(x.begin()+8,poDraw(po::kind_other,555,poScene));x.insert(x.begin()+8,poDraw(po::kind_other,100,poScene,24));st.due=false;const auto dumps=st.dumps;poFrame(st,x);
         po::Record r;CHECK(st.pop(r)&&r.reason==po::reason_new_pso&&!r.newSig&&st.dumps==dumps+1);poDrain(st);}
        CHECK(st.dumps<=po::dump_total_cap);
    }
    // ---- bounds
    {
        auto stBox=std::make_unique<po::State>();auto& st=*stBox;
        // 1100 draws: the first 1024 are stored, the rest counted; the cap per dump omits and says so
        std::vector<po::Draw> d;d.push_back(poDraw(po::kind_cpt,10,poScene));for(unsigned i=0;i<1099;++i)d.push_back(poDraw(po::kind_other,20+i%40,poScene));
        poFrame(st,d);
        CHECK(st.overflowFrames==1&&st.last.total==1100&&st.last.stored==po::draw_cap&&st.last.overflow==76&&st.draws==1100);
        po::Record r;CHECK(st.pop(r)&&r.type==po::rec_summary&&r.dumpDraws==po::dump_draw_cap&&r.omitted>0&&r.sum.overflow==76);
        poDrain(st);
        // a list recorded without a DrawCPT draw leaves nothing; a Reset discards; more lists than buffers evicts the oldest (counted)
        auto s2Box=std::make_unique<po::State>();auto& s2=*s2Box;
        s2.draw(0x20,1,poDraw(po::kind_acm,1,poScene));CHECK(!s2.close(0x20)&&s2.noCockpit==1&&s2.head==s2.tail&&!s2.find(0x20));
        s2.draw(0x21,1,poDraw(po::kind_cpt,1,poScene));s2.discard(0x21);CHECK(!s2.find(0x21)&&s2.discarded==1&&!s2.close(0x21));
        for(unsigned l=0;l<po::list_cap+3;++l)s2.draw(0x30+l,1,poDraw(po::kind_cpt,1,poScene));
        CHECK(s2.evicted==3&&!s2.find(0x30)&&s2.find(0x30+po::list_cap+2));
        for(unsigned i=0;i<po::queue_records+30;++i)s2.push(po::Record{});CHECK(s2.lost==30);
        // two lists interleaved keep separate recordings
        auto s3Box=std::make_unique<po::State>();auto& s3=*s3Box;
        s3.draw(0xA,1,poDraw(po::kind_cpt,1,poScene));s3.draw(0xB,1,poDraw(po::kind_other,2,poScene));s3.draw(0xA,1,poDraw(po::kind_other,3,poScene));
        CHECK(s3.find(0xA)->count==2&&s3.find(0xB)->count==1&&s3.close(0xA)&&!s3.close(0xB)&&s3.last.p[po::ph_hud].draws==1);
    }
    // ---- formatting: worst-case line bounds, typical lines are not truncated
    {
        const uint64_t M=UINT64_MAX;const uint32_t U32=UINT32_MAX;
        po::Record big;big.frame=M;big.list=M;big.generation=M;big.prevSig=M;big.reason=po::reason_new_signature;big.changed=0x3f;big.newSig=true;big.omitted=U32;big.dumpDraws=U32;
        auto& s=big.sum;s.sig=M;s.total=s.stored=s.overflow=s.members=s.firstMember=s.lastMember=s.regionEndOrd=s.tonemapOrd=U32;s.sceneRt=s.sceneW=M;s.sceneH=s.sceneFormat=U32;s.sceneKnown=true;
        for(auto& p:s.p){p.draws=p.indexed=p.onScene=p.unknownRt=p.offVp=p.newPso=U32;p.idx=p.vtx=p.inst=p.shape=M;}
        auto& d=big.draw;d.pso=d.id=d.rtv=d.rt=d.dsv=d.ds=d.rtWidth=M;d.rtHeight=d.rtFormat=d.dsFormat=d.vs8=d.ps8=d.count=d.instances=d.ordinal=U32;d.vpW=d.vpH=-1.2345678e30f;
        d.stencilRead=d.stencilWrite=U32;d.rcMask=15;for(auto& w:d.rc)w=U32;d.blendSrc=d.blendDst=d.blendOp=d.blendSrcA=d.blendDstA=d.blendOpA=d.depthFunc=d.frontFunc=d.backFunc=U32;
        d.frontPass=d.backPass=d.writeMask=U32;d.kind=po::kind_other;d.phase=po::ph_after_region;d.rtClass=po::rt_scene_size;d.omCount=255;
        d.indexed=d.psoKnown=d.rtKnown=d.dsKnown=d.vpKnown=d.blend=d.independentBlend=d.depthEnable=d.depthWrite=d.stencil=d.rcAvailable=d.psoNew=d.vpOffScene=true;
        char a[po::line_bytes],b[po::line_bytes];
        for(auto t:{po::rec_summary,po::rec_draw}){big.type=t;po::format(big,a,b);CHECK(std::strlen(a)<po::line_bytes&&std::strlen(b)<po::line_bytes&&a[0]&&(t!=po::rec_draw||b[0]));
            if(std::getenv("ACZ_PH_DUMP"))std::fprintf(stderr,"worst %d: a=%zu b=%zu\n%s\n%s\n",int(t),std::strlen(a),std::strlen(b),a,b);}
        auto stBox=std::make_unique<po::State>();auto& st=*stBox;
        auto base=poBase();base[6].rcAvailable=true;base[6].rcMask=5;base[6].rc[0]=0x1234;base[6].rc[2]=0xdeadbeef;base[6].blend=true;base[6].blendSrc=5;base[6].blendDst=6;base[6].blendOp=1;
        base[6].depthEnable=true;base[6].depthFunc=8;base[6].stencil=true;base[6].frontFunc=3;base[6].backFunc=3;
        base[6].rtv=0x7ff6a0b4c8;base[6].dsv=0x7ff6a0b4d0;base[6].ds=0x1f3a0e30c0;base[6].dsFormat=45;
        poFrame(st,base);
        unsigned lines=0;po::Record r;bool sawRc=false,sawFrame=false;
        while(st.pop(r)) {
            po::format(r,a,b);++lines;CHECK(a[0]&&!std::strstr(a,"truncated=1")&&!std::strstr(b,"truncated=1"));
            if(std::getenv("ACZ_PH_DUMP"))std::fprintf(stderr,"%s\n%s\n",a,b);
            if(r.type==po::rec_summary) {
                sawFrame=true;
                CHECK(std::string_view(a).find("phase-observe;event=frame;frame=1;list=10;gen=7;reason=first;draws=13;stored=13;overflow=0;members=3;first-member-ord=3;last-member-ord=6;"
                    "region-end-ord=10;tonemap-ord=11;scene-rt=aaa(2560x1440,fmt:10,known:1);")==0);
                CHECK(std::string_view(b).find(";pre=2/2/2/0/0/2/")!=std::string::npos&&std::string_view(b).find(";hud=3/2/2/1/0/")!=std::string::npos&&std::string_view(b).find(";tm=3/3/2/0/1/")!=std::string::npos);
            }
            if(r.type==po::rec_draw&&r.draw.ordinal==7) {
                sawRc=true;
                CHECK(std::string_view(b).find("phase-observe;event=draw-state;frame=1;ord=7;phase=hud-region;pso-known=1;depth=1,always,w0;stencil=1,front:equal,back:equal,")==0&&
                      std::string_view(b).find(";blend=1,src:5,dst:6,op:1,")!=std::string::npos&&std::string_view(b).find(";rc=mask:5:00001234,????????,deadbeef,????????")!=std::string::npos&&
                      std::string_view(b).find("ref:untracked")!=std::string::npos&&std::string_view(b).find("factor:untracked")!=std::string::npos);
                CHECK(std::string_view(a).find("phase-observe;event=draw;frame=1;ord=7;phase=hud-region;rt-class=scene;rt=aaa;rt-size=2560x1440;rt-fmt=10;rtv=7ff6a0b4c8;dsv=7ff6a0b4d0;ds=1f3a0e30c0;ds-fmt=45;")==0&&
                      std::string_view(a).find(";shader=other;")!=std::string::npos&&std::string_view(a).find("indexed=0;count=5;instances=1")!=std::string::npos);
            }
        }
        CHECK(lines==14&&sawRc&&sawFrame); // 1 summary + 13 draws (two lines each)
    }
    // ---- the headset key set with the observer (23 keys) stays inside the 24-key cap
    const char* keys[]={"log_enabled=true","diagnostic_ownership_pins=true","gpu_copy_witness_observation=true","gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true","gpu_copy_step3_whole_set=true","gpu_copy_step5_continuous=true","xr_stereo_output=true","xr_edge_feather=0.06",
        "gpu_copy_step5_members_by_class=true","gpu_copy_step5_hmd_fov=true","gpu_copy_step5_hud=true","xr_hud_layer=true","xr_hud_quad_distance_m=0.8",
        "xr_hud_scale=0.5","xr_world_quad_distance_m=500","gpu_copy_step5_units_per_metre=87","gpu_copy_step5_hud_markers=true","xr_hud_markers_layer=true",
        "xr_world_in_projection=true","gpu_copy_step5_world_objects=true","gpu_copy_step5_world_observe=true","gpu_copy_step5_phase_observe=true"};
    std::string cfg;for(const char* k:keys)cfg+=std::string(k)+"\n";
    {auto c=parse_config(cfg);CHECK(c.issue==ConfigIssue::none&&c.config.gpu_copy_step5_phase_observe()&&c.config.gpu_copy_step5_world_observe()&&c.config.gpu_copy_step5_continuous());}
    CHECK(std::size(keys)==23&&std::size(keys)<=max_config_keys&&cfg.size()<=max_config_bytes);
}
// Whole frames through the producer fixtures: members, the HUD region, the output pass (an R10 target view), a late tail on the scene target.
// Key ON must not change one recorded command (digest of every op and event) nor one published frame.
static void step5_phase_observe() {
    hudSetup(false);
    auto& s5=s5ref();auto* src=acz::stereo::frame_source();
    const auto script=[&]{hudBindScene();hudDraws(3);hudOutput();hudBindScene();hudDraws(2);};
    // The recorded stream depends on the ring slot (upload offsets, private targets), so compare frame i of a key-off run with frame i of a key-on run
    // of the same length (the slot rotation repeats): six frames each.
    constexpr unsigned N=6;
    std::vector<uint64_t> digOff,digOn;std::vector<std::vector<std::string>> evOff,evOn;std::vector<unsigned> slotOff,slotOn;
    for(unsigned i=0;i<N;++i){w->lists[0].ops.clear();hudFrame(0,script);digOff.push_back(flagsDigest());evOff.push_back(frameEvents());slotOff.push_back(s5.lastSlot);}
    const auto pubOff=src->frames_published();
    CHECK(pubOff==N&&acz::g_ph.draws==0&&acz::g_ph.cockpitLists==0&&acz::g_ph.lists==0);
    ticks(60);CHECK(dedicated().find("phase-observe")==std::string::npos); // key off: no line, no state
    acz::g_phaseObserveTest=true;
    // the armed-window control files live in a private temporary directory (never the real %LOCALAPPDATA%\acz-vr)
    const std::filesystem::path ctlDir=makeScratchDir(L"acz-ph-ctl");
    acz::g_phControlDirTest=ctlDir.wstring();
    for(unsigned i=0;i<N;++i){w->lists[0].ops.clear();hudFrame(0,script);digOn.push_back(flagsDigest());evOn.push_back(frameEvents());slotOn.push_back(s5.lastSlot);}
    if(std::getenv("ACZ_PH_DUMP")){for(unsigned i=0;i<N;++i)std::fprintf(stderr,"dig[%u] off %016llx on %016llx events %zu %zu\n",i,(unsigned long long)digOff[i],(unsigned long long)digOn[i],evOff[i].size(),evOn[i].size());
        std::fprintf(stderr,"published %llu skipped %llu disabled %d\n",(unsigned long long)src->frames_published(),(unsigned long long)src->frames_skipped(),int(s5.disabled));}
    {   // per ring slot: one digest key-off, the same digest key-on; the events are the same list
        std::map<unsigned,uint64_t> bySlot;bool consistent=true;
        for(unsigned i=0;i<N;++i){auto it=bySlot.find(slotOff[i]);if(it==bySlot.end())bySlot[slotOff[i]]=digOff[i];else consistent&=it->second==digOff[i];}
        CHECK(consistent);unsigned matched=0;
        for(unsigned i=0;i<N;++i){auto it=bySlot.find(slotOn[i]);if(it!=bySlot.end()){CHECK(it->second==digOn[i]);++matched;}}
        CHECK(matched>=3&&bySlot.size()>=2); // the key-on frames landed in slots the key-off frames also used
    }
    CHECK(evOn==evOff&&src->frames_published()==pubOff+N&&src->frames_skipped()==0&&s5.disabled==false);
    {std::lock_guard lock(acz::g_phMutex);
     const auto& s=acz::g_ph.last;
     if(std::getenv("ACZ_PH_DUMP"))std::fprintf(stderr,"phases pre=%u mem=%u btw=%u hud=%u end=%u tm=%u total=%u scene-known=%u region-end=%u tm-ord=%u\n",s.p[0].draws,s.p[1].draws,s.p[2].draws,
        s.p[3].draws,s.p[4].draws,s.p[5].draws,s.total,unsigned(s.sceneKnown),s.regionEndOrd,s.tonemapOrd);
     CHECK(acz::g_ph.cockpitLists==N&&acz::g_ph.dumps==3&&s.members==8&&s.p[po::ph_member].draws==8&&s.p[po::ph_before].draws==0&&s.p[po::ph_hud].draws==3&&
           s.p[po::ph_after_tonemap].draws==3&&s.p[po::ph_after_tonemap].onScene==2&&s.sceneKnown&&s.total==14);}
    ticks(60);
    const auto text=dedicated();
    if(std::getenv("ACZ_PH_DUMP")){size_t at=0;while((at=text.find("phase-observe",at))!=std::string::npos){const size_t e=text.find('\n',at);std::fprintf(stderr,"%s\n",text.substr(at,e-at).c_str());at=e==std::string::npos?text.size():e;}}
    CHECK(hasText(text,"phase-observe;event=frame;frame=1;","reason=first;draws=14;stored=14;overflow=0;members=8;"));
    CHECK(hasText(text,"phase-observe;event=frame-phases;frame=1;",";mem=8/8/"));
    CHECK(hasText(text,"phase-observe;event=draw;frame=1;ord=9;phase=hud-region;rt-class=scene;")); // the first HUD draw follows the 8 members
    CHECK(hasText(text,"phase-observe;event=draw;frame=1;","phase=after-tonemap;rt-class=r10-output;"));
    CHECK(hasText(text,"phase-observe;event=draw-state;frame=1;","phase=hud-region;pso-known=1;depth=1,always,w0;stencil=1,front:always,back:always,"));
    CHECK(hasText(text,"event=phase-observe-definitions;part=1;key=gpu_copy_step5_phase_observe;read-only=no-command,no-gpu-work,no-readback"));
    CHECK(hasText(text,"event=phase-observe-definitions;part=3;","stencil-ref=untracked;blend-factor=untracked;"));
    CHECK(hasText(text,"event=continuous-phase-observe;key=gpu_copy_step5_phase_observe;cockpit-frames=6;lists-closed=6;no-cockpit-lists=0;"));
    CHECK(hasText(text,"event=phase-observe-definitions;part=4;","caps=1024-draws-per-recording,200-draw-lines-per-frame,64-signatures,256-psos,200-frames"));
    CHECK(hasText(text,"event=continuous-phase-observe-last;key=gpu_copy_step5_phase_observe;frame=6;","draws-per-phase=pre:0,mem:8,btw:0,hud:3,end:0,tm:3;late-tail-draws-on-scene-rt=2("));
    for(const auto& l:w->lines)CHECK(l.find("phase-observe")==std::string::npos); // dedicated log only
    // a second identical frame: counted, not logged; a frame with one more HUD draw (new PSO + signature) is
    w->lists[0].ops.clear();hudFrame(0,script);
    // (the status ticks above made the 5-s sample due: the first unchanged frame after them is that sample)
    {std::lock_guard lock(acz::g_phMutex);CHECK(acz::g_ph.cockpitLists==N+1&&acz::g_ph.dumps==4&&acz::g_ph.dumpReason[po::reason_sample]==1&&!acz::g_ph.due);}
    w->lists[0].ops.clear();hudFrame(0,script);
    {std::lock_guard lock(acz::g_phMutex);CHECK(acz::g_ph.cockpitLists==N+2&&acz::g_ph.dumps==4);} // identical and no sample due: counted, not logged
    w->lists[0].ops.clear();hudFrame(0,[&]{hudBindScene();hudDraws(4);hudOutput();hudBindScene();hudDraws(2);});
    ticks(1);
    {std::lock_guard lock(acz::g_phMutex);CHECK(acz::g_ph.cockpitLists==N+3&&acz::g_ph.last.p[po::ph_hud].draws==4&&acz::g_ph.dumps==5&&acz::g_ph.dumpReason[po::reason_new_signature]==1);}
    // ---- armed window through the control files: the baseline poll saw no files; creating phase-observe.arm opens a window, .mark logs a mark
    {
        const auto touch=[&](const char* name,const char* text){std::ofstream f(ctlDir/name,std::ios::trunc);f<<text;};
        {std::lock_guard lock(acz::g_phMutex);CHECK(acz::g_ph.pollInit&&acz::g_ph.staleIgnored==0&&!acz::g_ph.win.active);}
        acz::step5PhaseObservePollNow(GetTickCount64());
        {std::lock_guard lock(acz::g_phMutex);CHECK(!acz::g_ph.win.active&&acz::g_ph.windowsStarted==0);}
        touch("phase-observe.arm","arm");
        acz::step5PhaseObservePollNow(GetTickCount64());
        {std::lock_guard lock(acz::g_phMutex);CHECK(acz::g_ph.win.active&&acz::g_ph.windowsStarted==1&&acz::g_ph.win.index==1);}
        for(unsigned i=0;i<3;++i){w->lists[0].ops.clear();hudFrame(0,script);}
        touch("phase-observe.mark","mark");
        acz::step5PhaseObservePollNow(GetTickCount64());
        {std::lock_guard lock(acz::g_phMutex);CHECK(acz::g_ph.marks==1&&acz::g_ph.win.markSeen);}
        for(unsigned i=0;i<2;++i){w->lists[0].ops.clear();hudFrame(0,script);}
        ticks(60);
        const auto wtext=dedicated();
        if(std::getenv("ACZ_PH_DUMP")){size_t at=0;while((at=wtext.find("phase-observe;event=phase-observe",at))!=std::string::npos){const size_t e=wtext.find('\n',at);std::fprintf(stderr,"%s\n",wtext.substr(at,e-at).c_str());at=e==std::string::npos?wtext.size():e;}}
        CHECK(hasText(wtext,"phase-observe;event=phase-observe-armed;accepted=1;window=1/4;","length-ms=20000;learn-ms=1500;"));
        CHECK(hasText(wtext,"phase-observe;event=phase-observe-mark;mark=1;","window=1;since-arm-ms="));
        CHECK(hasText(wtext,"phase-observe;event=frame;","reason=window;")&&hasText(wtext,";win=1;t=",";since-mark-ms="));
        CHECK(hasText(wtext,"event=continuous-phase-observe-window;key=gpu_copy_step5_phase_observe;control-files=%LOCALAPPDATA%/acz-vr/phase-observe.arm,phase-observe.mark;windows-started=1/4;"));
        CHECK(hasText(wtext,"event=phase-observe-definitions;part=6;pso-identity=creation-hashes"));
        // every cockpit frame of the window is logged (3 + 2 here), two lines each
        {std::lock_guard lock(acz::g_phMutex);CHECK(acz::g_ph.win.framesLogged==5&&acz::g_ph.win.frames==5);}
        // touching the arm file while the window is open is refused and logged
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        touch("phase-observe.arm","arm again");
        acz::step5PhaseObservePollNow(GetTickCount64());
        {std::lock_guard lock(acz::g_phMutex);CHECK(acz::g_ph.windowsStarted==1&&acz::g_ph.armRefused==1);}
        ticks(2);CHECK(hasText(dedicated(),"phase-observe;event=phase-observe-armed;accepted=0;reason=window-active;"));
        // an open window ends by time (a frame or a poll past its end)
        {std::lock_guard lock(acz::g_phMutex);acz::g_ph.end_window(GetTickCount64(),"time");}
        ticks(2);CHECK(hasText(dedicated(),"phase-observe;event=phase-observe-window-end;window=1/4;reason=time;"));
        std::filesystem::remove_all(ctlDir);acz::g_phControlDirTest.clear();
    }
    // key OFF again: nothing more is observed
    acz::g_phaseObserveTest=false;
    const auto drawsBefore=acz::g_ph.draws;
    w->lists[0].ops.clear();hudFrame(0,script);
    {std::lock_guard lock(acz::g_phMutex);CHECK(acz::g_ph.draws==drawsBefore&&acz::g_ph.cockpitLists==N+3+5);} // +5: the window frames above
    CHECK(src->frames_skipped()==0&&phase()==int(acz::Step1Phase::continuous)&&verdicts()==0);
    acz::g_hudReplayTest=false;
}
// ------------------------------------------------------------- J-phase-observe: stable PSO identity and the armed capture windows (State level)
static po::Draw poHashed(po::Kind k,uint64_t id,const PoRt& r,uint32_t vs8,uint32_t ps8,uint32_t count=9) {
    auto d=poDraw(k,id,r,count);d.vs8=vs8;d.ps8=ps8;return d;
}
static void poFrameAt(po::State& st,const std::vector<po::Draw>& d,uint64_t now,uint64_t list=0x10) {
    for(const auto& x:d)st.draw(list,7,x);
    CHECK(st.close(list,now));
}
// The reference frame with creation hashes: ids differ per call for the churning draw (same shaders, new PSO object every frame).
static std::vector<po::Draw> poHashedBase(uint64_t churnId) {
    return {poHashed(po::kind_other,1,poScene,0x11,0x21),poHashed(po::kind_acm,2,poScene,0xb569109e,0xc1b58903),
            poHashed(po::kind_cpt,10,poScene,0x12,0x22),poHashed(po::kind_cpt,10,poScene,0x12,0x22),poHashed(po::kind_other,3,poScene,0x13,0x23),poHashed(po::kind_cpt,10,poScene,0x12,0x22),
            poHashed(po::kind_other,4,poScene,0x14,0x24),poHashed(po::kind_other,5,poNone,0x15,0x25),poHashed(po::kind_other,6,poSide,0x16,0x26),
            poHashed(po::kind_other,7,poTm,0x17,0x27),poHashed(po::kind_other,churnId,poTm,0xa16573be,0x811cfc0e,1)};
}
static void poRecordsFormat(const po::Record& r,std::string& a,std::string& b) {
    char x[po::line_bytes],y[po::line_bytes];po::format(r,x,y);a=x;b=y;
    CHECK(a.size()<po::line_bytes&&b.size()<po::line_bytes&&a.find("truncated=1")==std::string::npos&&b.find("truncated=1")==std::string::npos);
}
static void step5_phase_observe_window() {
    // ---- (c) the PSO's identity is its creation hashes + state, not the pointer / creation id
    {
        auto d1=poHashed(po::kind_other,100,poTm,0xa16573be,0x811cfc0e,1),d2=poHashed(po::kind_other,200,poTm,0xa16573be,0x811cfc0e,1);
        d2.pso=0x999;
        CHECK(po::pso_key(d1)==po::pso_key(d2)&&po::shape_of(d1)==po::shape_of(d2));
        auto d3=d1;d3.blend=true;CHECK(po::pso_key(d3)!=po::pso_key(d1));
        auto d4=d1;d4.ps8=0x811cfc0f;CHECK(po::pso_key(d4)!=po::pso_key(d1));
        auto d5=d1;d5.depthFunc=3;CHECK(po::pso_key(d5)!=po::pso_key(d1));
        auto n1=poDraw(po::kind_other,100,poTm),n2=poDraw(po::kind_other,200,poTm); // no creation hashes (fixtures): the id, as before
        CHECK(po::pso_key(n1)==100&&po::pso_key(n1)!=po::pso_key(n2));
        auto stBox=std::make_unique<po::State>();auto& st=*stBox;
        for(unsigned i=0;i<12;++i)poFrameAt(st,poHashedBase(1000+i*7),i);
        // 12 frames, a 1x1 draw whose PSO object changes every frame: one signature, no PSO that is new after frame 1
        CHECK(st.sigCount==1&&st.newSigs==1&&st.dumpReason[po::reason_new_signature]==0&&st.dumpReason[po::reason_new_pso]==0);
        CHECK(st.psoCount==9&&st.newPsos==9); // one entry per distinct creation identity (the three DrawCPT draws share one)
    }
    // ---- control-file poll protocol (stamps: 0 = absent)
    {
        auto stBox=std::make_unique<po::State>();auto& st=*stBox;
        st.poll(1000,111,0,0);CHECK(st.pollInit&&st.staleIgnored==0&&st.windowsStarted==0&&st.marks==0);
        st.poll(1200,112,0,0);CHECK(st.windowsStarted==0);
        auto s2Box=std::make_unique<po::State>();auto& s2=*s2Box;
        s2.poll(1000,111,77,88);CHECK(s2.staleIgnored==2&&s2.windowsStarted==0&&s2.marks==0); // files left from an earlier session are not orders
        s2.poll(1200,112,77,88);CHECK(s2.windowsStarted==0&&s2.marks==0);
        s2.poll(1400,113,0,0);CHECK(s2.windowsStarted==0);                                      // removing them is not an order either
        s2.poll(1600,114,90,0);CHECK(s2.windowsStarted==1&&s2.win.active&&s2.win.startMs==1600&&s2.win.endMs==21600&&s2.marks==0);
        s2.poll(1800,115,90,0);CHECK(s2.windowsStarted==1);                                    // an unchanged stamp does nothing
        s2.poll(2000,116,90,95);CHECK(s2.marks==1&&s2.win.markSeen&&s2.win.markMs==2000);
        s2.poll(2200,117,91,95);CHECK(s2.windowsStarted==1&&s2.armRefused==1);                // re-touching while the window is open is refused (logged)
        po::Record r;std::vector<po::Record> recs;while(s2.pop(r))recs.push_back(r);
        CHECK(recs.size()==3&&recs[0].type==po::rec_armed&&recs[0].accepted&&recs[1].type==po::rec_mark&&recs[1].sinceArm==400&&recs[2].type==po::rec_armed&&!recs[2].accepted&&
              std::string_view(recs[2].why)=="window-active");
        s2.poll(21700,118,91,95);CHECK(!s2.win.active&&s2.windowsEnded==1);                     // the poll ends a window whose time is up even without cockpit frames
        s2.poll(21900,119,92,95);CHECK(s2.windowsStarted==2&&s2.win.active);
    }
    // ---- one window: reference = the last frame at arming (+ the first 1.5 s), every frame logged, novel draws logged once per key
    {
        auto stBox=std::make_unique<po::State>();auto& st=*stBox;
        for(unsigned i=0;i<20;++i)poFrameAt(st,poHashedBase(500+i),i*16); // saturate nothing, but fill the tables (all frames identical in shape)
        poDrain(st);
        const unsigned oldPsos=st.psoCount,oldSigs=st.sigCount;CHECK(oldPsos==9&&oldSigs==1);
        // saturate the legacy caps to prove the arm resets them: 300 distinct PSOs and 70 distinct signatures
        for(unsigned i=0;i<70;++i){auto d=poHashedBase(0);for(unsigned k=0;k<=i;++k)d.push_back(poHashed(po::kind_other,50+k,poTm,0x70000+k,0x80000+k));poFrameAt(st,d,400+i);}
        poDrain(st);
        CHECK(st.sigCount==po::sig_cap&&st.sigFull>0);
        const uint64_t t0=100000;
        st.arm(t0,1792859465303ull);
        CHECK(st.win.active&&st.win.index==1&&st.psoCount>0&&st.psoCount<=po::pso_cap&&st.sigCount==1&&st.win.endMs==t0+po::window_ms&&st.win.learnUntil==t0+po::window_learn_ms);
        {po::Record r;CHECK(st.pop(r)&&r.type==po::rec_armed&&r.accepted&&r.a0==1&&r.a1>0&&r.a2==po::sig_cap&&r.a4==1&&r.a3==st.win.refCount&&r.utc==1792859465303ull);
         std::string a,b;poRecordsFormat(r,a,b);
         CHECK(a.find("phase-observe;event=phase-observe-armed;accepted=1;window=1/4;t=100000;utc-ms=1792859465303;length-ms=20000;learn-ms=1500;ends-t=120000;bytes-cap=2097152;")==0);
         CHECK(a.find("prior-state=saturated;reset=psos:")!=std::string::npos);}
        // a frame during the learning period: logged as a window frame, nothing is novel (its keys join the reference)
        auto quiet=poHashedBase(5000);
        poFrameAt(st,quiet,t0+100);
        {po::Record r;CHECK(st.pop(r)&&r.type==po::rec_summary&&r.reason==po::reason_window&&r.winIndex==1&&r.novel==0&&r.sinceArm==100&&r.sinceMark==-1&&!st.pop(r));}
        // after learning: a frame with three draws of a PSO never seen (hud phase) and one of another (after-tonemap)
        auto firing=poHashedBase(5001);
        firing.insert(firing.begin()+7,poHashed(po::kind_other,60,poScene,0x600,0x601));firing.insert(firing.begin()+7,poHashed(po::kind_other,61,poScene,0x600,0x601));
        firing.insert(firing.begin()+7,poHashed(po::kind_other,62,poScene,0x600,0x601));firing.push_back(poHashed(po::kind_other,63,poTm,0x700,0x701,1));
        poFrameAt(st,firing,t0+3000);
        {po::Record r;CHECK(st.pop(r)&&r.type==po::rec_summary&&r.winIndex==1&&r.novel==4&&r.novelPhase[po::ph_hud]==3&&r.novelPhase[po::ph_after_tonemap]==1&&r.sinceArm==3000);
         std::string a,b;poRecordsFormat(r,a,b);
         CHECK(a.find(";win=1;t=103000;since-arm-ms=3000;since-mark-ms=none;ref-novel=4(0/0/0/3/0/1)")!=std::string::npos&&b.find("phase-observe;event=frame-phases;")==0);
         unsigned draws=0;while(st.pop(r)){CHECK(r.type==po::rec_draw&&r.winIndex==1&&r.key!=0);std::string x,y;poRecordsFormat(r,x,y);CHECK(x.find(";win=1;novel=1;key=")!=std::string::npos);++draws;}
         CHECK(draws==2);} // one line per novel KEY (the three same-key hud draws are one)
        // the mark, then another firing frame: the same keys are counted, not logged again; since-mark counts
        st.mark(t0+3500,1792859468803ull);
        {po::Record r;CHECK(st.pop(r)&&r.type==po::rec_mark&&r.a0==1&&r.winIndex==1&&r.sinceArm==3500&&r.frame==st.cockpitLists);
         std::string a,b;poRecordsFormat(r,a,b);CHECK(a.find("phase-observe;event=phase-observe-mark;mark=1;t=103500;utc-ms=1792859468803;")==0&&a.find("window=1;since-arm-ms=3500")!=std::string::npos);}
        poFrameAt(st,firing,t0+3600);
        {po::Record r;CHECK(st.pop(r)&&r.type==po::rec_summary&&r.novel==4&&r.sinceMark==100&&!st.pop(r));}
        // the window ends by time at the next close: per-key summaries, then the window summary
        poFrameAt(st,quiet,t0+po::window_ms+5);
        {std::vector<po::Record> recs;po::Record r;while(st.pop(r))recs.push_back(r);
         CHECK(recs.size()>=3&&!st.win.active&&st.windowsEnded==1);
         unsigned novelRecs=0,ends=0;for(const auto& x:recs){novelRecs+=x.type==po::rec_novel;ends+=x.type==po::rec_window_end;}
         CHECK(novelRecs==2&&ends==1);
         for(const auto& x:recs) {
             std::string a,b;poRecordsFormat(x,a,b);
             if(x.type==po::rec_novel)CHECK(a.find("phase-observe;event=phase-observe-novel;window=1;key=")==0&&a.find(";first-frame=")!=std::string::npos&&a.find(";frames-seen=2;")!=std::string::npos);
             if(x.type==po::rec_window_end) {
                 CHECK(a.find("phase-observe;event=phase-observe-window-end;window=1/4;reason=time;length-ms=20005;frames=3;logged=3;")==0);
                 CHECK(a.find("novel-draws=8;novel-keys=2;key-overflow=0;novel-lines=2;lines-omitted=0;mark-seen=1;mark-t=103500")!=std::string::npos);
             }
         }
        }
        // fresh tables are real: the legacy logic works again after the window (a never-seen signature is logged as new-signature)
        poDrain(st);
        auto odd=poHashedBase(1);odd.push_back(poHashed(po::kind_other,777,poScene,0x990,0x991));
        poFrameAt(st,odd,t0+po::window_ms+50);
        {po::Record r;CHECK(st.pop(r)&&r.type==po::rec_summary&&r.winIndex==0&&r.reason!=po::reason_window);}
    }
    // ---- caps: per-frame novel draw lines, novel keys, window bytes, 4 windows, refusals
    {
        auto stBox=std::make_unique<po::State>();auto& st=*stBox;
        poFrameAt(st,poHashedBase(1),0);poDrain(st);
        st.arm(0,0);poDrain(st);
        poFrameAt(st,poHashedBase(1),10);poDrain(st);
        const uint64_t late=po::window_learn_ms+10;
        auto many=poHashedBase(1);for(unsigned k=0;k<40;++k)many.push_back(poHashed(po::kind_other,200+k,poScene,0x1000+k,0x2000+k));
        poFrameAt(st,many,late);
        {std::vector<po::Record> recs;po::Record r;while(st.pop(r))recs.push_back(r);
         unsigned draws=0;for(const auto& x:recs)draws+=x.type==po::rec_draw;
         CHECK(recs[0].type==po::rec_summary&&recs[0].novel==40&&draws==po::window_novel_per_frame&&st.win.novelDrawsOmitted==40-po::window_novel_per_frame&&st.win.novelCount==40);}
        // 200 more distinct keys: the table holds 128 keys, the rest are counted
        auto lots=poHashedBase(1);for(unsigned k=0;k<200;++k)lots.push_back(poHashed(po::kind_other,400+k,poScene,0x3000+k,0x4000+k));
        poFrameAt(st,lots,late+16);poDrain(st);
        CHECK(st.win.novelCount==po::window_novel_cap&&st.win.novelKeyOverflow==(40+200)-po::window_novel_cap);
        // bytes: frames until the window closes itself; the charge never exceeds the cap and the file bytes never exceed the charge
        uint64_t summaries=0,maxBytes=0;bool ended=false;std::string endReason;
        for(unsigned i=0;i<2200&&st.win.active;++i) {
            poFrameAt(st,poHashedBase(1),late+32+i);
            po::Record r;while(st.pop(r)){summaries+=r.type==po::rec_summary&&r.winIndex==1;if(r.type==po::rec_window_end){ended=true;endReason=r.why;maxBytes=r.generation;}}
        }
        CHECK(ended&&endReason=="bytes"&&!st.win.active&&maxBytes<=po::window_byte_cap&&summaries>1900&&summaries<2200);
        // 4 windows per process: the 5th and later arms are refused; refusals are logged at most armed_log_cap times
        for(unsigned i=1;i<po::window_cap;++i) {
            st.arm(1000000*i,i);CHECK(st.win.active&&st.win.index==i+1);
            poFrameAt(st,poHashedBase(1),1000000*i+po::window_ms+1);CHECK(!st.win.active); // time ends it
        }
        poDrain(st);
        for(unsigned i=0;i<20;++i)st.arm(9000000+i,i);
        {unsigned armed=0,refused=0;po::Record r;while(st.pop(r)){armed+=r.type==po::rec_armed;refused+=r.type==po::rec_armed&&!r.accepted;
            if(r.type==po::rec_armed){std::string a,b;poRecordsFormat(r,a,b);CHECK(a.find("accepted=0;reason=window-cap;windows-started=4/4")!=std::string::npos);}}
         CHECK(st.windowsStarted==po::window_cap&&!st.win.active&&st.armRefused==20&&refused<=po::armed_log_cap&&armed==refused);}
        for(unsigned i=0;i<100;++i)st.mark(i,i);
        CHECK(st.marks==100&&st.marksLogged==po::mark_cap);
    }
    // ---- worst-case line bounds of the new records
    {
        const uint64_t M=UINT64_MAX;const uint32_t U32=UINT32_MAX;
        po::Record r;r.type=po::rec_summary;r.frame=M;r.list=M;r.generation=M;r.prevSig=M;r.reason=po::reason_window;r.changed=0x3f;r.newSig=true;r.omitted=U32;r.dumpDraws=U32;
        r.winIndex=255;r.t=M;r.sinceArm=INT64_MAX;r.sinceMark=INT64_MAX;r.novel=U32;for(auto& x:r.novelPhase)x=U32;
        auto& s=r.sum;s.sig=M;s.total=s.stored=s.overflow=s.members=s.firstMember=s.lastMember=s.regionEndOrd=s.tonemapOrd=U32;s.sceneRt=s.sceneW=M;s.sceneH=s.sceneFormat=U32;s.sceneKnown=true;
        for(auto& p:s.p){p.draws=p.indexed=p.onScene=p.unknownRt=p.offVp=p.newPso=U32;p.idx=p.vtx=p.inst=p.shape=M;}
        char a[po::line_bytes],b[po::line_bytes];po::format(r,a,b);
        if(std::getenv("ACZ_PH_DUMP"))std::fprintf(stderr,"worst window summary a=%zu b=%zu\n",std::strlen(a),std::strlen(b));
        CHECK(std::strlen(a)<po::line_bytes&&std::strlen(b)<po::line_bytes);
        r.type=po::rec_armed;r.accepted=true;r.a0=U32;r.a1=U32;r.a2=U32;r.a3=U32;r.a4=1;r.b0=M;r.utc=M;po::format(r,a,b);CHECK(std::strlen(a)<po::line_bytes&&!std::strstr(a,"truncated=1"));
        r.accepted=false;r.why="window-active";po::format(r,a,b);CHECK(std::strlen(a)<po::line_bytes&&!std::strstr(a,"truncated=1"));
        r.type=po::rec_mark;po::format(r,a,b);CHECK(std::strlen(a)<po::line_bytes&&!std::strstr(a,"truncated=1"));
        r.type=po::rec_window_end;r.why="bytes";r.b1=M;r.b2=M;r.b3=M;po::format(r,a,b);CHECK(std::strlen(a)<po::line_bytes&&!std::strstr(a,"truncated=1"));
        r.type=po::rec_novel;auto& d=r.draw;d.vs8=d.ps8=d.count=d.instances=U32;d.rtWidth=M;d.rtHeight=d.rtFormat=U32;d.blendSrc=d.blendDst=d.blendOp=d.depthFunc=d.frontFunc=U32;
        po::format(r,a,b);CHECK(std::strlen(a)<po::line_bytes&&!std::strstr(a,"truncated=1")&&std::strlen(b)<po::line_bytes&&!std::strstr(b,"truncated=1"));
        r.type=po::rec_draw;r.key=M;po::format(r,a,b);CHECK(std::strlen(a)<po::line_bytes&&std::strlen(b)<po::line_bytes); // extreme values may truncate (marked), never overrun
    }
}
// Regression (png-scenario flake): a recycled process id must never inherit an earlier process's scratch PNGs.
static void scratch_dir_unique() {
    wchar_t tmp[MAX_PATH]{};CHECK(GetTempPathW(MAX_PATH,tmp)>0);
    // The legacy fixed name of this very process id, holding a stale PNG exactly as a dead process with a recycled id left it.
    const std::wstring legacy=std::wstring(tmp)+L"acz-step3-test-"+std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(legacy.c_str(),nullptr);CreateDirectoryW((legacy+L"\\png").c_str(),nullptr);
    {HANDLE h=CreateFileW((legacy+L"\\png\\acz-vr-png-1-1-s1-world.png").c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);CHECK(h!=INVALID_HANDLE_VALUE);CloseHandle(h);}
    const std::wstring a=makeScratchDir(L"acz-step3-test"),b=makeScratchDir(L"acz-step3-test");
    CHECK(a!=b&&a!=legacy&&b!=legacy);
    for(const auto& d:{a,b}) {
        CHECK(GetFileAttributesW(d.c_str())!=INVALID_FILE_ATTRIBUTES);
        WIN32_FIND_DATAW fd;HANDLE h=FindFirstFileW((d+L"\\*").c_str(),&fd);CHECK(h!=INVALID_HANDLE_VALUE);
        unsigned entries=0;do if(wcscmp(fd.cFileName,L".")&&wcscmp(fd.cFileName,L".."))++entries;while(FindNextFileW(h,&fd));FindClose(h);
        CHECK(entries==0); // fresh and empty
    }
    {std::error_code ec;std::filesystem::remove_all(std::filesystem::path(legacy),ec);CHECK(!ec);}
    CHECK(scratchDirs().size()==2);
}
int main(int argc,char** argv) {
    std::set_terminate(traceTerminate);
    static_assert(sizeof(acz::Step1Submit)<=16,"Execute-root local stays tiny");
    static_assert(sizeof(acz::CopyNativeCall)<=32,"Wrapper guard stays tiny");
    struct Case {const char* name;void(*run)();};
    const Case cases[]={{"scratch-dir-unique",scratch_dir_unique},{"off",off},{"success",success},{"refusals",refusals_then_insert},{"no-qualified",no_qualified},
        {"prepare-fail",prepare_fail},{"close-fail",close_fail},{"duplicate",duplicate},{"reexecute",reexecute},
        {"uint64max",uint64max},{"signal-fail",signal_fail},{"mismatch",mismatch},{"closure-timeout",closure_timeout},
        {"no-execute",no_execute},{"reset-before-complete",reset_before_complete},{"poll-timeout",poll_timeout},
        {"cross-queue",cross_queue},{"off-list1",off_list1},{"list1-witnessed",list1_witnessed},
        {"list1-distinct",list1_distinct},{"prehook-discovery",prehook_discovery},{"release-recreate",release_recreate},
        {"early-off",early_off},{"early-device",early_device},{"early-repeat",early_repeat},
        {"early-mismatch",early_mismatch},{"early-distinct",early_distinct},
        {"later-generations",later_generations},{"later-generation-queries",later_generation_queries},
        {"later-reexecute-same",later_reexecute_same},{"later-reset-failed",later_reset_failed},
        {"later-reset-in-join",later_reset_in_join},{"later-unclosed-execute",later_unclosed_execute},
        {"later-released-before-reset",later_released_before_reset},{"later-execute-open",later_execute_open},{"early-binding",early_binding},
        {"step2-success",step2_success},{"step2-default-heap",step2_default_heap},{"step2-shape-changed",step2_shape_changed},
        {"step2-over-cap",step2_over_cap},{"step2-cpu-mismatch",step2_cpu_mismatch},{"step2-no-cpu",step2_no_cpu},
        {"step2-all-refused",step2_all_refused},{"step2-index-range",step2_index_range},{"step2-signal-fail",step2_signal_fail},
        {"step2-duplicate",step2_duplicate},{"step2-no-qualified",step2_no_qualified},{"step2-rules",step2_rules},
        {"step3-success",step3_success},{"step3-msaa",step3_msaa},{"step3-om-unknown",step3_om_unknown},{"step3-om-gap",step3_om_gap},
        {"step3-over-cap",step3_over_cap},{"step3-srv-unknown",step3_srv_unknown},{"step3-format",step3_format},
        {"step3-depth-equal",step3_depth_equal},{"step3-mismatch",step3_mismatch},{"step3-no-snapshot",step3_no_snapshot},
        {"step3-create-fail",step3_create_fail},{"step3-signal-fail",step3_signal_fail},{"step3-off",step3_off},
        {"step3-off-step2",step3_off_step2},{"step3-rules",step3_rules},{"step3-float",step3_float},
        {"step3-crop-clip",step3_crop_clip},{"step3-crop-offscreen",step3_crop_offscreen},{"step3-crop-cap",step3_crop_cap},
        {"step3-arm-timeout",step3_arm_timeout},{"step3-gs-rejected",step3_gs_rejected},{"step3-select",step3_select},
        {"step3-no-visible",step3_no_visible},{"step3-ring-srt",step3_ring_srt},{"step3-live-unreadable",step3_live_unreadable},
        {"step3-ring-overflow",step3_ring_overflow},{"step3-set",step3_set},{"step3-set-partial",step3_set_partial},
        {"step3-set-unreadable",step3_set_unreadable},{"step3-set-rules",step3_set_rules},
        {"step5-hud-observe",step5_hud_observe},{"step5-hud-off",step5_hud_off},{"step5-hud",step5_hud},{"step5-hud-refused",step5_hud_refused},{"step5-hud-rs-skip",step5_hud_rs_skip},
        {"step5-hud-cap",step5_hud_cap},{"step5-hud-region",step5_hud_region},{"step5-hud-faults",step5_hud_faults},{"step5-hud-rules",step5_hud_rules},{"step5-hud-radar-rules",step5_hud_radar_rules},{"step5-hud-radar-off",step5_hud_radar_off},{"step5-hud-radar-region",step5_hud_radar_region},{"step5-hud-radar",step5_hud_radar},{"step5-hud-radar-caps",step5_hud_radar_caps},{"step5-png-scene-end-radar",step5_png_scene_end_radar},
        {"step5-png-rules",step5_png_rules},{"step5-png-off",step5_png_off},{"step5-png",step5_png},{"step5-png-faults",step5_png_faults},{"step5-png-refused",step5_png_refused},{"step5-png-fx-rules",step5_png_fx_rules},{"step5-png-effects",step5_png_effects},{"step5-png-effects-threshold",step5_png_effects_threshold},{"step5-png-effects-invalid-threshold",step5_png_effects_invalid_threshold},{"step5-png-effects-off",step5_png_effects_off},{"step5-png-burst-file",step5_png_burst_file},{"step5-png-burst-file-stale",step5_png_burst_file_stale},{"step5-png-scene-end-barrier",step5_png_scene_end_barrier},{"step5-png-scene-end-declined",step5_png_scene_end_declined},{"step5-png-scene-end-absent",step5_png_scene_end_absent},{"step5-effects-alpha-only",step5_effects_alpha_only},{"step5-continuous",step5_continuous},{"step5-discard",step5_discard},{"step5-ambiguity",step5_ambiguity},
        {"step5-pose",step5_pose},{"step5-threads",step5_threads},{"step5-rules",step5_rules},
        {"step5-varying",step5_varying},{"step5-skip",step5_skip},{"step5-world",step5_world},{"step5-world-changed",step5_world_changed},{"step5-world-simultaneous",step5_world_simultaneous},{"step5-float-depth",step5_float_depth},{"step5-upload-retire",step5_upload_retire},{"step5-pose-drop",step5_pose_drop},
        {"step5-look-drive",step5_look_drive},{"step5-look-sweep",step5_look_sweep},
        {"step5-source",step5_source},{"step5-producer-dead",step5_producer_dead},{"step5-om-descriptor",step5_om_descriptor},{"step5-world-barriers",step5_world_barriers},{"step5-fence-epoch",step5_fence_epoch},
        {"step5-alt-admit",step5_alt_admit},{"step5-owner-aba",step5_owner_aba},{"step5-owner-batch",step5_owner_batch},{"step5-owner-pin",step5_owner_pin},{"step5-owner-held",step5_owner_held},{"step5-acquire-quarantine-race",step5_acquire_quarantine_race},{"step5-untracked-before-acquire",step5_untracked_before_acquire},{"step5-unexamined-phase",step5_unexamined_phase},{"step5-owner-table-full",step5_owner_table_full},{"step5-fence-rollover-held",step5_fence_rollover_held},{"step5-world-entry",step5_world_entry},
        {"step5-om-interleave",step5_om_interleave},{"step5-world-reselect-identity",step5_world_reselect_identity},{"step5-world-reselect-uncovered",step5_world_reselect_uncovered},
        {"step5-world-foreign-queue",step5_world_foreign_queue},{"step5-world-reexecute",step5_world_reexecute},
        {"step5-world-texture-release-reentry",step5_world_texture_release_reentry},{"step5-world-texture-retirement",step5_world_texture_retirement},{"step5-world-texture-uncovered",step5_world_texture_uncovered},
        {"step5-world-preinject",step5_world_preinject},{"step5-neutral-generation",step5_neutral_generation},{"step5-neutral-export",step5_neutral_export},
        {"step5-world-array",step5_world_array},{"feature-log",feature_log_unit},{"step5-feature-log",step5_feature_log},{"step5-consumer-rev5",step5_consumer_rev5},{"step3-eval-rules",step3_eval_rules},
        {"step5-cockpit-keys",step5_cockpit_keys},{"step5-hmd-fov-rules",step5_hmd_fov_rules},{"step5-hmd-fov",step5_hmd_fov},
        {"step5-precockpit-observe",step5_precockpit_observe},{"step5-members-by-class",step5_members_by_class},
        {"step5-hud-markers-rules",step5_hud_markers_rules},{"step5-hud-markers-off",step5_hud_markers_off},{"step5-hud-markers",step5_hud_markers},
        {"step5-hud-markers-refused",step5_hud_markers_refused},{"step5-hud-markers-region",step5_hud_markers_region},
        {"step5-hud-markers-create-refused",step5_hud_markers_create_refused},{"step5-tac",step5_tac},
        {"step5-effects-rules",step5_effects_rules},{"step5-world-scene-identity",step5_world_scene_identity},{"step5-effects-smoke-rules",step5_effects_smoke_rules},{"step5-effects-gamecloud-rules",step5_effects_gamecloud_rules},{"step5-effects-gamecloud",step5_effects_gamecloud},{"step5-effects-smoke",step5_effects_smoke},{"step5-effects-smoke-capture",step5_effects_smoke_capture},{"step5-effects-smoke-gpu",step5_effects_smoke_gpu},{"step5-effects-smoke-gpu-late",step5_effects_smoke_gpu_late},{"step5-effects-sprite-gpu",step5_effects_sprite_gpu},{"step5-eye-ring",step5_eye_ring},{"step5-eye-ring-eyeobj-off",step5_eye_ring_eyeobj_off},{"step5-eye-ring-no-extent",step5_eye_ring_no_extent},{"step5-eye-ring-fx",step5_eye_ring_fx},{"step5-effects-off",step5_effects_off},{"step5-effects",step5_effects},
        {"step5-effects-refused",step5_effects_refused},{"step5-effects-dual-rules",step5_effects_dual_rules},{"step5-effects-dual",step5_effects_dual},{"step5-effects-dual-refused",step5_effects_dual_refused},{"step5-effects-additive-off",step5_effects_additive_off},{"step5-effects-additive-on",step5_effects_additive_on},{"step5-effects-dual-clone-missing",step5_effects_dual_clone_missing},{"step5-effects-dual-mixed",step5_effects_dual_mixed},{"step5-effects-create-refused",step5_effects_create_refused},{"step5-effects-pred-refused",step5_effects_pred_refused},
        {"step5-world-hud",step5_world_hud},{"step5-world-keys",step5_world_keys},{"step5-world-rules",step5_world_rules},{"step5-world-objects",step5_world_objects},{"step5-world-vs-binding",step5_world_vs_binding},
        {"step5-flags-by-class",step5_flags_by_class},{"step5-flags-world",step5_flags_world},{"step5-flags-hud",step5_flags_hud},
        {"step5-world-observe-rules",step5_world_observe_rules},{"step5-world-observe",step5_world_observe},{"step5-world-observe-carrier",step5_world_observe_carrier},{"step5-world-early",step5_world_early},{"step5-world-early-look",step5_world_early_look},{"step5-world-evidence-rules",step5_world_evidence_rules},{"step5-world-evidence",step5_world_evidence},{"step5-world-evidence-early",step5_world_evidence_early},
        {"step5-phase-observe-rules",step5_phase_observe_rules},{"step5-phase-observe",step5_phase_observe},{"step5-phase-observe-window",step5_phase_observe_window},
        {"step5-world-observe-root",step5_world_observe_root},{"step5-world-stored",step5_world_stored}};
    if(argc!=2){std::fprintf(stderr,"usage: gpu_copy_step1_offline_tests <scenario>\n");return 2;}
    for(const auto& c:cases)if(std::string_view(argv[1])==c.name) {
        c.run();
        if(std::getenv("ACZ_STEP1_DUMP"))dump();
        std::printf("gpu_copy_step1 %s: synthetic fakes only; no native GPU evidence\n",c.name);
        return 0;
    }
    std::fprintf(stderr,"unknown scenario %s\n",argv[1]);return 2;
}
