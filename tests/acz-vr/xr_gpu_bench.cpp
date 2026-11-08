// xr_gpu_bench.cpp -- GPU-COST (docs/world-render/gpu-cost.md): OPT-IN benchmark of the REAL per-eye composite (xr_stereo_d3d12.cpp backend +
// embedded xr_stereo_blit.hlsl / xr_props.hlsl / cockpit_model.hlsl bytecode) on the REAL hardware adapter, with the REAL stage data loaded by the
// production loaders (acz::terrain::load_stage_file + build_atlas + build_env_texture, acz::props::load_props_file + snap_to_terrain + build_grid,
// acz::cockpit::load_model_file + build_mesh) and D3D12 timestamp marks between the passes (BlitJob::pass_timing).
//
// NOT a test: never registered with ctest (it needs a real GPU and the extracted game data). Offscreen only: no game, no SteamVR, no OpenXR runtime,
// no window. Default adapter = the first hardware adapter by DXGI high-performance preference; --warp runs the same code on WARP (smoke check only,
// tiny resolution recommended).
//
// Usage: xr_gpu_bench [--stage-dir D:\acz-extract\stage-all] [--props-dir D:\acz-extract\stage-perf (the installed 1.16M-instance v2 bake)][--model D:\acz-extract\models-c4\j35jk_vrcockpit.zvrm] [--w 2400 --h 1350]
//                     [--frames 30] [--warmup 8] [--out D:\acz-extract\gpu-bench] [--warp] [--quick] [--compare] [--png] [--csv <file>]
//   --compare: for every scene also renders each quality option (q:1, q:2, zpre) and compares eye 0 with the q:0 image (max error / pixels over budget).
//   PROPS-SHADING close-ups (docs/world-render/props-shading.md): --pose <name>:<x>,<y>,<z>,<yawdeg>,<pitchdeg> (absolute camera, repeatable; replaces the
//   built-in poses), --no-cockpit, --shade game|w3|both (props shading, default game; both = one image per mode), --hfov <deg> (eye horizontal FOV, 100).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cockpit_model.h"
#include "gpu_copy_step3.h"
#include "terrain_stage.h"
#include "world_props.h"
#include "xr_owner.h"
#include "xr_stereo_d3d12.h"
#include "xr_stereo_output.h"

namespace acz {
void* nativeDispatch(void* object, int slot) noexcept {   // nothing is hooked in this process: the saved native IS the vtable entry
    if (!object || slot < 0) return nullptr;
    return (*reinterpret_cast<void***>(object))[slot];
}
void enterProxyCreationScope(bool) noexcept {}
void leaveProxyCreationScope(bool) noexcept {}
} // namespace acz

using Microsoft::WRL::ComPtr;
using namespace acz::xr_stereo;
namespace T = acz::terrain;
namespace P = acz::props;
namespace K = acz::cockpit;

static float deg2rad(float d) { return d * 3.14159265358979f / 180.0f; }

