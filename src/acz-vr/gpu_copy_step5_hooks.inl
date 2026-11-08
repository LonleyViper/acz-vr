// J-3b8/J-3b9: GPU-copy "step 5", the CONTINUOUS stereo producer (D-230/D-236
// FrameSource). Included by gpu_copy_step1_hooks.inl after the step-3 adapter,
// inside d3d12_hook.cpp's anonymous namespace. Default OFF: only when
// gpu_copy_step5_continuous plus the step-3 and whole-set opt-ins are latched.
//
// Reuses step 3: the owner, the device/queue witnesses, the candidate selection
// and set building (whole-set), step3Capture / decide, the inline live SrtData
// read (step3InlineSrt) and the row-3 IPD delta (fallback). It does NOT use the
// one-shot ledger/witness transaction: every frame is its own small transaction.
//
// J-3b9 member matching (replaces the J-3b8 exact first-member shape match that
// skipped ~16% of live frames as "shape-mismatch"):
//   * The cockpit ordinal range is [first, last] selected member ordinal.
//   * A frame STARTS at the first DrawCPT draw of a qualified list generation
//     whose ordinal is in that range and whose required state holds
//     (step5MemberWhy: decide() + ring RT/DS format, viewport size, depth clear,
//     t10 root parameter, stencil state). PSO identity and draw arguments are
//     NOT part of the match: the replay re-issues the game's own Draw call with
//     the LIVE arguments under the game's bound PSO.
//   * Every later DrawCPT draw of that list generation (ordinal increasing, up to
//     64 members / 600 commands) whose required state holds is a member: the
//     member count may change per frame. A member whose state does not hold is
//     refused alone (counted, the frame is marked partial) and the frame goes on.
//   * Differences from the selected set (PSO, args, extra ordinal, member count)
//     are counted as mismatch-produced and described in rate-limited
//     `continuous-mismatch` lines (first one immediately, then <= 1 per 5 s).
//
// J-cockpit-fov (src/acz-vr/J-cockpit-fov-findings.md), three default-OFF keys:
//   * gpu_copy_step5_hmd_fov: the pose path builds each eye with its own XR FOV
//     (StereoFrame::cockpit_fov), the reference stays on the game lens.
//   * gpu_copy_step5_precockpit_observe: read-only `precockpit;` lines for the DrawCPT
//     draws before the first selected member (sampled, bounded, dedicated log).
//   * gpu_copy_step5_members_by_class: the frame starts at the first DrawCPT draw of an
//     admitted class (any ordinal) instead of the selected ordinal range; raised caps.
//
// Frame accounting (`continuous-definitions` line):
//   produced = published = recorded, executed once, private fence signalled.
//   skipped  = a list generation that had at least one DrawCPT draw in the cockpit
//              ordinal range but no frame was recorded for it, counted once at its
//              Close, with the reason of its last refused start: mismatch (state
//              refused), unqualified, unreadable (live SrtData) or no-slot.
//   discarded / ambiguous: a recorded frame that was Reset unsubmitted / quarantined.
//
// Per frame (game thread):
//   first member: list qualified (copy-observation witness: authenticated, open
//     generation, no predication/query/render pass/bundle), timeline tokens
//     bound; a free ring slot taken (try-only); camera + lens recovered from the
//     live bytes, poses read (try-only), per-frame matrices set up once; the
//     WORLD SNAPSHOT (below) if every rule holds; then the clears and the
//     member's 8 commands. Later members: 8 commands each.
//   Close: closed. Reset before submission: recording discarded -> slot freed.
//   ExecuteCommandLists exactly once on the bound queue -> after the native call
//     ONE private-fence Signal with the next value -> PUBLISHED. Duplicate,
//     cross-queue, failed Close, failed Signal, re-execution of a published
//     recording before its Reset, or not submitted within 2 s: ambiguous -> the
//     slot is quarantined (never published/reused); 3 consecutive ambiguous
//     frames disable the producer.
//
// J-3b9 WORLD SNAPSHOT (the one place a GAME resource is transitioned): at the
// first member's seam, before any replay command, the game's scene render
// target is copied into the slot's private world texture:
//   Barrier(game RT 0: RENDER_TARGET -> COPY_SOURCE), Barrier(world: RENDER_TARGET
//   -> COPY_DEST), CopyTextureRegion(world sub 0 <- game sub 0), Barrier(game:
//   COPY_SOURCE -> RENDER_TARGET), Barrier(world: COPY_DEST -> RENDER_TARGET).
// D3D12 rule: a subresource bound as a render target view must be in the
// RENDER_TARGET state when the GPU executes draws using it; the game's own draw
// follows immediately with this RTV bound, so at this point of the list the
// subresource is RENDER_TARGET, and the restore barrier returns it there before
// the game's draw. Required (else world_valid=false, the eyes are still
// published): the proxy's view tracking resolves the bound RTV 0 (exactly one
// RTV bound) to the SAME resource pointer/size/format observed at selection; the
// view names subresource 0 (null desc or TEXTURE2D mip 0 / plane 0); TEXTURE2D,
// single sample, no ALLOW_SIMULTANEOUS_ACCESS (whose resources may be used in
// several states at once); qualified list (no render pass, no bundle).
// Residual, not provable offline: the game using enhanced barriers (layouts) on
// this resource in the same list; the snapshot holds everything drawn before
// the first cockpit member (sky, terrain, world), not the HUD drawn later.
//
// Resources (ring) are created in the owner context and kept for the process
// lifetime (no release while anything could be in flight; teardown is vetoed).
// Slot metadata uses a small mutex; the game thread only ever try_locks it
// (a missed publication is applied by the next game-thread attempt or the worker).
#include "gpu_copy_step5.h"
#include "gpu_copy_png.h"          // J-hud-colour (d3d12_hook.cpp includes it before the anonymous namespace)
#include "stereo_pose_provider.h"
#include "object_interp.h"     // OBJECT-INTERP (interp:1, default OFF)
#include "fx_smoke.h"          // EFFECTS-OUTSIDE (C) fxmem:1 (already included by gpu_copy_step5_world.h before the anonymous namespace)
#include "fx_diag_flare_air.h"  // READ-ONLY flare / aircraft diagnostics (fxsig:f1a4e000 / fxsig:ac0f1a60)
#include "fx_census.h"         // EFFECTS-OUTSIDE (D) fxmem:3 (already included by gpu_copy_step5_world.h before the anonymous namespace)
#include "eye_ring.h"          // EYE-RING (already included by d3d12_hook.cpp before the anonymous namespace: ONE channel shared with the proxy / Output)
std::atomic<acz::stereo::PoseProvider*> g_step5Pose{nullptr};
// Consumer rev 5: the trust anchor for its GPU queue Wait. Called whenever the producer adopts
// a private fence (epoch == StereoFrame::fence_epoch); nullptr clears.
#ifdef ACZ_VR_OFFLINE_TEST
void* g_step5RegisteredFence=nullptr;uint64_t g_step5RegisteredEpoch=0;unsigned g_step5Registrations=0;
inline void step5RegisterFence(void* fence,uint64_t epoch) noexcept {g_step5RegisteredFence=fence;g_step5RegisteredEpoch=epoch;++g_step5Registrations;}
// DECOUPLE review 3 fixture seam: called by try_acquire_latest AFTER it selected a candidate and BEFORE it acquires it (meta held).
void (*g_step5AcquireSeamTest)(unsigned slot)=nullptr;
#else
inline void step5RegisterFence(void* fence,uint64_t epoch) noexcept {acz::xr_stereo::register_producer_fence(fence,epoch);}
#endif
struct Step5Slot {
    std::atomic<int> state{gpu_copy::step5::slot_free}; // written under `meta`, except the try-failed quarantine store (atomic, no tearing)
    uint64_t frameId=0,fenceValue=0,predicted=0;
    uint64_t epoch=0;   // fence epoch the published value belongs to (issue-29 consumer rev 4)
    uint64_t lastUseEpoch=0; // J-3b9c (review 2, P2): the fence epoch lastUse was signalled on
    uint64_t lastUse=0; // J-3b9 (review P1): private-fence value of the last SUBMITTED recording into this slot (step-1 mutex)
    // DECOUPLE review 3 (Astra P2): Step5State::untrackedExec when this slot's frame was SUBMITTED (the write that produced the published images).
    // Bound to the publication, never reset by the acquisition: an unexamined Execute between publication and acquisition stays visible.
    uint64_t pubUntracked=0;
    acz::stereo::EyePose pose[2]{};bool poseValid=false,partial=false,worldValid=false,lensValid=false;uint32_t members=0;float lens[4]{};
    bool worldIncomplete=false;uint32_t worldLost=0; // J-180: published with the frame (StereoFrame::world_incomplete / world_lost)
    uint32_t spaceGeneration=0; // consumer rev 5: pose snapshot space generation + 1 (0 = unknown)
    ID3D12Resource* rt[2]{};ID3D12Resource* ds[2]{};ID3D12DescriptorHeap* rtvHeap[2]{};ID3D12DescriptorHeap* dsvHeap[2]{};
    ID3D12Resource* world=nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2]{},dsv[2]{};uint64_t uploadOffset=0;
    // J-hud-capture (gpu_copy_step5_hud): private HUD target (+ depth-stencil) of this slot and the
    // published facts of the frame that last rendered into it.
    ID3D12Resource* hudRt=nullptr;ID3D12Resource* hudDs=nullptr;ID3D12DescriptorHeap* hudRtvHeap=nullptr;ID3D12DescriptorHeap* hudDsvHeap=nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE hudRtv{},hudDsv{};bool hudValid=false;uint32_t hudDraws=0,hudRefused=0;
    // J-cockpit-fov F1: the FOV the eyes of the published frame were rendered with (valid only then).
    bool cockpitFovValid=false;float cockpitFov[2][4]{};
    // J-quad-yaw: the neutral the published frame's eyes were recentred with (valid only with poseValid).
    bool neutralValid=false;float neutralPos[3]{};float neutralYaw=0.0f;
    // xr_look_drive_keys: the look the published frame was rendered with (valid only with the key on and a pose).
    bool lookValid=false;float lookYaw=0.0f,lookPitch=0.0f;
    // Timing-anchor diagnostics (StereoFrame::diag_*; diagnostic only, never read by a decision).
    bool diagLookMeasValid=false;uint8_t diagStart=0,diagLookSrc=0;float diagLookMeas[2]{},diagLookEst[2]{};
    uint64_t diagPoseSeq=0,diagGeneration=0;const char* diagRefusal=nullptr;int64_t diagStartUs=0,diagPublishUs=0;
    // J-hud-markers (gpu_copy_step5_hud_markers): second private HUD colour target of this slot (the HUD depth-stencil is
    // shared) and the published facts of the frame that last rendered marker draws into it.
    ID3D12Resource* hudMarkerRt=nullptr;ID3D12DescriptorHeap* hudMarkerRtvHeap=nullptr;D3D12_CPU_DESCRIPTOR_HANDLE hudMarkerRtv{};
    bool hudMarkersValid=false;uint32_t hudMarkerDraws=0,hudMarkerRefused=0;
    // J-effects-layer (gpu_copy_step5_effects): third private colour target of this slot (the HUD depth-stencil is shared) and the
    // published facts of the frame that last rendered into it.
    ID3D12Resource* effRt=nullptr;ID3D12DescriptorHeap* effRtvHeap=nullptr;D3D12_CPU_DESCRIPTOR_HANDLE effRtv{};
    bool effValid=false;uint32_t effDraws=0,effRefused=0,effTail=0;
    // WORLD-1: the game camera / frame-synchronous E6 sample of the published frame (StereoFrame::game_cam_*, game_eye*, e6_*).
    bool gameCamValid=false,gameEyeValid=false,e6Valid=false;float gameCam[9]{},gameEye[3]{},e6[7]{};uint64_t e6Seq=0;
    int64_t e6TNs=0,e6TReadNs=0;uint32_t e6Lock=0,e6Stage=0;uint64_t e6Tag=0; // CAM-EPOCH: the frame sample's read-start/publication time, epochs and recording-generation tag
    bool noCockpit=false; // POV-RENDER stage 2 (pov:2): a published chase frame without cockpit members (StereoFrame::no_cockpit)
    bool tacValid=false; // TACTICAL (tac:1): Step5State::tacSlot[this slot] holds the published frame's facts (key off: false)
    acz::interp::Facts interp{}; // OBJECT-INTERP (interp:1): StereoFrame::interp of the published frame (key off: invalid)
    // EFFECTS-OUTSIDE (C) fxmem:4 (fx_smoke_gpu.h): this slot's per-eye smoke images (owner context, process lifetime) and the published frame's facts.
    ID3D12Resource* smokeRt[2]{};ID3D12DescriptorHeap* smokeRtvHeap[2]{};D3D12_CPU_DESCRIPTOR_HANDLE smokeRtv[2]{};bool smokeValid=false;uint32_t smokeDraws=0,smokeRefused=0;
    // EFFECTS-OUTSIDE (F) fxmem:6 (fx_sprite_gpu.h): this slot's per-eye SPRITE images (explosions / GREATER sprites; owner context, process lifetime) and the facts.
    ID3D12Resource* spriteRt[2]{};ID3D12DescriptorHeap* spriteRtvHeap[2]{};D3D12_CPU_DESCRIPTOR_HANDLE spriteRtv[2]{};bool spriteValid=false;uint32_t spriteDraws=0,spriteRefused=0;
};
// J-effects-layer: per-frame bookkeeping of the speculative between-members replay (step-1 mutex).
// J-effects-smoke-fix: one speculative replay as the diagnostics see it (first effects_diag_per_frame of a frame). `committed` is set when its epoch commits,
// `word` is the predicate word read back from the (CPU-mapped) predicate memory at the list's Close = exactly what the GPU will read.
constexpr unsigned effects_diag_per_frame=8;
struct Step5EffDiag {
    int epoch=-1;uint32_t ord=0,vs8=0,ps8=0,depthFunc=0,count=0,instances=0;uint8_t blend[6]{};bool indexed=false,committed=false;uint64_t offset=0,word=0;
};
// The shader/blend identity of the draw being considered (read under the hook-state lock at the Draw seam).
struct Step5EffId {uint32_t vs8=0,ps8=0;uint8_t blend[6]{};bool root=false,rootUnknown=false;ID3D12PipelineState* game=nullptr,*clone=nullptr,*light=nullptr;}; // J-effects-dual: bound PSO + its private clone; (I) light = the explosion's per-eye PSO
struct Step5Eff {
    bool armed=false,cleared=false,dsCleared=false,sceneClosed=false;
    unsigned commands=0,ignored=0,notScene=0,predRefused=0;const char* firstWhy=nullptr;
    gpu_copy::step5::EffectsEpochs ep{};
    unsigned diagN=0,diagDropped=0;Step5EffDiag diag[effects_diag_per_frame]{};
    acz::fxsmoke::gpu::FrameState sm{};unsigned smFallback=0; // EFFECTS-OUTSIDE (C) fxmem:4: this frame's per-eye smoke replays (off unless latched at arm)
    acz::fxsprite::gpu::FrameState sp{};unsigned spFallback=0,spExplosion=0,spSprite=0,spLight=0;const char* spXpsoWhy=nullptr;unsigned spAdditive=0,spAddPoints=0; // (P) per-eye additive / point replays // (I) spLight / spXpsoWhy: explosion PSO choice; EFFECTS-OUTSIDE (F) fxmem:6: per-eye sprite replays (off unless latched at arm)
    uint64_t trBytes=0; // (N) bytes of the slot's TRAIL region (behind the game's span: fx_sprite_gpu.h trail_take) the frame's puff-memory hosts took; never part of spBytes
    uint64_t spBytes=0; // (H) the frame's packed sprite-span cursor: bytes of this slot's sprite UPLOAD span the frame's replays took (fx_sprite_gpu.h replay_bytes)
    unsigned acCaps=0;uint64_t acBytes=0; // (M6) fxatlas: capture quads recorded this frame / bytes of the slot's capture region they took (fx_puff_atlas.h)
};
struct Step5EffDiagLine {uint64_t frame=0;bool burst=false;unsigned mode=0;Step5EffDiag d{};};
// J-hud-capture: per-frame HUD region bookkeeping (step-1 mutex).
struct Step5Hud {
    bool armed=false,cleared=false,capped=false;const char* armWhy=nullptr;uintptr_t scene=0;
    unsigned draws=0,refused=0,commands=0,resets=0,depthTested=0,noTarget=0;unsigned reason[gpu_copy::step5::hud_categories]{};
    const char* firstWhy=nullptr;const char* endWhy=nullptr; // end: rt-change, draw-on-other-target, scene-barrier, or close
    // J-hud-markers: armed per frame (mSplit) when the second target exists. Marker draws are counted HERE and not in
    // draws/refused/reason (those stay the instrument layer's); `commands` counts both. dsCleared: the shared HUD
    // depth-stencil was cleared this frame (split only; key off uses `cleared` exactly as before).
    bool mSplit=false,mCleared=false,dsCleared=false;
    unsigned mDraws=0,mRefused=0,mBoxes=0,mLabels=0;unsigned mReason[gpu_copy::step5::hud_categories]{};
    const char* mFirstWhy=nullptr;gpu_copy::step5::HudMarkerState mState{};
    // J-hud-radar (gpu_copy_step5_hud_radar): armed per frame; rd = the stencil proof and counters, dg = the hud-end diagnostic. Untouched with the key off.
    bool radar=false;gpu_copy::step5::HudRadar rd{};gpu_copy::step5::HudRadarDiag dg{};
    // HUD-RS-SKIP: colour SUBTRACT/REV_SUBTRACT draws skipped this region (never in draws/refused); rsDsw = how many of them write depth or stencil.
    unsigned rsSkipped=0,rsDsw=0;bool depthGap=false;
    gpu_copy::step5::HudLateFrame late{}; // HUD-LATE (hudlate:1; hud_late.h): the post-tone-map HUD text of this frame (untouched with the key off)
};
// J-hud-capture: where the HUD region of the active frame ended. Written by the OM/barrier hooks
// (hook-state mutex held) and the Draw seams (step-1 mutex held); a LEAF lock that is never held
// while acquiring another lock or calling native code.
struct Step5HudRegion {bool active=false,ended=false;Step1Route list=0;uint64_t generation=0;uintptr_t scene=0;const char* why=nullptr;
    // J-scene-end: the StateBefore of the scene-target barrier that ended the region; `sceneEndTaken` : a scene-end copy was recorded for THIS end
    // (cleared when the region reopens, so a later end copies again over the same footprint).
    uint32_t endBefore=0;bool sceneEndTaken=false;
    // J-hud-radar (radar = the frame is armed with gpu_copy_step5_hud_radar): a draw on / bind of another target is an excursion (away) and not the end.
    // excWhy: the cause of the FIRST excursion when it was a hooked bind ("rt-change"): the scene-end copy fires there, exactly where the original region
    // ended; excCopy: that copy was taken, so a later scene-barrier end of the same region does not copy again.
    bool radar=false,away=false,window=false,excCopy=false;unsigned excursions=0,returns=0;const char* firstAwayWhy=nullptr;const char* excWhy=nullptr;
    // RADAR-SCENE-RESUME (radar key only): sceneLeftAway = the region ended at a scene barrier while away (resumable by the barrier back);
    // resumed = it was resumed at least once (sticky until a later member: the effects layer stays closed as after the original end).
    bool sceneLeftAway=false,resumed=false;unsigned sceneLeaves=0,resumes=0,sceneBackRefused=0;};
// What the Draw seam needs to know about the segmented region (a copy, taken under the leaf lock).
struct Step5RadarView {bool radar=false,away=false,late=false,window=false,resumed=false;unsigned excursions=0,returns=0,sceneLeaves=0,resumes=0,sceneBackRefused=0;
    const char* firstWhy=nullptr;};
std::mutex g_hudRegionMutex;Step5HudRegion g_hudRegion;
// What differed from the selected set (first difference of a frame).
struct Step5Detail {
    bool set=false,produced=false,selFound=false,selIndexed=false,liveIndexed=false;const char* what=nullptr;
    uint64_t ordinal=0,psoSel=0,psoLive=0,list=0,gen=0;uint32_t selArgs[5]{},liveArgs[5]{};unsigned frameMembers=0,selected=0;
    uint64_t root=0;uint32_t topology=0,ib=0,vb=0,t10=0; // the live member's signature
};
struct Step5Frame {
    bool active=false,closed=false,closeOk=false,submitted=false,stopped=false,partial=false,poseValid=false,diff=false;
    bool worldValid=false,lensValid=false,poseBroken=false;const char* worldEvidence=nullptr;
    // J-180 (Codex review P2): an eligible scene-target DrawACM_v draw of this frame's list was NOT replayed (world-cap, unreadable record, record /
    // camera / pose / cb10 refusal, or lost before the early opener). Published as StereoFrame::world_incomplete; the cockpit still publishes.
    bool worldIncomplete=false;unsigned wobjLost=0;
    bool lookValid=false;float lookYaw=0.0f,lookPitch=0.0f; // xr_look_drive_keys: the rendered look compensated in this frame
    // Timing-anchor diagnostics (StereoFrame::diag_*): look candidates, pose snapshot sequence, steady_clock microseconds at the frame start.
    bool diagLookMeasValid=false;uint8_t diagLookSrc=0;float diagLookMeas[2]{},diagLookEst[2]{};uint64_t diagPoseSeq=0;int64_t diagStartUs=0;
    unsigned slot=0,recorded=0,refusedMembers=0,commands=0,ticks=0;Step1Route list=0;uint64_t generation=0,lastOrdinal=0;
    acz::stereo::EyePose pose[2]{};uint64_t predicted=0;uint32_t spaceGeneration=0;const char* stop=nullptr;const char* worldWhy=nullptr;float lens[4]{};
    Step5Detail detail{};
    Step5Hud hud{};
    Step5Eff eff{}; // J-effects-layer (zero/unarmed with the key off)
    // J-cockpit-fov: F1 the frame's eye FOV (fovHmd only with poseValid); F3 by-class membership.
    bool fovHmd=false,fovClamped=false,byClass=false,capped=false;const char* fovWhy=nullptr;float fov[2][4]{};
    unsigned wobjRecorded=0,wobjRefused=0; // J-world-objects: DrawACM_v members recorded / refused in this frame
    // J-world-objects v2: `early` = the frame was OPENED by a scene-RT DrawACM_v draw of its list (slot, pose, clears, world camera);
    // `adopted` = the list's first accepted DrawCPT member continued it (cockpit camera verified, world snapshot, HUD armed).
    bool early=false,adopted=false;
    // POV-RENDER stage 2 (pov:2): an early frame opened as a NO-COCKPIT chase frame (published unadopted; a DrawCPT draw poisons it).
    bool noCockpit=false;
    gpu_copy::step5::NeutralExport neutral{}; // J-quad-yaw: valid only with poseValid
    // WORLD-1: game camera axes of this frame (cockpit camera: right, up, fwd), the DrawACM_v absolute camera eye, the E6 sample read at the first member.
    bool gameCamValid=false,gameEyeValid=false,e6Valid=false;float gameCam[9]{},gameEye[3]{},e6[7]{};uint64_t e6Seq=0;
    int64_t e6TNs=0,e6TReadNs=0;uint32_t e6Lock=0,e6Stage=0;uint64_t e6Tag=0; // CAM-EPOCH: the frame sample's read-start/publication time, epochs and recording-generation tag
    acz::interp::Facts interp{}; // OBJECT-INTERP (interp:1): this frame's plan (camera delta, k) and object counts; key off: never set
    uint64_t untrackedBase=0; // DECOUPLE review 3: Step5State::untrackedExec at this frame's submission (step-1 mutex), published as Step5Slot::pubUntracked
    // EFFECTS-OUTSIDE (C) fxmem:1: +0 (A) of this frame's first ACCEPTED DrawACM_v record (world objects key), compared with the smoke records' +0.
    bool fxObjA=false;float fxObjAm[16]{};
};
enum Step5SkipKind : int {skip_mismatch=0,skip_unqualified=1,skip_unreadable=2,skip_no_slot=3};
struct Step5Pend {bool set=false;Step1Route list=0;uint64_t gen=0;int kind=0;const char* why=nullptr;Step5Detail detail{};};
// J-3b9 (review: matching is a heuristic): the selected cockpit members' signature
// classes and the PSOs observed for the selected ordinals at selection time.
struct Step5Class {uint64_t root=0;uint32_t topology=0,ib=0,vb=0,t10=0,cut=0,rtv=0,dsv=0,samples=0,quality=0,depthFunc=0;
    bool depthEnable=false,depthWrite=false,stencil=false;};
inline Step5Class step5ClassOf(const gpu_copy::step3::Shape& m) noexcept {
    Step5Class c;c.root=m.root_route;c.topology=m.topology;c.ib=m.ib_stride;c.vb=m.vb_stride;c.t10=m.t10_parameter;c.cut=m.strip_cut;
    c.rtv=m.pso.rtv[0];c.dsv=m.pso.dsv;c.samples=m.pso.samples;c.quality=m.pso.quality;c.depthFunc=m.pso.depth_func;
    c.depthEnable=m.pso.depth_enable;c.depthWrite=m.pso.depth_write;c.stencil=m.pso.stencil;return c;
}
inline bool step5SameClass(const Step5Class& a,const Step5Class& b) noexcept {
    return a.root==b.root&&a.topology==b.topology&&a.ib==b.ib&&a.vb==b.vb&&a.t10==b.t10&&a.cut==b.cut&&a.rtv==b.rtv&&a.dsv==b.dsv&&
        a.samples==b.samples&&a.quality==b.quality&&a.depthFunc==b.depthFunc&&a.depthEnable==b.depthEnable&&a.depthWrite==b.depthWrite&&
        a.stencil==b.stencil;
}
struct Step5Admit {uint64_t pso=0,ordinal=0,frame=0;};
// J-hud-colour (gpu_copy_step5_png_once, default OFF): the once-per-process readback of the HUD
// target, world snapshot and eye-0 image (two samples). `stage` is written under the step-1 mutex,
// except by the worker's lock-free processing of a READY sample, which owns every other field until
// it stores png_processed (the game thread touches nothing while stage is ready).
enum Step5PngStage : int {png_off=0,png_armed=1,png_recorded=2,png_submitted=3,png_ready=4,png_processed=5,png_done=6,png_failed=7};
struct Step5Png {
    std::atomic<int> stage{png_off};
    ID3D12Resource* readback=nullptr;HRESULT createHr=S_OK;const char* refusal=nullptr;
    uint32_t w=0,h=0;uint64_t pitch=0,stride=0,bytes=0;
    unsigned read=0;uint64_t notBefore=0;          // samples read so far; published count the next sample waits for
    Step1Route list=0;uint64_t generation=0,value=0,epoch=0,publishedAt=0;unsigned slot=0,hudDraws=0,members=0;
    ID3D12Resource* src[gpu_copy::png::image_count_fx]{};
    bool reexecuted=false,emitArmed=false,emitFail=false,emitDone=false;const char* failWhy=nullptr;
    uint64_t waitedFrames=0;const char* lastWait=nullptr; // frames closed while armed but not eligible
    // J-effects-content (png_once + the effects layer ready): the readback holds a 4th image, the effects target of the captured slot.
    //  fxCapable : the readback was sized for 4 images (effects ready at creation); fx : THIS sample copied/holds the effects image
    //  (a base sample includes it when the frame cleared the effects target, so it holds no stale content; a burst sample always);
    //  burst : the sample being waited for / taken is an effects-burst sample (after the base samples, committed draws >= thr).
    bool fxCapable=false,fx=false,burst=false,emitBurstArmed=false;const char* thrSource="default";
    unsigned thr=gpu_copy::png::threshold_default,burstRead=0,fxDraws=0;uint64_t burstBelow=0;
    // J-effects-content burst protocol v2: every burst set (<= 3 per process, shared by both triggers) writes the world / eye0 / hud-over-world PNGs
    // too. A second trigger: the file %LOCALAPPDATA%\acz-vr\png-burst.now (worker polls its stamp every 200 ms; `fileTrig` counts changes after the
    // first poll; the game thread takes the next valid frame once `fileTrig != fileSeen`). burstFile : THIS burst set was file-triggered.
    std::atomic<uint32_t> fileTrig{0};std::atomic<uint64_t> fileReqTick{0};
    uint32_t fileSeen=0,fileBursts=0;bool burstFile=false;uint64_t captureTick=0,reqTickAtCapture=0,capturePublished=0;
    // J-scene-end: file-triggered burst sets also hold the game's scene render target at the START of the HUD region (after all scene draws), copied
    // DIRECTLY into footprint 4 of the readback at the region end (no private image) while a file request is pending and the machinery is armed.
    // seList/seGen/seCopies/seWhy : the copy recorded for the frame (list generation) now being recorded; se : THIS set holds the image;
    // seDeclinedRun/seDeclined : a file request waited for a frame with a copy (at most 2 in a row, then the set is served without it).
    Step1Route seList=0;uint64_t seGen=0,seDeclined=0;unsigned seCopies=0,seDeclinedRun=0;const char* seWhy=nullptr;bool se=false;
    // worker-only poller state
    bool fileBaselined=false;uint64_t fileStamp=0,filePollTick=0,fileRequests=0,fileIgnored=0;
    // the phase observer's summary of the captured frame (copied at the list's Close, only with gpu_copy_step5_phase_observe)
    bool phHave=false;uint64_t phFrame=0;gpu_copy::phobs::Summary ph{};
};
// J-180 (Codex review P1): bounded per-recording ownership of an injected writer. A SUBMITTED recording (a closed command list whose replay-bearing
// commands write slot `slot`'s private images and read its UPLOAD span) can execute again any number of times until it is Reset or destroyed,
// whatever was submitted in between (A -> B -> A). While an entry is live its slot is never handed to another recording (step5PickSlot_locked);
// every further Execute of the recording is an occurrence counted on the entry and signalled on the private fence. Entries are retired only by a
// successful Reset or the list's final Release. A full table refuses NEW replay insertion (an executable writer is never evicted).
constexpr unsigned step5_owner_cap=8;
struct Step5Owner {bool live=false;Step1Route route=0;uint64_t generation=0,epoch=0,lastUse=0,executes=0;unsigned slot=0;};
struct Step5State {
    std::mutex meta; // slot metadata only (game thread: try_lock only; consumer: lock)
    std::array<Step5Slot,gpu_copy::step5::slots> slot{};
    // TACTICAL (tac:1; tactical_frame.h): the open frame's facts, one published copy per slot (read by the consumer while the slot is HELD) and the
    // marker-sprite scratch. Heap storage (Step5State is new'ed), never on the game thread's stack. Untouched while acz::tac::producer_flags() == 0.
    acz::tac::TacFrame tacCur{};std::array<acz::tac::TacFrame,gpu_copy::step5::slots> tacSlot{};acz::tac::SpriteScratch tacScratch{};
    uint32_t tacIdx[acz::tac::kMaxSpriteVerts]{};uint8_t tacIbBytes[acz::tac::kMaxSpriteVerts*4]{};
    bool tacOpen=false,tacSuppressLabel=false;uint32_t tacFlags=0,tacBoxFirst=0,tacWedgePending=0;
    // LOCK-SHAPE / mk360cen (markers-360.md section 17.4): per-frame read budget, HUD draws since the first box draw, census lines of this frame; census run state
    unsigned tacLockReads=0,tacAfterBox=0,tacCensusLines=0,tacCensusDone=0;bool tacCensusFrame=false;uint64_t tacCensusLastMs=0,tacLockMoved=0,tacLockRejected=0;
    acz::tac::LockWhyStats tacLockWhy; // LOCK-WHY (headset test 8): lock_shape_test verdicts + the gates before it (log line <prefix>-tac-lock)
    acz::tac::LockDumpState tacDump;unsigned tacDumpReads=0; // mk360ld:1 (markers-360.md section 21.6): signatures / lines written, reads this frame
    uint64_t tacCensusFar=0; // CENSUS-AIM: instrument draws of census frames not logged (far from every box)
    // MSL-HUD (hud_msl.h; aircraft-auto.md section 14): the HUD readout block's glyph quads of a sampled frame -> the MSSL count -> acz::stores::signal().
    // Heap storage (Step5State is new'ed). Untouched while mslhud resolves to 0.
    bool mslSample=false,mslDumpFull=false;uint64_t mslFrameN=0,mslLastSummaryMs=0;unsigned mslVerts=0,mslEventLines=0,mslSpEventLines=0; // SP-STORES: mslSpEventLines = the SP row's own msl-hud event lines (max hm::kEventLines)
    acz::hudmsl::GlyphFrame mslFrame{};acz::hudmsl::Scratch mslScratch{};acz::hudmsl::Work mslWork{};acz::hudmsl::Decoder mslDec{};
    uint8_t mslIbBytes[acz::hudmsl::kMaxIdx*4]{};
    unsigned mslDumps=0,mslDumpCursor=0,mslDumpN=0;double mslLastDumpMs=-1e18;acz::hudmsl::GlyphFrame mslDumpFrame{};
    uint64_t tacFrames=0,tacObjSum=0,tacMarkSum=0,tacMatchSum=0,tacSupBoxes=0,tacSupLabels=0,tacUnreadBox=0,tacUnreadLabel=0,tacWedgeFrames=0,tacWedgeTail=0,
        tacBoxDraws=0,tacLabelDraws=0,tacLastLog=0;
    const char* tacLastUnread=nullptr;
    // OBJECT-INTERP (interp:1; object_interp.h): the tracker + camera planner (heap, ~80 KB), its frame sequence, and the display-time position of the
    // object just observed (handed to step5TacObject). Untouched while acz::interp::armed() is false.
    acz::interp::FramePlanner interpPlanner{};uint64_t interpSeq=0;bool interpConfigured=false,interpDispFresh=false;float interpDisp[3]{};
    uint64_t latestFrame=0,nextFrameId=0; // under meta
    // DECOUPLE review 2/3 (FrameSource::images_unchanged): Execute calls whose owned-recording re-executions were NOT examined (producer disabled,
    // oversize batch, null list array, any Execute while the phase is not continuous: step5ExecuteUnexamined). Incremented (seq_cst) before the
    // native Execute; a frame SUBMITTED before such a call is no longer provably unchanged (never acquired again, never reported unchanged).
    std::atomic<uint64_t> untrackedExec{0};
    uint64_t acquireLostRace=0; // under meta: acquisitions refused because the selected slot left PUBLISHED before the exchange (review 3)
    // Everything below: under the step-1 mutex.
    Step5Frame frame{};Step5Pend pend{};
    bool pending=false;Step5Frame pendingFrame{};uint64_t pendingValue=0;unsigned pendingFree=0; // bitmask
    uint64_t fenceCounter=0;Step1Route lastList=0;uint64_t lastGen=0;unsigned lastSlot=0;bool lastLive=false;
    // J-180 P1 ownership table (step-1 mutex) and its accounting. ownerReexec = Execute occurrences of an owned recording that was NOT the active
    // frame's submission; ownerHeld = of those, how many hit a slot a consumer held at that moment; ownerBlocked = slot picks that skipped an owned slot;
    // ownerRefused = new replay insertions refused because the table could not represent another recording.
    std::array<Step5Owner,step5_owner_cap> owner{};
    uint64_t ownerAdded=0,ownerRetired=0,ownerReexec=0,ownerHeld=0,ownerBlocked=0,ownerRefused=0;
    // J-180 P2: eligible world draws lost (any cause) and the frames published with world_incomplete.
    uint64_t wobjLostSum=0,wobjIncompleteFrames=0,wobjPreLostSum=0;
    // J-180 P2 depth fix: frames whose world camera was evaluated with the fix applied; the last modelled band of view depths [lo,hi) whose geometry the
    // fix admits although the original record clips it; frames whose A did not fit the measured depth model (no claim).
    uint64_t wobjClipFrames=0,wobjClipUnmodelled=0;double wobjClipLo=0,wobjClipHi=0;bool wobjClipValid=false;
    Step1Route wobjPreRoute=0;uint64_t wobjPreGen=0;unsigned wobjPreLost=0; // eligible losses of the CURRENT list generation before its frame opened
    std::atomic<uint64_t> published{0},skipped{0};
    uint64_t discarded=0,ambiguous=0,failures=0,consecutiveMismatch=0,reselections=0,poseFrames=0,frames=0,commandSum=0,memberSum=0,
        skipKind[4]{},mismatchProduced=0,memberRefused=0,extraMembers=0,fewerMembers=0,partialFrames=0,lensFrames=0,
        worldFrames=0,worldRefused=0,mismatchEvents=0,mismatchLines=0;
    const char* lastSkipWhy=nullptr;const char* lastMemberWhy=nullptr;const char* worldLastWhy=nullptr;
    bool disabled=false,finished=false,ringReady=false,emittedFirst=false,emittedDefinitions=false;
    uint32_t ringW=0,ringH=0,ringFormat=0,ringDsv=0,ringDsTypeless=0;float ringClear=0;const char* disableReason=nullptr;const char* lastFault=nullptr;
    // EYE-RING (eye_ring.h, eye-resolution.md 5.1 (a)): ringW/ringH are the PER-EYE target size (eye ring: the eye swapchain size), gameW/gameH the game's
    // viewport extents the targets were decided for (the game-screen images: world, HUD, markers, effects). Without the eye ring both pairs are equal and
    // every path is the old one. eyeRing is fixed for the process at ring creation. perMember: commands one member adds (8, +4 viewport/scissor on the ring).
    uint32_t gameW=0,gameH=0;bool eyeRing=false,eyeRingAsked=false;const char* eyeRingWhy="off";unsigned perMember=gpu_copy::step3::set_per_member;
    uint64_t eyeVpSub=0,eyeVpRestore=0,eyeScSub=0,eyeMemberDraws=0,eyeFxReplays=0,eyeRsRefused=0; // step-1 mutex (summary-eye-ring)
    unsigned statusTicks=0,mismatchTicks=0;uint64_t lastCompleted=0;
    ID3D12Resource* upload=nullptr;uint8_t* uploadPtr=nullptr;D3D12_GPU_VIRTUAL_ADDRESS uploadVa=0;HRESULT createHr=S_OK;
    gpu_copy::step5::Neutral neutral{};gpu_copy::step5::PoseFrame pf{};gpu_copy::step5::Scratch scratch{};gpu_copy::step5::Lens lens{};
    look_drive::Driver lookDrive{}; // xr_look_drive_keys (default OFF: never configured, never touched)
    bool wobjEarlyLookUsed=false;double wobjEarlyLookR[9]{}; // world-first frame under the look drive: the compensation its world members were built with
    // World snapshot: textures exist (owner context) and the selection's game RT.
    bool worldReady=false;const char* worldArmWhy="not-armed";uintptr_t worldSource=0;uint64_t worldW=0;uint32_t worldH=0,worldFormat=0;
    bool worldFirstPending=false,worldFirstEmitted=false,worldRefusalPending=false,worldRefusalEmitted=false;
    uint64_t worldFirstFrame=0;uintptr_t worldFirstResource=0;const char* worldFirstRefusal=nullptr;
    Step5Detail mismatchOut{};
    std::array<Step5Class,16> classes{};unsigned classCount=0;std::array<uint64_t,16> altPso{};unsigned altCount=0;
    std::array<Step5Admit,16> admits{};unsigned admitCount=0,admitLogged=0;uint64_t admitRefusedFull=0; // J-3b9c run-time alternates
    acz::feature_log::Handle log=acz::feature_log::invalid; // J-3b9c dedicated evidence log (step5-producer.log)
    uint64_t poseDropped=0,gpuBusy=0;const char* worldFirstEvidence=nullptr;
    // J-world-hold diagnostics (always on with step 5; counters + a few detail records, step-1 mutex): entry-state-unknown by
    // sub-condition, head/game look pitch of accepted vs refused frames, and the refusal detail ring (emitted by the worker).
    uint64_t worldEntrySub[gpu_copy::step5::entry_subs]{},worldEntryTotal=0,worldBursts=0,worldLastRefusedFrame=0,worldRefDetailDropped=0;bool worldRefDefsEmitted=false;
    gpu_copy::step5::PitchStat pitchAccepted{},pitchRefused{},pitchEntry{};
    std::array<gpu_copy::step5::RefusalDetail,gpu_copy::step5::refusal_detail_cap> refDetail{};unsigned refDetailCount=0,refDetailEmitted=0;
    // Issue-29 fence correlation: epoch = lifetime id of the private fence (assigned when the
    // producer first sees that fence object; a different fence object gets a new epoch and
    // starts its own count); lastSignalled = highest value whose native Signal returned S_OK
    // after the native ExecuteCommandLists returned (failed/ambiguous Signals never advance it).
    std::atomic<uint64_t> fenceEpoch{0},lastSignalled{0};ID3D12Fence* epochFence=nullptr;uint64_t publishRefused=0;
    // First published frame (one detailed evidence line).
    char firstSha[65]{};uint64_t firstFrame=0,firstValue=0;unsigned firstMembers=0,firstCommands=0;bool firstPose=false,firstCaptured=false;
    bool firstWorld=false,firstLens=false;float firstLensTan[4]{};
    const char* firstPoseWhy="not-evaluated";unsigned controlDiffer=999;std::array<uint8_t,416> firstLive{};
    gpu_copy::step5::PoseFrame controlPf{};std::array<uint8_t,416> controlOut{}; // heap: the once-only control
    // J-hud-capture: the HUD targets exist (owner context) for the selection's scene target.
    bool hudReady=false;const char* hudArmWhy="not-configured";uint32_t hudFormat=0,hudDsFormat=0,hudDsClear=D3D12_CLEAR_FLAG_DEPTH;
    uint64_t hudValidFrames=0,hudInvalidFrames=0,hudUnarmedFrames=0,hudDrawSum=0,hudRefusedSum=0,hudReason[gpu_copy::step5::hud_categories]{},
        hudResets=0,hudCapFrames=0,hudDepthTested=0,hudEnd[3]{};
    uint64_t hudRsSkipSum=0,hudRsSkipFrames=0,hudRsDswSum=0; // HUD-RS-SKIP (armed frames)
    gpu_copy::step5::HudLateSums hudLate{}; // HUD-LATE (armed radar frames while hudlate:1)
    gpu_copy::step5::HudLateOnset hudLateOnset{}; // SUBTITLE SHOT: onset detector for the eye-dump request
    const char* hudLastWhy=nullptr;const char* hudLastUnarmed=nullptr;
    bool hudFirstPending=false,hudFirstEmitted=false,hudRefusalPending=false,hudRefusalEmitted=false,hudRingEmitted=false;
    uint64_t hudFirstFrame=0;Step5Hud hudFirst{},hudRefusal{};const char* hudFirstEnd=nullptr;
    // J-hud-radar (gpu_copy_step5_hud_radar): sums over the armed frames and the pending hud-end diagnostic (zero / never touched with the key off).
    uint64_t radarFrames=0,radarExcFrames=0,radarEqualFrames=0,radarExcursions=0,radarReturns=0,radarReplace=0,radarEqual=0,radarEqualIndices=0,radarLate=0,
        radarWindows=0,radarA3050=0,radarNotEqual=0,radarLateSkipped=0,radarAwaySkipped=0,radarExcWriters=0,radarGaps=0,radarEqualRefused=0,radarGapRefused=0,
        radarMaxEqual=0,radarMaxGroup=0,radarGroupSum=0,
        radarSceneLeaves=0,radarSceneResumes=0,radarSceneBackRefused=0,radarResumedFrames=0; // RADAR-SCENE-RESUME
    bool radarDiagPending=false;uint64_t radarDiagFrame=0,radarDiagEmitted=0,radarDiagLastTick=0;const char* radarDiagEnd=nullptr;
    gpu_copy::step5::HudRadarDiag radarDiag{};gpu_copy::step5::HudRadar radarDiagRd{};
    // J-hud-markers: the second HUD targets exist (owner context); counters per frame armed with the split.
    bool hudMarkersReady=false;const char* hudMarkersArmWhy="not-configured";uint64_t hudMarkersCharged=0;
    uint64_t hudMarkerValidFrames=0,hudMarkerInvalidFrames=0,hudMarkerAbsentFrames=0,hudMarkerDrawSum=0,hudMarkerBoxSum=0,hudMarkerLabelSum=0,
        hudMarkerRefusedSum=0,hudMarkerReason[gpu_copy::step5::hud_categories]{};
    const char* hudMarkerLastWhy=nullptr;
    bool hudMarkerFirstPending=false,hudMarkerFirstEmitted=false,hudMarkerRefusalPending=false,hudMarkerRefusalEmitted=false;
    uint64_t hudMarkerFirstFrame=0;Step5Hud hudMarkerFirst{},hudMarkerRefusal{};
    // J-effects-layer (gpu_copy_step5_effects): targets + predicate buffer exist (owner context); counters per frame armed.
    bool effReady=false;const char* effArmWhy="not-configured";uint64_t effCharged=0;
    ID3D12Resource* effPred=nullptr;uint8_t* effPredPtr=nullptr;D3D12_GPU_VIRTUAL_ADDRESS effPredVa=0;
    uint64_t effValidFrames=0,effInvalidFrames=0,effAbsentFrames=0,effUnarmedFrames=0,effDrawSum=0,effTailSum=0,effRefusedSum=0,effConstBlend=0,
        effIgnored=0,effCommits=0,effEpochCapFrames=0,effReason[gpu_copy::step5::hud_categories]{},effSpeculative=0;
    const char* effLastWhy=nullptr;
    uint64_t hudDsWriters=0; // J-effects-content: HUD-replayed draws whose PSO writes depth into the DS shared with the effects layer (expected 0)
    bool effFirstPending=false,effFirstEmitted=false,effRefusalPending=false,effRefusalEmitted=false;
    uint64_t effFirstFrame=0;Step5Eff effFirst{},effRefusal{};
    // J-effects-smoke-fix: the allowlist, predication mode (read once at ring creation), the PS-prefix candidate table and the diagnostics line queue.
    gpu_copy::step5::EffectsAllow effAllow{};int effPredMode=gpu_copy::step5::effects_pred_eq0;const char* effAllowSource="default";const char* effPredSource="default";
    unsigned effAllowBad=0,effAllowAdded=0;gpu_copy::step5::EffectsPsTable effPs{};uint64_t effPredRefused=0,effUavRefused=0,effClassIgnored=0;
    uint64_t effGameCloudIgnored=0; // EFFECTS-OUTSIDE: game cloud draws not replayed (gamecloud:0)
    // EFFECTS-OUTSIDE (K, docs section 17): the candidates refused as dual-blend-unsupported (drawn NOWHERE in the eye: no window, no per-eye replay) and the
    // last one's identity, logged as `-hud-effects-dual-refused` so a live run names the blend tuple to support (43,679 GREATER 4caf6e2d draws in m03k).
    uint64_t effDualRefused=0;uint32_t effDualPs=0,effDualVs=0,effDualCount=0,effDualTopo=0,effDualDepth=0;uint8_t effDualBlend[6]{};
    bool effDualIndexed=false,effDualTracerReady=false,effDualReady=false;
    // EFFECTS-OUTSIDE (P) fxadd:1 (docs section 21): additive 4caf6e2d draws admitted by the effects layer (window or per eye), replayed per eye, the point-list draws among
    // them replayed per eye, and the frames with any per-eye additive replay; the eye-dump shots requested. Logged as `-hud-effects-additive` while fxadd is on.
    uint64_t effAddAdmitted=0,effAddPoints=0,effAddPerEye=0,effAddPtsPerEye=0,effAddFrames=0;gpu_copy::step5::AdditiveShot effAddShot{};
    static constexpr unsigned effDiagCap=64;std::array<Step5EffDiagLine,effDiagCap> effDiagQ{};unsigned effDiagHead=0,effDiagCount=0;
    uint64_t effDiagLinesTotal=0,effDiagDropped=0,effPsLogTick=0;unsigned effDiagValidSeen=0;
    // EFFECTS-OUTSIDE (C) fxmem:1 (effects_smoke_capture_flag; fx_smoke.h, docs section 8.3 steps 1-2): the open frame's bounded smoke capture (64 records,
    // ~105 KB, heap; 16 until docs section 18), the read scratch (two guarded passes must agree), the pending fx-smoke-frame line (worker) and its rate limit. Capture + log ONLY:
    // nothing here is recorded into a command list or written to game memory. Untouched while the flag is false.
    acz::fxsmoke::FrameCapture fxCap{};uint8_t fxRec[acz::fxsmoke::kRecordBytes]{},fxRec2[acz::fxsmoke::kRecordBytes]{};bool fxOpen=false;
    bool fxPending=false;uint64_t fxPendFrame=0;unsigned fxPendSkipped=0;acz::fxsmoke::FrameSummary fxPend{};acz::fxsmoke::LogGate fxGate{};
    uint64_t fxDraws=0,fxStored=0,fxUnread=0,fxFrames=0,fxLines=0;const char* fxLastUnread=nullptr;
    uint64_t fxOverCap=0;unsigned fxPeakDraws=0; // docs section 18 (read-only): smoke draws over the capture cap (each = a window-only trail) + the per-frame peak
    // EFFECTS-OUTSIDE (C) fxmem:2 (effects_smoke_replay_flag; docs section 8.6): the per-eye replay DRY RUN. Every stored smoke record is prepared for both eyes
    // from the frame's world camera into a bounded CPU store (fx_smoke.h ReplayStore, 200 KB, heap) and its trail points are counted per eye; the result is the
    // rate-limited fx-smoke-replay-plan line. CPU only: nothing is recorded into a command list, no GPU resource is created or bound. Latched per frame.
    acz::fxsmoke::ReplayStore fxStore{};acz::fxsmoke::ReplayDryRun fxDry{},fxDryPend{};bool fxDryOn=false,fxDryPending=false;
    uint64_t fxDryPrepared=0,fxDryRefused=0;
    // EFFECTS-OUTSIDE (D) fxmem:3 (effects_census_flag; gpu_copy_step5_fxcensus.inl): the draw census (~12 KB) and the class-gate verdict of the current draw.
    acz::fxcensus::Census fxCen{};const char* fxCenGate=nullptr;bool fxCenWindow=false;
    // EFFECTS-OUTSIDE (D) fxsig:<ps8> (effects_sig_filter; docs section 12): the per-draw log policy and its line queue (~19 KB), written by the draw seam, drained by the worker.
    acz::fxcensus::SigLog fxSigLog{};acz::fxcensus::SigQueue fxSigQ{};
    // READ-ONLY flare / aircraft diagnostics (fx_diag_flare_air.h; armed by the fxsig sentinels): limiters, the line queue (~14 KB) and the aircraft draw ordinal.
    acz::fxdiag::Limiter diagFlareLim{},diagAirLim{};acz::fxdiag::FlareTracker diagFlareTrk{};acz::fxdiag::LineQueue diagQ{};uint64_t diagOrd=0;
    // EFFECTS-OUTSIDE (E) fxmem:5 (effects_explosion_flag; gpu_copy_step5_fxsprite.inl): the explosion sprite read scratch (~70 KB) and the per-frame CPU dry-run plan.
    uint16_t fxSprIdx[acz::fxsprite::kMaxIndices]{};acz::fxmem::Vtx fxSprVtx[acz::fxsprite::kMaxVertices]{},fxSprEye[acz::fxsprite::kMaxVertices]{};uint8_t fxSprBad[acz::fxsprite::kMaxVertices]{};
    acz::fxsprite::Plan fxSpr[3]{},fxSprPend[3]{};bool fxSprOpen=false,fxSprPending[3]{};uint64_t fxSprPendFrame[3]{},fxSprFrames[3]{},fxSprRead[3]{},fxSprRefused[3]{},fxSprStreakLast=0,fxSprTracerLast=0;
    // WORLD-CLOUDS wcdiag:1 (fx_sprite_replay.h puff_samples; read-only): the game's puff quads of one frame every >= 250 ms (one fx-cloud-puff line).
    acz::fxsprite::PuffSample fxPuff[acz::fxsprite::kPuffPerLine]{};unsigned fxPuffN=0,fxPuffSeen=0;double fxPuffEye[3]{};bool fxPuffOpen=false,fxPuffPending=false,fxPuffEyeOk=false;
    uint64_t fxPuffPendFrame=0,fxPuffLastMs=0;
    unsigned fxSprPendSkipped[3]{};acz::fxsmoke::LogGate fxSprGate[3]{}; // [0] explosion (PS 542a11d7), [1] indexed GREATER 4caf6e2d sprites (tracer candidates), [2] the gun tracer (PS d34ee153, fxmem:6)
    // EFFECTS-OUTSIDE (F) fxmem:6 (effects_sprite_gpu_flag; fx_sprite_gpu.h, docs section 11): the last planned draw's two eye copies (64 KB; valid only while
    // fxSprLastPrepared, i.e. between step5FxSpriteCapture and the GPU plan of the SAME draw under one step-1 mutex hold), the sprite UPLOAD block (one 512 KiB
    // span per slot, persistently mapped; the images are per slot in Step5Slot), created once (ring or late) and kept for the process lifetime, the pending
    // fx-sprite-gpu line and the run totals. Untouched unless fxmem:6 is requested.
    acz::fxmem::Vtx fxSprKeep[2][acz::fxsprite::kMaxVertices]{};bool fxSprLastPrepared=false;int fxSprLastClass=-1;uint32_t fxSprLastLo=0,fxSprLastN=0;
    // EFFECTS-OUTSIDE (M) fxtrail:<s> (fx_puff_trail.h; docs section 19): the world-space puff memory (~49 KB), the selection of the current draw (valid only
    // between step5FxSpriteCapture and the GPU plan of the SAME draw, like fxSprKeep) with its two eye copies (2 x 24 KiB), the frame counter, the last game map
    // of the frame (the end-of-frame cull) and the pending fx-puff-trail line. Untouched unless fxtrail is set with fxmem:6.
    acz::fxpuff::Memory fxTrail{};uint16_t fxTrailSel[acz::fxpuff::kMaxEmit]{};unsigned fxTrailSelN=0;uint64_t fxTrailSelKey=0;
    acz::fxpuff::Group fxTrailGroups[2][acz::fxpuff::kMaxKeys]{};unsigned fxTrailGroupN[2]{};uint32_t fxTrailParam=UINT32_MAX; // (M4) per-key groups of the eye copies
    uint8_t fxTrailFade[acz::fxsprite::kMaxVertices]{}; // (M4c) fxtclip:fade: the border vertices of cut cards (alpha 0 in the eye copies)
    acz::fxmem::Vtx fxTrailEye[2][acz::fxpuff::kMaxEmitVerts]{};unsigned fxTrailVerts[2]{};
    uint64_t fxTrailFrame=1;bool fxTrailOpen=false,fxTrailGameOk=false;acz::fxmem::ScreenMap fxTrailGame{};acz::fxpuff::Options fxTrailOpt{};
    acz::fxpuff::Stats fxTrailWin{};unsigned fxTrailWinFrames=0;uint64_t fxTrailPendFrame=0,fxTrailLineMs=0; // (M2) the 1/s line's window (step5FxTrailTick)
    float fxTrailObj[acz::fxpuff::kObjCap][3]{};unsigned fxTrailObjN=0; // (M3) world positions of this frame's object records (the near rule)
    // EFFECTS-OUTSIDE (M6) fxatlas (fx_puff_atlas.h; docs section 23): our atlas texture (RGBA16F, PIXEL_SHADER_RESOURCE between our uses), its CPU RTV heap,
    // its shader-visible SRV heap (kTableMax SRVs of the atlas) and the capture vertex UPLOAD block (one 64 KiB region per slot, persistently mapped, written
    // only while the slot is PRODUCING). Created ONCE per process, late, by the worker tick when fxatlas is on and the sprite ring is ready, on the ring's
    // device; kept for the process lifetime (never released or recreated); atlasReady set last. The tile table lives in fxTrail.atlas.
    bool atlasReady=false,atlasTried=false,atlasRingPending=false;const char* atlasWhy="not-requested";HRESULT atlasHr=S_OK;
    ID3D12Resource* atlasTex=nullptr;ID3D12DescriptorHeap* atlasRtvHeap=nullptr;ID3D12DescriptorHeap* atlasSrvHeap=nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE atlasRtv{};D3D12_GPU_DESCRIPTOR_HANDLE atlasSrv{};
    ID3D12Resource* atlasUpload=nullptr;uint8_t* atlasUploadPtr=nullptr;D3D12_GPU_VIRTUAL_ADDRESS atlasUploadVa=0;
    // (M6) the fx-puff-atlas line: capture refusals / host refusals of the window (last reason), the run's tile counters and totals
    uint32_t fxAtlasCapNo=0,fxAtlasHostNo=0;const char* fxAtlasCapWhy=nullptr;const char* fxAtlasHostWhy=nullptr;
    acz::fxpuff::atlas::Counters fxAtlasRun{};
    // (M6b) the host root-signature layout: the last refused host's (lay= on the 1/s line) and the first host's (one fx-puff-atlas-host line)
    char fxAtlasHostLayout[72]{};char fxAtlasFirstLayout[72]{};const char* fxAtlasFirstWhy=nullptr;unsigned fxAtlasFirstHeaps=0;bool fxAtlasFirstSampler=false;
    bool fxAtlasLayoutLogged=false,fxAtlasLayoutPending=false;
    bool spriteReady=false,spriteRingPending=false,spriteTried=false,spriteLate=false;const char* spriteArmWhy="not-requested";uint64_t spriteCharged=0;
    ID3D12Resource* spriteUpload=nullptr;uint8_t* spriteUploadPtr=nullptr;D3D12_GPU_VIRTUAL_ADDRESS spriteUploadVa=0;
    bool spriteGpuPending=false;acz::fxsprite::gpu::FrameState spriteGpuPend{};unsigned spriteGpuPendFallback=0,spriteGpuPendExplosion=0,spriteGpuPendSprite=0,spriteGpuPendLight=0;const char* spriteGpuPendXpsoWhy=nullptr;
    uint64_t spriteGpuPendFrame=0;acz::fxsmoke::LogGate spriteGate{};
    uint64_t spriteFrames=0,spriteValidFrames=0,spriteReplays=0,spriteCommitted=0,spriteRefused=0,spriteFallback=0;const char* spriteLastWhy=nullptr;
    // EFFECTS-OUTSIDE (C) fxmem:4 (effects_smoke_gpu_flag; fx_smoke_gpu.h, docs section 8.7): the per-eye GPU replay's UPLOAD block (one 64 KiB span per slot,
    // persistently mapped; the images are per slot in Step5Slot), created once at ring creation and kept for the process lifetime; the store index of the draw
    // just prepared (-1 = none), the pending fx-smoke-gpu line and the run totals. Untouched unless fxmem:4 was requested when the ring was created.
    bool smokeReady=false,smokeRingPending=false,smokeTried=false,smokeLate=false;const char* smokeArmWhy="not-requested";uint64_t smokeCharged=0;
    ID3D12Resource* smokeUpload=nullptr;uint8_t* smokeUploadPtr=nullptr;D3D12_GPU_VIRTUAL_ADDRESS smokeUploadVa=0;
    int fxLastIdx=-1;bool fxGpuPending=false;acz::fxsmoke::gpu::FrameState fxGpuPend{};unsigned fxGpuPendFallback=0;
    uint64_t smokeFrames=0,smokeValidFrames=0,smokeReplays=0,smokeCommitted=0,smokeRefused=0,smokeFallback=0;const char* smokeLastWhy=nullptr;
    // J-cockpit-fov caps, fixed at ring creation (key off: 64 members, 600 commands, 64 KiB per slot).
    unsigned memberCap=gpu_copy::step3::set_cap,commandCap=gpu_copy::step3::set_command_cap;uint64_t slotUpload=gpu_copy::step5::slot_upload_bytes;
    // F1 (gpu_copy_step5_hmd_fov) per published posed frame.
    uint64_t fovFrames=0,fovFallback=0,fovClampedFrames=0;const char* fovLastWhy=nullptr;
    bool fovFirstPending=false,fovFirstEmitted=false,fovFallbackPending=false,fovFallbackEmitted=false;
    uint64_t fovFirstFrame=0;float fovFirst[2][4]{};const char* fovFirstWhy=nullptr;float fovFirstLens[4]{};bool firstFovHmd=false;
    // F3 (gpu_copy_step5_members_by_class): refusal categories, starts refused as out of class,
    // capped frames and the learned family classes (logged once each by the worker).
    uint64_t classReason[gpu_copy::step5::member_categories]{},classOutStarts=0,classCapFrames=0,classFrames=0,classPartial=0,classUnlogged=0;
    const char* classLastWhy=nullptr;
    struct FamilyAdmit {Step5Class c{};uint64_t pso=0,ordinal=0,frame=0;unsigned nearest=0;};
    std::array<FamilyAdmit,16> family{};unsigned familyCount=0,familyLogged=0;
    // J-world-objects (gpu_copy_step5_world_objects): the frame's world camera (reset by the frame setup) and counters.
    // wobjBeforeFrame: DrawACM_v draws with no active frame (never a frame start; expected for the earlier passes).
    gpu_copy::step5::world::Frame wobj{};
    uint64_t wobjSeen=0,wobjBeforeFrame=0,wobjRecordedSum=0,wobjRefusedSum=0,wobjFrames=0,wobjReason[gpu_copy::step5::world::categories]{};
    const char* wobjLastWhy=nullptr;
    // Totals across frames (the per-frame `wobj` is reset by every frame setup): frames whose world camera recovery was
    // EVALUATED (a first accepted record reached camera_setup), how many recovered / failed, the last failure, and the
    // DrawACM_v draws seen in ANOTHER list than the active frame's while a frame was active.
    uint64_t wobjCamEval=0,wobjCamOk=0,wobjCamFail=0,wobjOtherList=0;const char* wobjCamLastWhy=nullptr;
    bool wobjFirstPending=false,wobjFirstEmitted=false,wobjDefsEmitted=false,wobsDefsEmitted=false,phaseDefsEmitted=false;
    // J-world-objects v2 (same-list early frame start). Opened = a world draw opened a frame; adopted = its list's first cockpit member
    // continued it; abandoned = executed without adoption (never published); mismatch = adoption refused on the camera/lens/pose.
    // wobjEarlyReason: per-draw start refusals by world category; blockRoute/Gen: no further start attempt in that list recording.
    uint64_t wobjEarlyOpened=0,wobjEarlyAdopted=0,wobjEarlyAbandoned=0,wobjEarlyMismatch=0,wobjEarlyBlocked=0,wobjEarlyStreak=0,
        wobjEarlyReason[gpu_copy::step5::world::categories]{};
    Step1Route wobjBlockRoute=0;uint64_t wobjBlockGen=0;unsigned wobjEarlyHold=0;const char* wobjEarlyOff=nullptr,*wobjEarlyLastWhy=nullptr,*wobjAdoptLastWhy=nullptr;
    // POV-RENDER stage 2 (pov:2): no-cockpit chase frames (opened, refused by the E6 opener check, poisoned by a cockpit draw), the relative
    // (player-aircraft) world draws admitted, the level rig and its look rotation for setup_poses, the cockpit-seen hold.
    uint64_t povOpened=0,povOpenRefused=0,povCockpitPoisoned=0,povRelative=0,povRelativeRefused=0;const char* povLastWhy=nullptr;
    acz::povr::ProducerRig povRig;double povLookR[9]{};double povRigYaw=0;int64_t povCockpitHoldUntilUs=0;
    // POV-RENDER stages 3 + 4 (pov:3): opened frames per kind (chase, hud-only, cinematic), the cinematic anchor rig and its last event
    uint64_t povOpenedKind[3]{};acz::povr::ProducerAnchor povAnchor;const char* povAnchorLast=nullptr;
    uint64_t povOpenedAOnly=0;   // pov:3 A-only (escalated cutscene / replay) frames
    uint64_t wobjFirstFrame=0;unsigned wobjFirstMembers=0;double wobjFirstEye[3]{},wobjFirstModel[3]{},wobjFirstLens[2]{},wobjFirstOffset[2]{};
    // J-world-evidence: the liveSrt snapshot of the draw being decided is fresh only when THIS decision's read succeeded; the cb10 dword 0
    // the decision used (resolved from the root-constant witness at the Draw seam before the decision).
    bool wobjLiveFresh=false,wevDefsEmitted=false;const char* wobjReadStatus="not-read";gpu_copy::step5::world::Cb10 wobjCb10{};
    Step5Png png; // J-hud-colour (gpu_copy_step5_png_once)
    // J-compat-race: producer-dead watch (worker context, step-1 mutex).
    bool deadSeen=false;uint64_t deadSince=0;unsigned deadTicks=0,deadLines=0;
};
std::atomic<Step5State*> g_step5{nullptr};
// J-hud-colour: true only while a png_once sample is armed; l3Close checks it (one relaxed load) before
// calling step5PngClose, so the key OFF adds no call and no command.
std::atomic<bool> g_step5PngWanted{false};
// J-scene-end: true while a png-burst.now request waits to be served (set by the worker poll, cleared by the game thread when none is pending);
// the OM/barrier hooks check it (one relaxed load) before calling step5SceneEnd, so the key OFF adds no call and no command.
std::atomic<bool> g_step5SceneEndWanted{false};
// review 2026-11-01 F2 (READ-ONLY census, no behaviour change): the atlas group's SetDescriptorHeaps leaves the list's COMPUTE descriptor tables undefined.
// Count the Dispatch (slot 14, hooked only to count) and ExecuteIndirect calls the game records into a list AFTER an atlas group in that list. One list is
// tracked: the list of the last group, until its Reset. The calls are compared by interface pointer (an alias interface of the same list is not matched).
// Logged as the fx-puff-atlas-compute line (fx_puff_atlas.h format_compute_line) next to fx-puff-atlas.
std::atomic<uintptr_t> g_fxAtlasGroupList{0};
std::atomic<bool> g_fxAtlasListCounted{false};
std::atomic<int> g_fxAtlasDispatchHook{-1}; // -1 = not installed (step3 off), 0 = the patch failed, 1 = hooked
std::atomic<uint32_t> g_fxAtlasGroups{0},g_fxAtlasDispatchAll{0},g_fxAtlasDispatchAfter{0},g_fxAtlasIndirectAfter{0},g_fxAtlasListsAfter{0};
inline void fxAtlasGroupRecorded(void* list) noexcept {
    g_fxAtlasGroups.fetch_add(1,std::memory_order_relaxed);
    if(g_fxAtlasGroupList.exchange(reinterpret_cast<uintptr_t>(list),std::memory_order_relaxed)!=reinterpret_cast<uintptr_t>(list))
        g_fxAtlasListCounted.store(false,std::memory_order_relaxed);
}
inline void fxAtlasComputeRecorded(void* list,bool indirect) noexcept {
    if(!indirect)g_fxAtlasDispatchAll.fetch_add(1,std::memory_order_relaxed);
    if(reinterpret_cast<uintptr_t>(list)!=g_fxAtlasGroupList.load(std::memory_order_relaxed))return;
    (indirect?g_fxAtlasIndirectAfter:g_fxAtlasDispatchAfter).fetch_add(1,std::memory_order_relaxed);
    if(!g_fxAtlasListCounted.exchange(true,std::memory_order_relaxed))g_fxAtlasListsAfter.fetch_add(1,std::memory_order_relaxed);
}
inline void fxAtlasListReset(void* list) noexcept {
    uintptr_t expect=reinterpret_cast<uintptr_t>(list);
    (void)g_fxAtlasGroupList.compare_exchange_strong(expect,0,std::memory_order_relaxed);
}
// J-3b9c: every step-5 evidence line goes to the dedicated step5-producer.log (the record);
// `mirror` also sends it to the shared diagnostics log as before. Worker/owner contexts only.
struct Step5State;

uint64_t g_step5EpochCounter=0; // fence epochs handed out (owner context / worker)
void step5LogText(Step5State& s5,const char* text,bool mirror=true) noexcept {
    acz::feature_log::write(s5.log,text);if(mirror)step1Emit(text);
}
void step5Log(Step1State& st,Step5State& s5,bool mirror=true) noexcept {step5LogText(s5,st.text,mirror);}
bool step5Configured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_step5Test;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_continuous();
}

// J-171: the observer mutex protects only fixed POD copies. Never held across
// any other lock, formatting, feature_log call, or native call. Hook-state and
// producer locks may acquire it; the worker releases it before writing.
std::mutex g_hudMutex;
gpu_copy::hud::State g_hud;
#ifdef ACZ_VR_OFFLINE_TEST
bool g_hudTest=false;
#endif
bool hudConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_hudTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_hud_observe();
}
// Hook-state mutex held. Opaque routes only; get() does not retain a COM object.
gpu_copy::hud::Record hudSnapshot_locked(void* key,ListState& ls) noexcept {
    gpu_copy::hud::Record r;
    r.list=step1Route(key);
    if(auto* l=l3_list_locked(ls))r.generation=l->generation;
    r.pipeline=step1Route(ls.pso.get());
    auto& s=state();const auto p=s.pipelines.find(ls.pso.get());
    if(p!=s.pipelines.end()) {
        const auto& v=p->second;const auto& o=v.output;
        r.id=v.identity;r.matched=v.drawcpt_identity_matched;r.pso_known=o.known;
        std::memcpy(r.vs,v.hudVs.data(),sizeof r.vs);std::memcpy(r.ps,v.hudPs.data(),sizeof r.ps);
        r.depth_func=o.depth_func;r.depth_write=unsigned(o.depth_write);r.stencil=unsigned(o.stencil);
        r.blend=unsigned(o.blend0);r.mask=o.write_mask0;r.independent_blend=unsigned(o.independent_blend);
        r.stencil_read=o.stencil_read;r.stencil_write=o.stencil_write;r.front_func=o.front_func;r.back_func=o.back_func;r.front_pass=o.front_pass;
        if(o.known&&o.rt_count)r.format=o.rtv[0];
    }
    const auto& t=ls.step3;
    r.viewport_known=t.known&&!t.gap&&t.viewports==1;
    r.viewport[0]=t.vp0.TopLeftX;r.viewport[1]=t.vp0.TopLeftY;r.viewport[2]=t.vp0.Width;
    r.viewport[3]=t.vp0.Height;r.viewport[4]=t.vp0.MinDepth;r.viewport[5]=t.vp0.MaxDepth;
    if(t.omSet&&t.omCount&&!(t.omSingle&&t.omCount>1)) {
        for(const auto& v:s.step3Views)if(v.handle&&v.handle==t.rtv[0].ptr&&!v.depth) {
            r.rt=v.resource;r.rt_known=v.resource&&t.omProof&&t.copyEpoch==s.step3CopyEpoch&&t.rtvSerial[0]==v.serial;
            if(v.described)r.format=v.format;
            break;
        }
    }
    r.truncated=!r.rt_known||!r.format||!r.pso_known||!r.viewport_known||t.omCount!=1||t.gap;
    return r;
}
void hudObserveDraw(ID3D12GraphicsCommandList* self,UINT count,UINT instances,bool indexed) noexcept {
    if(!hudConfigured()||g_step1Phase.load(std::memory_order_relaxed)!=static_cast<int>(Step1Phase::continuous))return;
    gpu_copy::hud::Record r;
    {auto& s=state();std::lock_guard lock(s.mutex);auto it=s.states.find(listKey_locked(self));
     if(it==s.states.end())return;r=hudSnapshot_locked(it->first,it->second);}
    r.count=count;r.instances=instances;r.indexed=indexed;
    std::lock_guard lock(g_hudMutex);g_hud.draw(r);
}
void hudObserveTarget_locked(void* key,ListState& ls) noexcept {
    if(!hudConfigured())return;
    auto r=hudSnapshot_locked(key,ls);std::lock_guard lock(g_hudMutex);g_hud.target(r);
}
void hudObserveBarrier_locked(void* key,ListState& ls,const D3D12_RESOURCE_BARRIER& b) noexcept {
    if(!hudConfigured()||b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)return;
    auto r=hudSnapshot_locked(key,ls);r.rt=step1Route(b.Transition.pResource);r.rt_known=r.rt!=0;
    r.format=0; // the barrier resource need not be the currently bound RT
    for(const auto& v:state().step3Views)if(v.resource==r.rt&&v.described){r.format=v.format;break;}
    r.before=uint32_t(b.Transition.StateBefore);r.after=uint32_t(b.Transition.StateAfter);
    r.barrier_flags=uint32_t(b.Flags);r.subresource=b.Transition.Subresource;
    if(b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE||
       (b.Transition.Subresource!=0&&b.Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))r.truncated=true;
    std::lock_guard lock(g_hudMutex);g_hud.barrier(r);
}
void hudMember(const Step5Frame& f,uintptr_t scene,uint64_t ordinal,bool hudArm) noexcept {
    if(!hudConfigured())return;
    std::lock_guard lock(g_hudMutex);
    if(hudArm)g_hud.begin(f.list,f.generation,scene);
    if(g_hud.same(f.list,f.generation))g_hud.matched_member(ordinal);
}
void hudCancel(Step1Route list=0) noexcept {
    if(!hudConfigured())return;
    std::lock_guard lock(g_hudMutex);if(!list||g_hud.list==list)g_hud.active=false;
}
void hudClose(const Step5Frame& f,bool ok) noexcept {
    if(!hudConfigured())return;
    std::lock_guard lock(g_hudMutex);g_hud.close(f.list,f.generation,ok);
}
void hudDrain(acz::feature_log::Handle log) noexcept {
    if(!hudConfigured())return;
    gpu_copy::hud::Record r;char a[gpu_copy::hud::line_bytes],b[gpu_copy::hud::line_bytes];
    for(unsigned i=0;i<gpu_copy::hud::queue_records;++i) {
        {std::lock_guard lock(g_hudMutex);if(!g_hud.pop(r))break;}
        gpu_copy::hud::format(r,a,b);acz::feature_log::write(log,a);acz::feature_log::write(log,b);
    }
}

// ------------------------------------------------------------- J-hud-capture: HUD replay (default OFF)
// gpu_copy_step5_hud: every HUD-path function returns before touching any state when the key is
// off, so the native command stream, resources and log lines are those of the build without it.
#ifdef ACZ_VR_OFFLINE_TEST
bool g_hudReplayTest=false;
#endif
bool hudReplayConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_hudReplayTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_hud();
}
// J-hud-markers (gpu_copy_step5_hud_markers, default OFF; needs gpu_copy_step5_hud): with the key off no marker path touches any
// state, so the HUD command stream, resources and log lines are those of the build without it.
#ifdef ACZ_VR_OFFLINE_TEST
bool g_hudMarkersTest=false;
#endif
bool hudMarkersConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_hudReplayTest&&g_hudMarkersTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_hud()&&log->gpu_copy_step5_hud_markers();
}
// J-hud-radar (gpu_copy_step5_hud_radar, default OFF; needs gpu_copy_step5_hud): with the key off no radar path touches any state, so the
// HUD region latch, the replay command stream, the caps and the log lines are those of the build without it.
#ifdef ACZ_VR_OFFLINE_TEST
bool g_hudRadarTest=false;
#endif
bool hudRadarConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_hudReplayTest&&g_hudRadarTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_hud()&&log->gpu_copy_step5_hud_radar();
}
// J-effects-layer (gpu_copy_step5_effects, default OFF; needs gpu_copy_step5_continuous + gpu_copy_step5_hud): with the key off no
// effects path touches any state, so the command stream, resources and log lines are those of the build without it.
#ifdef ACZ_VR_OFFLINE_TEST
bool g_effectsTest=false;
#endif
bool effectsConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_hudReplayTest&&g_effectsTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_hud()&&log->gpu_copy_step5_effects();
}
// J-effects-dual (gpu_copy_step5_effects_dual, default OFF; latched only with the effects producer chain): effects candidates must be the game's exact
// dual-source tuple, are replayed with the private alpha-blend clone PSO and the published image is tagged coverage_v1. Key off: no clone is created,
// no SetPipelineState is recorded, no refusal category exists and the tag is legacy.
#ifdef ACZ_VR_OFFLINE_TEST
bool g_effectsDualTest=false;
#endif
bool effectsDualConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_hudReplayTest&&g_effectsTest&&g_effectsDualTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_hud()&&log->gpu_copy_step5_effects()&&log->gpu_copy_step5_effects_dual();
}
// J-test8-fixes xr_world_objects_depth_fix (default OFF; latched with gpu_copy_step5_world_objects): the replayed world objects carry a compressed
// depth scale (record +360, gpu_copy_step5_world.h depth_fix_scale). Key off (or a non-float depth format): the raw record bytes, as before.
#ifdef ACZ_VR_OFFLINE_TEST
bool g_worldDepthFixTest=false;
#endif
bool worldDepthFixConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_worldDepthFixTest;
#endif
    auto* log=diagnostics::sink();return log&&log->xr_world_objects_depth_fix();
}
// ------------------------------------------------------------- J-cockpit-fov keys (default OFF)
// Every F1/F2/F3 path returns before touching any state when its key is off, so the native
// command stream, resources and log lines are those of the build without it.
#ifdef ACZ_VR_OFFLINE_TEST
bool g_hmdFovTest=false,g_precockpitTest=false,g_byClassTest=false;
#endif
bool hmdFovConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_hmdFovTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_hmd_fov();
}
// ------------------------------------------------------------- xr_look_drive_keys (default OFF; look_drive_keys.h)
// Every path returns before touching any state when the key is off, so the poses, matrices and log lines are those of the build without it.
#ifdef ACZ_VR_OFFLINE_TEST
bool g_lookDriveTest=false,g_lookSweepTest=false,g_lookNoXrTest=false,g_lookXInputTest=false,g_lookClosedLoopTest=false,g_lookRefClusterTest=false;double g_leanLeverTest=0.0,g_leanScaleTest=1.0;double g_lookDriveClockMs=-1; // clock override (ms) for the fixtures
#endif
// The test sweep latched WITHOUT xr_stereo_output: no pose is read (the head is the scripted sweep), nothing is compensated or published.
bool lookNoXrConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_lookNoXrTest;
#endif
    auto* log=diagnostics::sink();return log&&log->xr_look_drive_no_xr();
}
bool lookDriveConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_lookDriveTest;
#endif
    auto* log=diagnostics::sink();return log&&log->xr_look_drive_keys();
}
bool lookSweepConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_lookSweepTest;
#endif
    auto* log=diagnostics::sink();return log&&log->xr_look_drive_test_sweep();
}
bool lookXInputConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_lookXInputTest;
#endif
    auto* log=diagnostics::sink();return log&&log->xr_look_drive_xinput();
}
bool lookClosedLoopConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_lookClosedLoopTest;
#endif
    auto* log=diagnostics::sink();return log&&log->xr_look_drive_closed_loop();
}
// POV fixes (docs/review/pov-fixes-impl.md): F-REF majority-cluster reference (needs the look drive) and F-LEAN (neck lever, translation scale).
bool lookRefClusterConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_lookRefClusterTest;
#endif
    auto* log=diagnostics::sink();return log&&log->xr_look_ref_cluster();
}
struct LeanCfg {
    acz::pov::Lean v;
    const acz::pov::Lean* get() const noexcept {return v.active()?&v:nullptr;} // nullptr = keys off: setup_poses runs its original expressions
};
LeanCfg leanCfg() noexcept {
    LeanCfg c;
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test){c.v=acz::pov::make_lean(g_leanLeverTest,g_leanScaleTest);return c;}
#endif
    if(auto* log=diagnostics::sink())c.v=acz::pov::make_lean(double(log->pov_neck_lever_m()),double(log->pov_translation_scale()));
    return c;
}
double lookDriveNowMs() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_lookDriveClockMs>=0)return g_lookDriveClockMs;
#endif
    return double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())/1000.0;
}
int64_t step5MonoUs() noexcept { // timing-anchor diagnostics: the same steady_clock domain the consumer stamps selection with
    return int64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
double unitsPerMetreConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return gpu_copy::step3::units_per_metre;
#endif
    auto* log=diagnostics::sink();return log?double(log->gpu_copy_step5_units_per_metre()):gpu_copy::step3::units_per_metre;
}
// WORLD-SCALE (world_scale.h): eye scale of the replayed WORLD objects and smoke = the world scale the XR proxy published from xr_terrain_stage
// (evidenced 5.0 u/m * wscale); unpublished (no terrain option) = unitsPerMetreConfigured(), the previous behaviour.
double worldUnitsPerMetreConfigured() noexcept {
    return acz::world_scale::producer_world_units_per_metre(unitsPerMetreConfigured());
}
bool precockpitConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_precockpitTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_precockpit_observe();
}
bool byClassConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_byClassTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_members_by_class();
}
// J-hud-colour (gpu_copy_step5_png_once, default OFF).
#ifdef ACZ_VR_OFFLINE_TEST
bool g_pngOnceTest=false;
#endif
bool pngOnceConfigured() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Test)return g_pngOnceTest;
#endif
    auto* log=diagnostics::sink();return log&&log->gpu_copy_step5_png_once();
}
// J-effects-content: the effects-burst threshold (committed effects draws per frame) from %LOCALAPPDATA%\acz-vr\png-effects.threshold, read ONCE at
// ring creation (owner context). Absent file -> the default (source "default"); a present but invalid file -> the default (source "file-invalid").
#ifdef ACZ_VR_OFFLINE_TEST
std::wstring g_pngFxDirTest; // fixtures: a temporary directory instead of %LOCALAPPDATA%\acz-vr
#endif
unsigned pngThresholdRead(const char*& source) noexcept {
    source="default";
    wchar_t path[MAX_PATH]{};
#ifdef ACZ_VR_OFFLINE_TEST
    if(!g_pngFxDirTest.empty())_snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\png-effects.threshold",g_pngFxDirTest.c_str());
    else
#endif
    {wchar_t local[MAX_PATH];const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
     if(!n||n>=MAX_PATH-48)return gpu_copy::png::threshold_default;
     _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\png-effects.threshold",local);}
    const HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return gpu_copy::png::threshold_default;
    char buf[32]{};DWORD got=0;const BOOL ok=ReadFile(h,buf,sizeof buf-1,&got,nullptr);CloseHandle(h);
    const unsigned v=ok?gpu_copy::png::parse_threshold(buf,got):0u;
    if(!v){source="file-invalid";return gpu_copy::png::threshold_default;}
    source="file";return v;
}
// F2 observer: the leaf mutex protects only the POD state (never held across another lock,
// formatting, feature_log or native calls). The draw-path scratch record is guarded by the
// step-1 mutex (its only writer, step5Draw, holds it).
std::mutex g_pcMutex;
gpu_copy::precockpit::State g_pc;
gpu_copy::precockpit::Record g_pcScratch;
// J-world-objects v2 step 1 observer: the leaf mutex protects only the POD state (same rules as g_pcMutex).
std::mutex g_woMutex;
gpu_copy::wobs::State g_wo;
// J-world-evidence (Astra reply 5): the leaf mutex protects only the POD state (same rules as g_woMutex).
std::mutex g_wevMutex;
gpu_copy::wev::State g_wev;
std::atomic<uint32_t> g_wevOrdinal{0}; // all-draw ordinal of the draw being decided (written at the Draw seam, read by its decision, same thread)
const char* listPointFailure_locked(const ListState& list,const capture::State& supplied) noexcept; // defined below this include (d3d12_hook.cpp)
void step5WorldEvidenceObserved(ID3D12GraphicsCommandList* self,const gpu_copy::wobs::Obs& o,const char* status,const uint8_t* bytes) noexcept; // below
void wevReset() noexcept { // offline fixtures only
    std::lock_guard lock(g_wevMutex);g_wev=gpu_copy::wev::State{};g_wevOrdinal.store(0);
}
// Root-signature diagnostics of the world observer: lock-free selection on the Draw seam (zero-initialised statics).
std::atomic<uint64_t> g_woRootKeys[gpu_copy::wobs::root_cap];
std::atomic<unsigned> g_woRootDraws[gpu_copy::wobs::root_cap];
std::atomic<unsigned> g_woRootStates{0};
void woRootDiagReset() noexcept { // offline fixtures only
    for(auto& k:g_woRootKeys)k.store(0);for(auto& d:g_woRootDraws)d.store(0);g_woRootStates.store(0);
}
// J-phase-observe: the leaf mutex protects only the POD state (same rules as g_woMutex).
std::mutex g_phMutex;
gpu_copy::phobs::State g_ph;
void hudRegionBegin(Step1Route list,uint64_t generation,uintptr_t scene,bool radar=false) noexcept {
    std::lock_guard lock(g_hudRegionMutex);g_hudRegion=Step5HudRegion{};
    g_hudRegion.active=scene!=0;g_hudRegion.list=list;g_hudRegion.generation=generation;g_hudRegion.scene=scene;g_hudRegion.radar=radar;
}
void hudRegionReopen() noexcept {
    std::lock_guard lock(g_hudRegionMutex);g_hudRegion.ended=false;g_hudRegion.why=nullptr;g_hudRegion.sceneEndTaken=false;
    if(g_hudRegion.radar){auto& r=g_hudRegion;r.away=r.window=r.excCopy=false;r.excursions=r.returns=0;r.firstAwayWhy=nullptr;r.excWhy=nullptr; // the region restarts after a later member
        r.sceneLeftAway=r.resumed=false;r.sceneLeaves=r.resumes=r.sceneBackRefused=0;}
}
// The region's end reason (nullptr: still open) and stop it.
const char* hudRegionStop(Step1Route list=0) noexcept {
    std::lock_guard lock(g_hudRegionMutex);auto& r=g_hudRegion;
    if(list&&r.list!=list)return nullptr;
    // RADAR-SCENE-RESUME: a region resumed after a scene barrier and not ended again still reports that barrier as its end (the end buckets keep their meaning).
    const char* why=r.active&&r.ended?r.why:(r.active&&r.radar&&r.resumed?"scene-barrier":nullptr);r.active=false;return why;
}
bool hudRegionEnded(Step1Route list,uint64_t generation) noexcept {
    std::lock_guard lock(g_hudRegionMutex);const auto& r=g_hudRegion;
    return !r.active||r.list!=list||r.generation!=generation||r.ended;
}
// J-effects-layer: the end reason of this frame's HUD region ("scene-barrier" = the scene target left RENDER_TARGET), else nullptr.
const char* hudRegionWhy(Step1Route list,uint64_t generation) noexcept {
    std::lock_guard lock(g_hudRegionMutex);const auto& r=g_hudRegion;
    return r.active&&r.list==list&&r.generation==generation&&r.ended?r.why:nullptr;
}
void hudRegionEnd(Step1Route list,uint64_t generation,uintptr_t resource,bool anyResource,const char* why,uint32_t before=0) noexcept {
    std::lock_guard lock(g_hudRegionMutex);auto& r=g_hudRegion;
    if(r.active&&!r.ended&&r.list==list&&r.generation==generation&&(anyResource?resource!=r.scene:resource==r.scene)){
        r.ended=true;r.why=why;r.endBefore=before;r.sceneEndTaken=r.radar&&(r.excCopy||r.resumes>0); // one scene-end copy per region lifetime (also across resumes)
        // RADAR-SCENE-RESUME: the scene leaves RENDER_TARGET during an excursion (the radar group's mid pass reads it): resumable by the barrier back.
        if(r.radar&&r.away&&!anyResource&&std::strcmp(why,"scene-barrier")==0){r.sceneLeftAway=true;++r.sceneLeaves;}
    }
}
// RADAR-SCENE-RESUME (barrier hook, radar key only): the scene target transitioned back INTO RENDER_TARGET after it left during an excursion. The region
// is open again, still AWAY: only a proven scene bind / scene draw returns, and every draw after it is late (window rules). Key off: never resumes.
bool hudRegionSceneBack(Step1Route list,uint64_t generation,uintptr_t resource,uint32_t flags,uint32_t subresource) noexcept {
    std::lock_guard lock(g_hudRegionMutex);auto& r=g_hudRegion;
    if(!(r.active&&r.radar&&r.ended&&r.sceneLeftAway&&r.list==list&&r.generation==generation&&resource==r.scene))return false;
    r.sceneLeftAway=false;
    if(gpu_copy::step5::radar_scene_back(flags,subresource,r.resumes)){++r.sceneBackRefused;return false;} // fail closed: the original end stands
    r.ended=false;r.why=nullptr;r.away=true;r.window=false;r.resumed=true;++r.resumes;return true;
}
// The region was resumed after a scene barrier (sticky until a later member): the effects layer treats it as closed by the scene barrier.
bool hudRegionResumed(Step1Route list,uint64_t generation) noexcept {
    std::lock_guard lock(g_hudRegionMutex);const auto& r=g_hudRegion;
    return r.active&&r.radar&&r.resumed&&r.list==list&&r.generation==generation;
}
// J-hud-radar. A PROVEN render target other than the scene (an OMSet bind or a draw on it): with the radar key an excursion (counted once per
// away/return cycle; more than hud_radar_excursion_cap per region end it), else the original end. Returns the first excursion's cause.
const char* hudRegionOtherTarget(Step1Route list,uint64_t generation,uintptr_t resource,const char* why) noexcept {
    std::lock_guard lock(g_hudRegionMutex);auto& r=g_hudRegion;
    if(!(r.active&&!r.ended&&r.list==list&&r.generation==generation&&resource!=r.scene))return nullptr;
    if(!r.radar){r.ended=true;r.why=why;return nullptr;}
    if(!r.away) {
        r.away=true;r.window=false;++r.excursions;if(!r.firstAwayWhy)r.firstAwayWhy=why;
        if(r.excursions==1&&std::strcmp(why,"rt-change")==0){r.excWhy=why;r.sceneEndTaken=false;} // J-scene-end: the first hooked excursion is the original region end
        if(r.excursions>gpu_copy::step5::hud_radar_excursion_cap){r.ended=true;r.why="excursion-cap";}
    }
    return r.firstAwayWhy;
}
// The scene target is bound / drawn on again.
void hudRegionSceneReturn(Step1Route list,uint64_t generation) noexcept {
    std::lock_guard lock(g_hudRegionMutex);auto& r=g_hudRegion;
    if(r.active&&r.radar&&!r.ended&&r.list==list&&r.generation==generation&&r.away){r.away=false;++r.returns;}
}
Step5RadarView hudRegionRadarView(Step1Route list,uint64_t generation) noexcept {
    std::lock_guard lock(g_hudRegionMutex);const auto& r=g_hudRegion;Step5RadarView v;
    if(!(r.active&&r.radar&&r.list==list&&r.generation==generation))return v;
    v.radar=true;v.away=r.away;v.late=r.excursions>0;v.window=r.window;v.excursions=r.excursions;v.returns=r.returns;v.firstWhy=r.firstAwayWhy;
    v.resumed=r.resumed;v.sceneLeaves=r.sceneLeaves;v.resumes=r.resumes;v.sceneBackRefused=r.sceneBackRefused;return v;
}
void hudRegionRadarWindow(Step1Route list,uint64_t generation,bool open) noexcept {
    std::lock_guard lock(g_hudRegionMutex);auto& r=g_hudRegion;
    if(r.active&&r.radar&&r.list==list&&r.generation==generation)r.window=open;
}
// OMSetRenderTargets hook (hook-state mutex held): a PROVEN RT 0 other than the scene target ends
// the frame's HUD region (the first render-target change after the last cockpit member). An
// unresolvable RT 0 does not end it; a draw under it is refused at the Draw seam (fail closed).
void hudReplayTarget_locked(void* key,ListState& ls) noexcept {
    if(!hudReplayConfigured())return;
    const auto& t=ls.step3;if(!t.omSet||!t.omCount)return;
    auto& s=state();uintptr_t rt=0;
    for(const auto& v:s.step3Views)if(v.handle&&v.handle==t.rtv[0].ptr&&!v.depth) {
        if(v.resource&&t.omProof&&t.copyEpoch==s.step3CopyEpoch&&t.rtvSerial[0]==v.serial)rt=uintptr_t(v.resource);
        break;
    }
    if(!rt)return;
    uint64_t generation=0;if(auto* l=l3_list_locked(ls))generation=l->generation;
    // J-hud-radar: with the key the bind of another target is an excursion and the bind of the scene target ends it (key off: the original end).
    if(rt==0)return;
    {const auto v=hudRegionRadarView(step1Route(key),generation);
     if(v.radar){
         bool scene=false;{std::lock_guard lock(g_hudRegionMutex);scene=rt==g_hudRegion.scene;}
         if(scene)hudRegionSceneReturn(step1Route(key),generation);else (void)hudRegionOtherTarget(step1Route(key),generation,rt,"rt-change");
         return;}}
    hudRegionEnd(step1Route(key),generation,rt,true,"rt-change");
}
// ResourceBarrier hook (hook-state mutex held): any transition of the scene target out of
// RENDER_TARGET (any subresource, split or not) ends the HUD region. With the radar key, the transition
// back INTO RENDER_TARGET after a scene barrier taken during an excursion resumes it (RADAR-SCENE-RESUME).
void hudReplayBarrier_locked(void* key,ListState& ls,const D3D12_RESOURCE_BARRIER& b) noexcept {
    if(!hudReplayConfigured()||b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)return;
    const bool before=(b.Transition.StateBefore&D3D12_RESOURCE_STATE_RENDER_TARGET)!=0,after=(b.Transition.StateAfter&D3D12_RESOURCE_STATE_RENDER_TARGET)!=0;
    if(before==after)return;
    if(after&&!hudRadarConfigured())return; // key off: exactly the original early return for a transition into RENDER_TARGET
    uint64_t generation=0;if(auto* l=l3_list_locked(ls))generation=l->generation;
    if(after){(void)hudRegionSceneBack(step1Route(key),generation,reinterpret_cast<uintptr_t>(b.Transition.pResource),uint32_t(b.Flags),b.Transition.Subresource);return;}
    hudRegionEnd(step1Route(key),generation,reinterpret_cast<uintptr_t>(b.Transition.pResource),false,"scene-barrier",uint32_t(b.Transition.StateBefore));
}

// ------------------------------------------------------------- slot metadata (meta held)
// J-3b9 (review P1): the CPU rewrites a slot's mapped upload blobs (and the GPU
// addresses are reused) when it records into it again. Same-queue ordering protects
// GPU work, NOT a CPU memcpy into memory a queued draw has yet to read. A slot is
// therefore reusable only when its previous submitted use has COMPLETED on the same
// private fence (completed >= lastUse; UINT64_MAX is loss, handled by the caller),
// and never while HELD by a consumer. `busy`: a candidate existed but the GPU lags.
// J-180 P1 (step-1 mutex held): a live owner (a submitted, not yet Reset/released recording) can still execute and write this slot.
bool step5OwnedSlot_locked(const Step5State& s5,unsigned i) noexcept {
    for(const auto& o:s5.owner)if(o.live&&o.slot==i)return true;
    return false;
}
unsigned step5OwnerLive_locked(const Step5State& s5) noexcept {
    unsigned n=0;for(const auto& o:s5.owner)n+=o.live;return n;
}
Step5Owner* step5OwnerFind_locked(Step5State& s5,Step1Route route) noexcept {
    if(!route)return nullptr;
    for(auto& o:s5.owner)if(o.live&&o.route==route)return &o;
    return nullptr;
}
// A successful Reset or the final Release of the list: the recording can never execute again.
void step5OwnerRetire_locked(Step5State& s5,Step1Route route) noexcept {
    if(!route)return;
    for(auto& o:s5.owner)if(o.live&&o.route==route){o=Step5Owner{};++s5.ownerRetired;}
}
// Called when a recording is SUBMITTED (the first Execute of its active frame). false = the table cannot represent it (the caller never
// reaches this: step5OwnerRoom_locked refuses the insertion at frame start).
bool step5OwnerAdd_locked(Step5State& s5,Step1Route route,uint64_t generation,unsigned slot,uint64_t value) noexcept {
    step5OwnerRetire_locked(s5,route); // a stale entry of a re-recorded list that never reported its Reset: replaced, never duplicated
    for(auto& o:s5.owner)if(!o.live) {
        o.live=true;o.route=route;o.generation=generation;o.slot=slot;o.lastUse=value;o.epoch=s5.fenceEpoch.load();o.executes=1;++s5.ownerAdded;return true;
    }
    return false;
}
// Frame start gate: room for one more owned recording (frame start refuses rather than evicting an executable writer).
bool step5OwnerRoom_locked(Step5State& s5) noexcept {
    if(step5OwnerLive_locked(s5)<step5_owner_cap)return true;
    ++s5.ownerRefused;return false;
}
int step5PickSlot_locked(Step5State& s5,uint64_t completed,bool& busy) noexcept {
    using namespace gpu_copy::step5;busy=false;
    // J-3b9c: a lastUse from another fence epoch is never compared with this fence's completion.
    const uint64_t epoch=s5.fenceEpoch.load();
    auto retired=[&](const Step5Slot& sl){return !sl.lastUse||(sl.lastUseEpoch==epoch&&sl.lastUse<=completed);};
    // J-180 P1: a slot written by a live owned recording is never picked, whatever its state or completion.
    for(unsigned i=0;i<slots;++i)if(s5.slot[i].state==slot_free) {
        if(step5OwnedSlot_locked(s5,i)){++s5.ownerBlocked;continue;}
        if(retired(s5.slot[i]))return int(i);
        busy=true;
    }
    // A published frame superseded by a newer one and not held (the consumer only
    // acquires the latest), once its replays completed on the GPU.
    for(unsigned i=0;i<slots;++i)if(s5.slot[i].state==slot_published&&s5.slot[i].frameId<s5.latestFrame) {
        if(step5OwnedSlot_locked(s5,i)){++s5.ownerBlocked;continue;}
        if(retired(s5.slot[i]))return int(i);
        busy=true;
    }
    return -1;
}
void step5Publish_locked(Step5State& s5,const Step5Frame& f,uint64_t value) noexcept {
    auto& sl=s5.slot[f.slot];
    // Never publish a value the producer has not itself signalled on the current fence.
    if(!value||value>s5.lastSignalled.load()||!s5.fenceEpoch.load()) {
        ++s5.publishRefused;if(sl.state==gpu_copy::step5::slot_producing)sl.state=gpu_copy::step5::slot_free;return;
    }
    // DECOUPLE review 3: a deferred publication (step5ApplyPending) never overwrites a quarantine recorded since the submission (a re-execution of
    // the recording, or a try-failed store): those images are being rendered again and are never published.
    if(sl.state.load()==gpu_copy::step5::slot_quarantined){++s5.publishRefused;return;}
    sl.pubUntracked=f.untrackedBase;
    sl.epoch=s5.fenceEpoch.load();
    sl.state=gpu_copy::step5::slot_published;sl.frameId=++s5.nextFrameId;sl.fenceValue=value;sl.predicted=f.predicted;
    sl.pose[0]=f.pose[0];sl.pose[1]=f.pose[1];sl.poseValid=f.poseValid;sl.partial=f.partial;sl.members=f.recorded;
    sl.worldIncomplete=f.worldIncomplete;sl.worldLost=f.wobjLost; // J-180 P2 (key off: always false / 0)
    sl.worldValid=f.worldValid&&sl.world;sl.lensValid=f.lensValid;sl.spaceGeneration=f.poseValid?f.spaceGeneration:0;for(int i=0;i<4;++i)sl.lens[i]=f.lens[i];
    // J-hud-capture: a frame without HUD capture (key off, unarmed) keeps the defaults (invalid, 0, 0).
    sl.hudValid=gpu_copy::step5::hud_frame_valid(f.hud.armed,f.hud.draws,f.hud.refused)&&sl.hudRt;sl.hudDraws=f.hud.draws;sl.hudRefused=f.hud.refused;
    // J-hud-markers: the marker layer is valid on its own (>= 1 marker draw, 0 marker refusals); instrument validity above is unaffected.
    sl.hudMarkersValid=f.hud.mSplit&&f.hud.mDraws>0&&f.hud.mRefused==0&&sl.hudMarkerRt;sl.hudMarkerDraws=f.hud.mDraws;sl.hudMarkerRefused=f.hud.mRefused;
    // J-effects-layer: published when >= 1 between-members draw was replayed (committed); refusals are counted only.
    sl.effValid=f.eff.armed&&gpu_copy::step5::effects_frame_valid(true,f.eff.ep.draws)&&sl.effRt;
    sl.effDraws=f.eff.armed?f.eff.ep.draws:0u;sl.effRefused=f.eff.armed?f.eff.ep.refused:0u;sl.effTail=f.eff.armed?f.eff.ep.tailDraws:0u;
    // EFFECTS-OUTSIDE (C) fxmem:4: the eye smoke images only when this frame cleared them and >= 1 replay executes (fx_smoke_gpu.h FrameState::valid).
    sl.smokeValid=f.eff.armed&&f.eff.sm.valid()&&sl.smokeRt[0]&&sl.smokeRt[1];sl.smokeDraws=f.eff.armed?f.eff.sm.committed:0u;sl.smokeRefused=f.eff.armed?f.eff.sm.refused:0u;
    // EFFECTS-OUTSIDE (F) fxmem:6: the eye sprite images under the same rule (fx_smoke_gpu.h FrameState::valid on the sprite replays).
    sl.spriteValid=f.eff.armed&&f.eff.sp.valid()&&sl.spriteRt[0]&&sl.spriteRt[1];sl.spriteDraws=f.eff.armed?f.eff.sp.committed:0u;sl.spriteRefused=f.eff.armed?f.eff.sp.refused:0u;
    // J-cockpit-fov F1: a frame without the HMD FOV (key off, fallback, no pose) keeps invalid + zeros.
    sl.cockpitFovValid=f.poseValid&&f.fovHmd;
    for(int e=0;e<2;++e)for(int i=0;i<4;++i)sl.cockpitFov[e][i]=sl.cockpitFovValid?f.fov[e][i]:0.0f;
    // J-quad-yaw: the neutral of THIS frame's generation, only with the pose path (else invalid + zeros).
    sl.neutralValid=f.poseValid&&f.neutral.valid;
    for(int k=0;k<3;++k)sl.neutralPos[k]=sl.neutralValid?f.neutral.pos[k]:0.0f;
    sl.neutralYaw=sl.neutralValid?f.neutral.yaw:0.0f;
    sl.lookValid=f.poseValid&&f.lookValid;sl.lookYaw=sl.lookValid?f.lookYaw:0.0f;sl.lookPitch=sl.lookValid?f.lookPitch:0.0f; // xr_look_drive_keys
    // Timing-anchor diagnostics: where the frame started, which pose snapshot and look it used, and when it was published.
    sl.diagStart=f.noCockpit?uint8_t(4):f.early?(f.adopted?uint8_t(2):uint8_t(3)):uint8_t(1);sl.noCockpit=f.noCockpit;sl.diagLookSrc=f.diagLookSrc;sl.diagLookMeasValid=f.diagLookMeasValid; // POV-RENDER stage 2: diagStart 4 = no-cockpit chase frame
    for(int k=0;k<2;++k){sl.diagLookMeas[k]=f.diagLookMeas[k];sl.diagLookEst[k]=f.diagLookEst[k];}
    sl.diagPoseSeq=f.poseValid?f.diagPoseSeq:0u;sl.diagGeneration=f.generation;sl.diagRefusal=f.worldWhy?f.worldWhy:f.stop;
    sl.diagStartUs=f.diagStartUs;sl.diagPublishUs=step5MonoUs();
    sl.gameCamValid=f.gameCamValid;sl.gameEyeValid=f.gameEyeValid;sl.e6Valid=f.e6Valid;sl.e6Seq=f.e6Seq; // WORLD-1
    sl.e6TNs=f.e6TNs;sl.e6TReadNs=f.e6TReadNs;sl.e6Lock=f.e6Lock;sl.e6Stage=f.e6Stage;sl.e6Tag=f.e6Tag; // CAM-EPOCH
    for(int k=0;k<9;++k)sl.gameCam[k]=f.gameCam[k];for(int k=0;k<3;++k)sl.gameEye[k]=f.gameEye[k];for(int k=0;k<7;++k)sl.e6[k]=f.e6[k];
    sl.interp=f.interp; // OBJECT-INTERP (key off: invalid)
    s5.latestFrame=sl.frameId;s5.published.fetch_add(1);
}
// Step-1 mutex held: apply a publication / frees the game thread could not.
void step5ApplyPending(Step5State& s5) noexcept {
    if(!s5.pending&&!s5.pendingFree)return;
    if(!s5.meta.try_lock())return;
    if(s5.pending){step5Publish_locked(s5,s5.pendingFrame,s5.pendingValue);s5.pending=false;}
    for(unsigned i=0;i<gpu_copy::step5::slots;++i)if(s5.pendingFree&(1u<<i)) {
        if(s5.slot[i].state==gpu_copy::step5::slot_producing)s5.slot[i].state=gpu_copy::step5::slot_free;
    }
    s5.pendingFree=0;
    s5.meta.unlock();
}
void step5SetSlot(Step5State& s5,unsigned i,int state) noexcept { // step-1 mutex held; try-only on meta
    if(state==gpu_copy::step5::slot_free){s5.pendingFree|=1u<<i;step5ApplyPending(s5);return;}
    // review 2026-11-01 F3: every quarantine path (step5Fault, a re-executed recording, a failed owner add) passes here under the step-1 mutex. The
    // atlas captures pending in that recording would never become ready nor dead: their cards are released to take a fresh tile (the tiles stay pinned).
    if(state==gpu_copy::step5::slot_quarantined)s5.fxTrail.atlas.slot_quarantined(i);
    if(s5.meta.try_lock()){s5.slot[i].state=state;s5.meta.unlock();}
    else s5.slot[i].state.store(state); // quarantine only ever moves away from producing; a racing reader sees a non-acquirable state
}
void step5Fault(Step1State& st,Step5State& s5,const char* why,bool quarantineSlot) noexcept { // step-1 mutex held
    hudCancel();
    if(hudReplayConfigured())(void)hudRegionStop();
    ++s5.ambiguous;++s5.failures;s5.lastFault=why;
    // J-hud-colour: copies recorded into an ambiguous frame may still execute into the readback:
    // the buffer is never reused (no further sample).
    if(s5.png.stage.load()==png_recorded&&s5.png.list==s5.frame.list&&s5.png.generation==s5.frame.generation)
        {s5.png.failWhy=why;s5.png.stage.store(png_failed);g_step5PngWanted.store(false);g_step5SceneEndWanted.store(false);}
    if(quarantineSlot)step5SetSlot(s5,s5.frame.slot,gpu_copy::step5::slot_quarantined);
    s5.frame.active=false;
    if(s5.failures>=gpu_copy::step5::fail_cap&&!s5.disabled){s5.disabled=true;s5.disableReason="3-consecutive-ambiguous-frames";}
    (void)st;
}
// ------------------------------------------------------------- FrameSource (consumer thread)
class Step5Source final:public acz::stereo::FrameSource {
public:
    bool try_acquire_latest(uint64_t after,acz::stereo::StereoFrame& out) override {
        auto* st=g_step1.load();auto* s5=g_step5.load();if(!st||!s5)return false;
        std::lock_guard lock(s5->meta);
        int best=-1;
        for(unsigned i=0;i<gpu_copy::step5::slots;++i) {
            const auto& sl=s5->slot[i];
            // DECOUPLE review 3 (Astra P2): a publication followed by an unexamined Execute is never acquired (its images may have been rewritten).
            if(sl.state==gpu_copy::step5::slot_published&&sl.frameId>after&&sl.epoch==s5->fenceEpoch.load()&&
               sl.fenceValue<=s5->lastSignalled.load()&&sl.pubUntracked==s5->untrackedExec.load()&&
               (best<0||sl.frameId>s5->slot[size_t(best)].frameId))best=int(i);
        }
        if(best<0)return false;
#ifdef ACZ_VR_OFFLINE_TEST
        if(g_step5AcquireSeamTest)g_step5AcquireSeamTest(unsigned(best));
#endif
        auto& sl=s5->slot[size_t(best)];
        // DECOUPLE review 3 (Astra P2): step5SetSlot stores QUARANTINED WITHOUT `meta` when its try_lock fails (a re-execution observed while we hold
        // it). The acquisition is therefore a compare-exchange from PUBLISHED: a quarantine stored after the selection above wins and nothing is
        // acquired (the next call selects again); one stored after this exchange turns HELD into QUARANTINED, which images_unchanged reports.
        int expected=gpu_copy::step5::slot_published;
        if(!sl.state.compare_exchange_strong(expected,gpu_copy::step5::slot_held)){++s5->acquireLostRace;return false;}
        out=acz::stereo::StereoFrame{};
        out.frame_id=sl.frameId;out.slot=unsigned(best);out.eye[0]=sl.rt[0];out.eye[1]=sl.rt[1];
        out.dxgi_format=s5->ringFormat;
        out.width=s5->gameW;out.height=s5->gameH;out.fence=st->fence.fence;out.fence_value=sl.fenceValue; // EYE-RING: the game frame size (= ringW/ringH without it)
        if(s5->eyeRing){out.eye_width=s5->ringW;out.eye_height=s5->ringH;} // EYE-RING: eyes, depth, smoke, sprite at the eye size
        out.pose[0]=sl.pose[0];out.pose[1]=sl.pose[1];out.pose_valid=sl.poseValid;out.predicted_display_time=sl.predicted;
        out.members=sl.members;out.partial=sl.partial;
        out.world_incomplete=sl.worldIncomplete;out.world_lost=sl.worldLost; // J-180 P2: eligible world draws of this frame that were not replayed
        // D-236: coverage depth (DEPTH_WRITE), world snapshot (RENDER_TARGET), lens.
        out.depth[0]=sl.ds[0];out.depth[1]=sl.ds[1];out.depth_format=sl.ds[0]?s5->ringDsTypeless:0; // the RESOURCE format (typeless, e.g. 19)
        out.world=sl.worldValid?sl.world:nullptr;out.world_valid=sl.worldValid&&sl.world;
        if(sl.lensValid)for(int i=0;i<4;++i)out.lens_tan[i]=sl.lens[i];
        out.lens_valid=sl.lensValid;
        // J-hud-capture: the private HUD layer (RENDER_TARGET, same fence) only when the frame replayed every HUD draw.
        out.hud=sl.hudValid?sl.hudRt:nullptr;out.hud_valid=sl.hudValid&&sl.hudRt;out.hud_format=sl.hudRt?s5->hudFormat:0;
        out.hud_draws=sl.hudDraws;out.hud_refused=sl.hudRefused;
        // J-hud-markers: the second private target (RENDER_TARGET, same fence) only when the frame replayed every marker draw.
        out.hud_markers=sl.hudMarkersValid?sl.hudMarkerRt:nullptr;out.hud_markers_valid=sl.hudMarkersValid&&sl.hudMarkerRt;
        out.hud_marker_draws=sl.hudMarkerDraws;out.hud_marker_refused=sl.hudMarkerRefused;
        // J-effects-layer: the third private target (RENDER_TARGET, same fence) only when a between-members draw was replayed.
        out.effects=sl.effValid?sl.effRt:nullptr;out.effects_valid=sl.effValid&&sl.effRt;
        out.effects_draws=sl.effDraws;out.effects_refused=sl.effRefused;out.effects_tail=sl.effTail;
        out.effects_repr=out.effects_valid&&effectsDualConfigured()?gpu_copy::step5::effects_repr_coverage_v1:gpu_copy::step5::effects_repr_legacy; // J-effects-dual
        // EFFECTS-OUTSIDE (C) fxmem:4: the per-eye smoke images (RENDER_TARGET, same fence, coverage_v1, eye size) only when the frame replayed smoke per eye.
        out.smoke_valid=sl.smokeValid&&sl.smokeRt[0]&&sl.smokeRt[1];out.smoke[0]=out.smoke_valid?sl.smokeRt[0]:nullptr;out.smoke[1]=out.smoke_valid?sl.smokeRt[1]:nullptr;
        out.smoke_format=out.smoke_valid?s5->hudFormat:0;out.smoke_draws=sl.smokeDraws;out.smoke_refused=sl.smokeRefused;
        // EFFECTS-OUTSIDE (F) fxmem:6: the per-eye sprite images (RENDER_TARGET, same fence, premultiplied light + coverage, eye size) only when replayed per eye.
        out.sprite_valid=sl.spriteValid&&sl.spriteRt[0]&&sl.spriteRt[1];out.sprite[0]=out.sprite_valid?sl.spriteRt[0]:nullptr;out.sprite[1]=out.sprite_valid?sl.spriteRt[1]:nullptr;
        out.sprite_format=out.sprite_valid?s5->hudFormat:0;out.sprite_draws=sl.spriteDraws;out.sprite_refused=sl.spriteRefused;
        // J-cockpit-fov F1: the FOV the eyes were rendered with (else invalid, zeros: use lens_tan).
        out.cockpit_fov_valid=sl.cockpitFovValid;
        if(sl.cockpitFovValid)for(int e=0;e<2;++e)for(int i=0;i<4;++i)out.cockpit_fov[e][i]=sl.cockpitFov[e][i];
        out.fence_epoch=sl.epoch;out.last_signalled=s5->lastSignalled.load();out.space_generation=sl.spaceGeneration;
        // J-quad-yaw: the seated-recentering neutral of this frame's space generation (else invalid, zeros).
        out.neutral_valid=sl.neutralValid;
        if(sl.neutralValid){for(int k=0;k<3;++k)out.neutral_pos[k]=sl.neutralPos[k];out.neutral_yaw=sl.neutralYaw;}
        out.look_valid=sl.lookValid;out.look_yaw=sl.lookYaw;out.look_pitch=sl.lookPitch; // xr_look_drive_keys (key off: false/0)
        out.tac=sl.tacValid?&s5->tacSlot[size_t(best)]:nullptr; // TACTICAL (key off: nullptr); stable while HELD
        out.interp=sl.interp; // OBJECT-INTERP (key off: invalid)
        out.diag_valid=true;out.diag_start=sl.diagStart;out.diag_look_src=sl.diagLookSrc;out.diag_look_meas_valid=sl.diagLookMeasValid;
        out.diag_look_meas_yaw=sl.diagLookMeas[0];out.diag_look_meas_pitch=sl.diagLookMeas[1];
        out.diag_look_est_yaw=sl.diagLookEst[0];out.diag_look_est_pitch=sl.diagLookEst[1];
        out.diag_pose_seq=sl.diagPoseSeq;out.diag_generation=sl.diagGeneration;out.diag_refusal=sl.diagRefusal;
        out.diag_start_us=sl.diagStartUs;out.diag_publish_us=sl.diagPublishUs; // timing-anchor diagnostics (never read by a decision)
        out.game_cam_valid=sl.gameCamValid;out.game_eye_valid=sl.gameEyeValid;out.e6_valid=sl.e6Valid;out.e6_seq=sl.e6Seq; // WORLD-1 terrain registration
        out.no_cockpit=sl.noCockpit; // POV-RENDER stage 2 (pov:2; key off: always false)
        out.e6_t_ns=sl.e6TNs;out.e6_t_read_ns=sl.e6TReadNs;out.e6_lock_epoch=sl.e6Lock;out.e6_stage_epoch=sl.e6Stage;out.e6_tag=sl.e6Tag; // CAM-EPOCH
        for(int k=0;k<3;++k){out.game_cam_right[k]=sl.gameCam[k];out.game_cam_up[k]=sl.gameCam[3+k];out.game_cam_fwd[k]=sl.gameCam[6+k];out.game_eye[k]=sl.gameEye[k];out.e6_campos[k]=sl.e6[k];}
        out.e6_pitch=sl.e6[3];out.e6_yaw=sl.e6[4];out.e6_roll=sl.e6[5];out.e6_heading=sl.e6[6];
        return true;
    }
    void release(const acz::stereo::StereoFrame& f) override {
        auto* s5=g_step5.load();if(!s5||f.slot>=gpu_copy::step5::slots)return;
        std::lock_guard lock(s5->meta);auto& sl=s5->slot[f.slot];
        // J-3b9c (review 2, P2): a frame of an older fence epoch is released into QUARANTINE, never
        // into the ring (its producer completion is no longer observable on the current fence).
        // DECOUPLE review 3: compare-exchange from HELD, so a try-failed quarantine store racing this release is never overwritten with FREE.
        if(sl.state==gpu_copy::step5::slot_held&&sl.frameId==f.frame_id) {
            int expected=gpu_copy::step5::slot_held;
            (void)sl.state.compare_exchange_strong(expected,sl.epoch==s5->fenceEpoch.load()&&sl.lastUseEpoch==s5->fenceEpoch.load()?
                gpu_copy::step5::slot_free:gpu_copy::step5::slot_quarantined);
        }
    }
    uint64_t frames_published() const override {auto* s5=g_step5.load();return s5?s5->published.load():0;}
    uint64_t frames_skipped() const override {auto* s5=g_step5.load();return s5?s5->skipped.load():0;}
    // DECOUPLE review 2/3 (stereo_frame_source.h): every write to a slot's images after its submission is a re-execution of its owning recording,
    // which step5BeforeExecute records BEFORE the native Execute by quarantining the slot (seq_cst state store; the acquisition is a
    // compare-exchange from PUBLISHED, so it never erases that store); an Execute it could not examine bumps untrackedExec first.
    // HELD + same frame + no unexamined Execute since the frame's SUBMISSION (pubUntracked) => no write submitted since publication.
    bool images_unchanged(const acz::stereo::StereoFrame& f) const override {
        auto* s5=g_step5.load();if(!s5||f.slot>=gpu_copy::step5::slots)return false;
        if(g_step1Phase.load()!=static_cast<int>(Step1Phase::continuous))return false;   // re-executions are only examined in the continuous phase
        std::lock_guard lock(s5->meta);const auto& sl=s5->slot[f.slot];
        return sl.state.load()==gpu_copy::step5::slot_held&&sl.frameId==f.frame_id&&sl.pubUntracked==s5->untrackedExec.load();
    }
};
Step5Source g_step5Source;
acz::stereo::FrameSource* step5FrameSource() noexcept {return g_step5.load()?&g_step5Source:nullptr;}
void step5SetPoseProvider(acz::stereo::PoseProvider* p) noexcept {g_step5Pose.store(p);}
// ------------------------------------------------------------- Draw seam (game thread)
// Per-eye blobs for the member whose live bytes are in s3.liveSrt (IPD fallback
// blobs already in inlineBlobs[1..2]); the pose path overwrites them. Heap only.
#ifdef ACZ_VR_OFFLINE_TEST
uint64_t g_step5PoseFailOrdinal=0; // fixture fault injection: pose substitution fails at this member ordinal
#endif
// J-3b9 (review P2): a member the pose path cannot express (non-affine, nonfinite)
// would be rendered without the pose the frame claims: the WHOLE frame is marked
// pose-broken and is never published (it still executes; its slot is retired by
// the fence). The member keeps the IPD blobs only so the recording stays sound.
__declspec(noinline) void step5PoseBlobs(Step1State& st,Step5State& s5) noexcept {
    if(!s5.frame.poseValid)return;
    bool fail=false;
#ifdef ACZ_VR_OFFLINE_TEST
    fail=g_step5PoseFailOrdinal&&st.s3.live.draw_ordinal==g_step5PoseFailOrdinal;
#endif
    for(unsigned e=0;e<2&&!fail;++e)
        fail=gpu_copy::step5::pose_substitute(st.s3.liveSrt.data(),s5.pf,e,st.s3.inlineBlobs[1+e].data(),s5.scratch)!=nullptr;
    if(fail) {
        s5.frame.poseBroken=true;
        gpu_copy::step3::DeltaFacts f;(void)gpu_copy::step3::delta_substitute(st.s3.liveSrt.data(),st.s3.inlineBlobs,f);
    }
}
// The first member of a frame: per-frame setup ONCE (camera, lens, poses, D_e, S).
// `adopt` (J-world-objects v2): the frame was opened earlier by a world draw, which already read the pose (f.pose, predicted,
// spaceGeneration) and rebased the neutral for that generation; only the cockpit camera, lens and the cockpit PoseFrame are
// established here, with the SAME pose. The world camera (s5.wobj) is kept.
// xr_look_drive_keys: configure the driver once (key on only).
void step5LookDriveEnsure(Step5State& s5) noexcept {
    if(lookDriveConfigured()&&!s5.lookDrive.active())s5.lookDrive.configure(lookSweepConfigured(),lookNoXrConfigured(),lookXInputConfigured(),lookClosedLoopConfigured(),lookRefClusterConfigured());
}
// F-REF: one cockpit member of the active frame joins the orientation clusters (its M256 is at liveSrt +256). Driver inactive: returns at once.
void step5RefMember(Step1State& st,Step5State& s5) noexcept {
    if(!s5.lookDrive.active())return;
    double m[16];gpu_copy::step5::load16(st.s3.liveSrt.data()+256,m);
    s5.lookDrive.ref_member(s5.frame.generation*1315423911ull^uint64_t(s5.frame.list),m);
}
__declspec(noinline) void step5FrameSetup(Step1State& st,Step5State& s5,bool adopt=false) noexcept {
    auto& f=s5.frame;if(!adopt){f.poseValid=false;f.predicted=0;f.diagStartUs=step5MonoUs();f.diagPoseSeq=0;}
    f.lensValid=false;f.neutral=gpu_copy::step5::NeutralExport{};
    if(!adopt){s5.wobj.camera=false;s5.wobj.failed=nullptr;f.gameEyeValid=false;} // J-world-objects: the world camera is recovered per frame from its first record
    const bool camera=!gpu_copy::step5::camera_frame(st.s3.liveSrt.data(),s5.pf,s5.scratch,s5.lens);
    if(camera){f.lensValid=true;for(int i=0;i<4;++i)f.lens[i]=s5.lens.tan[i];}
    // WORLD-1: the game camera this frame renders with (orientation only; the cockpit VP is camera-relative), published for the terrain.
    f.gameCamValid=camera;
    for(int k=0;k<3;++k){f.gameCam[k]=camera?float(s5.pf.cam.right[k]):0.0f;f.gameCam[3+k]=camera?float(s5.pf.cam.up[k]):0.0f;f.gameCam[6+k]=camera?float(s5.pf.cam.fwd[k]):0.0f;}
    // xr_look_drive_keys: the look the game rendered in THIS frame (camera basis vs the cockpit model), cancelled in the per-eye head rotation.
    const bool ld=lookDriveConfigured();const double ldNow=ld?lookDriveNowMs():0.0;const double* lookR=nullptr;
    if(ld) {
        step5LookDriveEnsure(s5);
        if(camera)s5.lookDrive.note_camera(s5.pf.cam.right,s5.pf.cam.up,s5.pf.cam.fwd,s5.scratch.m256,ldNow);else s5.lookDrive.note_no_camera();
        lookR=s5.lookDrive.compensation();
        f.lookValid=true;f.lookYaw=float(s5.lookDrive.comp_yaw());f.lookPitch=float(s5.lookDrive.comp_pitch()); // published: the world image is a view along it
        f.diagLookSrc=s5.lookDrive.comp_measured()?1:2;f.diagLookMeasValid=s5.lookDrive.measured().valid; // timing-anchor diagnostics
        f.diagLookMeas[0]=float(s5.lookDrive.measured().yaw);f.diagLookMeas[1]=float(s5.lookDrive.measured().pitch);
        f.diagLookEst[0]=float(s5.lookDrive.est_yaw_now());f.diagLookEst[1]=float(s5.lookDrive.est_pitch_now());
    } else f.lookValid=false;
    const bool noXr=ld&&lookNoXrConfigured();
    if(noXr)s5.lookDrive.advance_sweep(ldNow);
    auto* provider=g_step5Pose.load();
    if(adopt) {
        f.poseValid=false;
        if(!camera)s5.pf.valid=false,s5.pf.why=s5.lens.why;
        else if(!gpu_copy::step5::setup_poses_generation(f.pose,f.spaceGeneration,s5.neutral,s5.pf,s5.scratch,hmdFovConfigured(),unitsPerMetreConfigured(),lookR,leanCfg().get()))f.poseValid=true;
    } else {
    f.spaceGeneration=0;
    if(noXr) { // no-XR test sweep: no pose provider is used; the head is the scripted sweep (the controller reads it directly)
        std::memset(f.pose,0,sizeof f.pose);s5.pf.valid=false;s5.pf.why="look-drive-no-xr";
    } else if(provider&&provider->try_read_ex(f.pose,&f.predicted,&f.spaceGeneration)) {
        f.diagPoseSeq=provider->last_sequence(); // timing-anchor diagnostics
        if(ld)s5.lookDrive.sweep_poses(f.pose,ldNow); // test mode: the scripted head replaces the (static) null-HMD head
        if(!camera)s5.pf.valid=false,s5.pf.why=s5.lens.why;
        else if(!gpu_copy::step5::setup_poses_generation(f.pose,f.spaceGeneration,s5.neutral,s5.pf,s5.scratch,hmdFovConfigured(),unitsPerMetreConfigured(),lookR,leanCfg().get()))f.poseValid=true;
    } else {s5.pf.valid=false;s5.pf.why=provider?"pose-unavailable":"no-pose-provider";std::memset(f.pose,0,sizeof f.pose);}
    }
    if(ld)s5.lookDrive.on_frame(ldNow,f.poseValid||noXr,s5.scratch.R[0],s5.neutral.yaw0); // one key decision per game frame
    // J-quad-yaw: the neutral exported with this frame (setup_poses_generation already rebased it
    // for f.spaceGeneration; without the pose path: invalid).
    f.neutral=gpu_copy::step5::neutral_export(s5.neutral,f.poseValid);
    // J-cockpit-fov F1: what the eye matrices of this frame use (key off: never set).
    f.fovHmd=f.poseValid&&s5.pf.fovHmd;f.fovClamped=f.fovHmd&&s5.pf.fovClamped;f.fovWhy=f.poseValid?s5.pf.fovWhy:nullptr;
    if(f.fovHmd)for(int e=0;e<2;++e)for(int i=0;i<4;++i)f.fov[e][i]=s5.pf.fov[e].angle[i];
    if(!s5.firstCaptured) {
        // Once: the zero-offset control of the pose path must reproduce the raw
        // bytes exactly (the substitution with D_e forced to zero).
        s5.firstCaptured=true;s5.firstLive=st.s3.liveSrt;
        s5.controlPf=s5.pf;std::memset(s5.controlPf.D,0,sizeof s5.controlPf.D);s5.controlDiffer=999;
        if(camera&&!gpu_copy::step5::pose_substitute(st.s3.liveSrt.data(),s5.controlPf,0,s5.controlOut.data(),s5.scratch)) {
            s5.controlDiffer=0;for(size_t i=0;i<416;++i)s5.controlDiffer+=s5.controlOut[i]!=st.s3.liveSrt[i];
        }
        s5.firstPoseWhy=f.poseValid?"pose":s5.pf.why;
    }
    {   // WORLD-1: seam B also reads the E6 camera into the frame mailbox (terrain key only); a sample published DURING this call belongs to this frame.
        auto& fm=acz::terrain::frame_cam_mailbox();const uint64_t n0=fm.published();
        fm.set_tag(f.generation); // CAM-EPOCH: a sample published during this call is stamped with THIS recording generation (same thread as the publisher)
        ee_camera::notify_first_member(st.s3.liveSrt.data()); // J-w1-ee-camera seam B (one atomic load when OFF)
        acz::terrain::CamSample cs;
        f.e6Valid=fm.published()!=n0&&fm.try_read(cs);
        if(f.e6Valid){for(int k=0;k<3;++k)f.e6[k]=cs.campos[k];f.e6[3]=cs.pitch;f.e6[4]=cs.yaw;f.e6[5]=cs.roll;f.e6[6]=cs.heading;f.e6Seq=cs.seq;
                      f.e6TNs=cs.t_ns;f.e6TReadNs=cs.t_read_ns;f.e6Lock=cs.lock_epoch;f.e6Stage=cs.stage_epoch;f.e6Tag=cs.tag;}
        else {f.e6TNs=0;f.e6TReadNs=0;f.e6Lock=0;f.e6Stage=0;f.e6Tag=0;}
    }
}
// J-world-hold diagnostics (step-1 mutex held, after the frame setup): classify a world refusal and record the pitch proxies. No decision
// depends on it. head pitch = elevation of the published eye-0 pose (recentring is yaw-only, so it is also the pitch relative to neutral);
// game look = the recovered cockpit camera's forward axis (all three components: the game's up axis is not established).
__declspec(noinline) void step5NoteWorldPitch(Step5State& s5,const Step5Frame& f,const gpu_copy::step3::Shape& live,int pick) noexcept {
    double headDeg=0;const bool head=f.poseValid&&gpu_copy::step5::head_pitch_deg(f.pose[0].orientation,headDeg);
    const bool cam=f.lensValid;const double* fwd=s5.pf.cam.fwd;
    if(f.worldValid){s5.pitchAccepted.add(head,headDeg,cam,fwd);return;}
    const char* why=f.worldWhy?f.worldWhy:"world-texture-missing";
    (void)pick;
    s5.pitchRefused.add(head,headDeg,cam,fwd);
    const bool entry=std::strcmp(why,"entry-state-unknown")==0;
    unsigned sub=0;
    if(entry){sub=gpu_copy::step5::entry_sub_index(live.world_entry_sub);++s5.worldEntrySub[sub];++s5.worldEntryTotal;s5.pitchEntry.add(head,headDeg,cam,fwd);}
    const uint64_t frameNo=s5.nextFrameId+1;
    const bool burst=!s5.worldLastRefusedFrame||s5.worldLastRefusedFrame+1<frameNo; // frame ids are per PUBLISHED frame; a gap = a new burst
    if(burst)++s5.worldBursts;
    s5.worldLastRefusedFrame=frameNo;
    if(s5.refDetailCount<8||(burst&&s5.refDetailCount<gpu_copy::step5::refusal_detail_cap)) {
        auto& d=s5.refDetail[s5.refDetailCount++];
        d=gpu_copy::step5::RefusalDetail{};
        d.frame=frameNo;d.ordinal=live.draw_ordinal;d.generation=live.generation;d.sub=sub;d.entry=entry;d.why=why;
        d.head=head;d.headDeg=headDeg;d.cam=cam;if(cam)for(int k=0;k<3;++k)d.fwd[k]=fwd[k];
    } else ++s5.worldRefDetailDropped;
}
// EYE-RING: counts the eye-ring RS refusals (summary-eye-ring rs-refused); returns `why` unchanged. Step-1 mutex held.
inline const char* step5EyeRsNote(Step5State& s5,const char* why) noexcept {if(why&&s5.eyeRing&&std::strncmp(why,"eye-ring-",9)==0)++s5.eyeRsRefused;return why;}
// The member's replay state (pure; step-1 mutex held). nullptr or the refusal.
__declspec(noinline) const char* step5StateWhy(const Step1State& st,const Step5State& s5,const gpu_copy::step3::Shape& live) noexcept {
    using namespace gpu_copy::step3;
    Targets now;
    if(const char* why=decide(live,now))return why;
    if(now.width!=s5.gameW||now.height!=s5.gameH)return "viewport-size-changed"; // EYE-RING: the GAME size (= ringW/ringH without the eye ring)
    if(live.pso.rtv[0]!=s5.ringFormat)return "rt-format-changed";
    if(live.pso.dsv!=s5.ringDsv)return "ds-format-changed";
    if(now.depth_clear!=s5.ringClear)return "depth-clear-changed";
    // J-180: a DrawACM_v draw resolved through its own VERTEX-stage binding (a root that also carries the cockpit's GEOMETRY-stage t10 elsewhere) names
    // another root parameter than the cockpit's by construction; the draw's t10 is still validated by decide() (known, root value equal to the VA).
    if(!live.t10_vs&&live.t10_parameter!=st.s3.shape.t10_parameter)return "t10-parameter-changed";
    if(live.pso.stencil!=st.s3.shape.pso.stencil)return "stencil-state-changed";
    // EYE-RING (eye_ring.h): the replay rewrites the viewport/scissor and must set the game's back exactly: one viewport, one scissor, tracked.
    if(s5.eyeRing)if(const char* why=::acz::eyering::rs_restorable(live.rs_known,live.rs_gap,live.viewports,live.scissors))return why;
    return nullptr;
}
// J-cockpit-fov F3: the class verdict of a draw for membership by class (pure).
//  * an EXACT selected class -> nullptr, index = that class;
//  * the same FAMILY as a selected class -> nullptr, index = -1-k (k = that class): equal root
//    signature, topology, IB/VB strides, t10 parameter, RTV/DSV formats, sample count/quality and
//    stencil enable; only the depth-test facts (enable, write, function) and the PSO's strip-cut
//    value may differ. These facts belong to the GAME's own PSO, which the replay keeps bound;
//    the replay-relevant state (formats, viewport, depth clear, t10, stencil) is checked by
//    step5StateWhy before this (live: 235186659d0 at ordinal 23, same printed facts as the
//    cockpit class, refused before as signature-class-differs);
//  * otherwise out-of-class-<first differing fact against selected class 0>.
const char* step5ClassVerdict(const Step5State& s5,const Step5Class& c,int& index) noexcept {
    index=0;
    for(unsigned i=0;i<s5.classCount;++i)if(step5SameClass(s5.classes[i],c)){index=int(i);return nullptr;}
    auto family=[&](const Step5Class& a){return a.root==c.root&&a.topology==c.topology&&a.ib==c.ib&&a.vb==c.vb&&a.t10==c.t10&&a.rtv==c.rtv&&
        a.dsv==c.dsv&&a.samples==c.samples&&a.quality==c.quality&&a.stencil==c.stencil;};
    for(unsigned i=0;i<s5.classCount;++i)if(family(s5.classes[i])){index=-1-int(i);return nullptr;}
    index=INT_MIN;
    if(!s5.classCount)return "out-of-class-no-selected-class";
    const Step5Class& a=s5.classes[0];
    if(a.root!=c.root)return "out-of-class-root-signature";
    if(a.topology!=c.topology)return "out-of-class-topology";
    if(a.ib!=c.ib)return "out-of-class-ib-stride";
    if(a.vb!=c.vb)return "out-of-class-vb-stride";
    if(a.t10!=c.t10)return "out-of-class-t10-parameter";
    if(a.rtv!=c.rtv||a.dsv!=c.dsv)return "out-of-class-format";
    if(a.samples!=c.samples||a.quality!=c.quality)return "out-of-class-samples";
    return "out-of-class-stencil";
}
// F3: a family class seen for the first time is recorded once (worker logs it). Step-1 mutex held.
void step5NoteFamily(Step5State& s5,const Step5Class& c,const gpu_copy::step3::Shape& live,int index) noexcept {
    for(unsigned i=0;i<s5.familyCount;++i)if(step5SameClass(s5.family[i].c,c))return;
    if(s5.familyCount>=s5.family.size()){++s5.classUnlogged;return;}
    auto& a=s5.family[s5.familyCount++];a.c=c;a.pso=live.pso_route;a.ordinal=live.draw_ordinal;a.frame=s5.nextFrameId+1;a.nearest=unsigned(-1-index);
}
// The member's required state (step-1 mutex held). nullptr or the refusal.
__declspec(noinline) const char* step5MemberWhy(const Step1State& st,Step5State& s5,const gpu_copy::step3::Shape& live) noexcept {
    if(const char* why=step5EyeRsNote(s5,step5StateWhy(st,s5,live)))return why;
    // Signature class (root signature, topology, IB/VB strides, t10 parameter, strip cut,
    // output formats, sample count, depth/stencil state class) of a selected member.
    const Step5Class c=step5ClassOf(live);
    if(byClassConfigured()) {
        // F3: membership by class family; the PSO object and the ordinal are not part of it.
        int index=0;if(const char* why=step5ClassVerdict(s5,c,index))return why;
        if(index<0)step5NoteFamily(s5,c,live,index);
        return nullptr;
    }
    bool cls=false;for(unsigned i=0;i<s5.classCount&&!cls;++i)cls=step5SameClass(s5.classes[i],c);
    if(!cls)return "signature-class-differs";
    bool alt=false;for(unsigned i=0;i<s5.altCount&&!alt;++i)alt=s5.altPso[i]==live.pso_route;
    if(alt)return nullptr;
    // J-3b9c run-time admission: a PSO not seen at selection is admitted when EVERY class
    // fact equals the class of the selected member AT THE SAME ORDINAL (the PSO object is
    // the only difference). Capped (16); each admission is logged once by the worker.
    const gpu_copy::step3::Shape* sel=nullptr;
    for(unsigned i=0;i<st.s3.memberCount&&!sel;++i)if(st.s3.members[i].shape.draw_ordinal==live.draw_ordinal)sel=&st.s3.members[i].shape;
    if(!sel||!step5SameClass(step5ClassOf(*sel),c))return "pso-not-in-alternate-set";
    if(s5.altCount>=s5.altPso.size()){++s5.admitRefusedFull;return "alternate-set-full";}
    s5.altPso[s5.altCount++]=live.pso_route;
    if(s5.admitCount<s5.admits.size()){auto& a=s5.admits[s5.admitCount++];a.pso=live.pso_route;a.ordinal=live.draw_ordinal;a.frame=s5.nextFrameId+1;}
    return nullptr;
}
// Worker/owner context, step-1 mutex held: classes of the selected members and the
// PSOs observed (visible candidates) at the selected ordinals with such a class.
void step5BuildClasses(Step1State& st,Step5State& s5) noexcept {
    const auto& s3=st.s3;s5.classCount=0;s5.altCount=0;
    auto classOf=[](const gpu_copy::step3::Shape& m){return step5ClassOf(m);};
    auto same=[](const Step5Class& a,const Step5Class& b){return step5SameClass(a,b);};
    auto addAlt=[&](uint64_t pso){for(unsigned i=0;i<s5.altCount;++i)if(s5.altPso[i]==pso)return;if(s5.altCount<s5.altPso.size())s5.altPso[s5.altCount++]=pso;};
    for(unsigned m=0;m<s3.memberCount;++m) {
        const auto c=classOf(s3.members[m].shape);bool have=false;
        for(unsigned i=0;i<s5.classCount;++i)have=have||same(s5.classes[i],c);
        if(!have&&s5.classCount<s5.classes.size())s5.classes[s5.classCount++]=c;
        addAlt(s3.members[m].shape.pso_route);
    }
    for(const auto& v:s3.visibleCands) {
        bool selectedOrdinal=false;for(unsigned m=0;m<s3.memberCount;++m)selectedOrdinal=selectedOrdinal||s3.members[m].shape.draw_ordinal==v.shape.draw_ordinal;
        if(!selectedOrdinal)continue;
        const auto c=classOf(v.shape);bool ok=false;for(unsigned i=0;i<s5.classCount;++i)ok=ok||same(s5.classes[i],c);
        if(ok)addAlt(v.shape.pso_route);
    }
}
void step5EmitClasses(Step1State& st,Step5State& s5) noexcept {
    int n=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=member-classes;classes=%u;alternate-psos=%u;rule=class+alternate-pso,live-args",
        s5.classCount,s5.altCount);
    for(unsigned i=0;i<s5.classCount&&n>0&&size_t(n)<sizeof st.text-96;++i){const auto& c=s5.classes[i];
        n+=std::snprintf(st.text+n,sizeof st.text-size_t(n),";c%u=root:%llx,topo:%u,ib:%u,vb:%u,t10:%u,rtv:%u,dsv:%u",i,
            static_cast<unsigned long long>(c.root),c.topology,c.ib,c.vb,c.t10,c.rtv,c.dsv);}
    for(unsigned i=0;i<s5.altCount&&n>0&&size_t(n)<sizeof st.text-24;++i)
        n+=std::snprintf(st.text+n,sizeof st.text-size_t(n),i?",%llx":";psos=%llx",static_cast<unsigned long long>(s5.altPso[i]));
    step5Log(st,s5);
}
// Compare a live member with the selected set (same ordinal). Fills `d` once.
__declspec(noinline) bool step5Diff(const Step1State& st,const gpu_copy::step3::Shape& live,const char* refusal,Step5Detail& d) noexcept {
    const auto& s3=st.s3;const gpu_copy::step3::Shape* sel=nullptr;
    for(unsigned i=0;i<s3.memberCount;++i)if(s3.members[i].shape.draw_ordinal==live.draw_ordinal){sel=&s3.members[i].shape;break;}
    const char* what=refusal;
    if(!what) {
        if(!sel)what="extra-ordinal";
        else if(sel->pso_route!=live.pso_route)what="pso-changed";
        else if(sel->indexed!=live.indexed||sel->count!=live.count||sel->instances!=live.instances||sel->start!=live.start||
                sel->base!=live.base||sel->start_instance!=live.start_instance)what="draw-args-changed";
    }
    if(!what)return false;
    if(!d.set) {
        d=Step5Detail{};d.set=true;d.what=what;d.ordinal=live.draw_ordinal;d.psoLive=live.pso_route;d.list=live.list;d.gen=live.generation;
        d.liveIndexed=live.indexed;d.liveArgs[0]=live.count;d.liveArgs[1]=live.instances;d.liveArgs[2]=live.start;
        d.liveArgs[3]=uint32_t(live.base);d.liveArgs[4]=live.start_instance;d.selected=s3.memberCount;
        d.root=live.root_route;d.topology=live.topology;d.ib=live.ib_stride;d.vb=live.vb_stride;d.t10=live.t10_parameter;
        if(sel){d.selFound=true;d.psoSel=sel->pso_route;d.selIndexed=sel->indexed;d.selArgs[0]=sel->count;d.selArgs[1]=sel->instances;
            d.selArgs[2]=sel->start;d.selArgs[3]=uint32_t(sel->base);d.selArgs[4]=sel->start_instance;}
    }
    return true;
}
// World snapshot rules at the first member (step-1 mutex held). nullptr or why.
__declspec(noinline) const char* step5WorldWhy(const Step5State& s5,const gpu_copy::step3::Shape& live) noexcept {
    if(!s5.worldReady)return s5.worldArmWhy?s5.worldArmWhy:"world-unavailable";
    if(live.om_count!=1)return "game-rtv-count-not-1";
    if(std::strcmp(live.game_rt_status,"resolved")!=0)return "game-rt-unresolved";
    if(!live.game_rt||live.game_rt!=s5.worldSource)return "game-rt-resource-changed";
    if(live.game_rt_width!=s5.worldW||live.game_rt_height!=s5.worldH||live.game_rt_format!=s5.worldFormat)return "game-rt-desc-changed";
    if(!live.game_rt_view_sub0)return "game-rtv-not-subresource-0";
    if(live.game_rt_dim!=D3D12_RESOURCE_DIMENSION_TEXTURE2D)return "game-rt-not-texture2d";
    if(live.game_rt_samples!=1)return "game-rt-msaa";
    if(live.game_rt_flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)return "game-rt-simultaneous-access";
    if(live.game_rt_heap!=D3D12_HEAP_TYPE_DEFAULT)return "game-rt-not-default-heap";
    // J-3b9 (review): state evidence for the resource in THIS list generation. Refused on any
    // barrier the proxy cannot fully account for: untracked seams, enhanced barriers, split
    // (BEGIN/END_ONLY), a subresource other than ALL/0, aliasing or UAV barriers, or a last
    // tracked transition that does not end in RENDER_TARGET.
    if(!live.world_lifetime_covered)return "world-texture-lifetime-uncovered";
    if(!live.world_barriers_tracked)return "barrier-tracking-unavailable";
    if(live.world_enhanced)return "enhanced-barrier-in-list";
    if(live.world_barrier_why)return live.world_barrier_why;
    // J-3b9c (review 2, P2): a SUPPORTED entry state is required: a tracked transition TO
    // RENDER_TARGET earlier in this generation. Carried state is diagnostic only:
    // a later submission could invalidate it after recording. A bound RTV is not evidence.
    if(live.world_rt_transition)return live.world_rt_after==D3D12_RESOURCE_STATE_RENDER_TARGET?nullptr:"last-transition-not-render-target";
    return "entry-state-unknown"; // J-155: refuse BEFORE recording any game transition/copy
}
using Step5BarrierFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
using Step5CopyTexFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,const D3D12_TEXTURE_COPY_LOCATION*,UINT,UINT,UINT,
                                               const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*);
// 4 barriers (one per call) + 1 copy; the game RT ends in RENDER_TARGET again.
__declspec(noinline) void step5RecordWorld(ID3D12GraphicsCommandList* self,ID3D12Resource* game,ID3D12Resource* world) noexcept {
    const auto barrier=reinterpret_cast<Step5BarrierFn>(step1Native(self,26));
    const auto copy=reinterpret_cast<Step5CopyTexFn>(step1Native(self,16));
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Flags=D3D12_RESOURCE_BARRIER_FLAG_NONE;b.Transition.Subresource=0;
    auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        b.Transition.pResource=r;b.Transition.StateBefore=before;b.Transition.StateAfter=after;barrier(self,1,&b);
    };
    transition(game,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(world,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{},src{};
    dst.pResource=world;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=0;
    src.pResource=game;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=0;
    copy(self,&dst,0,0,0,&src,nullptr);
    transition(game,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(world,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
}
constexpr unsigned step5WorldCommands=5;
// What recording a member does besides its own replay (J-world-objects v2, plan step 3). The first member of a frame
// does all three; every other member none. They are separate so a frame that is opened by something other than its
// first cockpit member (the pending-frame state machine of the plan) can clear, take the world snapshot and arm the
// HUD region at different members. Today every caller passes `first_member(first)`: identical recordings.
struct MemberPhase {
    bool clear=false,    // ClearRTV x2, ClearDSV x2 of the slot's eye targets
         snapshot=false, // the world snapshot of the game's scene RT (only with a valid world)
         hudArm=false;   // arm the frame's HUD region / restart bookkeeping
    static constexpr MemberPhase first_member(bool first) noexcept {return MemberPhase{first,first,first};}
};
using Step5OmFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
using Step5VpFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*);   // EYE-RING: native RSSetViewports (vtable 21)
using Step5ScFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*);       // EYE-RING: native RSSetScissorRects (vtable 22)
// EYE-RING (eye_ring.h): the eye viewport/scissor of one per-eye replay (the game's scaled by eye / game) and the game's own to set back after it.
// nullptr = filled; else the refusal (the game's RS state is not exactly restorable: the replay must not be recorded on the eye ring).
struct Step5EyeRs {bool on=false;D3D12_VIEWPORT eyeVp{},gameVp{};D3D12_RECT eyeSc{},gameSc{};};
inline const char* step5EyeRsPlan(const Step5State& s5,const gpu_copy::step3::Shape& d,Step5EyeRs& out) noexcept {
    namespace er=::acz::eyering;
    out=Step5EyeRs{};
    if(!s5.eyeRing)return nullptr;
    if(const char* why=er::rs_restorable(d.rs_known,d.rs_gap,d.viewports,d.scissors))return why;
    const er::Viewport v=er::scale_viewport({d.vp0.x,d.vp0.y,d.vp0.w,d.vp0.h,d.vp0.min,d.vp0.max},s5.gameW,s5.gameH,s5.ringW,s5.ringH);
    const er::Rect r=er::scale_scissor({d.sc0.l,d.sc0.t,d.sc0.r,d.sc0.b},s5.gameW,s5.gameH,s5.ringW,s5.ringH);
    out.on=true;out.eyeVp={v.x,v.y,v.w,v.h,v.min_depth,v.max_depth};out.eyeSc={LONG(r.l),LONG(r.t),LONG(r.r),LONG(r.b)};
    out.gameVp={d.vp0.x,d.vp0.y,d.vp0.w,d.vp0.h,d.vp0.min,d.vp0.max};out.gameSc={LONG(d.sc0.l),LONG(d.sc0.t),LONG(d.sc0.r),LONG(d.sc0.b)};
    return nullptr;
}
inline void step5EyeRsSet(ID3D12GraphicsCommandList* self,const Step5EyeRs& r,bool eye) noexcept {
    if(!r.on)return;
    reinterpret_cast<Step5VpFn>(step1Native(self,21))(self,1,eye?&r.eyeVp:&r.gameVp);
    reinterpret_cast<Step5ScFn>(step1Native(self,22))(self,1,eye?&r.eyeSc:&r.gameSc);
}
// One member into the frame's slot: [one-time clears], L {OMSet,SRV,Draw}, R {OMSet,SRV,Draw}, restore {SRV,OMSet}.
__declspec(noinline) void step5RecordMember(ID3D12GraphicsCommandList* self,Step1State& st,Step5State& s5,unsigned slot,unsigned idx,MemberPhase phase) noexcept {
    using namespace gpu_copy::step3;
    using ClearRtvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT*,UINT,const D3D12_RECT*);
    using ClearDsvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*);
    auto& s3=st.s3;const auto& t=s3.targets;const auto& d=s3.live;const auto& sl=s5.slot[slot];
    const auto om=reinterpret_cast<Step5OmFn>(step1Native(self,46));
    const auto srv=reinterpret_cast<Step3SrvFn>(step1Native(self,40));
    void* const draw=step1Native(self,d.indexed?13:12);
    uint8_t* up=s5.uploadPtr+sl.uploadOffset+uint64_t(2*idx)*blob_stride;
    std::memcpy(up,s3.inlineBlobs[1].data(),size_t(blob_bytes));std::memcpy(up+blob_stride,s3.inlineBlobs[2].data(),size_t(blob_bytes));
    std::atomic_thread_fence(std::memory_order_release);
    if(phase.snapshot&&s5.frame.worldValid)step5RecordWorld(self,reinterpret_cast<ID3D12Resource*>(d.game_rt),sl.world);
    if(phase.clear) {
        const auto clearRtv=reinterpret_cast<ClearRtvFn>(step1Native(self,48));
        const auto clearDsv=reinterpret_cast<ClearDsvFn>(step1Native(self,47));
        static constexpr FLOAT clear[4]={0,0,0,0};
        const D3D12_CLEAR_FLAGS flags=D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH|(t.depth.stencil?D3D12_CLEAR_FLAG_STENCIL:0));
        clearRtv(self,sl.rtv[0],clear,0,nullptr);clearRtv(self,sl.rtv[1],clear,0,nullptr);
        if(t.has_depth){clearDsv(self,sl.dsv[0],flags,t.depth_clear,0,0,nullptr);clearDsv(self,sl.dsv[1],flags,t.depth_clear,0,0,nullptr);}
    }
    const D3D12_GPU_VIRTUAL_ADDRESS va=s5.uploadVa+sl.uploadOffset+uint64_t(2*idx)*blob_stride;
    // EYE-RING (eye_ring.h): the eye targets are ringW x ringH, the game's viewport/scissor were set for gameW x gameH: both scaled for the two eye draws,
    // the game's own set back before its draw follows. step5StateWhy proved exactly one viewport and one scissor (tracked). Off: no call at all.
    Step5EyeRs rs;
    if(s5.eyeRing&&!step5EyeRsPlan(s5,d,rs))step5EyeRsSet(self,rs,true);   // rs.on stays false when refused (cannot happen: step5StateWhy checked it)
    for(unsigned eye=0;eye<2;++eye) {
        om(self,1,&sl.rtv[eye],FALSE,t.has_depth?&sl.dsv[eye]:nullptr);
        srv(self,s3.t10,va+uint64_t(eye)*blob_stride);
        if(d.indexed)reinterpret_cast<Step3DrawIndexedFn>(draw)(self,d.count,d.instances,d.start,d.base,d.start_instance);
        else reinterpret_cast<Step3DrawFn>(draw)(self,d.count,d.instances,d.start,d.start_instance);
    }
    step5EyeRsSet(self,rs,false);   // the game's viewport/scissor back (eye ring only)
    srv(self,s3.t10,s3.t10Original);
    om(self,s3.omCount,s3.omCount?s3.omRtv.data():nullptr,s3.omSingle,s3.omDsv?&s3.omDsvHandle:nullptr);
}
// ------------------------------------------------------------- J-hud-capture Draw seam (game thread)
// Everything one HUD replay needs, copied under the step-1 mutex; recorded after it is released.
struct Step5HudCall {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{},dsv{};bool useDs=false,clear=false,clearDs=false;D3D12_CLEAR_FLAGS dsFlags=D3D12_CLEAR_FLAG_DEPTH;
    UINT omCount=0;BOOL omSingle=FALSE;std::array<D3D12_CPU_DESCRIPTOR_HANDLE,8> omRtv{};bool omDsv=false;D3D12_CPU_DESCRIPTOR_HANDLE omDsvHandle{};
    bool indexed=false;UINT count=0,instances=0,start=0,startInstance=0;INT base=0;
    ID3D12PipelineState* clone=nullptr,*game=nullptr; // HUD-LATE display marker (hud_late.h, F6): both set = replay under the alpha-marker clone, then restore the game's PSO
};
// [ClearRTV(hud, 0,0,0,0) [ClearDSV(hud, depth 0, stencil 0)] once per frame], OMSet(hud RT [+ hud DS]),
// the game's Draw with its live arguments under the game's own PSO/root/viewport state, OMSet(game
// targets from the bind-time descriptor ring). The game's own Draw follows, forwarded by the caller.
__declspec(noinline) void step5RecordHud(ID3D12GraphicsCommandList* self,const Step5HudCall& c) noexcept {
    using ClearRtvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT*,UINT,const D3D12_RECT*);
    using ClearDsvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*);
    const auto om=reinterpret_cast<Step5OmFn>(step1Native(self,46));
    if(c.clear) {
        static constexpr FLOAT transparent[4]={0,0,0,0};
        reinterpret_cast<ClearRtvFn>(step1Native(self,48))(self,c.rtv,transparent,0,nullptr);
    }
    // J-hud-markers: the shared depth-stencil is cleared by the first draw of either target (key off: clear and clearDs are set together).
    if(c.clearDs)reinterpret_cast<ClearDsvFn>(step1Native(self,47))(self,c.dsv,c.dsFlags,0.0f,0,0,nullptr);
    om(self,1,&c.rtv,FALSE,c.useDs?&c.dsv:nullptr);
    // HUD-LATE display marker: the private clone (alpha ONE / ONE / REV_SUBTRACT) replaces the game's PSO for this one replay Draw (native vtable call, the proxy's
    // PSO tracking is not touched) and the game's PSO is bound again before the game's own Draw.
    using SetPsoFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
    const bool swap=c.clone&&c.game;
    if(swap)reinterpret_cast<SetPsoFn>(step1Native(self,25))(self,c.clone);
    if(c.indexed)reinterpret_cast<Step3DrawIndexedFn>(step1Native(self,13))(self,c.count,c.instances,c.start,c.base,c.startInstance);
    else reinterpret_cast<Step3DrawFn>(step1Native(self,12))(self,c.count,c.instances,c.start,c.startInstance);
    if(swap)reinterpret_cast<SetPsoFn>(step1Native(self,25))(self,c.game);
    om(self,c.omCount,c.omCount?c.omRtv.data():nullptr,c.omSingle,c.omDsv?&c.omDsvHandle:nullptr);
}
// ------------------------------------------------------------- J-effects-layer replay (default OFF)
// Everything one speculative effects replay needs, copied under the step-1 mutex; recorded after it is released.
struct Step5EffCall {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{},dsv{};bool useDs=false,clear=false,clearDs=false;D3D12_CLEAR_FLAGS dsFlags=D3D12_CLEAR_FLAG_DEPTH;
    ID3D12Resource* predRes=nullptr;UINT64 predOffset=0;D3D12_PREDICATION_OP predOp=D3D12_PREDICATION_OP_EQUAL_ZERO;
    UINT omCount=0;BOOL omSingle=FALSE;std::array<D3D12_CPU_DESCRIPTOR_HANDLE,8> omRtv{};bool omDsv=false;D3D12_CPU_DESCRIPTOR_HANDLE omDsvHandle{};
    bool indexed=false;UINT count=0,instances=0,start=0,startInstance=0;INT base=0;
    ID3D12PipelineState* clone=nullptr,*game=nullptr; // J-effects-dual: both set = replay under the clone, then restore the game's PSO
};
using Step5PredFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,D3D12_PREDICATION_OP);
// [ClearRTV(effects, 0,0,0,0)] [ClearDSV(shared HUD depth-stencil, depth 0, stencil 0)] once per frame (never predicated),
// SetPredication(predicate[epoch], NOT_EQUAL_ZERO), OMSet(effects RT [+ HUD DS]), the game's Draw with its live arguments under the
// game's own PSO/root/viewport state, SetPredication(null), OMSet(game targets from the bind-time descriptor ring). The game's own
// Draw follows, forwarded by the caller, NOT predicated. The predicate is 0 until a LATER cockpit member commits the epoch.
__declspec(noinline) void step5RecordEffect(ID3D12GraphicsCommandList* self,const Step5EffCall& c) noexcept {
    using ClearRtvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT*,UINT,const D3D12_RECT*);
    using ClearDsvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*);
    const auto om=reinterpret_cast<Step5OmFn>(step1Native(self,46));
    const auto pred=reinterpret_cast<Step5PredFn>(step1Native(self,55));
    if(c.clear) {
        static constexpr FLOAT transparent[4]={0,0,0,0};
        reinterpret_cast<ClearRtvFn>(step1Native(self,48))(self,c.rtv,transparent,0,nullptr);
    }
    if(c.clearDs)reinterpret_cast<ClearDsvFn>(step1Native(self,47))(self,c.dsv,c.dsFlags,0.0f,0,0,nullptr);
    if(c.predRes)pred(self,c.predRes,c.predOffset,c.predOp); // D3D12: an ENABLED predication discards the draw (EQUAL_ZERO: word 0 = discard)
    om(self,1,&c.rtv,FALSE,c.useDs?&c.dsv:nullptr);
    // J-effects-dual: the private alpha-blend clone replaces the game's PSO for this replay only (native vtable call: the proxy's PSO tracking is
    // not touched); the game's PSO is bound again before the predication ends, so the game's own Draw sees exactly its state.
    using SetPsoFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
    const bool swap=c.clone&&c.game;
    if(swap)reinterpret_cast<SetPsoFn>(step1Native(self,25))(self,c.clone);
    if(c.indexed)reinterpret_cast<Step3DrawIndexedFn>(step1Native(self,13))(self,c.count,c.instances,c.start,c.base,c.startInstance);
    else reinterpret_cast<Step3DrawFn>(step1Native(self,12))(self,c.count,c.instances,c.start,c.startInstance);
    if(swap)reinterpret_cast<SetPsoFn>(step1Native(self,25))(self,c.game);
    if(c.predRes)pred(self,nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO); // only after our own SetPredication; the candidate gate proved the game's predicate off
    om(self,c.omCount,c.omCount?c.omRtv.data():nullptr,c.omSingle,c.omDsv?&c.omDsvHandle:nullptr);
}
// J-effects-smoke-fix: the first 8 hex digits of a creation hash (0 when absent).
inline uint32_t step5Hex8(const std::array<char,33>& h) noexcept {
    uint32_t v=0;for(unsigned i=0;i<8;++i){const char c=h[i];v=(v<<4)|(c>='0'&&c<='9'?unsigned(c-'0'):c>='a'&&c<='f'?unsigned(c-'a'+10):c>='A'&&c<='F'?unsigned(c-'A'+10):0u);}
    return v;
}
// J-effects-smoke-fix (Astra issue-31 P1-3): true when the root signature exposes a UAV to any stage (root UAV parameter or a descriptor-table UAV range).
inline bool step5RootHasUav(const acz::metadata::RootBindingMetadata& r) noexcept {
    for(const auto& p:r.layout().parameters) {
        if(p.kind==acz::metadata::ParameterKind::uav)return true;
        if(p.kind==acz::metadata::ParameterKind::table)for(const auto& g:p.ranges)if(g.kind==acz::metadata::BindingKind::uav)return true;
    }
    return false;
}
#include "gpu_copy_step5_fxcensus.inl" // EFFECTS-OUTSIDE (D) fxmem:3: explosion/tracer draw census (log only)
#include "gpu_copy_step5_fxdiag.inl" // READ-ONLY flare / aircraft diagnostics (fxsig sentinels; fx_diag_flare_air.h)
#include "gpu_copy_step5_fxsprite.inl" // EFFECTS-OUTSIDE (E) fxmem:5: explosion sprite capture + per-eye CPU dry run (log only)
// ------------------------------------------------------------- EFFECTS-OUTSIDE (C) fxmem:1: missile-smoke capture + log ONLY (fx_smoke.h; docs section 8.3)
// Steps 1-2 of the smoke design: at the effects class gate, every draw of the smoke pipeline (PS d2464ed2 + VS 9eff1f92, the identity the effects
// candidate table already reads) gets its 1,588-byte t10 record (GS root SRV element 0) copied from the game's pinned, CPU-mapped UPLOAD memory
// (step3MappedSpan_locked + two guarded reads that must agree), with its draw arguments and the cb10/cb11/cb12 root constants when readable, into a
// bounded per-frame store (16 records; over the cap: counted before any read). Nothing is recorded into a command list, the game's draw/PSO/bindings
// are untouched and game memory is only READ. Any unexpected binding/size/state is refused and counted (fail closed). One fx-smoke-frame line per
// published frame with smoke, rate-limited (the first 3, then <= 1 per second). Flag off (default): every function below returns at once.
inline bool step5FxSmokeOn() noexcept {return gpu_copy::step5::effects_smoke_capture_flag().load(std::memory_order_relaxed);}
inline bool step5FxSmokeDraw(const Step5EffId& id) noexcept {return id.ps8==acz::fxsmoke::kSmokePs8&&id.vs8==acz::fxsmoke::kSmokeVs8;}
inline bool step5FxSmokeReplayOn() noexcept {return gpu_copy::step5::effects_smoke_replay_flag().load(std::memory_order_relaxed);}
// fxmem:2 DRY RUN (step-1 mutex held): one stored record prepared for both eyes exactly as the GPU replay would bind them (fx_smoke.h prepare_eye_records: the
// world frame's D_e x S, the objects' depth scale, the eye clip, the rescaled lens), then its trail points counted per eye. Refused (counted, named) when the
// frame has no accepted world-objects record, no world camera / pose, or the record fails any replay precondition. Never touches the GPU or game memory.
__declspec(noinline) void step5FxSmokeDryRun(Step5State& s5,const acz::fxsmoke::Captured& cap) noexcept {
    namespace fx=acz::fxsmoke;
    auto& dr=s5.fxDry;++dr.draws;
    const auto& w=s5.wobj;const char* why=nullptr;
    if(!s5.frame.fxObjA)why="smoke-world-no-object-record";      // no accepted DrawACM_v record in this frame (world objects key off / refused)
    else if(!w.camera||w.failed)why="smoke-world-camera-unavailable";
    else if(!w.pf.valid)why="smoke-world-pose-unavailable";
    if(why){++dr.refused;dr.last_why=why;dr.world_why=why;++s5.fxDryRefused;return;}
    fx::EyeDelta ed;ed.D[0]=w.pf.D[0];ed.D[1]=w.pf.D[1];ed.S=w.pf.S;ed.world_ref64=w.ref;
    fx::ReplayCfg cfg;cfg.depth_scale=gpu_copy::step5::world::depth_fix_scale_for(worldDepthFixConfigured(),s5.ringDsv);dr.depth_scale=cfg.depth_scale;
    unsigned idx=0;
    if(const char* bad=s5.fxStore.put(cap.rec,fx::kRecordBytes,cap.args,cap.root_known?&cap.roots[0][0]:nullptr,ed,cfg,idx)){++dr.refused;dr.last_why=bad;++s5.fxDryRefused;return;}
    ++dr.prepared;++s5.fxDryPrepared;s5.fxLastIdx=int(idx); // fxmem:4: the store index of THIS draw's eye copies
    if(dr.gain==0)dr.gain=fx::lens_fit(cap.rec).gain;
    fx::count_eye_points(cap.rec,s5.fxStore.eye(idx,0),s5.fxStore.eye(idx,1),cap.args,dr);
}
// Hook-state mutex held (the HUD seam's identity block). cb10/cb11 (GS) and cb12 (PS) when each is a root-constants parameter of the bound root
// signature whose first two words were written since that root was bound. nullptr = all six floats in `out`; else why (out undefined).
__declspec(noinline) const char* step5FxSmokeRoots_locked(const ListState& ls,float out[6]) noexcept {
    const auto& b=ls.bindings;
    if(!b.tracking_complete||b.tracking_gap)return "binding-tracking-incomplete";
    if(!b.root||!b.root->valid())return "root-layout-unknown";
    const auto& L=b.root->layout();
    acz::fxsmoke::RootParamView v[64];const unsigned n=unsigned(L.parameters.size()<64?L.parameters.size():64);
    if(L.parameters.size()>64)return "root-layout-over-64";
    for(unsigned i=0;i<n;++i) {
        const auto& p=L.parameters[i];auto& q=v[i];
        q.kind=uint32_t(p.kind);q.visibility=uint32_t(p.visibility);q.reg=p.shader_register;q.space=p.space;q.constants=p.constant_count;
        if(p.kind==acz::metadata::ParameterKind::table)for(const auto& g:p.ranges)if(g.kind==acz::metadata::BindingKind::cbv&&!g.space)
            for(uint32_t r=10;r<=12;++r)if(g.base_register<=r&&(g.count==UINT32_MAX||r-g.base_register<g.count))q.table_cbv[r-10]=true;
    }
    for(uint32_t r=10;r<=12;++r) {
        unsigned idx=0;
        if(const char* why=acz::fxsmoke::find_root_constants(v,n,L.flags,r,idx))return why;
        const auto it=b.roots.find(idx);
        if(it==b.roots.end()||it->second.kind!=acz::metadata::ParameterKind::constants||it->second.constants.size()<2||it->second.written.size()<2)return "unwritten";
        const bool w[2]={bool(it->second.written[0]),bool(it->second.written[1])};
        if(const char* why=acz::fxsmoke::root_pair(it->second.constants.data(),w,2,out+2*(r-10)))return why;
    }
    return nullptr;
}
// One smoke draw (step-1 mutex held; `d` = the HUD seam's live Shape of THIS draw). Nests the hook-state mutex for the mapped read (documented order).
__declspec(noinline) void step5FxSmokeCapture(Step5State& s5,const gpu_copy::step3::Shape& d,const float* roots6,const char* rootsWhy) noexcept {
    s5.fxLastIdx=-1; // fxmem:4: no prepared copies for this draw until the dry run stores them
    if(!s5.fxOpen)return;
    ++s5.fxDraws;
    auto& c=s5.fxCap;
    if(c.full()){c.note_overflow();++s5.fxOverCap;return;} // bounded: no read over the cap (counted: the draw takes the window effects replay)
    const char* why=nullptr;
    if(d.indexed)why="smoke-draw-indexed";                                            // the smoke draw is non-indexed (one point per segment)
    else if(d.t10_parameter==UINT32_MAX||!d.t10_va||!d.t10_address)why="smoke-t10-unresolved";
    else if(d.t10_table)why="smoke-t10-descriptor-table";                            // a root SRV only: its VA IS element 0
    else if(d.t10_vs)why="smoke-t10-vertex-binding";
    else if(!d.t10_root_value)why="smoke-t10-trackers-disagree";
    if(!why) {
        auto& s=state();std::lock_guard lock(s.mutex);const uint8_t* p=nullptr;
        if(const char* bad=step3MappedSpan_locked(d.t10_address,d.t10_va,acz::fxsmoke::kRecordBytes,p))why=bad;   // pinned, mapped UPLOAD; span inside the resource
        else if(!guardedCpuRead(p,s5.fxRec,acz::fxsmoke::kRecordBytes)||!guardedCpuRead(p,s5.fxRec2,acz::fxsmoke::kRecordBytes))why="smoke-read-fault";
        else if(std::memcmp(s5.fxRec,s5.fxRec2,acz::fxsmoke::kRecordBytes)!=0)why="smoke-record-torn";             // being written while read: refused
    }
    if(why){++s5.fxUnread;s5.fxLastUnread=why;c.note_refused(why);return;}
    acz::fxsmoke::DrawArgs a;a.vertex_count=d.count;a.instance_count=d.instances;a.start_vertex=d.start;a.start_instance=d.start_instance;
    if(!c.add(s5.fxRec,acz::fxsmoke::kRecordBytes,a,d.t10_va,0,d.pso_route,roots6,rootsWhy)) { // record_check refuses (counted) anything not proven
        ++s5.fxStored;
        if(s5.fxDryOn)step5FxSmokeDryRun(s5,c.at(c.count()-1)); // fxmem:2 (latched per frame; default off: never runs)
    }
}
// An ACCEPTED DrawACM_v record of the open frame (world objects): its +0 = the frame's A, kept once for the fx-smoke-frame comparison.
inline void step5FxSmokeObjectA(Step5Frame& f,const uint8_t* raw) noexcept {
    if(f.fxObjA||!raw||!step5FxSmokeOn())return;
    std::memcpy(f.fxObjAm,raw,64);f.fxObjA=true;
}
// Per published effects-armed frame (step-1 mutex held): the frame's summary becomes the pending line when the rate limit admits it.
void step5FxSmokeAccount(Step5State& s5,const Step5Frame& f) noexcept {
    if(!s5.fxOpen)return;
    s5.fxOpen=false;
    const auto& c=s5.fxCap;
    if(!c.count()&&!c.refused()&&!c.overflow())return; // no smoke draw in this frame: no line
    {const unsigned n=c.count()+c.refused()+c.overflow();if(n>s5.fxPeakDraws)s5.fxPeakDraws=n;} // docs section 18: the run's peak smoke draws per frame
    const auto& sm=f.eff.sm; // EFFECTS-OUTSIDE (C) fxmem:4: run totals of every published frame with smoke draws (latched off: nothing counted)
    if(sm.on){++s5.smokeFrames;if(sm.valid())++s5.smokeValidFrames;s5.smokeReplays+=sm.replays;s5.smokeCommitted+=sm.committed;s5.smokeRefused+=sm.refused;
        s5.smokeFallback+=f.eff.smFallback;if(sm.last_why)s5.smokeLastWhy=sm.last_why;}
    ++s5.fxFrames;
    unsigned skipped=0;
    if(!s5.fxGate.admit(GetTickCount64(),skipped))return;
    if(s5.fxPending)skipped+=s5.fxPendSkipped+1; // the worker had not written the previous one yet: it is replaced and counted
    s5.fxPend=acz::fxsmoke::summarise(c,f.fxObjA?f.fxObjAm:nullptr);s5.fxPendFrame=s5.nextFrameId+1;s5.fxPendSkipped=skipped;s5.fxPending=true;
    s5.fxDryPending=s5.fxDryOn;if(s5.fxDryOn)s5.fxDryPend=s5.fxDry; // fxmem:2: the same frame's dry-run plan goes out with the frame line
    s5.fxGpuPending=sm.on;if(sm.on){s5.fxGpuPend=sm;s5.fxGpuPendFallback=f.eff.smFallback;} // fxmem:4: and its GPU replay facts
}
// ------------------------------------------------------------- EFFECTS-OUTSIDE (C) fxmem:4: the per-eye GPU smoke replay (fx_smoke_gpu.h; docs section 8.7)
// fxmem:2's prepared eye copies, recorded per eye on the GPU instead of the window effects replay of the same draw. Latched per frame at the effects arm
// (f.eff.sm.on) only when the resources exist; every refusal falls back to the window replay (the in-window smoke of fxmem:0..2) and is counted.
inline bool step5FxSmokeGpuOn() noexcept {return gpu_copy::step5::effects_smoke_gpu_flag().load(std::memory_order_relaxed);}
// Everything one per-eye smoke replay needs, copied under the step-1 mutex; recorded after it is released together with the effects call (its clears,
// predicate, clone/game PSOs and the game's OM targets).
struct Step5SmokeCall {D3D12_CPU_DESCRIPTOR_HANDLE rtv[2]{},dsv[2]{};bool clear=false;acz::fxsmoke::RootSwap swap{};Step5EyeRs rs{}; /* EYE-RING: off = no RS call */};
// Step-1 mutex held, right after the effects layer accepted a smoke draw (`ecall` filled) and the capture/dry run of the same draw. true = record the per-eye
// replay INSTEAD of the window effects draw (out filled, the eye copies written into this slot's UPLOAD span).
__declspec(noinline) bool step5FxSmokeGpuPlan_locked(Step1State& st,Step5State& s5,Step5Frame& f,const gpu_copy::step3::Shape& d,const Step5EffCall& ecall,
                                                     Step5SmokeCall& out) noexcept {
    namespace g=acz::fxsmoke::gpu;auto& sm=f.eff.sm;
    if(!sm.on)return false;
    const auto& sl=s5.slot[f.slot];
    g::GateIn in;
    in.ready=s5.smokeReady&&sl.smokeRt[0]&&sl.smokeRt[1]&&s5.smokeUploadPtr&&s5.smokeUploadVa;
    in.eye_depth=st.s3.targets.has_depth&&sl.ds[0]&&sl.ds[1];in.dual=ecall.clone&&ecall.game;in.effects_recorded=true;
    in.prepared=s5.fxLastIdx>=0&&unsigned(s5.fxLastIdx)<s5.fxStore.count()&&!ecall.indexed;
    in.t10_root_srv=d.t10_parameter!=UINT32_MAX&&!d.t10_table&&!d.t10_vs&&d.t10_root_value&&d.t10_va;
    in.pso_dsv=d.pso.dsv;in.eye_dsv=s5.ringDsv;in.pso_rtv0=d.pso.rtv[0];in.pso_rt_count=d.pso.rt_count;in.image_format=s5.hudFormat;in.pso_samples=d.pso.samples;
    in.depth_enable=d.pso.depth_enable;in.depth_write=d.pso.depth_write;in.stencil=d.pso.stencil;in.depth_func=d.pso.depth_func;
    in.stencil_noop=d.pso.front_func==gpu_copy::step3::cmp::always&&d.pso.back_func==gpu_copy::step3::cmp::always&&(d.pso.stencil_write==0||d.pso.stencil_ops_keep);
    in.vp_known=d.rs_known&&!d.rs_gap;in.viewports=d.viewports;in.vp[0]=d.vp0.x;in.vp[1]=d.vp0.y;in.vp[2]=d.vp0.w;in.vp[3]=d.vp0.h;
    in.image_w=s5.gameW;in.image_h=s5.gameH; // EYE-RING: the game's viewport covers the GAME image (= ringW/ringH without the eye ring); the eye one is substituted below
    const char* why=g::gate(in);
    const uint64_t span=uint64_t(f.slot)*g::kSlotUpload;
    if(!why)why=step5EyeRsNote(s5,step5EyeRsPlan(s5,d,out.rs)); // EYE-RING: nullptr and rs.on=false without it
    if(!why)why=acz::fxsmoke::plan_root_swap(d.t10_parameter,true,d.t10_va,s5.smokeUploadVa+span,g::kSlotUpload,unsigned(s5.fxLastIdx),out.swap);
    if(why){sm.note_refused(why);++f.eff.smFallback;return false;}
    // The eye copies into THIS slot's span: the slot is PRODUCING (not held, not owned by another live recording, its last GPU use completed: step5PickSlot_locked).
    const unsigned idx=unsigned(s5.fxLastIdx);
    for(unsigned e=0;e<2;++e)std::memcpy(s5.smokeUploadPtr+span+acz::fxsmoke::ReplayStore::offset(idx,e),s5.fxStore.eye(idx,e),acz::fxsmoke::kRecordBytes);
    std::atomic_thread_fence(std::memory_order_release);
    for(unsigned e=0;e<2;++e){out.rtv[e]=sl.smokeRtv[e];out.dsv[e]=sl.dsv[e];}
    out.clear=!sm.cleared;sm.cleared=true;
    sm.note_replay(ecall.predRes!=nullptr,ecall.predOffset);
    f.eff.commands+=g::kCmdsPerDraw-(gpu_copy::step5::effects_per_draw+2)+(out.clear?g::kCmdsClear:0u); // the effects call already counted 5 + 2 (dual)
    if(out.rs.on){f.eff.commands+=::acz::eyering::kExtraCommands;++s5.eyeFxReplays;++s5.eyeVpSub;++s5.eyeScSub;++s5.eyeVpRestore;} // EYE-RING
    return true;
}
// The effects call's once-per-frame clears, [the two eye smoke image clears], SetPredication(the effects epoch's word), then fx_smoke.h replay_commands
// (clone PSO, per eye OMSet(smoke image, eye objects DSV) + RootSRV(t10, eye copy) + Draw, RootSRV(t10, game VA), game PSO), SetPredication(null), OMSet(game
// targets). The game's own smoke draw follows, forwarded by the caller, under exactly its own state. Native vtable calls only (the proxy's trackers untouched).
__declspec(noinline) void step5RecordSmoke(ID3D12GraphicsCommandList* self,const Step5EffCall& c,const Step5SmokeCall& s) noexcept {
    namespace fx=acz::fxsmoke;
    using ClearRtvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT*,UINT,const D3D12_RECT*);
    using ClearDsvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*);
    using SetPsoFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
    const auto om=reinterpret_cast<Step5OmFn>(step1Native(self,46));
    const auto pred=reinterpret_cast<Step5PredFn>(step1Native(self,55));
    const auto srv=reinterpret_cast<Step3SrvFn>(step1Native(self,40));
    static constexpr FLOAT transparent[4]={0,0,0,0};
    if(c.clear)reinterpret_cast<ClearRtvFn>(step1Native(self,48))(self,c.rtv,transparent,0,nullptr);
    if(c.clearDs)reinterpret_cast<ClearDsvFn>(step1Native(self,47))(self,c.dsv,c.dsFlags,0.0f,0,0,nullptr);
    if(s.clear)for(unsigned e=0;e<2;++e)reinterpret_cast<ClearRtvFn>(step1Native(self,48))(self,s.rtv[e],transparent,0,nullptr);
    if(c.predRes)pred(self,c.predRes,c.predOffset,c.predOp);
    fx::Cmd cmds[fx::kMaxReplayCmds];const unsigned n=fx::replay_commands(s.swap,true,cmds);
    if(n)step5EyeRsSet(self,s.rs,true); // EYE-RING: the eye viewport/scissor for the two eye draws (off: no call); the list's om_restore sets the game's back
    for(unsigned i=0;i<n;++i) {
        const auto& k=cmds[i];
        switch(k.kind) {
        case fx::CmdKind::set_pso_clone:reinterpret_cast<SetPsoFn>(step1Native(self,25))(self,c.clone);break;
        case fx::CmdKind::om_eye:om(self,1,&s.rtv[k.eye],FALSE,&s.dsv[k.eye]);break;
        case fx::CmdKind::root_srv:srv(self,k.parameter,k.va);break;
        case fx::CmdKind::draw:reinterpret_cast<Step3DrawFn>(step1Native(self,12))(self,c.count,c.instances,c.start,c.startInstance);break;
        case fx::CmdKind::set_pso_game:reinterpret_cast<SetPsoFn>(step1Native(self,25))(self,c.game);break;
        case fx::CmdKind::om_restore:
            if(c.predRes)pred(self,nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO); // only after our own SetPredication (the effects gate proved the game's off)
            step5EyeRsSet(self,s.rs,false); // EYE-RING: the game's viewport/scissor back before its own draw
            om(self,c.omCount,c.omCount?c.omRtv.data():nullptr,c.omSingle,c.omDsv?&c.omDsvHandle:nullptr);break;
        }
    }
}
// At the list's Close (step-1 mutex held): the predicate words are final; count the replays the GPU executes.
inline void step5FxSmokeGpuClose(Step5State& s5,Step5Frame& f) noexcept {
    if(f.eff.armed&&f.eff.sm.on)f.eff.sm.close(s5.effPredPtr,s5.effPredMode);
}
// One non-member draw while the frame's effects layer is armed (step-1 mutex held, `d` captured by the HUD seam). Returns true when
// a speculative replay must be recorded (`call` filled). Counters only; never throws.
bool step5EffectsConsider_locked(Step1State& st,Step5State& s5,Step5Frame& f,const gpu_copy::step3::Shape& d,bool regionEnded,const Step5EffId& id,bool predClear,Step5EffCall& call) noexcept {
    using namespace gpu_copy::step5;
    auto& e=f.eff;auto& h=f.hud;
    if(regionEnded) {
        // Only the scene target leaving RENDER_TARGET closes the layer for good; any other end (an off-screen pass) is reopened by the
        // next member.
        const char* why=hudRegionWhy(f.list,f.generation);
        if(why&&std::strcmp(why,"scene-barrier")==0)e.sceneClosed=true;
    } else if(h.radar&&!e.sceneClosed&&hudRegionResumed(f.list,f.generation))e.sceneClosed=true; // RADAR-SCENE-RESUME: the resumed region was closed by the scene barrier
    if(d.om_known&&!d.om_gap&&d.om_set&&d.om_count==0){++e.notScene;return false;}                        // no render target bound
    if(d.om_count>=1&&std::strcmp(d.game_rt_status,"resolved")==0&&d.game_rt!=h.scene){++e.notScene;return false;} // a proven other target
    HudTarget t;t.format=s5.hudFormat;t.ds_format=s5.hudDsFormat;t.width=s5.worldW;t.height=s5.worldH;t.scene=h.scene;
    EffectsClassIn cin;cin.allow=&s5.effAllow;cin.ps8=id.ps8;cin.writable=id.root;cin.dual=effectsDualConfigured();
    cin.dropClouds=effects_drop_game_clouds_flag().load(std::memory_order_relaxed); // EFFECTS-OUTSIDE: gamecloud:0 (default false)
    cin.vs8=id.vs8;cin.tracer=effects_sprite_gpu_flag().load(std::memory_order_relaxed); // EFFECTS-OUTSIDE (G) fxmem:6: the gun tracer (PS d34ee153 + VS 534f29ca + exact blend)
    cin.additive=effects_additive_flag().load(std::memory_order_relaxed); // EFFECTS-OUTSIDE (P) fxadd:1: the additive 4caf6e2d family through its clone (default false)
    cin.explosion=effects_explosion_flag().load(std::memory_order_relaxed); // EFFECTS-OUTSIDE (E) fxmem:5: PS 542a11d7 admitted with its coverage clone (default false)
    const EffectsVerdict v=effects_decide(d,t,e.sceneClosed?effects_scene_closed:effects_open,e.ep.speculative,&cin);
    s5.fxCenGate=v.why; // EFFECTS-OUTSIDE (D) fxmem:3: the class-gate verdict for the census (nullptr = passed)
    // The candidate table: every draw that reached the class gate (allowlisted or not), so the next live run can extend the allowlist.
    const bool classSeen=v.kind!=effects_ignored||(v.why&&std::strncmp(v.why,"class-",6)==0);
    EffectsPsEntry* pe=nullptr;
    if(classSeen&&d.pso.known) {
        pe=s5.effPs.touch(id.ps8);
        if(pe){++pe->seen;pe->vs8=id.vs8;pe->depthFunc=d.pso.depth_func;pe->allowed=s5.effAllow.has(id.ps8);pe->blend=d.pso.blend0;
               pe->depthEnable=d.pso.depth_enable;pe->depthWrite=d.pso.depth_write;pe->indexed=d.indexed;}
    }
    if(v.kind==effects_ignored){++e.ignored;if(v.why&&std::strncmp(v.why,"class-",6)==0)++s5.effClassIgnored;
        if(v.why&&std::strcmp(v.why,"class-game-cloud")==0)++s5.effGameCloudIgnored;return false;}
    auto refuse=[&](const char* why,int cat){
        e.ep.note_refused(cat,std::strcmp(why,"blend-constant-factor")==0);
        if(!e.firstWhy)e.firstWhy=why;
        s5.effLastWhy=why;return false;
    };
    if(v.kind==effects_refused){if(std::strcmp(v.why,"shader-writable-binding")==0)++s5.effUavRefused;
        if(std::strcmp(v.why,"dual-blend-unsupported")==0) { // (K) diagnostics only: the verdict is unchanged
            ++s5.effDualRefused;s5.effDualPs=id.ps8;s5.effDualVs=id.vs8;std::memcpy(s5.effDualBlend,id.blend,sizeof s5.effDualBlend);s5.effDualIndexed=d.indexed;
            s5.effDualCount=d.count;s5.effDualTopo=d.topology;s5.effDualDepth=d.pso.depth_func;s5.effDualTracerReady=d.pso.tracer_ready;s5.effDualReady=d.pso.dual_ready;}
        return refuse(v.why,v.category);}
    // Astra issue-31 P1-3: positive eligibility needs the root layout (UAV exposure); unknown is refused.
    if(id.rootUnknown){++s5.effUavRefused;return refuse("root-layout-unknown",hud_state);}
    // Astra issue-31 P1-2: the replay ends with SetPredication(null); that would clear a GAME predicate before the game's own draw. The draw is replayed
    // only while the game's predication is provably off (native-observation witness); active / unknown / in flight = refused.
    if(!predClear){++e.predRefused;++s5.effPredRefused;return refuse("game-predication-not-provably-off",hud_state);}
    const auto& sl=s5.slot[f.slot];
    const bool hasDs=sl.hudDs!=nullptr;
    const bool needRt=!e.cleared;
    // The shared HUD depth-stencil is cleared by whichever consumer (HUD, markers, effects) draws first this frame.
    const bool needDs=hasDs&&!e.dsCleared&&!h.cleared&&!h.mCleared&&!h.dsCleared;
    // J-effects-dual: the clone must exist for THIS bound PSO (effects_decide proved d.pso.dual_ready); anything else is refused before a predicate slot is used.
    if(cin.dual&&(!id.clone||!id.game))return refuse("dual-clone-missing",hud_state);
    const int pred=e.ep.open_draw();
    if(pred<0)return refuse("effects-epoch-cap",hud_cap);
    call=Step5EffCall{};
    call.rtv=sl.effRtv;call.dsv=sl.hudDsv;call.useDs=d.pso.dsv!=0&&sl.hudDs;
    call.predRes=s5.effPredMode==effects_pred_off?nullptr:s5.effPred;
    call.predOffset=(uint64_t(f.slot)*effects_epoch_cap+uint64_t(pred))*effects_predicate_stride;
    call.predOp=s5.effPredMode==effects_pred_ne0?D3D12_PREDICATION_OP_NOT_EQUAL_ZERO:D3D12_PREDICATION_OP_EQUAL_ZERO;
    if(pe)++pe->replayed;
    if(cin.additive&&d.pso.known&&d.pso.additive_ready){++s5.effAddAdmitted;if(!d.indexed&&d.topology==1)++s5.effAddPoints;} // (P) fxadd: admitted through the additive clone
    if(e.diagN<effects_diag_per_frame) {
        auto& g=e.diag[e.diagN++];g=Step5EffDiag{};g.epoch=pred;g.ord=uint32_t(d.draw_ordinal);g.vs8=id.vs8;g.ps8=id.ps8;g.depthFunc=d.pso.depth_func;
        g.count=d.count;g.instances=d.instances;g.indexed=d.indexed;std::memcpy(g.blend,id.blend,sizeof g.blend);g.offset=call.predOffset;
    } else ++e.diagDropped;
    if(needRt||needDs){
        call.clear=needRt;call.clearDs=needDs;call.dsFlags=D3D12_CLEAR_FLAGS(s5.hudDsClear);
        e.cleared=true;e.commands+=unsigned(needRt)+unsigned(needDs);
    }
    if(hasDs)e.dsCleared=true;
    call.omCount=st.s3.omCount;call.omSingle=st.s3.omSingle;call.omRtv=st.s3.omRtv;call.omDsv=st.s3.omDsv;call.omDsvHandle=st.s3.omDsvHandle;
    call.indexed=d.indexed;call.count=d.count;call.instances=d.instances;call.start=d.start;call.base=d.base;call.startInstance=d.start_instance;
    e.commands+=effects_per_draw;
    if(cin.dual){call.clone=id.clone;call.game=id.game;e.commands+=2;} // SetPipelineState(clone) before and SetPipelineState(game) after the replay Draw
    return true;
}
// Frame start (the first member armed the HUD region, step-1 mutex held): the effects layer arms with it. The slot's predicate words
// are zeroed: nothing is committed until a later cockpit member.
void step5EffectsArm_locked(Step5State& s5,Step5Frame& f) noexcept {
    f.eff=Step5Eff{};
    s5.fxOpen=false;s5.fxDryOn=false; // EFFECTS-OUTSIDE (C): a new frame never sees the previous frame's smoke records
    if(!effectsConfigured()||!f.hud.armed)return;
    if(!s5.effReady||!s5.effPredPtr||!s5.slot[f.slot].effRt)return;
    f.eff.armed=true;
    if(step5FxSmokeOn()) { // fxmem:1 (default off: untouched)
        s5.fxCap.begin(0);s5.fxOpen=true;
        s5.fxDryOn=step5FxSmokeReplayOn();if(s5.fxDryOn){s5.fxStore.begin();s5.fxDry.begin();} // fxmem:2 dry run, latched for this frame
        f.eff.sm.begin(s5.fxDryOn&&step5FxSmokeGpuOn()&&s5.smokeReady); // fxmem:4 GPU replay, latched for this frame (resources from ring creation)
    }
    step5FxCensusBegin(s5); // EFFECTS-OUTSIDE (D) fxmem:3 (flag off: returns)
    step5FxSpriteBegin(s5); // EFFECTS-OUTSIDE (E) fxmem:5 (flag off: closed)
    f.eff.sp.begin(s5.fxSprOpen&&step5FxSpriteGpuOn()&&s5.spriteReady); // EFFECTS-OUTSIDE (F) fxmem:6 GPU sprite replay, latched for this frame
    std::memset(s5.effPredPtr+uint64_t(f.slot)*gpu_copy::step5::effects_epoch_cap*gpu_copy::step5::effects_predicate_stride,0,
        size_t(gpu_copy::step5::effects_epoch_cap*gpu_copy::step5::effects_predicate_stride));
    std::atomic_thread_fence(std::memory_order_release);
}
// A LATER cockpit member (not the frame's first): the candidates since the previous member are BETWEEN members; the CPU commits the
// epoch by writing its predicate (before the list is closed and submitted).
void step5EffectsMember_locked(Step5State& s5,Step5Frame& f) noexcept {
    if(!f.eff.armed)return;
    f.eff.sceneClosed=false;
    const int p=f.eff.ep.commit();
    if(p<0)return;
    const uint64_t off=(uint64_t(f.slot)*gpu_copy::step5::effects_epoch_cap+uint64_t(p))*gpu_copy::step5::effects_predicate_stride;
    const uint64_t one=1;std::memcpy(s5.effPredPtr+off,&one,sizeof one);
    std::atomic_thread_fence(std::memory_order_release);
    for(unsigned i=0;i<f.eff.diagN;++i)if(f.eff.diag[i].epoch==p)f.eff.diag[i].committed=true;
}
// A cockpit member was recorded (step-1 mutex held). The first member arms the frame's HUD
// region; a later member after HUD-region draws restarts it (those draws were between members,
// not the post-cockpit tail): counts forgotten, the private target cleared again before the next
// HUD draw. The region is "after the LAST member".
// ------------------------------------------------------------- TACTICAL (tac:1; tactical_frame.h / tactical_producer.h; docs/world-render/tactical.md)
// Everything below runs only while acz::tac::producer_flags() has kProducerOn (set by the consumer when it parses tac:1); key off: step5TacOpen
// sees 0 and every other function returns at once. Step-1 mutex held throughout. Read-only: the game's buffers are only READ (pinned, mapped
// UPLOAD memory via step3MappedSpan_locked + guardedCpuRead, like the SrtData records); nothing is recorded, replayed or written differently
// except the optional suppression of a marker box draw (and its label) whose every marker matched a replayed world draw.
void step5TacOpen(Step5State& s5) noexcept {
    s5.tacFlags=acz::tac::producer_flags();s5.tacOpen=(s5.tacFlags&acz::tac::kProducerOn)!=0;
    if(!s5.tacOpen)return;
    auto& t=s5.tacCur;
    t.valid=true;t.a_valid=false;t.nobj=0;t.obj_dropped=0;t.nmark=0;t.mark_dropped=0;t.box_draws=t.label_draws=0;t.box_unreadable=t.label_unreadable=0;
    t.boxes_suppressed=t.labels_suppressed=0;t.wedge=0;t.wedge_tail=0;t.why=nullptr;
    t.lock_shapes=0;t.box_q=acz::tac::QStat{};t.label_q=acz::tac::QStat{};
    s5.tacSuppressLabel=false;s5.tacBoxFirst=0;s5.tacWedgePending=0;
    s5.tacLockReads=0;s5.tacDumpReads=0;s5.tacAfterBox=0;s5.tacCensusLines=0;s5.tacCensusFrame=false;
}
// LOCK-SHAPE (markers-360.md section 17.4; flag kProducerLockShape): an INSTRUMENT draw after a box draw of this frame whose vertices form the lock diamond
// (tactical_producer.h lock_shape_test) becomes a marker of the frame (shape 1) and is replayed into the MARKER image instead of the HUD image. Bounded:
// only draws of <= kLockMaxVerts vertices, at most kLockReadsPerFrame reads per frame. true = moved (the caller classifies it hud_marker_box).
constexpr unsigned kLockReadsPerFrame=24;
const char* step5TacReadSprites(Step5State& s5,const gpu_copy::step3::Shape& d,unsigned& n) noexcept;
bool step5TacLockShape(Step5State& s5,const gpu_copy::step3::Shape& d,acz::tac::LockShape* out=nullptr,unsigned* nvOut=nullptr) noexcept {
    using namespace acz::tac;
    if(!s5.tacOpen||!(s5.tacFlags&(kProducerLockShape|kProducerLockCensus))||!s5.tacCur.nmark)return false;
    if(!d.count||d.count>kLockMaxVerts*3||s5.tacLockReads>=kLockReadsPerFrame)return false;
    ++s5.tacLockReads;
    unsigned n=0;
    if(const char* why=step5TacReadSprites(s5,d,n)){if(out){*out=LockShape{};out->why=why;}++s5.tacLockWhy.why[kLockWhyN-1];return false;}
    const LockShape r=lock_shape_test(s5.tacScratch.v,n,s5.tacCur);
    ++s5.tacLockWhy.why[lock_why_index(r.why)];
    if(out)*out=r;
    if(nvOut)*nvOut=n;
    if(!r.ok||!(s5.tacFlags&kProducerLockShape))return false;
    return add_lock_shape(s5.tacCur,r,n);
}
// mk360cen:1 (kProducerLockCensus, one-shot): kCensusFrames frames >= kCensusGapMs apart; each starts once the frame's tail holds a COLOURED box marker in the
// gunsight zone (CENSUS-AIM, tactical_producer.h census_trigger; markers-360.md section 18.2) with one tac-lock-census-boxes line, then every box / label /
// moved-diamond draw and every instrument draw centred near a box gets one tac-lock-census line (class, shaders, vertices, bbox, colour, q, the diamond
// test's verdict and the nearest box): the lock diamond's signature on record. Instrument draws far from every box are counted, not logged.
void step5TacLockCensus(Step5State& s5,const gpu_copy::step3::Shape& d,const char* cls,uint32_t vs8,uint32_t ps8,bool box,const acz::tac::LockShape* ls,unsigned lsN) noexcept {
    using namespace acz::tac;
    if(!s5.tacOpen||!(s5.tacFlags&kProducerLockCensus)||s5.tacCensusDone>=kCensusFrames)return;
    const uint64_t now=GetTickCount64();
    if(!s5.tacCensusFrame) {
        if(!census_trigger(s5.tacCur))return; // CENSUS-AIM: not on cloud sprites, not before a coloured box sits near the gunsight
        if(s5.tacCensusLastMs&&now-s5.tacCensusLastMs<kCensusGapMs)return;
        if(s5.diagQ.count+2>=acz::fxdiag::LineQueue::kCap)return;
        s5.tacCensusFrame=true;s5.tacCensusLastMs=now;++s5.tacCensusDone;
        char b[acz::fxdiag::kLineMax];format_census_boxes(b,sizeof b,s5.nextFrameId+1,s5.tacCur);
        if(s5.diagQ.push_text(b))++s5.tacCensusLines;
    }
    if(s5.tacCensusLines>=kCensusLinesPerFrame||s5.diagQ.count+1>=acz::fxdiag::LineQueue::kCap)return;
    const bool instrument=!box&&std::strcmp(cls,"instrument")==0;
    CensusFacts c;c.frame=s5.nextFrameId+1;c.ord=d.draw_ordinal;c.after_box=s5.tacAfterBox;c.cls=cls;c.vs8=vs8;c.ps8=ps8;c.indexed=d.indexed;c.count=d.count;
    unsigned n=lsN;
    if(!ls) { // a box / label / unread draw: read it for the bbox (the diamond test result is reported only for instrument draws)
        if(const char* why=d.count<=kMaxSpriteVerts?step5TacReadSprites(s5,d,n):"vertex-count"){c.read_why=why;n=0;}
        c.shape=box?"box-draw":"not-tested";
    } else {c.shape=ls->why;c.box=ls->box;c.dx=ls->dx;c.dy=ls->dy;if(!n)c.read_why=ls->why;}
    c.nv=n;
    if(n){float x0=1e30f,x1=-1e30f,y0=1e30f,y1=-1e30f,rgb[3]={};
        for(unsigned i=0;i<n;++i){const auto& v=s5.tacScratch.v[i];x0=std::fmin(x0,v.x);x1=std::fmax(x1,v.x);y0=std::fmin(y0,v.y);y1=std::fmax(y1,v.y);
            for(int k=0;k<3;++k)rgb[k]+=std::isfinite(v.rgba[k])?v.rgba[k]:0.0f;c.q.add(v.q);}
        c.x0=x0;c.y0=y0;c.x1=x1;c.y1=y1;for(int k=0;k<3;++k)c.rgb[k]=rgb[k]/float(n);normalise_rgb(c.rgb);}
    if(instrument&&!(n&&census_near_box(s5.tacCur,0.5f*(c.x0+c.x1),0.5f*(c.y0+c.y1)))){++s5.tacCensusFar;return;} // CENSUS-AIM: far from every box
    c.topo=d.topology;c.nvl=n<4u?n:4u;for(unsigned i=0;i<c.nvl;++i){c.vx[i]=s5.tacScratch.v[i].x;c.vy[i]=s5.tacScratch.v[i].y;} // runLOCK2: the shape itself
    char b[acz::fxdiag::kLineMax];format_lock_census(b,sizeof b,c);
    if(s5.diagQ.push_text(b))++s5.tacCensusLines;
}
// mk360ld:1 (kProducerLockDump; markers-360.md section 21.6): the rejected lock-diamond candidates, one line per new signature, <= kLockDumpLines in all. Only in a
// frame whose marker list holds a RED box, for an instrument draw (cls) that was not moved: the reason is the diamond test's verdict, or why it was not tested
// (refused-draw / no-marker / too-many-indices). A not-yet-read draw is read here (<= kLockDumpReadsPerFrame per frame); bbox 6..kLockDumpBox GS each way only.
void step5TacLockDump(Step5State& s5,const gpu_copy::step3::Shape& d,bool refused,bool tested,const acz::tac::LockShape& ls,unsigned lsN,uint32_t vs8,uint32_t ps8) noexcept {
    using namespace acz::tac;
    if(!s5.tacOpen||!(s5.tacFlags&kProducerLockDump)||s5.tacDump.lines>=kLockDumpLines||!lock_dump_trigger(s5.tacCur))return;
    if(s5.diagQ.count+2>=acz::fxdiag::LineQueue::kCap)return;
    unsigned n=lsN;const char* why=ls.why;
    if(!tested||!n) { // not read by the lock test: read it here, bounded
        why=refused?"refused-draw":d.count>48?"too-many-indices":!s5.tacCur.nmark?"no-marker":"unread";
        if(s5.tacDumpReads>=kLockDumpReadsPerFrame||d.count>kMaxSpriteVerts)return;
        ++s5.tacDumpReads;
        n=0;if(step5TacReadSprites(s5,d,n)||!n)return;
    }
    float x0=1e30f,x1=-1e30f,y0=1e30f,y1=-1e30f,rgb[3]={};
    for(unsigned i=0;i<n;++i){const auto& v=s5.tacScratch.v[i];
        if(!std::isfinite(v.x)||!std::isfinite(v.y))return;
        x0=std::fmin(x0,v.x);x1=std::fmax(x1,v.x);y0=std::fmin(y0,v.y);y1=std::fmax(y1,v.y);
        for(int k=0;k<3;++k)rgb[k]+=std::isfinite(v.rgba[k])?v.rgba[k]:0.0f;}
    const float w=x1-x0,h=y1-y0;
    if(w<6.0f||h<6.0f||w>kLockDumpBox||h>kLockDumpBox)return;
    if(!lock_dump_new_sig(s5.tacDump,why,n,d.count,0.5f*w,0.5f*h))return;
    LockDumpFacts c;c.frame=s5.nextFrameId+1;c.ord=d.draw_ordinal;c.why=why;c.vs8=vs8;c.ps8=ps8;c.topo=d.topology;c.count=d.count;c.indexed=d.indexed;c.nv=n;c.after_box=s5.tacAfterBox;
    for(int k=0;k<3;++k)c.rgb[k]=rgb[k]/float(n);
    normalise_rgb(c.rgb);
    c.nvl=n<kLockDumpVerts?n:kLockDumpVerts;for(unsigned i=0;i<c.nvl;++i){c.vx[i]=s5.tacScratch.v[i].x;c.vy[i]=s5.tacScratch.v[i].y;}
    char b[acz::fxdiag::kLineMax];format_lock_dump(b,sizeof b,c,x0,y0,x1,y1);
    if(s5.diagQ.push_text(b))++s5.tacDump.lines;
}
// An ACCEPTED world draw (raw = the record the decision used).
void step5TacObject(Step5State& s5,const uint8_t* raw,const gpu_copy::step3::Shape& d) noexcept {
    if(!s5.tacOpen||!raw)return;
    auto& t=s5.tacCur;
    if(!t.a_valid){std::memcpy(t.A,raw,64);t.a_valid=true;}
    if(t.nobj>=acz::tac::kObjCap){++t.obj_dropped;return;}
    auto& o=t.obj[t.nobj++];
    acz::tac::object_from_record(raw,o);
    o.sig=acz::tac::mesh_signature(d.indexed,d.count,d.vb_stride,d.topology);o.marker=0;
    o.td_valid=0;
    if(s5.interpDispFresh){for(int k=0;k<3;++k)o.td[k]=s5.interpDisp[k];o.td_valid=1;s5.interpDispFresh=false;} // OBJECT-INTERP: display-time position
}
// A cockpit member after the first: draws since the previous member were BETWEEN members (the firing wedge counts), and the HUD tail restarts
// (the HUD replay resets: marker boxes seen so far were not the tail).
void step5TacMember(Step5State& s5,bool first) noexcept {
    if(!s5.tacOpen)return;
    if(!first)s5.tacCur.wedge+=s5.tacWedgePending;
    s5.tacWedgePending=0;
    for(unsigned i=0;i<s5.tacCur.nobj;++i)s5.tacCur.obj[i].marker=0;
    acz::tac::restart_marker_tail(s5.tacCur,s5.tacUnreadBox,s5.tacUnreadLabel); // markers AND their drop/unread facts (markers-360.md section 22)
    s5.tacSuppressLabel=false;s5.tacBoxFirst=0;
    s5.tacLockReads=0;s5.tacDumpReads=0;s5.tacAfterBox=0; // LOCK-SHAPE: the tail restarts
}
// The draw's vertices (ps2_v layout: attr0 = GS x,y,z,w float4 at +0, attr1 = colour float4 at +16) into s5.tacScratch.v; n = vertices read
// (unique vertices for an indexed draw). nullptr = read, else why not.
const char* step5TacReadSprites(Step5State& s5,const gpu_copy::step3::Shape& d,unsigned& n) noexcept {
    n=0;
    const uint32_t stride=d.vb_stride;
    if(stride<32||stride>256)return "vb-stride";
    if(!d.count||d.count>acz::tac::kMaxSpriteVerts)return "vertex-count";
    if(!d.vb_va||!d.vb_bytes)return "vb-unbound";
    auto& s=state();std::lock_guard lock(s.mutex);
    const uint8_t* vb=nullptr;
    if(const char* why=step3MappedSpan_locked(d.vb_address,d.vb_va,d.vb_bytes,vb))return why;
    auto vertex=[&](uint64_t i)->bool{
        if((i+1)*uint64_t(stride)>d.vb_bytes)return false;
        float raw[8];
        if(!guardedCpuRead(vb+i*stride,raw,sizeof raw))return false;
        auto& v=s5.tacScratch.v[n++];v.x=raw[0];v.y=raw[1];for(int k=0;k<4;++k)v.rgba[k]=raw[4+k];v.q=raw[3];
        // markers-360.md section 17.1: VS 534f29ca outputs ((x-2048)*2/512, (y-2048)*2/-448, z*2^-24, 1) / q: the division by q cancels in the perspective
        // divide, the pixel lands at the STORED x, y whatever q is. The earlier x/q, y/q moved every vertex with q != 1 (textured glyphs) by 2048 (1/q - 1).
        return true;
    };
    if(!d.indexed) {
        for(uint32_t k=0;k<d.count;++k)if(!vertex(uint64_t(d.start)+k))return "vertex-outside-view";
        return nullptr;
    }
    const uint32_t isz=d.ib_stride==4?4u:d.ib_stride==2?2u:0u;
    if(!isz||!d.ib_va||!d.ib_bytes)return "ib-unbound";
    if((uint64_t(d.start)+d.count)*isz>d.ib_bytes)return "index-outside-view";
    const uint8_t* ib=nullptr;
    if(const char* why=step3MappedSpan_locked(d.ib_address,d.ib_va+uint64_t(d.start)*isz,uint64_t(d.count)*isz,ib))return why;
    uint32_t m=0;
    {const uint8_t* buf=s5.tacIbBytes;
     if(!guardedCpuRead(ib,s5.tacIbBytes,size_t(d.count)*isz))return "index-read-fault";
     for(uint32_t k=0;k<d.count;++k){uint32_t x=0;std::memcpy(&x,buf+size_t(k)*isz,isz);
         if((isz==2&&x==0xffffu)||(isz==4&&x==0xffffffffu))continue; // strip cut
         s5.tacIdx[m++]=x;}}
    std::sort(s5.tacIdx,s5.tacIdx+m);m=uint32_t(std::unique(s5.tacIdx,s5.tacIdx+m)-s5.tacIdx);
    for(uint32_t k=0;k<m;++k){const int64_t i=int64_t(s5.tacIdx[k])+d.base;if(i<0||!vertex(uint64_t(i)))return "vertex-outside-view";}
    return nullptr;
}
// A HUD-tail marker draw (classified box or label). true = suppress (do not replay): every marker of this box draw matched a replayed world
// draw and suppression is on (the consumer draws those markers itself), or the label belonging to such a box.
bool step5TacMarker(Step5State& s5,const gpu_copy::step3::Shape& d,bool box) noexcept {
    if(!s5.tacOpen)return false;
    using namespace acz::tac;
    auto& t=s5.tacCur;unsigned n=0;uint32_t dropped=0;
    if(!box) {
        ++t.label_draws;bool sup=s5.tacSuppressLabel;s5.tacSuppressLabel=false;
        // J-201 P1 (review-w4-2): an unreadable label is never hidden (it stays the game's draw); a read label only when every cluster is FRND/TGT text joined
        // one-to-one to a hidden box of the preceding box draw (label_suppress_ok: hiding is always paired with a replacement)
        if(const char* why=step5TacReadSprites(s5,d,n)){++t.label_unreadable;s5.tacLastUnread=why;sup=false;}
        else {for(unsigned i=0;i<n;++i)t.label_q.add(s5.tacScratch.v[i].q);
            const unsigned nc=cluster_vertices(s5.tacScratch,n,kLabelLink,dropped);int boxOf[kMarkCap];add_labels(t,s5.tacBoxFirst,s5.tacScratch.c,nc,boxOf);
            sup=label_suppress_ok(sup,true,s5.tacScratch.c,nc,dropped,boxOf,s5.tacBoxFirst,t.nmark);}
        if(sup){++t.labels_suppressed;return true;}
        return false;
    }
    ++t.box_draws;s5.tacSuppressLabel=false;
    const unsigned first=t.nmark;s5.tacBoxFirst=first;
    if(const char* why=step5TacReadSprites(s5,d,n)){++t.box_unreadable;s5.tacLastUnread=why;if(!t.why)t.why=why;return false;}
    for(unsigned i=0;i<n;++i)t.box_q.add(s5.tacScratch.v[i].q);
    const unsigned nc=cluster_vertices(s5.tacScratch,n,kBoxLink,dropped);t.mark_dropped+=dropped;
    const uint32_t capBefore=t.mark_dropped;add_boxes(t,s5.tacScratch.c,nc);const uint32_t lost=dropped+(t.mark_dropped-capBefore);
    const bool all=match_markers(t,first)&&!dropped;
    if(suppress_box_draw(s5.tacFlags,all,t.nmark-first,lost)) { // MARKERS-360 mk360:1 hides every kept box draw; tacboxes:1 the all-matched ones
        for(unsigned k=first;k<t.nmark;++k)t.mark[k].suppressed=true;
        ++t.boxes_suppressed;s5.tacSuppressLabel=true;return true;
    }
    return false;
}
// The frame closes (before publication / deferral): the tail's wedge-shaped draws are NOT between members; copy into the slot.
// ------------------------------------------------------------- MSL-HUD (hud_msl.h; docs/world-render/aircraft-auto.md section 14)
// xr_cockpit_model=...;mslhud:0|1|2 (default 1 = on; 0 = off, 2 on + dumps; stores:0 = off). Every kMslSampleEvery-th frame the HUD region's instrument
// draws are READ (vertices + indices from the mapped UPLOAD memory, like the tac sprites; nothing is recorded or replayed differently) into glyph quads; at
// the frame's close the decoder reads the MSSL field and a stable fall/rise becomes note_launch/note_rearm. Bounded: kMslDrawsPerFrame draws and
// kMslVertsPerFrame vertices per sampled frame.
constexpr unsigned kMslSampleEvery=6,kMslDrawsPerFrame=64,kMslVertsPerFrame=6144;
constexpr uint64_t kMslSummaryMs=5000;
int step5MslMode() noexcept {
    static std::atomic<int> s_mode{-1};
    int m=s_mode.load(std::memory_order_relaxed);
    if(m>=0)return m;
    auto* l=diagnostics::sink();if(!l)return 0; // not configured yet: off for this frame, asked again later
    m=acz::hudmsl::parse_mode(l->xr_cockpit_model());
    s_mode.store(m,std::memory_order_relaxed);acz::hudmsl::status().mode.store(uint32_t(m),std::memory_order_relaxed);
    return m;
}
// SP-STORES: does the special-weapon row drive the SP pylons? On with the reader (default), off with `spstores:0`, stores:0, mslhud:0 or no option string.
bool step5SpOn() noexcept {
    static std::atomic<int> s_sp{-1};
    const int m=s_sp.load(std::memory_order_relaxed);
    if(m>=0)return m==1;
    auto* l=diagnostics::sink();if(!l)return false; // not configured yet: asked again later
    const int v=acz::hudmsl::parse_sp(l->xr_cockpit_model())?1:0;
    s_sp.store(v,std::memory_order_relaxed);acz::hudmsl::status().sp_mode.store(uint32_t(v),std::memory_order_relaxed);
    return v==1;
}
void step5MslOpen(Step5State& s5) noexcept {
    s5.mslSample=false;
    if(step5MslMode()<=0)return;
    s5.mslSample=(++s5.mslFrameN%kMslSampleEvery)==0;
    if(s5.mslSample){s5.mslFrame.clear();s5.mslVerts=0;}
}
// One HUD-region instrument draw of a sampled frame (the caller holds what step5TacReadSprites' callers hold; this takes state().mutex itself).
void step5MslDraw(Step5State& s5,const gpu_copy::step3::Shape& d) noexcept {
    if(!s5.mslSample)return;
    namespace hm=acz::hudmsl;
    auto& f=s5.mslFrame;auto& sc=s5.mslScratch;
    if(f.draws>=kMslDrawsPerFrame||s5.mslVerts>=kMslVertsPerFrame){++f.unread;return;}
    if(d.topology!=hm::kTopoList&&d.topology!=hm::kTopoStrip){++f.draws;++f.rej_shape;return;}
    const uint32_t stride=d.vb_stride;
    if(stride<48||stride>256||!d.count||!d.vb_va||!d.vb_bytes){++f.unread;return;}
    auto& s=state();std::lock_guard lock(s.mutex);
    const uint8_t* vb=nullptr;
    if(step3MappedSpan_locked(d.vb_address,d.vb_va,d.vb_bytes,vb)){++f.unread;return;}
    auto vertex=[&](uint64_t i,hm::Vtx& o)->bool{
        if((i+1)*uint64_t(stride)>d.vb_bytes)return false;
        float raw[12];
        if(!guardedCpuRead(vb+i*stride,raw,sizeof raw))return false;
        o.x=raw[0];o.y=raw[1];o.q=raw[3];o.rgb[0]=raw[4];o.rgb[1]=raw[5];o.rgb[2]=raw[6];o.u=raw[8];o.v=raw[9]; // ps2_v: pos +0, COLOR0 +16, TEXCOORD +32
        return true;
    };
    unsigned nv=0,ni=0;
    if(!d.indexed) {
        if(d.count>hm::kMaxVerts){++f.unread;return;}
        for(uint32_t k=0;k<d.count;++k)if(!vertex(uint64_t(d.start)+k,sc.v[nv++])){++f.unread;return;}
    } else {
        const uint32_t isz=d.ib_stride==4?4u:d.ib_stride==2?2u:0u;
        if(!isz||!d.ib_va||!d.ib_bytes||d.count>hm::kMaxIdx||(uint64_t(d.start)+d.count)*isz>d.ib_bytes){++f.unread;return;}
        const uint8_t* ib=nullptr;
        if(step3MappedSpan_locked(d.ib_address,d.ib_va+uint64_t(d.start)*isz,uint64_t(d.count)*isz,ib)){++f.unread;return;}
        if(!guardedCpuRead(ib,s5.mslIbBytes,size_t(d.count)*isz)){++f.unread;return;}
        unsigned nu=0;
        for(uint32_t k=0;k<d.count;++k){uint32_t x=0;std::memcpy(&x,s5.mslIbBytes+size_t(k)*isz,isz);
            if((isz==2&&x==0xffffu)||(isz==4&&x==0xffffffffu)){sc.stream[ni++]=hm::kCut;continue;}
            const int64_t vi=int64_t(x)+d.base;if(vi<0||vi>=int64_t(hm::kCut)){sc.stream[ni++]=hm::kCut;continue;}
            sc.stream[ni++]=uint32_t(vi);
            if(nu<hm::kMaxVerts)sc.uniq[nu++]=uint32_t(vi);}
        std::sort(sc.uniq,sc.uniq+nu);nu=unsigned(std::unique(sc.uniq,sc.uniq+nu)-sc.uniq);
        for(unsigned k=0;k<nu;++k)if(!vertex(sc.uniq[k],sc.v[nv++])){++f.unread;return;}
    }
    s5.mslVerts+=nv;
    hm::add_draw(f,sc,nv,ni,d.indexed,d.topology,uint16_t(f.draws));
}
// Frame close (every produced frame): the sampled frame's reading -> signal; the summary; the dump lines (mslhud:2) spread over frames.
void step5MslClose(Step5State& s5) noexcept {
    const int mode=step5MslMode();
    if(mode<=0){s5.mslSample=false;return;}
    namespace hm=acz::hudmsl;
    namespace fd=acz::fxdiag;
    const uint64_t nowMs=GetTickCount64();const double now=double(nowMs);
    char b[fd::kLineMax];
    if(s5.mslSample) {
        s5.mslSample=false;
        const hm::Event ev=s5.mslDec.frame(s5.mslFrame,s5.mslWork,now);
        if(ev.rearm)acz::stores::signal().note_rearm();
        for(unsigned i=0;i<ev.launches;++i)acz::stores::signal().note_launch();
        {   // the absolute missile count caps the shown pylons (store_tracker.h set_remaining); sent only when the reading changed
            const uint32_t nv=s5.mslDec.has_baseline()?s5.mslDec.baseline():hm::kNoValue;
            if(nv!=hm::kNoValue&&nv!=hm::status().value.load(std::memory_order_relaxed))acz::stores::signal().set_remaining(nv);
        }
        hm::status().value.store(s5.mslDec.has_baseline()?s5.mslDec.baseline():hm::kNoValue,std::memory_order_relaxed);
        hm::status().launches.store(uint32_t(s5.mslDec.stats().launches),std::memory_order_relaxed);
        if((ev.rearm||ev.launches||ev.rebase)&&s5.mslEventLines<hm::kEventLines&&s5.diagQ.count+1<fd::LineQueue::kCap){
            ++s5.mslEventLines;hm::format_event(b,sizeof b,now,ev);s5.diagQ.push_text(b);}
        if(step5SpOn()) {   // SP-STORES: the special-weapon row (SAAM ---8) -> the SP mailbox (its own tracker; the same strel; spstores:0 = off)
            const hm::Event sev=s5.mslDec.sp_event();
            auto& sps=acz::stores::sp_signal();
            if(sev.rearm)sps.note_rearm();
            for(unsigned i=0;i<sev.launches;++i)sps.note_launch();
            const uint32_t snv=s5.mslDec.sp().has_baseline()?s5.mslDec.sp().baseline():hm::kNoValue;
            if(snv!=hm::kNoValue&&snv!=hm::status().sp_value.load(std::memory_order_relaxed))sps.set_remaining(snv);
            hm::status().sp_value.store(snv,std::memory_order_relaxed);
            hm::status().sp_launches.store(uint32_t(s5.mslDec.sp().launches()),std::memory_order_relaxed);
            if((sev.rearm||sev.launches||sev.rebase)&&s5.mslSpEventLines<hm::kEventLines&&s5.diagQ.count+1<fd::LineQueue::kCap){
                ++s5.mslSpEventLines;hm::format_sp_event(b,sizeof b,now,sev);s5.diagQ.push_text(b);}
        }
        // dump (mslhud:2): copy this frame's glyphs (the region only after the first hm::kDumpFull dumps)
        if(mode==2&&s5.mslDumps<hm::kDumpMax&&!s5.mslDumpN&&(now-s5.mslLastDumpMs>=hm::kDumpGapMs||ev.launches||ev.rearm||ev.rebase)&&s5.mslFrame.n&&s5.diagQ.count+1<fd::LineQueue::kCap) {
            const bool full=s5.mslDumps<hm::kDumpFull;auto& df=s5.mslDumpFrame;df=s5.mslFrame;df.n=0;
            for(unsigned i=0;i<s5.mslFrame.n;++i)if(full||hm::in_region(s5.mslFrame.g[i]))df.g[df.n++]=s5.mslFrame.g[i];
            ++s5.mslDumps;s5.mslLastDumpMs=now;s5.mslDumpCursor=0;s5.mslDumpN=df.n;s5.mslDumpFull=full;
            hm::format_dump_head(b,sizeof b,s5.nextFrameId+1,now,s5.mslDumps,s5.mslFrame,df.n,full,s5.mslDec.last());s5.diagQ.push_text(b);
        }
    }
    // pending dump lines: at most 4 per frame, only while the queue has room for others too
    for(unsigned line=0;line<4&&s5.mslDumpN&&s5.diagQ.count+4<fd::LineQueue::kCap;++line) {
        int o=std::snprintf(b,sizeof b,"msl-hud-glyph;dump=%u;first=%u;g=",s5.mslDumps,s5.mslDumpCursor);
        for(unsigned k=0;k<hm::kDumpPerLine&&s5.mslDumpCursor<s5.mslDumpN&&o>0&&size_t(o)<sizeof b-80;++k) {
            const int w=hm::format_glyph(b+o,sizeof b-size_t(o),s5.mslDumpFrame.g[s5.mslDumpCursor]);if(w<0)break;
            o+=w;++s5.mslDumpCursor;
            if(s5.mslDumpCursor<s5.mslDumpN&&k+1<hm::kDumpPerLine&&size_t(o)+1<sizeof b){b[o++]='|';b[o]=0;}
        }
        s5.diagQ.push_text(b);
        if(s5.mslDumpCursor>=s5.mslDumpN)s5.mslDumpN=0;
    }
    if(nowMs-s5.mslLastSummaryMs>=kMslSummaryMs&&s5.diagQ.count+1<fd::LineQueue::kCap) {
        s5.mslLastSummaryMs=nowMs;hm::format_summary(b,sizeof b,now,mode,s5.mslDec);s5.diagQ.push_text(b);
        if(step5SpOn()&&s5.diagQ.count+1<fd::LineQueue::kCap){hm::format_sp_summary(b,sizeof b,now,s5.mslDec);s5.diagQ.push_text(b);}   // SP-STORES (unconditional while on)
        s5.mslDec.window_reset();
    }
}
void step5TacClose(Step5State& s5,const Step5Frame& f) noexcept {
    s5.slot[f.slot].tacValid=false;
    if(!s5.tacOpen)return;
    auto& t=s5.tacCur;
    t.wedge_tail=s5.tacWedgePending;s5.tacWedgePending=0;
    // OBJECT-INTERP: the game's marker sprites were matched at frame time (above, during the HUD tail); the consumer's tracker gets display-time positions.
    // BOX-LOCK (markers-360.md section 15): the frame-time position is kept in tf (the sprites' own time) for the consumer's sprite matching.
    if(f.interp.valid)for(unsigned i=0;i<t.nobj&&i<acz::tac::kObjCap;++i)if(t.obj[i].td_valid){for(int k=0;k<3;++k){t.obj[i].tf[k]=t.obj[i].t[k];t.obj[i].t[k]=t.obj[i].td[k];}t.obj[i].tf_valid=1;}
    s5.tacSlot[f.slot]=t;s5.slot[f.slot].tacValid=true;
    ++s5.tacFrames;s5.tacObjSum+=t.nobj;s5.tacMarkSum+=t.nmark;s5.tacBoxDraws+=t.box_draws;s5.tacLabelDraws+=t.label_draws;
    for(unsigned k=0;k<t.nmark;++k)if(t.mark[k].obj)++s5.tacMatchSum;
    s5.tacSupBoxes+=t.boxes_suppressed;s5.tacSupLabels+=t.labels_suppressed;s5.tacUnreadBox+=t.box_unreadable;s5.tacUnreadLabel+=t.label_unreadable;
    if(t.wedge)++s5.tacWedgeFrames;s5.tacWedgeTail+=t.wedge_tail;
}
void step5HudMember_locked(Step5State& s5,Step5Frame& f,const gpu_copy::step3::Shape& live,bool hudArm) noexcept {
    if(!hudReplayConfigured())return;
    step5TacMember(s5,hudArm); // TACTICAL: HUD tail restarts here (key off: returns)
    auto& h=f.hud;
    if(hudArm) {
        h=Step5Hud{};
        if(!s5.hudReady)h.armWhy=s5.hudArmWhy?s5.hudArmWhy:"hud-unavailable";
        else if(!s5.slot[f.slot].hudRt)h.armWhy="hud-texture-missing";
        else if(std::strcmp(live.game_rt_status,"resolved")!=0)h.armWhy="scene-rt-unresolved";
        else if(!live.game_rt||live.game_rt!=s5.worldSource)h.armWhy="scene-rt-not-selection-rt";
        else {h.armed=true;h.scene=uintptr_t(live.game_rt);}
        h.mSplit=h.armed&&hudMarkersConfigured()&&s5.hudMarkersReady&&s5.slot[f.slot].hudMarkerRt;
        h.radar=h.armed&&hudRadarConfigured();
        hudRegionBegin(f.list,f.generation,h.armed?h.scene:0,h.radar);
        step5EffectsArm_locked(s5,f); // J-effects-layer (key off: resets the zero state and returns)
        return;
    }
    if(!h.armed)return;
    if(h.draws||h.refused||h.noTarget||h.mDraws||h.mRefused||h.dg.seen) {
        ++h.resets;h.draws=h.refused=h.depthTested=h.noTarget=0;h.capped=h.cleared=false;h.firstWhy=nullptr;
        h.rd=gpu_copy::step5::HudRadar{};h.dg=gpu_copy::step5::HudRadarDiag{}; // J-hud-radar (untouched with the key off)
        for(auto& r:h.reason)r=0;
        h.mDraws=h.mRefused=h.mBoxes=h.mLabels=0;h.mCleared=h.dsCleared=false;h.mFirstWhy=nullptr;h.mState=gpu_copy::step5::HudMarkerState{};
        for(auto& r:h.mReason)r=0;
    }
    hudRegionReopen();
}
// J-hud-radar diagnostics (step-1 mutex held; no allocation): one character per draw (stencil test function) and its disposition, plus the ORIGINAL
// hud_decide verdict (t.radar is false) of every draw after the first excursion = what the unsegmented replay would have said about it.
void step5RadarDiag(Step5Hud& h,const gpu_copy::step3::Shape& d,const gpu_copy::step5::HudTarget& t,char disp,bool post) noexcept {
    h.dg.add_draw(gpu_copy::step5::radar_func_char(d.pso),disp);
    if(post)h.dg.add_verdict(gpu_copy::step5::hud_decide(d,t).why);
}
// A draw of an excursion (a proven other target, or an unresolved one while away): skipped, never refused. Its stencil writes are assumed to land
// in the other target's own depth-stencil (counted in excursionWriters; the live run checks the result).
void step5RadarAway(Step5Hud& h,const gpu_copy::step3::Shape& d,const gpu_copy::step5::HudTarget& t,const char* why) noexcept {
    using namespace gpu_copy::step5;
    ++h.rd.awaySkipped;if(radar_a3050_shaped(d))++h.rd.a3050;if(hud_stencil_writer(d.pso))++h.rd.excursionWriters;
    if(!h.dg.haveEnd) {
        auto& g=h.dg;g.haveEnd=true;g.endWhy=why?why:"draw-on-other-target";g.endOrdinal=d.draw_ordinal;g.endRt=d.game_rt;g.endWidth=d.game_rt_width;
        g.endHeight=d.game_rt_height;g.endFormat=d.game_rt_format;g.endOmCount=d.om_count;g.endStatus=d.game_rt_status;
    }
    step5RadarDiag(h,d,t,'a',true);
}
__declspec(noinline) void step5HudDraw(ID3D12GraphicsCommandList* self,uint64_t countInstances,uint64_t startBase,uint64_t firstIndexed) noexcept {
    if(!hudReplayConfigured())return;
    auto* st=g_step1.load();auto* s5p=g_step5.load();if(!st||!s5p||!s5p->hudReady)return;
    auto& s5=*s5p;
    try {
        void* key=nullptr;bool acmDraw=false;Step5EffId effId;std::shared_ptr<gpu_copy::ObservationFeed> effFeed;
        acz::tac::WedgeFacts tacWedge;bool tacWedgeKnown=false; // TACTICAL: shader/blend identity for the firing-wedge test (filled only with tac on)
        const bool hudLate=gpu_copy::step5::hud_late_flag().load(std::memory_order_relaxed);gpu_copy::step5::HudLateDraw lateFacts;ID3D12PipelineState* lateClone=nullptr,*lateGame=nullptr; // HUD-LATE (hudlate:1); F6 marker clone / game PSO
        bool fxSmoke=false;float fxRoots[6]{};const char* fxRootsWhy="not-read"; // EFFECTS-OUTSIDE (C) fxmem:1: a smoke draw, its cb10/cb11/cb12 (flag off: never set)
        acz::fxcensus::RootShape fxCenRoots; // EFFECTS-OUTSIDE (D) fxmem:3 (flag off: never filled)
        uint32_t dgPs=0,dgVs=0;uint8_t dgBl[6]={}; // flare diagnostics (fxsig:f1a4e000): this draw's shader prefixes and blend tuple (flag off: never set)
        char fxSigRoots[72]={0}; // EFFECTS-OUTSIDE (D) fxsig: root summary of a draw of the filtered pixel shader (docs section 12)
        uint64_t fxTrailKey=0; // EFFECTS-OUTSIDE (M) fxtrail: the draw's binding key (first descriptor-table root argument; 0 = unknown)
        uint64_t fxTrailPso=0,fxTrailHeap=0;uint32_t fxTrailParam=UINT32_MAX; // (M4) the draw's host identity (game PSO, heaps, the key's root parameter)
        Step5AtlasHost fxAtlasHost;bool fxAtlasRead=false; // (M6) fxatlas: the draw's atlas facts (read only while fxatlas is on)
        {auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);
         auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));if(it==s.states.end())return;
         // DrawCPT-identity draws belong to the cockpit seam (members or refused there), never to the HUD.
         auto p=s.pipelines.find(it->second.pso.get());if(p!=s.pipelines.end()&&p->second.drawcpt_identity_matched)return;
         // J-world-objects: DrawACM-identity draws belong to the world seam (member or refused there), never to the HUD.
         if(p!=s.pipelines.end()&&p->second.acm_world_matched&&worldObjectsConfigured())return;
         acmDraw=p!=s.pipelines.end()&&p->second.acm_world_matched; // J-effects-layer: never an effect candidate, key or no key
         if(p!=s.pipelines.end()&&step5DiagFlareOn()) {
             const auto& v=p->second;dgPs=step5Hex8(v.hudPs);dgVs=step5Hex8(v.hudVs);if(!dgPs)dgPs=1;
             dgBl[0]=uint8_t(v.blend_src);dgBl[1]=uint8_t(v.blend_dst);dgBl[2]=uint8_t(v.blend_op);dgBl[3]=uint8_t(v.blend_src_alpha);dgBl[4]=uint8_t(v.blend_dst_alpha);dgBl[5]=uint8_t(v.blend_op_alpha);
         }
         if(p!=s.pipelines.end()&&(acz::tac::producer_flags()&acz::tac::kProducerOn)) {
             const auto& v=p->second;tacWedge.vs8=step5Hex8(v.hudVs);tacWedge.ps8=step5Hex8(v.hudPs);
             tacWedge.src=uint32_t(v.blend_src);tacWedge.dst=uint32_t(v.blend_dst);tacWedge.op=uint32_t(v.blend_op);tacWedgeKnown=true;
         }
         if(hudLate&&p!=s.pipelines.end()){const auto& v=p->second;lateFacts.vs8=step5Hex8(v.hudVs);lateFacts.ps8=step5Hex8(v.hudPs); // HUD-LATE: the text class identity
             lateFacts.src=uint32_t(v.blend_src);lateFacts.dst=uint32_t(v.blend_dst);
             if(v.hudLateClone&&gpu_copy::step5::hud_late_marker_flag().load(std::memory_order_relaxed)){lateClone=v.hudLateClone.get();lateGame=it->second.pso.get();}} // F6: marker clone only while the consumer shades non-default
         if(effectsConfigured()) { // J-effects-smoke-fix: shader/blend identity, root-layout UAV exposure and the observation feed of the list (effects key only)
             if(p!=s.pipelines.end()) {
                 const auto& v=p->second;effId.vs8=step5Hex8(v.hudVs);effId.ps8=step5Hex8(v.hudPs);
                 effId.blend[0]=uint8_t(v.blend_src);effId.blend[1]=uint8_t(v.blend_dst);effId.blend[2]=uint8_t(v.blend_op);
                 effId.blend[3]=uint8_t(v.blend_src_alpha);effId.blend[4]=uint8_t(v.blend_dst_alpha);effId.blend[5]=uint8_t(v.blend_op_alpha);
             }
             if(it->second.bindings.root)effId.root=step5RootHasUav(*it->second.bindings.root);else effId.rootUnknown=true;
             effFeed=it->second.copyObservation;
             if(effectsDualConfigured()){effId.game=it->second.pso.get();if(p!=s.pipelines.end()){effId.clone=p->second.effectsDualClone.get();effId.light=p->second.effectsExplosionLight.get();}}
             if(step5FxSmokeOn()&&step5FxSmokeDraw(effId)){fxSmoke=true;fxRootsWhy=step5FxSmokeRoots_locked(it->second,fxRoots);} // EFFECTS-OUTSIDE (C) fxmem:1
             if(step5FxCensusOn())step5FxCensusRoots_locked(it->second,fxCenRoots); // EFFECTS-OUTSIDE (D) fxmem:3
             if(const uint32_t sg=step5FxSigFilter();sg&&sg==effId.ps8)step5FxSigRoots_locked(it->second,fxSigRoots,sizeof fxSigRoots); // fxsig: only for the filtered signature
             if(step5FxTrailWanted())fxTrailKey=step5FxTrailKey_locked(it->second,fxTrailParam,fxTrailPso,fxTrailHeap); // EFFECTS-OUTSIDE (M) fxtrail / (M4)
             if(fxTrailKey&&gpu_copy::step5::effects_puff_atlas_flag().load(std::memory_order_relaxed)) { // (M6) fxatlas: capture pipeline, heaps, root tables
                 step5FxAtlasHost_locked(it->second,p!=s.pipelines.end()?&p->second:nullptr,fxTrailParam,fxAtlasHost);fxAtlasRead=true;}
         }}
        Step5HudCall call;bool record=false;Step5EffCall ecall;bool eRecord=false;Step5SmokeCall scall;bool sRecord=false; // sRecord: EFFECTS-OUTSIDE (C) fxmem:4
        Step5SpriteCall pcall;bool pRecord=false; // pRecord: EFFECTS-OUTSIDE (F) fxmem:6
        {std::lock_guard lock(st->mutex);auto& s3=st->s3;auto& f=s5.frame;auto& h=f.hud;
         using namespace gpu_copy::step5;
         if(s5.disabled||!f.active||f.closed||f.stopped||!h.armed||g_step1Phase.load()!=static_cast<int>(Step1Phase::continuous))return;
         if(step1Route(key)!=f.list)return;
         // J-effects-layer: the effects layer looks at the draws of the whole frame after the first member, also while the HUD region is
         // ended by an off-screen pass (key off: f.eff.armed is false and this is the previous `return` exactly).
         const bool ended=hudRegionEnded(f.list,f.generation);
         if(ended&&!f.eff.armed)return;
         s3.aCount=UINT(countInstances);s3.aInstances=UINT(countInstances>>32);s3.aStart=UINT(startBase);
         s3.aBase=INT(uint32_t(startBase>>32));s3.aStartInstance=UINT(firstIndexed);s3.aIndexed=(firstIndexed>>32)!=0;
         step3Capture(*st,key,s3.live,true);
         const auto& d=s3.live;
         if(d.generation!=f.generation)return;
         if(tacWedgeKnown&&s5.tacOpen&&!acmDraw) { // TACTICAL: the firing-only HUD wedge (D-318); counted, never changes the replay
             tacWedge.indexed=d.indexed;tacWedge.count=d.count;tacWedge.blend=d.pso.blend0;tacWedge.depth=d.pso.depth_enable;
             tacWedge.depth_write=d.pso.depth_write;tacWedge.depth_func=d.pso.depth_func;
             if(acz::tac::is_wedge(tacWedge))++s5.tacWedgePending;
         }
         if(f.eff.armed&&!acmDraw){
             const bool predClear=effFeed&&effFeed->game_predication_clear();
             eRecord=step5EffectsConsider_locked(*st,s5,f,d,ended,effId,predClear,ecall); // J-effects-layer (speculative, predicated)
             if(fxSmoke)step5FxSmokeCapture(s5,d,fxRootsWhy?nullptr:fxRoots,fxRootsWhy); // EFFECTS-OUTSIDE (C) fxmem:1: capture only (the replay above is unchanged)
             if(fxSmoke&&eRecord)sRecord=step5FxSmokeGpuPlan_locked(*st,s5,f,d,ecall,scall); // (C) fxmem:4: per eye INSTEAD of the window replay (else unchanged)
             step5FxSigDraw(s5,d,effId,fxSigRoots); // EFFECTS-OUTSIDE (D) fxsig: per-draw log of one pixel-shader prefix (before the census: it resets the gate verdict)
             step5FxCensusDraw(s5,d,effId,fxCenRoots); // EFFECTS-OUTSIDE (D) fxmem:3: census only (no frame open = returns)
             if(s5.fxSprOpen){const int sc=step5FxSpriteClass(effId,d); // EFFECTS-OUTSIDE (E) fxmem:5: read + CPU plan (closed: skipped)
                 // (M6) fxatlas: this draw can draw the atlas group (the atlas ready, the root tables checked); refusals counted for the fx-puff-atlas line
                 bool atlasHost=false;
                 if(fxAtlasRead&&sc==1&&s5.fxTrailOpen&&s5.fxTrailOpt.atlas) {
                     if(s5.atlasReady)step5FxAtlasClassify(fxAtlasHost); // (M6b) the game's sampler heap (outside the hook-state mutex)
                     if(!s5.atlasReady){++s5.fxAtlasHostNo;s5.fxAtlasHostWhy="atlas-not-ready";}
                     else if(fxAtlasHost.why){++s5.fxAtlasHostNo;s5.fxAtlasHostWhy=fxAtlasHost.why;
                         std::memcpy(s5.fxAtlasHostLayout,fxAtlasHost.layout,sizeof s5.fxAtlasHostLayout);} // (M6b) logged as lay= on fx-puff-atlas
                     else atlasHost=true;
                     if(!s5.fxAtlasLayoutLogged&&fxAtlasHost.layout[0]){s5.fxAtlasLayoutLogged=true;std::memcpy(s5.fxAtlasFirstLayout,fxAtlasHost.layout,sizeof s5.fxAtlasFirstLayout);
                         s5.fxAtlasFirstWhy=fxAtlasHost.why;s5.fxAtlasFirstHeaps=fxAtlasHost.ngame;s5.fxAtlasFirstSampler=fxAtlasHost.sampler!=nullptr;s5.fxAtlasLayoutPending=true;}
                 }
                 step5FxSpriteCapture(s5,d,sc,f.eff.sp.on&&eRecord&&!sRecord,fxTrailKey,fxTrailPso,fxTrailHeap,fxTrailParam,atlasHost);
                 if(sc>=0&&eRecord&&!sRecord&&f.eff.sp.on)pRecord=step5FxSpriteGpuPlan_locked(*st,s5,f,d,ecall,pcall,effId.light,fxAtlasRead?&fxAtlasHost:nullptr); // (F) fxmem:6: per eye INSTEAD of the window replay
                 s5.fxSprLastPrepared=false;step5FxTrailDrop(s5,s5.fxTrailSelN?"trail-stale-selection":nullptr);}}
         if(!ended)[&]{ // the HUD region itself (unchanged key off); its early returns leave this block only
         const bool radar=h.radar; // J-hud-radar: segmented region + radar group (false = the original behaviour below, line for line)
         HudTarget t;t.format=s5.hudFormat;t.ds_format=s5.hudDsFormat;t.width=s5.worldW;t.height=s5.worldH;t.scene=h.scene;t.depth_gap=h.depthGap;
         Step5RadarView rv;if(radar)rv=hudRegionRadarView(f.list,f.generation);
         // No render target bound: the draw cannot write the scene colour (not a HUD draw).
         if(d.om_known&&!d.om_gap&&d.om_set&&d.om_count==0){
             ++h.noTarget;
             if(radar){if(radar_a3050_shaped(d))++h.rd.a3050;if(hud_stencil_writer(d.pso))++h.rd.excursionWriters;step5RadarDiag(h,d,t,'t',rv.excursions>0);}
             return;}
         // A proven other RT 0: the HUD region ends here (e.g. the tone-map pass into the output); with the radar key it is an excursion.
         if(d.om_count>=1&&std::strcmp(d.game_rt_status,"resolved")==0&&d.game_rt!=h.scene) {
             if(!radar){hudRegionEnd(f.list,f.generation,uintptr_t(d.game_rt),true,"draw-on-other-target");return;}
             step5RadarAway(h,d,t,hudRegionOtherTarget(f.list,f.generation,uintptr_t(d.game_rt),"draw-on-other-target"));
             return;
         }
         if(radar&&rv.away) {
             // Still on the excursion (an unresolved target): skipped; the scene target drawn again ends it.
             if(d.om_count==1&&std::strcmp(d.game_rt_status,"resolved")==0&&d.game_rt==h.scene){hudRegionSceneReturn(f.list,f.generation);rv.away=false;rv.window=false;rv.late=true;}
             else {step5RadarAway(h,d,t,rv.firstWhy);return;}
         }
         HudVerdict v;int action=radar_replay;
         if(radar){const RadarVerdict rr=hud_radar_decide(d,t,h.rd,rv.late,rv.window);action=rr.action;v=rr.v;}
         else v=hud_decide(d,t);
         // HUD-LATE (hudlate:1; hud_late.h; markers-360.md section 18): a draw skipped as late after the post-tone-map scene resume that is of the HUD text
         // class (the radio subtitles) is replayed into the HUD INSTRUMENT image when the plain hud_decide accepts it and the caps allow. It opens no radar
         // window, is never classified (no box / label / lock test) and a refusal or a cap only skips it (counted; the frame stays valid). Key off: unchanged.
         bool lateText=false;
         if(action==radar_skip_late&&hudLate) {
             lateFacts.blend=d.pso.blend0;lateFacts.depth=d.pso.depth_enable;lateFacts.depth_write=d.pso.depth_write;lateFacts.depth_func=d.pso.depth_func;
             lateFacts.stencil=d.pso.stencil;lateFacts.front_func=d.pso.front_func;lateFacts.back_func=d.pso.back_func;lateFacts.stencil_ops_keep=d.pso.stencil_ops_keep;
             const bool text=d.pso.known&&gpu_copy::step5::hud_late_text(lateFacts)==nullptr;
             HudVerdict lv;if(text){HudTarget tr=t;radar_target(h.rd,tr);lv=hud_decide(d,tr);}
             const bool needRtL=!h.cleared,needDsL=h.mSplit?!h.dsCleared:needRtL;
             const unsigned clearsL=unsigned(needRtL)+(s5.slot[f.slot].hudDs!=nullptr&&needDsL?1u:0u);
             const bool capped=h.draws+h.mDraws>=hud_draw_cap_for(radar)||h.commands+hud_per_draw+clearsL>hud_command_cap_for(radar);
             switch(gpu_copy::step5::hud_late_decide(true,text,lv.why,capped)) {
             case gpu_copy::step5::hud_late_replay:lateText=true;action=radar_replay;v=lv;break;
             case gpu_copy::step5::hud_late_skip_refused:++h.late.refused;h.late.lastRefusal=lv.why;break;
             case gpu_copy::step5::hud_late_skip_capped:++h.late.capped;break;
             default:++h.late.other;break;
             }
         }
         if(action==radar_skip_a3050||action==radar_skip_notequal||action==radar_skip_late) {
             // Skipped by class (counted, never refused, the frame stays valid).
             if(action==radar_skip_a3050){++h.rd.a3050;if(hud_stencil_writer(d.pso))++h.rd.excursionWriters;}
             else {if(action==radar_skip_notequal)++h.rd.notEqualSkipped;else ++h.rd.lateSkipped;radar_note_not_replayed(h.rd,d);}
             step5RadarDiag(h,d,t,action==radar_skip_a3050?'n':action==radar_skip_notequal?'q':'l',rv.excursions>0);
             return;
         }
         // J-hud-markers: classify first (pure; Shape only, so refused draws are classified alike). Split off: always an instrument.
         HudClass cls=h.mSplit&&!lateText?hud_classify(h.mState,hud_depth_tested(d),d.indexed,d.pso_route):hud_instrument; // HUD-LATE text: never classified
         // LOCK-SHAPE (markers-360.md section 17.4; mk360:3 + mk360lock, default on): an instrument draw after this frame's first box draw that IS the lock
         // diamond (45-degree square in a box's colour within satellite reach) goes into the marker image like a box (the consumer joins it to the box's
         // stamp). Never for a refused draw; the box / label sequence of hud_classify is untouched. mk360cen:1 logs the census lines around it.
         bool lockMoved=false,lockTested=false;acz::tac::LockShape lockFacts;unsigned lockN=0;
         if(s5.tacOpen&&!lateText&&s5.tacCur.box_draws&&cls!=hud_marker_box)++s5.tacAfterBox;
         if(h.mSplit&&!lateText&&cls==hud_instrument&&s5.tacOpen&&s5.tacCur.box_draws&&(s5.tacFlags&acz::tac::kProducerLockShape)) { // LOCK-WHY: gates before the test
             if(v.why)++s5.tacLockWhy.gate_refused;else if(!s5.tacCur.nmark)++s5.tacLockWhy.gate_nobox;else if(d.count>48)++s5.tacLockWhy.gate_big;
         }
         if(h.mSplit&&!lateText&&cls==hud_instrument&&!v.why&&s5.tacOpen&&s5.tacCur.nmark&&d.count<=48&&(s5.tacFlags&(acz::tac::kProducerLockShape|acz::tac::kProducerLockCensus))) {
             lockTested=true;lockMoved=step5TacLockShape(s5,d,&lockFacts,&lockN);
             if(lockMoved){cls=hud_marker_box;++s5.tacLockMoved;}else if(lockFacts.ok)++s5.tacLockRejected;
         }
         if(s5.tacOpen&&!lateText&&(s5.tacFlags&acz::tac::kProducerLockCensus))
             step5TacLockCensus(s5,d,lockMoved?"lock-moved":cls==hud_marker_box?"box":cls==hud_marker_label?"label":"instrument",tacWedge.vs8,tacWedge.ps8,
                                cls==hud_marker_box&&!lockMoved,lockTested?&lockFacts:nullptr,lockN);
         if(h.mSplit&&!lateText&&!lockMoved&&cls==hud_instrument&&(s5.tacFlags&acz::tac::kProducerLockDump))step5TacLockDump(s5,d,v.why!=nullptr,lockTested,lockFacts,lockN,tacWedge.vs8,tacWedge.ps8); // mk360ld:1
         const bool marker=cls!=hud_instrument;
         if(!marker&&!lateText)step5MslDraw(s5,d); // MSL-HUD: a sampled frame's instrument draws -> glyph quads (read only)
         if(hud_rs_skip(d,hud_rs_skip_flag().load(std::memory_order_relaxed))) {
             // HUD-RS-SKIP (gpu_copy_step5.h): a colour SUBTRACT/REV_SUBTRACT draw (the sun flare) is skipped by class: not replayed into either private
             // image, counted, never refused (the frame stays valid). Classified first so the marker label rule sees the same draw sequence as before.
             ++h.rsSkipped;if((d.pso.depth_enable&&d.pso.depth_write)||hud_stencil_writer(d.pso))++h.rsDsw;
             if(d.pso.depth_enable&&d.pso.depth_write)h.depthGap=true; // W10-1 P1#2: later depth-TESTED draws are refused (stencil writers: radar gap below)
             if(radar){radar_note_not_replayed(h.rd,d);step5RadarDiag(h,d,t,'s',rv.excursions>0);}
             if(dgPs)step5FlareLog(s5,d,dgPs,dgVs,dgBl,"rs-skip","none",
                 acz::fxdiag::flare_diag_class(cls==hud_marker_box,cls==hud_marker_label,h.mSplit&&h.mState.text_known&&d.pso_route==h.mState.text_route),false);
             return;
         }
         if(marker&&!lockMoved&&s5.tacOpen&&!v.why&&step5TacMarker(s5,d,cls==hud_marker_box))return; // TACTICAL: suppressed (drawn by the consumer)
         const bool hasDs=s5.slot[f.slot].hudDs!=nullptr;
         const bool needRt=marker?!h.mCleared:!h.cleared,needDs=h.mSplit?!h.dsCleared:needRt;
         const unsigned clears=unsigned(needRt)+(hasDs&&needDs?1u:0u);
         if(!v.why&&(h.draws+h.mDraws>=hud_draw_cap_for(radar)||h.commands+hud_per_draw+clears>hud_command_cap_for(radar))){v.why="hud-cap";v.category=hud_cap;h.capped=true;}
         // W8-3 F1: the replay class `instrument` is the fallback; the diagnostic keeps only positively identified chrome (label / learned text route)
         const char* dgCls=acz::fxdiag::flare_diag_class(cls==hud_marker_box,cls==hud_marker_label,h.mSplit&&h.mState.text_known&&d.pso_route==h.mState.text_route);
         if(v.why) {
             if(dgPs)step5FlareLog(s5,d,dgPs,dgVs,dgBl,v.why,"none",dgCls,v.depth_tested); // flare diagnostics
             if(marker){++h.mRefused;++h.mReason[v.category];if(!h.mFirstWhy)h.mFirstWhy=v.why;s5.hudMarkerLastWhy=v.why;}
             else {++h.refused;++h.reason[v.category];if(!h.firstWhy)h.firstWhy=v.why;}
             if(radar) {
                 radar_note_not_replayed(h.rd,d);
                 if(std::strcmp(v.why,"stencil-writer-skipped")==0)++h.rd.gapRefused;
                 else if(std::strncmp(v.why,"radar-",6)==0)++h.rd.equalRefused;
                 step5RadarDiag(h,d,t,'x',rv.excursions>0);
             }
             s5.hudLastWhy=v.why;return;
         }
         if(dgPs)step5FlareLog(s5,d,dgPs,dgVs,dgBl,"admitted",marker?"markers":"hud",dgCls,v.depth_tested); // flare diagnostics
         if(lateText&&lateClone&&lateGame){call.clone=lateClone;call.game=lateGame;h.commands+=2;} // F6: SetPipelineState(clone) before and (game) after the replay Draw
         const auto& sl=s5.slot[f.slot];
         call.rtv=marker?sl.hudMarkerRtv:sl.hudRtv;call.dsv=sl.hudDsv;call.useDs=d.pso.dsv!=0&&sl.hudDs;
         if(needRt||(hasDs&&needDs)) {
             call.clear=needRt;call.clearDs=hasDs&&needDs;call.dsFlags=D3D12_CLEAR_FLAGS(s5.hudDsClear);
             if(needRt){if(marker)h.mCleared=true;else h.cleared=true;}
             if(h.mSplit)h.dsCleared=true;
             h.commands+=clears;
         }
         call.omCount=s3.omCount;call.omSingle=s3.omSingle;call.omRtv=s3.omRtv;call.omDsv=s3.omDsv;call.omDsvHandle=s3.omDsvHandle;
         call.indexed=d.indexed;call.count=d.count;call.instances=d.instances;call.start=d.start;call.base=d.base;call.startInstance=d.start_instance;
         if(marker){++h.mDraws;if(cls==hud_marker_box)++h.mBoxes;else ++h.mLabels;}else ++h.draws;
         if(d.pso.depth_enable&&d.pso.depth_write)++s5.hudDsWriters; // J-effects-content: would write the shared depth-stencil (diagnostic counter only)
         h.commands+=hud_per_draw;if(v.depth_tested)++h.depthTested;record=true;
         if(radar) {
             radar_note_replayed(h.rd,d,rv.late);
             if(lateText)++h.late.draws; // HUD-LATE: never a window opener
             else if(rv.late&&!rv.window){hudRegionRadarWindow(f.list,f.generation,true);++h.rd.windows;} // an opener (REPLACE writer / EQUAL test, see hud_radar_decide)
             step5RadarDiag(h,d,t,lateText?'T':'r',rv.excursions>0);
         }
         }();}
        if(eRecord){if(sRecord)step5RecordSmoke(self,ecall,scall);else if(pRecord)step5RecordSprite(self,ecall,pcall);else step5RecordEffect(self,ecall);} // fxmem:4/6: the eye images, not the window image
        if(record)step5RecordHud(self,call);
    } catch(...) {
        std::lock_guard lock(st->mutex);auto& h=s5.frame.hud;
        if(s5.frame.active&&h.armed){++h.refused;++h.reason[gpu_copy::step5::hud_state];if(!h.firstWhy)h.firstWhy="hud-exception";}
    }
}
// Out of line: the temporaries live in these frames, not in step5Draw's.
__declspec(noinline) void step5ClearDetail(Step5Detail& d) noexcept {d=Step5Detail{};}
__declspec(noinline) void step5ClearFrame(Step5Frame& f) noexcept {f=Step5Frame{};}
bool step5IsSelected(const Step1State& st,uint64_t ordinal) noexcept {
    for(unsigned i=0;i<st.s3.memberCount;++i)if(st.s3.members[i].shape.draw_ordinal==ordinal)return true;
    return false;
}
// A list generation in the cockpit range could not start a frame (yet).
void step5Pending(Step5State& s5,Step1Route list,uint64_t gen,int kind,const char* why) noexcept {
    s5.pend.set=true;s5.pend.list=list;s5.pend.gen=gen;s5.pend.kind=kind;s5.pend.why=why;
}
// J-cockpit-fov F2 (gpu_copy_step5_precockpit_observe): one sampled DrawCPT draw that precedes the
// selected cockpit members (ordinal < members[0]). Step-1 mutex held; s3.live was just captured by
// step5Draw for this draw. Read-only: no command, no game state, no step-3/5 state is written
// (the record and its SrtData copy are the observer's own); the SrtData bytes come from the
// CPU-mapped UPLOAD memory under the inline path's rules (no GPU readback).
__declspec(noinline) void step5PrecockpitObserve(Step1State& st,const Step5State& s5,Step1Route route,uint64_t pipelineId) noexcept {
    namespace pc=gpu_copy::precockpit;
    const auto& s3=st.s3;const auto& live=s3.live;
    if(!s3.memberCount||!(live.draw_ordinal<s3.members[0].shape.draw_ordinal))return;
    uint64_t frame=0;
    {std::lock_guard lock(g_pcMutex);if(!g_pc.admit(route,live.generation))return;frame=g_pc.sampled;}
    auto& r=g_pcScratch;r.marker=false;
    r.frame=frame;r.list=route;r.generation=live.generation;r.ordinal=live.draw_ordinal;r.first_member=s3.members[0].shape.draw_ordinal;
    r.pso=live.pso_route;r.id=pipelineId;r.root=live.root_route;r.t10_va=live.t10_va;r.vb_bytes=live.vb_bytes;
    const Step5Class c=step5ClassOf(live);
    r.topology=c.topology;r.ib=c.ib;r.vb=c.vb;r.t10=c.t10;r.cut=c.cut;r.rtv=c.rtv;r.dsv=c.dsv;r.samples=c.samples;r.depth_func=c.depthFunc;
    r.depth_enable=c.depthEnable;r.depth_write=c.depthWrite;r.stencil=c.stencil;r.blend=live.pso.blend0;r.pso_known=live.pso.known;
    r.indexed=live.indexed;r.count=live.count;r.instances=live.instances;r.start=live.start;r.base=live.base;r.start_instance=live.start_instance;
    const char* why=step5StateWhy(st,s5,live);r.state=why?why:"ok";
    int index=0;const char* out=step5ClassVerdict(s5,c,index);
    r.klass=out?out:(index>=0?"selected":"family");r.class_index=out?-1:(index>=0?index:-1-index);
    {auto& s=state();std::lock_guard lock(s.mutex);const uint8_t* p=nullptr;
     if(const char* bad=step3MappedSpan_locked(live.t10_address,live.t10_va,416,p))r.srt=bad;
     else if(!guardedCpuRead(p,r.bytes.data(),416))r.srt="read-fault";
     else r.srt="read";}
    std::lock_guard lock(g_pcMutex);g_pc.push(r);
}
// Worker (no producer lock held): format and write the queued records (dedicated log only).
void step5PrecockpitDrain(acz::feature_log::Handle log) noexcept {
    if(!precockpitConfigured())return;
    namespace pc=gpu_copy::precockpit;
    static pc::Record r;static pc::Derived d;static char a[pc::line_bytes],b[pc::line_bytes]; // worker only
    for(unsigned i=0;i<pc::queue_records;++i) {
        {std::lock_guard lock(g_pcMutex);if(!g_pc.pop(r))break;}
        pc::derive(r,g_pc,d); // g_pc.pf/sc: worker-only scratch, never touched by the draw path
        pc::format(r,d,a,b);acz::feature_log::write(log,a);if(b[0])acz::feature_log::write(log,b);
    }
}
// ------------------------------------------------------------- J-world-objects v2 step 1 (gpu_copy_step5_world_observe, default OFF)
// Read-only: no command is recorded, no step-3/step-5 state is written, no GPU readback (the first 3 ACM SrtData records of a
// list are copied from the game's CPU-mapped UPLOAD memory under the inline path's rules). The Draw seam updates a POD tally per
// list recording (gpu_copy::wobs::State); the Execute seam turns executed tallies into records; the worker formats them.
// Hook-state mutex held. The draw's list, pipeline kind and (ACM draws only) bound targets as the existing tracking knows them.
// ACM draws only: the root signature's layout the first time it is seen with an ACM PSO, and the raw root-parameter state of its first draws.
// Selection is lock-free (atomics), the facts are plain copies (no allocation, no I/O); the record is queued after the hook-state mutex is released.
__declspec(noinline) void woRootDiag_locked(ListState& ls,const diagnostics::L3Observation::List* l,const gpu_copy::wobs::Obs& o,gpu_copy::wobs::RootDiagOut& out) noexcept {
    namespace wo=gpu_copy::wobs;
    const uint64_t rk=ls.bindings.root_id?uint64_t(ls.bindings.root_id):1;
    unsigned slot=wo::root_cap;bool isNew=false;
    for(unsigned i=0;i<wo::root_cap;++i)if(g_woRootKeys[i].load(std::memory_order_relaxed)==rk){slot=i;break;}
    if(slot==wo::root_cap)for(unsigned i=0;i<wo::root_cap;++i) {
        uint64_t expected=0;
        if(g_woRootKeys[i].compare_exchange_strong(expected,rk)){slot=i;isNew=true;break;}
        if(expected==rk){slot=i;break;}
    }
    if(slot==wo::root_cap)return; // table full: the first root_cap signatures are the evidence
    if(isNew) {
        auto& f=out.rf;f.key=rk;f.identity=uint64_t(ls.bindings.root_identity);f.layoutPresent=ls.bindings.root.has_value();f.layoutValid=false;f.rejection[0]=0;
        f.flags=f.staticSamplers=f.paramCount=f.t10Offset=0;f.t10Parameter=UINT32_MAX;f.t10Table=false;f.cockpitMatches=f.worldMatches=f.matches=0;
        f.worldKeyOn=worldIdentityConfigured();f.pending=false;
        if(f.layoutPresent) {
            const auto& root=*ls.bindings.root;f.layoutValid=root.valid();
            std::snprintf(f.rejection,sizeof f.rejection,"%s",root.rejection().c_str());
            if(f.layoutValid) {
                const auto& lay=root.layout();f.flags=lay.flags;f.staticSamplers=lay.static_sampler_count;f.paramCount=uint32_t(lay.parameters.size());
                for(unsigned i=0;i<lay.parameters.size()&&i<wo::root_param_cap;++i) {
                    const auto& q=lay.parameters[i];auto& d=f.p[i];
                    d.kind=uint8_t(q.kind);d.vis=uint8_t(q.visibility);d.reg=q.shader_register;d.space=q.space;
                    d.count=q.kind==metadata::ParameterKind::constants?q.constant_count:uint32_t(q.ranges.size());
                    d.r0kind=0;d.r0base=d.r0count=d.r0space=0;
                    if(!q.ranges.empty()){d.r0kind=uint8_t(q.ranges[0].kind);d.r0base=q.ranges[0].base_register;d.r0count=q.ranges[0].count;d.r0space=q.ranges[0].space;}
                }
                diagnostics::L3Observation::List scratch{};L3T10Outcome outcome;
                l3ConfigureT10(scratch,root,&outcome);
                f.t10Parameter=scratch.t10_parameter;f.t10Offset=scratch.t10_offset;f.t10Table=scratch.t10_table;
                f.cockpitMatches=outcome.cockpitMatches;f.worldMatches=outcome.worldMatches;f.matches=outcome.matches;
            }
        }
        out.root=true;
    }
    if(g_woRootDraws[slot].fetch_add(1,std::memory_order_relaxed)<wo::state_per_root&&
       g_woRootStates.fetch_add(1,std::memory_order_relaxed)<wo::state_total_cap) {
        auto& x=out.rs;x.list=o.list;x.generation=o.generation;x.rootKey=rk;x.pso=o.pso;x.t10Va=o.t10Va;x.t10Address=o.t10Address;x.ordinal=o.l3Ordinal;
        x.t10Parameter=l?l->t10_parameter:UINT32_MAX;x.t10Table=l&&l->t10_table;x.layoutPresent=ls.bindings.root.has_value();x.setCount=0;
        for(const auto& e:ls.bindings.roots) {
            if(x.setCount>=wo::state_param_cap)break;
            auto& d=x.set[x.setCount++];d.index=e.first;d.kind=uint8_t(e.second.kind);d.value=e.second.value;d.words=uint32_t(e.second.constants.size());
        }
        out.state=true;
    }
}
void woSnapshot_locked(void* key,ListState& ls,gpu_copy::wobs::Obs& o,gpu_copy::wobs::RootDiagOut& diag) noexcept {
    namespace wo=gpu_copy::wobs;
    auto& s=state();
    o.list=step1Route(key);
    const diagnostics::L3Observation::List* l3=nullptr;
    if(auto* l=l3_list_locked(ls)){o.generation=l->generation;o.l3Ordinal=uint32_t(l->draws);o.t10Va=l->sources[0].va;o.t10Address=l->sources[0].address;l3=l;}
    o.pso=step1Route(ls.pso.get());
    const auto p=s.pipelines.find(ls.pso.get());
    if(p==s.pipelines.end())return;
    const auto& v=p->second;
    o.id=uint64_t(v.identity);
    o.kind=v.drawcpt_identity_matched?wo::kind_cpt:v.acm_world_matched?wo::kind_acm:wo::kind_other;
    for(unsigned i=0;i<8;++i){ // first 8 hex digits of the VS creation hash (identifies ps2_v / DrawMesh / ... in the log)
        const char c=v.hudVs[i];const unsigned d=c>='0'&&c<='9'?unsigned(c-'0'):c>='a'&&c<='f'?unsigned(c-'a'+10):c>='A'&&c<='F'?unsigned(c-'A'+10):0;
        o.vs8=(o.vs8<<4)|d;
    }
    if(o.kind!=wo::kind_acm)return;
    woRootDiag_locked(ls,l3,o,diag);
    const auto& t=ls.step3;auto& g=o.target;
    g.omCount=t.omSet?t.omCount:0;g.vpW=float(t.vp0.Width);g.vpH=float(t.vp0.Height);
    if(t.omSet&&t.omCount&&!(t.omSingle&&t.omCount>1)) {
        g.rtv=uint64_t(t.rtv[0].ptr);
        for(const auto& w:s.step3Views)if(w.handle&&w.handle==t.rtv[0].ptr&&!w.depth) {
            g.rt=uint64_t(w.resource);g.rtKnown=g.rt!=0;
            if(w.described){g.rtFormat=w.format;g.rtWidth=w.width;g.rtHeight=w.height;}
            break;
        }
    }
    if(t.omSet&&t.dsv) {
        g.dsv=uint64_t(t.dsvHandle.ptr);
        for(const auto& w:s.step3Views)if(w.handle&&w.handle==t.dsvHandle.ptr&&w.depth) {
            g.ds=uint64_t(w.resource);g.dsKnown=g.ds!=0;
            if(w.described)g.dsFormat=w.format;
            break;
        }
    }
}
// Game thread, called for EVERY draw of the continuous phase before the producer's own Draw seam (key off: one relaxed load).
__declspec(noinline) void step5WorldObserveDraw(ID3D12GraphicsCommandList* self) noexcept {
    namespace wo=gpu_copy::wobs;
    auto* st=g_step1.load();auto* s5p=g_step5.load();if(!st||!s5p)return;
    try {
        wo::Obs o;wo::RootDiagOut diag;void* key=nullptr;
        {auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);
         auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));if(it==s.states.end())return;
         woSnapshot_locked(key,it->second,o,diag);}
        if(diag.root||diag.state){std::lock_guard lock(g_woMutex);g_wo.push_root(diag);}
        if(!o.generation){std::lock_guard lock(g_woMutex);++g_wo.noList;return;}
        if(o.kind==wo::kind_acm) { // the producer's frame state (step-1 mutex; the hook-state mutex above is already released)
            std::lock_guard lock(st->mutex);const auto& f=s5p->frame;
            o.frameActive=f.active&&!f.closed&&!f.stopped;o.frameList=f.active?f.list:0;o.frameGeneration=f.active?f.generation:0;
            o.inFrameList=o.frameActive&&o.list==f.list&&o.generation==f.generation;
            o.target.sceneRt=o.target.rt&&o.target.rt==uint64_t(s5p->worldSource);
        }
        unsigned index=0;bool wantSrt=false;
        {std::lock_guard lock(g_woMutex);wantSrt=g_wo.draw(o,index);}
        if(!wantSrt)return;
        std::array<uint8_t,416> buf{};const char* status="read";
        {auto& s=state();std::lock_guard lock(s.mutex);const uint8_t* p=nullptr;
         if(const char* bad=step3MappedSpan_locked(o.t10Address,o.t10Va,416,p))status=bad;
         else if(!guardedCpuRead(p,buf.data(),416))status="read-fault";}
        {std::lock_guard lock(g_woMutex);g_wo.store_srt(o.list,o.generation,index,status,std::strcmp(status,"read")==0?buf.data():nullptr);}
        // J-world-evidence: without the replay key there is no acceptance decision to attach the snapshot to; store it as observed.
        if(!worldObjectsConfigured())step5WorldEvidenceObserved(self,o,status,std::strcmp(status,"read")==0?buf.data():nullptr);
    } catch(...) {}
}
// Game thread, ExecuteCommandLists of the continuous phase (before the producer's own accounting; no lock held on entry).
__declspec(noinline) void step5WorldObserveExecute(ID3D12CommandQueue* self,UINT count,ID3D12CommandList* const* lists) noexcept {
    namespace wo=gpu_copy::wobs;
    auto* st=g_step1.load();auto* s5p=g_step5.load();if(!st||!s5p||!lists)return;
    try {
        constexpr unsigned cap=16;struct Item {uint64_t list=0,generation=0;};std::array<Item,cap> items{};
        const unsigned n=count<cap?count:cap;
        {auto& s=state();std::lock_guard lock(s.mutex);
         for(unsigned i=0;i<n;++i) {
             void* key=lists[i]?listKey_locked(lists[i]):nullptr;if(!key)continue;
             auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));if(it==s.states.end())continue;
             items[i].list=step1Route(key);if(auto* l=l3_list_locked(it->second))items[i].generation=l->generation;
         }}
        uint64_t frameList=0,frameGen=0;
        {std::lock_guard lock(st->mutex);const auto& f=s5p->frame;if(f.active){frameList=f.list;frameGen=f.generation;}}
        const uint64_t queue=uint64_t(step1Route(self));
        uint64_t seq=0;
        {std::lock_guard lock(g_woMutex);g_wo.begin_call();seq=g_wo.execSeq;
         for(unsigned i=0;i<n;++i)
             if(items[i].list&&items[i].generation)
                 g_wo.execute(items[i].list,items[i].generation,queue,i,n,frameList&&items[i].list==frameList&&items[i].generation==frameGen);
         if(count>cap)g_wo.untracked+=count-cap;}
        if(worldEvidenceConfigured()) { // J-world-evidence: the Execute occurrence of the pending record snapshots
            std::lock_guard lock(g_wevMutex);
            for(unsigned i=0;i<n;++i)if(items[i].list&&items[i].generation)g_wev.note_execute(items[i].list,items[i].generation,seq);
        }
    } catch(...) {}
}
// Worker (no producer lock held): decode and write the queued records (dedicated log only).
void step5WorldObserveDrain(acz::feature_log::Handle log) noexcept {
    if(!worldObserveConfigured())return;
    namespace wo=gpu_copy::wobs;
    static wo::Record r;static wo::Derived d;static gpu_copy::step5::Scratch sc;static char a[wo::line_bytes],b[wo::line_bytes]; // worker only
    for(unsigned i=0;i<wo::queue_records;++i) {
        {std::lock_guard lock(g_woMutex);if(!g_wo.pop(r))break;}
        if(r.type==wo::rec_srt)wo::derive(r.srt,d,sc);else d=wo::Derived{};
        wo::format(r,d,a,b);acz::feature_log::write(log,a);if(b[0])acz::feature_log::write(log,b);
    }
}
// ------------------------------------------------------------- J-world-evidence (Astra reply 5; under world_observe / world_objects)
// (1) RT slot 1 metadata of the ORIGINAL draw's OMSetRenderTargets bind next to the ACM PSO's saved output facts; (2) the 416 B liveSrt
// snapshot a world acceptance decision used, copied into a fixed slot; (3) the live cb10 flags rule: the draw's cb10 dword 0 is resolved
// from the root-constant witness and checked with world::flags_check on every acceptance path. Read-only, bounded, no allocation at the
// Draw seam, no extra read of mapped memory, no formatting or I/O outside the worker (gpu_copy_world_evidence.h).
// Hook-state mutex held. Slot `i` of this list's original OMSetRenderTargets bind, resolved through the tracked RTV creations.
void wevSlot_locked(const ListState::Step3Track& t,unsigned i,gpu_copy::wev::SlotFacts& out) noexcept {
    namespace wev=gpu_copy::wev;
    auto& s=state();out=wev::SlotFacts{};
    if(!t.omSet){out.kind=wev::sk_om_never_set;out.detail="OMSetRenderTargets-not-observed-in-this-recording";return;}
    if(i>=t.omCount||i>=8){out.kind=wev::sk_unbound;out.detail="om-count-not-above-slot";return;}
    // The contiguous-range form stores only the base handle: slot i>0 is derived by the runtime (needs the device's RTV increment and
    // valid heap bounds), so a zero t.rtv[i] says NOTHING about it.
    if(t.omSingle&&i>0){out.kind=wev::sk_range_unresolved;out.detail="contiguous-range-form:only-the-base-handle-is-stored";return;}
    out.handle=uint64_t(t.rtv[i].ptr);out.bindSerial=t.rtvSerial[i];
    if(!out.handle){out.kind=wev::sk_unbound;out.detail="zero-handle";return;}
    for(const auto& w:s.step3Views) {
        if(!w.handle||w.handle!=t.rtv[i].ptr||w.depth)continue;
        out.serial=w.serial;out.current=t.omProof&&t.copyEpoch==s.step3CopyEpoch&&t.rtvSerial[i]==w.serial;
        if(!w.resource){out.kind=wev::sk_null_descriptor;out.detail="CreateRenderTargetView-with-a-null-resource";return;}
        out.kind=wev::sk_resource;out.resource=uint64_t(w.resource);out.width=w.width;out.height=w.height;out.resFormat=w.format;
        out.viewDesc=w.vd.present;out.viewFormat=w.vd.format;out.viewDim=w.vd.dim;out.mip=w.vd.mip;out.slice=w.vd.slice;
        return;
    }
    out.kind=wev::sk_unknown;out.detail="no-view-record-for-the-handle";
}
// Hook-state mutex held. The ACM PSO's saved output facts and the list's original OM binding.
void wevCollectRt_locked(const ListState& ls,const PipelineRecord& v,gpu_copy::wev::RtFacts& f) noexcept {
    namespace wev=gpu_copy::wev;
    auto& s=state();const auto& t=ls.step3;const auto& o=v.output;
    f.pso=uint64_t(step1Route(ls.pso.get()));f.psoId=uint64_t(v.identity);
    std::memcpy(f.vs,v.hudVs.data(),sizeof f.vs);f.vs[32]=0;std::memcpy(f.ps,v.hudPs.data(),sizeof f.ps);f.ps[32]=0;
    f.psoKnown=o.known;f.rtCount=o.rt_count;for(unsigned i=0;i<8;++i)f.rtv[i]=o.rtv[i];
    f.blend0=o.blend0;f.independent=o.independent_blend;f.mask0=o.write_mask0;f.blend1=v.blend1;f.mask1=v.write_mask1;
    f.omSet=t.omSet;f.omCount=t.omSet?uint32_t(t.omCount):0u;f.omSingle=t.omSingle!=FALSE;f.omProof=t.omProof;
    f.bindEpoch=t.copyEpoch;f.curEpoch=s.step3CopyEpoch;
    const unsigned stored=!t.omSet?0u:t.omSingle?(t.omCount?1u:0u):(t.omCount<8u?unsigned(t.omCount):8u);
    for(unsigned i=0;i<stored;++i)f.handles[i]=uint64_t(t.rtv[i].ptr);
    wevSlot_locked(t,0,f.slot0);wevSlot_locked(t,1,f.slot1);
    f.key=wev::om_key(f);
}
// Game thread, EVERY draw of the continuous phase (key off: not called). Counts the all-draw ordinal of the recording and, for an ACM
// draw, offers the (PSO, OM epoch) RT record.
__declspec(noinline) void step5WorldEvidenceDraw(ID3D12GraphicsCommandList* self) noexcept {
    namespace wev=gpu_copy::wev;
    try {
        wev::RtFacts f;bool acm=false;uint64_t list=0,gen=0;
        {auto& s=state();std::lock_guard lock(s.mutex);void* key=listKey_locked(self);
         auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));if(it==s.states.end())return;
         list=uint64_t(step1Route(key));if(auto* l=l3_list_locked(it->second))gen=l->generation;
         const auto p=s.pipelines.find(it->second.pso.get());
         acm=p!=s.pipelines.end()&&p->second.acm_world_matched;
         if(acm)wevCollectRt_locked(it->second,p->second,f);}
        uint32_t ord=0;
        {std::lock_guard lock(g_wevMutex);ord=g_wev.draw_ordinal(list,gen);
         if(acm){f.list=list;f.generation=gen;f.ordinal=ord;if(g_wev.rt_wanted(f.psoId,f.key))g_wev.rt_push(f);}}
        g_wevOrdinal.store(ord,std::memory_order_relaxed);
    } catch(...) {}
}
// Hook-state mutex held. This draw's cb10 dword 0 from the root-constant witness. The draw's own interval is still open (the native Draw has
// not returned), so the word is judged by RootConstantWitness::word_at_draw: same recording / root / thread / interval rules as the
// requirements snapshot, but only dword 0 must have been written. Anything else leaves `known` false (the caller refuses the draw).
void wevCb10Resolve_locked(const ListState& ls,const PipelineRecord* v,gpu_copy::step5::world::Cb10& c) noexcept {
    using W=diagnostics::RootConstantWitness;
    c=gpu_copy::step5::world::Cb10{};c.resolved=true;
    const auto& w=ls.constantWitness;const auto& b=ls.bindings;
    c.parameter=w.index;c.stages=w.stages;c.size=w.size;c.root=w.root;c.token=w.token;c.revision=w.revision;
    for(unsigned i=0;i<4&&i<w.size;++i)if(w.words[i].written){c.mask|=1u<<i;c.words[i]=w.words[i].value;}
    auto* log=diagnostics::sink();
    if(!log||!log->ownership_pins()){c.reason=W::layout;c.note="diagnostic_ownership_pins-off(root-constants-not-tracked)";return;}
    // The sub-condition that fails is named in `note` (run H: every ACM draw read witness-reason=history with note=none; the words themselves were present).
    bool inferred=false;
    const char* gap=gpu_copy::step5::world::cb10_provenance(listPointFailure_locked(ls,b),b.tracking_complete,v!=nullptr,v&&b.pipeline.identity==v->identity,
        b.root_identity,v?uint64_t(v->root_identity):0,inferred);
    const bool complete=!gap;
    if(gap)c.note=gap;else if(inferred)c.note="pso-root-identity-unknown(bound-root-layout-used)";
    uint32_t dword0=0;
    c.reason=unsigned(w.word_at_draw(b.list_identity,b.generation,b.root_id,b.root_identity,GetCurrentThreadId(),complete,0,dword0));
    if(c.reason==W::covered){c.known=true;c.value=dword0;}
}
#ifdef ACZ_VR_OFFLINE_TEST
bool g_wevCb10Fixed=false,g_wevCb10FixedUnknown=false;uint32_t g_wevCb10FixedValue=258; // fixtures without a root-constant witness: resolve to this witnessed value (or to "unknown")
#endif
// Step-1 mutex held (nests the hook-state mutex, documented order). Resolves the cb10 value of the world draw about to be decided.
__declspec(noinline) void step5WobjCb10(Step5State& s5,void* key) noexcept {
    s5.wobjCb10=gpu_copy::step5::world::Cb10{};
    try {
#ifdef ACZ_VR_OFFLINE_TEST
        if(g_wevCb10Fixed) {
            auto& c=s5.wobjCb10;c.resolved=true;c.size=2;c.parameter=3;c.stages=3;
            if(g_wevCb10FixedUnknown){c.known=false;c.reason=unsigned(diagnostics::RootConstantWitness::partial);c.note="fixture";return;}
            c.known=true;c.value=g_wevCb10FixedValue;c.mask=1;c.words[0]=c.value;return;
        }
#endif
        auto& s=state();std::lock_guard lock(s.mutex);
        auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));if(it==s.states.end())return;
        const auto p=s.pipelines.find(it->second.pso.get());
        wevCb10Resolve_locked(it->second,p!=s.pipelines.end()?&p->second:nullptr,s5.wobjCb10);
    } catch(...) {}
}
// Takes the hook-state mutex. PSO / shader identity, the t10 parameter's visibility and the t10 resource (serial, offset) of the draw's list;
// `withCb10` also resolves the draw's cb10 words (the observer-only path has no earlier resolution).
void wevFillMeta_locked_outer(void* key,gpu_copy::wev::SrtMeta& m,bool withCb10) noexcept {
    auto& s=state();std::lock_guard lock(s.mutex);
    auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));
    if(it==s.states.end())return;
    m.pso=uint64_t(step1Route(it->second.pso.get()));
    const auto p=s.pipelines.find(it->second.pso.get());
    if(p!=s.pipelines.end()) {
        m.psoId=uint64_t(p->second.identity);
        std::memcpy(m.vs,p->second.hudVs.data(),sizeof m.vs);m.vs[32]=0;std::memcpy(m.ps,p->second.hudPs.data(),sizeof m.ps);m.ps[32]=0;
    }
    if(withCb10)wevCb10Resolve_locked(it->second,p!=s.pipelines.end()?&p->second:nullptr,m.cb10);
    if(m.t10Param==UINT32_MAX)if(auto* l=l3_list_locked(it->second))m.t10Param=l->t10_parameter;
    const auto& rb=it->second.bindings.root;
    if(m.t10Param!=UINT32_MAX&&rb.has_value()&&rb->valid()&&m.t10Param<rb->layout().parameters.size())
        m.t10Vis=uint32_t(rb->layout().parameters[m.t10Param].visibility);
    if(const auto* r=m.t10Address?s.l3.resources.get(m.t10Address):nullptr) {
        m.t10ResSerial=r->serial;
        if(r->gpu_va&&m.t10Va>=r->gpu_va){m.t10Offset=m.t10Va-r->gpu_va;m.t10OffsetKnown=true;}
    }
}
// Step-1 mutex held. One ACM acceptance decision (`why` = refusal, nullptr = accepted): counted by reason; when the decision read the record
// (fresh liveSrt, or a failed read) and the bounded window quota allows, the snapshot the decision used is copied into a fixed slot.
__declspec(noinline) void step5WorldEvidenceDecision(Step1State& st,Step5State& s5,void* key,const char* why,bool early) noexcept {
    namespace wev=gpu_copy::wev;
    if(!worldEvidenceConfigured())return;
    try {
        auto& s3=st.s3;const uint64_t now=GetTickCount64();
        const bool haveRead=s5.wobjLiveFresh||std::strcmp(s5.wobjReadStatus,"not-read")!=0;
        const uint64_t list=uint64_t(step1Route(key)),gen=s3.live.generation;
        bool want=false;
        {std::lock_guard lock(g_wevMutex);g_wev.note_decision(why,early);want=haveRead&&g_wev.srt_wanted(list,gen,now);}
        if(!want)return;
        wev::SrtMeta m;m.list=list;m.generation=gen;m.ordinal=g_wevOrdinal.load(std::memory_order_relaxed);
        wev::copy_text(m.path,sizeof m.path,early?"early-open":"member");
        wev::copy_text(m.read,sizeof m.read,s5.wobjReadStatus);
        m.accepted=why==nullptr;wev::copy_text(m.decision,sizeof m.decision,why?why:"accepted");
        m.hasBytes=s5.wobjLiveFresh;m.cb10=s5.wobjCb10;
        m.t10Va=s3.live.t10_va;m.t10Address=s3.live.t10_address;m.t10Param=s3.live.t10_parameter;
        wevFillMeta_locked_outer(key,m,false);
        std::lock_guard lock(g_wevMutex);(void)g_wev.srt_offer(m,s3.liveSrt.data(),now);
    } catch(...) {}
}
// Observer-only (world_objects off): the first ACM records of a recording have no acceptance decision; they are stored as observed.
__declspec(noinline) void step5WorldEvidenceObserved(ID3D12GraphicsCommandList* self,const gpu_copy::wobs::Obs& o,const char* status,const uint8_t* bytes) noexcept {
    namespace wev=gpu_copy::wev;
    try {
        const uint64_t now=GetTickCount64();
        {std::lock_guard lock(g_wevMutex);g_wev.note_decision("observed-only(no-acceptance-decision)",false);if(!g_wev.srt_wanted(o.list,o.generation,now))return;}
        wev::SrtMeta m;m.list=o.list;m.generation=o.generation;m.ordinal=g_wevOrdinal.load(std::memory_order_relaxed);
        wev::copy_text(m.path,sizeof m.path,"observed");wev::copy_text(m.read,sizeof m.read,status);
        m.observed=true;m.hasBytes=bytes!=nullptr;m.t10Va=o.t10Va;m.t10Address=o.t10Address;
        void* key=nullptr;{auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);}
        wevFillMeta_locked_outer(key,m,true);
        std::lock_guard lock(g_wevMutex);(void)g_wev.srt_offer(m,bytes,now);
    } catch(...) {}
}
// A world draw that never reached a decision (no frame to join, another list, closed frame): counted only, so the class stays visible.
inline void step5WorldEvidenceNote(const char* why) noexcept {
    if(!worldEvidenceConfigured())return;
    std::lock_guard lock(g_wevMutex);g_wev.note_decision(why,false);
}
// Worker (no producer lock held): format and write the queued RT records and ready record snapshots (dedicated log only).
void step5WorldEvidenceDrain(acz::feature_log::Handle log) noexcept {
    if(!worldEvidenceConfigured())return;
    namespace wev=gpu_copy::wev;
    static wev::RtFacts rf;static wev::Srt sr;static char a[wev::line_bytes],b[wev::line_bytes],c[wev::line_bytes];static char lines[wev::srt_lines][wev::line_bytes];
    static unsigned rtN=0; // worker only
    unsigned written=0;
    for(unsigned i=0;i<wev::rt_queue;++i) {
        {std::lock_guard lock(g_wevMutex);if(!g_wev.rt_pop(rf))break;}
        wev::format_rt(rf,++rtN,a,b,c);acz::feature_log::write(log,a);acz::feature_log::write(log,b);acz::feature_log::write(log,c);written+=3;
    }
    for(unsigned i=0;i<wev::rec_per_window;++i) {
        {std::lock_guard lock(g_wevMutex);if(!g_wev.srt_pop(sr,GetTickCount64()))break;}
        const unsigned n=wev::format_srt(sr,lines);
        for(unsigned k=0;k<n;++k)acz::feature_log::write(log,lines[k]);
        written+=n;
        if(written>=48){acz::feature_log::flush();written=0;} // the feature log queues 512 lines between its flushes
    }
}
// ------------------------------------------------------------- J-phase-observe (gpu_copy_step5_phase_observe, default OFF)
// Read-only: no command is recorded, no step-3/step-5 state is written, no GPU readback, no allocation. Every draw of the continuous
// phase is copied (POD, fixed size) into the recording's buffer of gpu_copy::phobs::State; the list's Close assigns the frame phases and
// queues the records; the worker formats them (src/acz-vr/gpu_copy_phase_observe.h, J-phase-observe-findings.md).
inline uint32_t phHex8(const std::array<char,33>& h) noexcept {
    uint32_t v=0;for(unsigned i=0;i<8;++i){const char c=h[i];v=(v<<4)|(c>='0'&&c<='9'?unsigned(c-'0'):c>='a'&&c<='f'?unsigned(c-'a'+10):c>='A'&&c<='F'?unsigned(c-'A'+10):0u);}
    return v;
}
// Hook-state mutex held. The draw's list, pipeline facts (creation-time), bound targets and viewport as the existing tracking knows them.
void phSnapshot_locked(void* key,ListState& ls,gpu_copy::phobs::Draw& d,uint64_t& list,uint64_t& gen) noexcept {
    namespace ph=gpu_copy::phobs;
    auto& s=state();
    list=step1Route(key);gen=0;
    if(auto* l=l3_list_locked(ls))gen=l->generation;
    d.pso=step1Route(ls.pso.get());
    const auto p=s.pipelines.find(ls.pso.get());
    if(p!=s.pipelines.end()) {
        const auto& v=p->second;const auto& o=v.output;
        d.id=uint64_t(v.identity);
        d.kind=v.drawcpt_identity_matched?ph::kind_cpt:v.acm_world_matched?ph::kind_acm:ph::kind_other;
        d.vs8=phHex8(v.hudVs);d.ps8=phHex8(v.hudPs);
        d.psoKnown=o.known;d.blend=o.blend0;d.independentBlend=o.independent_blend;d.writeMask=o.write_mask0;
        d.blendSrc=v.blend_src;d.blendDst=v.blend_dst;d.blendOp=v.blend_op;d.blendSrcA=v.blend_src_alpha;d.blendDstA=v.blend_dst_alpha;d.blendOpA=v.blend_op_alpha;
        d.depthEnable=o.depth_enable;d.depthWrite=o.depth_write;d.depthFunc=o.depth_func;d.stencil=o.stencil;
        d.stencilRead=o.stencil_read;d.stencilWrite=o.stencil_write;d.frontFunc=o.front_func;d.backFunc=o.back_func;d.frontPass=o.front_pass;d.backPass=v.back_pass;
    }
    const auto& t=ls.step3;
    d.vpKnown=t.known&&!t.gap&&t.viewports==1;d.vpW=float(t.vp0.Width);d.vpH=float(t.vp0.Height);
    d.omCount=uint8_t(t.omSet&&t.omCount<=8?t.omCount:0);
    if(t.omSet&&t.omCount&&!(t.omSingle&&t.omCount>1)) {
        d.rtv=uint64_t(t.rtv[0].ptr);
        for(const auto& w:s.step3Views)if(w.handle&&w.handle==t.rtv[0].ptr&&!w.depth) {
            d.rt=uint64_t(w.resource);
            d.rtKnown=w.resource&&t.omProof&&t.copyEpoch==s.step3CopyEpoch&&t.rtvSerial[0]==w.serial; // the HUD region's proof rule
            if(w.described){d.rtFormat=w.format;d.rtWidth=w.width;d.rtHeight=w.height;}
            break;
        }
    }
    if(t.omSet&&t.dsv) {
        d.dsv=uint64_t(t.dsvHandle.ptr);
        for(const auto& w:s.step3Views)if(w.handle&&w.handle==t.dsvHandle.ptr&&w.depth) {
            d.ds=uint64_t(w.resource);d.dsKnown=d.ds!=0;
            if(w.described)d.dsFormat=w.format;
            break;
        }
    }
    // The cb10 root-constant words the witness tracks for the bound root signature (when it is configured): the first four.
    const auto& cw=ls.constantWitness;
    if(cw.configured&&cw.size) {
        d.rcAvailable=true;
        for(unsigned i=0;i<4&&i<cw.size;++i)if(cw.words[i].written){d.rcMask|=1u<<i;d.rc[i]=cw.words[i].value;}
    }
}
// Game thread, called for EVERY draw of the continuous phase (key off: one relaxed load; nothing else runs).
__declspec(noinline) void step5PhaseObserveDraw(ID3D12GraphicsCommandList* self,UINT count,UINT instances,bool indexed) noexcept {
    try {
        gpu_copy::phobs::Draw d;uint64_t list=0,gen=0;
        {auto& s=state();std::lock_guard lock(s.mutex);void* key=listKey_locked(self);
         auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));if(it==s.states.end())return;
         phSnapshot_locked(key,it->second,d,list,gen);}
        d.count=count;d.instances=instances;d.indexed=indexed;
        std::lock_guard lock(g_phMutex);g_ph.draw(list,gen,d);
    } catch(...) {}
}
inline void phaseObserveDraw(ID3D12GraphicsCommandList* self,UINT count,UINT instances,bool indexed) noexcept {
    if(!phaseObserveConfigured()||g_step1Phase.load(std::memory_order_relaxed)!=static_cast<int>(Step1Phase::continuous))return;
    step5PhaseObserveDraw(self,count,instances,indexed);
}
// READ-ONLY aircraft diagnostics (fxsig:ac0f1a60; fx_diag_flare_air.h; aircraft-shading-reflection.md section 5 steps 2/3): one line per DrawCPT / DrawACM identity draw
// of the continuous phase (rate limited): the cb10 flags (bit 0 = reflection pass), the PSO blend/depth state, the bound target and the root summary (descriptor
// table handles). Called for every draw: flag off = one relaxed load.
__declspec(noinline) void step5AirDiagDraw(ID3D12GraphicsCommandList* self,UINT count,UINT instances,bool indexed) noexcept {
    try {
        namespace fd=acz::fxdiag;
        auto* st=g_step1.load();auto* s5p=g_step5.load();if(!st||!s5p)return;
        gpu_copy::phobs::Draw d;uint64_t list=0,gen=0;char roots[132]={};bool cpt=false;
        {auto& s=state();std::lock_guard lock(s.mutex);void* key=listKey_locked(self);
         auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));if(it==s.states.end())return;
         const auto p=s.pipelines.find(it->second.pso.get());
         if(p==s.pipelines.end()||!(p->second.drawcpt_identity_matched||p->second.acm_world_matched))return;
         cpt=p->second.drawcpt_identity_matched;
         phSnapshot_locked(key,it->second,d,list,gen);
         step5FxSigRoots_locked(it->second,roots,sizeof roots);}
        std::lock_guard lock(st->mutex);auto& s5=*s5p;
        const uint64_t now=GetTickCount64();
        const uint64_t ord=s5.diagOrd++;
        step5DiagArmPoll(s5.diagAirLim,now);
        if(!s5.diagAirLim.allow_line(now,uint64_t(cpt)|(uint64_t(d.rcMask?d.rc[0]:0xffffffffu)<<1),false))return;
        fd::AirLine a;a.frame=s5.nextFrameId+1;a.ord=ord;a.cpt=cpt;a.ps8=d.ps8;a.vs8=d.vs8;a.count=count;a.instances=instances;a.indexed=indexed;
        a.rc_available=d.rcAvailable;a.rc_mask=d.rcMask;for(unsigned i=0;i<4;++i)a.rc[i]=d.rc[i];
        a.psoKnown=d.psoKnown;a.blend=d.blend;a.depth_enable=d.depthEnable;a.depth_write=d.depthWrite;a.depth_func=d.depthFunc;a.write_mask=d.writeMask;
        a.bl[0]=d.blendSrc;a.bl[1]=d.blendDst;a.bl[2]=d.blendOp;a.bl[3]=d.blendSrcA;a.bl[4]=d.blendDstA;a.bl[5]=d.blendOpA;
        a.rt=d.rt;a.rtv=d.rtv;a.rt_w=d.rtWidth;a.rt_h=d.rtHeight;a.rt_fmt=d.rtFormat;a.vp_w=d.vpW;a.vp_h=d.vpH;
        std::snprintf(a.roots,sizeof a.roots,"%s",roots);
        a.sup=s5.diagAirLim.take_suppressed();
        s5.diagQ.push_air(a);
    } catch(...) {}
}
inline void airDiagDraw(ID3D12GraphicsCommandList* self,uint64_t countInstances,uint64_t firstIndexed) noexcept {
    if(!step5DiagAirOn()||g_step1Phase.load(std::memory_order_relaxed)!=static_cast<int>(Step1Phase::continuous))return;
    step5AirDiagDraw(self,UINT(countInstances),UINT(countInstances>>32),(firstIndexed>>32)!=0);
}
// Worker (no producer lock held): format and write the queued records (dedicated log only).
// Control files of the armed windows: %LOCALAPPDATA%\acz-vr\phase-observe.arm and phase-observe.mark. The stamp of a file is derived from its creation
// and last-write times (0: absent, or a directory); creating or touching the file changes it. Nothing is ever written or deleted by the observer.
#ifdef ACZ_VR_OFFLINE_TEST
std::wstring g_phControlDirTest; // fixtures: a temporary directory instead of %LOCALAPPDATA%\acz-vr
#endif
bool phControlPath(const wchar_t* name,wchar_t (&out)[MAX_PATH]) noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(!g_phControlDirTest.empty()){_snwprintf_s(out,MAX_PATH,_TRUNCATE,L"%s\\%s",g_phControlDirTest.c_str(),name);return true;}
#endif
    wchar_t local[MAX_PATH];const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
    if(!n||n>=MAX_PATH-48)return false;
    _snwprintf_s(out,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\%s",local,name);return true;
}
uint64_t phFileStamp(const wchar_t* name) noexcept {
    wchar_t path[MAX_PATH];if(!phControlPath(name,path))return 0;
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if(!GetFileAttributesExW(path,GetFileExInfoStandard,&d)||(d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return 0;
    const uint64_t w=(uint64_t(d.ftLastWriteTime.dwHighDateTime)<<32)|d.ftLastWriteTime.dwLowDateTime;
    const uint64_t c=(uint64_t(d.ftCreationTime.dwHighDateTime)<<32)|d.ftCreationTime.dwLowDateTime;
    return (w*31u+c)|1u;
}
// Worker: one poll of both files at tick `now` (the rate limit is the caller's).
void step5PhaseObservePollNow(uint64_t now) noexcept {
    const uint64_t arm=phFileStamp(L"phase-observe.arm"),mark=phFileStamp(L"phase-observe.mark");
    FILETIME ft{};GetSystemTimeAsFileTime(&ft);
    const uint64_t t=(uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
    const uint64_t utcMs=t>116444736000000000ull?(t-116444736000000000ull)/10000ull:0;
    std::lock_guard lock(g_phMutex);g_ph.poll(now,utcMs,arm,mark);
}
void step5PhaseObserveDrain(acz::feature_log::Handle log) noexcept {
    if(!phaseObserveConfigured())return;
    namespace ph=gpu_copy::phobs;
    {static uint64_t lastPoll=0;const uint64_t now=GetTickCount64();if(!lastPoll||now-lastPoll>=200){lastPoll=now;step5PhaseObservePollNow(now);}}
    static ph::Record r;static char a[ph::line_bytes],b[ph::line_bytes]; // worker only
    unsigned lines=0;
    for(unsigned i=0;i<ph::queue_records;++i) {
        {std::lock_guard lock(g_phMutex);if(!g_ph.pop(r))break;}
        ph::format(r,a,b);acz::feature_log::write(log,a);++lines;if(b[0]){acz::feature_log::write(log,b);++lines;}
        // the feature log queues 512 lines between its (5 s) flushes: a window logs two lines per frame, so flush before it fills (live F1 dropped 1286 lines)
        if(lines>=96){acz::feature_log::flush();lines=0;}
    }
}
// ------------------------------------------------------------- J-world-objects (gpu_copy_step5_world_objects, default OFF)
// A DrawACM_v-identity draw (acm_world_matched PSO) INSIDE an active by-class frame is replayed per eye like a
// cockpit member (same 8 commands, same private targets, same game PSO/root state; only the OM targets and the
// root SRV t10 change). It never starts a frame and never touches the cockpit bookkeeping (ordinals, selection,
// pending skips, partial flag). The record is read from the game's CPU-mapped UPLOAD memory under the same rules
// as the cockpit's (step3MappedSpan_locked) and substituted by gpu_copy::step5::world::substitute (ABSOLUTE world
// coordinates, head offset applied relative to the game camera). Anything not provable is refused alone.
// Class of a world draw against selected class 0 (the cockpit ring): the root signature object is NOT compared
// (DrawACM_v has its own); everything the ring and the replay depend on is.
// The depth scale written into the replayed world records (1.0 = the raw bytes). Key off, or a depth format that is not FLOAT: 1.0.
// ------------------------------------------------------------- OBJECT-INTERP (interp:1; object_interp.h, docs/world-render/interp.md)
// Runs only while acz::interp::armed() (set by the consumer when it parses interp:1 with a terrain stage loaded); key off: step5InterpFrame returns
// at once, f.interp stays invalid and step5InterpObject returns at once. Step-1 mutex held. Only the REPLAYED copies of the record are changed (B,
// +256..+319: the model matrix, the same in both eye copies); the game's own record and draw are never touched.
static_assert(acz::interp::kCap==gpu_copy::step5::world::draw_cap,"OBJECT-INTERP: one track per recorded world draw");
// The frame's world camera was recovered (first accepted DrawACM_v record): plan the frame (the camera to the consumer's t_display, k for the objects).
void step5InterpFrame(Step5State& s5,Step5Frame& f,const gpu_copy::step5::world::Frame& w) noexcept {
    f.interp=acz::interp::Facts{};s5.interpDispFresh=false;
    if(!acz::interp::armed())return;
    if(!s5.interpConfigured) {
        const auto& sh=acz::interp::shared();
        acz::interp::Config oc;oc.max_extrap_frames=double(sh.max_frames_milli.load())*1e-3;oc.attitude=sh.attitude.load()!=0;
        acz::pacing::ExtrapConfig cc;cc.max_ms=float(double(sh.max_ms_milli.load())*1e-3);cc.attitude=oc.attitude;
        s5.interpPlanner.configure(oc,cc);s5.interpConfigured=true;
    }
    acz::pacing::Frame cam;
    for(int k=0;k<3;++k){cam.eye[k]=float(w.eyeAbs[k]);cam.right[k]=float(w.pf.cam.right[k]);cam.up[k]=float(w.pf.cam.up[k]);cam.fwd[k]=float(w.pf.cam.fwd[k]);}
    const acz::interp::FramePlan p=s5.interpPlanner.begin(++s5.interpSeq,step5MonoUs(),cam,acz::interp::horizon_ms());
    auto& fi=f.interp;
    fi.valid=true;fi.k=float(p.k);fi.horizon_ms=float(p.horizon_ms);fi.why=p.why;fi.cam_moved=!p.delta.identity;
    for(int k=0;k<3;++k)fi.dt[k]=float(p.delta.dt[k]);
    for(int k=0;k<9;++k)fi.dR[k]=float(p.delta.dR[k]);
    fi.lost=s5.interpPlanner.tracker().lost_last_frame();
}
// One accepted world draw whose per-eye copies (eye0/eye1 = s3.inlineBlobs[1..2]) were substituted: its model matrix at the frame's t_display, as the
// display camera sees it. Nothing to do (k 0, identity camera, untracked object): the copies keep the raw B bytes.
void step5InterpObject(Step5State& s5,Step5Frame& f,const uint8_t* raw,const gpu_copy::step3::Shape& d,uint8_t* eye0,uint8_t* eye1) noexcept {
    s5.interpDispFresh=false;
    auto& fi=f.interp;
    if(!fi.valid||!raw)return;
    const acz::interp::DrawKey key{acz::tac::mesh_signature(d.indexed,d.count,d.vb_stride,d.topology),d.vb_va,d.ib_va};
    acz::interp::Xform obs,out;acz::interp::xform_from_record(raw,obs);
    double disp[3];acz::interp::Result r;
    const bool write=s5.interpPlanner.object(key,obs,out,disp,r);
    ++fi.objects;
    if(r.moved)++fi.moved;else ++fi.held;
    if(r.kind==acz::interp::kind_appeared)++fi.appeared;
    else if(r.kind==acz::interp::kind_teleport)++fi.teleports;
    else if(r.kind==acz::interp::kind_scale)++fi.scale_changes;
    if(r.id&&fi.nsample<acz::interp::Facts::kSamples) { // the live trace (consumer interp-trace windows)
        auto& sm=fi.sample[fi.nsample++];sm.id=r.id;sm.kind=uint8_t(r.kind);sm.moved=r.moved?1:0;
        for(int k=0;k<3;++k){sm.obs[k]=float(obs.t[k]);sm.disp[k]=float(disp[k]);}
    }
    if(write&&!(acz::interp::xform_to_record(out,eye0)&&acz::interp::xform_to_record(out,eye1))) {
        ++fi.write_fail;std::memcpy(eye0+256,raw+256,64);std::memcpy(eye1+256,raw+256,64); // non-finite result: the raw B in both copies
        return;
    }
    for(int k=0;k<3;++k)s5.interpDisp[k]=float(disp[k]);
    s5.interpDispFresh=true;
}
inline float step5WobjDepthScale(const Step5State& s5) noexcept {
    return gpu_copy::step5::world::depth_fix_scale_for(worldDepthFixConfigured(),s5.ringDsv);
}
// J-180 (Codex review P2, depth fix): with xr_world_objects_depth_fix on and applied, each frame's world camera yields the view-depth band whose
// geometry the compressed depth scale admits although the original record clips it (see world::depth_fix_clip_band). Diagnostic only: counted
// per frame and logged; no replay decision depends on it. Key off / depth fix not applied: nothing runs.
void step5WobjClipNote(Step5State& s5,const gpu_copy::step5::world::Frame& w) noexcept {
    namespace world=gpu_copy::step5::world;
    if(world::depth_fix_scale_for(worldDepthFixConfigured(),s5.ringDsv)==world::depth_scale_ok)return;
    ++s5.wobjClipFrames;
    const auto band=world::depth_fix_clip_band(w.z0,w.b,double(world::depth_fix_scale));
    if(band.valid){s5.wobjClipLo=band.lo;s5.wobjClipHi=band.hi;s5.wobjClipValid=true;} else ++s5.wobjClipUnmodelled;
}
const char* step5WobjClassWhy(const Step5State& s5,const gpu_copy::step3::Shape& live) noexcept {
    if(!s5.classCount)return "world-class-no-selected-class";
    const Step5Class c=step5ClassOf(live);const Step5Class& a=s5.classes[0];
    if(a.topology!=c.topology)return "world-class-topology";
    if(a.ib!=c.ib)return "world-class-ib-stride";
    if(a.vb!=c.vb)return "world-class-vb-stride";
    if(!live.t10_vs&&a.t10!=c.t10)return "world-class-t10-parameter"; // J-180: not for a draw resolved through its own vertex-stage binding
    if(a.rtv!=c.rtv||a.dsv!=c.dsv)return "world-class-format";
    if(a.samples!=c.samples||a.quality!=c.quality)return "world-class-samples";
    if(a.stencil!=c.stencil)return "world-class-stencil";
    return nullptr;
}
// Step-1 mutex held (nests the hook-state mutex, documented order). The live record into s3.liveSrt.
__declspec(noinline) bool step5WobjRead(Step1State& st,Step5State& s5) noexcept {
    auto& s3=st.s3;const uint8_t* p=nullptr;
    s5.wobjLiveFresh=false;
    auto& s=state();std::lock_guard lock(s.mutex);
    if(const char* why=step3MappedSpan_locked(s3.live.t10_address,s3.live.t10_va,416,p)){s5.wobjReadStatus=why;return false;}
    if(!guardedCpuRead(p,s3.liveSrt.data(),416)){s5.wobjReadStatus="read-fault";return false;}
    s5.wobjReadStatus="read";s5.wobjLiveFresh=true;   // J-world-evidence: the bytes in liveSrt are THIS decision's snapshot
    return true;
}
// Astra issue-31 P2-8: production always qualifies the scene target; legacy offline fixtures that run world objects without a world snapshot opt out
// (g_wobjSceneOptionalTest, default true in the fixtures; the P2-8 fixture clears it).
#ifdef ACZ_VR_OFFLINE_TEST
bool g_wobjSceneOptionalTest=true;
#endif
inline bool step5WobjSceneOptional() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    return g_wobjSceneOptionalTest;
#else
    return false;
#endif
}
__declspec(noinline) const char* step5WobjWhy(Step1State& st,Step5State& s5) noexcept {
    using namespace gpu_copy::step5;
    auto& s3=st.s3;auto& f=s5.frame;auto& w=s5.wobj;
    s5.wobjLiveFresh=false;s5.wobjReadStatus="not-read";
    if(!f.byClass)return "world-class-needs-members-by-class";
    // J-world-evidence: a PSO with other than one render target is refused (and counted as rt-count) BEFORE the scene-target rule, which
    // would otherwise report it as world-offscreen-om-count.
    if(const char* why=world::rt_count_why(s3.live))return why;
    // A world draw is replayed only when it writes the producer's scene RT (offscreen passes, e.g. 128x128, never). With no known
    // scene RT the draw is REFUSED (Astra issue-31 P2-8: the identity qualification is mandatory and independent of the optional world-texture
    // allocation; before, a failed snapshot allocation silently dropped the scene-target filter in the member path).
    if(!step5WobjSceneOptional()||s5.worldReady)
        if(const char* why=world::scene_target_why(s3.live,s5.worldReady,s5.worldSource,s5.worldW,s5.worldH,s5.worldFormat))return why;
    if(const char* why=step5EyeRsNote(s5,step5StateWhy(st,s5,s3.live)))return why;
    if(const char* why=step5WobjClassWhy(s5,s3.live))return why;
    if(f.poseBroken)return "world-pose-broken";
    // The IPD row-3 fallback is cockpit-specific: not used here. A world-led (early) frame has no cockpit PoseFrame yet: its own is checked by substitute().
    if(!f.poseValid||(!f.early&&!s5.pf.valid))return "world-pose-unavailable";
    if(w.failed)return w.failed;
    if(!step5WobjRead(st,s5))return "live-srt-unreadable";
    const uint8_t* raw=s3.liveSrt.data();
    if(const char* why=world::record_check(raw))return why;
    if(const char* why=::acz::props::dedupe_record_why(raw))return why;   // WORLD-4 propdedupe:1: a building the props pass draws (out of scope, not a loss)
    // J-world-evidence (Astra reply 5): the cb10 fade flag rule is enforced for every accepted draw; an unknown value refuses it.
    if(const char* why=world::cb10_check(s5.wobjCb10))return why;
    if(!w.camera) {
        ++s5.wobjCamEval;
        const char* why=world::camera_setup(raw,&s5.pf.cam,w,s5.scratch);
        // The SAME recentring neutral and head offsets as the cockpit, applied to the world camera.
        if(!why&&setup_poses(f.pose,s5.neutral,w.pf,s5.scratch,hmdFovConfigured(),worldUnitsPerMetreConfigured(),lookDriveConfigured()?s5.lookDrive.compensation():nullptr,leanCfg().get()))why="world-pose-setup-failed";
        if(!why)why=world::fov_matches(w.pf,s5.pf);
        if(why){w.failed=why;w.camera=false;++s5.wobjCamFail;s5.wobjCamLastWhy=why;return why;}
        ++s5.wobjCamOk;step5WobjClipNote(s5,w);
        f.gameEyeValid=true;for(int k=0;k<3;++k)f.gameEye[k]=float(w.eyeAbs[k]); // WORLD-1: the absolute game camera of this frame (terrain position)
        step5InterpFrame(s5,f,w); // OBJECT-INTERP (key off: returns)
        if(!s5.wobjFirstEmitted&&!s5.wobjFirstPending) { // facts of the first recovered world camera (logged with the first published frame)
            for(int k=0;k<3;++k){s5.wobjFirstEye[k]=w.eyeAbs[k];s5.wobjFirstModel[k]=w.model[k];}
            s5.wobjFirstLens[0]=w.pf.cam.tanX;s5.wobjFirstLens[1]=w.pf.cam.tanY;
            for(int e=0;e<2;++e){double d=0;for(int k=0;k<3;++k){const double x=w.pf.eyeWorld[e][k]-w.pf.cam.eye[k];d+=x*x;}s5.wobjFirstOffset[e]=std::sqrt(d);}
        }
    }
    // Every record must carry the frame's camera (a record that just set the camera up matches itself, as before).
    // POV-RENDER stage 2 (no-cockpit chase frames only): a camera-relative record (the player's aircraft: A = T(p) x A_ref) is the same camera.
    bool rel=false;double relP[3]{};
    if(const char* why=world::record_matches(raw,w)) {
        if(!f.noCockpit)return why;
        if(const char* rw=world::relative_offset(raw,w,relP)){++s5.povRelativeRefused;return std::strcmp(rw,"world-vp-differs")==0?why:rw;}
        rel=true;++s5.povRelative;
    }
    const float depthScale=step5WobjDepthScale(s5);
    for(unsigned e=0;e<2;++e)
        if(const char* why=rel?world::substitute_relative(raw,w.pf,e,relP,s3.inlineBlobs[1+e].data(),depthScale)
                              :world::substitute(raw,w.pf,e,s3.inlineBlobs[1+e].data(),depthScale))return why;
    step5InterpObject(s5,f,raw,s3.live,s3.inlineBlobs[1].data(),s3.inlineBlobs[2].data()); // OBJECT-INTERP (key off: returns)
    return nullptr;
}
// J-180 P2 (step-1 mutex held): eligible world draws of the CURRENT list generation that were lost before its frame opened (early-open refusals,
// back-off / list-blocked skips). The frame that later opens in that list inherits them (a cockpit-opened frame included).
void step5WobjPreLoss(Step5State& s5,Step1Route route,uint64_t gen) noexcept {
    if(s5.wobjPreRoute!=route||s5.wobjPreGen!=gen){s5.wobjPreRoute=route;s5.wobjPreGen=gen;s5.wobjPreLost=0;}
    ++s5.wobjPreLost;++s5.wobjPreLostSum;
}
void step5WobjInheritLoss(Step5State& s5,Step5Frame& f) noexcept {
    if(s5.wobjPreLost&&s5.wobjPreRoute==f.list&&s5.wobjPreGen==f.generation){f.worldIncomplete=true;f.wobjLost+=s5.wobjPreLost;s5.wobjLostSum+=s5.wobjPreLost;}
    s5.wobjPreLost=0;
}
// J-world-objects v2 (same-list early frame start). The first DrawACM_v draw that writes the producer's scene RT in a qualified list
// OPENS the producer frame: world camera from the record's +64 matrix (no cockpit camera to compare yet), pose read, neutral
// rebased, per-eye substitution proven, slot taken; the frame's one-time eye clears are recorded with this first member. Nothing is
// recorded before every check passed (a refusal leaves no command and no slot). The list's first accepted DrawCPT member ADOPTS the
// frame (step5Adopt). Draws on any other target never open a frame. Step-1 mutex held; false = not opened (reason counted).
__declspec(noinline) bool step5WobjEarlyOpen(Step1State& st,Step5State& s5,void* key,const std::shared_ptr<gpu_copy::ObservationFeed>& feed,
                                             unsigned& idx,MemberPhase& phase) noexcept {
    using namespace gpu_copy::step5;
    auto& s3=st.s3;auto& f=s5.frame;
    if(!byClassConfigured()||!s5.worldReady||!s5.classCount)return false;
    const Step1Route route=step1Route(key);
    step3Capture(st,key,s3.live,true);
    const uint64_t gen=s3.live.generation;
    if(!gen)return false;
    s5.wobjLiveFresh=false;s5.wobjReadStatus="not-read";
    const bool sceneDraw=world::scene_draw(s3.live,s5.worldReady,s5.worldSource,s5.worldW,s5.worldH,s5.worldFormat); // J-180 P2: in scope for the completeness account
    // POV-RENDER stage 2 (pov:2): the consumer's route is chase -> this opener may start a NO-COCKPIT frame (published unadopted). Off: request 0, nothing changes.
    const uint32_t povReq=acz::povr::producer_request().load(std::memory_order_relaxed);
    const int64_t povNowUs=step5MonoUs();
    const char* povWhy=acz::povr::no_cockpit_why(povReq,povNowUs,s5.povCockpitHoldUntilUs);
    bool povNc=povWhy==nullptr;
    if(s5.wobjEarlyHold&&!povNc) { // backed off after consecutive frames that no cockpit member adopted (a no-cockpit chase frame is never abandoned: no back-off)
        if(!(s5.wobjBlockRoute==route&&s5.wobjBlockGen==gen)){--s5.wobjEarlyHold;s5.wobjBlockRoute=route;s5.wobjBlockGen=gen;}
        ++s5.wobjEarlyBlocked;step5WorldEvidenceNote("world-early-backoff-hold");
        if(sceneDraw)step5WobjPreLoss(s5,route,gen);
        return false;
    }
    if(s5.wobjBlockRoute==route&&s5.wobjBlockGen==gen){ // list-level refusal already counted
        ++s5.wobjEarlyBlocked;step5WorldEvidenceNote("world-early-list-blocked");
        if(sceneDraw)step5WobjPreLoss(s5,route,gen);
        return false;
    }
    auto refuse=[&](const char* why,bool block)->bool{
        ++s5.wobjEarlyReason[world::category(why)];s5.wobjEarlyLastWhy=why;
        if(world::loss_eligible(why,s3.live,s5.worldReady,s5.worldSource,s5.worldW,s5.worldH,s5.worldFormat))step5WobjPreLoss(s5,route,gen);
        if(block){s5.wobjBlockRoute=route;s5.wobjBlockGen=gen;}
        step5WorldEvidenceDecision(st,s5,key,why,true); // J-world-evidence: counted by reason, bounded record snapshot
        return false;
    };
    if(const char* why=world::rt_count_why(s3.live))return refuse(why,false); // J-world-evidence: rt-count before the scene-target rule
    if(const char* why=world::scene_target_why(s3.live,true,s5.worldSource,s5.worldW,s5.worldH,s5.worldFormat))return refuse(why,false);
    gpu_copy::Identity id{};uint64_t qgen=0;unsigned qwhy=0;
    if(!feed||!feed->insertion_candidate(id,qgen,qwhy))return refuse("world-early-list-unqualified",true);
    {const auto facts=st.registry.timeline();
     if(!facts.bound||facts.lost||id.device!=facts.device||id.queue!=facts.queue||id.fence!=facts.fence)return refuse("world-early-timeline-tokens",true);}
    if(const char* why=step5EyeRsNote(s5,step5StateWhy(st,s5,s3.live)))return refuse(why,false);
    if(const char* why=step5WobjClassWhy(s5,s3.live))return refuse(why,false);
    step5WobjCb10(s5,key); // J-world-evidence: this draw's cb10 dword 0 from the root-constant witness (resolved before the record is judged)
    if(!step5WobjRead(st,s5))return refuse("live-srt-unreadable",false);
    const uint8_t* raw=s3.liveSrt.data();
    if(const char* why=world::record_check(raw))return refuse(why,false);
    if(const char* why=::acz::props::dedupe_record_why(raw))return refuse(why,false);   // WORLD-4 propdedupe:1 (does not open the frame; the next record may)
    if(const char* why=world::cb10_check(s5.wobjCb10))return refuse(why,false); // the fade branch (flags & 8) reads C, which is not substituted
    auto& w=s5.wobj;
    w.camera=false;w.failed=nullptr;++s5.wobjCamEval;
    const char* why=world::camera_setup(raw,nullptr,w,s5.scratch);
    // POV-RENDER stage 2: the opener's A camera must be the chase camera of the raw E6 view sample (a camera-relative first draw, e.g. the player's
    // aircraft, is refused WITHOUT blocking the list: a later absolute draw may open the frame). Stages 3 + 4 (pov:3): the same check for the
    // requested kind (HUD-only: the E6 eye; cinematic: the E6 'other' camera within 2,000 units of the aircraft).
    double povRig[9]{};   // rig right, up, fwd (the axes the eyes ride on; published as the frame's game-camera axes)
    const acz::povr::NcKind povKind=acz::povr::req_kind(povReq);
    double povOff[3]{};bool povOffOn=false;   // stage 4 anchor: the eyes stand at the anchor = the A camera + povOff
    if(!why&&povNc) {
        acz::viewmode::Geo e6{};uint64_t e6n=0;const bool e6ok=acz::viewmode::view_mailbox().read(e6,e6n);
        // A-only (pov:3 escalation: a cutscene / replay camera the E6 block does not show): the game's render camera A alone, never a relative draw
        const char* pw=acz::povr::req_a_only(povReq)?acz::povr::opener_check_a_only(w.eyeAbs,e6ok?&e6:nullptr):acz::povr::opener_check(w.eyeAbs,e6ok?&e6:nullptr,povKind);
        if(pw){++s5.povOpenRefused;s5.povLastWhy=pw;w.camera=false;
               if(acz::povr::is_e6_mismatch(pw))acz::povr::producer_e6_mismatch().fetch_add(1,std::memory_order_relaxed);   // the consumer's escalation evidence
               return refuse(pw,false);}
        const Camera& c=w.pf.cam;double yaw=0;double apos[3]{};const char* aev=nullptr;
        if(povKind==acz::povr::NcKind::hud) {   // stage 3: the cockpit eye without the cockpit: the game camera exactly (head 6DoF about the eye)
            for(int k=0;k<3;++k){povRig[k]=c.right[k];povRig[3+k]=c.up[k];povRig[6+k]=c.fwd[k];}
        } else if(povKind==acz::povr::NcKind::cinematic&&acz::povr::req_level(povReq)&&s5.povAnchor.step(w.eyeAbs,c.fwd,povNowUs,apos,yaw,aev)) {
            s5.povRigYaw=yaw;s5.povAnchorLast=aev;acz::povr::level_axes(yaw,&povRig[0],&povRig[3],&povRig[6]);   // stage 4: the anchor rig
            acz::povr::producer_anchor_events().store(s5.povAnchor.enters()+s5.povAnchor.cuts()+s5.povAnchor.relatches(),std::memory_order_relaxed);
            for(int k=0;k<3;++k)povOff[k]=apos[k]-w.eyeAbs[k];
            povOffOn=true;
        } else if(povKind==acz::povr::NcKind::chase&&acz::povr::req_level(povReq)&&s5.povRig.step(c.fwd,povNowUs,acz::povr::req_tc_ms(povReq),yaw)) {
            s5.povRigYaw=yaw;acz::povr::level_axes(yaw,&povRig[0],&povRig[3],&povRig[6]);
        } else for(int k=0;k<3;++k){povRig[k]=c.right[k];povRig[3+k]=c.up[k];povRig[6+k]=c.fwd[k];}   // povrig:game: the game camera itself
        acz::povr::rig_look(c.right,c.up,c.fwd,&povRig[0],&povRig[3],&povRig[6],s5.povLookR);
    }
    acz::stereo::EyePose pose[2]{};uint64_t predicted=0;uint32_t spaceGeneration=0;uint64_t earlyPoseSeq=0;
    if(!why) {
        auto* provider=g_step5Pose.load();
        if(!provider||!provider->try_read_ex(pose,&predicted,&spaceGeneration))why="world-pose-unavailable";
        else {
            earlyPoseSeq=provider->last_sequence(); // timing-anchor diagnostics
            // xr_look_drive_keys (J-179 finding 4): this frame's cockpit is not set up yet, so the look of THIS frame is derived from the
            // world camera against the aircraft reference of the previous valid cockpit measurement. Without a reference the early open is
            // refused (the cockpit member then opens the frame with its own measurement). The adoption compares it with the cockpit's.
            // POV-RENDER stage 2: a no-cockpit frame has no cockpit look; the head rides on the rig (lookR = camera-local -> rig-local).
            const bool ld=lookDriveConfigured()&&!povNc;const double* lookR=povNc&&povKind!=acz::povr::NcKind::hud?s5.povLookR:nullptr;   // HUD-only: the camera itself
            s5.wobjEarlyLookUsed=false;
            if(ld) {
                step5LookDriveEnsure(s5);s5.lookDrive.sweep_poses(pose,lookDriveNowMs());
                lookR=s5.lookDrive.estimate_current(w.pf.cam.right,w.pf.cam.up,w.pf.cam.fwd,lookDriveNowMs());
                if(!lookR)why="world-pose-look-reference";
                else {s5.wobjEarlyLookUsed=true;std::memcpy(s5.wobjEarlyLookR,lookR,sizeof s5.wobjEarlyLookR);}
            }
            if(!why&&setup_poses_generation(pose,spaceGeneration,s5.neutral,w.pf,s5.scratch,hmdFovConfigured(),worldUnitsPerMetreConfigured(),lookR,leanCfg().get(),
                                            povOffOn?povOff:nullptr))why="world-pose-setup-failed";   // stage 4: the eyes at the anchor
        }
    }
    if(!why){const float depthScale=step5WobjDepthScale(s5);for(unsigned e=0;e<2&&!why;++e)why=world::substitute(raw,w.pf,e,s3.inlineBlobs[1+e].data(),depthScale);}
    if(why){w.failed=why;w.camera=false;++s5.wobjCamFail;s5.wobjCamLastWhy=why;return refuse(why,true);}
    ++s5.wobjCamOk;step5WobjClipNote(s5,w);
    // Slot: try-only, as the cockpit start (never waits).
    const uint64_t done=st.fence.fence?st.fence.completed(step1Route(st.fence.fence)):0;
    if(done==UINT64_MAX){s5.disabled=true;s5.disableReason="device-lost-uint64max";return false;}
    if(!step5OwnerRoom_locked(s5))return refuse("world-early-owner-table-full",true); // J-180 P1 (see the cockpit start)
    if(!s5.meta.try_lock())return refuse("world-early-slot-metadata-busy",true);
    bool busy=false;const int pick=step5PickSlot_locked(s5,done,busy);
    if(pick>=0)s5.slot[size_t(pick)].state=gpu_copy::step5::slot_producing;
    s5.meta.unlock();
    if(pick<0){if(busy)++s5.gpuBusy;return refuse("world-early-no-slot",true);}
    if(s5.pend.set&&s5.pend.list==route)s5.pend.set=false; // this generation produces a frame
    step5ClearFrame(f);step5TacOpen(s5);step5MslOpen(s5); // TACTICAL (key off: latches 0 and returns)
    f.active=true;f.slot=unsigned(pick);f.list=route;f.generation=gen;f.lastOrdinal=0;f.byClass=true;f.early=true;
    s5.fxTrail.atlas.slot_started(f.slot);s5.fxTrail.set_slot(f.slot); // (M6) fxatlas: a new recording holds the slot (its previous one retired)
    step5WobjInheritLoss(s5,f); // J-180 P2: eligible world draws of this list lost before this opener
    for(unsigned e=0;e<2;++e)f.pose[e]=pose[e];
    f.predicted=predicted;f.spaceGeneration=spaceGeneration;f.poseValid=true;
    f.diagPoseSeq=earlyPoseSeq;f.diagStartUs=step5MonoUs();f.diagLookSrc=s5.wobjEarlyLookUsed?2:0; // the early look is the estimate derived from the world camera
    f.neutral=gpu_copy::step5::neutral_export(s5.neutral,true);
    f.gameEyeValid=true;for(int k=0;k<3;++k)f.gameEye[k]=float(w.eyeAbs[k]); // WORLD-1: the absolute game camera of this frame (terrain position)
    f.commands=(s3.targets.has_depth?4u:2u)+s5.perMember;
    f.recorded=1;f.wobjRecorded=1;
    idx=0;phase=MemberPhase{true,false,false};
    if(povNc) {   // POV-RENDER stage 2: a NO-COCKPIT chase frame: lens = the A camera's, rig axes as the frame camera, world snapshot + HUD region at this opener
        f.noCockpit=true;f.diagLookSrc=0;
        f.lensValid=true;f.lens[0]=f.lens[1]=float(w.pf.cam.tanX);f.lens[2]=f.lens[3]=float(w.pf.cam.tanY);
        f.gameCamValid=true;for(int k=0;k<9;++k)f.gameCam[k]=float(povRig[k]);
        if(povOffOn)for(int k=0;k<3;++k)f.gameEye[k]=float(w.eyeAbs[k]+povOff[k]);   // stage 4: terrain and objects from the anchor (one camera)
        ++s5.povOpenedKind[unsigned(povKind)<3u?unsigned(povKind):0u];
        if(acz::povr::req_a_only(povReq))++s5.povOpenedAOnly;
        // the eyes' own frustum (J-cockpit-fov: the consumer submits the layer with exactly the FOV the images were rendered with)
        f.fovHmd=w.pf.fovHmd;f.fovClamped=f.fovHmd&&w.pf.fovClamped;f.fovWhy=w.pf.fovWhy;
        if(f.fovHmd)for(int e=0;e<2;++e)for(int i=0;i<4;++i)f.fov[e][i]=w.pf.fov[e].angle[i];
        f.worldWhy=step5WorldWhy(s5,s3.live);f.worldValid=!f.worldWhy&&s5.slot[f.slot].world;
        f.worldEvidence=f.worldValid?"tracked-transition-to-RENDER_TARGET-in-generation":nullptr;
        f.commands+=f.worldValid?step5WorldCommands:0u;
        phase=MemberPhase{true,f.worldValid,true};
        ++s5.povOpened;s5.povLastWhy="opened";
    }
    ++s5.wobjEarlyOpened;
    step5InterpFrame(s5,f,w);step5InterpObject(s5,f,raw,s3.live,s3.inlineBlobs[1].data(),s3.inlineBlobs[2].data()); // OBJECT-INTERP (key off: returns)
    step5TacObject(s5,raw,s3.live); // TACTICAL: the opener is a replayed world draw too (key off: returns)
    step5FxTrailObject(s5,raw,true); // EFFECTS-OUTSIDE (M3) fxtrail: the near rule's object positions (option off: returns)
    step5FxSmokeObjectA(f,raw); // EFFECTS-OUTSIDE (C) fxmem:1: the frame's A for the fx-smoke-frame comparison (flag off: returns)
    step5WorldEvidenceDecision(st,s5,key,nullptr,true); // J-world-evidence: accepted (the frame is opened by this draw)
    if(!s5.wobjFirstEmitted&&!s5.wobjFirstPending) { // facts of the first recovered world camera (logged with the first published frame)
        for(int k=0;k<3;++k){s5.wobjFirstEye[k]=w.eyeAbs[k];s5.wobjFirstModel[k]=w.model[k];}
        s5.wobjFirstLens[0]=w.pf.cam.tanX;s5.wobjFirstLens[1]=w.pf.cam.tanY;
        for(int e=0;e<2;++e){double d=0;for(int k=0;k<3;++k){const double x=w.pf.eyeWorld[e][k]-w.pf.cam.eye[k];d+=x*x;}s5.wobjFirstOffset[e]=std::sqrt(d);}
    }
    return true;
}
// The first accepted DrawCPT member of an early frame's list continues it (step-1 mutex held; s3.live captured, s3.liveSrt read).
// Cockpit camera, lens and PoseFrame are set up with the frame's own pose; the world camera must have the cockpit's orientation and
// lens, and the same FOV mode/tangents. A genuine mismatch poisons the frame (it executes, is never published, its slot is retired by
// the fence): the world members were already recorded with the world camera. A cockpit camera/pose that merely could not be recovered
// from THIS member is refused alone (the next member may adopt). nullptr = adopted.
__declspec(noinline) const char* step5Adopt(Step1State& st,Step5State& s5) noexcept {
    namespace world=gpu_copy::step5::world;
    auto& f=s5.frame;auto& s3=st.s3;
    if(f.noCockpit) {   // POV-RENDER stage 2: a cockpit draw in a no-cockpit chase frame (V back to the cockpit): poison it, ordinary frames for 1 s
        f.adopted=true;f.poseBroken=true;f.stopped=true;f.stop="pov-no-cockpit-frame-saw-cockpit";f.partial=true;
        s5.povCockpitHoldUntilUs=step5MonoUs()+acz::povr::kCockpitHoldUs;++s5.povCockpitPoisoned;s5.povLastWhy="cockpit-seen";
        s5.wobjAdoptLastWhy="pov-no-cockpit-frame-saw-cockpit";
        return "pov-no-cockpit-frame-saw-cockpit";
    }
    step5FrameSetup(st,s5,true);
    const char* bad=nullptr;
    if(!f.lensValid)bad="adopt-cockpit-camera-unrecoverable";
    else if(!f.poseValid)bad="adopt-cockpit-pose-unavailable";
    if(bad){s5.wobjAdoptLastWhy=bad;f.poseValid=true;return bad;} // f.poseValid stays the early pose; retried at the next member
    if(!s5.wobj.camera)bad="adopt-world-camera-missing";
    else if(const char* why=world::camera_matches_cockpit(s5.wobj.pf.cam,s5.pf.cam))bad=why;
    else if(const char* why=world::fov_matches(s5.wobj.pf,s5.pf))bad=why;
    // xr_look_drive_keys: the world members were built with the look estimated at the frame's start; the cockpit's own measurement is the
    // truth. A difference beyond the aircraft's per-frame rotation means world and cockpit would disagree about the look: poison the frame.
    else if(s5.wobjEarlyLookUsed&&lookDriveConfigured()) {
        if(look_drive::rotation_angle(s5.wobjEarlyLookR,s5.lookDrive.compensation())>look_drive::Driver::early_look_tolerance_rad)bad="adopt-look-compensation-mismatch";
    }
    s5.wobjEarlyLookUsed=false;
    if(bad) {
        f.adopted=true;f.poseBroken=true;f.stopped=true;f.stop="adopt-mismatch";f.partial=true;
        ++s5.wobjEarlyMismatch;s5.wobjAdoptLastWhy=bad;
        return bad;
    }
    f.adopted=true;
    f.worldWhy=step5WorldWhy(s5,s3.live);f.worldValid=!f.worldWhy&&s5.slot[f.slot].world;
    f.worldEvidence=f.worldValid?"tracked-transition-to-RENDER_TARGET-in-generation":nullptr;
    f.commands+=f.worldValid?step5WorldCommands:0u;
    ++s5.wobjEarlyAdopted;
    step5NoteWorldPitch(s5,f,s3.live,int(f.slot)); // J-test8-fixes: the world refusal sub-condition / pitch diagnostics also on the EARLY frame-open path (test 8 was blind)
    return nullptr;
}
// true: record this draw as the frame's next member (idx). Step-1 mutex held. A world draw may OPEN the frame (early start).
__declspec(noinline) bool step5WobjMember(Step1State& st,Step5State& s5,void* key,const std::shared_ptr<gpu_copy::ObservationFeed>& feed,
                                          unsigned& idx,MemberPhase& phase) noexcept {
    using namespace gpu_copy::step5;
    auto& s3=st.s3;auto& f=s5.frame;
    ++s5.wobjSeen;
    if(!f.active) {
        if(step5WobjEarlyOpen(st,s5,key,feed,idx,phase))return true;
        ++s5.wobjBeforeFrame;return false; // not a frame start (reason in wobjEarlyReason): earlier-pass / offscreen draws land here
    }
    if(f.closed||f.stopped){++s5.wobjBeforeFrame;step5WorldEvidenceNote("world-frame-closed-or-stopped");return false;}
    if(step1Route(key)!=f.list){++s5.wobjOtherList;step5WorldEvidenceNote("world-other-list-while-frame-active");return false;} // an active frame, a DIFFERENT list: never replayed (counted)
    step3Capture(st,key,s3.live,true);
    if(s3.live.generation!=f.generation)return false;
    const char* bad=nullptr;
    s5.wobjLiveFresh=false;s5.wobjReadStatus="not-read";
    // J-test8-fixes: draw_cap 96 and a cockpit reserve (a world burst never takes the member slots the cockpit members after it need).
    if(f.recorded+world::cockpit_reserve>=s5.memberCap||f.commands+s5.perMember>s5.commandCap||f.wobjRecorded>=world::draw_cap)bad="world-cap";
    else {step5WobjCb10(s5,key);bad=step5WobjWhy(st,s5);} // J-world-evidence: cb10 resolved first, enforced inside step5WobjWhy
    step5WorldEvidenceDecision(st,s5,key,bad,false);
    if(bad) {
        ++s5.wobjRefusedSum;++f.wobjRefused;++s5.wobjReason[world::category(bad)];s5.wobjLastWhy=bad;
        // J-180 P2: the loss of an ELIGIBLE scene draw (cap, unreadable, record, camera, pose, cb10, class, state) makes the frame world-incomplete;
        // an offscreen / out-of-scope draw does not.
        if(world::loss_eligible(bad,s3.live,s5.worldReady,s5.worldSource,s5.worldW,s5.worldH,s5.worldFormat)){f.worldIncomplete=true;++f.wobjLost;++s5.wobjLostSum;}
        return false;
    }
    idx=f.recorded;++f.recorded;++f.wobjRecorded;f.commands+=s5.perMember;
    step5TacObject(s5,s3.liveSrt.data(),s3.live); // TACTICAL (key off: returns)
    step5FxTrailObject(s5,s3.liveSrt.data(),false); // EFFECTS-OUTSIDE (M3) fxtrail (option off: returns)
    step5FxSmokeObjectA(f,s3.liveSrt.data()); // EFFECTS-OUTSIDE (C) fxmem:1 (flag off: returns)
    return true;
}
__declspec(noinline) void step5Draw(ID3D12GraphicsCommandList* self,uint64_t countInstances,uint64_t startBase,uint64_t firstIndexed) noexcept {
    auto* st=g_step1.load();auto* s5p=g_step5.load();if(!st||!s5p)return;
    auto& s5=*s5p;
    if(worldEvidenceConfigured())step5WorldEvidenceDraw(self); // J-world-evidence (Astra reply 5; RT slot 1 metadata + all-draw ordinal; default OFF)
    airDiagDraw(self,countInstances,firstIndexed); // READ-ONLY aircraft diagnostics (fxsig:ac0f1a60; flag off: one relaxed load)
    if(worldObserveConfigured())step5WorldObserveDraw(self); // J-world-objects v2 step 1 (read-only; default OFF)
    try {
        void* key=nullptr;std::shared_ptr<gpu_copy::ObservationFeed> feed;uint64_t pipelineId=0;bool world=false;
        {auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);
         auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));if(it==s.states.end())return;
         auto p=s.pipelines.find(it->second.pso.get());if(p==s.pipelines.end())return;
         if(!p->second.drawcpt_identity_matched) {
             // J-world-objects: only a DrawACM-identity PSO (never set with the key off) takes the world path.
             if(!p->second.acm_world_matched||!worldObjectsConfigured())return;
             world=true;
         }
         pipelineId=uint64_t(p->second.identity);
         feed=it->second.copyObservation;}
        unsigned slot=0,idx=0;bool record=false;MemberPhase phase{};
        {std::lock_guard lock(st->mutex);auto& s3=st->s3;auto& f=s5.frame;
         if(s5.disabled||!s5.ringReady||!s3.memberCount||g_step1Phase.load()!=static_cast<int>(Step1Phase::continuous))return;
         step5ApplyPending(s5);
         s3.aCount=UINT(countInstances);s3.aInstances=UINT(countInstances>>32);s3.aStart=UINT(startBase);
         s3.aBase=INT(uint32_t(startBase>>32));s3.aStartInstance=UINT(firstIndexed);s3.aIndexed=(firstIndexed>>32)!=0;
         if(world) {
             if(step5WobjMember(*st,s5,key,feed,idx,phase)){record=true;slot=f.slot;
                 // POV-RENDER stage 2: a no-cockpit opener arms the HUD region (the cockpit's adopting member does it otherwise); else unchanged.
                 const bool povArm=f.noCockpit&&phase.hudArm;
                 if(povArm)hudMember(f,s5.worldSource,s3.live.draw_ordinal,true);
                 step5HudMember_locked(s5,f,s3.live,povArm);}
         } else {
         const Step1Route route=step1Route(key);
         const bool byClass=byClassConfigured(); // F3 (default OFF)
         if(!f.active) {
             step3Capture(*st,key,s3.live,true);
             const uint64_t lo=s3.members[0].shape.draw_ordinal,hi=s3.members[s3.memberCount-1].shape.draw_ordinal;
             const uint64_t ord=s3.live.draw_ordinal,gen=s3.live.generation;
             if(precockpitConfigured())step5PrecockpitObserve(*st,s5,route,pipelineId); // F2: read-only
             if(!byClass) {
                 if(ord<lo||ord>hi)return; // not the cockpit range: not a frame start
             } else {
                 // F3: any ordinal; a draw outside the admitted classes is not a cockpit draw (counted,
                 // never a pending skip). Its own state is not judged here.
                 int index=0;if(step5ClassVerdict(s5,step5ClassOf(s3.live),index)){++s5.classOutStarts;return;}
             }
             // The frame's list recording must qualify exactly as a step-3 insertion.
             gpu_copy::Identity id{};uint64_t qgen=0;unsigned why=0;
             if(!feed||!feed->insertion_candidate(id,qgen,why)){step5Pending(s5,route,gen,skip_unqualified,"list-unqualified");return;}
             const auto facts=st->registry.timeline();
             if(!facts.bound||facts.lost||id.device!=facts.device||id.queue!=facts.queue||id.fence!=facts.fence)
                 {step5Pending(s5,route,gen,skip_unqualified,"timeline-tokens");return;}
             if(const char* bad=step5MemberWhy(*st,s5,s3.live)) {
                 if(byClass) {
                     ++s5.classReason[gpu_copy::step5::member_category(bad)];s5.classLastWhy=bad;
                     if(gpu_copy::step5::other_target(bad))return; // F3: another target's draw is not a cockpit frame start
                 }
                 if(!(s5.pend.set&&s5.pend.list==route&&s5.pend.gen==gen&&s5.pend.kind==skip_mismatch))step5ClearDetail(s5.pend.detail);
                 (void)step5Diff(*st,s3.live,bad,s5.pend.detail);step5Pending(s5,route,gen,skip_mismatch,bad);return;
             }
             if(!step3InlineSrt(*st)){step5Pending(s5,route,gen,skip_unreadable,"live-srt-unreadable");return;}
             if(byClass) {
                 // F3: the frame's camera comes from its first member; with class membership that may
                 // be a draw never replayed before, so it must yield a camera (else try the next draw).
                 gpu_copy::step5::Lens probe;
                 if(const char* why=gpu_copy::step5::camera_frame(s3.liveSrt.data(),s5.pf,s5.scratch,probe))
                     {(void)why;++s5.classReason[gpu_copy::step5::member_camera];step5Pending(s5,route,gen,skip_unreadable,"camera-unrecoverable");return;}
             }
             // Slot: try-only; the generation is skipped when none is free or the GPU has
             // not completed the slot's previous use (J-3b9, same private fence; nonblocking).
             const uint64_t done=st->fence.fence?st->fence.completed(step1Route(st->fence.fence)):0;
             if(done==UINT64_MAX){s5.disabled=true;s5.disableReason="device-lost-uint64max";return;}
             // J-180 P1: a NEW replay-bearing recording needs an ownership entry; a full table refuses it rather than evicting an executable writer.
             if(!step5OwnerRoom_locked(s5)){step5Pending(s5,route,gen,skip_no_slot,"owner-table-full");return;}
             if(!s5.meta.try_lock()){step5Pending(s5,route,gen,skip_no_slot,"slot-metadata-busy");return;}
             bool busy=false;const int pick=step5PickSlot_locked(s5,done,busy);
             if(pick>=0)s5.slot[size_t(pick)].state=gpu_copy::step5::slot_producing;
             s5.meta.unlock();
             if(pick<0){if(busy)++s5.gpuBusy;step5Pending(s5,route,gen,skip_no_slot,busy?"slot-gpu-not-complete":"no-free-slot");return;}
             const bool hadPend=s5.pend.set&&s5.pend.list==route&&s5.pend.gen==gen&&(s5.pend.kind==skip_mismatch||s5.pend.kind==skip_unreadable);
             if(s5.pend.set&&s5.pend.list==route)s5.pend.set=false; // this generation produces a frame
             step5ClearFrame(f);step5TacOpen(s5);step5MslOpen(s5);f.active=true;f.slot=unsigned(pick);f.list=route;f.generation=gen;f.lastOrdinal=ord;f.byClass=byClass;
             s5.fxTrail.atlas.slot_started(f.slot);s5.fxTrail.set_slot(f.slot); // (M6) fxatlas: a new recording holds the slot (its previous one retired)
             step5WobjInheritLoss(s5,f); // J-180 P2 (key off: wobjPreLost is always 0)
             // An earlier selected member was refused (F3: any earlier in-class refusal of this generation;
             // out-of-class draws never leave a pending refusal).
             if(hadPend&&(byClass||s5.pend.detail.selFound)){f.partial=true;++f.refusedMembers;f.detail=s5.pend.detail;}
             f.worldWhy=step5WorldWhy(s5,s3.live);f.worldValid=!f.worldWhy&&s5.slot[size_t(pick)].world;
             f.worldEvidence=f.worldValid?"tracked-transition-to-RENDER_TARGET-in-generation":nullptr;
             step5FrameSetup(*st,s5);step5PoseBlobs(*st,s5);step5RefMember(*st,s5);
             step5NoteWorldPitch(s5,f,s3.live,pick);
             f.diff=byClass?false:step5Diff(*st,s3.live,nullptr,f.detail); // F3: no ordinal comparison
             f.recorded=1;f.commands=(s3.targets.has_depth?4u:2u)+s5.perMember+(f.worldValid?step5WorldCommands:0u);
             record=true;phase=MemberPhase::first_member(true);slot=f.slot;idx=0;
         } else {
             if(route!=f.list||f.closed||f.stopped)return;
             step3Capture(*st,key,s3.live,true);
             if(s3.live.generation!=f.generation||s3.live.draw_ordinal<=f.lastOrdinal)return;
             f.lastOrdinal=s3.live.draw_ordinal;
             if(precockpitConfigured())step5PrecockpitObserve(*st,s5,route,pipelineId); // F2: read-only
             if(f.recorded>=s5.memberCap||f.commands+s5.perMember>s5.commandCap) {
                 f.stopped=true;f.stop="member-cap";
                 if(f.byClass){f.partial=true;f.capped=true;++s5.classReason[gpu_copy::step5::member_cap];s5.classLastWhy="member-cap";}
                 return;
             }
             const char* bad=step5MemberWhy(*st,s5,s3.live);
             if(!bad&&!step3InlineSrt(*st))bad="live-srt-unreadable";
             // J-world-objects v2: the first accepted cockpit member of a frame opened by a world draw ADOPTS it (cockpit camera and
             // lens verified against the world camera, world snapshot and HUD region at this member, one-time clears already done).
             const bool adopting=f.early&&!f.adopted;
             if(!bad&&adopting){bad=step5Adopt(*st,s5);if(!bad)phase=MemberPhase{false,true,true};}
             // F3: every member's own camera must be the frame's (the first member's) camera.
             else if(!bad&&f.byClass&&f.lensValid)bad=gpu_copy::step5::camera_differs(s3.liveSrt.data(),s5.pf,s5.scratch);
             if(bad) {
                 // Refused alone. Only a SELECTED member ordinal makes the frame partial;
                 // another in-range draw that fails (e.g. excluded at selection) is just not replayed.
                 // F3: every refused IN-CLASS draw makes it partial; out-of-class draws are not cockpit draws.
                 ++f.refusedMembers;++s5.memberRefused;s5.lastMemberWhy=bad;
                 if(f.byClass) {
                     ++s5.classReason[gpu_copy::step5::member_category(bad)];s5.classLastWhy=bad;
                     if(!gpu_copy::step5::out_of_class(bad)&&!gpu_copy::step5::other_target(bad)){f.partial=true;(void)step5Diff(*st,s3.live,bad,f.detail);}
                 } else if(step5IsSelected(*st,s3.live.draw_ordinal)){f.partial=true;(void)step5Diff(*st,s3.live,bad,f.detail);}
                 return;
             }
             if(!f.byClass&&step5Diff(*st,s3.live,nullptr,f.detail))f.diff=true;
             step5PoseBlobs(*st,s5);step5RefMember(*st,s5);
             idx=f.recorded;++f.recorded;f.commands+=s5.perMember;
             record=true;slot=f.slot;
         }
         if(record){hudMember(f,s5.worldSource,s3.live.draw_ordinal,phase.hudArm);step5HudMember_locked(s5,f,s3.live,phase.hudArm);
             if(!phase.hudArm)step5EffectsMember_locked(s5,f);} // J-effects-layer: a later cockpit member commits the epoch before it
         }
         if(record&&s5.eyeRing){++s5.eyeVpSub;++s5.eyeScSub;++s5.eyeVpRestore;++s5.eyeMemberDraws;}} // EYE-RING: step5RecordMember rewrites and restores (counted under the mutex)
        if(record)step5RecordMember(self,*st,s5,slot,idx,phase);
    } catch(...) {std::lock_guard lock(st->mutex);if(s5.frame.active&&!s5.frame.stopped){s5.frame.stopped=true;s5.frame.stop="member-exception";s5.frame.partial=true;}}
}
// ------------------------------------------------------------- Close / Reset / Execute (game thread)
// Mismatch detail for the worker's rate-limited line (step-1 mutex held).
void step5OfferDetail(Step5State& s5,const Step5Detail& d,bool produced,unsigned members) noexcept {
    ++s5.mismatchEvents;
    if(s5.mismatchOut.set||!d.set)return;
    s5.mismatchOut=d;s5.mismatchOut.produced=produced;s5.mismatchOut.frameMembers=members;
}
void step5NoteClose(void* self,HRESULT hr) noexcept {
    auto* st=g_step1.load();auto* s5=g_step5.load();if(!st||!s5)return;
    void* key=nullptr;{auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);}
    std::lock_guard lock(st->mutex);auto& f=s5->frame;
    const auto route=step1Route(key);
    if(phaseObserveConfigured()){std::lock_guard pl(g_phMutex);const bool cockpit=g_ph.close(uint64_t(route),GetTickCount64()); // J-phase-observe: finalise the recording
        // burst protocol v2: a png sample recorded into THIS list keeps the observer's per-phase summary of the same frame (POD copy, no I/O)
        auto& p=s5->png;
        if(cockpit&&p.stage.load()==png_recorded&&p.list==route&&f.active&&p.generation==f.generation){p.ph=g_ph.last;p.phFrame=g_ph.lastFrame;p.phHave=true;}}
    if(s5->pend.set&&s5->pend.list==route&&!(f.active&&f.list==route)) {
        // A cockpit-range generation that produced nothing: one skipped frame.
        auto& p=s5->pend;p.set=false;
        ++s5->skipKind[p.kind];s5->skipped.fetch_add(1);s5->lastSkipWhy=p.why;
        if(p.kind==skip_mismatch){++s5->consecutiveMismatch;step5OfferDetail(*s5,p.detail,false,0);}
    }
    if(!f.active||f.closed||route!=f.list)return;
    if(s5->lookDrive.active())s5->lookDrive.ref_close(f.generation*1315423911ull^uint64_t(f.list),lookDriveNowMs()); // F-REF diagnostic / reference
    hudClose(f,SUCCEEDED(hr));
    if(f.hud.radar){const auto v=hudRegionRadarView(route,f.generation);f.hud.rd.excursions=v.excursions;f.hud.rd.returns=v.returns; // J-hud-radar
        f.hud.rd.sceneLeaves=v.sceneLeaves;f.hud.rd.sceneResumes=v.resumes;f.hud.rd.sceneBackRefused=v.sceneBackRefused;}            // RADAR-SCENE-RESUME
    if(hudReplayConfigured()){const char* end=hudRegionStop(route);f.hud.endWhy=end?end:"close";}
    if(f.eff.armed) {
        f.eff.ep.finish(); // J-effects-layer: whatever is still open came after the last member (the tail)
        // J-effects-smoke-fix: the predicate word of every logged replay as it stands at the Close = what the GPU reads (CPU-mapped UPLOAD memory).
        if(s5->effPredPtr)for(unsigned i=0;i<f.eff.diagN;++i) {
            auto& g=f.eff.diag[i];uint64_t w=0;std::memcpy(&w,s5->effPredPtr+g.offset,sizeof w);g.word=w;
        }
        step5FxSmokeGpuClose(*s5,f); // EFFECTS-OUTSIDE (C) fxmem:4: replays the GPU will execute (latched off: returns)
        step5FxSpriteGpuClose(*s5,f); // EFFECTS-OUTSIDE (F) fxmem:6: same (latched off: returns)
    }
    f.closed=true;f.closeOk=SUCCEEDED(hr);
}
void step5NoteReset(void* self) noexcept {
    auto* st=g_step1.load();auto* s5=g_step5.load();if(!st||!s5)return;
    void* key=nullptr;{auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);}
    std::lock_guard lock(st->mutex);auto& f=s5->frame;
    const auto route=step1Route(key);
    if(worldObserveConfigured()){std::lock_guard wl(g_woMutex);g_wo.discard(uint64_t(route));} // J-world-objects v2 step 1: unexecuted recordings
    if(worldEvidenceConfigured()){std::lock_guard wl(g_wevMutex);g_wev.discard(uint64_t(route));} // J-world-evidence: a re-recorded list restarts its ordinals
    if(phaseObserveConfigured()){std::lock_guard pl(g_phMutex);g_ph.discard(uint64_t(route));}   // J-phase-observe: a re-recorded list
    hudCancel(route);
    if(hudReplayConfigured())(void)hudRegionStop(route);
    if(s5->lastLive&&route==s5->lastList)s5->lastLive=false; // the published recording can no longer be re-executed
    step5OwnerRetire_locked(*s5,route); // J-180 P1: a successful Reset ends the recording's ability to write its slot / upload span
    if(s5->wobjPreRoute==route)s5->wobjPreLost=0;           // J-180 P2: a re-recorded list restarts its pre-open loss count
    if(s5->pend.set&&s5->pend.list==route)s5->pend.set=false; // reset unclosed: not a submitted frame
    if(f.active&&!f.submitted&&route==f.list) {
        // Discarded before any submission: the GPU never sees these commands.
        // J-hud-colour: a sample recorded into it never executes either: armed again.
        if(s5->png.stage.load()==png_recorded&&s5->png.list==f.list&&s5->png.generation==f.generation)
            {if(s5->png.burstFile){--s5->png.fileSeen;--s5->png.fileBursts;} // the file request was not served: pending again
             s5->png.stage.store(png_armed);g_step5PngWanted.store(true);g_step5SceneEndWanted.store(true);}
        ++s5->discarded;step5SetSlot(*s5,f.slot,gpu_copy::step5::slot_free);f.active=false;
    }
}
// J-180 P1: the final Release of a command list (destruction): its recording can never execute again. Called by the Release hook AFTER the hook-state
// mutex is dropped (lock order: step-1 mutex, then hook state). `route` was computed from the list key before the native Release.
void step5NoteListReleased(Step1Route route) noexcept {
    auto* st=g_step1.load();auto* s5=g_step5.load();if(!st||!s5||!route)return;
    std::lock_guard lock(st->mutex);
    step5OwnerRetire_locked(*s5,route);
}
// ------------------------------------------------------------- J-hud-colour: png_once (game thread)
// l3Close, BEFORE the native Close of the frame's list (only while g_step5PngWanted): when a sample
// is armed and THIS recording is the producer's active frame with a valid world snapshot and a valid
// HUD replay (armed, >= 1 draw, 0 refused, not pose-broken), append per image
//   Barrier(private RT -> COPY_SOURCE), CopyTextureRegion(READBACK footprint i, full box), Barrier(back)
// for the slot's HUD target, world snapshot and eye-0 target (9 commands, private resources only, all
// in RENDER_TARGET at that point of the list and again after it). The copies execute with the frame
// and complete with its private-fence Signal; nothing waits or maps on the game thread.
// J-effects-content: `images` = 3, or 4 when the effects target of the slot is copied too (image index 3, same recipe).
__declspec(noinline) void step5PngRecord(ID3D12GraphicsCommandList* self,ID3D12Resource* const* src,unsigned images,
                                         ID3D12Resource* readback,uint32_t w,uint32_t h,uint64_t pitch,uint64_t stride) noexcept {
    const auto barrier=reinterpret_cast<Step5BarrierFn>(step1Native(self,26));
    const auto copy=reinterpret_cast<Step5CopyTexFn>(step1Native(self,16));
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Flags=D3D12_RESOURCE_BARRIER_FLAG_NONE;b.Transition.Subresource=0;
    D3D12_TEXTURE_COPY_LOCATION dst{},from{};
    from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=0;
    dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT(gpu_copy::png::fp16_format);dst.PlacedFootprint.Footprint.Width=w;
    dst.PlacedFootprint.Footprint.Height=h;dst.PlacedFootprint.Footprint.Depth=1;dst.PlacedFootprint.Footprint.RowPitch=UINT(pitch);
    const D3D12_BOX box{0,0,0,w,h,1};
    for(unsigned i=0;i<images;++i) {
        b.Transition.pResource=src[i];b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier(self,1,&b);
        from.pResource=src[i];dst.PlacedFootprint.Offset=uint64_t(i)*stride;copy(self,&dst,0,0,0,&from,&box);
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier(self,1,&b);
    }
}
// J-scene-end: the scene render target (RENDER_TARGET here) into footprint `image_scene_end` of the readback: Barrier(RT->COPY_SOURCE), Copy, Barrier(back).
__declspec(noinline) void step5SceneEndCopy(ID3D12GraphicsCommandList* self,ID3D12Resource* scene,ID3D12Resource* readback,uint32_t w,uint32_t h,
                                            uint64_t pitch,uint64_t stride) noexcept {
    const auto barrier=reinterpret_cast<Step5BarrierFn>(step1Native(self,26));
    const auto copy=reinterpret_cast<Step5CopyTexFn>(step1Native(self,16));
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Flags=D3D12_RESOURCE_BARRIER_FLAG_NONE;b.Transition.Subresource=0;
    D3D12_TEXTURE_COPY_LOCATION dst{},from{};
    from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=0;from.pResource=scene;
    dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Offset=uint64_t(gpu_copy::png::image_scene_end)*stride;
    dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT(gpu_copy::png::fp16_format);dst.PlacedFootprint.Footprint.Width=w;
    dst.PlacedFootprint.Footprint.Height=h;dst.PlacedFootprint.Footprint.Depth=1;dst.PlacedFootprint.Footprint.RowPitch=UINT(pitch);
    const D3D12_BOX box{0,0,0,w,h,1};
    b.Transition.pResource=scene;b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier(self,1,&b);
    copy(self,&dst,0,0,0,&from,&box);
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier(self,1,&b);
}
constexpr unsigned step5SceneEndMaxCopies=3; // per frame (a reopened region copies again over the same footprint)
// J-scene-end (OM hook after the native call / barrier hook BEFORE the native call; NO hook-state lock held; only while g_step5SceneEndWanted): when THIS call
// ended the frame's HUD region (`why` = "rt-change" | "scene-barrier"), a file-triggered burst set is pending and the machinery is armed, the scene target
// (still RENDER_TARGET) is copied into the readback's 5th footprint inside the game's recording: that is the content of the scene target after ALL scene
// draws recorded before this point (cockpit members and the between-members effects included). Lock order: region leaf mutex alone, then the step-1 mutex.
void step5SceneEnd(ID3D12GraphicsCommandList* self,const char* why) noexcept {
    if(!g_step5SceneEndWanted.load(std::memory_order_relaxed)||!pngOnceConfigured())return;
    auto* st=g_step1.load();auto* s5p=g_step5.load();if(!st||!s5p)return;
    auto& s5=*s5p;
    try {
        void* key=nullptr;{auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);}
        const Step1Route route=step1Route(key);
        uint64_t generation=0;uintptr_t scene=0;
        {std::lock_guard lock(g_hudRegionMutex);auto& r=g_hudRegion;
         // J-hud-radar: the first hooked excursion (rt-change) counts as the region end (the region itself stays open); one copy per region lifetime.
         const bool exc=r.radar&&r.excWhy&&!r.excCopy&&std::strcmp(r.excWhy,why)==0;
         const bool end=r.ended&&r.why&&std::strcmp(r.why,why)==0;
         if(!r.active||r.sceneEndTaken||r.list!=route||!(end||exc))return;
         if(end&&std::strcmp(why,"scene-barrier")==0&&r.endBefore!=uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET))return; // state not provably RENDER_TARGET
         r.sceneEndTaken=true;if(exc&&!end)r.excCopy=true;generation=r.generation;scene=r.scene;}
        ID3D12Resource* rb=nullptr;uint32_t w=0,h=0;uint64_t pitch=0,stride=0;
        {std::lock_guard lock(st->mutex);auto& f=s5.frame;auto& p=s5.png;
         namespace png=gpu_copy::png;
         if(p.stage.load()!=png_armed||!p.burst||!p.fxCapable||p.refusal||!p.readback||p.burstRead>=png::burst_samples)return;
         if(p.fileTrig.load()==p.fileSeen)return;                         // only a pending file request takes the scene-end copy
         if(s5.disabled||!f.active||f.closed||f.submitted||f.list!=route||f.generation!=generation||!f.worldValid)return;
         if(!scene||scene!=uintptr_t(s5.worldSource)||!s5.worldReady)return;
         if(p.seList!=f.list||p.seGen!=f.generation){p.seList=f.list;p.seGen=f.generation;p.seCopies=0;}
         if(p.seCopies>=step5SceneEndMaxCopies)return;
         ++p.seCopies;p.seWhy=why;rb=p.readback;w=p.w;h=p.h;pitch=p.pitch;stride=p.stride;}
        step5SceneEndCopy(self,reinterpret_cast<ID3D12Resource*>(scene),rb,w,h,pitch,stride);
    } catch(...) {}
}
void step5PngClose(ID3D12GraphicsCommandList* self) noexcept {
    if(!pngOnceConfigured())return;
    auto* st=g_step1.load();auto* s5p=g_step5.load();if(!st||!s5p)return;
    auto& s5=*s5p;
    try {
        void* key=nullptr;{auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);}
        ID3D12Resource* src[gpu_copy::png::image_count_fx]{};bool record=false;unsigned images=gpu_copy::png::image_count;
        {std::lock_guard lock(st->mutex);auto& f=s5.frame;auto& p=s5.png;
         if(p.stage.load()!=png_armed||s5.disabled||!f.active||f.closed||f.submitted||step1Route(key)!=f.list)return;
         // burst protocol v2: a pending png-burst.now request (a change of the file seen by the worker poll) takes the next valid frame,
         // without the 30-frame gap and without the effects-draw threshold; burst sets are shared with the threshold trigger (<= 3 per process).
         const bool fileReq=p.fxCapable&&p.burst&&p.burstRead<gpu_copy::png::burst_samples&&p.fileTrig.load()!=p.fileSeen;
         if(s5.published.load()<p.notBefore&&!fileReq)return;
         const char* wait=nullptr;const auto& sl=s5.slot[f.slot];
         if(!f.worldValid)wait="world-invalid";
         else if(!gpu_copy::step5::hud_frame_valid(f.hud.armed,f.hud.draws,f.hud.refused))wait="hud-invalid";
         else if(f.poseBroken)wait="pose-broken";
         else if(!sl.hudRt||!sl.world||!sl.rt[0])wait="slot-image-missing";
         if(wait){++p.waitedFrames;p.lastWait=wait;return;}
         // J-effects-content: the effects target holds this frame's content only when the frame cleared it (the first speculative replay clears
         // it); the committed draws (between members) are what the burst trigger counts. A burst sample waits for the threshold (not a "wait").
         const bool fxFresh=p.fxCapable&&f.eff.armed&&f.eff.cleared&&sl.effRt;
         const bool thrHit=fxFresh&&f.eff.ep.draws>=p.thr;
         if(p.burst&&!thrHit&&!fileReq){++p.burstBelow;return;}
         // J-scene-end: a file request waits (at most 2 frames in a row) for a frame that holds a scene-end copy (the request may have arrived after
         // this frame's HUD region ended); a copy exists only for this very list generation.
         const bool seHave=p.seList==f.list&&p.seGen==f.generation&&p.seCopies>0;
         if(p.burst&&fileReq&&p.fxCapable&&!seHave&&p.seDeclinedRun<2){++p.seDeclinedRun;++p.seDeclined;g_step5SceneEndWanted.store(true);return;}
         p.se=p.burst&&fileReq&&seHave;p.seDeclinedRun=0;
         p.burstFile=p.burst&&fileReq; // the file request has priority when both hold; it consumes every request seen so far
         if(p.burstFile){p.fileSeen=p.fileTrig.load();++p.fileBursts;p.reqTickAtCapture=p.fileReqTick.load();}
         p.captureTick=GetTickCount64();p.capturePublished=s5.published.load();p.phHave=false;
         src[gpu_copy::png::image_hud]=sl.hudRt;src[gpu_copy::png::image_world]=sl.world;src[gpu_copy::png::image_eye0]=sl.rt[0];
         p.fx=fxFresh;p.fxDraws=fxFresh?f.eff.ep.draws:0u;
         if(p.fx){src[gpu_copy::png::image_effects]=sl.effRt;images=gpu_copy::png::image_count_fx;}
         for(unsigned i=0;i<gpu_copy::png::image_count_fx;++i)p.src[i]=src[i];
         p.list=f.list;p.generation=f.generation;p.slot=f.slot;p.hudDraws=f.hud.draws;p.members=f.recorded;p.reexecuted=false;
         p.stage.store(png_recorded);g_step5PngWanted.store(false);record=true;
         if(p.fileTrig.load()==p.fileSeen)g_step5SceneEndWanted.store(false);}
        if(record)step5PngRecord(self,src,images,s5.png.readback,s5.png.w,s5.png.h,s5.png.pitch,s5.png.stride);
    } catch(...) {}
}
// J-effects-layer: per published frame (step-1 mutex held; the epochs were finished at the list's Close).
void step5EffectsAccount(Step5State& s5,const Step5Frame& f) noexcept {
    using namespace gpu_copy::step5;const auto& e=f.eff;
    if(!e.armed){++s5.effUnarmedFrames;return;}
    step5FxSmokeAccount(s5,f); // EFFECTS-OUTSIDE (C) fxmem:1 (flag off: fxOpen is never set, returns)
    step5FxCensusAccount(s5); // EFFECTS-OUTSIDE (D) fxmem:3 (no frame open: returns)
    step5FxSpriteAccount(s5,f); // EFFECTS-OUTSIDE (E) fxmem:5 (+ (F) fxmem:6 GPU facts; closed: returns)
    const auto& p=e.ep;
    const bool valid=effects_frame_valid(true,p.draws)&&s5.slot[f.slot].effRt;
    if(valid)++s5.effValidFrames;else if(p.tailDraws||p.refused||p.tailRefused)++s5.effInvalidFrames;else ++s5.effAbsentFrames;
    s5.effDrawSum+=p.draws;s5.effTailSum+=p.tailDraws;s5.effRefusedSum+=p.refused;s5.effConstBlend+=p.constBlend;
    for(int i=0;i<hud_categories;++i)s5.effReason[i]+=p.reason[i];
    s5.effIgnored+=e.ignored;s5.effCommits+=p.commits;s5.effSpeculative+=p.speculative;
    if(p.used>=effects_epoch_cap)++s5.effEpochCapFrames;
    if(valid&&!s5.effFirstEmitted&&!s5.effFirstPending){s5.effFirstPending=true;s5.effFirstFrame=s5.nextFrameId+1;s5.effFirst=e;}
    // J-effects-smoke-fix: the per-replay diagnostics of the first 3 effects-valid frames and of every frame a burst png sample was recorded into.
    {
        const auto& pn=s5.png;
        const bool burst=pngOnceConfigured()&&pn.stage.load()==png_recorded&&pn.list==f.list&&pn.generation==f.generation&&pn.burst;
        const bool early=valid&&s5.effDiagValidSeen<3;
        if(valid)++s5.effDiagValidSeen;
        if((early||burst)&&e.diagN) {
            for(unsigned i=0;i<e.diagN;++i) {
                if(s5.effDiagCount>=Step5State::effDiagCap){++s5.effDiagDropped;continue;}
                auto& q=s5.effDiagQ[(s5.effDiagHead+s5.effDiagCount)%Step5State::effDiagCap];
                q.frame=s5.nextFrameId+1;q.burst=burst;q.mode=unsigned(s5.effPredMode);q.d=e.diag[i];++s5.effDiagCount;++s5.effDiagLinesTotal;
            }
        }
    }
    if(p.refused&&!s5.effRefusalEmitted&&!s5.effRefusalPending){s5.effRefusalPending=true;s5.effRefusal=e;}
}
// J-hud-radar: per published armed frame (step-1 mutex held). Sums, and the pending hud-end diagnostic (the first 3 frames with a region-ending
// event, then at most one every 5 s).
void step5RadarAccount(Step5State& s5,const Step5Frame& f) noexcept {
    const auto& h=f.hud;const auto& r=h.rd;
    ++s5.radarFrames;s5.radarExcFrames+=r.excursions>0;s5.radarEqualFrames+=r.equalReplayed>0;s5.radarExcursions+=r.excursions;s5.radarReturns+=r.returns;
    s5.radarReplace+=r.replaceReplayed;s5.radarEqual+=r.equalReplayed;s5.radarEqualIndices+=r.equalIndices;s5.radarLate+=r.lateReplayed;s5.radarWindows+=r.windows;
    s5.radarA3050+=r.a3050;s5.radarNotEqual+=r.notEqualSkipped;s5.radarLateSkipped+=r.lateSkipped;s5.radarAwaySkipped+=r.awaySkipped;
    s5.radarExcWriters+=r.excursionWriters;s5.radarGaps+=r.stencilGaps;s5.radarEqualRefused+=r.equalRefused;s5.radarGapRefused+=r.gapRefused;
    s5.radarSceneLeaves+=r.sceneLeaves;s5.radarSceneResumes+=r.sceneResumes;s5.radarSceneBackRefused+=r.sceneBackRefused;s5.radarResumedFrames+=r.sceneResumes>0;
    const uint64_t group=uint64_t(r.replaceReplayed)+r.equalReplayed+r.lateReplayed;
    s5.radarGroupSum+=group;if(r.equalReplayed>s5.radarMaxEqual)s5.radarMaxEqual=r.equalReplayed;if(group>s5.radarMaxGroup)s5.radarMaxGroup=group;
    if(gpu_copy::step5::hud_late_flag().load(std::memory_order_relaxed)) { // HUD-LATE (hudlate:1)
        s5.hudLate.add(h.late);
        if(s5.hudLateOnset.step(h.late.draws,GetTickCount64())) { // SUBTITLE SHOT: one eye dump request at a subtitle onset (consumer: xr_png_once re-arm)
            gpu_copy::step5::hud_late_eye_request().store(true,std::memory_order_release);
            char b[160];std::snprintf(b,sizeof b,"hud-late-onset;frame=%llu;text-draws=%u;shot=%u;request=eye-dump(needs-xr_png_once)",
                static_cast<unsigned long long>(s5.nextFrameId+1),h.late.draws,s5.hudLateOnset.shots);
            (void)s5.diagQ.push_text(b);
        }
    }
    const char* end=h.endWhy?h.endWhy:"close";
    if((h.dg.haveEnd||std::strcmp(end,"close")!=0)&&!s5.radarDiagPending) {
        const uint64_t now=GetTickCount64();
        if(s5.radarDiagEmitted<3||now-s5.radarDiagLastTick>=5000) {
            s5.radarDiagPending=true;s5.radarDiagFrame=s5.nextFrameId+1;s5.radarDiag=h.dg;s5.radarDiagRd=h.rd;s5.radarDiagEnd=end;
            ++s5.radarDiagEmitted;s5.radarDiagLastTick=now;
        }
    }
}
// J-hud-capture: per published frame (step-1 mutex held).
void step5HudAccount(Step5State& s5,const Step5Frame& f) noexcept {
    using namespace gpu_copy::step5;const auto& h=f.hud;
    if(!h.armed){++s5.hudUnarmedFrames;s5.hudLastUnarmed=h.armWhy;return;}
    const bool valid=hud_frame_valid(h.armed,h.draws,h.refused)&&s5.slot[f.slot].hudRt;
    if(valid)++s5.hudValidFrames;else ++s5.hudInvalidFrames;
    s5.hudDrawSum+=h.draws;s5.hudRefusedSum+=h.refused;for(int i=0;i<hud_categories;++i)s5.hudReason[i]+=h.reason[i];
    s5.hudResets+=h.resets;s5.hudCapFrames+=h.capped;s5.hudDepthTested+=h.depthTested;
    s5.hudRsSkipSum+=h.rsSkipped;s5.hudRsSkipFrames+=h.rsSkipped>0;s5.hudRsDswSum+=h.rsDsw; // HUD-RS-SKIP
    const char* end=h.endWhy?h.endWhy:"close";
    ++s5.hudEnd[std::strcmp(end,"scene-barrier")==0?1:std::strcmp(end,"close")==0?2:0];
    if(valid&&!s5.hudFirstEmitted&&!s5.hudFirstPending){s5.hudFirstPending=true;s5.hudFirstFrame=s5.nextFrameId+1;s5.hudFirst=h;s5.hudFirstEnd=end;}
    if(h.refused&&!s5.hudRefusalEmitted&&!s5.hudRefusalPending){s5.hudRefusalPending=true;s5.hudRefusal=h;}
    if(h.radar)step5RadarAccount(s5,f);
    if(!h.mSplit)return; // J-hud-markers (key off / second target unavailable: nothing below runs)
    const bool mvalid=h.mDraws>0&&h.mRefused==0&&s5.slot[f.slot].hudMarkerRt;
    if(mvalid)++s5.hudMarkerValidFrames;else if(h.mDraws||h.mRefused)++s5.hudMarkerInvalidFrames;else ++s5.hudMarkerAbsentFrames;
    s5.hudMarkerDrawSum+=h.mDraws;s5.hudMarkerBoxSum+=h.mBoxes;s5.hudMarkerLabelSum+=h.mLabels;s5.hudMarkerRefusedSum+=h.mRefused;
    for(int i=0;i<hud_categories;++i)s5.hudMarkerReason[i]+=h.mReason[i];
    if(mvalid&&!s5.hudMarkerFirstEmitted&&!s5.hudMarkerFirstPending){s5.hudMarkerFirstPending=true;s5.hudMarkerFirstFrame=s5.nextFrameId+1;s5.hudMarkerFirst=h;}
    if(h.mRefused&&!s5.hudMarkerRefusalEmitted&&!s5.hudMarkerRefusalPending){s5.hudMarkerRefusalPending=true;s5.hudMarkerRefusal=h;}
}
struct Step5Submit {bool signal=false;uint64_t value=0;bool frame=false;}; // frame: the Signal belongs to the active frame's submission (else: re-executed owned recordings only)
// DECOUPLE review 3 (Astra open risk 2): an Execute forwarded to the native call WITHOUT step5BeforeExecute examining it (the phase is not
// continuous: reselection, inserted/ready/done) may re-execute an owned recording. Called before the native Execute; fails every frame submitted
// before it closed (never acquired again, never reported unchanged), independent of any diagnostic observation succeeding.
void step5ExecuteUnexamined() noexcept {if(auto* s5=g_step5.load())s5->untrackedExec.fetch_add(1);}
void step5BeforeExecute(ID3D12CommandQueue* self,UINT count,ID3D12CommandList* const* lists,Step5Submit& out) noexcept {
    auto* st=g_step1.load();auto* s5=g_step5.load();if(!s5)return;
    if(!st||!lists){s5->untrackedExec.fetch_add(1);return;}   // review 3: forwarded unexamined -> fail closed
    if(worldObserveConfigured())step5WorldObserveExecute(self,count,lists); // J-world-objects v2 step 1 (read-only; default OFF)
    std::array<Step1Route,gpu_copy::owner::Timeline::batch_cap> routes{};
    const unsigned n=count<routes.size()?count:static_cast<unsigned>(routes.size());
    // Diagnostic carried state only: never a recording prerequisite. Correlate
    // watch lifetime and queue, and process EVERY execution (including closed-list
    // re-execution). Unknown/foreign/oversize histories stay unknown.
    {auto& s=state();std::lock_guard lock(s.mutex);
     const auto watch=g_step5WorldWatch.load();const uint64_t serial=step5WatchSerial_locked();
     if(self!=st->queue||!serial||s.step5RtResource!=watch||s.step5RtSerial!=serial||s.step5RtQueue!=self)
         s.step5RtCarriedKnown=false;
     s.step5RtResource=watch;s.step5RtSerial=serial;s.step5RtQueue=self;
     for(unsigned i=0;i<n;++i) {
         void* key=lists[i]?listKey_locked(lists[i]):nullptr;routes[i]=key?step1Route(key):0;
         auto it=key?s.states.find(static_cast<ID3D12GraphicsCommandList*>(key)):s.states.end();
         if(it==s.states.end()){s.step5RtCarriedKnown=false;continue;}
         auto& t=it->second.step3;
         if(t.rtPendingCounted){t.rtPendingCounted=false;if(s.step5RtPending>0)--s.step5RtPending;}
         const auto* recording=l3_list_locked(it->second);
         if(self!=st->queue||!serial||!recording||!recording->generation||!step3BarriersHooked_locked(it->second)||t.enhanced||t.rtBad){s.step5RtCarriedKnown=false;continue;}
         if(t.rtTransition) {
             if(t.rtResource==watch&&t.rtSerial==serial&&t.rtWatchEpoch==s.step5WatchEpoch&&t.rtGeneration==recording->generation) {
                 s.step5RtCarriedKnown=true;s.step5RtCarried=t.rtAfter;
             } else s.step5RtCarriedKnown=false;
         }
     }
     if(count>routes.size())s.step5RtCarriedKnown=false;}
    std::lock_guard lock(st->mutex);auto& f=s5->frame;
    if(s5->disabled){s5->untrackedExec.fetch_add(1);return;}   // DECOUPLE review 2: re-executions no longer examined -> no held image is provably unchanged
    unsigned occ=0;
    for(unsigned i=0;i<n;++i){if(f.active&&routes[i]==f.list)++occ;}
    if(count>routes.size()&&(f.active||step5OwnerLive_locked(*s5))){s5->untrackedExec.fetch_add(1);if(f.active)step5Fault(*st,*s5,"oversize-batch",true);return;}
    // J-180 P1: EVERY Execute occurrence of an owned recording that is not the active frame's submission is a re-execution of an injected writer
    // (A -> B -> A included): correlated on the owner entry and the private fence, the slot never reusable while the recording is alive.
    uint64_t reexecValue=0;
    for(unsigned i=0;i<n;++i) {
        if(f.active&&routes[i]==f.list)continue;
        auto* o=step5OwnerFind_locked(*s5,routes[i]);if(!o)continue;
        if(!reexecValue)reexecValue=++s5->fenceCounter;
        ++o->executes;++s5->ownerReexec;o->lastUse=reexecValue;o->epoch=s5->fenceEpoch.load();
        auto& sl=s5->slot[o->slot];sl.lastUse=reexecValue;sl.lastUseEpoch=s5->fenceEpoch.load();
        const int state=sl.state.load();
        if(state==gpu_copy::step5::slot_held)++s5->ownerHeld;
        // The published / held / in-production images are about to be rendered again: never published again, never reused (a consumer that
        // already acquired the slot cannot be revoked; its release then finds a quarantined slot). A FREE slot (frame never published) stays
        // pinned by the owner entry and is simply re-timestamped above.
        if(state!=gpu_copy::step5::slot_free&&state!=gpu_copy::step5::slot_quarantined)step5SetSlot(*s5,o->slot,gpu_copy::step5::slot_quarantined);
        ++s5->ambiguous;++s5->failures;s5->lastFault="re-executed-recording";
        // J-hud-colour: that recording's copies run again (same images; reported, not hidden).
        if(s5->png.list==o->route&&s5->png.generation==o->generation){const int g=s5->png.stage.load();
            if(g==png_submitted)s5->png.reexecuted=true;}
        if(s5->failures>=gpu_copy::step5::fail_cap){s5->disabled=true;s5->disableReason="3-consecutive-ambiguous-frames";}
    }
    if(reexecValue){out.signal=true;out.value=reexecValue;out.frame=false;} // one Signal after the batch covers every re-executed list in it
    if(!f.active||!occ)return;
    if(!f.closed){step5Fault(*st,*s5,"unclosed-generation",true);return;}
    if(f.submitted){step5Fault(*st,*s5,"re-executed-before-publication",true);return;}
    if(self!=st->queue){step5Fault(*st,*s5,"cross-queue-execute",true);return;}
    if(occ>1){step5Fault(*st,*s5,"duplicate-in-batch",true);return;}
    if(!f.closeOk){step5Fault(*st,*s5,"close-failed",true);return;}
    f.submitted=true;out.signal=true;out.frame=true;out.value=++s5->fenceCounter;
    f.untrackedBase=s5->untrackedExec.load(); // review 3: the publication's proof starts at the write that produces its images, not at acquisition
    s5->slot[f.slot].lastUse=out.value;s5->slot[f.slot].lastUseEpoch=s5->fenceEpoch.load(); // J-3b9: reuse waits for completed >= this value on THIS epoch
    // J-180 P1: from this submission on the recording owns its slot until Reset / destruction. The table has room by construction (frame start
    // refuses when it is full), so a failed add is an invariant violation: quarantined, never silently dropped.
    if(!step5OwnerAdd_locked(*s5,f.list,f.generation,f.slot,out.value))step5SetSlot(*s5,f.slot,gpu_copy::step5::slot_quarantined);
}
void step5AfterExecute(const Step5Submit& in) noexcept {
    auto* st=g_step1.load();auto* s5=g_step5.load();if(!st||!s5||!in.signal)return;
    const bool ok=st->fence.signal(step1Route(st->queue),step1Route(st->fence.fence),in.value); // exactly one native Signal, no lock held
    std::lock_guard lock(st->mutex);auto& f=s5->frame;
    if(!in.frame) {
        // J-180 P1: only owned recordings were re-executed in this batch (the active frame, if any, was not submitted by it). The value correlates
        // their occurrence with the private fence; a failed Signal leaves their slots pinned by the owner entries (never reusable) and counts a failure.
        if(!ok){++s5->failures;s5->lastFault="signal-failed";}
        else if(in.value>s5->lastSignalled.load())s5->lastSignalled.store(in.value);
        return;
    }
    if(!ok){step5Fault(*st,*s5,"signal-failed",true);return;}
    if(in.value>s5->lastSignalled.load())s5->lastSignalled.store(in.value); // the native Execute and Signal both returned
    // (M6) fxatlas: the recording's first captures become drawable: the native Execute returned, so every recording that reads them is queued after it
    // (a recording started before this point saw them pending and does not read them)
    if(f.active&&f.submitted)s5->fxTrail.atlas.slot_submitted(f.slot);
    // J-hud-colour: the sample's copies complete with this private-fence value (worker observes it).
    if(s5->png.stage.load()==png_recorded&&s5->png.list==f.list&&s5->png.generation==f.generation) {
        auto& p=s5->png;p.value=in.value;p.epoch=s5->fenceEpoch.load();p.publishedAt=s5->published.load()+1;p.stage.store(png_submitted);
    }
    if(f.early&&!f.adopted&&!f.poseBroken&&!f.noCockpit) {   // POV-RENDER stage 2: a no-cockpit chase frame is published below
        // J-world-objects v2: opened by a world draw, never adopted by a cockpit member of its list: executes, never published.
        // Consecutive abandoned frames back the early start off (a list with world draws but no cockpit draws would starve the producer).
        ++s5->wobjEarlyAbandoned;
        if(++s5->wobjEarlyStreak>=3){s5->wobjEarlyHold=120;s5->wobjEarlyOff="early-never-adopted";s5->wobjEarlyStreak=0;}
        s5->lastList=f.list;s5->lastGen=f.generation;s5->lastSlot=f.slot;s5->lastLive=true;
        step5SetSlot(*s5,f.slot,gpu_copy::step5::slot_free);f.active=false;return;
    }
    if(f.poseBroken) {
        // J-3b9: a member was rendered without the claimed pose: never published. The
        // recording executes; the slot returns to the ring, reusable once the fence completes.
        ++s5->poseDropped;s5->skipped.fetch_add(1);s5->lastSkipWhy=f.early?"world-early-adopt-mismatch":"member-pose-substitution-failed";
        s5->lastList=f.list;s5->lastGen=f.generation;s5->lastSlot=f.slot;s5->lastLive=true;
        step5SetSlot(*s5,f.slot,gpu_copy::step5::slot_free);f.active=false;return;
    }
    if(f.early)s5->wobjEarlyStreak=0;
    ++s5->frames;s5->commandSum+=f.commands;s5->memberSum+=f.recorded;if(f.poseValid)++s5->poseFrames;if(f.lensValid)++s5->lensFrames;
    if(f.partial)++s5->partialFrames;
    if(f.worldIncomplete)++s5->wobjIncompleteFrames; // J-180 P2
    if(f.wobjRecorded) { // J-world-objects (only ever non-zero with the key)
        ++s5->wobjFrames;s5->wobjRecordedSum+=f.wobjRecorded;
        if(!s5->wobjFirstEmitted&&!s5->wobjFirstPending){s5->wobjFirstPending=true;s5->wobjFirstFrame=s5->nextFrameId+1;s5->wobjFirstMembers=f.wobjRecorded;}
    }
    const unsigned selected=st->s3.memberCount;
    if(f.recorded>selected)++s5->extraMembers;else if(f.recorded<selected)++s5->fewerMembers;
    if(f.byClass) {
        // F3: the member count follows the frame's own class members; only refusals are mismatches.
        ++s5->classFrames;if(f.partial)++s5->classPartial;if(f.capped)++s5->classCapFrames;
        if(f.diff||f.partial){++s5->mismatchProduced;step5OfferDetail(*s5,f.detail,true,f.recorded);}
    } else {
    if(f.recorded!=selected&&!f.detail.set){f.detail.set=true;f.detail.what="member-count";f.detail.selected=selected;f.detail.list=st->s3.live.list;
        f.detail.gen=f.generation;}
    if(f.diff||f.recorded!=selected||f.partial){++s5->mismatchProduced;step5OfferDetail(*s5,f.detail,true,f.recorded);}
    }
    if(hmdFovConfigured()&&f.poseValid) {
        // F1: per published posed frame, the HMD FOV or the game-lens fallback (with its reason).
        if(f.fovHmd) {
            ++s5->fovFrames;if(f.fovClamped)++s5->fovClampedFrames;
            if(!s5->fovFirstEmitted&&!s5->fovFirstPending){s5->fovFirstPending=true;s5->fovFirstFrame=s5->nextFrameId+1;s5->fovFirstWhy=f.fovWhy;
                for(int e=0;e<2;++e)for(int i=0;i<4;++i)s5->fovFirst[e][i]=f.fov[e][i];for(int i=0;i<4;++i)s5->fovFirstLens[i]=f.lens[i];}
        } else {
            ++s5->fovFallback;s5->fovLastWhy=f.fovWhy;
            if(!s5->fovFallbackEmitted&&!s5->fovFallbackPending)s5->fovFallbackPending=true;
        }
    }
    if(f.worldValid){++s5->worldFrames;if(!s5->worldFirstEmitted&&!s5->worldFirstPending){s5->worldFirstPending=true;
        s5->worldFirstFrame=s5->nextFrameId+1;s5->worldFirstResource=s5->worldSource;s5->worldFirstEvidence=f.worldEvidence;}}
    else {++s5->worldRefused;s5->worldLastWhy=f.worldWhy?f.worldWhy:"world-texture-missing";
        if(!s5->worldRefusalEmitted&&!s5->worldRefusalPending){s5->worldRefusalPending=true;s5->worldFirstRefusal=s5->worldLastWhy;}}
    if(hudReplayConfigured())step5HudAccount(*s5,f);
    if(effectsConfigured())step5EffectsAccount(*s5,f); // J-effects-layer (key off: not called)
    s5->failures=0;s5->consecutiveMismatch=0;
    s5->lastList=f.list;s5->lastGen=f.generation;s5->lastSlot=f.slot;s5->lastLive=true;
    if(!s5->firstFrame){s5->firstFrame=s5->nextFrameId+1;s5->firstValue=in.value;s5->firstMembers=f.recorded;s5->firstCommands=f.commands;
        s5->firstPose=f.poseValid;s5->firstFovHmd=f.poseValid&&f.fovHmd;s5->firstWorld=f.worldValid;s5->firstLens=f.lensValid;for(int i=0;i<4;++i)s5->firstLensTan[i]=f.lens[i];}
    step5TacClose(*s5,f); // TACTICAL: the frame's facts into the slot (key off: marks the slot's facts invalid)
    if(hudReplayConfigured())step5MslClose(*s5); // MSL-HUD (off when mslhud:0, stores:0 or no cockpit option: returns)
    if(s5->meta.try_lock()){step5Publish_locked(*s5,f,in.value);s5->meta.unlock();}
    else {s5->pending=true;s5->pendingFrame=f;s5->pendingValue=in.value;}
    f.active=false;
}
// ------------------------------------------------------------- owner context
// The selection's game RT as the world source (owner context / worker). nullptr or why.
const char* step5WorldSourceWhy(const Step1State& st) noexcept {
    const auto& g=st.s3.shape;const auto& t=st.s3.targets;
    if(std::strcmp(g.game_rt_status,"resolved")!=0)return "selection-game-rt-unresolved";
    if(g.game_rt_width!=t.width||g.game_rt_height!=t.height)return "game-rt-size-differs-from-viewport";
    if(g.game_rt_format!=g.pso.rtv[0])return "game-rt-format-differs-from-pso";
    if(g.game_rt_dim!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||g.game_rt_samples!=1)return "game-rt-not-single-sample-texture2d";
    if(g.game_rt_flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)return "game-rt-simultaneous-access";
    if(!g.game_rt_view_sub0)return "game-rtv-not-subresource-0";
    if(g.game_rt_heap!=D3D12_HEAP_TYPE_DEFAULT)return "game-rt-not-default-heap";
    return nullptr;
}
// Typeless family of a depth format (the resource format; DSV/SRV views are typed).
uint32_t step5TypelessDepth(uint32_t dsv) noexcept {
    switch(dsv) {
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:return DXGI_FORMAT_R32G8X24_TYPELESS;  // 20 -> 19
    case DXGI_FORMAT_D32_FLOAT:return DXGI_FORMAT_R32_TYPELESS;                 // 40 -> 39
    case DXGI_FORMAT_D24_UNORM_S8_UINT:return DXGI_FORMAT_R24G8_TYPELESS;       // 45 -> 44
    case DXGI_FORMAT_D16_UNORM:return DXGI_FORMAT_R16_TYPELESS;                 // 55 -> 53
    default:return dsv;
    }
}
void step5CreateHud(Step1State& st,Step5State& s5) noexcept; // J-hud-capture (below)
void step5CreateHudMarkers(Step1State& st,Step5State& s5) noexcept; // J-hud-markers (below)
void step5CreateEffects(Step1State& st,Step5State& s5) noexcept;    // J-effects-layer (below)
void step5CreateSmokeGpu(Step1State& st,Step5State& s5,bool late) noexcept;   // EFFECTS-OUTSIDE (C) fxmem:4 (below)
void step5CreateSpriteGpu(Step1State& st,Step5State& s5,bool late) noexcept;  // EFFECTS-OUTSIDE (F) fxmem:6 (below)
void step5CreatePng(Step1State& st,Step5State& s5) noexcept; // J-hud-colour (below)
const char* step5PngStageName(int g) noexcept;               // J-hud-colour (below)
// The ring: per slot and eye one RT (the PSO format, viewport size, RENDER_TARGET)
// and one DS (DEPTH_WRITE), CPU-only RTV/DSV heaps; one world texture per slot
// (the game RT's size/format, RENDER_TARGET) when the selection's game RT
// qualifies; one persistently mapped UPLOAD buffer with 64 KiB per slot
// (2 x 512 B per member). Kept for the process lifetime.
const char* step5CreateRing(Step1State& st,Step5State& s5) noexcept {
    using namespace gpu_copy::step3;
    auto& s3=st.s3;const auto& t=s3.targets;
    D3D12_CLEAR_VALUE colour{};colour.Format=DXGI_FORMAT(s3.shape.pso.rtv[0]);
    D3D12_CLEAR_VALUE depth{};depth.Format=DXGI_FORMAT(s3.shape.pso.dsv);depth.DepthStencil.Depth=t.depth_clear;
    using RtvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    using DsvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_DEPTH_STENCIL_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    HRESULT hr=E_PENDING;
    for(unsigned i=0;i<gpu_copy::step5::slots;++i)for(unsigned e=0;e<2;++e) {
        auto& sl=s5.slot[i];
        // EYE-RING: ringW x ringH (the eye size on the eye ring; else exactly the targets' size, as before)
        if(!step3CreateTextureWH(st,s5.ringW,s5.ringH,s3.shape.pso.rtv[0],D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,colour,sl.rt[e],hr))
            {s5.createHr=hr;return "ring-rt-create-failed";}
        if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,sl.rtvHeap[e],hr,sl.rtv[e]))return "ring-rtv-heap-failed";
        reinterpret_cast<RtvFn>(step1Native(st.device,20))(st.device,sl.rt[e],nullptr,sl.rtv[e]);
        if(t.has_depth) {
            // J-3b9 (consumer rev 3): the depth resource is TYPELESS (e.g. R32G8X24_TYPELESS) with
            // ALLOW_DEPTH_STENCIL only, so the consumer can read plane 0 through an SRV
            // (R32_FLOAT_X8X24_TYPELESS); the replays use a typed DSV of the PSO's DSVFormat.
            if(!step3CreateTextureWH(st,s5.ringW,s5.ringH,s5.ringDsTypeless,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE,depth,sl.ds[e],hr))
                {s5.createHr=hr;return "ring-ds-create-failed";}
            if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_DSV,sl.dsvHeap[e],hr,sl.dsv[e]))return "ring-dsv-heap-failed";
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT(s3.shape.pso.dsv);dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
            dv.Flags=D3D12_DSV_FLAG_NONE;dv.Texture2D.MipSlice=0;
            reinterpret_cast<DsvFn>(step1Native(st.device,21))(st.device,sl.ds[e],s5.ringDsTypeless!=s3.shape.pso.dsv?&dv:nullptr,sl.dsv[e]);
        }
        sl.uploadOffset=uint64_t(i)*s5.slotUpload; // 64 KiB (F3 on: 128 KiB) per slot
    }
    HRESULT uhr=E_PENDING;
    if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,s5.upload,uhr,
        s5.slotUpload*gpu_copy::step5::slots))return "ring-upload-create-failed";
    using VaFn=D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*);
    s5.uploadVa=reinterpret_cast<VaFn>(step1Native(s5.upload,11))(s5.upload);
    using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
    const D3D12_RANGE none{0,0};void* p=nullptr;
    if(FAILED(reinterpret_cast<MapFn>(step1Native(s5.upload,8))(s5.upload,0,&none,&p))||!p||!s5.uploadVa)return "ring-upload-map-failed";
    s5.uploadPtr=static_cast<uint8_t*>(p);
    // World textures: optional (a failure leaves world_valid=false, the eyes run).
    if(const char* why=step5WorldSourceWhy(st))s5.worldArmWhy=why;
    else {
        bool ok=true;
        for(unsigned i=0;i<gpu_copy::step5::slots&&ok;++i)
            ok=step3CreateTexture(st,s3.shape.game_rt_format,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,colour,s5.slot[i].world,hr);
        if(!ok)s5.worldArmWhy="world-texture-create-failed";
        else {s5.worldReady=true;s5.worldArmWhy=nullptr;s5.worldSource=s3.shape.game_rt;s5.worldW=s3.shape.game_rt_width;
            s5.worldH=s3.shape.game_rt_height;s5.worldFormat=s3.shape.game_rt_format;}
    }
    if(hudReplayConfigured())step5CreateHud(st,s5);
    if(hudMarkersConfigured())step5CreateHudMarkers(st,s5);
    if(effectsConfigured())step5CreateEffects(st,s5);
    if(effectsConfigured()&&step5FxSmokeGpuOn())step5CreateSmokeGpu(st,s5,false); // EFFECTS-OUTSIDE (C) fxmem:4 (requested when the ring is created)
    if(effectsConfigured()&&step5FxSpriteGpuOn())step5CreateSpriteGpu(st,s5,false); // EFFECTS-OUTSIDE (F) fxmem:6 (requested when the ring is created)
    if(pngOnceConfigured())step5CreatePng(st,s5);
    return nullptr;
}
// J-hud-colour (owner context, only with gpu_copy_step5_png_once): one READBACK buffer of three FP16
// footprints (HUD, world, eye 0; 3 x 29.5 MB at 2560x1440), kept for the process lifetime like the
// ring. Optional: a refusal leaves the producer unchanged (no buffer, no command, one line).
void step5CreatePng(Step1State& st,Step5State& s5) noexcept {
    namespace png=gpu_copy::png;auto& p=s5.png;
    const auto& t=st.s3.targets;
    p.emitArmed=true; // the worker logs the outcome once the dedicated log is open
    if(!s5.hudReady)p.refusal=hudReplayConfigured()?"hud-unavailable":"needs-gpu_copy_step5_hud";
    else if(!s5.worldReady)p.refusal="world-unavailable";
    else if(s5.ringFormat!=png::fp16_format||s5.worldFormat!=png::fp16_format||s5.hudFormat!=png::fp16_format)p.refusal="format-not-fp16";
    else if(s5.eyeRing)p.refusal="eye-ring-sizes-differ(use-eyeobj:0)"; // EYE-RING: the producer dump has one footprint size (the consumer xr_png_once still works)
    else if(s5.worldW!=t.width||s5.worldH!=t.height||s5.ringW!=t.width||s5.ringH!=t.height)p.refusal="image-sizes-differ";
    if(p.refusal)return;
    p.w=t.width;p.h=t.height;p.pitch=png::row_pitch(p.w);p.stride=png::footprint_stride(p.w,p.h);p.bytes=png::readback_bytes(p.w,p.h);
    // J-effects-content: with the effects layer ready (same size/format as the HUD target by construction) the readback gets a 4th footprint and the
    // burst trigger is read once. Effects off / not ready: nothing below runs and the buffer is exactly the 3-image one.
    if(effectsConfigured()&&s5.effReady&&s5.slot[0].effRt){
        p.fxCapable=true;p.bytes=png::readback_bytes_se(p.w,p.h);p.thr=pngThresholdRead(p.thrSource);p.emitBurstArmed=true;
    }
    HRESULT hr=E_PENDING;
    if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,p.readback,hr,p.bytes)||!p.readback)
        {p.createHr=hr;p.refusal="readback-create-failed";return;}
    p.createHr=hr;p.stage.store(png_armed);g_step5PngWanted.store(true);
}
// J-hud-capture (owner context, only with gpu_copy_step5_hud): per slot one HUD RT of the scene
// target's format/size (RENDER_TARGET, cleared 0,0,0,0) and, when the cockpit uses a depth format,
// one depth-stencil of that typed format (DEPTH_WRITE, cleared depth 0 / stencil 0); CPU-only
// RTV/DSV heaps. Optional: a failure leaves hud_valid=false, the eyes/world run. Kept for the
// process lifetime like the ring. Requires the world source (the selection's scene target).
void step5CreateHud(Step1State& st,Step5State& s5) noexcept {
    if(!s5.worldReady){s5.hudArmWhy="scene-target-unavailable";return;}
    using RtvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    using DsvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_DEPTH_STENCIL_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    D3D12_CLEAR_VALUE colour{};colour.Format=DXGI_FORMAT(s5.worldFormat);
    D3D12_CLEAR_VALUE depth{};depth.Format=DXGI_FORMAT(s5.ringDsv);depth.DepthStencil.Depth=0.0f;depth.DepthStencil.Stencil=0;
    gpu_copy::step3::DepthFormat df;const bool hasDs=s5.ringDsv&&gpu_copy::step3::depth_format(s5.ringDsv,df);
    HRESULT hr=E_PENDING;const char* why=nullptr;
    for(unsigned i=0;i<gpu_copy::step5::slots&&!why;++i) {
        auto& sl=s5.slot[i];
        if(!step3CreateTexture(st,s5.worldFormat,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,colour,sl.hudRt,hr))why="hud-rt-create-failed";
        else if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,sl.hudRtvHeap,hr,sl.hudRtv))why="hud-rtv-heap-failed";
        else {
            reinterpret_cast<RtvFn>(step1Native(st.device,20))(st.device,sl.hudRt,nullptr,sl.hudRtv);
            if(!hasDs)continue;
            if(!step3CreateTexture(st,s5.ringDsv,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE,depth,sl.hudDs,hr))why="hud-ds-create-failed";
            else if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_DSV,sl.hudDsvHeap,hr,sl.hudDsv))why="hud-dsv-heap-failed";
            else reinterpret_cast<DsvFn>(step1Native(st.device,21))(st.device,sl.hudDs,nullptr,sl.hudDsv);
        }
    }
    if(why){s5.hudArmWhy=why;s5.createHr=hr;return;} // partial objects are retained (process lifetime)
    s5.hudReady=true;s5.hudArmWhy=nullptr;s5.hudFormat=s5.worldFormat;s5.hudDsFormat=hasDs?s5.ringDsv:0;
    s5.hudDsClear=D3D12_CLEAR_FLAG_DEPTH|(hasDs&&df.stencil?D3D12_CLEAR_FLAG_STENCIL:0);
}
// J-hud-markers (owner context, only with gpu_copy_step5_hud_markers): per slot one more colour target of the HUD target's
// format/size (RENDER_TARGET, cleared 0,0,0,0 by the first marker draw of a frame) and a CPU-only RTV heap; the HUD
// depth-stencil is shared. Charged and capped by hud_markers_budget (checked BEFORE any allocation). Any refusal or failure
// leaves hudMarkersReady=false: the HUD then runs exactly as without the key (marker draws stay in the HUD target).
void step5CreateHudMarkers(Step1State& st,Step5State& s5) noexcept {
    if(!s5.hudReady){s5.hudMarkersArmWhy="hud-unavailable";return;}
    uint64_t charged=0;
    if(const char* why=gpu_copy::step5::hud_markers_budget(s5.hudFormat,s5.worldW,s5.worldH,gpu_copy::step5::slots,charged)){s5.hudMarkersArmWhy=why;return;}
    using RtvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    D3D12_CLEAR_VALUE colour{};colour.Format=DXGI_FORMAT(s5.worldFormat);
    HRESULT hr=E_PENDING;const char* why=nullptr;
    for(unsigned i=0;i<gpu_copy::step5::slots&&!why;++i) {
        auto& sl=s5.slot[i];
        if(!step3CreateTexture(st,s5.worldFormat,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,colour,sl.hudMarkerRt,hr))why="hud-markers-rt-create-failed";
        else if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,sl.hudMarkerRtvHeap,hr,sl.hudMarkerRtv))why="hud-markers-rtv-heap-failed";
        else reinterpret_cast<RtvFn>(step1Native(st.device,20))(st.device,sl.hudMarkerRt,nullptr,sl.hudMarkerRtv);
    }
    if(why){s5.hudMarkersArmWhy=why;s5.createHr=hr;return;} // partial objects are retained (process lifetime), never used
    s5.hudMarkersReady=true;s5.hudMarkersArmWhy=nullptr;s5.hudMarkersCharged=charged;
}
// J-effects-layer (owner context, only with gpu_copy_step5_effects): per slot one more colour target of the HUD target's format/size
// (RENDER_TARGET, cleared 0,0,0,0 by the first candidate replay of a frame) and a CPU-only RTV heap, plus ONE small persistently
// mapped UPLOAD buffer of predicate words (slots x epochs x 8 bytes) that the CPU commits per epoch. The HUD depth-stencil is shared
// (only depth/stencil-READ-ONLY draws are replayed: effects_decide). Charged and capped by effects_budget BEFORE any allocation. Any
// refusal or failure leaves effReady=false: nothing is recorded and the HUD/world run exactly as without the key.
// J-effects-smoke-fix: a small optional file of %LOCALAPPDATA%\acz-vr (fixtures: g_pngFxDirTest). Returns false when absent.
bool step5ReadSmallFile(const wchar_t* name,char* buf,size_t cap,DWORD& got) noexcept {
    got=0;wchar_t path[MAX_PATH]{};
#ifdef ACZ_VR_OFFLINE_TEST
    if(!g_pngFxDirTest.empty())_snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\%s",g_pngFxDirTest.c_str(),name);
    else
#endif
    {wchar_t local[MAX_PATH];const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
     if(!n||n>=MAX_PATH-64)return false;
     _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\%s",local,name);}
    const HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    const BOOL ok=ReadFile(h,buf,DWORD(cap-1),&got,nullptr);CloseHandle(h);
    if(!ok){got=0;return false;}
    buf[got]=0;return true;
}
void step5ReadEffectsConfig(Step5State& s5) noexcept {
    using namespace gpu_copy::step5;
    s5.effAllow=EffectsAllow::defaults();s5.effAllowSource="default";s5.effAllowAdded=s5.effAllowBad=0;
    s5.effPredMode=effects_pred_eq0;s5.effPredSource="default";
    char buf[1024];DWORD got=0;
    if(step5ReadSmallFile(L"effects-ps.txt",buf,sizeof buf,got)) {
        unsigned bad=0;s5.effAllowAdded=parse_effects_ps_text(buf,got,s5.effAllow,bad);s5.effAllowBad=bad;
        s5.effAllowSource=bad?"file-with-invalid-tokens":"file";
    }
    if(step5ReadSmallFile(L"effects-predicate.txt",buf,sizeof buf,got)) {
        const int m=parse_effects_pred_mode(buf,got);
        if(m>=0){s5.effPredMode=m;s5.effPredSource="file";} else s5.effPredSource="file-invalid";
    }
}
void step5CreateEffects(Step1State& st,Step5State& s5) noexcept {
    using namespace gpu_copy::step5;
    step5ReadEffectsConfig(s5);
    if(!s5.hudReady){s5.effArmWhy="hud-unavailable";return;}
    uint64_t charged=0;
    if(const char* why=effects_budget(s5.hudFormat,s5.worldW,s5.worldH,slots,charged)){s5.effArmWhy=why;return;}
    using RtvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    D3D12_CLEAR_VALUE colour{};colour.Format=DXGI_FORMAT(s5.worldFormat);
    HRESULT hr=E_PENDING;const char* why=nullptr;
    for(unsigned i=0;i<slots&&!why;++i) {
        auto& sl=s5.slot[i];
        if(!step3CreateTexture(st,s5.worldFormat,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,colour,sl.effRt,hr))why="effects-rt-create-failed";
        else if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,sl.effRtvHeap,hr,sl.effRtv))why="effects-rtv-heap-failed";
        else reinterpret_cast<RtvFn>(step1Native(st.device,20))(st.device,sl.effRt,nullptr,sl.effRtv);
    }
    if(!why) {
        HRESULT uhr=E_PENDING;
        if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,s5.effPred,uhr,uint64_t(slots)*effects_epoch_cap*effects_predicate_stride)||!s5.effPred)
            {hr=uhr;why="effects-predicate-create-failed";}
        else {
            using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
            const D3D12_RANGE none{0,0};void* p=nullptr;
            if(FAILED(reinterpret_cast<MapFn>(step1Native(s5.effPred,8))(s5.effPred,0,&none,&p))||!p)why="effects-predicate-map-failed";
            else {s5.effPredPtr=static_cast<uint8_t*>(p);std::memset(s5.effPredPtr,0,size_t(uint64_t(slots)*effects_epoch_cap*effects_predicate_stride));}
        }
    }
    if(why){s5.effArmWhy=why;s5.createHr=hr;return;} // partial objects are retained (process lifetime), never used
    s5.effReady=true;s5.effArmWhy=nullptr;s5.effCharged=charged;
}
// EFFECTS-OUTSIDE (C) fxmem:4 (fx_smoke_gpu.h): per slot two colour targets (one per eye) of the effects target's format at the eye size (RENDER_TARGET,
// cleared 0,0,0,0 by the frame's first smoke replay) with CPU-only RTV heaps, and ONE persistently mapped UPLOAD buffer of one 64 KiB span per slot for the
// eye record copies. Needs the effects layer, effects_dual (coverage_v1) and the eye depth (the smoke is depth-tested against the eye objects DSV). Charged and
// capped BEFORE any allocation; created only when fxmem:4 asks for it. Tried ONCE per process (smokeTried): at ring creation when the option is already set
// (owner context, s5 not yet published), else LATE by the worker tick on the first tick after the option arrives (live run R2: the proxy sets the flag at VR
// start, after the ring). Late creation runs with the step-1 mutex held (no Draw seam, arm or publication runs meanwhile), builds every object into locals
// first, stores the pointers into the slots under `meta` and sets smokeReady LAST: no recording can reference them before smokeReady, and the first frame that
// can is the next one armed (f.eff.sm latched at the arm). Kept for the process lifetime like the ring (never released or recreated). Any refusal / failure
// leaves smokeReady=false: every smoke draw then takes the window effects replay as without fxmem:4. `late` only selects the log token.
void step5CreateSmokeGpu(Step1State& st,Step5State& s5,bool late) noexcept {
    namespace g=acz::fxsmoke::gpu;using namespace gpu_copy::step5;
    if(s5.smokeTried)return;
    s5.smokeTried=true;s5.smokeLate=late;s5.smokeRingPending=true;s5.smokeArmWhy=nullptr;
    const auto& t=st.s3.targets;
    gpu_copy::step3::ColorFormat cf;const unsigned bpp=gpu_copy::step3::color_format(s5.hudFormat,cf)?unsigned(cf.bytes):0u;
    uint64_t charged=0;
    if(!s5.effReady)s5.smokeArmWhy="effects-unavailable";
    else if(!effectsDualConfigured())s5.smokeArmWhy="needs-effects-dual";
    else if(!t.has_depth||!s5.ringDsv)s5.smokeArmWhy="eye-depth-missing";
    else if(s5.worldW!=t.width||s5.worldH!=t.height)s5.smokeArmWhy="effects-size-differs-from-eye"; // the window effects image = the game targets (unchanged)
    // EYE-RING: the per-eye smoke images are the eye objects' size (they are depth-tested against the ring DSV): ringW x ringH, eye-size caps on the eye ring.
    else if(const char* why=s5.eyeRing?g::budget(bpp,s5.ringW,s5.ringH,slots,charged,g::kEyeImageCap,g::kEyeTotalCap):g::budget(bpp,t.width,t.height,slots,charged))s5.smokeArmWhy=why;
    if(s5.smokeArmWhy)return;
    using RtvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    D3D12_CLEAR_VALUE colour{};colour.Format=DXGI_FORMAT(s5.hudFormat);
    HRESULT hr=E_PENDING;const char* why=nullptr;
    ID3D12Resource* rt[slots][2]{};ID3D12DescriptorHeap* heap[slots][2]{};D3D12_CPU_DESCRIPTOR_HANDLE rtv[slots][2]{};
    for(unsigned i=0;i<slots&&!why;++i)for(unsigned e=0;e<2&&!why;++e) {
        if(!step3CreateTextureWH(st,s5.ringW,s5.ringH,s5.hudFormat,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,colour,rt[i][e],hr))why="smoke-rt-create-failed";
        else if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,heap[i][e],hr,rtv[i][e]))why="smoke-rtv-heap-failed";
        else reinterpret_cast<RtvFn>(step1Native(st.device,20))(st.device,rt[i][e],nullptr,rtv[i][e]);
    }
    ID3D12Resource* up=nullptr;uint8_t* upPtr=nullptr;D3D12_GPU_VIRTUAL_ADDRESS upVa=0;
    if(!why) {
        HRESULT uhr=E_PENDING;
        if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,up,uhr,g::kSlotUpload*slots)||!up){hr=uhr;why="smoke-upload-create-failed";}
        else {
            using VaFn=D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*);
            using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
            upVa=reinterpret_cast<VaFn>(step1Native(up,11))(up);
            const D3D12_RANGE none{0,0};void* p=nullptr;
            if(FAILED(reinterpret_cast<MapFn>(step1Native(up,8))(up,0,&none,&p))||!p||!upVa||upVa%16)why="smoke-upload-map-failed";
            else {upPtr=static_cast<uint8_t*>(p);std::memset(upPtr,0,size_t(g::kSlotUpload*slots));}
        }
    }
    {   // commit (also on failure: partial objects are retained for the process lifetime, never used). `meta`: the consumer's acquisition reads the slots.
        std::lock_guard lock(s5.meta);
        for(unsigned i=0;i<slots;++i)for(unsigned e=0;e<2;++e){auto& sl=s5.slot[i];sl.smokeRt[e]=rt[i][e];sl.smokeRtvHeap[e]=heap[i][e];sl.smokeRtv[e]=rtv[i][e];}
        s5.smokeUpload=up;s5.smokeUploadVa=upVa;s5.smokeUploadPtr=why?nullptr:upPtr;
    }
    if(why){s5.smokeArmWhy=why;s5.createHr=hr;return;}
    s5.smokeCharged=charged;s5.smokeReady=true; // last: from here the next armed frame may latch the GPU path
}
// EFFECTS-OUTSIDE (F) fxmem:6 (fx_sprite_gpu.h): the sprite analogue of step5CreateSmokeGpu, with the same rules word for word -- per slot two colour targets
// (one per eye) of the effects target's format at the eye size (RENDER_TARGET, cleared 0,0,0,0 by the frame's first sprite replay) with CPU-only RTV heaps,
// and ONE persistently mapped UPLOAD buffer of one 512 KiB span per slot for the eye vertex copies; charged and capped BEFORE any allocation; tried ONCE per
// process (spriteTried): at ring creation when fxmem:6 is already set, else LATE by the worker tick (step-1 mutex held: no Draw seam, arm or publication
// runs meanwhile); every object built into locals, the pointers stored into the slots under `meta`, spriteReady set LAST (the next ARMED frame is the first
// that can latch it: f.eff.sp). Kept for the process lifetime (never released or recreated). Any refusal / failure leaves spriteReady=false: every sprite
// draw then takes the window effects replay exactly as with fxmem:5.
void step5CreateSpriteGpu(Step1State& st,Step5State& s5,bool late) noexcept {
    namespace g=acz::fxsprite::gpu;using namespace gpu_copy::step5;
    if(s5.spriteTried)return;
    s5.spriteTried=true;s5.spriteLate=late;s5.spriteRingPending=true;s5.spriteArmWhy=nullptr;
    const auto& t=st.s3.targets;
    gpu_copy::step3::ColorFormat cf;const unsigned bpp=gpu_copy::step3::color_format(s5.hudFormat,cf)?unsigned(cf.bytes):0u;
    uint64_t charged=0;
    if(!s5.effReady)s5.spriteArmWhy="effects-unavailable";
    else if(!effectsDualConfigured())s5.spriteArmWhy="needs-effects-dual";
    else if(!t.has_depth||!s5.ringDsv)s5.spriteArmWhy="eye-depth-missing";
    else if(s5.worldW!=t.width||s5.worldH!=t.height)s5.spriteArmWhy="effects-size-differs-from-eye"; // the window effects image = the game targets (unchanged)
    // EYE-RING: the per-eye sprite images are the eye objects' size (depth-tested against the ring DSV): ringW x ringH, eye-size caps on the eye ring.
    else if(const char* why=s5.eyeRing?g::budget(bpp,s5.ringW,s5.ringH,slots,charged,g::kEyeImageCap,g::kEyeTotalCap):g::budget(bpp,t.width,t.height,slots,charged))s5.spriteArmWhy=why;
    if(s5.spriteArmWhy)return;
    using RtvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    D3D12_CLEAR_VALUE colour{};colour.Format=DXGI_FORMAT(s5.hudFormat);
    HRESULT hr=E_PENDING;const char* why=nullptr;
    ID3D12Resource* rt[slots][2]{};ID3D12DescriptorHeap* heap[slots][2]{};D3D12_CPU_DESCRIPTOR_HANDLE rtv[slots][2]{};
    for(unsigned i=0;i<slots&&!why;++i)for(unsigned e=0;e<2&&!why;++e) {
        if(!step3CreateTextureWH(st,s5.ringW,s5.ringH,s5.hudFormat,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,colour,rt[i][e],hr))why="sprite-rt-create-failed";
        else if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,heap[i][e],hr,rtv[i][e]))why="sprite-rtv-heap-failed";
        else reinterpret_cast<RtvFn>(step1Native(st.device,20))(st.device,rt[i][e],nullptr,rtv[i][e]);
    }
    ID3D12Resource* up=nullptr;uint8_t* upPtr=nullptr;D3D12_GPU_VIRTUAL_ADDRESS upVa=0;
    if(!why) {
        HRESULT uhr=E_PENDING;
        if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,up,uhr,g::kSlotStride*slots)||!up){hr=uhr;why="sprite-upload-create-failed";}
        else {
            using VaFn=D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*);
            using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
            upVa=reinterpret_cast<VaFn>(step1Native(up,11))(up);
            const D3D12_RANGE none{0,0};void* p=nullptr;
            if(FAILED(reinterpret_cast<MapFn>(step1Native(up,8))(up,0,&none,&p))||!p||!upVa||upVa%16)why="sprite-upload-map-failed";
            else {upPtr=static_cast<uint8_t*>(p);std::memset(upPtr,0,size_t(g::kSlotStride*slots));}
        }
    }
    {   // commit (also on failure: partial objects are retained for the process lifetime, never used). `meta`: the consumer's acquisition reads the slots.
        std::lock_guard lock(s5.meta);
        for(unsigned i=0;i<slots;++i)for(unsigned e=0;e<2;++e){auto& sl=s5.slot[i];sl.spriteRt[e]=rt[i][e];sl.spriteRtvHeap[e]=heap[i][e];sl.spriteRtv[e]=rtv[i][e];}
        s5.spriteUpload=up;s5.spriteUploadVa=upVa;s5.spriteUploadPtr=why?nullptr:upPtr;
    }
    if(why){s5.spriteArmWhy=why;s5.createHr=hr;return;}
    s5.spriteCharged=charged;s5.spriteReady=true; // last: from here the next armed frame may latch the GPU path
}
// The fx-sprite-ring line (step-1 mutex held; dedicated log): once, after the creation attempt (at the ring or late).
void step5EmitSpriteRing(Step1State& st,Step5State& s5) noexcept {
    if(!s5.spriteRingPending)return;
    s5.spriteRingPending=false;
    const int k=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
    int m=-1;
    if(k>0&&size_t(k)<sizeof st.text)m=acz::fxsprite::gpu::format_ring(st.text+k,sizeof st.text-size_t(k),s5.spriteReady,s5.spriteArmWhy,gpu_copy::step5::slots,
        s5.hudFormat,s5.ringW,s5.ringH,s5.spriteCharged); // EYE-RING: the per-eye image size (= the targets' size without the eye ring)
    if(m>0&&size_t(k)+size_t(m)<sizeof st.text)std::snprintf(st.text+k+m,sizeof st.text-size_t(k+m),";created=%s",s5.spriteLate?"late(worker,option-after-ring)":"ring");
    step5Log(st,s5,false);
}
// EFFECTS-OUTSIDE (M6) fxatlas (fx_puff_atlas.h; docs section 23): the atlas resources, tried ONCE per process by the worker tick (step-1 mutex held: no
// Draw seam, arm or publication runs meanwhile) when fxatlas is on and the sprite resources are ready; on the ring's device (st.device). Charged against
// atlas::kBudget before any allocation; every object built into locals; atlasReady set LAST. Kept for the process lifetime (never released or recreated:
// a recording that captured into it or reads it may still be in flight; teardown is vetoed as for the ring). Any failure leaves atlasReady=false: no
// capture and no atlas draw ever happen, the M5 key path is unchanged. The atlas starts in PIXEL_SHADER_RESOURCE (the state between our uses in every list).
void step5CreateAtlas(Step1State& st,Step5State& s5) noexcept {
    namespace A=acz::fxpuff::atlas;using namespace gpu_copy::step5;
    if(s5.atlasTried)return;
    s5.atlasTried=true;s5.atlasRingPending=true;s5.atlasWhy=nullptr;
    if(!st.device){s5.atlasWhy="atlas-no-device";return;}
    if(slots>A::kSlotCap){s5.atlasWhy="atlas-slot-count";return;}
    if(A::kBytes+uint64_t(slots)*A::kCopyRegion>A::kBudget+(1ull<<20)){s5.atlasWhy="atlas-over-budget";return;}
    using ResFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    using HeapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_DESCRIPTOR_HEAP_DESC*,REFIID,void**);
    using CpuStartFn=D3D12_CPU_DESCRIPTOR_HANDLE*(STDMETHODCALLTYPE*)(ID3D12DescriptorHeap*,D3D12_CPU_DESCRIPTOR_HANDLE*);
    using GpuStartFn=D3D12_GPU_DESCRIPTOR_HANDLE*(STDMETHODCALLTYPE*)(ID3D12DescriptorHeap*,D3D12_GPU_DESCRIPTOR_HANDLE*);
    using IncFn=UINT(STDMETHODCALLTYPE*)(ID3D12Device*,D3D12_DESCRIPTOR_HEAP_TYPE);
    using SrvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_SHADER_RESOURCE_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    using RtvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    HRESULT hr=E_PENDING;const char* why=nullptr;
    ID3D12Resource* tex=nullptr;ID3D12DescriptorHeap* rtvHeap=nullptr;ID3D12DescriptorHeap* srvHeap=nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    {   // the atlas: TEXTURE2D kWidth x kHeight, kFormat, render target, one mip, created in PIXEL_SHADER_RESOURCE (fx_puff_atlas_d3d12.h)
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        const D3D12_RESOURCE_DESC desc=A::texture_desc();
        D3D12_CLEAR_VALUE clear{};clear.Format=desc.Format;
        void* value=nullptr;
        hr=reinterpret_cast<ResFn>(step1Native(st.device,27))(st.device,&heap,D3D12_HEAP_FLAG_NONE,&desc,A::kBetweenUses,&clear,
            __uuidof(ID3D12Resource),&value);
        tex=static_cast<ID3D12Resource*>(value);
        if(FAILED(hr)||!tex)why="atlas-texture-create-failed";
    }
    if(!why&&!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,rtvHeap,hr,rtv))why="atlas-rtv-heap-failed";
    if(!why)reinterpret_cast<RtvFn>(step1Native(st.device,20))(st.device,tex,nullptr,rtv);
    if(!why) {   // our shader-visible SRV heap: kTableMax SRVs of the atlas (any descriptor of the game's key table the shader reads finds the atlas)
        const D3D12_DESCRIPTOR_HEAP_DESC hd=A::srv_heap_desc();
        void* value=nullptr;
        hr=reinterpret_cast<HeapFn>(step1Native(st.device,14))(st.device,&hd,__uuidof(ID3D12DescriptorHeap),&value);
        srvHeap=static_cast<ID3D12DescriptorHeap*>(value);
        if(FAILED(hr)||!srvHeap)why="atlas-srv-heap-failed";
        else {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{};reinterpret_cast<CpuStartFn>(step1Native(srvHeap,9))(srvHeap,&cpu);
            reinterpret_cast<GpuStartFn>(step1Native(srvHeap,10))(srvHeap,&srvGpu);
            const UINT inc=reinterpret_cast<IncFn>(step1Native(st.device,15))(st.device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if(!cpu.ptr||!srvGpu.ptr||!inc)why="atlas-srv-heap-start";
            else {
                const D3D12_SHADER_RESOURCE_VIEW_DESC sd=A::srv_desc();
                for(unsigned k=0;k<A::kTableMax;++k){D3D12_CPU_DESCRIPTOR_HANDLE h{cpu.ptr+SIZE_T(k)*inc};reinterpret_cast<SrvFn>(step1Native(st.device,18))(st.device,tex,&sd,h);}
            }
        }
    }
    ID3D12Resource* up=nullptr;uint8_t* upPtr=nullptr;D3D12_GPU_VIRTUAL_ADDRESS upVa=0;
    if(!why) {   // the capture vertex regions (one per slot, persistently mapped)
        HRESULT uhr=E_PENDING;
        if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,up,uhr,A::kCopyRegion*slots)||!up){hr=uhr;why="atlas-upload-create-failed";}
        else {
            using VaFn=D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*);
            using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
            upVa=reinterpret_cast<VaFn>(step1Native(up,11))(up);
            const D3D12_RANGE none{0,0};void* p=nullptr;
            if(FAILED(reinterpret_cast<MapFn>(step1Native(up,8))(up,0,&none,&p))||!p||!upVa||upVa%16)why="atlas-upload-map-failed";
            else {upPtr=static_cast<uint8_t*>(p);std::memset(upPtr,0,size_t(A::kCopyRegion*slots));}
        }
    }
    // commit (also on failure: partial objects are retained for the process lifetime, never used)
    s5.atlasTex=tex;s5.atlasRtvHeap=rtvHeap;s5.atlasSrvHeap=srvHeap;s5.atlasRtv=rtv;s5.atlasSrv=srvGpu;
    s5.atlasUpload=up;s5.atlasUploadVa=upVa;s5.atlasUploadPtr=why?nullptr:upPtr;
    if(why){s5.atlasWhy=why;s5.atlasHr=hr;return;}
    s5.atlasReady=true; // last: from here the next host draw may capture into / draw from the atlas
}
// The fx-puff-atlas-ring line (step-1 mutex held; dedicated log): once, after the creation attempt.
void step5EmitAtlasRing(Step1State& st,Step5State& s5) noexcept {
    namespace A=acz::fxpuff::atlas;
    if(!s5.atlasRingPending)return;
    s5.atlasRingPending=false;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=fx-puff-atlas-ring;outcome=%s;reason=%.32s;hr=%08lx;size=%ux%u;format=%u;tiles=%u(%ux%u,content=%u,gutter=%u);"
        "bytes=%llu;copy-region=%llux%u;srv-heap=%u(shader-visible);budget=%llu;lifetime=process;state=pixel-shader-resource(between-uses)",
        s5.atlasReady?"ready":"refused",s5.atlasWhy?s5.atlasWhy:"none",static_cast<unsigned long>(s5.atlasHr),A::kWidth,A::kHeight,A::kFormat,A::kTiles,A::kCols,A::kRows,
        A::kContent,A::kGutter,static_cast<unsigned long long>(A::kBytes),static_cast<unsigned long long>(A::kCopyRegion),gpu_copy::step5::slots,A::kTableMax,
        static_cast<unsigned long long>(A::kBudget));
    step5Log(st,s5,false);
}
// The fx-smoke-ring line (step-1 mutex held; dedicated log): once, after the creation attempt (at the ring or late).
void step5EmitSmokeRing(Step1State& st,Step5State& s5) noexcept {
    if(!s5.smokeRingPending)return;
    s5.smokeRingPending=false;
    const int k=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
    int m=-1;
    if(k>0&&size_t(k)<sizeof st.text)m=acz::fxsmoke::gpu::format_ring(st.text+k,sizeof st.text-size_t(k),s5.smokeReady,s5.smokeArmWhy,gpu_copy::step5::slots,
        s5.hudFormat,s5.ringW,s5.ringH,s5.smokeCharged); // EYE-RING: the per-eye image size (= the targets' size without the eye ring)
    if(m>0&&size_t(k)+size_t(m)<sizeof st.text)std::snprintf(st.text+k+m,sizeof st.text-size_t(k+m),";created=%s",s5.smokeLate?"late(worker,option-after-ring)":"ring");
    step5Log(st,s5,false);
}
void step5EmitEffectsRing(Step1State& st,Step5State& s5) noexcept { // dedicated log only; key off: not called
    using ull=unsigned long long;using namespace gpu_copy::step5;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=effects-ring;outcome=%s;reason=%s;slots=%u;format=%u;size=%llux%u;charged=%llu;caps=slot:%llu,total:%llu,draws:%u,epochs:%u;"
        "ds=shared-with-hud(no-depth-stencil-writers);per-draw=5(SetPredication,OMSet-effects,Draw,SetPredication-null,OMSet-restore);predicate=upload(%ux%ux8);"
        "clear=once-per-frame(0,0,0,0);commit=later-cockpit-member;fallback=key-off",
        s5.effReady?"ready":"refused",s5.effArmWhy?s5.effArmWhy:"none",slots,s5.hudFormat,ull(s5.worldW),s5.worldH,ull(s5.effCharged),
        ull(effects_slot_cap),ull(effects_total_cap),effects_draw_cap,effects_epoch_cap,slots,effects_epoch_cap);
    step5Log(st,s5,false);
    // J-effects-smoke-fix: the replay contract of this build (separate line: the ring line is at the line cap).
    char al[160]="";size_t n=0;
    for(unsigned i=0;i<s5.effAllow.count;++i)n+=size_t(std::snprintf(al+n,sizeof al-n,"%s%08x",i?",":"",s5.effAllow.ps[i]));
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=effects-ring-config;predication=%s(source=%s;d3d12=enabled-predication-discards-the-draw;word0=discard,word1=execute);"
        "class-gate=ps-allowlist(%u,source=%s,added=%u,bad=%u):%s+blend+depth-GREATER|GREATER_EQUAL+no-depth-write;never=ps-d34ee153;uav-or-unknown-root-layout=refused;"
        "game-predicate=refused-unless-provably-off",effects_pred_mode_name(s5.effPredMode),s5.effPredSource,s5.effAllow.count,s5.effAllowSource,s5.effAllowAdded,s5.effAllowBad,al);
    step5Log(st,s5,false);
    step5EmitSmokeRing(st,s5); // EFFECTS-OUTSIDE (C) fxmem:4: the per-eye smoke resources when created at ring creation (else: not pending)
    step5EmitSpriteRing(st,s5); // EFFECTS-OUTSIDE (F) fxmem:6: the per-eye sprite resources (same rule)
}
void step5EmitHudMarkersRing(Step1State& st,Step5State& s5) noexcept { // dedicated log only; key off: not called
    using ull=unsigned long long;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-markers-ring;outcome=%s;reason=%s;slots=%u;format=%u;size=%llux%u;charged=%llu;"
        "caps=slot:%llu,total:%llu;ds=shared-with-hud;per-draw=3(OMSet-marker,Draw,OMSet-restore);clear=once-per-frame(0,0,0,0);"
        "classify=depth-greater->box,next-text-draw->label;split=exclusive;fallback=hud-unchanged",
        s5.hudMarkersReady?"ready":"refused",s5.hudMarkersArmWhy?s5.hudMarkersArmWhy:"none",gpu_copy::step5::slots,s5.hudFormat,ull(s5.worldW),s5.worldH,
        ull(s5.hudMarkersCharged),ull(gpu_copy::step5::hud_marker_slot_cap),ull(gpu_copy::step5::hud_marker_total_cap));
    step5Log(st,s5,false);
}
void step5EmitHudRing(Step1State& st,Step5State& s5) noexcept { // dedicated log only
    using ull=unsigned long long;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-ring;outcome=%s;reason=%s;slots=%u;format=%u;ds-format=%u;size=%llux%u;scene=%llx;"
        "caps=draws:%u,commands:%u;per-draw=3(OMSet-hud,Draw,OMSet-restore);clear=once-per-frame(0,0,0,0;depth0,stencil0);mono=1;matrices=unchanged",
        s5.hudReady?"ready":"refused",s5.hudArmWhy?s5.hudArmWhy:"none",gpu_copy::step5::slots,s5.hudFormat,s5.hudDsFormat,ull(s5.worldW),s5.worldH,
        ull(s5.worldSource),gpu_copy::step5::hud_draw_cap_for(hudRadarConfigured()),gpu_copy::step5::hud_command_cap_for(hudRadarConfigured()));
    step5Log(st,s5,false);
    if(hudRadarConfigured()) { // J-hud-radar (dedicated log only)
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-radar-ring;key=gpu_copy_step5_hud_radar;caps=draws:%u,commands:%u;excursion-cap=%u;"
            "region=segmented(excursion=draw-or-bind-on-another-target,resume=scene-target-again);late-draws=window-only(REPLACE-writer|EQUAL-opens,excursion-closes);"
            "equal=accepted-with-replayed-REPLACE-writer-and-no-skipped-writer;skips=a3050-shaped,notequal-quad;stencil-ref=untracked;budget=%s",
            gpu_copy::step5::hud_radar_draw_cap,gpu_copy::step5::hud_radar_command_cap,gpu_copy::step5::hud_radar_excursion_cap,
            gpu_copy::step5::hud_radar_budget(gpu_copy::step5::hud_radar_draw_cap,gpu_copy::step5::hud_radar_command_cap,gpu_copy::step5::hud_radar_clears,
                gpu_copy::step5::hud_radar_command_bytes,gpu_copy::step5::hud_radar_memory_cap)?"over":"ok");
        step5Log(st,s5,false);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-radar-legend;stencil-func=A:always,E:equal,N:not-equal,-:off,l:less,k:less-equal,g:greater,h:greater-equal,X:never;"
            "disposition=r:replayed,x:refused,a:away(another-target),t:no-target,n:a3050-skipped,q:notequal-skipped,l:late-skipped;stencil-ref=untracked(the proxy does not track OMSetStencilRef)");
        step5Log(st,s5,false);
    }
    if(hudMarkersConfigured())step5EmitHudMarkersRing(st,s5);
    if(effectsConfigured())step5EmitEffectsRing(st,s5);
}
void step5EmitRing(Step1State& st,Step5State& s5) noexcept {
    const auto& t=st.s3.targets;
    // EYE-RING: the per-eye pairs are ringW x ringH (without the eye ring: exactly the targets' size and bytes, the old line)
    const uint64_t eyeRt=s5.eyeRing?::acz::eyering::image_bytes(s5.ringW,s5.ringH,unsigned(t.color.bytes)):t.rt_bytes;
    const uint64_t eyeDs=s5.eyeRing?(t.has_depth?::acz::eyering::image_bytes(s5.ringW,s5.ringH,unsigned(t.depth.bytes)):0u):t.ds_bytes;
    const uint64_t perEye=eyeRt+eyeDs,world=s5.worldReady?t.rt_bytes*gpu_copy::step5::slots:0;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=ring;mode=continuous;slots=%u;per-slot=2x(RT %s %ux%u + DS %s)+world:%u;"
        "bytes=%llu(=%llu MiB);world-bytes=%llu;ds=one-pair-per-slot,resource-format:%u,dsv-format:%u;upload=%llu;members=%u;"
        "commands-per-frame=%u(+%u world);readback=none;png=none;world=%s;world-source=%llx",gpu_copy::step5::slots,t.color.name,s5.ringW,s5.ringH,
        t.has_depth?t.depth.name:"none",unsigned(s5.worldReady),static_cast<unsigned long long>(perEye*2*gpu_copy::step5::slots+world),
        static_cast<unsigned long long>((perEye*2*gpu_copy::step5::slots+world)>>20),static_cast<unsigned long long>(world),s5.ringDsTypeless,s5.ringDsv,
        static_cast<unsigned long long>(s5.slotUpload*gpu_copy::step5::slots),st.s3.memberCount,
        (t.has_depth?4u:2u)+s5.perMember*st.s3.memberCount,s5.worldReady?step5WorldCommands:0u,
        s5.worldReady?"ready":(s5.worldArmWhy?s5.worldArmWhy:"unavailable"),static_cast<unsigned long long>(s5.worldSource));
    step5Log(st,s5);
    if(s5.eyeRingAsked) { // EYE-RING: one line once eyeres asked for it (eyeres:0 / absent: no line)
        namespace er=::acz::eyering;using ull=unsigned long long;
        const unsigned rb=unsigned(t.color.bytes),db=t.has_depth?unsigned(t.depth.bytes):0u;
        const auto ch=er::read();
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=eye-ring;mode=%s;why=%s;game=%ux%u;per-eye=%ux%u;eye-extent=%ux%u;eyeobj=%u;"
            "ring-mib=%llu(game-size-%llu);per-eye-fx-kind-mib=%llu(game-size-%llu);rs=scaled(eye/game)+restored-per-replay;"
            "game-screen-images=game-size(world,hud,markers,effects);extra-commands-per-replay=%u;fallback=eyeobj:0",
            s5.eyeRing?"eye":"game-size",s5.eyeRingWhy,s5.gameW,s5.gameH,s5.ringW,s5.ringH,ch.w,ch.h,ch.objects?1u:0u,
            ull(er::ring_bytes(s5.ringW,s5.ringH,rb,db,gpu_copy::step5::slots)>>20),ull(er::ring_bytes(s5.gameW,s5.gameH,rb,db,gpu_copy::step5::slots)>>20),
            ull(er::per_eye_kind_bytes(s5.ringW,s5.ringH,rb,gpu_copy::step5::slots)>>20),ull(er::per_eye_kind_bytes(s5.gameW,s5.gameH,rb,gpu_copy::step5::slots)>>20),
            s5.eyeRing?er::kExtraCommands:0u);
        step5Log(st,s5,false); // dedicated step5-producer.log only (the shared log's record budget is not touched)
    }
}
// Ready phase, after the set was chosen and the targets decided (owner context).
// Returns nullptr when the producer runs, or the refusal.
const char* step5Arm(Step1State& st) noexcept {
    auto* s5=g_step5.load();
    const auto& t=st.s3.targets;
    if(!s5) {
        s5=new(std::nothrow) Step5State;if(!s5)return "step5-state-unavailable";
        s5->ringW=t.width;s5->ringH=t.height;s5->ringFormat=st.s3.shape.pso.rtv[0];s5->ringDsv=st.s3.shape.pso.dsv;s5->ringClear=t.depth_clear;
        s5->ringDsTypeless=step5TypelessDepth(s5->ringDsv);
        if(byClassConfigured()) { // F3: raised caps and per-slot upload, fixed for the process lifetime
            s5->memberCap=gpu_copy::step5::class_member_cap;s5->commandCap=gpu_copy::step5::class_command_cap;
            s5->slotUpload=gpu_copy::step5::class_slot_upload_bytes;
        }
        // EYE-RING (eye_ring.h, eye-resolution.md 5.1 (a)): the per-eye targets at the eye swapchain size when eyeres + eyeobj asked for it and the XR
        // swapchains already exist (vsearly); fixed for the process lifetime. Not asked (eyeres:0 / absent): nothing below changes anything.
        s5->gameW=t.width;s5->gameH=t.height;
        {
            const auto ch=::acz::eyering::read();
            const auto c=::acz::eyering::choose(ch,t.width,t.height,unsigned(t.color.bytes),t.has_depth?unsigned(t.depth.bytes):0u);
            s5->eyeRingAsked=ch.requested;s5->eyeRingWhy=c.why;
            if(c.eye) {
                s5->eyeRing=true;s5->ringW=c.w;s5->ringH=c.h;
                s5->perMember=gpu_copy::step3::set_per_member+::acz::eyering::kExtraCommands;
                s5->commandCap+=::acz::eyering::kExtraCommands*s5->memberCap; // the same member capacity as before (4 RS calls per member)
            }
        }
        if(const char* why=step5CreateRing(st,*s5))return why; // partial objects are retained (process lifetime)
        s5->ringReady=true;
        s5->log=acz::feature_log::open("step5-producer");
        s5->epochFence=st.fence.fence;s5->fenceEpoch.store(++g_step5EpochCounter);
        step5RegisterFence(st.fence.fence,s5->fenceEpoch.load());
        step5SetWorldWatch(s5->worldReady?s5->worldSource:0);
        {std::lock_guard lock(st.mutex);step5EmitRing(st,*s5);if(hudReplayConfigured())step5EmitHudRing(st,*s5);
         step5BuildClasses(st,*s5);step5EmitClasses(st,*s5);}
        g_step5.store(s5);
        return nullptr;
    }
    // Re-selection: the ring was built for the earlier target shape.
    if(t.width!=s5->gameW||t.height!=s5->gameH||st.s3.shape.pso.rtv[0]!=s5->ringFormat||st.s3.shape.pso.dsv!=s5->ringDsv|| // EYE-RING: the GAME size
       t.depth_clear!=s5->ringClear)return "reselected-target-shape-changed";
    {std::lock_guard lock(st.mutex);
     if(s5->worldReady) { // the world textures stay; the source follows the new selection when it qualifies
         const auto& g=st.s3.shape;
         if(!step5WorldSourceWhy(st)&&g.game_rt_width==s5->worldW&&g.game_rt_height==s5->worldH&&g.game_rt_format==s5->worldFormat)
             s5->worldSource=g.game_rt;
         else s5->worldSource=0;
     }
     step5SetWorldWatch(s5->worldReady?s5->worldSource:0);
     step5BuildClasses(st,*s5);step5EmitClasses(st,*s5);}
    return nullptr;
}
void step5Reselect(Step1State& st) noexcept { // step-1 mutex held
    auto& s3=st.s3;auto* s5=g_step5.load();
    ++s5->reselections;s5->consecutiveMismatch=0;s5->pend.set=false;
    s3.observed=false;s3.armed=false;s3.selectionClosed=false;s3.ringHead=s3.ringTail=s3.recorded=0;s3.dropped=0;s3.evaluated=0;
    s3.visibleCount=0;s3.unreadable=0;s3.haveBest=false;s3.refusal=nullptr;s3.firstRefusal=nullptr;s3.selectTicks=0;s3.memberCount=0;
    s3.visibleCands.clear();s3.visibleEvals.clear();s3.tableText.clear();s3.tableLines=0;
    g_step1Phase.store(static_cast<int>(Step1Phase::ready));
}
// J-compat-race (headset test 2): once the native binding observer has disabled itself
// (fail-closed, permanent for the process), every live SRT read refuses with
// `binding-observer-disabled` and the producer skips EVERY frame without any FAIL line.
// Make that state loud: one `producer-dead` line in step5-producer.log (mirrored to the
// shared log) when first seen, then at most one per minute, capped. Observation only:
// the fail-closed semantics are unchanged and nothing re-arms the observer.
void step5DeadWatch(Step1State& st,Step5State& s5) noexcept { // step-1 mutex held (nests the hook-state mutex)
    const char* fresh=st.s3.freshness;
    if(!fresh||std::strcmp(fresh,"binding-observer-disabled")!=0)return;
    char why[96]{};bool off=false;
    {auto& s=state();std::lock_guard lock(s.mutex);off=!s.nativeBindings.enabled();
     if(off){const char* r=s.nativeBindings.disable_reason();std::snprintf(why,sizeof why,"%s",r?r:"unknown");}}
    if(!off)return;
    if(!s5.deadSeen){s5.deadSeen=true;s5.deadSince=s5.published.load();s5.deadTicks=0;}
    else if(s5.deadLines>=gpu_copy::step5::dead_line_cap||++s5.deadTicks<gpu_copy::step5::dead_repeat_ticks)return;
    else s5.deadTicks=0;
    ++s5.deadLines;
    using ull=unsigned long long;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=producer-dead;reason=binding-observer-disabled;since-frame=%llu;published=%llu;skipped=%llu;"
        "observer-reason=%s;line=%u/%u;recovery=none(fail-closed,process-restart-required)",ull(s5.deadSince),ull(s5.published.load()),
        ull(s5.skipped.load()),why,s5.deadLines,gpu_copy::step5::dead_line_cap);
    step5Log(st,s5);
}
void step5Status(Step1State& st,Step5State& s5,const char* prefix) noexcept { // step-1 mutex held
    unsigned states[5]{};
    if(s5.meta.try_lock()){for(const auto& sl:s5.slot)if(sl.state>=0&&sl.state<5)++states[sl.state];s5.meta.unlock();}
    using ull=unsigned long long;
    std::snprintf(st.text,sizeof st.text,"%s;mode=continuous;published=%llu;skipped=%llu;skip=mismatch:%llu,no-slot:%llu,unqualified:%llu,"
        "unreadable:%llu;mismatch-produced=%llu;member-refused=%llu;discarded=%llu;ambiguous=%llu;failures=%llu;partial=%llu;"
        "slots=free:%u,producing:%u,published:%u,held:%u,quarantined:%u;fence=%llu,completed:%llu,lag:%llu;avg-commands=%.1f;"
        "avg-members=%.2f;reselections=%llu;last-fault=%s",prefix,ull(s5.published.load()),ull(s5.skipped.load()),ull(s5.skipKind[0]),
        ull(s5.skipKind[3]),ull(s5.skipKind[1]),ull(s5.skipKind[2]),ull(s5.mismatchProduced),ull(s5.memberRefused),ull(s5.discarded),
        ull(s5.ambiguous),ull(s5.failures),ull(s5.partialFrames),states[0],states[1],states[2],states[3],states[4],ull(s5.fenceCounter),
        ull(s5.lastCompleted),ull(s5.fenceCounter>s5.lastCompleted?s5.fenceCounter-s5.lastCompleted:0),
        s5.frames?double(s5.commandSum)/double(s5.frames):0.0,s5.frames?double(s5.memberSum)/double(s5.frames):0.0,
        ull(s5.reselections),s5.lastFault?s5.lastFault:"none");
    step5Log(st,s5);
    // Second line: quality (pose, lens, world, member-count changes, last reasons).
    char p2[64];std::snprintf(p2,sizeof p2,"%s-quality",prefix);
    std::snprintf(st.text,sizeof st.text,"%s;pose-valid=%llu/%llu;lens-valid=%llu/%llu;lens-tan=%.5f,%.5f,%.5f,%.5f;world=valid:%llu,refused:%llu,"
        "last-refusal:%s;members=extra:%llu,fewer:%llu;mismatch-events=%llu;pose-dropped=%llu;gpu-busy=%llu;last-skip=%s;last-member-refusal=%s;"
        "last-source=%s",p2,ull(s5.poseFrames),
        ull(s5.frames),ull(s5.lensFrames),ull(s5.frames),double(s5.lens.tan[0]),double(s5.lens.tan[1]),double(s5.lens.tan[2]),
        double(s5.lens.tan[3]),ull(s5.worldFrames),ull(s5.worldRefused),s5.worldLastWhy?s5.worldLastWhy:"none",ull(s5.extraMembers),
        ull(s5.fewerMembers),ull(s5.mismatchEvents),ull(s5.poseDropped),ull(s5.gpuBusy),s5.lastSkipWhy?s5.lastSkipWhy:"none",
        s5.lastMemberWhy?s5.lastMemberWhy:"none",st.s3.freshness?st.s3.freshness:"none");
    step5Log(st,s5);
    if(s5.eyeRingAsked) { // EYE-RING: unconditional with every status while eyeres asked for it (zero counters = nothing replayed on the eye ring)
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=summary-eye-ring;mode=%s;why=%s;per-eye=%ux%u;game=%ux%u;member-draws=%llu;fx-replays=%llu;"
            "vp-sub=%llu;scissor-sub=%llu;vp-restore=%llu;rs-refused=%llu;smoke=%s;sprite=%s;no-headset-claim",s5.eyeRing?"eye":"game-size",s5.eyeRingWhy,
            s5.ringW,s5.ringH,s5.gameW,s5.gameH,ull(s5.eyeMemberDraws),ull(s5.eyeFxReplays),ull(s5.eyeVpSub),ull(s5.eyeScSub),ull(s5.eyeVpRestore),
            ull(s5.eyeRsRefused),s5.smokeReady?"ready":(s5.smokeArmWhy?s5.smokeArmWhy:"none"),s5.spriteReady?"ready":(s5.spriteArmWhy?s5.spriteArmWhy:"none"));
        step5Log(st,s5,false); // dedicated step5-producer.log only
    }
    // FPS investigation (docs/world-render/fps-investigation.md): where the game-side milliseconds go, per game frame of this window
    // (frames = published+skipped delta). Dedicated log only (not mirrored into the shared ring). Observe-only counters (perf_counters.h).
    {
        static perf::Window s_window;static uint64_t s_frames=0;
        const uint64_t frames=s5.published.load()+s5.skipped.load();
        const auto report=perf::take(s_window);
        if(report.window_us>0) {
            perf::format(st.text,sizeof st.text,report,frames-s_frames,1);
            step5Log(st,s5,false);
            perf::format(st.text,sizeof st.text,report,frames-s_frames,2);
            step5Log(st,s5,false);
        }
        s_frames=frames;
    }
    // J-180 P1: recording ownership of injected writers. Only emitted once the ownership rule had an EFFECT (a re-execution of an owned recording, a
    // slot pick that skipped an owned slot, a refused insertion): ordinary runs (Reset every frame) keep their log byte-identical.
    if(s5.ownerReexec||s5.ownerBlocked||s5.ownerRefused) {
        char po[64];std::snprintf(po,sizeof po,"%s-owners",prefix);
        std::snprintf(st.text,sizeof st.text,"%s;live=%u/%u;added=%llu;retired=%llu;re-executions=%llu;re-executions-of-held-slot=%llu;slot-picks-blocked=%llu;"
            "insertions-refused-table-full=%llu;rule=slot-pinned-until-reset-or-release",po,step5OwnerLive_locked(s5),step5_owner_cap,ull(s5.ownerAdded),
            ull(s5.ownerRetired),ull(s5.ownerReexec),ull(s5.ownerHeld),ull(s5.ownerBlocked),ull(s5.ownerRefused));
        step5Log(st,s5,false);
    }
    // J-world-hold: world refusal split and look-pitch proxies (always on with step 5; only once a refusal happened; dedicated log).
    if(s5.worldRefused) {
        char subs[240]{};size_t at=0;
        for(unsigned i=0;i<gpu_copy::step5::entry_subs&&at+40<sizeof subs;++i)if(s5.worldEntrySub[i]) {
            const int n=std::snprintf(subs+at,sizeof subs-at,"%s%s:%llu",at?",":"",gpu_copy::step5::entry_sub_name(i),ull(s5.worldEntrySub[i]));
            if(n>0)at+=size_t(n);
        }
        char p11[64];std::snprintf(p11,sizeof p11,"%s-world-refusal",prefix);
        std::snprintf(st.text,sizeof st.text,"%s;refused=%llu;entry-state-unknown=%llu;sub=%s;bursts=%llu;details=%u/%u;details-dropped=%llu",p11,ull(s5.worldRefused),
            ull(s5.worldEntryTotal),at?subs:"none",ull(s5.worldBursts),s5.refDetailEmitted,s5.refDetailCount,ull(s5.worldRefDetailDropped));
        step5Log(st,s5,false);
        auto stat=[](const gpu_copy::step5::PitchStat& p,char* out,size_t cap) {
            if(!p.nHead)std::snprintf(out,cap,"n=%llu,head=none",ull(p.n));
            else std::snprintf(out,cap,"n=%llu,head-deg=%.1f..%.1f",ull(p.n),p.headMin,p.headMax);
        };
        char a[64],r[64],e[64];stat(s5.pitchAccepted,a,sizeof a);stat(s5.pitchRefused,r,sizeof r);stat(s5.pitchEntry,e,sizeof e);
        char p12[64];std::snprintf(p12,sizeof p12,"%s-world-pitch",prefix);
        std::snprintf(st.text,sizeof st.text,"%s;accepted=%s;refused=%s;entry-state-unknown=%s",p12,a,r,e);
        size_t len=std::strlen(st.text);
        auto cam=[&](const gpu_copy::step5::PitchStat& p,const char* tag) {
            if(!p.nCam||len+90>=sizeof st.text)return;
            const int n=std::snprintf(st.text+len,sizeof st.text-len,";%s=n:%llu,x:%.2f..%.2f,y:%.2f..%.2f,z:%.2f..%.2f",tag,ull(p.nCam),p.fwdMin[0],p.fwdMax[0],
                p.fwdMin[1],p.fwdMax[1],p.fwdMin[2],p.fwdMax[2]);
            if(n>0)len+=size_t(n);
        };
        cam(s5.pitchAccepted,"cam-fwd-accepted");cam(s5.pitchRefused,"cam-fwd-refused");
        if(len+60<sizeof st.text)std::snprintf(st.text+len,sizeof st.text-len,";head=eye0-pose-elevation(yaw-only-recentring)");
        step5Log(st,s5,false);
    }
    // J-cockpit-fov status lines (dedicated log only; each only with its key).
    if(hmdFovConfigured()) {
        char p4[64];std::snprintf(p4,sizeof p4,"%s-fov",prefix);
        const auto& v=s5.pf.fov;
        std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_hmd_fov;hmd=%llu;fallback=%llu;clamped=%llu;posed=%llu/%llu;last-fallback=%s;"
            "last=%s;eye0=%.5f,%.5f,%.5f,%.5f;eye1=%.5f,%.5f,%.5f,%.5f;tan-max=%.1f;reference=game-lens;rt=%ux%u",p4,ull(s5.fovFrames),ull(s5.fovFallback),
            ull(s5.fovClampedFrames),ull(s5.poseFrames),ull(s5.frames),s5.fovLastWhy?s5.fovLastWhy:"none",s5.pf.fovWhy?s5.pf.fovWhy:"none",
            double(v[0].angle[0]),double(v[0].angle[1]),double(v[0].angle[2]),double(v[0].angle[3]),double(v[1].angle[0]),double(v[1].angle[1]),
            double(v[1].angle[2]),double(v[1].angle[3]),gpu_copy::step5::fov_tan_max,s5.ringW,s5.ringH);
        step5Log(st,s5,false);
    }
    if(byClassConfigured()) {
        char p5[64];std::snprintf(p5,sizeof p5,"%s-members",prefix);
        const auto& r=s5.classReason;
        std::snprintf(st.text,sizeof st.text,"%s;mode=by-class;frames=%llu;partial=%llu;cap-frames=%llu;avg-members=%.2f;refused=state:%llu,descriptor:%llu,"
            "format:%llu,cap:%llu,class:%llu,camera:%llu,unreadable:%llu;out-of-class-starts=%llu;family-classes=%u(+%llu-unlogged);selected-classes=%u;"
            "caps=members:%u,commands:%u,upload-per-slot:%llu;last-refusal=%s",p5,ull(s5.classFrames),ull(s5.classPartial),ull(s5.classCapFrames),
            s5.frames?double(s5.memberSum)/double(s5.frames):0.0,ull(r[0]),ull(r[1]),ull(r[2]),ull(r[3]),ull(r[4]),ull(r[5]),ull(r[6]),ull(s5.classOutStarts),
            s5.familyCount,ull(s5.classUnlogged),s5.classCount,s5.memberCap,s5.commandCap,ull(s5.slotUpload),s5.classLastWhy?s5.classLastWhy:"none");
        step5Log(st,s5,false);
    }
    if(worldObjectsConfigured()) { // J-world-objects (dedicated log only)
        char p8[64];std::snprintf(p8,sizeof p8,"%s-world",prefix);
        const auto& r=s5.wobjReason;
        // Totals across frames. `not-evaluated` when no frame ever reached the world camera recovery (nothing is claimed about it).
        char camera[160];
        if(!s5.wobjCamEval)std::snprintf(camera,sizeof camera,"not-evaluated");
        else std::snprintf(camera,sizeof camera,"evaluated:%llu,recovered:%llu,failed:%llu,last-failure:%s",ull(s5.wobjCamEval),ull(s5.wobjCamOk),
            ull(s5.wobjCamFail),s5.wobjCamLastWhy?s5.wobjCamLastWhy:"none");
        std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_world_objects;frames=%llu;members=%llu;avg-per-frame=%.2f;seen=%llu;before-frame=%llu;"
            "refused=state:%llu,class:%llu,record:%llu,camera:%llu,pose:%llu,unreadable:%llu,cap:%llu,target:%llu;last-refusal=%s;"
            "pso=vs-match:%u,vs-hash-only:%u,ps-fingerprint:%u,identified:%u;cap=%u-per-frame",p8,ull(s5.wobjFrames),ull(s5.wobjRecordedSum),
            s5.wobjFrames?double(s5.wobjRecordedSum)/double(s5.wobjFrames):0.0,ull(s5.wobjSeen),ull(s5.wobjBeforeFrame),ull(r[0]),ull(r[1]),ull(r[2]),
            ull(r[3]),ull(r[4]),ull(r[5]),ull(r[6]),ull(r[7]),s5.wobjLastWhy?s5.wobjLastWhy:"none",g_worldPso.vsMatch.load(),g_worldPso.vsHashOnly.load(),
            g_worldPso.psFingerprint.load(),g_worldPso.matched.load(),gpu_copy::step5::world::draw_cap);
        step5Log(st,s5,false);
        // Same-list early frame start (J-world-objects v2): opened by a scene-RT world draw, adopted by the list's first cockpit member.
        {char pe[64];std::snprintf(pe,sizeof pe,"%s-world-early",prefix);const auto& e=s5.wobjEarlyReason;
         std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_world_objects;opened=%llu;adopted=%llu;abandoned=%llu;adopt-camera-mismatch=%llu;"
            "same-list-blocked=%llu;backoff-remaining-lists=%u;last-start-refusal=%s;last-adopt-refusal=%s;last-backoff=%s",pe,ull(s5.wobjEarlyOpened),
            ull(s5.wobjEarlyAdopted),ull(s5.wobjEarlyAbandoned),ull(s5.wobjEarlyMismatch),ull(s5.wobjEarlyBlocked),s5.wobjEarlyHold,
            s5.wobjEarlyLastWhy?s5.wobjEarlyLastWhy:"none",s5.wobjAdoptLastWhy?s5.wobjAdoptLastWhy:"none",s5.wobjEarlyOff?s5.wobjEarlyOff:"none");
         step5Log(st,s5,false);
         if(s5.povOpened||s5.povOpenRefused||s5.povCockpitPoisoned) {   // POV-RENDER stage 2 (pov:2): only once a chase request reached the producer
             std::snprintf(st.text,sizeof st.text,"%s-pov;key=xr_terrain_stage(pov:2);no-cockpit-opened=%llu;opener-refused=%llu;cockpit-poisoned=%llu;relative-admitted=%llu;"
                "relative-refused=%llu;rig-enters=%llu;rig-yaw-deg=%.2f;last=%s;kinds=chase:%llu,hud:%llu,cin:%llu;anchor=e:%llu,c:%llu,r:%llu,last:%s;a-only=%llu",
                pe,ull(s5.povOpened),ull(s5.povOpenRefused),ull(s5.povCockpitPoisoned),ull(s5.povRelative),
                ull(s5.povRelativeRefused),ull(s5.povRig.enters()),s5.povRigYaw*57.29577951308232,s5.povLastWhy?s5.povLastWhy:"none",
                ull(s5.povOpenedKind[0]),ull(s5.povOpenedKind[1]),ull(s5.povOpenedKind[2]),ull(s5.povAnchor.enters()),ull(s5.povAnchor.cuts()),
                ull(s5.povAnchor.relatches()),s5.povAnchorLast?s5.povAnchorLast:"none",ull(s5.povOpenedAOnly));
             step5Log(st,s5,false);
         }
         std::snprintf(st.text,sizeof st.text,"%s-refused;key=gpu_copy_step5_world_objects;start-refused=state:%llu,class:%llu,record:%llu,camera:%llu,pose:%llu,"
            "unreadable:%llu,cap:%llu,offscreen-target:%llu",pe,ull(e[0]),ull(e[1]),ull(e[2]),ull(e[3]),ull(e[4]),ull(e[5]),ull(e[6]),ull(e[7]));
         step5Log(st,s5,false);}
        // Second line: camera recovery TOTALS across frames (never a last-frame flag) and the other-list counter.
        char p9[64];std::snprintf(p9,sizeof p9,"%s-world-camera",prefix);
        std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_world_objects;camera=%s;seen-in-other-list-while-frame-active=%llu;refused-total=%llu;"
            "recorded-total=%llu;frames-with-world-members=%llu;seen=%llu;before-frame=%llu",p9,camera,ull(s5.wobjOtherList),ull(s5.wobjRefusedSum),
            ull(s5.wobjRecordedSum),ull(s5.wobjFrames),ull(s5.wobjSeen),ull(s5.wobjBeforeFrame));
        step5Log(st,s5,false);
        // J-world-evidence: ALL ten refusal categories (the two added by Astra reply 5: rt-count = PSO with other than one render target,
        // cb10 = the draw's cb10 dword 0 unknown / fade bit set), member path and early-start path.
        {char pc[64];std::snprintf(pc,sizeof pc,"%s-world-categories",prefix);const auto& m=s5.wobjReason;const auto& e=s5.wobjEarlyReason;
         std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_world_objects;m=state:%llu,class:%llu,record:%llu,camera:%llu,pose:%llu,unreadable:%llu,"
            "cap:%llu,target:%llu,rt-count:%llu,cb10:%llu;s=state:%llu,class:%llu,record:%llu,camera:%llu,pose:%llu,unreadable:%llu,cap:%llu,"
            "target:%llu,rt-count:%llu,cb10:%llu",pc,ull(m[0]),ull(m[1]),ull(m[2]),ull(m[3]),ull(m[4]),ull(m[5]),ull(m[6]),ull(m[7]),ull(m[8]),ull(m[9]),
            ull(e[0]),ull(e[1]),ull(e[2]),ull(e[3]),ull(e[4]),ull(e[5]),ull(e[6]),ull(e[7]),ull(e[8]),ull(e[9]));
         step5Log(st,s5,false);}
        // J-180 (Codex review): completeness, PS authentication, depth-fix clip band. world-incomplete frames are PUBLISHED (cockpit kept) with the flag.
        {char pj[64];std::snprintf(pj,sizeof pj,"%s-world-j180",prefix);
         char band[96];if(s5.wobjClipValid)std::snprintf(band,sizeof band,"%.4g..%.4g",s5.wobjClipLo,s5.wobjClipHi);else std::snprintf(band,sizeof band,"none");
         std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_world_objects;world-incomplete-frames=%llu;lost-draws=%llu;lost-before-frame-open=%llu;"
            "ps-gate=decoded-hash+full-sha256;ps-full-match=%u;ps-fingerprint-unverified-refused=%u;"
            "depth-fix-clip=diagnostic-approximation;depth-fix-frames=%llu;unmodelled=%llu;relaxed-view-depth-band=%s",pj,ull(s5.wobjIncompleteFrames),ull(s5.wobjLostSum),
            ull(s5.wobjPreLostSum),g_worldPso.psFull.load(),g_worldPso.psUnverified.load(),ull(s5.wobjClipFrames),ull(s5.wobjClipUnmodelled),band);
         step5Log(st,s5,false);}
    }
    if(worldEvidenceConfigured()) { // J-world-evidence (dedicated log only)
        gpu_copy::wev::State::Totals t;static char sum[gpu_copy::wev::summary_lines][gpu_copy::wev::line_bytes];unsigned sumN=0;
        {std::lock_guard lock(g_wevMutex);t=g_wev.totals();sumN=gpu_copy::wev::format_summary(g_wev,sum);}
        char pv[64];std::snprintf(pv,sizeof pv,"%s-world-evidence",prefix);
        std::snprintf(st.text,sizeof st.text,"%s;keys=world_observe|world_objects;rt-draws-seen=%llu;rt-records=%llu(cap %u);rt-suppressed=%llu;"
            "rt-pso-table-full=%llu;rt-lost=%llu;records-stored=%llu;records-written=%llu;windows-opened=%llu(cap %u);drops=window-full:%llu,window-cap:%llu,"
            "spacing:%llu,slots-busy:%llu;decisions=%llu;distinct-reasons=%u;reason-overflow=%llu;commands=none;gpu-work=none",pv,ull(t.rtSeen),ull(t.rtRecords),
            gpu_copy::wev::rt_record_cap,ull(t.rtSuppressed),ull(t.rtTableFull),ull(t.rtLost),ull(t.srtStored),ull(t.srtDone),ull(t.windows),gpu_copy::wev::window_cap,
            ull(t.dropWindowFull),ull(t.dropWindowCap),ull(t.dropSpacing),ull(t.dropBusy),ull(t.decisions),t.reasons,ull(t.reasonOverflow));
        step5Log(st,s5,false);
        for(unsigned i=0;i<sumN;++i)acz::feature_log::write(s5.log,sum[i]);
    }
    if(precockpitConfigured()) {
        char p6[64];std::snprintf(p6,sizeof p6,"%s-precockpit",prefix);
        uint64_t frames=0,sampled=0,records=0,lost=0,truncated=0;
        {std::lock_guard lock(g_pcMutex);frames=g_pc.frames;sampled=g_pc.sampled;records=g_pc.records;lost=g_pc.lost;truncated=g_pc.truncated;}
        std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_precockpit_observe;generations=%llu;sampled=%llu;records=%llu;lost=%llu;truncated-frames=%llu;"
            "rule=first-%u-then-1-per-5s;cap=%u-per-frame;srt=cpu-mapped-read(no-gpu-readback);commands=none",p6,ull(frames),ull(sampled),ull(records),
            ull(lost),ull(truncated),gpu_copy::precockpit::sampled_full,gpu_copy::precockpit::draws_per_frame);
        step5Log(st,s5,false);
    }
    if(worldObserveConfigured()) { // J-world-objects v2 step 1 (dedicated log only)
        gpu_copy::wobs::State::Totals t;
        {std::lock_guard lock(g_woMutex);t=g_wo.totals();}
        char p10[64];std::snprintf(p10,sizeof p10,"%s-world-observe",prefix);
        std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_world_observe;acm-draws=%llu;cpt-draws=%llu;other-draws=%llu;lists-executed=%llu;acm-lists=%llu;"
            "cockpit-lists=%llu;carrier-lists=%llu;windows=%llu;windows-with-carriers=%llu;sampled-windows=%llu;records=%llu;lost=%llu;truncated-windows=%llu;"
            "rule=first-%u-then-1-per-5s;caps=%u-lists,%u-draws,%u-srt-per-window;commands=none",p10,ull(t.acmDraws),ull(t.cptDraws),ull(t.otherDraws),ull(t.executed),
            ull(t.acmLists),ull(t.cockpitLists),ull(t.carrierLists),ull(t.windows),ull(t.windowsWithCarriers),ull(t.sampledWindows),ull(t.records),ull(t.lost),
            ull(t.truncated),gpu_copy::wobs::sampled_full,gpu_copy::wobs::list_lines,gpu_copy::wobs::draw_lines,gpu_copy::wobs::srt_lines);
        step5Log(st,s5,false);
        std::snprintf(st.text,sizeof st.text,"%s-activity;key=gpu_copy_step5_world_observe;acm-seen-with-frame-active=%llu(same-list:%llu,other-list:%llu);"
            "acm-seen-without-frame=%llu;list-recordings-evicted=%llu;reset-discarded=%llu;untracked-executes=%llu;draws-without-list-identity=%llu",p10,ull(t.acmActive),
            ull(t.acmActiveSameList),ull(t.acmActiveOtherList),ull(t.acmNoFrame),ull(t.evicted),ull(t.discarded),ull(t.untracked),ull(t.noList));
        step5Log(st,s5,false);
    }
    if(phaseObserveConfigured()) { // J-phase-observe (dedicated log only)
        gpu_copy::phobs::State::Totals t;
        {std::lock_guard lock(g_phMutex);t=g_ph.totals();}
        char p11[64];std::snprintf(p11,sizeof p11,"%s-phase-observe",prefix);
        std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_phase_observe;cockpit-frames=%llu;lists-closed=%llu;no-cockpit-lists=%llu;draws-seen=%llu;"
            "overflow-frames=%llu;distinct-signatures=%llu(table-full:%llu);distinct-psos=%llu(table-full:%llu);new-pso-draws=%llu;dumps=%llu(first:%llu,"
            "new-signature:%llu,new-pso:%llu,change:%llu,sample:%llu);suppressed=%llu;lost=%llu;records=%llu;discarded=%llu;evicted=%llu;commands=none",p11,
            ull(t.cockpitLists),ull(t.lists),ull(t.noCockpit),ull(t.draws),ull(t.overflowFrames),ull(t.distinctSigs),ull(t.sigFull),ull(t.distinctPsos),ull(t.psoFull),
            ull(t.newPsos),ull(t.dumps),ull(t.dumpReason[1]),ull(t.dumpReason[2]),ull(t.dumpReason[3]),ull(t.dumpReason[4]),ull(t.dumpReason[5]),ull(t.suppressed),
            ull(t.lost),ull(t.records),ull(t.discarded),ull(t.evicted));
        step5Log(st,s5,false);
        const auto& l=t.last;
        std::snprintf(st.text,sizeof st.text,"%s-last;key=gpu_copy_step5_phase_observe;frame=%llu;sig=%016llx;draws-per-phase=pre:%u,mem:%u,btw:%u,hud:%u,end:%u,tm:%u;"
            "late-tail-draws-on-scene-rt=%u(total-all-frames:%llu);unknown-rt-draws-all-frames=%llu;stored=%u;overflow=%u;region-end-ord=%u;tonemap-ord=%u",p11,
            ull(t.lastFrame),ull(l.sig),l.p[0].draws,l.p[1].draws,l.p[2].draws,l.p[3].draws,l.p[4].draws,l.p[5].draws,l.p[5].onScene,ull(t.tailSceneDraws),
            ull(t.unknownRtDraws),l.stored,l.overflow,l.regionEndOrd,l.tonemapOrd);
        step5Log(st,s5,false);
        std::snprintf(st.text,sizeof st.text,"%s-window;key=gpu_copy_step5_phase_observe;control-files=%%LOCALAPPDATA%%/acz-vr/phase-observe.arm,phase-observe.mark;windows-started=%llu/%u;"
            "windows-ended=%llu;active=%u(window:%u,bytes:%llu/%llu);arm-refused=%llu;marks=%llu;files-present-at-first-poll-ignored=%llu;window-frames=%llu;window-bytes-ended=%llu",p11,
            ull(t.windowsStarted),gpu_copy::phobs::window_cap,ull(t.windowsEnded),unsigned(t.windowActive),t.windowIndex,ull(t.windowBytes),ull(gpu_copy::phobs::window_byte_cap),
            ull(t.armRefused),ull(t.marks),ull(t.staleIgnored),ull(t.windowFrames),ull(t.windowBytesTotal));
        step5Log(st,s5,false);
    }
    if(pngOnceConfigured()) { // J-hud-colour (dedicated log only)
        char p7[64];std::snprintf(p7,sizeof p7,"%s-png",prefix);const auto& p=s5.png;
        std::snprintf(st.text,sizeof st.text,"%s;key=gpu_copy_step5_png_once;stage=%s;read=%u/%u;not-before=%llu;waited-frames=%llu;last-wait=%s;"
            "refusal=%s;failure=%s",p7,step5PngStageName(p.stage.load()),p.read,gpu_copy::png::samples,ull(p.notBefore),ull(p.waitedFrames),
            p.lastWait?p.lastWait:"none",p.refusal?p.refusal:"none",p.failWhy?p.failWhy:"none");
        if(p.fxCapable){ // J-effects-content (appended; absent without the effects layer)
            const size_t n=std::strlen(st.text);
            std::snprintf(st.text+n,sizeof st.text-n,";effects-bursts=%u/%u;threshold=%u;below-threshold-frames=%llu;mode=%s",p.burstRead,gpu_copy::png::burst_samples,p.thr,
                ull(p.burstBelow),p.burst?"burst":"base");
            const size_t n2=std::strlen(st.text); // burst protocol v2
            std::snprintf(st.text+n2,sizeof st.text-n2,";file-requests=%llu;file-bursts=%u;file-ignored=%llu",ull(p.fileRequests),p.fileBursts,ull(p.fileIgnored));
        }
        step5Log(st,s5,false);
    }
    if(!hudReplayConfigured())return;
    // J-hud-capture third line (dedicated log only).
    const uint64_t armed=s5.hudValidFrames+s5.hudInvalidFrames;
    char p3[64];std::snprintf(p3,sizeof p3,"%s-hud",prefix);
    std::snprintf(st.text,sizeof st.text,"%s;ready=%u;frames=valid:%llu,invalid:%llu,unarmed:%llu;draws=%llu;avg-draws=%.1f;refused=%llu(state:%llu,"
        "descriptor:%llu,format:%llu,cap:%llu);cap-frames=%llu;resets=%llu;depth-tested=%llu;end=rt-change:%llu,scene-barrier:%llu,close:%llu;"
        "last-refusal=%s;last-unarmed=%s",p3,unsigned(s5.hudReady),ull(s5.hudValidFrames),ull(s5.hudInvalidFrames),ull(s5.hudUnarmedFrames),
        ull(s5.hudDrawSum),armed?double(s5.hudDrawSum)/double(armed):0.0,ull(s5.hudRefusedSum),ull(s5.hudReason[0]),ull(s5.hudReason[1]),
        ull(s5.hudReason[2]),ull(s5.hudReason[3]),ull(s5.hudCapFrames),ull(s5.hudResets),ull(s5.hudDepthTested),ull(s5.hudEnd[0]),
        ull(s5.hudEnd[1]),ull(s5.hudEnd[2]),s5.hudLastWhy?s5.hudLastWhy:"none",s5.hudLastUnarmed?s5.hudLastUnarmed:(s5.hudReady?"none":s5.hudArmWhy));
    step5Log(st,s5,false);
    // HUD-RS-SKIP (its own short line, with the status cadence: the -hud line is near the 480-byte cap): only once a draw was skipped or the switch is off.
    if(const bool rsOn=gpu_copy::step5::hud_rs_skip_flag().load(std::memory_order_relaxed);s5.hudRsSkipSum||!rsOn) {
        std::snprintf(st.text,sizeof st.text,"%s-rs-skip;n=%llu;frames=%llu;ds-writers=%llu;on=%u;rule=rev-subtract-one-one-flare-not-replayed(frame-stays-valid;skipped-depth-writer-refuses-depth-tested)",
            p3,ull(s5.hudRsSkipSum),ull(s5.hudRsSkipFrames),ull(s5.hudRsDswSum),unsigned(rsOn));
        step5Log(st,s5,false);
    }
    if(gpu_copy::step5::hud_late_flag().load(std::memory_order_relaxed)) { // HUD-LATE (hudlate:1) status line (dedicated log only)
        gpu_copy::step5::format_hud_late(st.text,sizeof st.text,prefix,s5.hudLate);
        step5Log(st,s5,false);
    }
    if(hudRadarConfigured()) { // J-hud-radar status line (dedicated log only)
        char pr[64];std::snprintf(pr,sizeof pr,"%s-hud-radar",prefix);
        std::snprintf(st.text,sizeof st.text,"%s;frames=%llu(excursion:%llu,equal:%llu);excursions=%llu;returns=%llu;"
            "replayed=replace:%llu,equal:%llu(indices:%llu),late:%llu,windows:%llu;group-draws=avg:%.2f,max:%llu;max-equal=%llu;"
            "skipped=a3050:%llu,notequal:%llu,late:%llu,away:%llu;excursion-stencil-writers=%llu;stencil-gaps=%llu;refused=equal:%llu,writer-skipped:%llu;stencil-ref=untracked",
            pr,ull(s5.radarFrames),ull(s5.radarExcFrames),ull(s5.radarEqualFrames),
            ull(s5.radarExcursions),ull(s5.radarReturns),ull(s5.radarReplace),ull(s5.radarEqual),ull(s5.radarEqualIndices),ull(s5.radarLate),ull(s5.radarWindows),
            s5.radarFrames?double(s5.radarGroupSum)/double(s5.radarFrames):0.0,ull(s5.radarMaxGroup),ull(s5.radarMaxEqual),ull(s5.radarA3050),ull(s5.radarNotEqual),
            ull(s5.radarLateSkipped),ull(s5.radarAwaySkipped),ull(s5.radarExcWriters),ull(s5.radarGaps),ull(s5.radarEqualRefused),ull(s5.radarGapRefused));
        step5Log(st,s5,false);
        // RADAR-SCENE-RESUME: its own short line (the radar status line is already near the 480-byte feature-log limit).
        std::snprintf(st.text,sizeof st.text,"%s-resume;scene-leaves-away=%llu;resumes=%llu(frames:%llu);back-refused=%llu;rule=scene-barrier-while-away-then-barrier-back-into-RENDER_TARGET",
            pr,ull(s5.radarSceneLeaves),ull(s5.radarSceneResumes),ull(s5.radarResumedFrames),ull(s5.radarSceneBackRefused));
        step5Log(st,s5,false);
    }
    if(effectsConfigured()) { // J-effects-layer status line (dedicated log only): draws = committed (between members), tail = predicated off
        const uint64_t armedE=s5.effValidFrames+s5.effInvalidFrames+s5.effAbsentFrames;
        char p5[64];std::snprintf(p5,sizeof p5,"%s-hud-effects",prefix);
        std::snprintf(st.text,sizeof st.text,"%s;ready=%u;frames=valid:%llu,tail-only:%llu,absent:%llu,unarmed:%llu;draws=%llu;avg-draws=%.2f;tail=%llu;speculative=%llu;"
            "refused=%llu(state:%llu,descriptor:%llu,format:%llu,cap:%llu);const-blend=%llu;ignored=%llu;commits=%llu;epoch-cap-frames=%llu;last-refusal=%s;arm=%s",
            p5,unsigned(s5.effReady),ull(s5.effValidFrames),ull(s5.effInvalidFrames),ull(s5.effAbsentFrames),ull(s5.effUnarmedFrames),ull(s5.effDrawSum),
            armedE?double(s5.effDrawSum)/double(armedE):0.0,ull(s5.effTailSum),ull(s5.effSpeculative),ull(s5.effRefusedSum),ull(s5.effReason[0]),ull(s5.effReason[1]),
            ull(s5.effReason[2]),ull(s5.effReason[3]),ull(s5.effConstBlend),ull(s5.effIgnored),ull(s5.effCommits),ull(s5.effEpochCapFrames),
            s5.effLastWhy?s5.effLastWhy:"none",s5.effArmWhy?s5.effArmWhy:"none");
        {const size_t n=std::strlen(st.text);std::snprintf(st.text+n,sizeof st.text-n,";hud-ds-writers=%llu",ull(s5.hudDsWriters));} // J-effects-content
        step5Log(st,s5,false);
        { // J-effects-smoke-fix: mode and counters, then the candidate table (pixel-shader prefix: seen/replayed, A = allowlisted)
            std::snprintf(st.text,sizeof st.text,"%s-ps;pred-mode=%s(source=%s);allow=%u(source=%s,added=%u,bad=%u);class-ignored=%llu;pred-refused=%llu;uav-or-layout-refused=%llu;"
                "diag-lines=%llu(dropped=%llu);ps-table-overflow=%llu",p5,gpu_copy::step5::effects_pred_mode_name(s5.effPredMode),s5.effPredSource,s5.effAllow.count,
                s5.effAllowSource,s5.effAllowAdded,s5.effAllowBad,ull(s5.effClassIgnored),ull(s5.effPredRefused),ull(s5.effUavRefused),ull(s5.effDiagLinesTotal),
                ull(s5.effDiagDropped),ull(s5.effPs.overflow));
            step5Log(st,s5,false);
            if(s5.effGameCloudIgnored||gpu_copy::step5::effects_drop_game_clouds_flag().load(std::memory_order_relaxed)) { // EFFECTS-OUTSIDE gamecloud:0 (absent otherwise)
                std::snprintf(st.text,sizeof st.text,"%s-gamecloud;replay=%s;class=ps-%08x+depth-greater-equal;ignored=%llu;key=xr_terrain_stage(gamecloud:0|1)",p5,
                    gpu_copy::step5::effects_drop_game_clouds_flag().load(std::memory_order_relaxed)?"off":"on",gpu_copy::step5::effects_ps_cloud,ull(s5.effGameCloudIgnored));
                step5Log(st,s5,false);
            }
            if(s5.effDualRefused) { // (K) absent while nothing was refused as dual-blend-unsupported
                const uint8_t* bl=s5.effDualBlend;
                std::snprintf(st.text,sizeof st.text,"%s-dual-refused;count=%llu;last=ps%08x/vs%08x;blend=%u,%u,%u/%u,%u,%u;dual-ready=%u;tracer-ready=%u;depth=%s;%s=%u;topo=%u",p5,
                    ull(s5.effDualRefused),s5.effDualPs,s5.effDualVs,unsigned(bl[0]),unsigned(bl[1]),unsigned(bl[2]),unsigned(bl[3]),unsigned(bl[4]),unsigned(bl[5]),
                    unsigned(s5.effDualReady),unsigned(s5.effDualTracerReady),gpu_copy::phobs::func_name(s5.effDualDepth),s5.effDualIndexed?"idx":"vtx",s5.effDualCount,s5.effDualTopo);
                step5Log(st,s5,false);
            }
            if(gpu_copy::step5::effects_additive_flag().load(std::memory_order_relaxed)) { // (P) fxadd:1 only (absent otherwise: default byte-identical)
                gpu_copy::step5::format_additive_line(st.text,sizeof st.text,p5,s5.effAddAdmitted,s5.effAddPoints,s5.effAddPerEye,s5.effAddPtsPerEye,s5.effAddFrames,
                    s5.effAddShot.shots);
                step5Log(st,s5,false);
            }
            size_t n=0;char tab[400]="";
            for(unsigned i=0;i<s5.effPs.count&&n+48<sizeof tab;++i) {
                const auto& e=s5.effPs.e[i];
                n+=size_t(std::snprintf(tab+n,sizeof tab-n,"%s%08x:%s:vs%08x:%s:b%u:dw%u:%s:seen%llu:rep%llu",i?",":"",e.ps8,e.allowed?"A":"-",e.vs8,
                    gpu_copy::phobs::func_name(e.depthFunc),unsigned(e.blend),unsigned(e.depthWrite),e.indexed?"idx":"vtx",ull(e.seen),ull(e.replayed)));
            }
            std::snprintf(st.text,sizeof st.text,"%s-ps-candidates;entries=%u;list=%s",p5,s5.effPs.count,tab);
            step5Log(st,s5,false);
        }
    }
    if(!hudMarkersConfigured())return;
    // J-hud-markers status line (dedicated log only): avg-boxes is the per-frame marker count to compare with the targets in the mirror.
    const uint64_t split=s5.hudMarkerValidFrames+s5.hudMarkerInvalidFrames+s5.hudMarkerAbsentFrames;
    char p4[64];std::snprintf(p4,sizeof p4,"%s-hud-markers",prefix);
    std::snprintf(st.text,sizeof st.text,"%s;ready=%u;frames=valid:%llu,invalid:%llu,absent:%llu;draws=%llu(boxes:%llu,labels:%llu);avg-boxes=%.2f;avg-labels=%.2f;"
        "refused=%llu(state:%llu,descriptor:%llu,format:%llu,cap:%llu);last-refusal=%s;arm=%s",p4,unsigned(s5.hudMarkersReady),ull(s5.hudMarkerValidFrames),
        ull(s5.hudMarkerInvalidFrames),ull(s5.hudMarkerAbsentFrames),ull(s5.hudMarkerDrawSum),ull(s5.hudMarkerBoxSum),ull(s5.hudMarkerLabelSum),
        split?double(s5.hudMarkerBoxSum)/double(split):0.0,split?double(s5.hudMarkerLabelSum)/double(split):0.0,ull(s5.hudMarkerRefusedSum),
        ull(s5.hudMarkerReason[0]),ull(s5.hudMarkerReason[1]),ull(s5.hudMarkerReason[2]),ull(s5.hudMarkerReason[3]),
        s5.hudMarkerLastWhy?s5.hudMarkerLastWhy:"none",s5.hudMarkersArmWhy?s5.hudMarkersArmWhy:"none");
    step5Log(st,s5,false);
    if(!s5.tacFrames)return; // TACTICAL status (only once a tac frame closed: key off never prints it)
    std::snprintf(st.text,sizeof st.text,"%s-tac;frames=%llu;avg-objects=%.2f;avg-markers=%.2f;markers-matched=%llu;box-draws=%llu;label-draws=%llu;"
        "suppressed=boxes:%llu,labels:%llu;unreadable=boxes:%llu,labels:%llu(last=%s);wedge-frames=%llu;wedge-tail-draws=%llu;flags=%u;"
        "lock-shape=moved:%llu,diamond-not-moved:%llu;census-frames=%u(far-skipped:%llu);lock-dump-lines=%u",prefix,ull(s5.tacFrames),
        double(s5.tacObjSum)/double(s5.tacFrames),double(s5.tacMarkSum)/double(s5.tacFrames),ull(s5.tacMatchSum),ull(s5.tacBoxDraws),ull(s5.tacLabelDraws),
        ull(s5.tacSupBoxes),ull(s5.tacSupLabels),ull(s5.tacUnreadBox),ull(s5.tacUnreadLabel),s5.tacLastUnread?s5.tacLastUnread:"none",ull(s5.tacWedgeFrames),
        ull(s5.tacWedgeTail),s5.tacFlags,ull(s5.tacLockMoved),ull(s5.tacLockRejected),s5.tacCensusDone,ull(s5.tacCensusFar),s5.tacDump.lines);
    step5Log(st,s5,false);
    if(s5.tacFlags&acz::tac::kProducerLockShape) { // LOCK-WHY: why no diamond was moved (headset test 8)
        acz::tac::format_lock_why(st.text,sizeof st.text,prefix,s5.tacLockWhy);
        step5Log(st,s5,false);
    }
}
void step5EmitHudEvents(Step1State& st,Step5State& s5) noexcept { // worker, step-1 mutex held; dedicated log only
    using ull=unsigned long long;
    auto emit=[&](const char* outcome,uint64_t frame,const Step5Hud& h,const char* end) {
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-frame;outcome=%s;frame=%llu;draws=%u;refused=%u(state:%u,descriptor:%u,format:%u,"
            "cap:%u);first-refusal=%s;commands=%u;resets=%u;depth-tested=%u;no-target=%u;end=%s;scene=%llx;format=%u;hud-valid=%u;premultiplied=1",
            outcome,ull(frame),h.draws,h.refused,h.reason[0],h.reason[1],h.reason[2],h.reason[3],h.firstWhy?h.firstWhy:"none",h.commands,h.resets,
            h.depthTested,h.noTarget,end?end:"close",ull(h.scene),s5.hudFormat,unsigned(gpu_copy::step5::hud_frame_valid(h.armed,h.draws,h.refused)));
        step5Log(st,s5,false);
    };
    if(s5.radarDiagPending&&hudRadarConfigured()) { // J-hud-radar: the hud-end diagnostic (3 short lines: the feature log truncates a line at 480 bytes)
        s5.radarDiagPending=false;const auto& g=s5.radarDiag;const auto& r=s5.radarDiagRd;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-end;frame=%llu;end=%s;first-excursion=%s;ordinal=%llu;rt=%llx,%llux%u,fmt=%u,%s;om-count=%u;"
            "excursions=%u;returns=%u;draws-seen=%u;post-excursion-draws=%u;replayed=replace:%u,equal:%u(indices:%u),late:%u;skipped=a3050:%u,notequal:%u,late:%u,away:%u;"
            "excursion-stencil-writers=%u;stencil-gaps=%u;stencil-ref=untracked",
            ull(s5.radarDiagFrame),s5.radarDiagEnd?s5.radarDiagEnd:"close",g.haveEnd?g.endWhy:"none",ull(g.endOrdinal),ull(g.endRt),ull(g.endWidth),g.endHeight,g.endFormat,
            g.endStatus?g.endStatus:"none",g.endOmCount,r.excursions,r.returns,g.seen,g.post,r.replaceReplayed,r.equalReplayed,r.equalIndices,r.lateReplayed,r.a3050,
            r.notEqualSkipped,r.lateSkipped,r.awaySkipped,r.excursionWriters,r.stencilGaps);
        step5Log(st,s5,false);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-end-resume;frame=%llu;scene-leaves-away=%u;scene-resumes=%u;scene-back-refused=%u", // RADAR-SCENE-RESUME
            ull(s5.radarDiagFrame),r.sceneLeaves,r.sceneResumes,r.sceneBackRefused);
        step5Log(st,s5,false);
        size_t n=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-end-verdicts;frame=%llu;rule=original-hud_decide-on-every-draw-after-the-first-excursion;verdicts=",ull(s5.radarDiagFrame));
        for(unsigned i=0;i<gpu_copy::step5::radar_diag_verdicts&&g.verdictWhy[i]&&n<sizeof st.text-64;++i)
            n+=size_t(std::snprintf(st.text+n,sizeof st.text-n,"%s%s:%u",i?",":"",g.verdictWhy[i],g.verdictCount[i]));
        if(n<sizeof st.text-32)std::snprintf(st.text+n,sizeof st.text-n,";other=%u",g.verdictOther);
        step5Log(st,s5,false);
        char fn[gpu_copy::step5::radar_diag_draws+1],dp[gpu_copy::step5::radar_diag_draws+1];const unsigned shown=g.ordered(fn,dp);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-end-draws;frame=%llu;last=%u/%u;stencil-func=%s;disposition=%s",
            ull(s5.radarDiagFrame),shown,g.seen,fn,dp);
        step5Log(st,s5,false);
    }
    if(s5.hudFirstPending&&!s5.hudFirstEmitted){s5.hudFirstPending=false;s5.hudFirstEmitted=true;emit("first-valid",s5.hudFirstFrame,s5.hudFirst,s5.hudFirstEnd);}
    if(s5.hudRefusalPending&&!s5.hudRefusalEmitted){s5.hudRefusalPending=false;s5.hudRefusalEmitted=true;
        emit("first-refused",0,s5.hudRefusal,s5.hudRefusal.endWhy);}
    if(effectsConfigured()) { // J-effects-layer (first effects-valid / first refused-candidate frame)
        auto eemit=[&](const char* outcome,uint64_t frame,const Step5Eff& e) {
            const auto& p=e.ep;
            std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=effects-frame;outcome=%s;frame=%llu;draws=%u;refused=%u(state:%u,descriptor:%u,format:%u,cap:%u);"
                "const-blend=%u;tail=%u;speculative=%u;epochs=%u/%u;commits=%u;ignored=%u;not-scene=%u;commands=%u;first-refusal=%s;effects-valid=%u;premultiplied=1;predicated=1",
                outcome,ull(frame),p.draws,p.refused,p.reason[0],p.reason[1],p.reason[2],p.reason[3],p.constBlend,p.tailDraws,p.speculative,p.used,
                gpu_copy::step5::effects_epoch_cap,p.commits,e.ignored,e.notScene,e.commands,e.firstWhy?e.firstWhy:"none",unsigned(gpu_copy::step5::effects_frame_valid(e.armed,p.draws)));
            step5Log(st,s5,false);
        };
        if(s5.effFirstPending&&!s5.effFirstEmitted){s5.effFirstPending=false;s5.effFirstEmitted=true;eemit("first-valid",s5.effFirstFrame,s5.effFirst);}
        // J-effects-smoke-fix: one line per logged replay (<= 8 per frame): predicate offset and value at Close, the shader pair, depth func and blend tuple.
        for(unsigned n=0;n<8&&s5.effDiagCount;++n) {
            const auto& q=s5.effDiagQ[s5.effDiagHead];const auto& d=q.d;
            const bool exec=gpu_copy::step5::effects_pred_executes(int(q.mode),d.word);
            std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=effects-replay;frame=%llu;set=%s;epoch=%d;pred-offset=%llu;pred-word-at-close=%llu;pred-op=%s;"
                "gpu-executes=%u;phase=%s;ord=%u;vs8=%08x;ps8=%08x;depth=%s;blend=%u,%u,%u/%u,%u,%u;indexed=%u;count=%u;instances=%u;allowlisted=%u",
                ull(q.frame),q.burst?"burst":"early",d.epoch,ull(d.offset),ull(d.word),gpu_copy::step5::effects_pred_mode_name(int(q.mode)),unsigned(exec),
                d.committed?"btw(committed)":"tail",d.ord,d.vs8,d.ps8,gpu_copy::phobs::func_name(d.depthFunc),d.blend[0],d.blend[1],d.blend[2],d.blend[3],d.blend[4],d.blend[5],
                unsigned(d.indexed),d.count,d.instances,unsigned(s5.effAllow.has(d.ps8)));
            step5Log(st,s5,false);
            s5.effDiagHead=(s5.effDiagHead+1)%Step5State::effDiagCap;--s5.effDiagCount;
        }
        if(s5.effRefusalPending&&!s5.effRefusalEmitted){s5.effRefusalPending=false;s5.effRefusalEmitted=true;eemit("first-refused",0,s5.effRefusal);}
        if(s5.fxPending) { // EFFECTS-OUTSIDE (C) fxmem:1: the rate-limited fx-smoke-frame line (< 480 bytes, offline-tested) + the run totals (short line)
            s5.fxPending=false;++s5.fxLines;
            const int k=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
            if(k>0&&size_t(k)<sizeof st.text)acz::fxsmoke::format_summary(st.text+k,sizeof st.text-size_t(k),s5.fxPendFrame,s5.fxPend,s5.fxPendSkipped);
            step5Log(st,s5,false);
            std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=fx-smoke-totals;smoke-frames=%llu;draws=%llu;stored=%llu;unread=%llu(last=%s);lines=%llu;"
                "store-cap=%u;mode=capture-log-only(no-replay);over-cap=%llu;peak-draws=%u",ull(s5.fxFrames),ull(s5.fxDraws),ull(s5.fxStored),ull(s5.fxUnread),
                s5.fxLastUnread?s5.fxLastUnread:"none",ull(s5.fxLines),acz::fxsmoke::FrameCapture::kCap,ull(s5.fxOverCap),s5.fxPeakDraws);
            step5Log(st,s5,false);
            if(s5.fxDryPending) { // fxmem:2: the same frame's per-eye replay plan (CPU dry run) + its run totals
                s5.fxDryPending=false;
                const int k2=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
                if(k2>0&&size_t(k2)<sizeof st.text)acz::fxsmoke::format_dry_run(st.text+k2,sizeof st.text-size_t(k2),s5.fxPendFrame,s5.fxDryPend);
                step5Log(st,s5,false);
                std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=fx-smoke-replay-totals;prepared=%llu;refused=%llu;store-cap=%u;store-bytes=%u;"
                    "mode=dry-run(cpu-only,no-gpu-replay)",ull(s5.fxDryPrepared),ull(s5.fxDryRefused),acz::fxsmoke::ReplayStore::kCap,
                    unsigned(acz::fxsmoke::ReplayStore::kBytes));
                step5Log(st,s5,false);
            }
            if(s5.fxGpuPending) { // fxmem:4: the same frame's GPU replay facts + the run totals
                s5.fxGpuPending=false;
                const int k3=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
                if(k3>0&&size_t(k3)<sizeof st.text)acz::fxsmoke::gpu::format_frame(st.text+k3,sizeof st.text-size_t(k3),s5.fxPendFrame,s5.fxGpuPend,s5.fxGpuPendFallback);
                step5Log(st,s5,false);
                std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=fx-smoke-gpu-totals;frames=%llu;valid-frames=%llu;replays=%llu;committed=%llu;refused=%llu;"
                    "window-fallback=%llu;last-why=%.32s",ull(s5.smokeFrames),ull(s5.smokeValidFrames),ull(s5.smokeReplays),ull(s5.smokeCommitted),ull(s5.smokeRefused),
                    ull(s5.smokeFallback),s5.smokeLastWhy?s5.smokeLastWhy:"none");
                step5Log(st,s5,false);
            }
        }
        if(s5.fxCen.onset_count()||s5.fxCenWindow)step5FxCensusEmit(st,s5); // EFFECTS-OUTSIDE (D) fxmem:3 (nothing pending: skipped)
        if(s5.diagQ.count)step5FxDiagEmit(st,s5); // READ-ONLY flare / aircraft diagnostic lines
        if(s5.fxSigQ.count)step5FxSigEmit(st,s5); // EFFECTS-OUTSIDE (D) fxsig: queued per-draw lines
        if(s5.fxSprPending[0]||s5.fxSprPending[1]||s5.fxSprPending[2]||s5.spriteGpuPending||s5.fxPuffPending)step5FxSpriteEmit(st,s5); // EFFECTS-OUTSIDE (E) fxmem:5 (+ (F) fxmem:6; WORLD-CLOUDS wcdiag)
        step5FxTrailTick(st,s5); // EFFECTS-OUTSIDE (M2) fxtrail: one fx-puff-trail line per second while the option is set (unconditional)
    }
    else if(s5.diagQ.count)step5FxDiagEmit(st,s5); // MSL-HUD lines (summary-msl-hud, msl-hud events/dumps) also without the effects layer
    if(!hudMarkersConfigured())return;
    auto memit=[&](const char* outcome,uint64_t frame,const Step5Hud& h) { // J-hud-markers (first marker-valid / first marker-refused frame)
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hud-markers-frame;outcome=%s;frame=%llu;marker-draws=%u(boxes:%u,labels:%u);marker-refused=%u(state:%u,"
            "descriptor:%u,format:%u,cap:%u);first-refusal=%s;instrument-draws=%u;instrument-refused=%u;markers-valid=%u;premultiplied=1",
            outcome,ull(frame),h.mDraws,h.mBoxes,h.mLabels,h.mRefused,h.mReason[0],h.mReason[1],h.mReason[2],h.mReason[3],h.mFirstWhy?h.mFirstWhy:"none",
            h.draws,h.refused,unsigned(h.mDraws>0&&h.mRefused==0));
        step5Log(st,s5,false);
    };
    if(s5.hudMarkerFirstPending&&!s5.hudMarkerFirstEmitted){s5.hudMarkerFirstPending=false;s5.hudMarkerFirstEmitted=true;memit("first-valid",s5.hudMarkerFirstFrame,s5.hudMarkerFirst);}
    if(s5.hudMarkerRefusalPending&&!s5.hudMarkerRefusalEmitted){s5.hudMarkerRefusalPending=false;s5.hudMarkerRefusalEmitted=true;memit("first-refused",0,s5.hudMarkerRefusal);}
}
// J-cockpit-fov F1 events (worker, step-1 mutex held): the first frame published with the HMD FOV
// and the first posed frame that fell back to the game lens (mirrored: one line each per run).
void step5EmitFovEvents(Step1State& st,Step5State& s5) noexcept {
    using ull=unsigned long long;
    if(s5.fovFirstPending&&!s5.fovFirstEmitted) {
        s5.fovFirstPending=false;s5.fovFirstEmitted=true;const auto& v=s5.fovFirst;const auto& l=s5.fovFirstLens;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hmd-fov;outcome=applied;frame=%llu;why=%s;eye0=%.5f,%.5f,%.5f,%.5f;eye1=%.5f,%.5f,%.5f,%.5f;"
            "units=radians(left,right,up,down);reference=game-lens(%.5f,%.5f,%.5f,%.5f);rt=%ux%u;stereo-frame=cockpit_fov_valid=1",ull(s5.fovFirstFrame),
            s5.fovFirstWhy?s5.fovFirstWhy:"hmd",double(v[0][0]),double(v[0][1]),double(v[0][2]),double(v[0][3]),double(v[1][0]),double(v[1][1]),
            double(v[1][2]),double(v[1][3]),double(l[0]),double(l[1]),double(l[2]),double(l[3]),s5.ringW,s5.ringH);
        step5Log(st,s5);
    }
    if(s5.fovFallbackPending&&!s5.fovFallbackEmitted) {
        s5.fovFallbackPending=false;s5.fovFallbackEmitted=true;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=hmd-fov;outcome=fallback;reason=%s;eyes=game-lens;stereo-frame=cockpit_fov_valid=0",
            s5.fovLastWhy?s5.fovLastWhy:"?");
        step5Log(st,s5);
    }
}
// J-cockpit-fov F3: each family class admitted at run time, once (worker, step-1 mutex held).
void step5EmitFamily(Step1State& st,Step5State& s5) noexcept {
    using ull=unsigned long long;
    while(s5.familyLogged<s5.familyCount) {
        const auto& a=s5.family[s5.familyLogged++];const auto& c=a.c;
        const Step5Class& n=a.nearest<s5.classCount?s5.classes[a.nearest]:c;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=class-family-admit;pso=%llx;ordinal=%llu;frame=%llu;family-of=c%u;root=%llx;topo=%u;ib=%u;vb=%u;"
            "t10=%u;rtv=%u;dsv=%u;depth=%u/%u/%u(selected:%u/%u/%u);cut=%x(selected:%x);rule=equal-root,ia,t10,formats,samples,stencil;depth-and-cut-may-differ",
            ull(a.pso),ull(a.ordinal),ull(a.frame),a.nearest,ull(c.root),c.topology,c.ib,c.vb,c.t10,c.rtv,c.dsv,unsigned(c.depthEnable),unsigned(c.depthWrite),
            c.depthFunc,unsigned(n.depthEnable),unsigned(n.depthWrite),n.depthFunc,c.cut,n.cut);
        step5Log(st,s5);
    }
}
void step5EmitMismatch(Step1State& st,Step5State& s5,bool mirror) noexcept { // step-1 mutex held
    const auto& d=s5.mismatchOut;using ull=unsigned long long;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=continuous-mismatch;n=%llu;of=%llu;outcome=%s;what=%s;ordinal=%llu;list=%llx;gen=%llx;"
        "pso=sel:%llx,live:%llx;args=sel:%s%u,%u,%u,%d,%u,live:%s%u,%u,%u,%d,%u;members=frame:%u,selected:%u;"
        "sig=root:%llx,topo:%u,ib:%u,vb:%u,t10:%u",ull(++s5.mismatchLines),ull(s5.mismatchEvents),d.produced?"produced":"skipped",d.what?d.what:"?",ull(d.ordinal),
        ull(d.list),ull(d.gen),ull(d.psoSel),ull(d.psoLive),d.selFound?(d.selIndexed?"i":"n"):"none:",d.selArgs[0],d.selArgs[1],d.selArgs[2],
        int(d.selArgs[3]),d.selArgs[4],d.liveIndexed?"i":"n",d.liveArgs[0],d.liveArgs[1],d.liveArgs[2],int(d.liveArgs[3]),d.liveArgs[4],
        d.frameMembers,d.selected,ull(d.root),d.topology,d.ib,d.vb,d.t10);
    step5Log(st,s5,mirror);
    s5.mismatchOut=Step5Detail{};
}
// ------------------------------------------------------------- J-hud-colour: png_once (worker)
const char* step5PngStageName(int g) noexcept {
    static constexpr const char* n[]={"off","armed","recorded","submitted","ready","processed","done","failed"};return g>=0&&g<8?n[g]:"?";
}
// Burst protocol v2 (worker, NO lock held, every call of the tick): polls %LOCALAPPDATA%\acz-vr\png-burst.now every 200 ms (fixtures: every call).
// The first poll only records the stamp (a file left over from an earlier session is not an order); afterwards any change of the stamp (creation,
// rewrite, touch of last-write time) with the file present is ONE request; a deletion is not. Nothing is ever written or deleted by the producer.
void step5PngPoll(Step5State& s5) noexcept {
    if(!pngOnceConfigured())return;
    auto& p=s5.png;
    if(p.stage.load()==png_off||p.stage.load()==png_failed||!p.fxCapable||p.refusal)return;
    const uint64_t now=GetTickCount64();
#ifndef ACZ_VR_OFFLINE_TEST
    if(p.filePollTick&&now-p.filePollTick<200)return;
#endif
    p.filePollTick=now?now:1;
    const uint64_t stamp=phFileStamp(L"png-burst.now");
    char line[acz::feature_log::line_cap];using ull=unsigned long long;
    if(!p.fileBaselined) {
        p.fileBaselined=true;p.fileStamp=stamp;
        std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-burst-config;file-trigger=%%LOCALAPPDATA%%\\acz-vr\\png-burst.now;rule=create-or-rewrite-the-file-after-the-game-is-in-the-cockpit"
            "(present-at-the-first-poll-is-ignored);poll-ms=200;effect=next-valid-frame-takes-one-burst-set;sets-shared-with-threshold-trigger=max-%u;"
            "burst-outputs=world,eye0,hud-over-world,effects,effects-on-black,effects-alpha-raw,effects-nonzero,scene-end(file-triggered-sets);file-present-at-first-poll=%u",
            gpu_copy::png::burst_samples,unsigned(stamp!=0));
        acz::feature_log::write(s5.log,line);return;
    }
    if(stamp==p.fileStamp)return;
    p.fileStamp=stamp;
    if(!stamp)return;
    ++p.fileRequests;
    const int g=p.stage.load();
    std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-burst-request;request=%llu;tick=%llu;stage=%s;mode=%s;bursts-read=%u/%u;status=%s",ull(p.fileRequests),ull(now),
        step5PngStageName(g),p.burst?"burst":"base",p.burstRead,gpu_copy::png::burst_samples,
        p.burstRead>=gpu_copy::png::burst_samples||g==png_done?"ignored-all-burst-sets-taken":(p.burst?"pending-next-valid-frame":"pending-until-the-base-samples-are-read"));
    acz::feature_log::write(s5.log,line);
    if(p.burstRead>=gpu_copy::png::burst_samples||g==png_done){++p.fileIgnored;return;}
    p.fileReqTick.store(now);p.fileTrig.fetch_add(1);
    g_step5SceneEndWanted.store(true); // J-scene-end: the next frames copy the scene target at their HUD region end
}
// Worker, NO lock held: a READY sample (its copies completed on the private fence, observed under the
// step-1 mutex) is mapped once, its statistics logged and its PNG files written; then `processed`.
// The game thread never touches the readback or these fields while the stage is ready.
void step5PngWork(Step1State& st,Step5State& s5) noexcept {
    if(!pngOnceConfigured())return;
    auto& p=s5.png;if(p.stage.load()!=png_ready)return;
    namespace png=gpu_copy::png;using ull=unsigned long long;
    char line[acz::feature_log::line_cap];
    const unsigned sample=p.read+p.burstRead+1; // base samples 1..2, burst samples 3.. (burstRead stays 0 without the effects layer)
    using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
    using UnmapFn=void(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*);
    const D3D12_RANGE range{0,static_cast<SIZE_T>(p.bytes)},none{0,0};void* data=nullptr;
    const HRESULT hr=reinterpret_cast<MapFn>(step1Native(p.readback,8))(p.readback,0,&range,&data);
    if(FAILED(hr)||!data) {
        std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-once;outcome=failed;sample=%u;reason=map-failed;hr=%08lx",sample,static_cast<unsigned long>(hr));
        acz::feature_log::write(s5.log,line);p.failWhy="map-failed";p.emitFail=true;p.stage.store(png_failed);return;
    }
    const uint8_t* base=static_cast<const uint8_t*>(data);
    // J-effects-content: fx = the effects image was copied (4th footprint). Burst protocol v2: a burst set writes world, eye0, hud-over-world and (fx) the 4 effects
    // outputs (7 files, ~15 MB each stored); a base sample writes all of its outputs.
    const bool hasSe=p.burst&&p.se; // J-scene-end: footprint 4 holds the scene target at the HUD region end (file-triggered burst sets only)
    const unsigned nimg=hasSe?png::image_count_se:p.fx?png::image_count_fx:png::image_count;
    char label[16];if(p.burst)std::snprintf(label,sizeof label,"b%u",p.burstRead+1);else std::snprintf(label,sizeof label,"s%u",sample);
    unsigned outs[png::out_count_se]{},nouts=0;
    if(p.burst) {
        outs[nouts++]=png::out_world;if(hasSe)outs[nouts++]=png::out_scene_end;outs[nouts++]=png::out_eye0;outs[nouts++]=png::out_hud_over_world;
        if(p.fx)for(unsigned o=png::out_effects;o<png::out_count_fx;++o)outs[nouts++]=o;
    } else for(unsigned o=0;o<(p.fx?unsigned(png::out_count_fx):unsigned(png::out_count));++o)outs[nouts++]=o;
    const uint8_t* src[png::image_count_se]{};for(unsigned i=0;i<nimg;++i)src[i]=base+uint64_t(i)*p.stride;
    png::FxStats fxs;if(p.fx)png::fx_stats(src[png::image_effects],p.w,p.h,p.pitch,fxs);
    png::Stats imgStats[png::image_count_se]{};
    for(unsigned i=0;i<nimg;++i) {
        if(i==png::image_effects&&!p.fx)continue; // (footprint 3 holds nothing of this frame when the effects image was not copied)
        png::Stats& s=imgStats[i];png::stats(src[i],p.w,p.h,p.pitch,s);
        const double t=s.texels?double(s.texels):1.0;
        std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-stats;sample=%u;image=%s;size=%ux%u;format=10(R16G16B16A16_FLOAT,RGBA);texels=%llu;"
            "nonfinite=%llu;r=%.4g/%.4g/%.4g;g=%.4g/%.4g/%.4g;b=%.4g/%.4g/%.4g;a=%.4g/%.4g/%.4g(min/max/mean,finite);alpha-gt0=%.5f;rgb-gt0=%.5f",
            sample,png::image_name(i),p.w,p.h,ull(s.texels),ull(s.nonfinite),s.min[0],s.max[0],s.mean[0],s.min[1],s.max[1],s.mean[1],
            s.min[2],s.max[2],s.mean[2],s.min[3],s.max[3],s.mean[3],double(s.alpha_positive)/t,double(s.lit)/t);
        acz::feature_log::write(s5.log,line);
    }
    std::wstring dir=step3OutputDir();
    char dirUtf8[200]="unavailable";
    if(!dir.empty()) {
        try {dir+=L"\\png";} catch(...) {dir.clear();}
        if(!dir.empty()){CreateDirectoryW(dir.c_str(),nullptr); // an existing directory is fine
            if(WideCharToMultiByte(CP_UTF8,0,dir.c_str(),-1,dirUtf8,int(sizeof dirUtf8),nullptr,nullptr)<=0)std::snprintf(dirUtf8,sizeof dirUtf8,"unprintable");}
    }
    if(p.fx) { // J-effects-content: the effects target's coverage (one line; also inside the read line below)
        const double tx=fxs.texels?double(fxs.texels):1.0;
        std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-effects;sample=%u;kind=%s;committed-draws=%u;threshold=%u;texels=%llu;nonzero=%llu(%.5f);"
            "alpha-gt0=%.5f;rgb-gt0=%.5f;alpha-max=%.4g;alpha-mean=%.6g;rgb-max=%.4g;nonfinite=%llu;bbox=%d,%d,%d,%d(x0,y0,x1,y1);premultiplied=1",
            sample,p.burst?"burst":"base",p.fxDraws,p.thr,ull(fxs.texels),ull(fxs.nonzero),double(fxs.nonzero)/tx,double(fxs.alpha_positive)/tx,
            double(fxs.rgb_positive)/tx,fxs.alpha_max,fxs.alpha_mean,fxs.rgb_max,ull(fxs.nonfinite),fxs.x0,fxs.y0,fxs.x1,fxs.y1);
        acz::feature_log::write(s5.log,line);
    }
    png::DiffStats seDs;bool haveSeDs=false;
    if(hasSe) { // J-scene-end: the one line that answers the question (scene target at the HUD region end vs the world snapshot)
        const auto& ss=imgStats[png::image_scene_end];const auto& sw=imgStats[png::image_world];const double t=ss.texels?double(ss.texels):1.0;
        png::DiffStats ds;png::diff_stats(src[png::image_scene_end],src[png::image_world],p.w,p.h,p.pitch,0.004f,ds);seDs=ds;haveSeDs=true;
        std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-scene-end;sample=%u;region-end=%s;copies=%u;declined=%llu;scene-end-nonblack=%llu(%.5f);scene-end-max=%.4g;"
            "scene-end-mean-rgb=%.4g/%.4g/%.4g;world-nonblack=%llu(%.5f);changed-vs-world=%llu(%.5f);changed-bbox=%d,%d,%d,%d(x0,y0,x1,y1);eps=0.004",
            sample,p.seWhy?p.seWhy:"?",p.seCopies,ull(p.seDeclined),ull(ss.lit),double(ss.lit)/t,std::fmax(ss.max[0],std::fmax(ss.max[1],ss.max[2])),
            ss.mean[0],ss.mean[1],ss.mean[2],ull(sw.lit),double(sw.lit)/t,ull(ds.changed),double(ds.changed)/t,ds.x0,ds.y0,ds.x1,ds.y1);
        acz::feature_log::write(s5.log,line);
    }
    if(p.fx) { // J-effects-smoke-fix: does the effects target hold the smoke (changed region world vs scene-end) and/or the HUD tail (bbox of the HUD image)?
        png::FxStats hudBox;png::fx_stats(src[png::image_hud],p.w,p.h,p.pitch,hudBox);
        png::RegionStats rs,rh;
        const bool smokeBox=haveSeDs&&seDs.changed>0;
        if(smokeBox)png::region_stats(src[png::image_effects],p.w,p.h,p.pitch,seDs.x0,seDs.y0,seDs.x1,seDs.y1,rs);
        if(hudBox.nonzero>0)png::region_stats(src[png::image_effects],p.w,p.h,p.pitch,hudBox.x0,hudBox.y0,hudBox.x1,hudBox.y1,rh);
        char sm[160]="smoke-bbox=none(no-scene-end-or-no-change)",hb[160]="hud-bbox=none(hud-image-empty)";
        if(smokeBox)std::snprintf(sm,sizeof sm,"smoke-bbox=%d,%d,%d,%d(world-vs-scene-end-changed:%llu);effects-in-smoke-bbox=nonzero:%llu/%llu,alpha-max:%.4g,rgb-max:%.4g",
            seDs.x0,seDs.y0,seDs.x1,seDs.y1,ull(seDs.changed),ull(rs.nonzero),ull(rs.area),rs.alpha_max,rs.rgb_max);
        if(hudBox.nonzero>0)std::snprintf(hb,sizeof hb,"hud-bbox=%d,%d,%d,%d(hud-nonzero:%llu);effects-in-hud-bbox=nonzero:%llu/%llu,alpha-max:%.4g,rgb-max:%.4g",
            hudBox.x0,hudBox.y0,hudBox.x1,hudBox.y1,ull(hudBox.nonzero),ull(rh.nonzero),ull(rh.area),rh.alpha_max,rh.rgb_max);
        std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-effects-tails;sample=%u;kind=%s;effects-nonzero=%llu;%s;%s;"
            "reading=smoke-bbox-nonzero>0:smoke-drawn;hud-bbox-nonzero>0:HUD-tail-executed(predication-leak)",sample,p.burst?"burst":"base",ull(fxs.nonzero),sm,hb);
        acz::feature_log::write(s5.log,line);
    }
    unsigned written=0;
    for(unsigned oi=0;oi<nouts;++oi) {
        const unsigned o=outs[oi];
        char name[128];std::snprintf(name,sizeof name,"acz-vr-png-%lu-%llu-%s-%s.png",GetCurrentProcessId(),ull(st.tStart),label,png::output_name(o));
        std::vector<uint8_t> rgba,file;bool ok=!dir.empty()&&png::build(src,p.w,p.h,p.pitch,o,rgba,nimg)&&
            gpu_copy::step3::png_encode(rgba.data(),p.w,p.h,uint64_t(p.w)*4,false,file);
        if(ok) {
            std::wstring path;
            try {path=dir+L"\\";for(const char* c=name;*c;++c)path.push_back(wchar_t(*c));ok=step3WriteFile(path,file);} catch(...) {ok=false;}
        }
        written+=ok;
        std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-file;sample=%u;output=%s;file=%s;ok=%u;bytes=%llu;png=RGBA8,row0=top",sample,
            png::output_name(o),name,unsigned(ok),ull(ok?file.size():0));
        acz::feature_log::write(s5.log,line);
    }
    reinterpret_cast<UnmapFn>(step1Native(p.readback,9))(p.readback,0,&none);
    if(p.burst) { // burst protocol v2: one `png-burst` line per set (trigger reason + frame statistics) and the phase observer's view of the same frame
        const auto& sw=imgStats[png::image_world];const auto& se=imgStats[png::image_eye0];const auto& sh=imgStats[png::image_hud];
        const double t=sw.texels?double(sw.texels):1.0;
        char fxNz[24]="n/a",lat[24]="n/a";
        if(p.fx)std::snprintf(fxNz,sizeof fxNz,"%llu",ull(fxs.nonzero));
        if(p.burstFile&&p.reqTickAtCapture&&p.captureTick>=p.reqTickAtCapture)std::snprintf(lat,sizeof lat,"%llu",ull(p.captureTick-p.reqTickAtCapture));
        std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-burst;sample=%u;set=%u/%u;reason=%s;file-requests=%llu;request-to-capture-ms=%s;published-frame=%llu;slot=%u;"
            "members=%u;hud-draws=%u;committed-draws=%u;effects-image=%u;world-nonblack=%llu(%.5f);world-max=%.4g;world-mean-rgb=%.4g/%.4g/%.4g;eye0-nonblack=%llu(%.5f);"
            "hud-nonblack=%llu(%.5f);effects-nonzero=%s;scene-end=%u;files=%u/%u",
            sample,p.burstRead+1,png::burst_samples,p.burstFile?"file":"threshold",ull(p.fileRequests),lat,ull(p.publishedAt),p.slot,p.members,p.hudDraws,p.fxDraws,
            unsigned(p.fx),ull(sw.lit),double(sw.lit)/t,std::fmax(sw.max[0],std::fmax(sw.max[1],sw.max[2])),sw.mean[0],sw.mean[1],sw.mean[2],ull(se.lit),double(se.lit)/t,
            ull(sh.lit),double(sh.lit)/t,fxNz,unsigned(hasSe),written,nouts);
        acz::feature_log::write(s5.log,line);
        namespace ph=gpu_copy::phobs;
        if(!p.phHave) {
            std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-burst-phases;sample=%u;observer=none(needs-gpu_copy_step5_phase_observe=true-and-a-cockpit-frame-summary)",sample);
            acz::feature_log::write(s5.log,line);
        } else {
            const auto& s=p.ph;char d[160]="",ix[160]="",on[160]="",nw[160]="";size_t a=0,b=0,c=0,e=0;
            for(unsigned i=0;i<ph::phase_count;++i) {
                a+=size_t(std::snprintf(d+a,sizeof d-a,"%s%s:%u",i?",":"",ph::phase_short(i),s.p[i].draws));
                b+=size_t(std::snprintf(ix+b,sizeof ix-b,"%s%s:%u",i?",":"",ph::phase_short(i),s.p[i].indexed));
                c+=size_t(std::snprintf(on+c,sizeof on-c,"%s%s:%u",i?",":"",ph::phase_short(i),s.p[i].onScene));
                e+=size_t(std::snprintf(nw+e,sizeof nw-e,"%s%s:%u",i?",":"",ph::phase_short(i),s.p[i].newPso));
            }
            std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-burst-phases;sample=%u;observer-frame=%llu;draws-per-phase=%s;indexed=%s;on-scene-rt=%s;new-pso=%s;"
                "first-member-ord=%u;last-member-ord=%u;region-end-ord=%u;tonemap-ord=%u;total=%u;stored=%u;overflow=%u;sig=%016llx;"
                "pre=inside-the-world-snapshot;btw=after-the-first-member-and-before-the-hud-region",sample,ull(p.phFrame),d,ix,on,nw,s.firstMember,s.lastMember,s.regionEndOrd,
                s.tonemapOrd,s.total,s.stored,s.overflow,ull(s.sig));
            acz::feature_log::write(s5.log,line);
            char is[200]="",vs[200]="",sh2[200]="";size_t f1=0,f2=0,f3=0;
            for(unsigned i=0;i<ph::phase_count;++i) {
                f1+=size_t(std::snprintf(is+f1,sizeof is-f1,"%s%s:%llu",i?",":"",ph::phase_short(i),ull(s.p[i].idx)));
                f2+=size_t(std::snprintf(vs+f2,sizeof vs-f2,"%s%s:%llu",i?",":"",ph::phase_short(i),ull(s.p[i].vtx)));
                f3+=size_t(std::snprintf(sh2+f3,sizeof sh2-f3,"%s%s:%016llx",i?",":"",ph::phase_short(i),ull(s.p[i].shape)));
            }
            std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-burst-phase-shapes;sample=%u;observer-frame=%llu;index-sum=%s;vertex-sum=%s;shape-sum=%s",sample,ull(p.phFrame),is,vs,sh2);
            acz::feature_log::write(s5.log,line);
            { // J-effects-smoke-fix: draws per phase by pixel-shader prefix (smoke d2464ed2, effects sprite 4caf6e2d, HUD sprite d34ee153)
                char a1[120]="",a2[120]="",a3[120]="";size_t k1=0,k2=0,k3=0;
                for(unsigned i=0;i<ph::phase_count;++i) {
                    k1+=size_t(std::snprintf(a1+k1,sizeof a1-k1,"%s%s:%u",i?",":"",ph::phase_short(i),s.p[i].psSmoke));
                    k2+=size_t(std::snprintf(a2+k2,sizeof a2-k2,"%s%s:%u",i?",":"",ph::phase_short(i),s.p[i].psSprite));
                    k3+=size_t(std::snprintf(a3+k3,sizeof a3-k3,"%s%s:%u",i?",":"",ph::phase_short(i),s.p[i].psHud));
                }
                std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-burst-ps;sample=%u;observer-frame=%llu;ps-d2464ed2=%s;ps-4caf6e2d=%s;ps-d34ee153=%s",sample,ull(p.phFrame),a1,a2,a3);
                acz::feature_log::write(s5.log,line);
            }
        }
    }
    if(p.fx) { // J-effects-content: kind and the coverage summary BEFORE dir= (the line cap is 480 bytes; the png-effects line has the detail)
        std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-once;outcome=read;sample=%u;fence-value=%llu;published-frame=%llu;slot=%u;members=%u;"
            "hud-draws=%u;reexecuted=%u;map-hr=%08lx;files=%u/%u;kind=%s;effects=draws:%u,nonzero:%llu,alpha-max:%.4g,alpha-mean:%.5g,bbox:%d,%d,%d,%d;dir=%s;"
            "conversion=consumer(clamp,exposure1,srgb)",sample,ull(p.value),ull(p.publishedAt),p.slot,p.members,p.hudDraws,unsigned(p.reexecuted),
            static_cast<unsigned long>(hr),written,nouts,p.burst?"burst":"base",p.fxDraws,ull(fxs.nonzero),fxs.alpha_max,fxs.alpha_mean,
            fxs.x0,fxs.y0,fxs.x1,fxs.y1,dirUtf8);
    } else
    std::snprintf(line,sizeof line,"gpu-copy-step1;event=png-once;outcome=read;sample=%u;fence-value=%llu;published-frame=%llu;slot=%u;members=%u;"
        "hud-draws=%u;reexecuted=%u;map-hr=%08lx;files=%u/%u;dir=%s;conversion=consumer(clamp,exposure1,srgb)",sample,ull(p.value),ull(p.publishedAt),
        p.slot,p.members,p.hudDraws,unsigned(p.reexecuted),static_cast<unsigned long>(hr),written,nouts,dirUtf8);
    acz::feature_log::write(s5.log,line);
    p.stage.store(png_processed);
}
// Worker, step-1 mutex held: arm/refusal line, completion observation, next sample, terminal lines.
void step5PngTick_locked(Step1State& st,Step5State& s5,uint64_t completed) noexcept {
    if(!pngOnceConfigured())return;
    namespace png=gpu_copy::png;using ull=unsigned long long;auto& p=s5.png;
    if(p.emitArmed) {
        p.emitArmed=false;
        if(p.refusal)std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png-once;outcome=refused;reason=%s;hr=%08lx;readback=none;commands=none",
            p.refusal,static_cast<unsigned long>(p.createHr));
        else if(p.fxCapable)std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png-once;outcome=armed;images=hud,world,eye0,effects;format=10;size=%ux%u;pitch=%llu;"
            "stride=%llu;readback-bytes=%llu;samples=%u;second-after=%u-frames;commands=%u-at-close(%u-with-effects);outputs=hud-on-black,hud-alpha-raw,hud-over-world,world,eye0,"
            "effects,effects-on-black,effects-alpha-raw,effects-nonzero;dir=logs\\png",p.w,p.h,ull(p.pitch),ull(p.stride),ull(p.bytes),png::samples,png::second_gap_frames,
            png::commands_per_sample,png::commands_per_sample+3);
        else std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png-once;outcome=armed;images=hud,world,eye0;format=10;size=%ux%u;pitch=%llu;"
            "stride=%llu;readback-bytes=%llu;samples=%u;second-after=%u-frames;commands=%u-at-close;outputs=hud-on-black,hud-alpha-raw,hud-over-world,world,eye0;"
            "dir=logs\\png",p.w,p.h,ull(p.pitch),ull(p.stride),ull(p.bytes),png::samples,png::second_gap_frames,png::commands_per_sample);
        step5Log(st,s5,false);
    }
    if(p.emitBurstArmed&&!p.refusal) { // J-effects-content: the burst trigger (only with the effects layer ready)
        p.emitBurstArmed=false;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png-effects-config;trigger=first-frame-with-committed-effects-draws>=threshold-after-the-base-samples;"
            "threshold=%u;source=%s;file=%%LOCALAPPDATA%%\\acz-vr\\png-effects.threshold;max-bursts=%u;min-gap=%u-frames;burst-outputs=effects,effects-on-black,effects-alpha-raw,effects-nonzero",
            p.thr,p.thrSource,png::burst_samples,png::burst_gap_frames);
        step5Log(st,s5,false);
    }
    int g=p.stage.load();
    if(g==png_submitted) {
        if(p.epoch!=s5.fenceEpoch.load()){p.failWhy="fence-epoch-changed";p.stage.store(png_failed);}
        else if(completed==UINT64_MAX){p.failWhy="device-lost";p.stage.store(png_failed);}
        else if(completed>=p.value)p.stage.store(png_ready); // processed by the next tick, before the lock
    } else if(g==png_processed) {
        if(p.burst)++p.burstRead;else ++p.read;
        if(!p.burst&&p.read<png::samples){p.notBefore=s5.published.load()+png::second_gap_frames;p.stage.store(png_armed);g_step5PngWanted.store(true);}
        else if(p.fxCapable&&p.burstRead<png::burst_samples){ // J-effects-content: the next set waits for the threshold (>= gap frames after this one)
            p.burst=true;p.notBefore=s5.published.load()+png::burst_gap_frames;p.stage.store(png_armed);g_step5PngWanted.store(true);}
        else {p.stage.store(png_done);g_step5PngWanted.store(false);g_step5SceneEndWanted.store(false);}
    }
    g=p.stage.load();
    if(g==png_done&&!p.emitDone) {
        p.emitDone=true;
        if(p.fxCapable)std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png-once;outcome=done;samples=%u;effects-bursts=%u;readback=retained(process-lifetime)",p.read,p.burstRead);
        else std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png-once;outcome=done;samples=%u;readback=retained(process-lifetime)",p.read);
        step5Log(st,s5,false);
    }
    if(g==png_failed&&!p.emitFail) {
        p.emitFail=true;g_step5PngWanted.store(false);g_step5SceneEndWanted.store(false);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png-once;outcome=failed;sample=%u;reason=%s;no-further-sample",p.read+1,
            p.failWhy?p.failWhy:"?");
        step5Log(st,s5,false);
    }
}
// Worker, continuous phase. false when the producer finished (verdict emitted).
bool step5Tick(Step1State& st) noexcept {
    auto* s5p=g_step5.load();if(!s5p)return false;auto& s5=*s5p;
    // GPU progress (nonblocking; no lock held). UINT64_MAX: device removed.
    const uint64_t completed=st.fence.fence?st.fence.completed(step1Route(st.fence.fence)):0;
    hudDrain(s5.log);
    step5PrecockpitDrain(s5.log); // J-cockpit-fov F2 (key off: returns at once)
    step5WorldObserveDrain(s5.log); // J-world-objects v2 step 1 (key off: returns at once)
    step5WorldEvidenceDrain(s5.log); // J-world-evidence (key off: returns at once)
    step5PhaseObserveDrain(s5.log); // J-phase-observe (key off: returns at once)
    step5PngWork(st,s5);          // J-hud-colour (key off: returns at once)
    step5PngPoll(s5);             // burst protocol v2: png-burst.now poll (key off: returns at once)
    acz::feature_log::tick(GetTickCount64()); // I/O before taking the producer mutex.
    diagnostics::flush_from_worker();         // FPS investigation, perf_lean only: the diagnostics flush off the game thread (no mutex held)
    std::unique_lock lock(st.mutex);
    s5.lastCompleted=completed;
    if(st.fence.fence!=s5.epochFence) {
        // A different private fence object: a new epoch; its values start afresh. Frames of
        // the old epoch are no longer acquirable (the consumer correlates fence+epoch).
        // Slots whose last use was signalled on the OLD fence cannot be retired any more
        // (that completion is no longer observable): quarantined, never reused. The value
        // counter stays monotonic.
        if(s5.meta.try_lock()){s5.epochFence=st.fence.fence;s5.lastSignalled.store(0);s5.fenceEpoch.store(++g_step5EpochCounter);
            step5RegisterFence(st.fence.fence,s5.fenceEpoch.load());
            for(auto& sl:s5.slot)if(sl.lastUse&&sl.state!=gpu_copy::step5::slot_held)sl.state=gpu_copy::step5::slot_quarantined;
            s5.meta.unlock();}
    }
    if(st.fence.sawMax&&!s5.disabled){s5.disabled=true;s5.disableReason="device-lost-uint64max";}
    step5ApplyPending(s5);
    if(s5.lookDrive.active()) { // xr_look_drive_keys: one bounded line per second, from the tick (the Draw path never does I/O)
        char ldBuf[640];
        if(s5.lookDrive.take_log(lookDriveNowMs(),ldBuf,sizeof ldBuf)) {
            step5LogText(s5,ldBuf,false);
            if(s5.lookDrive.take_status(ldBuf,sizeof ldBuf))step5LogText(s5,ldBuf,false); // companion line: route, loop mode, calibration, foreground window
        }
        char refBuf[480];
        if(s5.lookDrive.take_ref_diag(refBuf,sizeof refBuf))step5LogText(s5,refBuf,false); // F-REF diagnostic line (bounded, rate limited)
    }
    step5PngTick_locked(st,s5,completed); // J-hud-colour (key off: returns at once)
    // EFFECTS-OUTSIDE (C) fxmem:4 requested AFTER the ring was created (live run R2: the proxy sets it at VR start): the one creation attempt, here (step-1
    // mutex held), then its fx-smoke-ring line. Option absent: one relaxed load per tick, nothing else.
    if(!s5.smokeTried&&s5.ringReady&&step5FxSmokeGpuOn()&&effectsConfigured()){step5CreateSmokeGpu(st,s5,true);step5EmitSmokeRing(st,s5);}
    // EFFECTS-OUTSIDE (F) fxmem:6: the same one late attempt for the sprite resources (after the smoke's: the creation order is fixed).
    if(!s5.spriteTried&&s5.ringReady&&step5FxSpriteGpuOn()&&effectsConfigured()){step5CreateSpriteGpu(st,s5,true);step5EmitSpriteRing(st,s5);}
    // EFFECTS-OUTSIDE (M6) fxatlas: the one creation attempt for the atlas, once the sprite resources exist (option absent: one relaxed load per tick)
    if(!s5.atlasTried&&s5.spriteReady&&step5FxSpriteGpuOn()&&gpu_copy::step5::effects_puff_atlas_flag().load(std::memory_order_relaxed)){step5CreateAtlas(st,s5);step5EmitAtlasRing(st,s5);}
    using ull=unsigned long long;
    if(!s5.emittedDefinitions) {
        s5.emittedDefinitions=true;
        step5LogText(s5,"gpu-copy-step1;event=continuous-definitions;produced=published(recorded,executed-once,fence-signalled);"
            "skipped=list-generation-with-DrawCPT-draws-in-cockpit-ordinal-range-but-no-frame(once,at-Close);"
            "skip-reasons=mismatch(state-refused),no-slot,unqualified,unreadable;mismatch-produced=published-but-differs"
            "(pso/args/ordinal/count/refused);match=ordinal-range+required-state;replay=live-args");
        if(byClassConfigured())
            step5LogText(s5,"gpu-copy-step1;event=members-by-class-definitions;start=first-DrawCPT-draw-of-an-admitted-class(any-ordinal)+required-state"
                "+camera-recovered;members=later-DrawCPT-draws-of-the-generation;admitted=selected-classes+family(equal-root,ia,t10,formats,samples,stencil);"
                "refused-alone=state,descriptor,format,class,camera,unreadable;cap=128-members,1100-commands;partial=refused-in-class-or-cap;"
                "mismatch-produced=partial-only;camera=every-member-equals-first",false);
        if(worldObjectsConfigured()) {
            step5LogText(s5,"gpu-copy-step1;event=world-objects-definitions;part=1;key=gpu_copy_step5_world_objects;identity=DrawACM_v(VS hash+full sha256),"
                "DrawACM_p(partial sha256 fingerprint c3e3b1a7...b72f),no-GS/HS/DS/SO,4xfloat4-layout;member=DrawACM-draw-inside-an-active-by-class-frame"
                "(never-a-frame-start;draws-before-the-first-cockpit-member-are-not-replayed)",false);
            step5LogText(s5,"gpu-copy-step1;event=world-objects-definitions;part=2;record=SrtData-416B@t10(A=+0,C=+64,B=+256,params=+400:512,-448,2048,2048);"
                "substitution=C+D_e,A+D_e*inverse(C)*A(absolute-world,no-M256);camera=from-C,must-equal-cockpit-axes-and-lens;pose=cockpit-neutral-shared,"
                "no-ipd-fallback(refused:world-pose-unavailable);cap=32-per-frame(shares-member-and-command-caps);partial=not-set-by-world-refusals;"
                "hud=world-draws-excluded-from-hud-replay",false);
        }
        if(hmdFovConfigured())
            step5LogText(s5,"gpu-copy-step1;event=hmd-fov-definitions;eyes=per-eye-xr-fov(tan-clamp-4.0,snap-1e-5-to-game-lens);reference=game-lens;"
                "fallback=game-lens-both-eyes(fov-zero,nonfinite,over-90-degrees,not-ordered,span-too-small);published=StereoFrame::cockpit_fov",false);
    }
    if(worldObserveConfigured()&&!s5.wobsDefsEmitted) {
        s5.wobsDefsEmitted=true;
        step5LogText(s5,"gpu-copy-step1;event=world-observe-definitions;part=1;key=gpu_copy_step5_world_observe;read-only=no-command,no-gpu-work,no-readback"
            "(srt=cpu-mapped-read-of-the-first-3-acm-draws-per-list);kinds=acm(DrawACM_v-identity-pso),cpt(DrawCPT-identity-pso),other(every-other-draw);"
            "list=route+generation(l3);ordinal=position-among-ALL-draws-of-the-list-recording;l3-ordinal=the-DrawCPT-only-counter-the-cockpit-ordinals-use",false);
        step5LogText(s5,"gpu-copy-step1;event=world-observe-definitions;part=2;window=lists-executed-since-the-previous-list-with-DrawCPT-draws"
            "(the-cockpit-list-closes-it;a-carrier-executed-right-after-the-previous-cockpit-list-is-attributed-to-the-NEXT-window:compare-exec-seq);"
            "exec-seq=ExecuteCommandLists-call-counter(batch=index-in-call);role=cockpit(has-cpt)/carrier(acm-without-cpt);"
            "around-acm=non-acm-draws-before-the-first,between-first-and-last,after-the-last-acm-draw;sampling=first-3-windows-then-1-per-5s;"
            "caps=12-lists,8-acm-draws,3-srt-per-window",false);
    }
    if(worldEvidenceConfigured()&&!s5.wevDefsEmitted) {
        s5.wevDefsEmitted=true;
        step5LogText(s5,"gpu-copy-step1;event=world-evidence-definitions;part=1;keys=gpu_copy_step5_world_observe|gpu_copy_step5_world_objects(no-key-of-its-own);"
            "read-only=no-command,no-gpu-work,no-readback,no-extra-mapped-read;acm-rt=per-ACM-PSO-first-occurrence-and-each-change-of-(pso-identity,om-binding-epoch)",false);
        step5LogText(s5,"gpu-copy-step1;event=world-evidence-definitions;part=2;acm-rt=pso-creation-facts(rt-count,all-8-rtv-formats,slot0-and-effective-slot1-blend/write-mask)"
            "+the-ORIGINAL-game-draw's-OMSetRenderTargets-metadata;label=original-game-binding;injected-eye=count-1-slot1-none-or-none(draw-refused)",false);
        step5LogText(s5,"gpu-copy-step1;event=world-evidence-definitions;part=3;slot-kinds=RESOURCE/NULL-DESCRIPTOR/UNBOUND/UNKNOWN/range-unresolved;"
            "range-unresolved=contiguous-range-form:only-the-base-handle-is-stored,slot1-NOT-interpreted(a-zero-rtv1-is-not-unbound);"
            "still-current=bind-time-copy-epoch-and-view-serial-unchanged;view-fmt/mip/slice=the-game's-CreateRenderTargetView-desc;vs/ps=32-hex-container-hashes",false);
        step5LogText(s5,"gpu-copy-step1;event=world-evidence-definitions;part=4;acm-record=copy-of-the-416-byte-liveSrt-snapshot-the-decision-used(never-reread);"
            "caps=3-per-window,8-windows-per-process,>=2s-between-windows;lines=acm-record+acm-record-decision+4-acm-record-hex(128-bytes-each,fnv1a64-of-all-416);"
            "drops-counted;exec-seq=only-with-world_observe(else-unknown)",false);
        step5LogText(s5,"gpu-copy-step1;event=world-evidence-definitions;part=5;cb10=dword0-from-the-root-constant-witness(needs-diagnostic_ownership_pins);"
            "unknown-refuses-the-draw(category-cb10);bit8-set-refuses(world-flags-fade-reads-c);rt-count!=1-refuses(category-rt-count);decision-summary=counts-per-reason",false);
    }
    if(phaseObserveConfigured()&&!s5.phaseDefsEmitted) {
        s5.phaseDefsEmitted=true;
        step5LogText(s5,"gpu-copy-step1;event=phase-observe-definitions;part=1;key=gpu_copy_step5_phase_observe;read-only=no-command,no-gpu-work,no-readback;"
            "scope=every-draw(DrawInstanced/DrawIndexedInstanced)-of-a-list-recording-with-DrawCPT-draws;ord=position-among-ALL-draws-of-the-recording;"
            "shader=DrawCPT_v(cockpit-member-identity)/DrawACM_v(world-object-identity)/other(vs8-ps8-are-the-first-8-hex-digits-of-the-VS-PS-creation-hashes:"
            "name-ps2_v-DrawMesh-text-from-them)",false);
        step5LogText(s5,"gpu-copy-step1;event=phase-observe-definitions;part=2;phases=before-first-member,member(DrawCPT),between-members(non-DrawCPT-between-first-and-last-member),"
            "hud-region(after-the-last-member-until-the-first-PROVEN-rt-change-away-from-the-scene-target:the-HUD-replay-rule),after-region-end(before-the-tone-map),"
            "after-tonemap(from-the-first-draw-after-the-region-end-into-an-R10G10B10A2-target-or-with-a-viewport-different-from-the-first-member's)",false);
        step5LogText(s5,"gpu-copy-step1;event=phase-observe-definitions;part=3;rt-class=scene(first-member-target)/scene-size/r10-output/offscreen/unknown(unresolved-rt:"
            "never-ends-the-region);back-buffer=unknown(not-tracked);stencil-ref=untracked;blend-factor=untracked;rc=cb10-root-constant-words-the-witness-tracks-or-none",false);
        step5LogText(s5,"gpu-copy-step1;event=phase-observe-definitions;part=4;log=first-3-frames,every-frame-with-a-signature-never-seen,every-frame-with-a-PSO-never-seen,"
            "changed-frames(6-per-5s),1-sample-per-5s;draws-written=changed-phases(+new-pso-draws);caps=1024-draws-per-recording,200-draw-lines-per-frame,"
            "64-signatures,256-psos,200-frames",false);
        step5LogText(s5,"gpu-copy-step1;event=phase-observe-definitions;part=5;frame-phases-fields=draws/indexed/on-scene-rt/unknown-rt/vp-off-scene/new-pso/"
            "index-sum/vertex-sum/instance-sum/shape-hash;shape-hash=order-independent,ignores-counts(pso-identity,state,target-class);sig=hash-of-draws+shape-per-phase",false);
        step5LogText(s5,"gpu-copy-step1;event=phase-observe-definitions;part=6;pso-identity=creation-hashes(vs8,ps8)+creation-state-NOT-the-object-pointer-or-creation-id;"
            "armed-windows=create-or-touch-%LOCALAPPDATA%/acz-vr/phase-observe.arm-after-the-game-is-in-the-cockpit(a-file-present-at-the-first-poll-is-ignored);"
            "window=20s,every-cockpit-frame-logged(frame+frame-phases),novel-draws(absent-from-the-quiet-reference:last-frame-at-arming+first-1.5s)-logged-once-per-key,"
            "end-line-with-per-key-summaries;caps=4-windows,2MiB-per-window,24-novel-draw-lines-per-frame,128-novel-keys;"
            "mark=touch-%LOCALAPPDATA%/acz-vr/phase-observe.mark(logs-phase-observe-mark-with-tick-and-utc-ms,frames-then-carry-since-mark-ms)",false);
    }
    auto& f=s5.frame;
    if(f.active&&!f.submitted&&++f.ticks>20)step5Fault(st,s5,"frame-not-submitted-within-2s",true);
    if(s5.published.load()&&!s5.emittedFirst) {
        s5.emittedFirst=true;char sha[65];step1Sha256(s5.firstLive.data(),416,sha);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=continuous-first;frame=%llu;fence-value=%llu;members=%u;commands=%u;"
            "pose=%s;pose-valid=%u;control-zero-pose-bytes-differ=%u/416;member0-live-sha256=%s;lens-valid=%u;lens-tan=%.5f,%.5f,%.5f,%.5f;"
            "world-valid=%u;eyes=%s",ull(s5.firstFrame),ull(s5.firstValue),s5.firstMembers,s5.firstCommands,s5.firstPoseWhy,unsigned(s5.firstPose),
            s5.controlDiffer,sha,unsigned(s5.firstLens),double(s5.firstLensTan[0]),double(s5.firstLensTan[1]),double(s5.firstLensTan[2]),
            double(s5.firstLensTan[3]),unsigned(s5.firstWorld),
            s5.firstPose?(s5.firstFovHmd?"pose-full-substitution-hmd-fov":"pose-full-substitution-game-lens"):"ipd-row3-fallback");
        step5Log(st,s5);
        step5Status(st,s5,"gpu-copy-step1;event=continuous-status");
    }
    if(s5.worldFirstPending&&!s5.worldFirstEmitted) {
        s5.worldFirstPending=false;s5.worldFirstEmitted=true;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=world-snapshot;outcome=copied;frame=%llu;game-rt=%llx;size=%llux%u;format=%u;"
            "barrier-calls=4(game:RT->COPY_SOURCE->RT,world:RT->COPY_DEST->RT);copies=1"
            "(CopyTextureRegion,sub0);restored=recorded-before-the-game-draw;state-evidence=%s;facts=view-sub0,1-rtv,same-resource,"
            "barrier-seam-hooked,no-split/alias/uav/enhanced,qualified-list",
            ull(s5.worldFirstFrame),ull(s5.worldFirstResource),ull(s5.worldW),s5.worldH,s5.worldFormat,
            s5.worldFirstEvidence?s5.worldFirstEvidence:"?");
        step5Log(st,s5);
    }
    // J-world-hold: per-refusal detail records (the first 8 and the first of each later burst), at most 4 lines per tick.
    if(s5.refDetailCount&&!s5.worldRefDefsEmitted) {
        s5.worldRefDefsEmitted=true;
        step5LogText(s5,"gpu-copy-step1;event=world-refusal-definitions;sub=the-first-failing-test-of-step3Capture-for-entry-state-unknown;"
            "no-transition-tracked=no-barrier-to-RENDER_TARGET-seen-for-the-scene-RT-in-this-list-recording;"
            "recording-missing/generation-zero/generation-mismatch/watch-missing/serial-missing/resource-mismatch/watch-epoch-mismatch/serial-mismatch="
            "a-transition-was-seen-but-not-for-this-list-generation-watch-epoch-serial;capture-incomplete=no-hook-state-for-the-list;"
            "head-pitch=elevation-of-the-published-eye0-pose;cam-fwd=recovered-cockpit-camera-forward-axis",false);
    }
    for(unsigned emitted=0;s5.refDetailEmitted<s5.refDetailCount&&emitted<4;++emitted) {
        const auto& d=s5.refDetail[s5.refDetailEmitted++];
        char head[24]="none",cam[56]="none";
        if(d.head)std::snprintf(head,sizeof head,"%.1f",d.headDeg);
        if(d.cam)std::snprintf(cam,sizeof cam,"%.3f,%.3f,%.3f",d.fwd[0],d.fwd[1],d.fwd[2]);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=world-refusal-detail;n=%u;frame=%llu;reason=%s;sub=%s;draw-ordinal=%llu;list-generation=%llu;"
            "head-pitch-deg=%s;cam-fwd=%s;no-headset-claim",s5.refDetailEmitted,ull(d.frame),d.why?d.why:"?",d.entry?gpu_copy::step5::entry_sub_name(d.sub):"n/a",
            ull(d.ordinal),ull(d.generation),head,cam);
        step5Log(st,s5,false);
    }
    if(s5.worldRefusalPending&&!s5.worldRefusalEmitted) {
        s5.worldRefusalPending=false;s5.worldRefusalEmitted=true;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=world-snapshot;outcome=refused;reason=%s;world-valid=0;eyes=published;"
            "barrier-calls=0;game-rt=%llx",s5.worldFirstRefusal?s5.worldFirstRefusal:"?",ull(s5.worldSource));
        step5Log(st,s5);
    }
    if(hudReplayConfigured())step5EmitHudEvents(st,s5);
    if(s5.wobjFirstPending&&!s5.wobjFirstEmitted) { // J-world-objects: first published frame with world members
        s5.wobjFirstPending=false;s5.wobjFirstEmitted=true;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=world-objects;outcome=first-published;frame=%llu;world-members=%u;camera-abs=%.1f,%.1f,%.1f;"
            "model-translation=%.1f,%.1f,%.1f;lens-tan=%.5f,%.5f;eye-offset-units=%.3f,%.3f;units-per-metre=%.1f;measured=nothing-live",
            ull(s5.wobjFirstFrame),s5.wobjFirstMembers,s5.wobjFirstEye[0],s5.wobjFirstEye[1],s5.wobjFirstEye[2],s5.wobjFirstModel[0],s5.wobjFirstModel[1],
            s5.wobjFirstModel[2],s5.wobjFirstLens[0],s5.wobjFirstLens[1],s5.wobjFirstOffset[0],s5.wobjFirstOffset[1],worldUnitsPerMetreConfigured());
        step5Log(st,s5,false);
    }
    if(hmdFovConfigured())step5EmitFovEvents(st,s5);
    if(byClassConfigured())step5EmitFamily(st,s5);
    // Mismatch detail: the first immediately, then at most one per status period.
    if(s5.mismatchTicks<gpu_copy::step5::status_every_ticks)++s5.mismatchTicks;
    // J-3b9c: every detail goes to the dedicated log (one per tick); the shared log keeps
    // the first and then at most one per 5 s.
    if(s5.mismatchOut.set){const bool mirror=s5.mismatchLines==0||s5.mismatchTicks>=gpu_copy::step5::status_every_ticks;
        step5EmitMismatch(st,s5,mirror);if(mirror)s5.mismatchTicks=0;}
    while(s5.admitLogged<s5.admitCount) {
        const auto& a=s5.admits[s5.admitLogged++];const Step5Class* c=nullptr;
        Step5Class k{};
        for(unsigned m=0;m<st.s3.memberCount&&!c;++m)if(st.s3.members[m].shape.draw_ordinal==a.ordinal){k=step5ClassOf(st.s3.members[m].shape);c=&k;}
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=alt-pso-admit;pso=%llx;ordinal=%llu;frame=%llu;alternates=%u/16;compared=root:%llx,topo:%u,ib:%u,"
            "vb:%u,t10:%u,cut:%x,rtv:%u,dsv:%u,samples:%u/%u,depth:%u/%u/%u,stencil:%u;rule=all-class-facts-equal-at-ordinal",ull(a.pso),ull(a.ordinal),
            ull(a.frame),s5.altCount,ull(c?c->root:0),c?c->topology:0,c?c->ib:0,c?c->vb:0,c?c->t10:0,c?c->cut:0,c?c->rtv:0,c?c->dsv:0,c?c->samples:0,
            c?c->quality:0,c?unsigned(c->depthEnable):0,c?unsigned(c->depthWrite):0,c?c->depthFunc:0,c?unsigned(c->stencil):0);
        step5Log(st,s5);
    }
    step5DeadWatch(st,s5); // J-compat-race: loud, rate-limited producer-dead line
    if(++s5.statusTicks>=gpu_copy::step5::status_every_ticks){s5.statusTicks=0;step5Status(st,s5,"gpu-copy-step1-status;event=continuous");
        if(precockpitConfigured()){std::lock_guard pl(g_pcMutex);g_pc.due=true;} // F2: the next generation is sampled
        if(worldObserveConfigured()){std::lock_guard wl(g_woMutex);g_wo.due=true;} // J-world-objects v2 step 1: the next window is sampled
        if(phaseObserveConfigured()){std::lock_guard pl(g_phMutex);g_ph.due=true;g_ph.budget=gpu_copy::phobs::dump_budget;}} // J-phase-observe: sample + refill
    if(!s5.disabled&&s5.consecutiveMismatch>=gpu_copy::step5::reselect_after) {
        if(s5.reselections>=gpu_copy::step5::reselect_cap){s5.disabled=true;s5.disableReason="reselection-cap";}
        else {step5Reselect(st);return true;}
    }
    if(s5.disabled&&!s5.finished) {
        s5.finished=true;
        if(s5.lookDrive.active())s5.lookDrive.shutdown(); // xr_look_drive_keys: release every key on a producer shutdown
        step5Status(st,s5,"gpu-copy-step1;event=continuous-final");
        step1Finish(st,"FAIL",s5.disableReason?s5.disableReason:"producer-disabled");
        lock.unlock();acz::feature_log::flush();
        return false;
    }
    return true;
}