// ------------------------------------------------------------------------------------------------ device
struct Dev {
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q; ComPtr<ID3D12CommandAllocator> al; ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence; HANDLE ev = nullptr; UINT64 fv = 0; std::string name; size_t vram_mb = 0;
    bool init(bool warp, std::string& why) {
        ComPtr<IDXGIFactory6> fac;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { why = "CreateDXGIFactory2 failed"; return false; }
        ComPtr<IDXGIAdapter1> ad;
        if (warp) {
            if (FAILED(fac->EnumWarpAdapter(IID_PPV_ARGS(&ad)))) { why = "EnumWarpAdapter failed"; return false; }
        } else {
            for (UINT i = 0;; ++i) {   // highest-performance first; skip software adapters (WARP / Basic Render Driver)
                ComPtr<IDXGIAdapter1> a;
                if (fac->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
                if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) { ad = a; break; }
            }
            if (!ad) { why = "no hardware D3D12 adapter"; return false; }
        }
        if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) { why = "D3D12CreateDevice failed"; return false; }
        DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d);
        char nm[256]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, sizeof nm, nullptr, nullptr); name = nm; vram_mb = size_t(d.DedicatedVideoMemory >> 20);
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { why = "queue/list/fence creation failed"; return false; }
        cl->Close();
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return ev != nullptr;
    }
    bool begin() { return SUCCEEDED(al->Reset()) && SUCCEEDED(cl->Reset(al.Get(), nullptr)); }
    bool submit() {
        if (FAILED(cl->Close())) return false;
        ID3D12CommandList* l[] = {cl.Get()};
        q->ExecuteCommandLists(1, l);
        if (FAILED(q->Signal(fence.Get(), ++fv))) return false;
        if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); if (WaitForSingleObject(ev, 60000) != WAIT_OBJECT_0) return false; }
        return SUCCEEDED(dev->GetDeviceRemovedReason());
    }
    ComPtr<ID3D12Resource> tex(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv = nullptr) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = f; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = flags;
        ComPtr<ID3D12Resource> r; dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, cv, IID_PPV_ARGS(&r)); return r;
    }
    // FP16 colour source cleared to (0,0,0,0) in RENDER_TARGET (the producer's publish state).
    ComPtr<ID3D12Resource> colour(UINT w, UINT h) {
        auto t = tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12DescriptorHeap> heap; D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        if (!t || FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return nullptr;
        dev->CreateRenderTargetView(t.Get(), nullptr, heap->GetCPUDescriptorHandleForHeapStart());
        const FLOAT z[4] = {0, 0, 0, 0};
        if (!begin()) return nullptr;
        cl->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(), z, 0, nullptr);
        return submit() ? t : nullptr;
    }
    // D32 depth (typeless resource, the producer's D-240 format) cleared to 0 = no replayed cockpit / object coverage anywhere.
    ComPtr<ID3D12Resource> depth(UINT w, UINT h) {
        auto t = tex(w, h, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ComPtr<ID3D12DescriptorHeap> heap; D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
        if (!t || FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return nullptr;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(t.Get(), &dv, heap->GetCPUDescriptorHandleForHeapStart());
        if (!begin()) return nullptr;
        cl->ClearDepthStencilView(heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        return submit() ? t : nullptr;
    }
    bool readback(ID3D12Resource* t, std::vector<uint8_t>& out) {   // RGBA8 target in RENDER_TARGET -> tight rows
        const D3D12_RESOURCE_DESC rd = t->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rb = 0, total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rb, &total);
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rbk;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbk))) || !begin()) return false;
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = t;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rbk.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        src.pResource = t; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
        if (!submit()) return false;
        uint8_t* m = nullptr; if (FAILED(rbk->Map(0, nullptr, reinterpret_cast<void**>(&m)))) return false;
        out.assign(size_t(rd.Width) * rd.Height * 4, 0);
        for (UINT r = 0; r < rows; ++r) std::memcpy(out.data() + size_t(r) * rd.Width * 4, m + size_t(fp.Footprint.RowPitch) * r, size_t(rd.Width) * 4);
        D3D12_RANGE none{0, 0}; rbk->Unmap(0, &none);
        return true;
    }
};
static Dev g;

// ------------------------------------------------------------------------------------------------ scene description
struct Pose { std::string name; float x, z, agl, pitch_deg, heading_deg; bool abs_y = false; };
struct Feature { bool env = true, look = true, props = true, trees = true, cockpit = true, shade_game = true; };
struct Variant { const char* name; uint32_t quality = 0; bool zpre = false; bool dc = false; };   // dc: DECOUPLE re-projection on (decouple.md)
static std::string feat_name(const Feature& f) {
    std::string s;
    s += f.env ? "env" : "noenv"; if (f.env) s += f.look ? "+look" : "";
    s += f.props ? "+props" : ""; s += f.trees ? "+trees" : ""; s += f.cockpit ? "+cockpit" : ""; s += (f.props || f.trees) ? (f.shade_game ? "+game" : "+w3") : "";
    return s;
}

struct Stats { double med[kPassMarks] = {}; };
static double median(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string stage_dir = "D:\\acz-extract\\stage-all", props_dir = "D:\\acz-extract\\stage-perf", model = "D:\\acz-extract\\models-c4\\j35jk_vrcockpit.zvrm", out_dir = "D:\\acz-extract\\gpu-bench", csv;
    unsigned W = 2400, H = 1350, frames = 30, warmup = 8;
    bool warp = false, quick = false, compare = false, png_out = false, no_cockpit = false, decouple = false;
    std::string shade = "game"; float hfov = 100.0f;
    std::vector<Pose> user_poses;
    for (int i = 1; i < argc; ++i) {
        auto nx = [&](void) -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--stage-dir")) stage_dir = nx();
        else if (!std::strcmp(argv[i], "--props-dir")) props_dir = nx();
        else if (!std::strcmp(argv[i], "--model")) model = nx();
        else if (!std::strcmp(argv[i], "--w")) W = unsigned(std::atoi(nx()));
        else if (!std::strcmp(argv[i], "--h")) H = unsigned(std::atoi(nx()));
        else if (!std::strcmp(argv[i], "--frames")) frames = unsigned(std::atoi(nx()));
        else if (!std::strcmp(argv[i], "--warmup")) warmup = unsigned(std::atoi(nx()));
        else if (!std::strcmp(argv[i], "--out")) out_dir = nx();
        else if (!std::strcmp(argv[i], "--csv")) csv = nx();
        else if (!std::strcmp(argv[i], "--warp")) warp = true;
        else if (!std::strcmp(argv[i], "--quick")) quick = true;
        else if (!std::strcmp(argv[i], "--compare")) compare = true;
        else if (!std::strcmp(argv[i], "--png")) png_out = true;
        else if (!std::strcmp(argv[i], "--no-cockpit")) no_cockpit = true;
        else if (!std::strcmp(argv[i], "--decouple")) decouple = true;   // DECOUPLE: + zpre and zpre+dc (the re-projected per-XR-frame composite)
        else if (!std::strcmp(argv[i], "--shade")) shade = nx();
        else if (!std::strcmp(argv[i], "--hfov")) hfov = float(std::atof(nx()));
        else if (!std::strcmp(argv[i], "--pose")) {   // name:x,y,z,yaw,pitch
            const std::string a = nx(); const size_t c = a.find(':');
            Pose p{}; float v[5] = {0, 0, 0, 0, 0};
            if (c == std::string::npos || std::sscanf(a.c_str() + c + 1, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) != 5) { std::printf("bad --pose %s\n", a.c_str()); return 2; }
            p.name = a.substr(0, c); p.x = v[0]; p.agl = v[1]; p.z = v[2]; p.heading_deg = v[3]; p.pitch_deg = v[4]; p.abs_y = true;
            user_poses.push_back(p);
        }
        else { std::printf("unknown argument %s\n", argv[i]); return 2; }
    }
    if (!W || !H || W > 8192 || H > 8192 || !frames) { std::printf("bad size/frames\n"); return 2; }
    if (shade != "game" && shade != "w3" && shade != "both") { std::printf("bad --shade\n"); return 2; }
    if (!(hfov > 10.0f && hfov < 170.0f)) { std::printf("bad --hfov\n"); return 2; }
    std::string why;
    if (!g.init(warp, why)) { std::printf("FAIL device: %s\n", why.c_str()); return 1; }
    std::printf("adapter: %s (%zu MB dedicated)%s\nper-eye target: %ux%u RGBA8 sRGB, frames %u (+%u warm-up), median of the per-pass deltas\n", g.name.c_str(), g.vram_mb,
        warp ? " [WARP: smoke only]" : "", W, H, frames, warmup);

    // ---------------------------------------------------------------- data, through the production loaders
    T::StageData st;
    const std::string spath = T::stage_path("m02a", stage_dir);
    T::LoadStatus ls = T::load_stage_file(spath, st);
    if (ls != T::LoadStatus::ok) { std::printf("FAIL stage %s: %s\n", spath.c_str(), T::status_name(ls)); return 1; }
    T::Conventions conv;   // the default conventions (sea guess etc. as the DLL applies them)
    T::apply_sea_convention(st, conv);
    T::Atlas at; T::build_atlas(st, at);
    T::Conventions conv_look = conv; conv_look.look = true;
    std::vector<float> env, env_look;
    T::build_env_texture(st, conv, env); T::build_env_texture(st, conv_look, env_look);
    std::printf("stage %s: tiles %u env %s hmax %.0f u sea %s %.0f\n", st.id, st.ntiles, env.empty() ? "absent" : "present", double(st.hmax_byte) * st.hstep,
        st.sea_present() ? "yes" : "no", double(st.sea_level));
    auto load_props = [&](bool props, bool trees, uint64_t gen) -> std::shared_ptr<P::Runtime> {
        auto rt = std::make_shared<P::Runtime>();
        const std::string ppath = P::props_path("m02a", props_dir);
        const P::LoadStatus ps = P::load_props_file(ppath, rt->set);
        if (ps != P::LoadStatus::ok || !P::snap_to_terrain(rt->set, st)) { std::printf("WARN props %s: %s (props off)\n", ppath.c_str(), P::status_name(ps)); return nullptr; }
        rt->opt.props = props; rt->opt.trees = trees; rt->generation = gen; rt->opt.shade_game = true;
        P::build_grid(rt->set, rt->opt, rt->grid);
        return rt;
    };
    auto pr_all = load_props(true, true, 1), pr_props = load_props(true, false, 2), pr_trees = load_props(false, true, 3);
    // PROPS-SHADING: the same sets with the WORLD-3/4 shading (propshade:w3)
    auto w3 = [](const std::shared_ptr<P::Runtime>& r, uint64_t gen) { if (!r) return r; auto c = std::make_shared<P::Runtime>(*r); c->opt.shade_game = false; c->generation = gen; return c; };
    auto pr_all_w3 = w3(pr_all, 4), pr_props_w3 = w3(pr_props, 5), pr_trees_w3 = w3(pr_trees, 6);
    if (pr_all) std::printf("props: %zu instances, %zu models\n", pr_all->set.inst.size(), pr_all->set.models.size());
    K::ModelData md; K::Options kopt; K::Mesh mesh;
    const K::LoadStatus ks = K::load_model_file(model, md);
    const bool have_cockpit = ks == K::LoadStatus::ok && K::build_mesh(md, kopt, mesh, why);
    if (!have_cockpit) std::printf("WARN cockpit model %s: %s %s (cockpit off)\n", model.c_str(), K::status_name(ks), why.c_str());
    else std::printf("cockpit %s: %u triangles (opaque %u blend %u glass %u vertices)\n", model.c_str(), mesh.triangles, mesh.opaque_vertices, mesh.blend_vertices, mesh.glass_vertices);

    // the busiest place: the grid cell with the most props (buildings, x20) + trees within its 5x5 neighbourhood (the town/forest of the mission area);
    // every pose starts above it (the low poses fly over that forest/town)
    float fx = 0, fz = 0;
    if (pr_all) {
        const auto& s0 = pr_all->grid.start[0];
        const auto& s1 = pr_all->grid.start[1];
        const int D = P::kGridDim;
        auto cnt = [&](const std::vector<uint32_t>& s, int x, int z) { return (x < 0 || z < 0 || x >= D || z >= D) ? 0u : s[size_t(z) * D + x + 1] - s[size_t(z) * D + x]; };
        long best = -1; int bx = 0, bz = 0; uint32_t bp = 0, bt = 0;
        for (int z = 0; z < D; ++z) for (int x = 0; x < D; ++x) {
            uint32_t np = 0, nt = 0;
            for (int dz = -2; dz <= 2; ++dz) for (int dx = -2; dx <= 2; ++dx) { np += cnt(s0, x + dx, z + dz); nt += cnt(s1, x + dx, z + dz); }
            const long sc = long(np) * 20 + long(nt);
            if (sc > best) { best = sc; bx = x; bz = z; bp = np; bt = nt; }
        }
        fx = (float(bx) + 0.5f) * P::kGridCell - 262144.0f; fz = (float(bz) + 0.5f) * P::kGridCell - 262144.0f;
        std::printf("start cell (%.0f, %.0f): %u props + %u trees within +-2 cells (20 km square)\n", double(fx), double(fz), bp, bt);
    }
    // heading toward the stage centre (the long view across the map)
    const float hd = std::atan2(-(0.0f - fx), -(0.0f - fz)) * 57.29578f;   // forward = (-sin h, -cos h)
    std::vector<Pose> poses = !user_poses.empty() ? user_poses : std::vector<Pose>{
        {"level-2500", fx, fz, 2500.0f, 0.0f, hd},
        {"low-300-forest", fx, fz, 300.0f, -8.0f, hd},
        {"nose-down-30", fx, fz, 2500.0f, -30.0f, hd},
        {"horizon-300", fx, fz, 300.0f, 1.0f, hd},
    };
    std::vector<Feature> feats;
    { Feature f; f.cockpit = !no_cockpit; f.shade_game = shade != "w3"; feats.push_back(f); }   // everything on (the live configuration: env+look+props+trees+model cockpit)
    if (shade == "both") { Feature f; f.cockpit = !no_cockpit; f.shade_game = false; feats.push_back(f); }
    if (!quick && user_poses.empty()) {
        Feature f;
        f = Feature{}; f.props = false; f.trees = false; feats.push_back(f);
        f = Feature{}; f.trees = false; feats.push_back(f);
        f = Feature{}; f.look = false; feats.push_back(f);
        f = Feature{}; f.env = false; f.look = false; feats.push_back(f);
        f = Feature{}; f.cockpit = false; feats.push_back(f);
        f = Feature{}; f.env = false; f.look = false; f.props = false; f.trees = false; f.cockpit = false; feats.push_back(f);   // bare terrain march
    }
    std::vector<Variant> variants = {{"q0", 0, false}};
    if (compare) { variants.push_back({"zpre", 0, true}); variants.push_back({"q1", 1, false}); variants.push_back({"q1+zpre", 1, true}); variants.push_back({"q2+zpre", 2, true}); }
    if (decouple) {   // DECOUPLE: the composite of one XR frame with the objects image re-projected (P0 = the head 2 deg to the left, camera delta 0.5 deg)
        if (!compare) variants.push_back({"zpre", 0, true});
        variants.push_back({"dc", 0, false, true}); variants.push_back({"zpre+dc", 0, true, true});
    }

    // ---------------------------------------------------------------- backend + targets
    auto blit = make_d3d12_blit(g.dev.Get(), g.q.Get());
    if (!blit->setup()) { std::printf("FAIL blit setup\n"); return 1; }
    ComPtr<ID3D12Resource> tgt[2];
    for (unsigned e = 0; e < 2; ++e) {
        tgt[e] = g.tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
        SwapchainImages si; si.count = 1; si.images[0] = tgt[e].Get();
        if (!tgt[e] || !blit->bind(e, si, W, H, rgba8_srgb)) { std::printf("FAIL bind eye %u\n", e); return 1; }
    }
    ComPtr<ID3D12Resource> src[2], dep[2], world = g.colour(64, 64);
    for (unsigned e = 0; e < 2; ++e) { src[e] = g.colour(W, H); dep[e] = g.depth(W, H); if (!src[e] || !dep[e]) { std::printf("FAIL sources\n"); return 1; } }
    if (!world) return 1;
    // eye FOV: 100 deg horizontal per eye, square pixels (vertical from the aspect), IPD 64 mm, head at the seat origin looking forward
    const float th = std::tan(deg2rad(0.5f * hfov)), tv = th * float(H) / float(W);
    acz::xr_owner::EyeSample eyes[2];
    for (int e = 0; e < 2; ++e) {
        eyes[e].pose.position[0] = e ? 0.032f : -0.032f;
        eyes[e].fov = {-std::atan(th), std::atan(th), std::atan(tv), -std::atan(tv)};
    }
    const float lens[4] = {std::tan(deg2rad(31.6f)), std::tan(deg2rad(31.6f)), std::tan(deg2rad(19.085f)), std::tan(deg2rad(19.085f))};
    const float anchor[3] = {0, 0, 0};
    WorldComposite wc[2];
    for (int e = 0; e < 2; ++e) if (!world_composite_params(eyes[e], anchor, 0.0f, lens, 40.0f, wc[e], 0.0f)) { std::printf("FAIL world params\n"); return 1; }
    K::Constants kc[2]{};
    if (have_cockpit) for (int e = 0; e < 2; ++e) {
        const float fov[4] = {eyes[e].fov.left, eyes[e].fov.right, eyes[e].fov.up, eyes[e].fov.down};
        K::EyeCam cam{};
        if (!K::eye_camera(eyes[e].pose.orientation, eyes[e].pose.position, fov, anchor, 0.0f, mesh.eye, kopt.upm, kopt.near_u, cam)) { std::printf("FAIL eye camera\n"); return 1; }
        kc[e] = K::make_constants(cam, kopt, 1.0f, 0u);
    }

    auto make_job = [&](const Pose& p, const Feature& f, const Variant& v) {
        BlitJob j;
        for (int e = 0; e < 2; ++e) { j.eyes[e] = {src[e].Get(), 10u, W, H}; j.depth[e] = {dep[e].Get(), 39u, W, H}; j.map[e] = full_rect(W, H, W, H); }
        j.use_depth = true;
        j.use_world = true; j.world = {world.Get(), 10u, 64, 64}; j.world_composite = true; j.world_eye = {wc[0], wc[1]};
        j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at;
        j.terrain_gen = f.env ? (f.look ? 12 : 11) : 10;   // one upload per env texture (the generation keys the GPU copy)
        j.terrain_env = f.env ? (f.look ? &env_look : &env) : nullptr; j.terrain_env_bits = f.env ? T::kEnvAll : 0u;
        j.terrain_haze_l = f.look ? T::kLookHazeL : conv.haze_l; j.terrain_far = conv.far_dist;
        j.terrain_quality = v.quality;
        const float ground = p.abs_y ? 0.0f : T::height_effective(st, p.x, p.z);
        const float campos[3] = {p.x, ground + p.agl, p.z};
        T::CamFrame cf{};
        T::camera_frame(campos, deg2rad(p.pitch_deg), deg2rad(p.heading_deg), 0.0f, conv, cf);
        for (int e = 0; e < 2; ++e) {
            T::eye_world(cf, wc[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
        }
        const P::Runtime* pr = f.shade_game ? (f.props && f.trees ? pr_all.get() : f.props ? pr_props.get() : f.trees ? pr_trees.get() : nullptr)
                                            : (f.props && f.trees ? pr_all_w3.get() : f.props ? pr_props_w3.get() : f.trees ? pr_trees_w3.get() : nullptr);
        j.props = pr; j.props_async = false;
        if (f.cockpit && have_cockpit) { j.cockpit = true; j.cockpit_mesh = &mesh; j.cockpit_gen = 1; j.cockpit_eye = {kc[0], kc[1]}; j.cockpit_prepass = v.zpre; }
        if (v.dc) {   // DECOUPLE: P1 = eyes (the composite), P0 = the render pose of the images: the head yawed 2 deg; camera delta: 0.5 deg about the up axis
            const double a = deg2rad(0.5f), Q[3][3] = {{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}};
            for (int e = 0; e < 2; ++e) {
                acz::xr_owner::EyeSample p0 = eyes[e];
                const float h = 0.5f * deg2rad(2.0f); p0.pose.orientation[1] = std::sin(h); p0.pose.orientation[3] = std::cos(h);
                WorldComposite w0{};
                if (!world_composite_params(p0, anchor, 0.0f, lens, 40.0f, w0, 0.0f) || !reproject_params(w0, Q, Q, true, true, 0.02f, j.reproject[e])) std::printf("WARN dc params refused\n");
            }
        }
        j.pass_timing = true;
        return j;
    };
    auto run = [&](const BlitJob& j, RecordFacts& facts, PassTiming& pt) {
        static uint64_t sv = 0;
        facts = blit->record(j);
        if (!facts.ok) return false;
        bool dispatch = false;
        if (!blit->close() || !blit->execute(dispatch) || !blit->signal(++sv)) return false;
        uint64_t done = 0; const DWORD t0 = GetTickCount();
        for (;;) { if (!blit->sample(done)) return false; if (done >= sv) break; if (GetTickCount() - t0 > 60000) return false; SwitchToThread(); }
        return blit->pass_timing(pt);
    };
    CreateDirectoryA(out_dir.c_str(), nullptr);
    FILE* fcsv = csv.empty() ? nullptr : std::fopen(csv.c_str(), "w");
    if (fcsv) std::fprintf(fcsv, "pose,features,variant,props_ms,eye0_pre,eye0_composite,eye0_model,eye0_glass,eye1_pre,eye1_composite,eye1_model,eye1_glass,frame_ms,instances,max_err,bad_frac\n");
    std::printf("\n%-15s %-34s %-8s %7s | %7s %7s %7s %7s | %7s %7s %7s %7s | %8s %7s %s\n", "pose", "features", "variant", "props", "e0 pre", "e0 comp", "e0 model", "e0 glass",
        "e1 pre", "e1 comp", "e1 model", "e1 glass", "frame ms", "inst", "vs q0 (max err, >2/255 frac)");
    for (const Pose& p : poses) {
        for (const Feature& f : feats) {
            std::vector<uint8_t> ref;
            // the variants are INTERLEAVED frame by frame (v0 v1 v2 .. v0 v1 ..) so a GPU clock change affects all of them alike
            const size_t nv = variants.size();
            std::vector<BlitJob> jobs; for (const Variant& v : variants) jobs.push_back(make_job(p, f, v));
            std::vector<std::vector<double>> dd(nv * kPassMarks);
            std::vector<RecordFacts> vfacts(nv);
            bool all_ok = true;
            for (size_t vi = 0; vi < nv && all_ok; ++vi) { PassTiming pt{}; for (unsigned i = 0; i < warmup && all_ok; ++i) all_ok = run(jobs[vi], vfacts[vi], pt); }
            for (unsigned i = 0; i < frames && all_ok; ++i) for (size_t vi = 0; vi < nv && all_ok; ++vi) {
                PassTiming pt{};
                all_ok = run(jobs[vi], vfacts[vi], pt);
                if (!all_ok) break;
                const double ms = 1000.0 / double(pt.hz);
                for (unsigned m = 1; m < kPassMarks; ++m) dd[vi * kPassMarks + m].push_back(double(int64_t(pt.ticks[m] - pt.ticks[m - 1])) * ms);
                dd[vi * kPassMarks].push_back(double(int64_t(pt.ticks[kPassEnd] - pt.ticks[kPassStart])) * ms);
            }
            if (!all_ok) { std::printf("%-15s %-34s FAILED (record/execute/timing)\n", p.name.c_str(), feat_name(f).c_str()); continue; }
            for (size_t vi = 0; vi < nv; ++vi) {
                const Variant& v = variants[vi];
                RecordFacts facts{}; PassTiming pt{};
                if (!run(jobs[vi], facts, pt)) { std::printf("%-15s %-34s %-8s FAILED (final frame)\n", p.name.c_str(), feat_name(f).c_str(), v.name); continue; }   // the image read below
                if (v.zpre && !facts.cockpit_prepass && facts.cockpit_used) std::printf("WARN zpre requested but not used\n");
                double md2[kPassMarks]; for (unsigned m = 0; m < kPassMarks; ++m) md2[m] = median(dd[vi * kPassMarks + m]);
                std::string cmp = "";
                double max_err = 0, bad_frac = 0;
                if (compare || png_out) {
                    std::vector<uint8_t> px;
                    if (g.readback(tgt[0].Get(), px)) {
                        if (ref.empty()) { ref = px; cmp = "(reference)"; }
                        else {
                            size_t bad = 0; int me = 0;
                            for (size_t i = 0; i < px.size(); i += 4) {
                                int e = 0; for (int c = 0; c < 3; ++c) e = std::max(e, std::abs(int(px[i + c]) - int(ref[i + c])));
                                me = std::max(me, e); if (e > 2) ++bad;
                            }
                            max_err = me / 255.0; bad_frac = double(bad) / double(px.size() / 4);
                            char b[96]; std::snprintf(b, sizeof b, "max %d/255, %.4f%% > 2/255", me, 100.0 * bad_frac); cmp = b;
                        }
                        if (&f == &feats[0] || !user_poses.empty()) {   // PROPS-SHADING: every feature for --pose   // the full configuration only (uncompressed PNGs are ~13 MB each)
                            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
                            std::vector<uint8_t> enc;
                            const std::string png = out_dir + "\\" + p.name + "_" + v.name + (user_poses.empty() ? std::string() : "_" + feat_name(f)) + ".png";
                            if (acz::gpu_copy::step3::png_encode(px.data(), W, H, uint64_t(W) * 4, false, enc)) {
                                if (FILE* fp = std::fopen(png.c_str(), "wb")) { std::fwrite(enc.data(), 1, enc.size(), fp); std::fclose(fp); }
                            }
                            // a 1/3-scale preview (3x3 box in the stored sRGB bytes; viewing only)
                            const unsigned sw = W / 3, sh = H / 3;
                            std::vector<uint8_t> sm(size_t(sw) * sh * 4, 255);
                            for (unsigned y = 0; y < sh; ++y) for (unsigned x = 0; x < sw; ++x) for (int c = 0; c < 3; ++c) {
                                unsigned acc = 0;
                                for (unsigned yy = 0; yy < 3; ++yy) for (unsigned xx = 0; xx < 3; ++xx) acc += px[(size_t(y * 3 + yy) * W + x * 3 + xx) * 4 + c];
                                sm[(size_t(y) * sw + x) * 4 + c] = uint8_t(acc / 9);
                            }
                            if (acz::gpu_copy::step3::png_encode(sm.data(), sw, sh, uint64_t(sw) * 4, false, enc)) {
                                if (FILE* fp = std::fopen((out_dir + "\\" + p.name + "_" + v.name + (user_poses.empty() ? std::string() : "_" + feat_name(f)) + "_small.png").c_str(), "wb")) { std::fwrite(enc.data(), 1, enc.size(), fp); std::fclose(fp); }
                            }
                        }
                    }
                }
                std::printf("%-15s %-34s %-8s %7.3f | %7.3f %7.3f %7.3f %7.3f | %7.3f %7.3f %7.3f %7.3f | %8.3f %7u %s\n", p.name.c_str(), feat_name(f).c_str(), v.name, md2[kPassProps],
                    md2[kPassEyePre(0)], md2[kPassEyeComposite(0)], md2[kPassEyeModel(0)], md2[kPassEyeGlass(0)],
                    md2[kPassEyePre(1)], md2[kPassEyeComposite(1)], md2[kPassEyeModel(1)], md2[kPassEyeGlass(1)], md2[0], facts.props_instances, cmp.c_str());
                if (fcsv) std::fprintf(fcsv, "%s,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%.4f,%.6f\n", p.name.c_str(), feat_name(f).c_str(), v.name, md2[kPassProps],
                    md2[kPassEyePre(0)], md2[kPassEyeComposite(0)], md2[kPassEyeModel(0)], md2[kPassEyeGlass(0)],
                    md2[kPassEyePre(1)], md2[kPassEyeComposite(1)], md2[kPassEyeModel(1)], md2[kPassEyeGlass(1)], md2[0], facts.props_instances, max_err, bad_frac);
            }
        }
    }
    if (fcsv) std::fclose(fcsv);
    blit->destroy();
    return 0;
}
