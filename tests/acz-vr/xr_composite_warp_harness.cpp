// xr_composite_warp_harness.cpp -- executes the REAL xr_stereo D3D12 blit backend (xr_stereo_d3d12.cpp) and the REAL embedded
// xr_stereo_blit.hlsl bytecode on the Windows WARP software adapter, with synthetic producer images, and checks the pixels read
// back from a render-target "swapchain image" against (a) hand-derived semantic expectations and (b) the CPU mirrors in
// xr_stereo_output.cpp (shade, shade_hud, world_ray_uv, world_ray_extend, effects_over_world, edge_ramp, debug_view_rgb).
//
// No game, no SteamVR, no OpenXR runtime: the BlitBackend interface (xr_stereo_output.h) is the existing seam. The one link-time
// seam is acz::nativeDispatch (normally exported by d3d12_hook.cpp, the saved-original vtable lookup); this harness defines it as the
// plain vtable lookup, which is exactly what it returns for an unhooked object. Production code is unchanged.
//
// If WARP cannot create a D3D12 device the program prints "SKIP" and exits 0.
// Options: --out <dir> (PNG output folder, default warp-composite-out), --docs <dir> (also write the representative sheets there).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gpu_copy_step3.h" // png_encode: the encoder the proxy's xr_png_once sink uses
#include "xr_owner.h"
#include "xr_stereo_output.h"
#include "xr_stereo_d3d12.h"
#include "lens_flare.h"   // LENS-FLARE: the CPU mirror of lf_over
#include "eye_res.h"      // WORLD-WINDOW FILTER: filter_select / filter_offset (CPU mirror of world_rgb)

namespace acz {
// Test-side definition of the seam declared in d3d12_hook.h: nothing is hooked here, so the saved native IS the live vtable entry.
void* nativeDispatch(void* object, int slot) noexcept {
    if (!object || slot < 0) return nullptr;
    return (*reinterpret_cast<void***>(object))[slot];
}
// PERF: the props init worker brackets its device calls with the proxy creation scope (d3d12_hook.cpp); nothing is hooked here. Counted so the
// harness can prove the worker entered and left it.
static std::atomic<int> g_scope_enter{0}, g_scope_leave{0};
void enterProxyCreationScope(bool) noexcept { ++g_scope_enter; }
void leaveProxyCreationScope(bool) noexcept { ++g_scope_leave; }
} // namespace acz

using Microsoft::WRL::ComPtr;
using namespace acz::xr_stereo;
namespace xr_owner = acz::xr_owner;

// ------------------------------------------------------------------ checks
static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
#define INFO(...) do { std::printf("  "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

// ------------------------------------------------------------ number utils
static uint16_t f2h(float f) {
    uint32_t x; std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int e = int((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (((x >> 23) & 0xff) == 0xff) return uint16_t(sign | 0x7c00u | (m ? 0x200u : 0u)); // Inf / NaN
    if (e <= 0) {
        if (e < -10) return uint16_t(sign);
        m |= 0x800000u;
        const int shift = 14 - e;
        uint32_t r = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (r & 1u))) ++r;
        return uint16_t(sign | r);
    }
    if (e >= 31) return uint16_t(sign | 0x7c00u);
    uint32_t r = (uint32_t(e) << 10) | (m >> 13);
    const uint32_t rem = m & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (r & 1u))) ++r;
    return uint16_t(sign | r);
}
static float h2f(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 31u, m = h & 1023u, x;
    if (e == 0) {
        if (!m) x = sign;
        else { int s = 0; while (!(m & 1024u)) { m <<= 1; ++s; } m &= 1023u; x = sign | (uint32_t(127 - 15 + 1 - s) << 23) | (m << 13); }
    } else if (e == 31) x = sign | 0x7f800000u | (m << 13);
    else x = sign | ((e + 127 - 15) << 23) | (m << 13);
    float f; std::memcpy(&f, &x, 4); return f;
}
static float q16(float f) { return h2f(f2h(f)); }
static float sat(float x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }
static float srgb_enc(float l) { l = sat(l); return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f; }
static float srgb_dec(float s) { return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); }
static float deg2rad(float d) { return d * 3.14159265358979f / 180.0f; }

// ------------------------------------------------------------- CPU images
constexpr int W = 320, H = 180;
struct Img {
    int w = 0, h = 0;
    std::vector<float> p; // rgba, FP16-quantised
    Img() = default;
    Img(int w_, int h_) : w(w_), h(h_), p(size_t(w_) * h_ * 4, 0.0f) {}
    float* at(int x, int y) { return &p[(size_t(y) * w + x) * 4]; }
    const float* at(int x, int y) const { return &p[(size_t(y) * w + x) * 4]; }
};
static void fill(Img& im, int x0, int y0, int x1, int y1, float r, float g, float b, float a) {
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { float* q = im.at(x, y); q[0] = r; q[1] = g; q[2] = b; q[3] = a; }
}
static void quantise(Img& im) { for (float& f : im.p) f = q16(f); }
struct DRect { int x0, y0, x1, y1; float v; };
struct Depth { int w = W, h = H; std::vector<float> d = std::vector<float>(size_t(W) * H, 0.0f); std::vector<DRect> rects; }; // rects: replayed as DSV rect clears
static void fill_depth(Depth& d, int x0, int y0, int x1, int y1, float v) {
    d.rects.push_back({x0, y0, x1, y1, v});
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) d.d[size_t(y) * d.w + x] = v;
}
static void bilinear(const Img& im, float u, float v, float out[4]) {
    const float x = u * float(im.w) - 0.5f, y = v * float(im.h) - 0.5f;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    auto get = [&](int xi, int yi) { xi = std::min(std::max(xi, 0), im.w - 1); yi = std::min(std::max(yi, 0), im.h - 1); return im.at(xi, yi); };
    const float *a = get(x0, y0), *b = get(x0 + 1, y0), *c = get(x0, y0 + 1), *d = get(x0 + 1, y0 + 1);
    for (int i = 0; i < 4; ++i) out[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy;
}

// ------------------------------------------------- J-world-cache CPU model
// An independent CPU implementation of the cache write / fill measurement / read, written from the documented algorithm (docs/review/world-cache-impl.md) with the
// production GEOMETRY helpers (world_cache_*) only for the frame maths those helpers define. The GPU result is compared against it (tolerance covers the
// R11G11B10 quantisation of the cache, ~3 % of a channel, and the D3D 8-bit bilinear weights).
static const float kPiH = 3.14159265358979f;
struct CacheModel {
    int cw = 0, ch = 0;
    std::vector<float> col, stamp;
    float sky[3] = {0, 0, 0}, ground[3] = {0, 0, 0}, hz[2] = {0, 0};
    void init(int w, int h) { cw = w; ch = h; col.assign(size_t(w) * h * 3, 0.0f); stamp.assign(size_t(w) * h, 0.0f); for (float& f : sky) f = 0; for (float& f : ground) f = 0; hz[0] = hz[1] = 0; }
    static void tex(const Img& im, float u, float v, float o[3]) { float t[4]; bilinear(im, u, v, t); for (int i = 0; i < 3; ++i) o[i] = t[i] > 0.0f ? t[i] : 0.0f; }
    void measure_fill(const Img& wd, const float lens[4], float ly, float lp) {
        for (int k = 0; k < 2; ++k) {
            float acc[3] = {0, 0, 0};
            for (int r = 0; r < 4; ++r) {
                const float v = k == 0 ? 0.02f + 0.02f * float(r) : 0.98f - 0.02f * float(r);
                for (int c = 0; c < 16; ++c) { float t[3]; tex(wd, (float(c) + 0.5f) / 16.0f, v, t); for (int i = 0; i < 3; ++i) acc[i] += t[i]; }
            }
            for (int i = 0; i < 3; ++i) (k == 0 ? sky : ground)[i] = q16(acc[i] / 64.0f);
        }
        float pre[17][3] = {};
        for (int r = 0; r < 16; ++r) {
            float row[3] = {0, 0, 0};
            for (int k = 0; k < 8; ++k) { float t[3]; tex(wd, 0.15f + 0.1f * float(k), (float(r) + 0.5f) / 16.0f, t); for (int i = 0; i < 3; ++i) row[i] += t[i]; }
            for (int i = 0; i < 3; ++i) pre[r + 1][i] = pre[r][i] + row[i] * 0.125f;
        }
        float best = 0.0f, bests = 8.0f;
        for (int s = 3; s <= 13; ++s) {
            float stp = 0;
            for (int i = 0; i < 3; ++i) stp += std::fabs(pre[s][i] / float(s) - (pre[16][i] - pre[s][i]) / float(16 - s));
            if (stp > best) { best = stp; bests = float(s); }
        }
        const float vh = bests / 16.0f;
        const float tany = lens[2] - vh * (lens[2] + lens[3]);               // the world row vh -> tan(elevation) in the window frame
        const float dd[3] = {0.0f, tany, -1.0f}; float g[3]; world_cache_to_cache(ly, lp, dd, g);
        hz[0] = q16(std::atan2(g[1], std::sqrt(g[0] * g[0] + g[2] * g[2]))); hz[1] = world_cache_horizon_conf(best);
    }
    void write(const Img& wd, const float lens[4], float ly, float lp, float now) {
        const float ku = 1.0f / (lens[0] + lens[1]), kv = 1.0f / (lens[2] + lens[3]);
        const float qu = 0.25f * (2.0f * kPiH / float(cw)) * ku, qv = 0.25f * (kPiH / float(ch)) * kv;
        const CacheRect r = world_cache_update_rect(lens, ly, lp, uint32_t(cw), uint32_t(ch));
        for (uint32_t y = r.y0; y < r.y1; ++y) for (uint32_t x = r.x0; x < r.x1; ++x) {
            const CacheWindowSample s = world_cache_texel_window(lens, ly, lp, (float(x) + 0.5f) / float(cw), (float(y) + 0.5f) / float(ch));
            if (!s.hit) continue;
            const float off[4][2] = {{-qu, -qv}, {qu, -qv}, {-qu, qv}, {qu, qv}};
            float acc[3] = {0, 0, 0};
            for (const auto& o : off) { float t[4]; bilinear(wd, s.u + o[0], s.v + o[1], t); for (int i = 0; i < 3; ++i) acc[i] += t[i]; }
            bool fin = true; for (int i = 0; i < 3; ++i) { acc[i] *= 0.25f; fin = fin && std::isfinite(acc[i]); }
            if (!fin) continue;
            for (int i = 0; i < 3; ++i) col[(size_t(y) * cw + x) * 3 + i] = std::min(std::max(acc[i], 0.0f), kCacheMaxValue);
            stamp[size_t(y) * cw + x] = now;
        }
        measure_fill(wd, lens, ly, lp);
    }
    void fill_color(const float g[3], float out[3]) const {
        const float el = std::atan2(g[1], std::sqrt(g[0] * g[0] + g[2] * g[2]));
        const float t = world_cache_fill_t(el, hz[0], hz[1]);
        for (int i = 0; i < 3; ++i) out[i] = ground[i] + (sky[i] - ground[i]) * t;
    }
    void read(const float d[3], float ly, float lp, float now, float out[3]) const {
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float dn[3] = {d[0] / len, d[1] / len, d[2] / len};
        float g[3]; world_cache_to_cache(ly, lp, dn, g);
        float u, v; world_cache_dir_uv(g, u, v);
        const float px = u * float(cw) - 0.5f, py = v * float(ch) - 0.5f;
        const float fx0 = std::floor(px), fy0 = std::floor(py), fx = px - fx0, fy = py - fy0;
        const int x0 = int(fx0), y0 = int(fy0);
        float acc[3] = {0, 0, 0}, wsum = 0.0f;
        for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
            const int ix = (x0 + i + cw) % cw, iy = std::min(std::max(y0 + j, 0), ch - 1);
            const float wb = (i == 0 ? 1.0f - fx : fx) * (j == 0 ? 1.0f - fy : fy);
            const float a = world_cache_age_weight(stamp[size_t(iy) * cw + ix], now);
            for (int k = 0; k < 3; ++k) acc[k] += wb * a * col[(size_t(iy) * cw + ix) * 3 + k];
            wsum += wb * a;
        }
        float fc[3]; fill_color(g, fc);
        const float rest = 1.0f - sat(wsum);
        for (int k = 0; k < 3; ++k) out[k] = acc[k] + rest * fc[k];
    }
};

// -------------------------------------------------------------- PNG output
static uint32_t crc32_of(const uint8_t* p, size_t n) {
    static uint32_t t[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = true; }
    uint32_t c = 0xFFFFFFFFu; for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 255u] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}
static uint32_t adler32_of(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0; for (size_t i = 0; i < n; ++i) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; } return (b << 16) | a;
}
struct BitW {
    std::vector<uint8_t> out; uint32_t acc = 0; int n = 0;
    void bit(uint32_t b) { acc |= (b & 1u) << n; if (++n == 8) { out.push_back(uint8_t(acc)); acc = 0; n = 0; } }
    void bits(uint32_t v, int c) { for (int i = 0; i < c; ++i) bit(v >> i); }
    void huff(uint32_t code, int len) { for (int i = len - 1; i >= 0; --i) bit(code >> i); }
    void lit(uint32_t s) { if (s < 144) huff(0x30 + s, 8); else if (s < 256) huff(0x190 + (s - 144), 9); else if (s < 280) huff(s - 256, 7); else huff(0xC0 + (s - 280), 8); }
    void flush() { while (n) bit(0); }
};
static std::vector<uint8_t> deflate_fixed(const std::vector<uint8_t>& d) {
    static const int lb[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int le[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int db[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int de[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    BitW w; w.bits(1, 1); w.bits(1, 2);
    const size_t n = d.size();
    for (size_t i = 0; i < n;) {
        int best = 0, bd = 0;
        for (int dist = 1; dist <= 64 && size_t(dist) <= i; ++dist) {
            int len = 0; while (i + size_t(len) < n && len < 258 && d[i + size_t(len)] == d[i + size_t(len) - size_t(dist)]) ++len;
            if (len > best) { best = len; bd = dist; }
        }
        if (best >= 3) {
            int li = 28; while (lb[li] > best) --li;
            w.lit(257 + uint32_t(li)); w.bits(uint32_t(best - lb[li]), le[li]);
            int di = 29; while (db[di] > bd) --di;
            w.huff(uint32_t(di), 5); w.bits(uint32_t(bd - db[di]), de[di]);
            i += size_t(best);
        } else { w.lit(d[i]); ++i; }
    }
    w.lit(256); w.flush();
    return w.out;
}
// 8-bit RGB PNG, filter Up + fixed-Huffman LZ77 (synthetic images compress to a few KiB).
static bool write_png_rgb(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h) {
    std::vector<uint8_t> raw; raw.reserve(size_t(w) * h * 3 + h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(y ? 2 : 0);
        for (int x = 0; x < w * 3; ++x) {
            const uint8_t c = rgb[size_t(y) * w * 3 + x], up = y ? rgb[size_t(y - 1) * w * 3 + x] : 0;
            raw.push_back(uint8_t(c - up));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x9C}; { auto df = deflate_fixed(raw); z.insert(z.end(), df.begin(), df.end()); }
    const uint32_t ad = adler32_of(raw.data(), raw.size()); for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(ad >> s));
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    auto be32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); };
    auto chunk = [&](const char* t, const uint8_t* p, size_t n) {
        be32(uint32_t(n)); const size_t st = out.size(); out.insert(out.end(), t, t + 4); if (n) out.insert(out.end(), p, p + n);
        be32(crc32_of(out.data() + st, n + 4));
    };
    uint8_t ihdr[13] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr, 13); chunk("IDAT", z.data(), z.size()); chunk("IEND", nullptr, 0);
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "wb") || !f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size(); std::fclose(f); return ok;
}

// -------------------------------------------------------------- D3D12 glue
struct Gpu {
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q; ComPtr<ID3D12CommandAllocator> al; ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence; HANDLE ev = nullptr; UINT64 fv = 0;
    bool init(std::string& why) {
        ComPtr<IDXGIFactory4> fac;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { why = "CreateDXGIFactory1 failed"; return false; }
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(fac->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) { why = "EnumWarpAdapter failed"; return false; }
        HRESULT hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        if (FAILED(hr)) { char b[64]; std::snprintf(b, sizeof b, "D3D12CreateDevice(WARP) hr=0x%08lx", (unsigned long)hr); why = b; return false; }
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { why = "queue/list/fence creation failed"; return false; }
        cl->Close();
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DXGI_ADAPTER_DESC ad{}; warp->GetDesc(&ad); char nm[128]; WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, nm, sizeof nm, nullptr, nullptr);
        std::printf("adapter: %s\n", nm);
        return ev != nullptr;
    }
    bool begin() { return SUCCEEDED(al->Reset()) && SUCCEEDED(cl->Reset(al.Get(), nullptr)); }
    bool submit() {
        if (FAILED(cl->Close())) return false;
        ID3D12CommandList* l[] = {cl.Get()};
        q->ExecuteCommandLists(1, l);
        if (FAILED(q->Signal(fence.Get(), ++fv))) return false;
        if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); if (WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0) return false; }
        return SUCCEEDED(dev->GetDeviceRemovedReason());
    }
    ComPtr<ID3D12Resource> tex(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = f; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = flags;
        ComPtr<ID3D12Resource> r; dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)); return r;
    }
    ComPtr<ID3D12Resource> buffer(UINT64 n, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = n; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r; dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)); return r;
    }
    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb{}; rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; rb.Transition.pResource = r;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; rb.Transition.StateBefore = a; rb.Transition.StateAfter = b; cl->ResourceBarrier(1, &rb);
    }
    // Resource is in `st`; contents replaced from tightly packed rows, returned to `st`.
    bool upload(ID3D12Resource* t, const void* data, UINT row_bytes, D3D12_RESOURCE_STATES st) {
        const D3D12_RESOURCE_DESC rd = t->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rb = 0, total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rb, &total);
        auto up = buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* m = nullptr; if (!up || FAILED(up->Map(0, nullptr, reinterpret_cast<void**>(&m)))) return false;
        for (UINT r = 0; r < rows; ++r) std::memcpy(m + size_t(fp.Footprint.RowPitch) * r, static_cast<const uint8_t*>(data) + size_t(row_bytes) * r, row_bytes);
        up->Unmap(0, nullptr);
        if (!begin()) return false;
        barrier(t, st, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = t; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(t, D3D12_RESOURCE_STATE_COPY_DEST, st);
        return submit();
    }
    // RGBA8 resource in RENDER_TARGET -> tight rows (raw bytes: the sRGB-encoded values the RTV wrote).
    bool readback(ID3D12Resource* t, std::vector<uint8_t>& out) {
        const D3D12_RESOURCE_DESC rd = t->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rb = 0, total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rb, &total);
        auto rbk = buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!rbk || !begin()) return false;
        barrier(t, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rbk.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        src.pResource = t; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(t, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!submit()) return false;
        uint8_t* m = nullptr; if (FAILED(rbk->Map(0, nullptr, reinterpret_cast<void**>(&m)))) return false;
        out.assign(size_t(rd.Width) * rd.Height * 4, 0);
        for (UINT r = 0; r < rows; ++r) std::memcpy(out.data() + size_t(r) * rd.Width * 4, m + size_t(fp.Footprint.RowPitch) * r, size_t(rd.Width) * 4);
        D3D12_RANGE none{0, 0}; rbk->Unmap(0, &none);
        return true;
    }
};
static Gpu g;

// A producer image that lives on the GPU (RENDER_TARGET for colour, DEPTH_WRITE for depth: the producer's publish contract).
static ComPtr<ID3D12Resource> make_color(const Img& im) {
    auto t = g.tex(UINT(im.w), UINT(im.h), DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    std::vector<uint16_t> h(im.p.size()); for (size_t i = 0; i < h.size(); ++i) h[i] = f2h(im.p[i]);
    if (!t || !g.upload(t.Get(), h.data(), UINT(im.w) * 8, D3D12_RESOURCE_STATE_RENDER_TARGET)) { std::printf("make_color failed\n"); std::exit(2); }
    return t;
}
// Depth is produced by DSV clears (whole image to 0, then each rectangle): works for every depth(+stencil) format, no buffer->depth copy needed.
// `res` = the typeless resource format the producer publishes (D-240), `dsv` = its depth-stencil view format.
static ComPtr<ID3D12Resource> make_depth(const Depth& d, DXGI_FORMAT res = DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT dsv = DXGI_FORMAT_D32_FLOAT) {
    auto t = g.tex(UINT(d.w), UINT(d.h), res, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    if (!t || FAILED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) { std::printf("make_depth failed (resource/heap)\n"); std::exit(2); }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = dsv; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g.dev->CreateDepthStencilView(t.Get(), &dv, heap->GetCPUDescriptorHandleForHeapStart());
    if (!g.begin()) std::exit(2);
    g.cl->ClearDepthStencilView(heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
    for (const DRect& r : d.rects) {
        const D3D12_RECT rc{r.x0, r.y0, r.x1, r.y1};
        g.cl->ClearDepthStencilView(heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, r.v, 0, 1, &rc);
    }
    if (!g.submit()) { std::printf("make_depth failed (clear)\n"); std::exit(2); }
    return t;
}

// -------------------------------------------------------------- scene data
struct Scene {
    Img eye[2], world, effects, effects_cov, hud, markers;
    Depth depth[2], bare[2];                 // bare = depth 0 everywhere (no cockpit coverage)
    ComPtr<ID3D12Resource> eye_t[2], depth_t[2], bare_t[2], world_t, effects_t, effects_cov_t, hud_t, markers_t;
};
// J-effects-dual: CPU accumulation oracle of the producer's two summaries for the game's dual-source blend (RGB SRC_ALPHA/INV_SRC_ALPHA,
// alpha SRC1_ALPHA/INV_SRC1_ALPHA). A draw = output 0 (S, a) and output 1 alpha b. Later draws go over earlier ones.
//   P' = a*S + (1-a)*P   A' = a + (1-a)*A (RGB coverage)   G' = a*b + (1-b)*G (the game's stored alpha, NOT coverage)   U' = (1-b)*U
// RGB_final = P + (1-A)*world.rgb. Written independently of the production mirror (never calls it to derive expectations).
struct Acc { float P[3] = {0, 0, 0}, A = 0, G = 0, U = 1; };
static void accumulate(Acc& r, const float S[3], float a, float b) {
    for (int i = 0; i < 3; ++i) r.P[i] = a * S[i] + (1 - a) * r.P[i];
    r.A = a + (1 - a) * r.A; r.G = a * b + (1 - b) * r.G; r.U = (1 - b) * r.U;
}
// Cockpit (per eye): bottom panel y 105..179 x 60..259, left pillar x 70..89, HDR patch, dim patch, a depth hole in the panel (canopy glass),
// and a colour patch with depth 0 (replay colour the depth rule must discard). The FP16 alpha is 1.0 where nothing is drawn and 0.25 where
// the cockpit is: the replay alpha is NOT a coverage mask (D-229), the shader must ignore it.
static void build_scene(Scene& s) {
    for (int e = 0; e < 2; ++e) {
        s.eye[e] = Img(W, H); fill(s.eye[e], 0, 0, W, H, 0, 0, 0, 1.0f);
        const float c0[3] = {e ? 0.1f : 0.8f, 0.2f, e ? 0.8f : 0.1f};
        fill(s.eye[e], 60, 105, 260, 180, c0[0], c0[1], c0[2], 0.25f); fill_depth(s.depth[e], 60, 105, 260, 180, 0.5f);
        fill(s.eye[e], 70, 0, 90, 105, c0[0] * 0.5f, c0[1] * 0.5f, c0[2] * 0.5f, 0.25f); fill_depth(s.depth[e], 70, 0, 90, 105, 0.5f);
        fill(s.eye[e], 120, 120, 140, 140, 3.0f, 3.0f, 3.0f, 0.25f);   // HDR (clamp -> 1, Reinhard -> 0.75)
        fill(s.eye[e], 160, 120, 180, 140, 0.05f, 0.05f, 0.05f, 0.25f); // dark (sRGB toe)
        fill(s.eye[e], 190, 108, 200, 118, 0.0f, 0.0f, 0.0f, 1.0f); fill_depth(s.depth[e], 190, 108, 200, 118, 0.0f); // depth hole in the panel
        fill(s.eye[e], 10, 10, 40, 40, 0.0f, 0.5f, 0.0f, 1.0f);        // colour with depth 0: discarded
        quantise(s.eye[e]);
    }
    s.world = Img(W, H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float* p = s.world.at(x, y); p[0] = float(x) / (W - 1); p[1] = float(y) / (H - 1); p[2] = 0.25f; p[3] = 1.0f; }
    fill(s.world, 10, 10, 20, 20, 0, 0, 1, 1);          // blue: top-left
    fill(s.world, W - 20, H - 20, W - 10, H - 10, 1, 0, 1, 1); // magenta: bottom-right
    fill(s.world, 155, 85, 165, 95, 1, 1, 1, 1);        // white: centre
    fill(s.world, 250, 20, 260, 30, 1, 1, 0, 1);        // yellow: top-right-ish
    quantise(s.world);
    s.effects = Img(W, H);
    fill(s.effects, 40, 30, 80, 60, 0.6f, 0.0f, 0.0f, 0.6f);      // R: solid red, premultiplied a = 0.6
    fill(s.effects, 230, 60, 280, 100, 0.0f, 0.25f, 0.0f, 0.5f);  // G: translucent green
    fill(s.effects, 140, 20, 170, 40, 0.8f, 0.8f, 0.8f, 0.0f);    // W: bright, alpha channel 0 (alpha is not a coverage mask)
    fill(s.effects, 130, 100, 190, 140, 0.9f, 0.9f, 0.0f, 0.9f);  // Y: under the cockpit panel
    quantise(s.effects);
    // coverage_v1 effects image (J-effects-dual): (P premultiplied, A coverage) accumulated by the oracle from literal draws, same blob positions.
    s.effects_cov = Img(W, H);
    {   const float black[3] = {0, 0, 0}, s1[3] = {0.5f, 0.25f, 0}, s2[3] = {0, 0.5f, 1};
        Acc f1; accumulate(f1, black, 0.25f, 0.5f);                          // finding 1: out0 colour black, alpha 0.25, src1 alpha 0.5
        fill(s.effects_cov, 40, 30, 80, 60, f1.P[0], f1.P[1], f1.P[2], f1.A);
        Acc r1; accumulate(r1, s1, 0.25f, 0.0f);
        fill(s.effects_cov, 230, 60, 280, 100, r1.P[0], r1.P[1], r1.P[2], r1.A);
        Acc md; accumulate(md, s1, 0.25f, 0.5f); accumulate(md, s2, 0.5f, 0.75f);   // ordered multi-draw
        fill(s.effects_cov, 140, 20, 170, 40, md.P[0], md.P[1], md.P[2], md.A);
        Acc full; accumulate(full, s1, 1.0f, 0.25f);                          // a = 1: opaque
        fill(s.effects_cov, 130, 100, 190, 140, full.P[0], full.P[1], full.P[2], full.A);
        quantise(s.effects_cov); }
    s.hud = Img(W, H);
    fill(s.hud, 20, 150, 60, 170, 0.5f, 0.0f, 0.0f, 0.5f); fill(s.hud, 100, 10, 140, 30, 0.6f, 0.6f, 0.6f, 0.0f); fill(s.hud, 200, 80, 240, 120, 0.0f, 0.3f, 0.0f, 0.3f);
    quantise(s.hud);
    s.markers = Img(W, H);
    fill(s.markers, 150, 40, 170, 60, 1.0f, 1.0f, 0.0f, 1.0f); fill(s.markers, 250, 140, 270, 160, 0.0f, 0.4f, 0.4f, 0.4f);
    quantise(s.markers);
    for (int e = 0; e < 2; ++e) {
        s.eye_t[e] = make_color(s.eye[e]);
        s.depth_t[e] = make_depth(s.depth[e]); // the producer publishes the typeless resource format (D-240: 39)
        s.bare_t[e] = make_depth(s.bare[e]);
    }
    s.world_t = make_color(s.world); s.effects_t = make_color(s.effects); s.effects_cov_t = make_color(s.effects_cov); s.hud_t = make_color(s.hud); s.markers_t = make_color(s.markers);
}

// ------------------------------------------------------------ backend glue
struct Chains {
    std::shared_ptr<BlitBackend> blit; ComPtr<ID3D12Resource> img[kTargets][2]; int64_t format = 0; bool ok = false;
    UINT64 sv = 0; int cw = W, ch = H;
};
static std::unique_ptr<Chains> make_chains(int64_t fmt, int cw = W, int chh = H) {
    auto c = std::make_unique<Chains>(); c->format = fmt; c->cw = cw; c->ch = chh;
    c->blit = make_d3d12_blit(g.dev.Get(), g.q.Get());
    if (!c->blit->setup()) { std::printf("blit setup() failed\n"); return c; }
    const DXGI_FORMAT rf = fmt == rgba8_srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    for (unsigned t = 0; t < kTargets; ++t) {
        SwapchainImages si; si.count = 2;
        for (unsigned i = 0; i < 2; ++i) {
            c->img[t][i] = g.tex(UINT(cw), UINT(chh), rf, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
            si.images[i] = c->img[t][i].Get();
        }
        if (!c->blit->bind(t, si, UINT(cw), UINT(chh), fmt)) { std::printf("bind(%u) failed\n", t); return c; }
    }
    c->ok = true; return c;
}

struct Result { RecordFacts facts; bool ok = false; std::vector<uint8_t> px[kTargets]; PngCapture png; bool png_taken = false; double record_ms = 0; };

// Poison the images the job will write (so a pixel the pass fails to write shows), record, execute, wait, read back.
static Result run_job(Chains& c, BlitJob job, unsigned read_mask, bool want_png = false) {
    Result r;
    const std::vector<uint8_t> poison(size_t(c.cw) * c.ch * 4, 0xCD);
    for (unsigned t = 0; t < kTargets; ++t) if (read_mask & (1u << t)) g.upload(c.img[t][job.index[t]].Get(), poison.data(), UINT(c.cw) * 4, D3D12_RESOURCE_STATE_RENDER_TARGET);
    LARGE_INTEGER q0, q1, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    r.facts = c.blit->record(job);
    QueryPerformanceCounter(&q1); r.record_ms = double(q1.QuadPart - q0.QuadPart) * 1000.0 / double(qf.QuadPart);
    if (!r.facts.ok) { std::printf("record() failed\n"); return r; }
    bool dispatch = false;
    if (!c.blit->close() || !c.blit->execute(dispatch) || !c.blit->signal(++c.sv)) { std::printf("close/execute/signal failed\n"); return r; }
    uint64_t done = 0; const DWORD t0 = GetTickCount();
    while (true) {
        if (!c.blit->sample(done)) { std::printf("sample() invalid (device removed?)\n"); return r; }
        if (done >= c.sv) break;
        if (GetTickCount() - t0 > 30000) { std::printf("timeout waiting for the private fence\n"); return r; }
        Sleep(1);
    }
    if (want_png && r.facts.png_recorded) r.png_taken = c.blit->png_take(r.png);
    for (unsigned t = 0; t < kTargets; ++t) if (read_mask & (1u << t)) if (!g.readback(c.img[t][job.index[t]].Get(), r.px[t])) { std::printf("readback failed\n"); return r; }
    r.ok = true; return r;
}

// ------------------------------------------------------------ view setups
struct ViewSetup { xr_owner::EyeSample eye[2]; float anchor[3] = {0, 0, 0}; float yaw = 0, pitch = 0; float lens[4]; float dist = 40.0f; const char* name = ""; };
static void quat_yaw_pitch(float yaw, float pitch, float q[4]) { // Ry(yaw) * Rx(pitch)
    const float hy = 0.5f * yaw, hp = 0.5f * pitch, ay = std::sin(hy), aw = std::cos(hy), bx = std::sin(hp), bw = std::cos(hp);
    q[0] = aw * bx; q[1] = ay * bw; q[2] = -ay * bx; q[3] = aw * bw;
}
static ViewSetup make_view(const char* name, float head_yaw_deg, float head_pitch_deg, const float pos[3], float anchor_yaw, float look_pitch) {
    ViewSetup v; v.name = name; v.yaw = anchor_yaw; v.pitch = look_pitch;
    v.lens[0] = v.lens[1] = std::tan(deg2rad(31.6f)); v.lens[2] = v.lens[3] = std::tan(deg2rad(19.085f)); // 63.2 x 38.17 deg
    for (int e = 0; e < 2; ++e) {
        quat_yaw_pitch(deg2rad(head_yaw_deg), deg2rad(head_pitch_deg), v.eye[e].pose.orientation);
        v.eye[e].pose.position[0] = pos[0] + (e ? 0.032f : -0.032f); v.eye[e].pose.position[1] = pos[1]; v.eye[e].pose.position[2] = pos[2];
        v.eye[e].fov = {-deg2rad(50.0f), deg2rad(50.0f), deg2rad(40.0f), -deg2rad(40.0f)};
    }
    return v;
}
static bool params_for(const ViewSetup& v, WorldComposite out[2]) {
    for (int e = 0; e < 2; ++e) if (!world_composite_params(v.eye[e], v.anchor, v.yaw, v.lens, v.dist, out[e], v.pitch)) return false;
    return true;
}

// -------------------------------------------------------------- CPU mirror
struct Cfg {
    Tonemap tm = Tonemap::clamp; float exposure = 1.0f, feather = 0.0f;
    unsigned mode = 0;          // 0 cockpit, 2 world composite, 3 + effects, 4 + coverage_v1 effects (J-effects-dual)
    bool use_depth = true;
    WorldComposite wc{};
    float fade = 1.0f; bool extend = false; unsigned dbg = 0;
    bool occlude = false;       // J-occlusion-fix: mode_bits bit 16 (window_occlude_keep)
    bool model = false;         // COCKPIT-2: mode_bits bit 17 (model_cockpit_cover: the replayed cockpit band is dropped)
    const CacheModel* cache = nullptr; float cly = 0, clp = 0, cnow = 1;   // J-world-cache: the periphery comes from the CPU model (null = not a cache job)
};
static Shade shade_tex(const Img& im, float u, float v, const Cfg& c, bool hud) {
    float t[4]; bilinear(im, u, v, t);
    return hud ? shade_hud(t, c.tm, c.exposure) : shade(t, false, 0.0f, c.tm, c.exposure);
}
static Shade world_layer_ref(const Scene& s, const Cfg& c, float u, float v) {
    Shade z{0, 0, 0, 0};
    if (c.cache) {   // J-world-cache: the live window exactly as the extend path, the periphery from the cache model
        const WorldHit h = world_ray_uv(c.wc, u, v);
        if (h.hit) {
            Shade w = shade_tex(s.world, h.u, h.v, c, false); w = {w.r * c.fade, w.g * c.fade, w.b * c.fade, 1.0f};
            if (c.mode == 3) { float e[4]; bilinear(s.effects, h.u, h.v, e); w = effects_over_world(e, c.tm, c.exposure, 1.0f, w); }
            if (c.mode == 4) { float e[4]; bilinear(s.effects_cov, h.u, h.v, e); w = effects_coverage_over_world(e, c.tm, c.exposure, 1.0f, w); }
            return w;
        }
        float d[3]; for (int i = 0; i < 3; ++i) d[i] = c.wc.base[i] + u * c.wc.ux[i] + v * c.wc.vy[i];
        float rgb[3]; c.cache->read(d, c.cly, c.clp, c.cnow, rgb);
        const Shade w = shade(rgb, false, 0.0f, c.tm, c.exposure);
        return {w.r * c.fade, w.g * c.fade, w.b * c.fade, 1.0f};
    }
    if (!c.extend) {
        const WorldHit h = world_ray_uv(c.wc, u, v);
        if (!h.hit) return z;
        Shade w = shade_tex(s.world, h.u, h.v, c, false); w = {w.r * c.fade, w.g * c.fade, w.b * c.fade, 1.0f};
        const float ramp = edge_ramp(h.u, h.v, c.feather);
        w = apply_feather(w, ramp);
        if (c.mode == 3) { float e[4]; bilinear(s.effects, h.u, h.v, e); w = effects_over_world(e, c.tm, c.exposure, ramp, w); }
        if (c.mode == 4) { float e[4]; bilinear(s.effects_cov, h.u, h.v, e); w = effects_coverage_over_world(e, c.tm, c.exposure, ramp, w); }
        return w;
    }
    const WorldExtendSample e = world_ray_extend(c.wc, u, v);
    Shade w = shade_tex(s.world, e.u, e.v, c, false); const float k = e.k * c.fade; w = {w.r * k, w.g * k, w.b * k, 1.0f};
    if (c.mode == 3 && !e.ext) { float fx[4]; bilinear(s.effects, e.u, e.v, fx); w = effects_over_world(fx, c.tm, c.exposure, 1.0f, w); }
    if (c.mode == 4 && !e.ext) { float fx[4]; bilinear(s.effects_cov, e.u, e.v, fx); w = effects_coverage_over_world(fx, c.tm, c.exposure, 1.0f, w); }
    return w;
}
// Reference for one eye (ps_main), `d` = the depth image bound for coverage.
static Img ref_eye(const Scene& s, const Cfg& c, int eye, const Depth& d, int ow = W, int oh = H) {
    Img out(ow, oh);
    for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) {
        const float u = (float(x) + 0.5f) / float(ow), v = (float(y) + 0.5f) / float(oh);
        float t[4]; bilinear(s.eye[eye], u, v, t);
        const Shade sc = shade(t, false, 0.0f, c.tm, c.exposure); // tonemapped colour, a = 1
        const int dx = std::min(int(u * float(d.w)), d.w - 1), dy = std::min(int(v * float(d.h)), d.h - 1); // nearest texel, as the shader
        float cover = c.use_depth ? (d.d[size_t(dy) * d.w + dx] > 0.0f ? 1.0f : 0.0f) : 1.0f;
        if (c.occlude && c.use_depth && c.mode >= 2 && cover > 0.0f) cover = window_occlude_keep(c.wc, u, v, d.d[size_t(dy) * d.w + dx]); // J-occlusion-fix
        if (c.model) cover = model_cockpit_cover(c.use_depth, d.d[size_t(dy) * d.w + dx], cover);                                       // COCKPIT-2
        float* o = out.at(x, y);
        if (c.dbg) {
            const Shade wl = (c.mode == 2 || c.mode == 3) ? world_layer_ref(s, c, u, v) : Shade{0, 0, 0, 0};
            const float col[3] = {sc.r, sc.g, sc.b}, wv[4] = {wl.r, wl.g, wl.b, wl.a};
            debug_view_rgb(c.dbg, cover, col, wv, o); o[3] = 1.0f; continue;
        }
        Shade p = apply_feather({sc.r * cover, sc.g * cover, sc.b * cover, cover}, edge_ramp(u, v, c.feather));
        if (c.mode >= 2) p = composite_over_world(p, world_layer_ref(s, c, u, v));
        o[0] = p.r; o[1] = p.g; o[2] = p.b; o[3] = p.a;
    }
    return out;
}
static Img ref_hud(const Img& src, const Cfg& c) {
    Img out(W, H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const float u = (float(x) + 0.5f) / W, v = (float(y) + 0.5f) / H;
        Shade p = apply_feather(shade_tex(src, u, v, c, true), edge_ramp(u, v, c.feather));
        float* o = out.at(x, y); o[0] = p.r; o[1] = p.g; o[2] = p.b; o[3] = p.a;
    }
    return out;
}
static Img ref_quad(const Scene& s, const Cfg& c) { // world quad target: mode 0, no depth, opaque
    Img out(W, H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const float u = (float(x) + 0.5f) / W, v = (float(y) + 0.5f) / H;
        Shade sc = shade_tex(s.world, u, v, c, false);
        Shade p = apply_feather(sc, edge_ramp(u, v, c.feather));
        float* o = out.at(x, y); o[0] = p.r; o[1] = p.g; o[2] = p.b; o[3] = p.a;
    }
    return out;
}

// ----------------------------------------------------------------- compare
struct Cmp { float max_err = 0; size_t bad = 0, n = 0; int fx = -1, fy = -1; };
static Cmp compare(const std::vector<uint8_t>& gpu, const Img& ref, float tol) {
    Cmp r; r.n = size_t(ref.w) * ref.h;
    for (int y = 0; y < ref.h; ++y) for (int x = 0; x < ref.w; ++x) {
        const uint8_t* g8 = &gpu[(size_t(y) * ref.w + x) * 4]; const float* rf = ref.at(x, y);
        float e = 0;
        for (int i = 0; i < 3; ++i) e = std::max(e, std::fabs(srgb_dec(g8[i] / 255.0f) - sat(rf[i])));
        e = std::max(e, std::fabs(g8[3] / 255.0f - sat(rf[3])));
        r.max_err = std::max(r.max_err, e);
        if (e > tol) { if (!r.bad) { r.fx = x; r.fy = y; } ++r.bad; }
    }
    return r;
}
static void px_lin(const std::vector<uint8_t>& gpu, int x, int y, float o[4], int w = W) {
    const uint8_t* p = &gpu[(size_t(y) * w + x) * 4]; for (int i = 0; i < 3; ++i) o[i] = srgb_dec(p[i] / 255.0f); o[3] = p[3] / 255.0f;
}
static bool near4(const float a[4], float r, float g, float b, float al, float tol) {
    return std::fabs(a[0] - r) <= tol && std::fabs(a[1] - g) <= tol && std::fabs(a[2] - b) <= tol && std::fabs(a[3] - al) <= tol;
}
static std::string g_out = "warp-composite-out", g_docs;
static void save_sheet(const char* name, const std::vector<uint8_t>& gpu, const Img& ref, bool docs) {
    const int W = ref.w, H = ref.h;
    std::vector<uint8_t> rgb(size_t(W) * 3 * H * 3);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const uint8_t* g8 = &gpu[(size_t(y) * W + x) * 4]; const float* rf = ref.at(x, y);
        for (int i = 0; i < 3; ++i) {
            const uint8_t rb = uint8_t(std::lround(srgb_enc(sat(rf[i])) * 255.0f));
            rgb[(size_t(y) * W * 3 + x) * 3 + i] = g8[i];
            rgb[(size_t(y) * W * 3 + W + x) * 3 + i] = rb;
            rgb[(size_t(y) * W * 3 + 2 * W + x) * 3 + i] = uint8_t(std::min(255, std::abs(int(g8[i]) - int(rb)) * 16));
        }
    }
    CreateDirectoryA(g_out.c_str(), nullptr);
    write_png_rgb(g_out + "\\" + name + ".png", rgb, W * 3, H);
    if (docs && !g_docs.empty()) { CreateDirectoryA(g_docs.c_str(), nullptr); write_png_rgb(g_docs + "\\" + name + ".png", rgb, W * 3, H); }
}

// -------------------------------------------------------------- job helpers
static BlitJob base_job(const Scene& s, bool bare = false, int ow = W, int oh = H) {
    BlitJob j;
    for (int e = 0; e < 2; ++e) {
        j.eyes[e] = {s.eye_t[e].Get(), 10u, W, H};
        j.depth[e] = {(bare ? s.bare_t[e] : s.depth_t[e]).Get(), 39u, W, H};
    }
    j.use_depth = true;
    for (unsigned t = 0; t < kTargets; ++t) j.map[t] = full_rect(W, H, uint32_t(ow), uint32_t(oh));
    return j;
}
static void job_world(BlitJob& j, const Scene& s, const ViewSetup& v, bool effects) {
    j.use_world = true; j.world = {s.world_t.Get(), 10u, W, H}; j.world_composite = true;
    WorldComposite wc[2]; if (!params_for(v, wc)) { std::printf("world_composite_params refused\n"); std::exit(2); }
    j.world_eye = {wc[0], wc[1]};
    if (effects) { j.use_effects = true; j.effects = {s.effects_t.Get(), 10u, W, H}; }
}
static void job_world_cov(BlitJob& j, const Scene& s, const ViewSetup& v) { // J-effects-dual: the coverage_v1 image (shader mode 4)
    job_world(j, s, v, false);
    j.use_effects = true; j.effects_coverage = true; j.effects = {s.effects_cov_t.Get(), 10u, W, H};
}
static Cfg cfg_for(const BlitJob& j, int eye) {
    Cfg c; c.tm = j.tonemap; c.exposure = j.exposure; c.feather = clamp_feather(j.edge_feather); c.use_depth = j.use_depth;
    c.mode = j.use_world && j.world_composite ? (j.use_effects ? (j.effects_coverage ? 4u : 3u) : 2u) : 0u;
    if (c.mode) c.wc = j.world_eye[eye];
    c.fade = std::isfinite(j.world_fade) ? std::min(1.0f, std::max(0.0f, j.world_fade)) : 0.0f; c.extend = j.world_extend; c.dbg = j.debug_view;
    c.occlude = j.occlude_window && j.use_depth;
    return c;
}
// Run + compare both eyes; returns the result for further assertions.
static Result eye_case(Chains& ch, const Scene& s, const char* name, const BlitJob& job, bool bare, float tol, size_t allow_bad, bool sheet = true, bool docs = false) {
    const int ow = ch.cw, oh = ch.ch;
    Result r = run_job(ch, job, 3u);
    CHECK(r.ok && r.facts.ok, "%s: job did not run", name);
    if (!r.ok) return r;
    CHECK(r.facts.sources_restored && r.facts.swapchain_untouched && r.facts.barriers_out == r.facts.barriers_back, "%s: barrier contract", name);
    CHECK(r.facts.draws >= 2, "%s: draws=%u", name, r.facts.draws);
    for (int e = 0; e < 2; ++e) {
        const Cfg c = cfg_for(job, e);
        const Img ref = ref_eye(s, c, e, bare ? s.bare[e] : s.depth[e], ow, oh);
        const Cmp m = compare(r.px[e], ref, tol);
        INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu%s", name, e, m.max_err, m.bad, m.n, m.bad ? "" : "");
        CHECK(m.bad <= allow_bad, "%s eye%d: %zu pixels differ from the CPU mirror by > %.3f (first at %d,%d)", name, e, m.bad, tol, m.fx, m.fy);
        if (sheet && e == 0) save_sheet(name, r.px[0], ref, docs);
    }
    return r;
}

// ------------------------------------------------------------------- cases
static void case_mode0(Chains& ch, const Scene& s) {
    std::printf("[a] mode 0 cockpit-only identity\n");
    BlitJob j = base_job(s);
    Result r = eye_case(ch, s, "a_mode0_clamp", j, false, 0.006f, 0);
    if (!r.ok) return;
    float p[4];
    for (int e = 0; e < 2; ++e) {
        const float c0[3] = {e ? 0.1f : 0.8f, 0.2f, e ? 0.8f : 0.1f};
        px_lin(r.px[e], 100, 150, p);  CHECK(near4(p, q16(c0[0]), q16(c0[1]), q16(c0[2]), 1.0f, 0.006f), "eye%d panel colour %.3f %.3f %.3f a=%.3f", e, p[0], p[1], p[2], p[3]);
        px_lin(r.px[e], 300, 20, p);   CHECK(near4(p, 0, 0, 0, 0.0f, 0.003f), "eye%d empty pixel must be transparent (the FP16 alpha 1.0 is ignored) a=%.3f", e, p[3]);
        px_lin(r.px[e], 20, 20, p);    CHECK(near4(p, 0, 0, 0, 0.0f, 0.003f), "eye%d colour-with-depth-0 patch must be discarded: %.3f %.3f %.3f a=%.3f", e, p[0], p[1], p[2], p[3]);
        px_lin(r.px[e], 195, 113, p);  CHECK(near4(p, 0, 0, 0, 0.0f, 0.003f), "eye%d depth hole must be transparent a=%.3f", e, p[3]);
        px_lin(r.px[e], 130, 130, p);  CHECK(near4(p, 1, 1, 1, 1.0f, 0.006f), "eye%d HDR 3.0 clamps to 1: %.3f", e, p[0]);
    }
    CHECK(r.px[0] != r.px[1], "the two eyes must differ (left/right target mapping)");
    // Independent byte checks on the first pixel (the poison 0xCD must be gone everywhere).
    size_t poison = 0; for (unsigned t = 0; t < 2; ++t) for (size_t i = 0; i < r.px[t].size(); i += 4) if (r.px[t][i] == 0xCD && r.px[t][i + 1] == 0xCD && r.px[t][i + 2] == 0xCD && r.px[t][i + 3] == 0xCD) ++poison;
    CHECK(poison == 0, "%zu pixels were never written", poison);
    // Reinhard + exposure.
    BlitJob j2 = base_job(s); j2.tonemap = Tonemap::reinhard; j2.exposure = 1.0f;
    Result r2 = eye_case(ch, s, "a_mode0_reinhard", j2, false, 0.006f, 0);
    if (r2.ok) { px_lin(r2.px[0], 130, 130, p); CHECK(near4(p, 0.75f, 0.75f, 0.75f, 1.0f, 0.006f), "Reinhard 3.0 -> 0.75, got %.4f", p[0]); }
    BlitJob j3 = base_job(s); j3.exposure = 0.5f;
    Result r3 = eye_case(ch, s, "a_mode0_exposure_half", j3, false, 0.006f, 0, false);
    if (r3.ok) { px_lin(r3.px[0], 100, 150, p); CHECK(near4(p, q16(0.4f), q16(0.1f), q16(0.05f), 1.0f, 0.006f), "exposure 0.5 scales colour: %.4f", p[0]); }
    // use_depth off: opaque everywhere (alpha 1) -- the documented fallback.
    BlitJob j4 = base_job(s); j4.use_depth = false;
    Result r4 = eye_case(ch, s, "a_mode0_no_depth", j4, false, 0.006f, 0, false);
    if (r4.ok) { px_lin(r4.px[0], 300, 20, p); CHECK(near4(p, 0, 0, 0, 1.0f, 0.003f), "no depth: alpha 1 everywhere, got a=%.3f", p[3]); CHECK(!r4.facts.depth_used, "depth_used"); }
    // Second swapchain image index + D32_FLOAT (40) depth resource.
    BlitJob j5 = base_job(s); j5.index = {1, 1, 1, 1, 1};
    Result r5 = eye_case(ch, s, "a_mode0_image_index1", j5, false, 0.006f, 0, false);
    if (r5.ok && r.ok) CHECK(r5.px[0] == r.px[0], "image index 1 must hold the same result as index 0");
}

static void case_srgb(Chains& ch28, Chains& ch29, const Scene& s) {
    std::printf("[f] sRGB / encode policy\n");
    Img ramp(W, H); Depth full; fill_depth(full, 0, 0, W, H, 1.0f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float* p = ramp.at(x, y); p[0] = p[1] = p[2] = float(x) / (W - 1); p[3] = 1.0f; }
    // FP16 ramp keeps ~10 bits: compare against the quantised values.
    quantise(ramp);
    Scene rs = s; rs.eye[0] = rs.eye[1] = ramp; rs.depth[0] = rs.depth[1] = full;
    rs.eye_t[0] = rs.eye_t[1] = make_color(ramp); rs.depth_t[0] = rs.depth_t[1] = make_depth(full);
    BlitJob j = base_job(rs);
    Result r28 = run_job(ch28, j, 3u), r29 = run_job(ch29, j, 3u);
    CHECK(r28.ok && r29.ok, "ramp jobs");
    if (!(r28.ok && r29.ok)) return;
    int worst = 0;
    for (int x = 0; x < W; ++x) {
        const int want = int(std::lround(srgb_enc(ramp.at(x, 90)[0]) * 255.0f));
        const int got = r28.px[0][(size_t(90) * W + x) * 4];
        worst = std::max(worst, std::abs(want - got));
    }
    INFO("grey ramp: worst sRGB byte error vs 255*encode(linear) = %d", worst);
    CHECK(worst <= 1, "encode-once: worst byte error %d", worst);
    CHECK(r28.px[0] == r29.px[0], "format 28 (typeless + sRGB RTV) and format 29 (sRGB resource) must give identical bytes");
    // Mid-grey sanity: linear 0.18 -> sRGB 0.4614 -> 118; an extra encode would give ~176, a missing one 46.
    float lin = ramp.at(int(0.18f * (W - 1) + 0.5f), 0)[0]; const int want = int(std::lround(srgb_enc(lin) * 255.0f));
    CHECK(std::abs(int(r28.px[0][(size_t(90) * W + int(0.18f * (W - 1) + 0.5f)) * 4]) - want) <= 1, "mid grey");
    save_sheet("f_srgb_ramp", r28.px[0], [&] { Img rf(W, H); for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float* p = rf.at(x, y); p[0] = p[1] = p[2] = ramp.at(x, 0)[0]; p[3] = 1; } return rf; }(), false);
}

// Window / mapping checks: bare cockpit (no coverage) so the output is the world layer alone.
static void case_world(Chains& ch, const Scene& s, bool docs) {
    std::printf("[b] world-in-projection (mode 2)\n");
    const float origin[3] = {0, 0, 0};
    const ViewSetup front = make_view("frontal", 0, 0, origin, 0, 0);
    {
        BlitJob j = base_job(s, true); job_world(j, s, front, false);
        Result r = eye_case(ch, s, "b_world_bare_frontal", j, true, 0.012f, 4);
        if (r.ok) {
            // (1) window extent: first/last world column on the centre row and first/last row on the centre column vs analytic tan geometry.
            const float tl = std::tan(front.eye[0].fov.left), tr = std::tan(front.eye[0].fov.right), tu = std::tan(front.eye[0].fov.up), td = std::tan(front.eye[0].fov.down);
            auto ex_col = [&](int e, float tan_edge) { // eye e, window edge direction -> pixel boundary (plane at dist, eye offset parallax)
                const float xe = front.eye[e].pose.position[0];
                const float tn = tan_edge - xe / front.dist; return (tn - tl) / (tr - tl) * W; };
            for (int e = 0; e < 2; ++e) {
                int first = -1, last = -1;
                for (int x = 0; x < W; ++x) if (r.px[e][(size_t(90) * W + x) * 4 + 3] > 127) { if (first < 0) first = x; last = x; }
                const float wl = ex_col(e, -front.lens[0]), wr = ex_col(e, front.lens[1]);
                INFO("eye%d centre row: world columns %d..%d, analytic edges %.2f..%.2f (63.2 deg window = tan +-%.4f)", e, first, last, wl, wr, front.lens[0]);
                CHECK(std::fabs(float(first) - wl) <= 1.0f && std::fabs(float(last + 1) - wr) <= 1.0f, "eye%d horizontal window extent", e);
                int top = -1, bot = -1;
                for (int y = 0; y < H; ++y) if (r.px[e][(size_t(y) * W + 160) * 4 + 3] > 127) { if (top < 0) top = y; bot = y; }
                const float wt = (tu - front.lens[2]) / (tu - td) * H, wb = (tu + front.lens[3]) / (tu - td) * H;
                INFO("eye%d centre column: world rows %d..%d, analytic edges %.2f..%.2f (38.17 deg window = tan +-%.4f)", e, top, bot, wt, wb, front.lens[2]);
                CHECK(std::fabs(float(top) - wt) <= 1.0f && std::fabs(float(bot + 1) - wb) <= 1.0f, "eye%d vertical window extent", e);
                // (2) marker positions from independent geometry: world uv -> plane point -> eye pixel (flip check: v=0 is the TOP of the world image).
                struct M { float wu, wv, r, g, b; const char* n; } ms[] = {{14.5f / W, 14.5f / H, 0, 0, 1, "blue top-left"}, {(W - 14.5f) / W, (H - 14.5f) / H, 1, 0, 1, "magenta bottom-right"},
                    {160.0f / W, 90.0f / H, 1, 1, 1, "white centre"}, {255.0f / W, 25.0f / H, 1, 1, 0, "yellow upper right"}};
                for (const M& m : ms) {
                    const float tx = (m.wu - 0.5f) * (front.lens[0] + front.lens[1]);
                    const float ty = (0.5f - m.wv) * (front.lens[2] + front.lens[3]);
                    // eye at x_e looks at plane point h.x = (wu-0.5)*dist*(l+r); tan_eye = (h.x - x_e)/dist
                    const float tane = tx - front.eye[e].pose.position[0] / front.dist;
                    const int px = int((tane - tl) / (tr - tl) * W), py = int((tu - ty) / (tu - td) * H);
                    float p[4]; px_lin(r.px[e], px, py, p);
                    CHECK(near4(p, m.r, m.g, m.b, 1.0f, 0.1f), "eye%d marker '%s' expected at pixel %d,%d: got %.2f %.2f %.2f a=%.2f", e, m.n, px, py, p[0], p[1], p[2], p[3]);
                }
            }
        }
    }
    {   // cockpit over the world: world only where cockpit alpha is 0, hole shows the world
        BlitJob j = base_job(s); job_world(j, s, front, false);
        Result r = eye_case(ch, s, "b_world_over_cockpit_frontal", j, false, 0.012f, 4, true, docs);
        if (r.ok) {
            float p[4], q[4];
            px_lin(r.px[0], 100, 150, p); CHECK(near4(p, 0.8f, 0.2f, 0.1f, 1.0f, 0.01f), "cockpit panel stays opaque cockpit colour, no world behind it: %.3f %.3f %.3f", p[0], p[1], p[2]);
            px_lin(r.px[0], 195, 113, p); CHECK(p[3] > 0.99f && !near4(p, 0, 0, 0, 1, 0.02f), "depth hole shows the world (a=%.2f rgb %.2f %.2f %.2f)", p[3], p[0], p[1], p[2]);
            const WorldComposite wc0 = j.world_eye[0]; const WorldHit h = world_ray_uv(wc0, 195.5f / W, 113.5f / H);
            if (h.hit) { const float er = h.u, eg = h.v, eb = 0.25f; px_lin(r.px[0], 195, 113, q); CHECK(near4(q, er, eg, eb, 1.0f, 0.03f), "hole pixel = world gradient at the ray hit: want %.2f %.2f %.2f got %.2f %.2f %.2f", er, eg, eb, q[0], q[1], q[2]); }
            px_lin(r.px[0], 5, 5, p);     CHECK(near4(p, 0, 0, 0, 0.0f, 0.003f), "outside the 63x38 window and outside the cockpit: transparent (mode 2 without extend)");
            px_lin(r.px[0], 20, 20, p);   CHECK(near4(p, 0, 0, 0, 0.0f, 0.003f), "discarded colour patch outside the window stays transparent");
            px_lin(r.px[0], 160, 60, p);  CHECK(p[3] > 0.99f, "world visible where the cockpit alpha is 0 inside the window (a=%.2f)", p[3]);
        }
    }
    {
        const float pos[3] = {0.05f, 0.02f, -0.03f};
        const ViewSetup rot = make_view("rotated", 12.0f, 8.0f, pos, 0.1f, 0.05f);
        BlitJob j = base_job(s, true); job_world(j, s, rot, false);
        eye_case(ch, s, "b_world_bare_rotated_head", j, true, 0.012f, 8);
        BlitJob j2 = base_job(s); job_world(j2, s, rot, false);
        eye_case(ch, s, "b_world_over_cockpit_rotated", j2, false, 0.012f, 8);
        // feather on the world edge
        BlitJob j3 = base_job(s); job_world(j3, s, front, false); j3.edge_feather = 0.15f;
        eye_case(ch, s, "b_world_feather", j3, false, 0.012f, 8);
        // tonemap
        BlitJob j4 = base_job(s); job_world(j4, s, front, false); j4.tonemap = Tonemap::reinhard; j4.exposure = 1.5f;
        eye_case(ch, s, "b_world_reinhard_exposure", j4, false, 0.012f, 8, false);
    }
    {   // mode-0 non-composite world quad target (kQuad)
        BlitJob j = base_job(s); j.use_world = true; j.world = {s.world_t.Get(), 10u, W, H};
        Result r = run_job(ch, j, 7u);
        CHECK(r.ok, "quad job");
        if (r.ok) {
            const Cfg c = cfg_for(j, 0);
            const Cmp m = compare(r.px[kQuad], ref_quad(s, c), 0.012f);
            INFO("world quad target          max_err=%.4f bad=%zu", m.max_err, m.bad);
            CHECK(m.bad <= 0, "world quad target differs from CPU mirror (%zu)", m.bad);
            float p[4]; px_lin(r.px[kQuad], 5, 5, p); CHECK(p[3] > 0.99f, "world quad is opaque");
        }
    }
}

static void case_effects(Chains& ch, const Scene& s, bool docs) {
    std::printf("[c] effects layer (mode 3)\n");
    const float origin[3] = {0, 0, 0};
    const ViewSetup front = make_view("frontal", 0, 0, origin, 0, 0);
    BlitJob j = base_job(s); job_world(j, s, front, true);
    Result r = eye_case(ch, s, "c_effects_mode3", j, false, 0.012f, 8, true, docs);
    if (!r.ok) return;
    CHECK(r.facts.effects_used, "effects_used");
    const WorldComposite wc = j.world_eye[0];
    // find eye0 pixels for each blob in world uv: choose the eye pixel whose hit uv is nearest the blob centre
    auto find = [&](float wu, float wv, int& bx, int& by) {
        float best = 1e9f; bx = by = -1;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { const WorldHit h = world_ray_uv(wc, (x + 0.5f) / W, (y + 0.5f) / H); if (!h.hit) continue;
            const float d = (h.u - wu) * (h.u - wu) + (h.v - wv) * (h.v - wv); if (d < best) { best = d; bx = x; by = y; } }
    };
    float p[4];
    struct B { float cx, cy; const char* n; float er, eg, eb, ea; } bl[] = {{60, 45, "R solid", 0.6f, 0, 0, 0.6f}, {255, 80, "G translucent", 0, 0.25f, 0, 0.5f}, {155, 30, "W alpha0", 0.8f, 0.8f, 0.8f, 0.8f}};
    for (const B& b : bl) {
        int x, y; find(b.cx / W, b.cy / H, x, y);
        const WorldHit h = world_ray_uv(wc, (x + 0.5f) / W, (y + 0.5f) / H);
        float cover_depth = s.depth[0].d[size_t(y) * W + x];
        // hand formula: out = e + (1 - e.a) * world, world = gradient at the hit uv (alpha 1), cockpit over it when covered
        const float wr = h.u, wg = h.v, wb = 0.25f;
        const float exp_r = b.er + (1 - b.ea) * wr, exp_g = b.eg + (1 - b.ea) * wg, exp_b = b.eb + (1 - b.ea) * wb;
        px_lin(r.px[0], x, y, p);
        if (cover_depth > 0) INFO("blob '%s' at %d,%d is under the cockpit (skipped)", b.n, x, y);
        else CHECK(near4(p, exp_r, exp_g, exp_b, 1.0f, 0.03f), "blob '%s' at %d,%d: want %.3f %.3f %.3f got %.3f %.3f %.3f a=%.3f", b.n, x, y, exp_r, exp_g, exp_b, p[0], p[1], p[2], p[3]);
    }
    {   // Y blob: world uv centre (160,120) is mapped under the panel -> must show the cockpit colour only (no effects through the cockpit)
        int x, y; find(160.0f / W, 120.0f / H, x, y);
        INFO("Y blob centre maps to eye pixel %d,%d depth=%.1f", x, y, s.depth[0].d[size_t(y) * W + x]);
        if (s.depth[0].d[size_t(y) * W + x] > 0) { px_lin(r.px[0], x, y, p); const float c0[3] = {0.8f, 0.2f, 0.1f}; CHECK(near4(p, q16(c0[0]), q16(c0[1]), q16(c0[2]), 1.0f, 0.01f), "no effects where the cockpit covers: %.3f %.3f %.3f", p[0], p[1], p[2]); }
    }
    {   // outside the window: effects are not drawn (window rays only), mode 3 without extend
        px_lin(r.px[0], 5, 5, p); CHECK(near4(p, 0, 0, 0, 0.0f, 0.003f), "outside window: transparent, effects not sampled");
    }
    // The effects texture is in the SAME world uv as the world: with effects disabled the blob pixels equal the mode-2 world.
    BlitJob j2 = base_job(s); job_world(j2, s, front, false);
    Result r2 = run_job(ch, j2, 1u);
    if (r2.ok) { int x, y; find(60.0f / W, 45.0f / H, x, y); float q[4]; px_lin(r2.px[0], x, y, q); px_lin(r.px[0], x, y, p);
        CHECK(p[0] > q[0] + 0.05f, "effects brighten the red channel over the world (%.3f vs %.3f)", p[0], q[0]); }
    // Rotated head
    const float pos[3] = {0.05f, 0.02f, -0.03f};
    const ViewSetup rot = make_view("rotated", 12.0f, 8.0f, pos, 0.1f, 0.05f);
    BlitJob j3 = base_job(s); job_world(j3, s, rot, true); j3.edge_feather = 0.1f;
    eye_case(ch, s, "c_effects_rotated_feather", j3, false, 0.012f, 12, false);
    BlitJob j4 = base_job(s); job_world(j4, s, front, true); j4.tonemap = Tonemap::reinhard;
    eye_case(ch, s, "c_effects_reinhard", j4, false, 0.012f, 8, false);
}

// J-effects-dual: literal CPU cases (no GPU): the accumulation oracle and the production mirror agree with the hand-derived numbers.
static bool close6(const float* a, const float* b, int n) { for (int i = 0; i < n; ++i) if (!(std::fabs(a[i] - b[i]) <= 1e-6f)) return false; return true; }
static void case_effects_dual_cpu() {
    std::printf("[c2] effects dual-source coverage_v1 (CPU oracle + mirror, literal values)\n");
    const float S[3] = {0.5f, 0.25f, 0}, black[3] = {0, 0, 0}, S2[3] = {0, 0.5f, 1};
    const Shade B{0.25f, 0.5f, 0.75f, 1.0f};
    struct Row { const float* s; float a, b; float want[4]; float wantG; float final[4]; } rows[] = {
        {S, 0.25f, 0.0f, {0.125f, 0.0625f, 0, 0.25f}, 0.0f, {0.3125f, 0.4375f, 0.5625f, 1}},
        {S, 0.25f, 1.0f, {0.125f, 0.0625f, 0, 0.25f}, 0.25f, {0.3125f, 0.4375f, 0.5625f, 1}},
        {S, 0.25f, 0.5f, {0.125f, 0.0625f, 0, 0.25f}, 0.125f, {0.3125f, 0.4375f, 0.5625f, 1}},
        {S, 0.0f, 1.0f, {0, 0, 0, 0}, 0.0f, {0.25f, 0.5f, 0.75f, 1}},
        {S, 1.0f, 0.25f, {0.5f, 0.25f, 0, 1}, 0.25f, {0.5f, 0.25f, 0, 1}},
        {black, 0.25f, 0.5f, {0, 0, 0, 0.25f}, 0.125f, {0.1875f, 0.375f, 0.5625f, 1}}};   // finding 1: coverage 0.25 (not the 0.125 alpha-heuristic)
    for (const Row& r : rows) {
        Acc a; accumulate(a, r.s, r.a, r.b);
        const float got[4] = {a.P[0], a.P[1], a.P[2], a.A};
        CHECK(close6(got, r.want, 4) && std::fabs(a.G - r.wantG) <= 1e-6f, "oracle a=%.2f b=%.2f: P=%.6f %.6f %.6f A=%.6f G=%.6f", r.a, r.b, a.P[0], a.P[1], a.P[2], a.A, a.G);
        const Shade o = effects_coverage_over_world(got, Tonemap::clamp, 1.0f, 1.0f, B);
        const float of[4] = {o.r, o.g, o.b, o.a};
        CHECK(close6(of, r.final, 4), "mirror a=%.2f b=%.2f: %.6f %.6f %.6f %.6f", r.a, r.b, o.r, o.g, o.b, o.a);
    }
    {   // the old colour/alpha heuristic leaves 0.875 of the world for finding 1's replay (RGB 0, game alpha 0.125): the logged RGB equation needs 0.75
        const float legacy[4] = {0, 0, 0, 0.125f};
        const Shade o = effects_over_world(legacy, Tonemap::clamp, 1.0f, 1.0f, B);
        const float of[4] = {o.r, o.g, o.b, o.a}, want[4] = {0.21875f, 0.4375f, 0.65625f, 1};
        CHECK(close6(of, want, 4), "legacy heuristic reference value (documents the finding-1 error): %.6f", o.r);
    }
    {   // ordered multi-draw and its reverse (order matters)
        Acc f; accumulate(f, S, 0.25f, 0.5f); accumulate(f, S2, 0.5f, 0.75f);
        const float P[4] = {0.0625f, 0.28125f, 0.5f, 0.625f};
        const float got[4] = {f.P[0], f.P[1], f.P[2], f.A};
        CHECK(close6(got, P, 4) && std::fabs(f.G - 0.40625f) <= 1e-6f && std::fabs(f.U - 0.125f) <= 1e-6f, "multi-draw summaries");
        const Shade o = effects_coverage_over_world(got, Tonemap::clamp, 1.0f, 1.0f, B);
        const float of[4] = {o.r, o.g, o.b, o.a}, want[4] = {0.15625f, 0.46875f, 0.78125f, 1};
        CHECK(close6(of, want, 4), "multi-draw final");
        CHECK(std::fabs(f.G + f.U * 0.5f - 0.46875f) <= 1e-6f, "initial game alpha 0.5 reconstructs to 0.46875 (not the consumer alpha)");
        Acc r; accumulate(r, S2, 0.5f, 0.75f); accumulate(r, S, 0.25f, 0.5f);
        const float R[4] = {0.125f, 0.25f, 0.375f, 0.625f}, rg[4] = {r.P[0], r.P[1], r.P[2], r.A};
        CHECK(close6(rg, R, 4) && std::fabs(r.G - 0.3125f) <= 1e-6f && std::fabs(r.U - 0.125f) <= 1e-6f, "reverse-order summaries");
        const Shade o2 = effects_coverage_over_world(rg, Tonemap::clamp, 1.0f, 1.0f, B);
        const float o2f[4] = {o2.r, o2.g, o2.b, o2.a}, want2[4] = {0.21875f, 0.4375f, 0.65625f, 1};
        CHECK(close6(o2f, want2, 4), "reverse-order final (order sensitive)");
    }
    const float row1[4] = {0.125f, 0.0625f, 0, 0.25f};
    {   // transparent filtering edge: bilinear half weight against a clear texel; no straight-colour interpolation, no second multiply by coverage
        const float half[4] = {0.0625f, 0.03125f, 0, 0.125f};
        const Shade o = effects_coverage_over_world(half, Tonemap::clamp, 1.0f, 1.0f, B);
        const float of[4] = {o.r, o.g, o.b, o.a}, want[4] = {0.28125f, 0.46875f, 0.65625f, 1};
        CHECK(close6(of, want, 4), "filtered edge");
    }
    {   // window ramp 0.5 over an already-ramped world (0.125, 0.25, 0.375, 0.5); then an opaque cockpit shows exactly its own colour
        const Shade w{0.125f, 0.25f, 0.375f, 0.5f};
        const Shade o = effects_coverage_over_world(row1, Tonemap::clamp, 1.0f, 0.5f, w);
        const float of[4] = {o.r, o.g, o.b, o.a}, want[4] = {0.171875f, 0.25f, 0.328125f, 0.5625f};
        CHECK(close6(of, want, 4), "window ramp");
        const Shade cock{0.2f, 0.1f, 0.0f, 1.0f};
        const Shade fin = composite_over_world(cock, o);
        const float ff[4] = {fin.r, fin.g, fin.b, fin.a}, wantc[4] = {0.2f, 0.1f, 0.0f, 1.0f};
        CHECK(close6(ff, wantc, 4), "cockpit over effects");
    }
    {   // tonemap policy: Reinhard, exposure 2: straight (0.5, 0.25, 0) -> (1, 0.5, 0) -> (0.5, 1/3, 0) -> premultiplied (0.125, 1/12, 0)
        const Shade o = effects_coverage_over_world(row1, Tonemap::reinhard, 2.0f, 1.0f, B);
        const float of[4] = {o.r, o.g, o.b, o.a}, want[4] = {0.3125f, 11.0f / 24.0f, 0.5625f, 1};
        CHECK(std::fabs(of[0] - want[0]) <= 1e-6f && std::fabs(of[1] - want[1]) <= 1e-6f && std::fabs(of[2] - want[2]) <= 1e-6f && of[3] == 1.0f, "reinhard exposure 2: %.9f %.9f", o.r, o.g);
    }
    {   // zero / NaN coverage: the world unchanged, no NaN; coverage above 1 saturates
        const float zero[4] = {0, 0, 0, 0}, nan[4] = {1, 1, 1, std::numeric_limits<float>::quiet_NaN()}, hi[4] = {0.5f, 0.25f, 0, 2.0f};
        const Shade z = effects_coverage_over_world(zero, Tonemap::reinhard, 2.0f, 1.0f, B), n = effects_coverage_over_world(nan, Tonemap::clamp, 1.0f, 1.0f, B);
        const float zf[4] = {z.r, z.g, z.b, z.a}, nf[4] = {n.r, n.g, n.b, n.a}, bw[4] = {0.25f, 0.5f, 0.75f, 1};
        CHECK(close6(zf, bw, 4) && close6(nf, bw, 4), "zero / NaN coverage returns the world");
        const Shade h = effects_coverage_over_world(hi, Tonemap::clamp, 1.0f, 1.0f, B);
        CHECK(std::isfinite(h.r) && h.a <= 1.0f + 1e-6f && h.r <= 1.0f + 1e-6f, "coverage above 1 saturates");
    }
}
static void case_effects_dual(Chains& ch, const Scene& s, bool docs) {
    std::printf("[c3] effects dual-source coverage_v1 (shader mode 4 on WARP)\n");
    const float origin[3] = {0, 0, 0};
    const ViewSetup front = make_view("frontal", 0, 0, origin, 0, 0);
    BlitJob j = base_job(s); job_world_cov(j, s, front);
    Result r = eye_case(ch, s, "c_effects_coverage_mode4", j, false, 0.012f, 8, true, docs);
    if (!r.ok) return;
    CHECK(r.facts.effects_used, "effects_used (coverage_v1)");
    const WorldComposite wc = j.world_eye[0];
    auto find = [&](float wu, float wv, int& bx, int& by) {
        float best = 1e9f; bx = by = -1;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { const WorldHit h = world_ray_uv(wc, (x + 0.5f) / W, (y + 0.5f) / H); if (!h.hit) continue;
            const float d = (h.u - wu) * (h.u - wu) + (h.v - wv) * (h.v - wv); if (d < best) { best = d; bx = x; by = y; } }
    };
    float p[4];
    {   // finding 1: black out0 colour, alpha 0.25, src1 alpha 0.5 -> coverage 0.25 -> exactly 0.75 of the world, not the 0.875 of the old heuristic
        int x, y; find(60.0f / W, 45.0f / H, x, y);
        const WorldHit h = world_ray_uv(wc, (x + 0.5f) / W, (y + 0.5f) / H);
        px_lin(r.px[0], x, y, p);
        const float w[3] = {q16(h.u), q16(h.v), q16(0.25f)};
        if (s.depth[0].d[size_t(y) * W + x] > 0) INFO("finding-1 blob at %d,%d is under the cockpit (skipped)", x, y);
        else {
            CHECK(near4(p, 0.75f * w[0], 0.75f * w[1], 0.75f * w[2], 1.0f, 0.012f), "finding 1: want 0.75*world %.3f %.3f %.3f got %.3f %.3f %.3f", 0.75f * w[0], 0.75f * w[1], 0.75f * w[2], p[0], p[1], p[2]);
            CHECK(std::fabs(p[2] - 0.875f * w[2]) > 0.015f, "finding 1: the 0.875 alpha-heuristic result is NOT produced (blue %.3f vs %.3f)", p[2], 0.875f * w[2]);
        }
    }
    {   // row 1 (colour + coverage 0.25): P + 0.75*world
        int x, y; find(255.0f / W, 80.0f / H, x, y);
        const WorldHit h = world_ray_uv(wc, (x + 0.5f) / W, (y + 0.5f) / H);
        px_lin(r.px[0], x, y, p);
        if (s.depth[0].d[size_t(y) * W + x] <= 0)
            CHECK(near4(p, 0.125f + 0.75f * q16(h.u), 0.0625f + 0.75f * q16(h.v), 0.75f * q16(0.25f), 1.0f, 0.012f), "row 1: P + (1-A)*world: got %.3f %.3f %.3f", p[0], p[1], p[2]);
    }
    {   // outside the window: effects are not drawn
        px_lin(r.px[0], 5, 5, p); CHECK(near4(p, 0, 0, 0, 0.0f, 0.003f), "outside window: transparent, effects not sampled (mode 4)");
    }
    const float pos[3] = {0.05f, 0.02f, -0.03f};
    BlitJob j3 = base_job(s); job_world_cov(j3, s, make_view("rotated", 12.0f, 8.0f, pos, 0.1f, 0.05f)); j3.edge_feather = 0.1f;
    eye_case(ch, s, "c_effects_coverage_rotated_feather", j3, false, 0.012f, 12, false);
    BlitJob j4 = base_job(s); job_world_cov(j4, s, front); j4.tonemap = Tonemap::reinhard; j4.exposure = 2.0f;
    eye_case(ch, s, "c_effects_coverage_reinhard_exp2", j4, false, 0.012f, 8, false);
}

// EFFECTS-OUTSIDE (F) fxmem:6 (fx_sprite_gpu.h sprite_layer / sprite_over): the eye sprite image at t14 (mode_bits bit 19) on the REAL backend + shader.
// Differential against the same world-composite job without it: eye 0's image holds coverage-free LIGHT (P > 0, A = 0: the extra7_p explosion clone over
// empty space), eye 1's holds nothing. Expected: eye 1 byte-identical; eye 0 = baseline + light (clamped) on every pixel outside the replayed cockpit band
// (depth >= 2^-10 keeps its pixel, as the smoke), alpha unchanged; two more barriers out and back; the facts say sprite_used.
static void case_sprite(Chains& ch, const Scene& s) {
    std::printf("[sprite] fxmem:6 eye sprite image (t14, bit 19): coverage-free light added in DISPLAY space after the composite, cockpit band kept\n");
    if (ch.cw != W || ch.ch != H) { CHECK(false, "sprite case expects an unscaled chain"); return; }
    const float pos[3] = {0, 0, 0};
    const ViewSetup v = make_view("sprite", 0, 0, pos, 0, 0);
    BlitJob j = base_job(s); job_world(j, s, v, false);
    const Result b = run_job(ch, j, 3u);
    // EFFECTS-OUTSIDE (I): A = 0 (shared clone: light only) and A = -0.5 (the explosion's light PSO: T = 1.5, the game's (1+a) background brightening).
    // Expected (fx_sprite_gpu.h sprite_over): decode(min(1, L + T x encode(under))), alpha unchanged.
    // (J, headset 2026-10-29 item 7) A = 1: an opaque coverage puff in the eye sprite image (the non-indexed 4caf6e2d smoke/debris quads now replayed per eye)
    // REPLACES every non-band pixel, object pixels included (T = 0: decode(L)); the producer's eye-depth test already removed it where an object is nearer.
    for (const float A : {0.0f, -0.5f, 1.0f}) {
        Img glow(W, H), zero(W, H);
        const float L[3] = {q16(0.25f), q16(0.125f), q16(0.0625f)}, T = 1.0f - q16(A);
        for (size_t i = 0; i < glow.p.size(); i += 4) { glow.p[i] = L[0]; glow.p[i + 1] = L[1]; glow.p[i + 2] = L[2]; glow.p[i + 3] = q16(A); }
        auto gt = make_color(glow); auto zt = make_color(zero);
        BlitJob js = j; js.use_sprite = true; js.sprite = {{{gt.Get(), 10u, W, H}, {zt.Get(), 10u, W, H}}};
        const Result r = run_job(ch, js, 3u);
        CHECK(b.ok && r.ok && !b.facts.sprite_used && r.facts.sprite_used, "sprite job: ran %d/%d used %d/%d", int(b.ok), int(r.ok), int(b.facts.sprite_used), int(r.facts.sprite_used));
        if (!b.ok || !r.ok) return;
        CHECK(r.facts.sources_restored && r.facts.swapchain_untouched && r.facts.barriers_out == r.facts.barriers_back && r.facts.barriers_out == b.facts.barriers_out + 2,
              "sprite barriers %u/%u vs %u", r.facts.barriers_out, r.facts.barriers_back, b.facts.barriers_out);
        CHECK(r.px[1] == b.px[1], "eye 1 (empty sprite image) must be byte-identical to the job without it");
        size_t added = 0, kept = 0, bad = 0, objects = 0; int fx = -1, fy = -1;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            float bp[4], gp[4]; px_lin(b.px[0], x, y, bp); px_lin(r.px[0], x, y, gp);
            const float dz = s.depth[0].d[size_t(y) * W + x];
            const bool band = dz >= 0.0009765625f;
            bool okp = std::fabs(gp[3] - (band ? bp[3] : std::min(1.0f, std::max(0.0f, q16(A) + T * bp[3])))) <= 0.003f;   // = saturate(A + T a_under) (A <= 0 and a_under = 1: unchanged)
            for (int i = 0; i < 3; ++i) okp = okp && std::fabs(gp[i] - (band ? bp[i] : srgb_dec(std::min(1.0f, L[i] + T * srgb_enc(bp[i]))))) <= 0.012f;
            if (!okp) { if (!bad) { fx = x; fy = y; } ++bad; } else if (band) ++kept; else { ++added; if (dz > 0.0f) ++objects; }
        }
        if (A == 1.0f) INFO("sprite eye0 (A 1): opaque puff, of which object-band pixels %zu (the scene may hold none)", objects);
        INFO("sprite eye0 (A %.2f): added %zu, cockpit band kept %zu, bad %zu", A, added, kept, bad);
        CHECK(bad == 0, "sprite eye0 (A %.2f): %zu pixels differ from decode(L + T encode(baseline)) (first at %d,%d)", A, bad, fx, fy);
        CHECK(added > size_t(W) * H / 4 && kept > 0, "sprite eye0: added %zu kept %zu", added, kept);
    }
}
static void case_hold_extend(Chains& ch, const Scene& s, bool docs) {
    std::printf("[d] hold / extend\n");
    const float origin[3] = {0, 0, 0};
    const ViewSetup front = make_view("frontal", 0, 0, origin, 0, 0);
    {   // extend: periphery fill
        BlitJob j = base_job(s); job_world(j, s, front, false); j.world_extend = true;
        Result r = eye_case(ch, s, "d_extend_frontal", j, false, 0.015f, 20, true, docs);
        if (r.ok) {
            float p[4]; px_lin(r.px[0], 5, 5, p); CHECK(p[3] > 0.99f, "extend fills the periphery (opaque) a=%.2f", p[3]);
            CHECK(p[0] > 0.0f || p[1] > 0.0f || p[2] > 0.0f, "extend periphery has colour");
            float q[4]; px_lin(r.px[0], 160, 60, q);
            BlitJob jn = base_job(s); job_world(jn, s, front, false); Result rn = run_job(ch, jn, 1u);
            if (rn.ok) { px_lin(rn.px[0], 160, 60, p); CHECK(near4(p, q[0], q[1], q[2], q[3], 0.004f), "window rays are unchanged by extend"); }
        }
        BlitJob jc = base_job(s, true); job_world(jc, s, front, true); jc.world_extend = true;
        eye_case(ch, s, "d_extend_with_effects", jc, true, 0.015f, 20, false);
    }
    {   // extend with big head turn (angular map, rays that miss the plane)
        const float pos[3] = {0, 0, 0};
        const ViewSetup side = make_view("side", 70.0f, 0, pos, 0, 0);
        BlitJob j = base_job(s, true); job_world(j, s, side, false); j.world_extend = true;
        eye_case(ch, s, "d_extend_head_yaw70", j, true, 0.02f, 60, true);
        const ViewSetup up = make_view("up", 0, 55.0f, pos, 0.3f, 0.2f);
        BlitJob j2 = base_job(s, true); job_world(j2, s, up, false); j2.world_extend = true;
        eye_case(ch, s, "d_extend_head_pitch55", j2, true, 0.02f, 60, false);
    }
    {   // hold: capture, then use the held copy with a fade
        BlitJob j = base_job(s); job_world(j, s, front, false); j.world_capture = true;
        Result r = eye_case(ch, s, "d_hold_capture", j, false, 0.012f, 4, false);
        CHECK(r.ok && r.facts.world_captured, "world captured into the held image");
        BlitJob h = base_job(s); h.use_world = true; h.world_composite = true; h.world_held = true; h.world_fade = 0.5f;
        WorldComposite wc[2]; params_for(front, wc); h.world_eye = {wc[0], wc[1]};
        Result rh = run_job(ch, h, 3u);
        CHECK(rh.ok && rh.facts.held_used, "held image sampled");
        if (rh.ok) {
            for (int e = 0; e < 2; ++e) {
                const Cfg c = cfg_for(h, e);
                const Cmp m = compare(rh.px[e], ref_eye(s, c, e, s.depth[e]), 0.012f);
                INFO("d_hold_fade_half            eye%d max_err=%.4f bad=%zu", e, m.max_err, m.bad);
                CHECK(m.bad <= 4, "held+fade differs from the CPU mirror (%zu)", m.bad);
            }
            float p[4], f[4]; px_lin(rh.px[0], 160, 60, p);
            BlitJob jf = base_job(s); job_world(jf, s, front, false); Result rf = run_job(ch, jf, 1u); px_lin(rf.px[0], 160, 60, f);
            CHECK(std::fabs(p[0] - 0.5f * f[0]) < 0.01f && std::fabs(p[1] - 0.5f * f[1]) < 0.01f, "fade 0.5 halves the held world (linear): %.3f vs %.3f", p[0], 0.5f * f[0]);
            BlitJob h0 = h; h0.world_fade = 0.0f; Result r0 = run_job(ch, h0, 1u);
            if (r0.ok) { px_lin(r0.px[0], 160, 60, p); CHECK(near4(p, 0, 0, 0, 1.0f, 0.004f), "fade 0 = black world (still opaque in the window)"); }
        }
    }
}

static void case_debug_png(Chains& ch, const Scene& s, bool docs) {
    std::printf("[e] debug views and png capture\n");
    const float origin[3] = {0, 0, 0};
    const ViewSetup front = make_view("frontal", 0, 0, origin, 0, 0);
    for (unsigned v = 1; v <= 4; ++v) {
        BlitJob j = base_job(s); job_world(j, s, front, true); j.debug_view = v;
        char name[48]; std::snprintf(name, sizeof name, "e_debug_view%u_mode3", v);
        Result r = eye_case(ch, s, name, j, false, 0.012f, 6, true, docs && v == 4);
        if (!r.ok) continue;
        float p[4];
        px_lin(r.px[0], 100, 150, p);   // covered cockpit panel
        if (v == 1) CHECK(near4(p, 1, 1, 1, 1, 0.004f), "view1 covered = white");
        if (v == 4) CHECK(near4(p, 1, p[1], 0, 1, 0.01f), "view4 covered = red (+ world green): %.2f %.2f %.2f", p[0], p[1], p[2]);
        px_lin(r.px[0], 20, 20, p);     // colour but depth 0
        if (v == 4) CHECK(near4(p, 0, 0, 1, 1, 0.01f), "view4 discarded colour = blue: %.2f %.2f %.2f", p[0], p[1], p[2]);
        if (v == 2) CHECK(near4(p, 0, 0.5f, 0, 1, 0.01f), "view2 shows the colour whatever the depth: %.3f %.3f %.3f", p[0], p[1], p[2]);
        if (v == 1) CHECK(near4(p, 0, 0, 0, 1, 0.004f), "view1 uncovered = black opaque");
        px_lin(r.px[0], 5, 5, p); CHECK(p[3] > 0.99f, "debug views are opaque (a=%.2f)", p[3]);
    }
    {   // debug view in mode 0: world black
        BlitJob j = base_job(s); j.debug_view = 3;
        Result r = eye_case(ch, s, "e_debug_view3_mode0", j, false, 0.004f, 0, false);
        float p[4]; if (r.ok) { px_lin(r.px[0], 100, 150, p); CHECK(near4(p, 0, 0, 0, 1, 0.004f), "view3 without a world is black"); }
    }
    // png_once
    BlitJob j = base_job(s); job_world(j, s, front, true); j.png_capture = true;
    Result r = run_job(ch, j, 1u, true);
    CHECK(r.ok && r.facts.png_recorded && r.png_taken, "png capture recorded/taken (rec=%d taken=%d)", int(r.facts.png_recorded), int(r.png_taken));
    if (r.ok && r.png_taken) {
        CHECK(r.png.w == W && r.png.h == H, "png size %ux%u", r.png.w, r.png.h);
        CHECK(r.png.final_rgba.size() == size_t(W) * H * 4, "final size");
        CHECK(r.png.final_rgba == r.px[0], "png 'final' == the swapchain eye 0 image (bytes)");
        // alpha image = debug view 1; worldfx = debug view 3
        for (int k = 0; k < 2; ++k) {
            BlitJob jd = base_job(s); job_world(jd, s, front, true); jd.debug_view = k ? 3u : 1u;
            Result rd = run_job(ch, jd, 1u);
            const std::vector<uint8_t>& got = k ? r.png.worldfx_rgba : r.png.alpha_rgba;
            if (rd.ok) CHECK(got == rd.px[0], "png %s == eye 0 debug view %d", k ? "worldfx" : "alpha", k ? 3 : 1);
        }
        // The production sink path: the proxy hands these RGBA buffers to gpu_copy::step3::png_encode and writes the bytes. Do exactly that.
        const std::vector<uint8_t>* imgs[3] = {&r.png.final_rgba, &r.png.alpha_rgba, &r.png.worldfx_rgba}; const char* nm[3] = {"final", "alpha", "worldfx"};
        for (int k = 0; k < 3; ++k) {
            std::vector<uint8_t> file;
            CHECK(acz::gpu_copy::step3::png_encode(imgs[k]->data(), r.png.w, r.png.h, uint64_t(r.png.w) * 4, false, file), "png_encode %s", nm[k]);
            CHECK(file.size() > 8 && file[1] == 'P' && file[2] == 'N' && file[3] == 'G', "png signature %s", nm[k]);
            CreateDirectoryA(g_out.c_str(), nullptr);
            FILE* f = nullptr;
            if (!fopen_s(&f, (g_out + "\\e_png_once_" + nm[k] + ".png").c_str(), "wb") && f) { std::fwrite(file.data(), 1, file.size(), f); std::fclose(f); }
        }
        size_t nz = 0; for (size_t i = 0; i < r.png.final_rgba.size(); i += 4) nz += r.png.final_rgba[i] | r.png.final_rgba[i + 1] | r.png.final_rgba[i + 2];
        CHECK(nz > 1000, "png final has content");
    }
    // a second capture request after png_take must still work (private target is released after take)
    Result r2 = run_job(ch, j, 1u, true);
    CHECK(r2.ok && r2.png_taken, "second png capture");
}

static void case_hud_markers(Chains& ch, const Scene& s) {
    std::printf("[g] HUD / marker targets (mode 1)\n");
    BlitJob j = base_job(s);
    j.use_hud = true; j.hud = {s.hud_t.Get(), 10u, W, H};
    j.use_markers = true; j.markers = {s.markers_t.Get(), 10u, W, H};
    Result r = run_job(ch, j, (1u << kHud) | (1u << kMarkers) | 3u);
    CHECK(r.ok && r.facts.hud_used && r.facts.markers_used, "hud/markers used");
    if (!r.ok) return;
    const Cfg c = cfg_for(j, 0);
    const Cmp mh = compare(r.px[kHud], ref_hud(s.hud, c), 0.006f), mm = compare(r.px[kMarkers], ref_hud(s.markers, c), 0.006f);
    INFO("hud     max_err=%.4f bad=%zu", mh.max_err, mh.bad); INFO("markers max_err=%.4f bad=%zu", mm.max_err, mm.bad);
    CHECK(mh.bad == 0 && mm.bad == 0, "HUD/markers differ from shade_hud mirror");
    float p[4];
    px_lin(r.px[kHud], 40, 160, p);  CHECK(near4(p, 0.5f, 0, 0, 0.5f, 0.006f), "HUD premultiplied pixel passes through unchanged (not multiplied by alpha again): %.3f a=%.3f", p[0], p[3]);
    px_lin(r.px[kHud], 120, 20, p);  CHECK(near4(p, 0.6f, 0.6f, 0.6f, 0.6f, 0.006f), "HUD alpha-0 bright pixel gets alpha = max(rgb): %.3f a=%.3f", p[0], p[3]);
    px_lin(r.px[kHud], 300, 150, p); CHECK(near4(p, 0, 0, 0, 0, 0.003f), "HUD empty = transparent");
    save_sheet("g_hud", r.px[kHud], ref_hud(s.hud, c), false);
    // unreadable HUD (wrong format) -> cleared, not drawn
    BlitJob jb = j; jb.hud.format = 2u; // R32G32B32A32 does not match the FP16 resource
    Result rb = run_job(ch, jb, 1u << kHud);
    CHECK(rb.ok && !rb.facts.hud_used, "mismatching HUD format is refused without failing the frame");
    if (rb.ok) { px_lin(rb.px[kHud], 40, 160, p); CHECK(near4(p, 0, 0, 0, 0, 0.003f), "refused HUD image is cleared to transparent"); }
    // HUD-MENU: the pause-menu backdrop floor (mode_bits 24..31, HUD target only): empty texels get alpha = floor (rgb 0: darkens the layers below),
    // brighter texels keep max(rgb), premultiplied colour unchanged; the marker target in the same job is untouched.
    BlitJob jm = j; jm.hud_backdrop_bits = acz::hudmenu::backdrop_bits(0.6f);
    Result rm = run_job(ch, jm, (1u << kHud) | (1u << kMarkers) | 3u);
    CHECK(rm.ok && rm.facts.hud_used, "backdrop job runs");
    if (rm.ok) {
        px_lin(rm.px[kHud], 300, 150, p); CHECK(near4(p, 0, 0, 0, 0.6f, 0.006f), "backdrop: empty HUD texel = (0,0,0,0.6): %.3f a=%.3f", p[0], p[3]);
        px_lin(rm.px[kHud], 120, 20, p);  CHECK(near4(p, 0.6f, 0.6f, 0.6f, 0.6f, 0.006f), "backdrop: bright texel keeps max(rgb): %.3f a=%.3f", p[0], p[3]);
        px_lin(rm.px[kHud], 40, 160, p);  CHECK(std::fabs(p[0] - 0.5f) < 0.006f && std::fabs(p[3] - 0.6f) < 0.006f, "backdrop: premultiplied colour kept, alpha raised to the floor: %.3f a=%.3f", p[0], p[3]);
        const Cmp mm2 = compare(rm.px[kMarkers], ref_hud(s.markers, c), 0.006f);
        CHECK(mm2.bad == 0, "backdrop bits never reach the marker target");
    }
}

// RADAR-BG (docs/world-render/radar-background.md; hud_menu.h radar_bg_bits / hud_alpha): the HUD target as the game's radar group leaves it in a
// zero-cleared target: the square rgb 0 / alpha 1 (black background, alpha ZERO/ONE; REPLACE mask quads write alpha 1), additive rings and an
// opaque contact over it (alpha 1), a few texels inside with alpha 0, and ordinary HUD symbols outside (alpha 0 or a translucent panel).
// The real shader on the real backend must equal the CPU rule over the whole target, keep every texel outside the square byte-identical to the
// job without the bits, and give the square the game's translucency (scene x (1 - 0.5625)).
static void case_radar_bg(Chains& ch, const Scene& s) {
    std::printf("[g2] HUD radar background (RADAR-BG, mode 1 bits 19..23)\n");
    namespace M = acz::hudmenu;
    Img hud(W, H);
    const int sx0 = 30, sy0 = 60, sx1 = 130, sy1 = 160;                               // the radar square
    fill(hud, sx0, sy0, sx1, sy1, 0.0f, 0.0f, 0.0f, 1.0f);                           // background + mask: black, alpha 1
    fill(hud, sx0, 109, sx1, 111, 0.0f, 0.35f, 0.0f, 1.0f);                          // ring/cross line (additive green)
    fill(hud, 79, sy0, 81, sy1, 0.0f, 0.35f, 0.0f, 1.0f);
    fill(hud, 60, 80, 68, 88, 0.3f, 0.3f, 0.8f, 1.0f);                               // contact (blue, opaque)
    fill(hud, 120, 150, 125, 155, 0.0f, 0.0f, 0.0f, 0.0f);                           // texels inside the square without alpha (seen live)
    fill(hud, 200, 20, 240, 30, 0.6f, 0.6f, 0.6f, 0.0f);                             // HUD symbol outside (alpha 0)
    fill(hud, 200, 120, 240, 140, 0.1f, 0.1f, 0.1f, 0.3f);                           // translucent panel outside
    quantise(hud);
    ComPtr<ID3D12Resource> hud_t = make_color(hud);
    BlitJob j = base_job(s);
    j.use_hud = true; j.hud = {hud_t.Get(), 10u, W, H};
    j.use_markers = true; j.markers = {s.markers_t.Get(), 10u, W, H};
    const uint32_t rb = M::radar_bg_bits(M::kRadarBgDefault);
    const Cfg c = cfg_for(j, 0);
    auto ref = [&](uint32_t bits) {
        Img out(W, H);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const float u = (float(x) + 0.5f) / W, v = (float(y) + 0.5f) / H;
            float t[4]; bilinear(hud, u, v, t);
            const Shade sc = shade(t, false, 0.0f, c.tm, c.exposure);
            const Shade p = apply_feather({sc.r, sc.g, sc.b, M::hud_alpha(t[3], sc.r, sc.g, sc.b, bits)}, edge_ramp(u, v, c.feather));
            float* o = out.at(x, y); o[0] = p.r; o[1] = p.g; o[2] = p.b; o[3] = p.a;
        }
        return out;
    };
    Result r0 = run_job(ch, j, (1u << kHud) | (1u << kMarkers) | 3u);
    BlitJob jr = j; jr.hud_backdrop_bits = rb;
    Result r1 = run_job(ch, jr, (1u << kHud) | (1u << kMarkers) | 3u);
    CHECK(r0.ok && r1.ok && r1.facts.hud_used, "radar-bg jobs run");
    if (!r0.ok || !r1.ok) return;
    const Cmp m0 = compare(r0.px[kHud], ref(0), 0.006f), m1 = compare(r1.px[kHud], ref(rb), 0.006f);
    INFO("bits 0: max_err=%.4f bad=%zu; radar-bg: max_err=%.4f bad=%zu", m0.max_err, m0.bad, m1.max_err, m1.bad);
    CHECK(m0.bad == 0, "bits 0 == the CPU rule (old alpha)");
    CHECK(m1.bad == 0, "radar-bg GPU == CPU reference over the whole HUD target (first bad %d,%d)", m1.fx, m1.fy);
    CHECK(compare(r1.px[kHud], ref_hud(hud, c), 0.006f).bad > 0, "radar-bg changes the square (mutation guard)");
    size_t outside = 0, diff = 0, inside_changed = 0;   // the texels outside the square (1-texel margin) are byte-identical; inside, only alpha moves
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const uint8_t* a = &r0.px[kHud][(size_t(y) * W + x) * 4]; const uint8_t* b = &r1.px[kHud][(size_t(y) * W + x) * 4];
        const bool in = x >= sx0 - 1 && x <= sx1 && y >= sy0 - 1 && y <= sy1;
        if (!in) { ++outside; diff += std::memcmp(a, b, 4) != 0; }
        else { CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2], "radar-bg never changes the premultiplied colour (%d,%d)", x, y); inside_changed += a[3] != b[3]; }
    }
    INFO("outside texels %zu, changed %zu; inside texels with a new alpha %zu", outside, diff, inside_changed);
    CHECK(outside > 45000 && diff == 0, "every texel outside the radar square is byte-identical");
    CHECK(inside_changed > 9000, "the square's alpha is reconstructed");
    float p[4];
    px_lin(r0.px[kHud], 45, 140, p); CHECK(near4(p, 0, 0, 0, 1.0f, 0.003f), "bits 0: the square is opaque black (the known issue)");
    px_lin(r1.px[kHud], 45, 140, p); CHECK(near4(p, 0, 0, 0, 0.5625f, 0.004f), "radar-bg: background (0,0,0,0.5625): a=%.4f", p[3]);
    px_lin(r1.px[kHud], 100, 110, p); CHECK(near4(p, 0, 0.35f, 0, 0.5625f + 0.4375f * 0.35f, 0.006f), "ring: rb + (1-rb)*0.35: a=%.4f", p[3]);
    px_lin(r1.px[kHud], 64, 84, p);  CHECK(near4(p, 0.3f, 0.3f, 0.8f, 0.5625f + 0.4375f * 0.8f, 0.006f), "contact: rb + (1-rb)*0.8: a=%.4f", p[3]);
    px_lin(r1.px[kHud], 122, 152, p); CHECK(near4(p, 0, 0, 0, 0, 0.003f), "texel without stored alpha stays transparent");
    px_lin(r1.px[kHud], 220, 25, p); CHECK(near4(p, 0.6f, 0.6f, 0.6f, 0.6f, 0.006f), "symbol outside: max(rgb) as before");
    // The radar crop over the world (premultiplied over, in linear light) against the game's own blend scene * (1 - a_bg) (+ symbol).
    { const float world = 0.5f; px_lin(r1.px[kHud], 45, 140, p);
      const float got = p[0] + (1.0f - p[3]) * world;
      CHECK(std::fabs(got - world * (1.0f - 0.5625f)) < 0.004f, "radar crop over a 0.5 world = the game's translucent black %.4f (want %.4f)", got, world * 0.4375f); }
    // with the menu backdrop as well: the floor still wins where it is higher (0.6 > 0.5625), the markers target never sees the bits
    BlitJob jm = jr; jm.hud_backdrop_bits = rb | M::backdrop_bits(0.6f);
    Result rm = run_job(ch, jm, (1u << kHud) | (1u << kMarkers) | 3u);
    CHECK(rm.ok, "radar-bg + backdrop job runs");
    if (rm.ok) {
        const Cmp mm = compare(rm.px[kHud], ref(jm.hud_backdrop_bits), 0.006f);
        CHECK(mm.bad == 0, "radar-bg + backdrop GPU == CPU reference (first bad %d,%d)", mm.fx, mm.fy);
        px_lin(rm.px[kHud], 45, 140, p);  CHECK(near4(p, 0, 0, 0, 0.6f, 0.006f), "menu floor over the square: a=%.4f", p[3]);
        px_lin(rm.px[kHud], 300, 170, p); CHECK(near4(p, 0, 0, 0, 0.6f, 0.006f), "menu floor outside: a=%.4f", p[3]);
        CHECK(compare(rm.px[kMarkers], ref_hud(s.markers, c), 0.006f).bad == 0, "radar-bg/backdrop bits never reach the marker target");
    }
    save_sheet("g2_radar_bg", r1.px[kHud], ref(rb), false);
}

// Swapchain smaller than the source (the production swapchain_extent policy caps the width at 2400 for a 2560 source): colour is filtered
// bilinearly, coverage comes from the NEAREST depth texel. The CPU mirror reproduces this exactly; the case also measures the silhouette fringe.
static void case_downscale(const Scene& s) {
    std::printf("[h] swapchain smaller than the source (non-1:1 sampling)\n");
    const int ow = 300, oh = 169;
    auto ch = make_chains(rgba8_unorm, ow, oh);
    if (!ch->ok) { CHECK(false, "downscale chains"); return; }
    const float origin[3] = {0, 0, 0};
    const ViewSetup front = make_view("frontal", 0, 0, origin, 0, 0);
    BlitJob j = base_job(s, false, ow, oh);
    Result r = eye_case(*ch, s, "h_downscale_mode0", j, false, 0.012f, 0, true);
    if (r.ok) {
        size_t dark = 0, edge = 0;
        for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) {
            float p[4]; px_lin(r.px[0], x, y, p, ow);
            if (p[3] > 0.99f) { ++edge; if (p[0] < 0.8f * 0.95f && x > 100 && x < 250 && y > 94 && y < 104) ++dark; }
        }
        INFO("covered pixels=%zu; panel-top-edge pixels with colour < 95%% of the panel red: %zu (coverage is nearest-depth, colour is bilinear)", edge, dark);
    }
    BlitJob jw = base_job(s, false, ow, oh); job_world(jw, s, front, true);
    eye_case(*ch, s, "h_downscale_mode3", jw, false, 0.012f, 12, false);
    ch->blit->destroy();
}

// Every depth format the consumer accepts must give the same coverage as the D32 baseline (typeless resource, DSV cleared per rectangle).
static void case_depth_formats(Chains& ch, const Scene& s) {
    std::printf("[i] depth formats (SRV plane views on WARP)\n");
    struct F { DXGI_FORMAT res, dsv; uint32_t code; const char* n; } fm[] = {
        {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, 39, "R32_TYPELESS"},
        {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 19, "R32G8X24_TYPELESS"},
        {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, 44, "R24G8_TYPELESS"},
        {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, 53, "R16_TYPELESS"}};
    BlitJob base = base_job(s);
    Result rb = run_job(ch, base, 3u);
    for (const F& f : fm) {
        BlitJob j = base_job(s);
        ComPtr<ID3D12Resource> keep[2];
        for (int e = 0; e < 2; ++e) { keep[e] = make_depth(s.depth[e], f.res, f.dsv); j.depth[e].resource = keep[e].Get(); j.depth[e].format = f.code; }
        Result r = run_job(ch, j, 3u);
        CHECK(r.ok && r.facts.depth_used, "%s: depth used", f.n);
        if (r.ok && rb.ok) { const bool same = r.px[0] == rb.px[0] && r.px[1] == rb.px[1]; CHECK(same, "%s: coverage differs from the D32 baseline", f.n); INFO("%-18s coverage identical to baseline: %d", f.n, int(same)); }
    }
}

// FP16 specials: NaN -> 0 (HLSL max), negative -> 0, +Inf: clamp -> 1; Reinhard c/(1+c) = NaN -> 0 in UNORM (documented, not changed).
static void case_special_values(Chains& ch, const Scene& s0) {
    std::printf("[j] FP16 NaN / Inf / negative\n");
    Scene s = s0;
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (int e = 0; e < 2; ++e) {
        fill(s.eye[e], 100, 150, 110, 160, nan, nan, nan, 1); fill(s.eye[e], 120, 150, 130, 160, inf, inf, inf, 1); fill(s.eye[e], 140, 150, 150, 160, -2.0f, -2.0f, -2.0f, 1);
        s.eye_t[e] = make_color(s.eye[e]);
    }
    for (int tm = 0; tm < 2; ++tm) {
        BlitJob j = base_job(s); j.tonemap = tm ? Tonemap::reinhard : Tonemap::clamp;
        Result r = run_job(ch, j, 1u);
        CHECK(r.ok, "special values job");
        if (!r.ok) continue;
        float pn[4], pi[4], pm[4];
        px_lin(r.px[0], 105, 155, pn); px_lin(r.px[0], 125, 155, pi); px_lin(r.px[0], 145, 155, pm);
        INFO("%s: NaN -> %.3f (a=%.2f)  +Inf -> %.3f (a=%.2f)  -2 -> %.3f (a=%.2f)", tm ? "reinhard" : "clamp", pn[0], pn[3], pi[0], pi[3], pm[0], pm[3]);
        CHECK(near4(pn, 0, 0, 0, 1, 0.004f), "NaN colour -> 0 with alpha from depth");
        CHECK(near4(pm, 0, 0, 0, 1, 0.004f), "negative colour -> 0");
        // +Inf is informational: bilinear lerp(inf, inf) = inf + w*(inf - inf) = NaN on WARP, so an Inf texel reads as black (the CPU mirror says 1).
        // FP16 Inf needs a value > 65504; the PS2 replay does not produce that.
        const Cfg c = cfg_for(j, 0);
        const Cmp m = compare(r.px[0], ref_eye(s, c, 0, s.depth[0]), 0.006f);
        CHECK(m.bad <= 100, "specials: GPU differs from the mirror outside the Inf patch (%zu)", m.bad); // 100 = the Inf patch
    }
}

// J-occlusion-fix (key xr_world_window_occlude; docs/review/occlusion-fix-impl.md): replayed world objects (object-band depth 0 < d < 2^-10) are hidden
// inside the world window and kept in the periphery; cockpit depth and use_depth=0 are untouched; flag off = unchanged bytes. Own scene so every existing
// case keeps its inputs. Window (frontal): about x 77..243, y 53..127 of the 320x180 eye image (checked below with the CPU window test).
static void case_occlude(Chains& ch, const Scene& base) {
    std::printf("[g] window occlusion fix (mode_bits bit 16)\n");
    const float origin[3] = {0, 0, 0};
    const ViewSetup front = make_view("frontal", 0, 0, origin, 0, 0);
    const float kObj = 0.0005f, kHigh = 0.3f, kCock = 0.5f;           // object band / uncompressed object depth / cockpit depth
    const float objc[3] = {0.9f, 0.2f, 0.9f}, cockc[3] = {0.2f, 0.8f, 0.2f}, rawc[3] = {0.9f, 0.9f, 0.1f};
    Scene s = base;
    for (int e = 0; e < 2; ++e) {
        s.eye[e] = Img(W, H); fill(s.eye[e], 0, 0, W, H, 0, 0, 0, 1.0f);
        s.depth[e] = Depth(); s.bare[e] = Depth();
        auto rect = [&](int x0, int y0, int x1, int y1, const float c[3], float d) { fill(s.eye[e], x0, y0, x1, y1, c[0], c[1], c[2], 0.25f); fill_depth(s.depth[e], x0, y0, x1, y1, d); };
        rect(140, 60, 180, 80, objc, kObj);     // object inside the window
        rect(5, 60, 45, 80, objc, kObj);        // object in the periphery (left of the window)
        rect(140, 95, 180, 115, cockc, kCock);  // cockpit inside the window
        rect(190, 60, 220, 80, rawc, kHigh);    // object depth NOT compressed (no depth fix): above the band, must be kept
        rect(70, 88, 90, 100, objc, kObj);      // across the left window edge: the seam
        quantise(s.eye[e]);
        s.eye_t[e] = make_color(s.eye[e]); s.depth_t[e] = make_depth(s.depth[e]); s.bare_t[e] = make_depth(s.bare[e]);
    }
    BlitJob jf = base_job(s); job_world(jf, s, front, false);                  // flag OFF
    BlitJob jo = jf; jo.occlude_window = true;                                  // flag ON
    BlitJob jb = base_job(s, true); job_world(jb, s, front, false);            // bare depth: the pure world layer
    const WorldComposite wc = jf.world_eye[0];
    auto inwin = [&](int x, int y) { return world_ray_uv(wc, (x + 0.5f) / W, (y + 0.5f) / H).hit; };
    CHECK(inwin(160, 70) && inwin(160, 105) && inwin(200, 70) && !inwin(20, 70) && !inwin(5, 5), "scene geometry: window test of the chosen pixels");
    CHECK(window_occlude_keep(wc, 160.5f / W, 70.5f / H, kObj) == 0.0f && window_occlude_keep(wc, 20.5f / W, 70.5f / H, kObj) == 1.0f &&
          window_occlude_keep(wc, 160.5f / W, 70.5f / H, kCock) == 1.0f && window_occlude_keep(wc, 160.5f / W, 70.5f / H, kHigh) == 1.0f &&
          window_occlude_keep(wc, 160.5f / W, 70.5f / H, 0.0f) == 1.0f && window_occlude_keep(wc, 160.5f / W, 70.5f / H, std::nanf("")) == 1.0f &&
          window_occlude_keep(wc, 160.5f / W, 70.5f / H, kOccludeObjectDepth) == 1.0f, "CPU mirror: band, periphery, cockpit, zero, NaN, band edge");
    Result rf = eye_case(ch, s, "g_occlude_flag_off", jf, false, 0.012f, 4, false);
    Result ro = eye_case(ch, s, "g_occlude_on_frontal", jo, false, 0.012f, 6, true);
    Result rb = run_job(ch, jb, 3u);
    float p[4], q[4];
    if (rf.ok) { px_lin(rf.px[0], 160, 70, p); CHECK(near4(p, objc[0], objc[1], objc[2], 1.0f, 0.01f), "flag off: the window object is drawn (as before): %.3f %.3f %.3f", p[0], p[1], p[2]); }
    if (ro.ok && rb.ok && rf.ok) {
        for (int e = 0; e < 2; ++e) {
            px_lin(ro.px[e], 160, 70, p); px_lin(rb.px[e], 160, 70, q);
            CHECK(near4(p, q[0], q[1], q[2], q[3], 0.004f) && p[3] > 0.99f, "eye%d in-window object removed: pixel == the world layer: %.3f %.3f %.3f vs %.3f %.3f %.3f", e, p[0], p[1], p[2], q[0], q[1], q[2]);
            px_lin(ro.px[e], 20, 70, p); CHECK(near4(p, objc[0], objc[1], objc[2], 1.0f, 0.01f), "eye%d periphery object kept: %.3f %.3f %.3f", e, p[0], p[1], p[2]);
            px_lin(ro.px[e], 160, 105, p); CHECK(near4(p, cockc[0], cockc[1], cockc[2], 1.0f, 0.01f), "eye%d cockpit inside the window kept: %.3f %.3f %.3f", e, p[0], p[1], p[2]);
            px_lin(ro.px[e], 200, 70, p); CHECK(near4(p, rawc[0], rawc[1], rawc[2], 1.0f, 0.01f), "eye%d depth above the object band (no depth fix) kept: %.3f %.3f %.3f", e, p[0], p[1], p[2]);
            px_lin(ro.px[e], 5, 5, p); CHECK(near4(p, 0, 0, 0, 0.0f, 0.003f), "eye%d empty periphery unchanged (transparent)", e);
        }
        // pixels outside the object rects are identical with the flag on and off
        size_t diff = 0, in_band_rects = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const bool near_obj = (x >= 140 && x < 180 && y >= 60 && y < 80) || (x >= 70 && x < 90 && y >= 88 && y < 100);
            const bool same = std::memcmp(&ro.px[0][(size_t(y) * W + x) * 4], &rf.px[0][(size_t(y) * W + x) * 4], 4) == 0;
            if (near_obj) ++in_band_rects; else if (!same) ++diff;
        }
        CHECK(diff == 0 && in_band_rects > 0, "flag on changes only the in-window object pixels (%zu other pixels differ)", diff);
        // seam: across the left window edge keep ramps 0..1; the GPU must match the mirror (eye_case above) and show a partial value
        int partial = 0;
        for (int x = 70; x < 90; ++x) { const float k = window_occlude_keep(wc, (x + 0.5f) / W, 93.5f / H, kObj); if (k > 0.05f && k < 0.95f) ++partial; }
        INFO("seam: %d pixels with 0.05 < keep < 0.95 on row 93", partial);
        CHECK(partial >= 1, "seam ramp has partial pixels");
    }
    {   // flag on but nothing in the object band: bytes identical to flag off (the base scene: cockpit 0.5 / 0 only)
        BlitJob a = base_job(base); job_world(a, base, front, false); BlitJob b = a; b.occlude_window = true;
        Result ra = run_job(ch, a, 3u), rb2 = run_job(ch, b, 3u);
        CHECK(ra.ok && rb2.ok && ra.px[0] == rb2.px[0] && ra.px[1] == rb2.px[1], "no object band: flag on == flag off (both eyes, bytes)");
    }
    {   // use_depth = 0: the bit is not set; every pixel opaque as before
        BlitJob a = jf; a.use_depth = false; BlitJob b = a; b.occlude_window = true;
        Result ra = run_job(ch, a, 3u), rb2 = run_job(ch, b, 3u);
        CHECK(ra.ok && rb2.ok && ra.px[0] == rb2.px[0] && ra.px[1] == rb2.px[1], "use_depth=0: flag on == flag off (bytes)");
    }
    {   // extend on: window rays unchanged (the window object is replaced by the world window pixel); periphery object kept over the fill
        BlitJob a = jo; a.world_extend = true; BlitJob bb = jb; bb.world_extend = true;
        Result ra = eye_case(ch, s, "g_occlude_on_extend", a, false, 0.02f, 40, false), rbb = run_job(ch, bb, 1u);
        if (ra.ok && rbb.ok) {
            px_lin(ra.px[0], 160, 70, p); px_lin(rbb.px[0], 160, 70, q); CHECK(near4(p, q[0], q[1], q[2], 1.0f, 0.004f), "extend: in-window object removed");
            px_lin(ra.px[0], 20, 70, p); CHECK(near4(p, objc[0], objc[1], objc[2], 1.0f, 0.01f), "extend: periphery object kept over the fill");
        }
    }
    {   // rotated head + feather + Reinhard: GPU vs mirror over the whole image
        const float pos[3] = {0.05f, 0.02f, -0.03f};
        const ViewSetup rot = make_view("rotated", 12.0f, 8.0f, pos, 0.1f, 0.05f);
        BlitJob a = base_job(s); job_world(a, s, rot, false); a.occlude_window = true; a.edge_feather = 0.1f; a.tonemap = Tonemap::reinhard;
        eye_case(ch, s, "g_occlude_rotated_feather", a, false, 0.015f, 12, false);
    }
}

// ======================================================================= J-world-cache (key xr_world_cache; docs/review/world-cache-impl.md)
// CPU part: the frame maths the shader mirrors, the age weight, the conservative write rectangle, the fill blend and the budget. No GPU.
static void case_world_cache_cpu() {
    std::printf("[w0] world cache: CPU mirrors (frame maths, age, rectangle, fill, budget)\n");
    const float lens[4] = {std::tan(deg2rad(31.6f)), std::tan(deg2rad(31.6f)), std::tan(deg2rad(19.085f)), std::tan(deg2rad(19.085f))};
    const float looks_y[] = {-3.0f, -1.2f, 0.0f, 0.7f, 2.9f}, looks_p[] = {-0.6f, 0.0f, 0.5f, 1.2f, 1.7f};
    int bad_inv = 0, bad_centre = 0, bad_rect = 0, rects = 0;
    for (float ly : looks_y) for (float lp : looks_p) {
        const float d[3] = {0.3f, -0.4f, -0.8f};
        float g[3], d2[3]; world_cache_to_cache(ly, lp, d, g); world_cache_to_window(ly, lp, g, d2);
        if (std::fabs(d2[0] - d[0]) > 1e-5f || std::fabs(d2[1] - d[1]) > 1e-5f || std::fabs(d2[2] - d[2]) > 1e-5f) ++bad_inv;
        // the window centre lies at az = -look yaw, el = look pitch (the world quad is turned by +yaw about +Y, its centre is at (-sin, 0, -cos) * dist)
        const float axis[3] = {0.0f, 0.0f, -1.0f}; float c[3]; world_cache_to_cache(ly, lp, axis, c);
        float u, v; world_cache_dir_uv(c, u, v);
        float az = (u - 0.5f) * 2.0f * kPiH + ly; if (az > kPiH) az -= 2.0f * kPiH; if (az < -kPiH) az += 2.0f * kPiH;
        const float el = (0.5f - v) * kPiH;
        if (lp <= 1.5f && (std::fabs(az) > 1e-4f || std::fabs(el - lp) > 1e-4f)) ++bad_centre;   // beyond the pole (pitch > 90 deg) az flips: not checked
        // every direction of the window rectangle lies inside the conservative rectangle, for two cache sizes
        for (int sz = 0; sz < 2; ++sz) {
            const uint32_t cw = sz ? 3840u : 960u, ch = sz ? 1920u : 480u;
            const CacheRect r = world_cache_update_rect(lens, ly, lp, cw, ch);
            ++rects;
            for (int iy = 0; iy <= 32; ++iy) for (int ix = 0; ix <= 32; ++ix) {
                const float tx = -lens[0] + (lens[0] + lens[1]) * float(ix) / 32.0f, ty = lens[2] - (lens[2] + lens[3]) * float(iy) / 32.0f;
                const float w[3] = {tx, ty, -1.0f}; float gg[3]; world_cache_to_cache(ly, lp, w, gg);
                float cu, cv; world_cache_dir_uv(gg, cu, cv);
                const int x = std::min(std::max(int(std::floor(cu * float(cw))), 0), int(cw) - 1), y = std::min(std::max(int(std::floor(cv * float(ch))), 0), int(ch) - 1);
                if (x < int(r.x0) || x >= int(r.x1) || y < int(r.y0) || y >= int(r.y1)) ++bad_rect;
            }
        }
    }
    CHECK(bad_inv == 0, "to_window(to_cache(d)) == d (%d failures)", bad_inv);
    CHECK(bad_centre == 0, "window centre at az=-look yaw, el=look pitch (%d failures)", bad_centre);
    CHECK(bad_rect == 0, "update rectangle covers every window direction (%d texels outside over %d rectangles)", bad_rect, rects);
    {   const CacheRect r = world_cache_update_rect(lens, 0.0f, 0.0f, 3840, 1920);
        const double frac = double(r.x1 - r.x0) * double(r.y1 - r.y0) / (3840.0 * 1920.0);
        INFO("update rectangle at look 0, 3840x1920: %ux%u texels = %.1f%% of the cache", r.x1 - r.x0, r.y1 - r.y0, frac * 100.0);
        CHECK(frac < 0.20, "the write rectangle is a small part of the cache (%.3f)", frac); }
    {   float g[3] = {0.2f, -0.5f, -0.7f}, h[3], u, v; world_cache_dir_uv(g, u, v); world_cache_uv_dir(u, v, h);
        const float n = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        CHECK(std::fabs(h[0] - g[0] / n) < 1e-5f && std::fabs(h[1] - g[1] / n) < 1e-5f && std::fabs(h[2] - g[2] / n) < 1e-5f, "uv_dir(dir_uv(g)) == g/|g|");
        float a[3] = {0, 0, -1}, b[3] = {1, 0, 0}, c[3] = {0, 1, 0}, e[3] = {0, 0, 1}; float ua, va, ub, vb, uc, vc, ue, ve;
        world_cache_dir_uv(a, ua, va); world_cache_dir_uv(b, ub, vb); world_cache_dir_uv(c, uc, vc); world_cache_dir_uv(e, ue, ve);
        CHECK(std::fabs(ua - 0.5f) < 1e-6f && std::fabs(va - 0.5f) < 1e-6f, "ahead (-Z) is the cache centre");
        CHECK(std::fabs(ub - 0.75f) < 1e-6f && std::fabs(vb - 0.5f) < 1e-6f, "+X (right) is az +90 deg");
        CHECK(std::fabs(vc) < 1e-6f, "+Y is the top row");
        CHECK(std::fabs(ue - 1.0f) < 1e-5f || std::fabs(ue) < 1e-5f, "behind (+Z) is the az seam"); }
    {   const float now = 100.0f;
        CHECK(world_cache_age_weight(0.0f, now) == 0.0f && world_cache_age_weight(-5.0f, now) == 0.0f && world_cache_age_weight(std::nanf(""), now) == 0.0f, "never written / negative / NaN stamp = 0");
        CHECK(world_cache_age_weight(now, now) == 1.0f && world_cache_age_weight(now - kCacheFullS, now) == 1.0f && world_cache_age_weight(now + 5.0f, now) == 1.0f, "age 0, the full window and a future stamp weigh 1");
        CHECK(std::fabs(world_cache_age_weight(now - 7.5f, now) - 0.5f) < 1e-6f, "age 7.5 s weighs 0.5 (%.4f)", world_cache_age_weight(now - 7.5f, now));
        CHECK(world_cache_age_weight(now - kCacheZeroS, now) == 0.0f && world_cache_age_weight(now - 60.0f, now) == 0.0f, "age >= 12 s weighs 0"); }
    {   CHECK(world_cache_horizon_conf(0.0f) == 0.0f && world_cache_horizon_conf(kCacheFillConfLo) == 0.0f && world_cache_horizon_conf(kCacheFillConfHi) == 1.0f &&
              std::fabs(world_cache_horizon_conf(0.5f * (kCacheFillConfLo + kCacheFillConfHi)) - 0.5f) < 1e-6f, "horizon confidence ramp");
        CHECK(std::fabs(world_cache_fill_t(0.0f, 0.0f, 1.0f) - 0.5f) < 1e-6f && world_cache_fill_t(1.0f, 0.0f, 1.0f) == 1.0f && world_cache_fill_t(-1.0f, 0.0f, 1.0f) == 0.0f, "fill: sky above, ground below, 0.5 on the horizon");
        CHECK(std::fabs(world_cache_fill_t(0.0f, 0.8f, 0.0f) - 0.5f) < 1e-6f && std::fabs(world_cache_fill_t(0.8f, 0.8f, 1.0f) - 0.5f) < 1e-6f, "fill: the horizon elevation counts only with confidence"); }
    {   const float u0 = 0.3f, v0 = 0.6f, tx = -lens[0] + u0 * (lens[0] + lens[1]), ty = lens[2] - v0 * (lens[2] + lens[3]);
        const float ly = 0.7f, lp = 0.4f, w[3] = {tx, ty, -1.0f}; float g[3]; world_cache_to_cache(ly, lp, w, g); float cu, cv; world_cache_dir_uv(g, cu, cv);
        const CacheWindowSample s = world_cache_texel_window(lens, ly, lp, cu, cv);
        CHECK(s.hit && std::fabs(s.u - u0) < 1e-3f && std::fabs(s.v - v0) < 1e-3f, "cache texel -> window uv inverts window uv -> cache texel (%.4f %.4f)", s.u, s.v);
        const CacheWindowSample out = world_cache_texel_window(lens, ly, lp, 0.02f, 0.9f);
        CHECK(!out.hit, "a texel far from the window is not hit"); }
    CHECK(uint64_t(kWorldCacheWidth) * kWorldCacheHeight * 8u <= kWorldCacheBudgetBytes && kBlitConstantsAll == 28 && kBlitConstantsAll <= 32, "memory budget (%.2f MiB) and root constants", double(uint64_t(kWorldCacheWidth) * kWorldCacheHeight * 8u) / 1048576.0);
}

constexpr int kCW = 960, kCH = 480;   // the harness' cache size (production: kWorldCacheWidth x kWorldCacheHeight)
static void job_cache(BlitJob& j, const ViewSetup& v, bool write, float now, bool clear = false) {
    j.world_cache = true; j.world_cache_write = write; j.world_cache_clear = clear;
    j.cache_look_yaw = v.yaw; j.cache_look_pitch = v.pitch; j.cache_now_s = now;   // anchor yaw 0: the rendered look is the plane yaw/pitch
    for (int i = 0; i < 4; ++i) j.cache_lens[i] = v.lens[i];
    j.cache_w = kCW; j.cache_h = kCH;
}
static Scene with_world(const Scene& s, const Img& w) { Scene o = s; o.world = w; o.world_t = make_color(w); return o; }
static void quat_mul(const float a[4], const float b[4], float o[4]) { // (x, y, z, w) Hamilton product a*b
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static ViewSetup make_view_roll(const char* name, float head_yaw_deg, float head_pitch_deg, float roll_deg, const float pos[3], float anchor_yaw, float look_pitch) {
    ViewSetup v = make_view(name, head_yaw_deg, head_pitch_deg, pos, anchor_yaw, look_pitch);
    const float hr = 0.5f * deg2rad(roll_deg), qz[4] = {0.0f, 0.0f, std::sin(hr), std::cos(hr)};
    for (int e = 0; e < 2; ++e) { float q[4]; quat_mul(v.eye[e].pose.orientation, qz, q); for (int i = 0; i < 4; ++i) v.eye[e].pose.orientation[i] = q[i]; }
    return v;
}
// Run a bare-world cache job, apply the same clear/write to the CPU model, and compare BOTH eyes pixel by pixel with the model's reference image.
static Result cache_case(Chains& ch, const Scene& s, const char* name, const BlitJob& job, CacheModel& m, float tol, size_t allow_bad, bool sheet = false, bool docs = false) {
    Result r = run_job(ch, job, 3u);
    CHECK(r.ok && r.facts.ok, "%s: job did not run", name);
    if (!r.ok) return r;
    CHECK(r.facts.cache_read && r.facts.cache_written == job.world_cache_write, "%s: cache_read=%d cache_written=%d", name, int(r.facts.cache_read), int(r.facts.cache_written));
    CHECK(r.facts.sources_restored && r.facts.swapchain_untouched && r.facts.barriers_out == r.facts.barriers_back, "%s: barrier contract", name);
    if (job.world_cache_clear) m.init(m.cw, m.ch);
    if (job.world_cache_write) m.write(s.world, job.cache_lens, job.cache_look_yaw, job.cache_look_pitch, job.cache_now_s);
    for (int e = 0; e < 2; ++e) {
        Cfg c = cfg_for(job, e); c.cache = &m; c.cly = job.cache_look_yaw; c.clp = job.cache_look_pitch; c.cnow = job.cache_now_s;
        const Img ref = ref_eye(s, c, e, s.bare[e]);
        const Cmp cm = compare(r.px[e], ref, tol);
        INFO("%-30s eye%d max_err=%.4f bad=%zu/%zu", name, e, cm.max_err, cm.bad, cm.n);
        CHECK(cm.bad <= allow_bad, "%s eye%d: %zu pixels differ from the cache model by > %.3f (first at %d,%d)", name, e, cm.bad, tol, cm.fx, cm.fy);
        if (sheet && e == 0) save_sheet(name, r.px[0], ref, docs);
    }
    return r;
}
// WORLD-WINDOW FILTER (eyeres > 0; eye_res.h filter_select / filter_offset, mode_bits bit 21) on the REAL backend + shader. The harness eye (100x80 deg over
// 320x180) minifies the 320x180 world window about 1.9x / 2.4x, so the filter takes a 2x2 grid. (1) a 1-texel checkerboard world: the GPU output equals the CPU
// mirror (footprint grid of bilinear taps) and the aliasing (mean |pixel - 0.5|) drops; (2) the scene's gradient world: equals the CPU mirror; (3) magnified
// (eye 4x larger): every axis 1 tap, so bit on == bit off byte-for-byte; (4) bit off: exactly the old CPU mirror (the old single tap).
static void case_world_filter(Chains& ch, const Scene& s) {
    std::printf("[w] world-window filter (eyeres, mode_bits bit 21)\n");
    const float origin[3] = {0, 0, 0};
    const ViewSetup front = make_view("frontal", 0, 0, origin, 0, 0);
    Img cb(W, H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { const float v = ((x + y) & 1) ? 1.0f : 0.0f; float* q = cb.at(x, y); q[0] = q[1] = q[2] = v; q[3] = 1.0f; }
    const Scene sc = with_world(s, cb);
    auto mirror = [&](const Img& world, const WorldComposite& wc, int cw, int chh, int x, int y, float out[3]) -> bool {   // filtered linear rgb (clamp tonemap)
        const WorldHit h = world_ray_uv(wc, (x + 0.5f) / cw, (y + 0.5f) / chh);
        const WorldHit hx = world_ray_uv(wc, (x + 1.5f) / cw, (y + 0.5f) / chh), hy = world_ray_uv(wc, (x + 0.5f) / cw, (y + 1.5f) / chh);
        if (!h.hit || !hx.hit || !hy.hit) return false;   // window edge: the neighbour plane uv lies outside [0,1] (the shader still filters; not compared)
        const float ju[2] = {hx.u - h.u, hx.v - h.v}, jv[2] = {hy.u - h.u, hy.v - h.v};
        const acz::eyeres::FilterGrid fg = acz::eyeres::filter_select(ju, jv, uint32_t(world.w), uint32_t(world.h));
        float acc[3] = {0, 0, 0};
        for (uint32_t j = 0; j < fg.gv; ++j) for (uint32_t i = 0; i < fg.gu; ++i) {
            float o[2], t[4]; acz::eyeres::filter_offset(fg, i, j, ju, jv, o); bilinear(world, h.u + o[0], h.v + o[1], t);
            for (int k = 0; k < 3; ++k) acc[k] += t[k];
        }
        for (int k = 0; k < 3; ++k) out[k] = sat(acc[k] / float(fg.taps()));
        return true;
    };
    for (int pass = 0; pass < 2; ++pass) {
        const Scene& sx = pass == 0 ? sc : s;
        const char* nm = pass == 0 ? "checkerboard" : "gradient";
        BlitJob off = base_job(sx, true); job_world(off, sx, front, false);
        BlitJob on = off; on.world_filter = true;
        const Result ro = run_job(ch, off, 3u), rn = run_job(ch, on, 3u);
        CHECK(ro.ok && rn.ok && ro.facts.ok && rn.facts.ok, "world filter %s: jobs ran", nm);
        if (!ro.ok || !rn.ok) continue;
        size_t n = 0, bad = 0; double dev_off = 0, dev_on = 0, max_err = 0; unsigned grids[5] = {};
        for (int e = 0; e < 2; ++e) for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            float want[3]; if (!mirror(sx.world, on.world_eye[e], W, H, x, y, want)) continue;
            float pn[4], po[4]; px_lin(rn.px[e], x, y, pn); px_lin(ro.px[e], x, y, po);
            float err = 0; for (int k = 0; k < 3; ++k) err = std::max(err, std::fabs(pn[k] - want[k]));
            max_err = std::max(max_err, double(err)); if (err > 0.03f) ++bad;
            dev_on += std::fabs(pn[0] - 0.5f); dev_off += std::fabs(po[0] - 0.5f); ++n;
            const WorldHit h = world_ray_uv(on.world_eye[e], (x + 0.5f) / W, (y + 0.5f) / H), hx = world_ray_uv(on.world_eye[e], (x + 1.5f) / W, (y + 0.5f) / H);
            ++grids[std::min(4u, acz::eyeres::filter_grid(acz::eyeres::footprint_texels(hx.u - h.u, hx.v - h.v, W, H)))];
        }
        INFO("world filter %-12s window px=%zu max_err=%.4f bad=%zu  mean|px-0.5| off=%.4f on=%.4f  u-grids 1:%u 2:%u 3:%u 4:%u", nm, n, max_err, bad,
             n ? dev_off / n : 0.0, n ? dev_on / n : 0.0, grids[1], grids[2], grids[3], grids[4]);
        CHECK(n > 10000 && bad <= n / 200, "world filter %s: GPU equals the CPU footprint-grid mirror (%zu of %zu pixels off by > 0.03)", nm, bad, n);
        CHECK(grids[2] + grids[3] + grids[4] > n / 2, "world filter %s: the harness eye minifies (grid > 1 on most window pixels)", nm);
        if (pass == 0) CHECK(dev_on < 0.5 * dev_off, "world filter checkerboard: aliasing drops (mean |px - 0.5| %.4f -> %.4f)", n ? dev_off / n : 0.0, n ? dev_on / n : 0.0);
        // bit off: the old single tap = the existing CPU mirror of world_layer (the job without the request is unchanged)
        const Cmp m = compare(ro.px[0], ref_eye(sx, cfg_for(off, 0), 0, sx.bare[0]), 0.012f);
        CHECK(m.bad <= 4, "world filter %s off: the old single tap (%zu px differ from the old mirror)", nm, m.bad);
    }
    {   // magnified: the eye 4x larger than the window's texels -> 1 tap per axis -> byte-identical to the job without the request
        auto big = make_chains(rgba8_unorm, 4 * W, 4 * H);
        CHECK(big->ok, "world filter: magnified chains");
        if (big->ok) {
            BlitJob off = base_job(sc, true, 4 * W, 4 * H); job_world(off, sc, front, false);
            BlitJob on = off; on.world_filter = true;
            const Result ro = run_job(*big, off, 3u), rn = run_job(*big, on, 3u);
            CHECK(ro.ok && rn.ok && ro.px[0] == rn.px[0] && ro.px[1] == rn.px[1], "world filter magnified: bit on == bit off byte-for-byte (1 tap)");
            big->blit->destroy();
        }
    }
}
static void case_world_cache(Chains& cc, const Scene& s, bool docs) {
    std::printf("[w] world cache (periphery fill v2) on WARP: %dx%d cache\n", kCW, kCH);
    const float origin[3] = {0, 0, 0};
    const ViewSetup frontA = make_view("A", 0, 0, origin, 0.0f, 0.0f);
    float p[4];
    // ---- key off: any cache fields without world_cache leave every byte alone and create nothing
    {
        BlitJob a = base_job(s, true); job_world(a, s, frontA, false);
        BlitJob b = a; b.cache_look_yaw = 0.7f; b.cache_look_pitch = 0.2f; b.cache_now_s = 99.0f; b.cache_w = 512; b.cache_h = 256; b.world_cache_write = true; b.world_cache_clear = true;
        for (int i = 0; i < 4; ++i) b.cache_lens[i] = frontA.lens[i];
        Result ra = run_job(cc, a, 3u), rb = run_job(cc, b, 3u);
        CHECK(ra.ok && rb.ok && ra.px[0] == rb.px[0] && ra.px[1] == rb.px[1], "key off: cache fields without world_cache change no byte (both eyes)");
        CHECK(rb.ok && !rb.facts.cache_read && !rb.facts.cache_written && rb.facts.cache_bytes == 0, "key off: no cache is created or used (bytes=%llu)", rb.ok ? (unsigned long long)rb.facts.cache_bytes : 0ull);
        BlitJob c = a; job_cache(c, frontA, false, 5.0f); c.cache_look_yaw = std::nanf("");   // a non-finite look: the cache is refused, the job still runs as without it
        Result rc = run_job(cc, c, 3u);
        CHECK(rc.ok && !rc.facts.cache_read && rc.px[0] == ra.px[0] && rc.px[1] == ra.px[1], "non-finite look: cache refused (cache_read=false), output == the no-cache job");
    }
    // ---- never-seen directions get the sky/ground fill (no black): synthetic horizon at world row 0.5
    Img sg(W, H);
    fill(sg, 0, 0, W, H / 2, 0.2f, 0.4f, 0.9f, 1.0f); fill(sg, 0, H / 2, W, H, 0.30f, 0.22f, 0.10f, 1.0f); quantise(sg);
    const Scene sgs = with_world(s, sg);
    CacheModel m; m.init(kCW, kCH);
    {
        BlitJob j = base_job(sgs, true); job_world(j, sgs, frontA, false); job_cache(j, frontA, true, 20.0f, true);
        Result r = cache_case(cc, sgs, "w_fill_sky_ground", j, m, 0.045f, 60, true, docs);
        CHECK(r.ok && r.facts.cache_bytes == uint64_t(kCW) * kCH * 8u, "cache bytes = %llu", r.ok ? (unsigned long long)r.facts.cache_bytes : 0ull);
        INFO("model fill: sky %.3f %.3f %.3f ground %.3f %.3f %.3f horizon el %.4f conf %.3f", m.sky[0], m.sky[1], m.sky[2], m.ground[0], m.ground[1], m.ground[2], m.hz[0], m.hz[1]);
        CHECK(std::fabs(m.sky[2] - 0.9f) < 0.01f && std::fabs(m.ground[0] - 0.30f) < 0.01f && m.hz[1] > 0.99f && std::fabs(m.hz[0]) < 0.01f, "fill measurement: sky = top rows, ground = bottom rows, horizon fitted at the window centre row");
        if (r.ok) {
            int sky_n = 0, ground_n = 0, bad = 0, holes = 0;
            for (int e = 0; e < 2; ++e) for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
                if (world_ray_uv(j.world_eye[e], u, v).hit) continue;                       // the live window
                px_lin(r.px[e], x, y, p);
                if (p[3] < 0.99f) ++holes;
                float d[3], g[3]; for (int i = 0; i < 3; ++i) d[i] = j.world_eye[e].base[i] + u * j.world_eye[e].ux[i] + v * j.world_eye[e].vy[i];
                world_cache_to_cache(0.0f, 0.0f, d, g);
                const float el = std::atan2(g[1], std::sqrt(g[0] * g[0] + g[2] * g[2]));
                float uc, vc; world_cache_dir_uv(g, uc, vc);
                const bool written = world_cache_texel_window(frontA.lens, 0.0f, 0.0f, uc, vc).hit;   // texels the window wrote this frame keep window content
                if (written) continue;
                if (el > 0.3f) { ++sky_n; if (!near4(p, 0.2f, 0.4f, 0.9f, 1.0f, 0.03f)) ++bad; }
                if (el < -0.3f) { ++ground_n; if (!near4(p, 0.30f, 0.22f, 0.10f, 1.0f, 0.03f)) ++bad; }
            }
            INFO("never-seen periphery: %d sky pixels, %d ground pixels, %d wrong, %d transparent", sky_n, ground_n, bad, holes);
            CHECK(sky_n > 500 && ground_n > 500 && bad <= 40, "never-seen directions show the sky colour above and the ground colour below (%d of %d wrong)", bad, sky_n + ground_n);
            CHECK(holes == 0, "the periphery is opaque everywhere: no black hole outside the window (%d transparent pixels)", holes);
        }
    }
    // ---- window interior == the extend path's window pixels (bytes within 1 LSB); the cache only changes pixels outside the window
    {
        BlitJob je = base_job(s, true); job_world(je, s, frontA, false); je.world_extend = true;
        BlitJob jc = base_job(s, true); job_world(jc, s, frontA, false); job_cache(jc, frontA, true, 21.0f, true);
        Result re = run_job(cc, je, 3u), rc = run_job(cc, jc, 3u);
        m.init(kCW, kCH); m.write(s.world, jc.cache_lens, jc.cache_look_yaw, jc.cache_look_pitch, jc.cache_now_s);
        if (re.ok && rc.ok) {
            int maxd = 0; size_t win = 0, differ_out = 0;
            for (int e = 0; e < 2; ++e) for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const bool in = world_ray_uv(jc.world_eye[e], (x + 0.5f) / W, (y + 0.5f) / H).hit;
                for (int k = 0; k < 4; ++k) {
                    const int d = std::abs(int(re.px[e][(size_t(y) * W + x) * 4 + k]) - int(rc.px[e][(size_t(y) * W + x) * 4 + k]));
                    if (in) maxd = std::max(maxd, d); else if (d > 8) ++differ_out;
                }
                if (in) ++win;
            }
            INFO("window interior: %zu pixels, max byte difference vs extend = %d; outside pixels that differ from extend: %zu", win, maxd, differ_out);
            CHECK(win > 2000 && maxd <= 1, "window interior unchanged by the cache (max byte diff %d over %zu pixels)", maxd, win);
            CHECK(differ_out > 100, "the periphery is NOT the extend fill any more (%zu pixels differ)", differ_out);
        }
    }
    // ---- frame A (gradient world, look 0) then frame B (inverted world, look yaw +40 deg): the old window stays visible left of ... where the head still looks
    Img inv(W, H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float* q = inv.at(x, y); q[0] = 1.0f - float(x) / (W - 1); q[1] = 1.0f - float(y) / (H - 1); q[2] = 0.75f; q[3] = 1.0f; }
    quantise(inv);
    const Scene sinv = with_world(s, inv);
    const ViewSetup viewB = make_view("B", 0, 0, origin, deg2rad(40.0f), 0.0f);
    {
        m.init(kCW, kCH);
        BlitJob ja = base_job(s, true); job_world(ja, s, frontA, false); job_cache(ja, frontA, true, 30.0f, true);
        cache_case(cc, s, "w_roundtrip_A", ja, m, 0.045f, 120);
        BlitJob jb = base_job(sinv, true); job_world(jb, sinv, viewB, false); job_cache(jb, viewB, true, 31.0f);
        Result r = cache_case(cc, sinv, "w_roundtrip_B_periphery_is_A", jb, m, 0.045f, 120, true, docs);
        if (r.ok) {
            for (int e = 0; e < 2; ++e) {
                px_lin(r.px[e], 160, 90, p);   // straight ahead: outside window B (az -71.6..-8.4 deg), inside the old window A -> A's centre (0.5, 0.5, 0.25)
                CHECK(near4(p, 1.0f, 1.0f, 1.0f, 1.0f, 0.04f), "eye%d: ahead shows window A (its white centre marker) from the cache, not B: %.3f %.3f %.3f", e, p[0], p[1], p[2]);
            }
            size_t a_like = 0, b_like = 0;
            for (int y = 0; y < H; y += 3) for (int x = 0; x < W; x += 3) { px_lin(r.px[0], x, y, p); if (std::fabs(p[2] - 0.25f) < 0.03f) ++a_like; if (std::fabs(p[2] - 0.75f) < 0.03f) ++b_like; }
            INFO("frame B: pixels with A's blue (cache) %zu, with B's blue (live window) %zu", a_like, b_like);
            CHECK(a_like > 300 && b_like > 300, "both the cached window A and the live window B are on screen");
        }
        // ---- age fade: A was written at t=30 (the centre pixel's texel), B at 31. Read only.
        float c_full[3] = {0, 0, 0}, c_half[3] = {0, 0, 0}, c_old[3] = {0, 0, 0};
        const float ages[3] = {32.0f, 37.5f, 43.0f};
        float* outs[3] = {c_full, c_half, c_old};
        for (int k = 0; k < 3; ++k) {
            BlitJob jr = base_job(sinv, true); job_world(jr, sinv, viewB, false); job_cache(jr, viewB, false, ages[k]);
            char nm[40]; std::snprintf(nm, sizeof nm, "w_age_t%.1f", ages[k]);
            Result rr = cache_case(cc, sinv, nm, jr, m, 0.045f, 120);
            if (rr.ok) { px_lin(rr.px[0], 160, 90, p); outs[k][0] = p[0]; outs[k][1] = p[1]; outs[k][2] = p[2]; }
        }
        float fcol[3]; { const float g[3] = {0, 0, -1}; m.fill_color(g, fcol); }
        const float d_full = std::fabs(c_full[0] - 1.0f) + std::fabs(c_full[1] - 1.0f) + std::fabs(c_full[2] - 1.0f);
        const float d_old = std::fabs(c_old[0] - fcol[0]) + std::fabs(c_old[1] - fcol[1]) + std::fabs(c_old[2] - fcol[2]);
        float d_half = 0; for (int i = 0; i < 3; ++i) d_half += std::fabs(c_half[i] - 0.5f * (c_full[i] + c_old[i]));
        INFO("age fade at the centre texel: age 2 s -> %.3f %.3f %.3f ; 7.5 s -> %.3f %.3f %.3f ; 13 s -> %.3f %.3f %.3f (fill %.3f %.3f %.3f)",
             c_full[0], c_full[1], c_full[2], c_half[0], c_half[1], c_half[2], c_old[0], c_old[1], c_old[2], fcol[0], fcol[1], fcol[2]);
        CHECK(d_full < 0.06f, "age 2 s (inside the full window) shows the cached colour (%.3f)", d_full);
        CHECK(d_old < 0.06f, "age 13 s (past the fade) shows exactly the neutral fill (%.3f)", d_old);
        CHECK(d_half < 0.06f, "age 7.5 s is the half-way mix of cache and fill (%.3f)", d_half);
    }
    // ---- roll / yaw / pitch of the head and of the rendered look; the az seam; a pole-touching window
    {
        const float pos[3] = {0.02f, 0.0f, 0.0f};
        const ViewSetup v1 = make_view_roll("v1", 25.0f, -15.0f, 20.0f, pos, deg2rad(10.0f), deg2rad(5.0f));
        BlitJob j1 = base_job(sinv, true); job_world(j1, sinv, v1, false); job_cache(j1, v1, false, 32.0f);
        cache_case(cc, sinv, "w_view_roll20_yaw25_pitch-15", j1, m, 0.045f, 160, true, docs);
        const ViewSetup vc = make_view("C", 170.0f, 0.0f, origin, deg2rad(170.0f), 0.0f);   // the window straddles the az = +-180 deg seam
        BlitJob jc = base_job(s, true); job_world(jc, s, vc, false); job_cache(jc, vc, true, 40.0f);
        cache_case(cc, s, "w_seam_write_yaw170", jc, m, 0.045f, 160);
        const ViewSetup vr = make_view("C2", 150.0f, 5.0f, origin, 0.0f, 0.0f);
        BlitJob jr = base_job(sinv, true); job_world(jr, sinv, vr, false); job_cache(jr, vr, false, 41.0f);
        cache_case(cc, sinv, "w_seam_read", jr, m, 0.045f, 160, true, docs);
        const ViewSetup vd = make_view("D", 0.0f, 60.0f, origin, 0.0f, deg2rad(60.0f));    // look pitch 60 deg: the window cone touches the pole
        BlitJob jd = base_job(s, true); job_world(jd, s, vd, false); job_cache(jd, vd, true, 50.0f);
        cache_case(cc, s, "w_pole_write_pitch60", jd, m, 0.045f, 200);
        const ViewSetup vq = make_view_roll("E", -30.0f, 40.0f, -35.0f, pos, deg2rad(-20.0f), deg2rad(-10.0f));
        BlitJob jq = base_job(sinv, true); job_world(jq, sinv, vq, false); job_cache(jq, vq, false, 51.0f);
        cache_case(cc, sinv, "w_view_roll-35_yaw-30_pitch40", jq, m, 0.045f, 200);
    }
    // ---- non-finite world texels are not written: the cache keeps the previous content there
    {
        m.init(kCW, kCH);
        BlitJob ja = base_job(s, true); job_world(ja, s, frontA, false); job_cache(ja, frontA, true, 60.0f, true);
        cache_case(cc, s, "w_nan_baseline", ja, m, 0.045f, 120);
        Img bad = s.world; const float nan = std::numeric_limits<float>::quiet_NaN();
        fill(bad, 140, 70, 180, 110, nan, nan, nan, 1.0f);
        const Scene sbad = with_world(s, bad);
        BlitJob jn = base_job(sbad, true); job_world(jn, sbad, frontA, false); job_cache(jn, frontA, true, 61.0f);
        Result rn = run_job(cc, jn, 3u);
        CHECK(rn.ok && rn.facts.cache_written, "NaN patch frame ran");
        // read it back with the window moved away (look yaw -60 deg) so the patch region is periphery: it must still hold frame A's content
        const ViewSetup away = make_view("away", 0, 0, origin, deg2rad(-60.0f), 0.0f);
        BlitJob jr = base_job(s, true); job_world(jr, s, away, false); job_cache(jr, away, false, 62.0f);
        Result rr = run_job(cc, jr, 3u);
        if (rr.ok) {
            px_lin(rr.px[0], 160, 90, p);
            CHECK(std::isfinite(p[0]) && near4(p, 1.0f, 1.0f, 1.0f, 1.0f, 0.05f), "NaN texels were not written: the patch keeps frame A's colour %.3f %.3f %.3f", p[0], p[1], p[2]);
        }
    }
    // ---- a different requested size recreates (and so clears) the cache; the budget is respected
    {
        BlitJob j = base_job(s, true); job_world(j, s, frontA, false); job_cache(j, frontA, false, 70.0f);
        j.cache_w = 800; j.cache_h = 400;
        Result r = run_job(cc, j, 3u);
        CHECK(r.ok && r.facts.cache_read && !r.facts.cache_written && r.facts.cache_bytes == 800ull * 400 * 8, "resize: new cache of %llu bytes", r.ok ? (unsigned long long)r.facts.cache_bytes : 0ull);
        if (r.ok) { px_lin(r.px[0], 160, 90, p); px_lin(r.px[0], 5, 5, p); CHECK(p[3] > 0.99f && near4(p, 0, 0, 0, 1.0f, 0.004f), "resize cleared the cache (and the fill): black until the next write, opaque (%.3f %.3f %.3f)", p[0], p[1], p[2]); }
        BlitJob big = base_job(s, true); job_world(big, s, frontA, false); job_cache(big, frontA, true, 71.0f);
        big.cache_w = 8192; big.cache_h = 4096;   // 256 MiB: over the 64 MiB budget
        Result rb = run_job(cc, big, 3u);
        CHECK(rb.ok && !rb.facts.cache_read && !rb.facts.cache_written, "over-budget size refused (cache_read=false), the frame still renders");
    }
}

// ----------------------------------------------------------------- J-terrain-render: the REAL shader's terrain pass vs acz::terrain::trace (the CPU reference)
static Img ref_terrain(const acz::terrain::StageData& st, const acz::terrain::Atlas& at, const BlitJob& j, int eye, const Scene& s) {
    const Cfg c = cfg_for(j, eye);
    Img out(W, H);
    const WorldComposite& wc = j.world_eye[eye];
    const TerrainEye& te = j.terrain_eye[eye];
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const float u = (float(x) + 0.5f) / float(W), v = (float(y) + 0.5f) / float(H);
        float d[3], dw[3];
        for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + u * wc.ux[i] + v * wc.vy[i];
        for (int i = 0; i < 3; ++i) dw[i] = te.right[i] * d[0] + te.up[i] * d[1] + te.fwd[i] * (-d[2]);
        const float l = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
        for (int i = 0; i < 3; ++i) dw[i] /= l;
        const acz::terrain::Trace tr = acz::terrain::trace(st, at, te.eye, dw, j.terrain_haze_l, j.terrain_far);
        Shade base{sat(tr.rgb[0] * c.exposure), sat(tr.rgb[1] * c.exposure), sat(tr.rgb[2] * c.exposure), 1.0f};
        Shade p = base;
        if (j.terrain_blend) { const Shade wl = world_layer_ref(s, c, u, v); p = {wl.r + (1 - wl.a) * base.r, wl.g + (1 - wl.a) * base.g, wl.b + (1 - wl.a) * base.b, wl.a + (1 - wl.a) * base.a}; }
        float* o = out.at(x, y); o[0] = p.r; o[1] = p.g; o[2] = p.b; o[3] = p.a;
    }
    return out;
}
static void case_terrain(Chains& ch, const Scene& s, bool docs) {
    std::printf("[t] terrain-render (shader ray march vs CPU reference)\n");
    namespace T = acz::terrain;
    T::StageData st = T::make_synthetic_stage(3, false);
    T::Atlas at; T::build_atlas(st, at);
    const float origin[3] = {0, 0, 0};
    const ViewSetup v = make_view("terrain", 20.0f, -8.0f, origin, 0.1f, 0);
    T::Conventions conv;
    auto make_job = [&](const T::StageData& stage, const T::Atlas& atlas, uint64_t gen, float yaw_deg, float pitch_deg, float alt, bool blend) {
        BlitJob j = base_job(s, true); job_world(j, s, v, false);
        j.terrain = true; j.terrain_blend = blend; j.terrain_stage = &stage; j.terrain_atlas = &atlas; j.terrain_gen = gen;
        const float campos[3] = {2000.0f, alt, -3000.0f};
        T::CamFrame cf{};
        T::camera_frame(campos, deg2rad(pitch_deg), deg2rad(yaw_deg), 0.0f, conv, cf);
        for (int e = 0; e < 2; ++e) {
            T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
        }
        return j;
    };
    auto check_case = [&](const char* name, const BlitJob& j, const T::StageData& stage, const T::Atlas& atlas, float tol, double allow_frac, bool expect_uploaded, bool expect_used) {
        Result r = run_job(ch, j, 3u);
        CHECK(r.ok && r.facts.ok, "%s: job did not run", name);
        if (!r.ok) return r;
        CHECK(r.facts.terrain_used == expect_used && r.facts.terrain_uploaded == expect_uploaded, "%s: terrain_used=%d uploaded=%d", name, int(r.facts.terrain_used), int(r.facts.terrain_uploaded));
        CHECK(r.facts.sources_restored && r.facts.swapchain_untouched && r.facts.barriers_out == r.facts.barriers_back, "%s: barrier contract", name);
        for (int e = 0; e < 2; ++e) {
            const Img ref = ref_terrain(stage, atlas, j, e, s);
            const Cmp m = compare(r.px[e], ref, tol);
            INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu", name, e, m.max_err, m.bad, m.n);
            CHECK(double(m.bad) <= allow_frac * double(m.n), "%s eye%d: %zu pixels differ from the CPU trace by > %.3f (first at %d,%d)", name, e, m.bad, tol, m.fx, m.fy);
            if (e == 0) save_sheet(name, r.px[0], ref, docs);
        }
        return r;
    };
    // 1) looking down over the rolling stage: first use uploads the textures; the pass matches trace() per pixel
    Result r1 = check_case("t_terrain_down", make_job(st, at, 1, -30.0f, -12.0f, 6500.0f, false), st, at, 0.05f, 0.01, true, true);
    // the image really is terrain (not the world checker): the lower half is lit ground, the top is sky-ish blue
    if (r1.ok) {
        float top[4], bot[4]; px_lin(r1.px[0], W / 2, 4, top); px_lin(r1.px[0], W / 2, H - 4, bot);
        CHECK(top[3] > 0.99f && bot[3] > 0.99f && (top[2] != bot[2] || top[1] != bot[1]), "terrain: opaque and not a flat colour");
    }
    // 2) second use of the same generation: no re-upload, same result path; a different camera changes the picture
    check_case("t_terrain_turn", make_job(st, at, 1, 60.0f, -5.0f, 5200.0f, false), st, at, 0.05f, 0.01, false, true);
    // 3) sea plane (same stage with a sea level above part of the ground) needs a new generation: re-upload
    T::StageData sea = st; sea.flags = T::kFlagSeaPresent | T::kFlagSeaGuess; sea.sea_level = 14.0f * sea.hstep;
    check_case("t_terrain_sea", make_job(sea, at, 2, -30.0f, -12.0f, 6500.0f, false), sea, at, 0.05f, 0.01, true, true);
    // 4) A/B: the game's world window over the terrain (xr_terrain_blend)
    check_case("t_terrain_blend", make_job(sea, at, 2, -30.0f, -12.0f, 6500.0f, true), sea, at, 0.05f, 0.01, false, true);
    // 4b) D-344 C1: the same sea masked by tile-word bit 13 (sea tiles on a checkerboard of 8x8-tile blocks): t_bits bit 13, water only on the flagged tiles,
    //     the shader matches trace() and the picture differs from the unmasked sea (runs after 4) so the gen-2 upload expectations above are untouched)
    {
        T::StageData seam = sea;
        for (uint32_t j = 0; j < T::kTGrid; ++j) for (uint32_t i = 0; i < T::kTGrid; ++i) if (((i / 8u) + (j / 8u)) & 1u) seam.words[size_t(j) * T::kTGrid + i] |= T::kTileSeaBit;
        T::apply_sea_convention(seam, conv);
        CHECK(seam.sea_masked, "t_terrain_sea_masked: the stage carries the sea tile mask");
        Result rm = check_case("t_terrain_sea_masked", make_job(seam, at, 5, -30.0f, -12.0f, 6500.0f, false), seam, at, 0.05f, 0.01, true, true);
        Result ru = run_job(ch, make_job(sea, at, 2, -30.0f, -12.0f, 6500.0f, false), 3u);
        CHECK(rm.ok && ru.ok && !(rm.px[0] == ru.px[0]), "t_terrain_sea_masked: the mask changes the picture");
        // T4-SEA: the same GUESSED sea without any bit-13 tile (the old m03k bake) loses its plane at load: the shader draws the dry stage byte for byte
        T::StageData seag = sea; T::apply_sea_convention(seag, conv);
        CHECK(!seag.sea_present(), "t_terrain_sea_guess: a guessed plane without sea tiles is removed");
        Result rg = run_job(ch, make_job(seag, at, 6, -30.0f, -12.0f, 6500.0f, false), 3u);
        Result rd = run_job(ch, make_job(st, at, 7, -30.0f, -12.0f, 6500.0f, false), 3u);
        CHECK(rg.ok && rd.ok && rg.px[0] == rd.px[0] && rg.px[1] == rd.px[1] && !(rg.px[0] == ru.px[0]), "t_terrain_sea_guess: byte-identical to the dry stage, differs from the false sea");
    }
    // 5) a stage the backend refuses (atlas/tile count mismatch): terrain_used=false and the picture is the plain world layer (identical to the key-off job)
    {
        T::Atlas badAt = at; badAt.ntiles = 3;
        BlitJob jb = make_job(st, badAt, 3, -30.0f, -12.0f, 6500.0f, false);
        BlitJob joff = base_job(s, true); job_world(joff, s, v, false);
        Result rb = run_job(ch, jb, 3u), ro = run_job(ch, joff, 3u);
        CHECK(rb.ok && ro.ok && !rb.facts.terrain_used, "refused terrain: used=false");
        if (rb.ok && ro.ok) CHECK(rb.px[0] == ro.px[0] && rb.px[1] == ro.px[1], "refused terrain: the eye images are byte-identical to the key-off job");
        // and a dead generation is not retried (still refused, still identical)
        Result rb2 = run_job(ch, jb, 3u);
        CHECK(rb2.ok && !rb2.facts.terrain_used, "dead generation stays refused");
    }
    // 6) key off after terrain use: byte-identical to a job that never had the terrain fields
    {
        BlitJob a = base_job(s, true); job_world(a, s, v, false);
        BlitJob b = a; b.terrain = false; b.terrain_stage = &st; b.terrain_atlas = &at; b.terrain_gen = 1;   // fields set, switch off
        Result ra = run_job(ch, a, 3u), rb = run_job(ch, b, 3u);
        CHECK(ra.ok && rb.ok && !ra.facts.terrain_used && !rb.facts.terrain_used && ra.px[0] == rb.px[0] && ra.px[1] == rb.px[1], "terrain switch off: images byte-identical");
    }
    // 7) determinism: the same job twice gives identical bytes
    {
        BlitJob j = make_job(sea, at, 2, 10.0f, -9.0f, 6000.0f, false);
        Result a = run_job(ch, j, 3u), b = run_job(ch, j, 3u);
        CHECK(a.ok && b.ok && a.px[0] == b.px[0] && a.px[1] == b.px[1], "terrain: GPU output deterministic");
    }
}

// ----------------------------------------------------------------- DTEX: the shader's detail grain (atlas detail slices, t_bits bit 14) vs trace() with the same atlas
static void case_terrain_detail(Chains& ch, const Scene& s, bool docs) {
    std::printf("[t] terrain detail texture (dtex: shader vs CPU detail_term)\n");
    namespace T = acz::terrain;
    T::StageData st = T::make_synthetic_stage(3, false);
    T::Atlas plain; T::build_atlas(st, plain);
    T::Atlas det = plain;
    CHECK(T::build_detail(T::make_synthetic_detail(5), det) && det.has_detail(), "detail: synthetic detail encodes");
    const float origin[3] = {0, 0, 0};
    const ViewSetup v = make_view("terrain-detail", 20.0f, -8.0f, origin, 0.1f, 0);
    T::Conventions conv;
    auto make_job = [&](const T::Atlas& atlas, uint64_t gen, float pitch_deg, float above) {
        BlitJob j = base_job(s, true); job_world(j, s, v, false);
        j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &atlas; j.terrain_gen = gen;
        const float campos[3] = {2000.0f, T::height_bilinear(st, 2000.0f, -3000.0f) + above, -3000.0f};
        T::CamFrame cf{};
        T::camera_frame(campos, deg2rad(pitch_deg), deg2rad(25.0f), 0.0f, conv, cf);
        for (int e = 0; e < 2; ++e) {
            T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
        }
        return j;
    };
    // 1) near ground (eye 60 units up, 50 deg down: every hit inside the fade, lod 0..1) and 2) a shallower view across the fade and beyond it
    const struct { const char* name; float pitch, above; } cases[] = {{"t_detail_near", -50.0f, 60.0f}, {"t_detail_fade", -12.0f, 300.0f}};
    uint64_t gen = 600;
    for (const auto& c : cases) {
        const BlitJob j = make_job(det, gen++, c.pitch, c.above);
        Result r = run_job(ch, j, 3u);
        CHECK(r.ok && r.facts.ok && r.facts.terrain_used && r.facts.terrain_uploaded, "%s: ran with the terrain and uploaded the detail atlas", c.name);
        if (!r.ok) continue;
        for (int e = 0; e < 2; ++e) {
            const Img ref = ref_terrain(st, det, j, e, s);
            const Cmp m = compare(r.px[e], ref, 0.05f);
            INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu", c.name, e, m.max_err, m.bad, m.n);
            CHECK(double(m.bad) <= 0.01 * double(m.n), "%s eye%d: %zu pixels differ from the CPU trace by > 0.05 (first at %d,%d)", c.name, e, m.bad, m.fx, m.fy);
            if (e == 0) save_sheet(c.name, r.px[0], ref, docs);
        }
        // the grain is really there: the same pose without the detail differs, by a small zero-mean amount
        Result r0 = run_job(ch, make_job(plain, gen++, c.pitch, c.above), 3u);
        CHECK(r0.ok && r0.facts.terrain_used, "%s: no-detail run", c.name);
        if (r0.ok) {
            double sum = 0, sabs = 0; size_t n = 0;
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                float a[4], b[4]; px_lin(r.px[0], x, y, a); px_lin(r0.px[0], x, y, b);
                sum += a[1] - b[1]; sabs += std::fabs(a[1] - b[1]); ++n;
            }
            INFO("%-28s detail vs none: mean |dG| %.4f, mean dG %.4f (linear)", c.name, sabs / n, sum / n);
            // (the near view covers ~1.5 detail repeats, so its local mean is not the texture's zero mean; terrain_offline_tests checks the mean over many repeats)
            CHECK(sabs / n > (c.pitch < -30.0f ? 0.002 : 0.0002) && std::fabs(sum / n) < 0.75 * sabs / n, "%s: the detail changes the picture by a grain", c.name);
        }
    }
    // 3) a detail-less atlas keeps bit 14 clear: byte-identical to the plain terrain job of case_terrain's kind (same gen re-run = no re-upload, same bytes)
    {
        const BlitJob j = make_job(plain, 650, -50.0f, 60.0f);
        Result a = run_job(ch, j, 3u), b = run_job(ch, j, 3u);
        CHECK(a.ok && b.ok && a.px[0] == b.px[0] && a.px[1] == b.px[1], "detail off: deterministic, unchanged path");
        const Img ref = ref_terrain(st, plain, j, 0, s);
        const Cmp m = compare(a.px[0], ref, 0.05f);
        CHECK(double(m.bad) <= 0.01 * double(m.n), "detail off: matches trace() without detail");
    }
}

// ----------------------------------------------------------------- WORLD-2: the shader's environment features (sky/light/cloud/water, t10 + t_bits 3..6) vs trace_env
static void case_terrain_env(Chains& ch, const Scene& s, bool docs) {
    std::printf("[t] terrain environment (sky/light/cloud/water: shader vs CPU trace_env)\n");
    namespace T = acz::terrain;
    T::StageData st = T::make_synthetic_stage(3, false); T::add_synthetic_env(st, 9000.0f);
    st.flags |= T::kFlagSeaPresent | T::kFlagSeaGuess; st.sea_level = 14.0f * st.hstep;   // sea over part of the hills: water shading visible
    T::Atlas at; T::build_atlas(st, at);
    std::vector<float> env; T::build_env_texture(st, T::Conventions{}, env);
    const float origin[3] = {0, 0, 0};
    T::Conventions conv;
    auto make_job = [&](const ViewSetup& v, uint64_t gen, float yaw_deg, float pitch_deg, float alt, uint32_t bits) {
        BlitJob j = base_job(s, true); job_world(j, s, v, false);
        j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at; j.terrain_gen = gen;
        j.terrain_env = &env; j.terrain_env_bits = bits;
        const float campos[3] = {8192.0f, alt, 2000.0f};
        T::CamFrame cf{};
        T::camera_frame(campos, deg2rad(pitch_deg), deg2rad(yaw_deg), 0.0f, conv, cf);
        for (int e = 0; e < 2; ++e) {
            T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
        }
        return j;
    };
    auto ref = [&](const BlitJob& j, int eye, uint32_t bits) {
        const Cfg c = cfg_for(j, eye);
        Img out(W, H);
        const WorldComposite& wc = j.world_eye[eye];
        const TerrainEye& te = j.terrain_eye[eye];
        const T::EnvView ev{j.terrain_env->data(), bits};
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const float u = (float(x) + 0.5f) / float(W), v = (float(y) + 0.5f) / float(H);
            float d[3], dw[3];
            for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + u * wc.ux[i] + v * wc.vy[i];
            for (int i = 0; i < 3; ++i) dw[i] = te.right[i] * d[0] + te.up[i] * d[1] + te.fwd[i] * (-d[2]);
            const float l = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
            for (int i = 0; i < 3; ++i) dw[i] /= l;
            const T::Trace tr = T::trace_env(st, at, ev, te.eye, dw, j.terrain_haze_l, j.terrain_far);
            float* o = out.at(x, y); o[0] = sat(tr.rgb[0] * c.exposure); o[1] = sat(tr.rgb[1] * c.exposure); o[2] = sat(tr.rgb[2] * c.exposure); o[3] = 1.0f;
        }
        return out;
    };
    const ViewSetup vdown = make_view("terrain-env", 20.0f, -8.0f, origin, 0.1f, 0);
    const ViewSetup vup = make_view("terrain-env-up", 0.0f, 25.0f, origin, 0.1f, 0);
    struct C { const char* name; const ViewSetup* v; float yaw, pitch, alt; uint32_t bits; bool upload; };
    const C cases[] = {
        {"t_env_sky_moon", &vup, 53.0f, 0.0f, 6000.0f, T::kEnvSky, true},             // toward the moon sprite (heading -53; make_job negates)
        {"t_env_sky_sun", &vup, 130.0f, 25.0f, 6000.0f, T::kEnvSky, false},           // toward the sun (light el 50, heading -130)
        {"t_env_light", &vdown, -30.0f, -12.0f, 6500.0f, T::kEnvLight, false},
        {"t_env_water", &vdown, -30.0f, -20.0f, 3000.0f, T::kEnvWater | T::kEnvSky, false},
        {"t_env_clouds_below", &vup, 10.0f, 10.0f, 6000.0f, T::kEnvCloud | T::kEnvSky, false},
        {"t_env_clouds_above", &vdown, 10.0f, -15.0f, 14000.0f, T::kEnvCloud | T::kEnvSky | T::kEnvLight, false},
        {"t_env_all", &vdown, 120.0f, -6.0f, 5000.0f, T::kEnvAll, false},
    };
    for (const C& c : cases) {
        const BlitJob j = make_job(*c.v, 41, -c.yaw, c.pitch, c.alt, c.bits);
        Result r = run_job(ch, j, 3u);
        CHECK(r.ok && r.facts.ok && r.facts.terrain_used, "%s: ran with the terrain", c.name);
        if (!r.ok) continue;
        CHECK(r.facts.terrain_uploaded == c.upload, "%s: upload=%d", c.name, int(r.facts.terrain_uploaded));
        for (int e = 0; e < 2; ++e) {
            const Img rf = ref(j, e, c.bits);
            const Cmp m = compare(r.px[e], rf, 0.05f);
            INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu", c.name, e, m.max_err, m.bad, m.n);
            CHECK(double(m.bad) <= 0.01 * double(m.n), "%s eye%d: %zu pixels differ from trace_env by > 0.05 (first at %d,%d)", c.name, e, m.bad, m.fx, m.fy);
            if (e == 0) save_sheet(c.name, r.px[0], rf, docs);
        }
        // every feature changes the picture against the legacy shading (same camera, bits 0)
        BlitJob j0 = j; j0.terrain_env_bits = 0;
        Result r0 = run_job(ch, j0, 3u);
        CHECK(r0.ok && r0.px[0] != r.px[0], "%s: the feature changes the image", c.name);
    }
    // WORLD-4 look:1 (P[26..29] in the same texture): ground gain, signed-elevation low sky / haze target, reshaped stops, faint moon; shader vs trace_env
    {
        T::Conventions lc; lc.look = true;
        std::vector<float> envl; T::build_env_texture(st, lc, envl);
        const C lcases[] = {
            {"t_look_horizon", &vup, 53.0f, -2.0f, 6000.0f, T::kEnvAll, false},     // the moon, the glow, the below-horizon sky and far ground
            {"t_look_down", &vdown, -30.0f, -12.0f, 6500.0f, T::kEnvAll, false},    // gain + haze toward F
            {"t_look_sky_only", &vdown, 120.0f, -6.0f, 5000.0f, T::kEnvSky, false},
        };
        for (const C& c : lcases) {
            BlitJob j = make_job(*c.v, 44, -c.yaw, c.pitch, c.alt, c.bits);
            j.terrain_env = &envl; j.terrain_haze_l = T::kLookHazeL;
            Result r = run_job(ch, j, 3u);
            CHECK(r.ok && r.facts.ok && r.facts.terrain_used, "%s: ran with the terrain", c.name);
            if (!r.ok) continue;
            for (int e = 0; e < 2; ++e) {
                const Img rf = ref(j, e, c.bits);
                const Cmp m = compare(r.px[e], rf, 0.05f);
                INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu", c.name, e, m.max_err, m.bad, m.n);
                CHECK(double(m.bad) <= 0.01 * double(m.n), "%s eye%d: %zu pixels differ from trace_env by > 0.05 (first at %d,%d)", c.name, e, m.bad, m.fx, m.fy);
                if (e == 0) save_sheet(c.name, r.px[0], rf, docs);
            }
            BlitJob j0 = j; j0.terrain_env = &env; j0.terrain_gen = 45;   // same everything with the WORLD-2 texture: the look changes the picture
            Result r0 = run_job(ch, j0, 3u);
            CHECK(r0.ok && r0.px[0] != r.px[0], "%s: look:1 changes the image", c.name);
        }
    }
    // LENS-FLARE (P[32] in the same texture, lf_over): the head-look flare vs the CPU mirror (lens_flare.h) composited over the trace_env reference;
    // the switch off (P[32] zero) is byte-identical to the WORLD-2 texture; a view away from the sun is unchanged; the sun moves with the HEAD.
    {
        namespace LF = acz::lensflare;
        T::Conventions fc; fc.lens_flare = true; fc.lens_flare_k = 1.0f;
        std::vector<float> envf; T::build_env_texture(st, fc, envf);
        auto ref_flare = [&](const BlitJob& j, int eye, uint32_t bits) {
            Img out = ref(j, eye, bits);
            const WorldComposite& wc = j.world_eye[eye];
            const TerrainEye& te = j.terrain_eye[eye];
            const float* P = j.terrain_env->data();
            const float* lc = &P[size_t(T::kPLight) * 4];
            const LF::Frame fr = LF::make_frame(wc.base, wc.ux, wc.vy, te.right, te.up, te.fwd, lc);
            const float k = P[size_t(T::kPFlare) * 4];
            const float* tint = &P[size_t(T::kPLightCol) * 4];
            float vis = -1.0f;   // evaluated once (uniform per eye), only when needed
            auto visf = [&] {
                if (vis < 0.0f) vis = LF::terrain_vis(te.eye, fr.lw, float(st.hmax_byte) * st.hstep, j.terrain_far,
                                                      [&](float x, float z) { return T::height_effective(st, x, z); });   // bare depth: nothing covers the sun
                return vis;
            };
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const float u = (float(x) + 0.5f) / float(W), v = (float(y) + 0.5f) / float(H);
                float d[3], F[3];
                for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + u * wc.ux[i] + v * wc.vy[i];
                LF::eval(fr, d, tint, k, visf, F);
                float* o = out.at(x, y); float c[3];
                LF::composite(o, F, c);
                o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
            }
            return out;
        };
        // toward the sun (light el 50, heading -130), the sun inside the view
        BlitJob j = make_job(vup, 46, -130.0f, 25.0f, 6000.0f, T::kEnvSky);
        j.terrain_env = &envf;
        Result r = run_job(ch, j, 3u);
        CHECK(r.ok && r.facts.ok && r.facts.terrain_used, "t_env_flare: ran with the terrain");
        if (r.ok) {
            for (int e = 0; e < 2; ++e) {
                const Img rf = ref_flare(j, e, T::kEnvSky);
                const Cmp m = compare(r.px[e], rf, 0.05f);
                INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu", "t_env_flare", e, m.max_err, m.bad, m.n);
                CHECK(double(m.bad) <= 0.01 * double(m.n), "t_env_flare eye%d: %zu pixels differ from the CPU flare by > 0.05 (first at %d,%d)", e, m.bad, m.fx, m.fy);
                if (e == 0) save_sheet("t_env_flare", r.px[0], rf, docs);
            }
            BlitJob j0 = j; j0.terrain_env = &env; j0.terrain_gen = 47;   // the same view with the flare off (P[32] zero)
            Result r0 = run_job(ch, j0, 3u);
            CHECK(r0.ok && r0.px[0] != r.px[0], "t_env_flare: the flare changes the image");
            // head turned (anchor yaw unchanged = same plane heading): the flare moves with the head (images differ from the straight-ahead flare)
            const ViewSetup vturn = make_view("terrain-env-flare-turn", -12.0f, 25.0f, origin, 0.1f, 0);
            BlitJob jt = make_job(vturn, 46, -130.0f, 25.0f, 6000.0f, T::kEnvSky); jt.terrain_env = &envf;
            Result rt = run_job(ch, jt, 3u);
            if (rt.ok) {
                const Img rf = ref_flare(jt, 0, T::kEnvSky);
                const Cmp m = compare(rt.px[0], rf, 0.05f);
                CHECK(double(m.bad) <= 0.01 * double(m.n), "t_env_flare_turn: %zu pixels differ from the CPU flare (first at %d,%d)", m.bad, m.fx, m.fy);
            }
        }
        // looking away from the sun: the flare texture gives the same bytes as the flare-off texture
        BlitJob ja = make_job(vdown, 48, -30.0f, -12.0f, 6500.0f, T::kEnvAll); ja.terrain_env = &envf;
        BlitJob jb = ja; jb.terrain_env = &env; jb.terrain_gen = 49;
        Result ra = run_job(ch, ja, 3u), rb = run_job(ch, jb, 3u);
        CHECK(ra.ok && rb.ok && ra.px[0] == rb.px[0] && ra.px[1] == rb.px[1], "t_env_flare_away: the sun out of view leaves the image byte-identical");
    }
    // WORLD-CLOUDS (P[33..34], wc_accum / wc_over in world_layer_terrain_t2): the world puff field vs the CPU mirror (terrain_stage.cpp world_clouds /
    // world_clouds_apply over the trace_env reference); below the deck looking up, inside the band level, above looking down onto the terrain, a head turn;
    // the field changes the image; wcloud off (P[33] zero) is byte-identical to the WORLD-2 texture.
    {
        T::Conventions wcv; wcv.wcloud = true;
        std::vector<float> envw; T::build_env_texture(st, wcv, envw);
        auto ref_wc = [&](const BlitJob& j, int eye, uint32_t bits) {
            const Cfg c = cfg_for(j, eye);
            Img out(W, H);
            const WorldComposite& wc = j.world_eye[eye];
            const TerrainEye& te = j.terrain_eye[eye];
            const T::EnvView ev{j.terrain_env->data(), bits};
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const float u = (float(x) + 0.5f) / float(W), v = (float(y) + 0.5f) / float(H);
                float d[3], dw[3];
                for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + u * wc.ux[i] + v * wc.vy[i];
                for (int i = 0; i < 3; ++i) dw[i] = te.right[i] * d[0] + te.up[i] * d[1] + te.fwd[i] * (-d[2]);
                const float l = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
                for (int i = 0; i < 3; ++i) dw[i] /= l;
                const T::Trace tr = T::trace_env(st, at, ev, te.eye, dw, j.terrain_haze_l, j.terrain_far);
                T::WcAccum wa; T::world_clouds(ev, te.eye, dw, 0.0f, tr.hit ? tr.t : 3.0e38f, j.terrain_haze_l, j.terrain_far, wa);
                float rgb[3] = {tr.rgb[0], tr.rgb[1], tr.rgb[2]};
                T::world_clouds_apply(wa, rgb);
                float* o = out.at(x, y); o[0] = sat(rgb[0] * c.exposure); o[1] = sat(rgb[1] * c.exposure); o[2] = sat(rgb[2] * c.exposure); o[3] = 1.0f;
            }
            return out;
        };
        const ViewSetup vlevel = make_view("terrain-env-wc-level", 0.0f, 2.0f, origin, 0.1f, 0);
        const ViewSetup vturn = make_view("terrain-env-wc-turn", 70.0f, 15.0f, origin, 0.1f, 0);
        const C wcases[] = {
            {"t_wc_below", &vup, 10.0f, 10.0f, 5000.0f, T::kEnvSky | T::kEnvCloud, false},     // under the deck, looking up: puffs over the sky
            {"t_wc_inside", &vlevel, 40.0f, 0.0f, 9000.0f, T::kEnvSky, false},                 // inside the band, level: puffs all around (near fade)
            {"t_wc_above", &vdown, -20.0f, -15.0f, 13500.0f, T::kEnvAll, false},              // above, looking down: puffs over the terrain, haze
            {"t_wc_turn", &vturn, 10.0f, 5.0f, 6500.0f, T::kEnvSky | T::kEnvLight, false},     // head turned 70 deg off the nose: the same world field
        };
        for (const C& c : wcases) {
            BlitJob j = make_job(*c.v, 50, -c.yaw, c.pitch, c.alt, c.bits);
            j.terrain_env = &envw;
            Result r = run_job(ch, j, 3u);
            CHECK(r.ok && r.facts.ok && r.facts.terrain_used, "%s: ran with the terrain", c.name);
            if (!r.ok) continue;
            for (int e = 0; e < 2; ++e) {
                const Img rf = ref_wc(j, e, c.bits);
                const Cmp m = compare(r.px[e], rf, 0.05f);
                INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu", c.name, e, m.max_err, m.bad, m.n);
                CHECK(double(m.bad) <= 0.01 * double(m.n), "%s eye%d: %zu pixels differ from the CPU world clouds by > 0.05 (first at %d,%d)", c.name, e, m.bad, m.fx, m.fy);
                if (e == 0) save_sheet(c.name, r.px[0], rf, docs);
            }
            BlitJob j0 = j; j0.terrain_env = &env; j0.terrain_gen = 51;   // the same view without the field
            Result r0 = run_job(ch, j0, 3u);
            CHECK(r0.ok && r0.px[0] != r.px[0], "%s: the world clouds change the image", c.name);
        }
        // WCLOUD-FIX2 wcdiag:2: the shader paints the puff coverage magenta (the one-dump live proof that the branch runs); matches the CPU mirror
        {
            T::Conventions tcv = wcv; tcv.wc_tint = true;
            std::vector<float> envt; T::build_env_texture(st, tcv, envt);
            BlitJob j = make_job(vup, 52, -10.0f, 10.0f, 5000.0f, T::kEnvSky | T::kEnvCloud);
            j.terrain_env = &envt;
            Result r = run_job(ch, j, 3u);
            CHECK(r.ok && r.facts.ok && r.facts.terrain_used, "t_wc_tint: ran with the terrain");
            if (r.ok) {
                const Img rf = ref_wc(j, 0, T::kEnvSky | T::kEnvCloud);
                const Cmp m = compare(r.px[0], rf, 0.05f);
                INFO("%-28s eye0 max_err=%.4f bad=%zu/%zu", "t_wc_tint", m.max_err, m.bad, m.n);
                CHECK(double(m.bad) <= 0.01 * double(m.n), "t_wc_tint: %zu pixels differ from the CPU mirror by > 0.05", m.bad);
                size_t mag = 0;
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float p[4]; px_lin(r.px[0], x, y, p); if (p[0] > 0.5f && p[2] > 0.5f && p[1] < 0.2f) ++mag; }
                INFO("t_wc_tint magenta pixels %zu", mag);
                CHECK(mag > 0, "t_wc_tint: the wcdiag:2 tint shows magenta puffs");
                save_sheet("t_wc_tint", r.px[0], rf, docs);
            }
        }
        // off: a texture built with wcloud:0 is the WORLD-2 texture (P[33..34] zero) -> the same bytes as before
        T::Conventions wc0; wc0.wcloud = false;
        std::vector<float> envo; T::build_env_texture(st, wc0, envo);
        CHECK(envo == env, "wcloud:0 builds the WORLD-2 texture unchanged");
    }
    // env bits 0 (or no env texture) = the legacy terrain image byte for byte (same generation, and a generation without the texture)
    {
        BlitJob a = make_job(vdown, 41, 30.0f, -12.0f, 6500.0f, 0);
        BlitJob b = a; b.terrain_env = nullptr; b.terrain_gen = 42;
        Result ra = run_job(ch, a, 3u), rb = run_job(ch, b, 3u);
        CHECK(ra.ok && rb.ok && ra.px[0] == rb.px[0] && ra.px[1] == rb.px[1], "env off: byte-identical to a terrain job without the environment");
        BlitJob c2 = b; c2.terrain_env_bits = T::kEnvAll;   // bits without a texture: ignored (legacy)
        Result rc = run_job(ch, c2, 3u);
        CHECK(rc.ok && rc.px[0] == rb.px[0], "env bits without a texture are ignored");
        BlitJob bad = a; bad.terrain_gen = 43; std::vector<float> shortenv(16, 0.0f); bad.terrain_env = &shortenv; bad.terrain_env_bits = T::kEnvAll;
        Result rbad = run_job(ch, bad, 3u);
        CHECK(rbad.ok && rbad.facts.terrain_used && rbad.px[0] == ra.px[0], "malformed env texture: skipped, legacy shading");
    }
}

// ----------------------------------------------------------------- terrain ORIENTATION: a known world-space marker left / right of the heading must land on the correct
// side of the eye image. The expected side is derived by hand from the stated convention (right-handed, +X east, -Z north, heading h: forward = (-sin h, -cos h); the point
// LEFT of f = (fx, fz) is the quarter turn counter-clockwise seen from above with north up: left = (fz, -fx)), NOT from camera_frame().
static acz::terrain::StageData orient_stage(const float fwd_xz[2], const float eye_xz[2], float ahead, float lateral, int rad = 1, float* centres = nullptr) {
    namespace T = acz::terrain;
    T::StageData st = T::make_synthetic_stage(1, false);
    std::fill(st.heights.begin(), st.heights.end(), uint8_t(5));
    st.hmax_byte = 5;
    std::fill(st.words.begin(), st.words.end(), uint16_t(0));
    for (uint32_t t = 0; t < 4; ++t) std::fill(st.tiles.begin() + t * 4096, st.tiles.begin() + (t + 1) * 4096, uint8_t(t));
    for (int c = 0; c < 4; ++c) { st.clut[c * 4 + 3] = 255; }
    const uint8_t pal[4][3] = {{110, 110, 110}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    for (int c = 0; c < 4; ++c) for (int k = 0; k < 3; ++k) st.clut[c * 4 + k] = pal[c][k];
    const float left[2] = {fwd_xz[1], -fwd_xz[0]};
    for (int side = 0; side < 2; ++side) {   // side 0 = left marker (red), 1 = right marker (green)
        const float sg = side == 0 ? 1.0f : -1.0f;
        const float x = eye_xz[0] + ahead * fwd_xz[0] + sg * lateral * left[0], z = eye_xz[1] + ahead * fwd_xz[1] + sg * lateral * left[1];
        const int tx = int(std::floor((x + st.half) / (2.0f * st.cell))), tz = int(std::floor((z + st.half) / (2.0f * st.cell)));
        for (int dz = -rad; dz <= rad; ++dz) for (int dx = -rad; dx <= rad; ++dx) st.words[size_t(tz + dz) * T::kTGrid + size_t(tx + dx)] = uint16_t(side + 1);
        if (centres) { centres[side * 2] = (float(tx) + 0.5f) * 2.0f * st.cell - st.half; centres[side * 2 + 1] = (float(tz) + 0.5f) * 2.0f * st.cell - st.half; }
    }
    return st;
}
// WORLD-1 step 1: terrain ROLL (and pitch/heading) on the real shader. The expected marker positions are derived BY HAND, not from camera_frame(): the camera
// orientation is the standard Euler composition M = Ry(heading) Rx(pitch) Rz(roll) (column vectors; Ry positive turns -Z toward -X, Rx positive lifts the nose,
// Rz positive rotates the up axis toward -X = a LEFT bank, which is what the E6 roll means: headset test 16, campos - air on 173 samples). Camera coordinates of a
// world direction w are c = M^T w (x right, y up, -z forward); the anchor-frame ray of eye pixel (u, v) is base + u ux + v vy (WorldComposite), solved for c.
static bool hand_project(const WorldComposite& wc, const float c[3], float& u, float& v) {
    // base + u ux + v vy - s c = 0  ->  [ux vy -c] (u v s)^T = -base (Cramer)
    const float* a = wc.ux; const float* b = wc.vy; const float m[3] = {-c[0], -c[1], -c[2]}; const float r[3] = {-wc.base[0], -wc.base[1], -wc.base[2]};
    auto det = [](const float* x, const float* y, const float* z) { return x[0] * (y[1] * z[2] - y[2] * z[1]) - y[0] * (x[1] * z[2] - x[2] * z[1]) + z[0] * (x[1] * y[2] - x[2] * y[1]); };
    const float D = det(a, b, m);
    if (std::fabs(D) < 1e-12f) return false;
    u = det(r, b, m) / D; v = det(a, r, m) / D; const float s = det(a, b, r) / D;
    return s > 0;
}
static void hand_camera(float pitch, float heading, float roll, float M[3][3]) {   // M[row][col], columns = camera right, up, back (+Z) in world space
    const float ch = std::cos(heading), sh = std::sin(heading), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
    const float Ry[3][3] = {{ch, 0, sh}, {0, 1, 0}, {-sh, 0, ch}};
    const float Rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    const float Rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    float T[3][3];
    for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) { T[i][k] = 0; for (int l = 0; l < 3; ++l) T[i][k] += Ry[i][l] * Rx[l][k]; }
    for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) { M[i][k] = 0; for (int l = 0; l < 3; ++l) M[i][k] += T[i][l] * Rz[l][k]; }
}
static void case_terrain_roll(Chains& ch, const Scene& s) {
    std::printf("[t] terrain roll/pitch/heading registration (hand-derived marker positions)\n");
    namespace T = acz::terrain;
    const float origin[3] = {0, 0, 0};
    // game = the WORLD-1 production path: the terrain frame built by camera_frame_axes from the GAME camera axes (here the hand camera's columns, as the
    // producer publishes them) with a head TRANSLATION (pos, metres; 65 units per metre like the replayed objects): the landmark must land where the hand
    // camera, moved by the same head offset, projects it (the replayed objects / the game's markers are drawn with exactly that camera).
    struct Att { float pitch, roll, heading, head_yaw_deg; bool game; float pos[3]; };
    const Att cases[] = {{0.0f, 0.5f, -0.785f, 0, false, {0, 0, 0}}, {0.0f, -0.5f, 0.0f, 0, false, {0, 0, 0}}, {0.2f, 1.0f, 1.2f, 0, false, {0, 0, 0}},
                         {-0.3f, 3.0f, -0.5f, 0, false, {0, 0, 0}}, {0.0f, 0.5f, -0.785f, 25, false, {0, 0, 0}}, {-0.3f, 3.0f, -0.5f, 25, false, {0, 0, 0}},
                         {0.1f, -1.6f, 2.4f, 0, false, {0, 0, 0}},
                         {0.1f, 0.8f, -0.785f, 0, true, {20.0f, -5.0f, 10.0f}}, {-0.2f, -2.0f, 1.0f, 25, true, {-15.0f, 8.0f, 0.0f}}, {0.0f, 0.5f, -0.785f, 0, true, {0, 0, 0}}};
    int n = 0;
    for (int rs = 0; rs < 2; ++rs) for (const Att& a : cases) {
        if (rs && !(a.roll == 0.5f && a.head_yaw_deg == 0 && !a.game)) continue;   // the refuted +1 sense: one case is enough to show the check discriminates
        ++n;
        const ViewSetup v = make_view("roll", a.head_yaw_deg, 0.0f, a.pos, deg2rad(a.head_yaw_deg), 0.0f);
        T::Conventions conv; if (rs) conv.roll_sign = 1.0f;
        conv.head_units_per_metre = 65.0f;
        float M[3][3]; hand_camera(a.pitch, a.heading, a.roll, M);
        const float f[2] = {-std::sin(a.heading), -std::cos(a.heading)};
        const float ahead = 15000.0f, lateral = 3000.0f, ground = 5.0f * 80.0f;
        const float dep = std::min(std::max(-a.pitch, 0.07f), 0.5f);
        const float cam[3] = {3000.0f, ground + ahead * std::tan(dep), -7000.0f};
        const float eye_xz[2] = {cam[0], cam[2]};
        float cen[4];
        T::StageData st = orient_stage(f, eye_xz, ahead, lateral, 0, cen);
        T::Atlas at; T::build_atlas(st, at);
        BlitJob j = base_job(s, true); job_world(j, s, v, false);
        j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at; j.terrain_gen = 20000 + uint64_t(n);
        T::CamFrame cf{};
        if (a.game) {
            const float gr[3] = {M[0][0], M[1][0], M[2][0]}, gu[3] = {M[0][1], M[1][1], M[2][1]}, gfw[3] = {-M[0][2], -M[1][2], -M[2][2]};
            CHECK(T::camera_frame_axes(cam, gr, gu, gfw, conv, cf), "roll: game-camera frame accepted");
            T::CamFrame ef{}; T::camera_frame(cam, a.pitch, a.heading, a.roll, conv, ef);
            CHECK(T::frame_angle_deg(cf, ef) < 0.01f, "the game camera and the E6 Euler frame agree for the same attitude (%.4f deg)", T::frame_angle_deg(cf, ef));
        } else CHECK(T::camera_frame(cam, a.pitch, a.heading, a.roll, conv, cf), "roll: camera frame");
        for (int e = 0; e < 2; ++e) {
            T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
        }
        j.terrain_haze_l = 55000.0f; j.terrain_far = 160000.0f;
        Result r = run_job(ch, j, 3u);
        CHECK(r.ok && r.facts.terrain_used, "roll case %d: ran", n);
        if (!r.ok) continue;
        for (int e = 0; e < 2; ++e) {
            float pu[2], pv[2]; bool inview = true;
            const float* o = j.world_eye[e].origin;   // the eye in the anchor frame (metres): the hand camera moved by the head offset
            float he[3]; for (int k = 0; k < 3; ++k) he[k] = cam[k] + 65.0f * (M[k][0] * o[0] + M[k][1] * o[1] + M[k][2] * o[2]);
            for (int side = 0; side < 2; ++side) {
                const float w[3] = {cen[side * 2] - he[0], ground - he[1], cen[side * 2 + 1] - he[2]};
                float c[3]; for (int k = 0; k < 3; ++k) c[k] = M[0][k] * w[0] + M[1][k] * w[1] + M[2][k] * w[2];
                inview = hand_project(j.world_eye[e], c, pu[side], pv[side]) && inview && pu[side] > 0.1f && pu[side] < 0.9f && pv[side] > 0.1f && pv[side] < 0.9f;
            }
            CHECK(inview, "roll case %d eye%d: the hand-projected markers are inside the image (test geometry)", n, e);
            double sx[2] = {0, 0}, sy[2] = {0, 0}; long cnt[2] = {0, 0};
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const uint8_t* p = &r.px[e][(size_t(y) * W + x) * 4];
                const int side = (p[0] > 150 && p[0] > p[1] + 80 && p[0] > p[2] + 80) ? 0 : (p[1] > 150 && p[1] > p[0] + 80 && p[1] > p[2] + 80) ? 1 : -1;
                if (side < 0) continue;
                sx[side] += (x + 0.5) / W; sy[side] += (y + 0.5) / H; ++cnt[side];
            }
            float mu[2] = {-1, -1}, mv[2] = {-1, -1};
            for (int k = 0; k < 2; ++k) if (cnt[k]) { mu[k] = float(sx[k] / cnt[k]); mv[k] = float(sy[k] / cnt[k]); }
            INFO("%s rsgn=%+d pitch %.2f roll %+.2f heading %+.3f head %2.0f pos %.0f,%.0f,%.0f eye%d: red %ld px at (%.3f,%.3f) hand (%.3f,%.3f); green %ld px at (%.3f,%.3f) hand (%.3f,%.3f)",
                 a.game ? "game" : "e6  ", rs ? 1 : -1, a.pitch, a.roll, a.heading, a.head_yaw_deg, a.pos[0], a.pos[1], a.pos[2], e, cnt[0], mu[0], mv[0], pu[0], pv[0], cnt[1], mu[1], mv[1], pu[1], pv[1]);
            const bool match = cnt[0] > 10 && cnt[1] > 10 && std::fabs(mu[0] - pu[0]) < 0.025f && std::fabs(mv[0] - pv[0]) < 0.025f && std::fabs(mu[1] - pu[1]) < 0.025f && std::fabs(mv[1] - pv[1]) < 0.025f;
            if (!rs) {
                CHECK(match, "roll case %d eye%d: the red/green markers sit where the hand-derived camera puts them", n, e);
                if (a.roll == 0.5f) CHECK(mv[0] < mv[1] - 0.02f, "positive E6 roll (left bank): the LEFT (red) marker is ABOVE the right (green) one");
                if (a.roll == -0.5f) CHECK(mv[0] > mv[1] + 0.02f, "negative E6 roll (right bank): the left (red) marker is BELOW the right one");
                if (a.roll == 3.0f) CHECK(mu[0] > 0.5f && mu[1] < 0.5f, "inverted (roll 3.0): the left marker appears in the RIGHT half");
            } else {
                CHECK(!match && mv[0] > mv[1], "rsgn:+1 (the test-16 convention) puts the markers elsewhere: the check discriminates");
            }
        }
    }
}
static void case_terrain_orientation(Chains& ch, const Scene& s) {
    std::printf("[t] terrain orientation (known landmarks left / right of the heading)\n");
    namespace T = acz::terrain;
    const float origin[3] = {0, 0, 0};
    // head yaw / anchor yaw pairs that cancel (the cockpit frame stays in front): the picture must not depend on them
    const float head_anchor[3][2] = {{0.0f, 0.0f}, {25.0f, 25.0f}, {-40.0f, -40.0f}};
    const float headings_deg[] = {0.0f, -45.0f, 90.0f, 170.0f};
    for (int ha = 0; ha < 3; ++ha) for (int mir = 0; mir < 2; ++mir) for (const float hd : headings_deg) {
        if (mir && ha) continue;   // the mirror override is checked with the identity head only
        const ViewSetup v = make_view("orient", head_anchor[ha][0], 0.0f, origin, deg2rad(head_anchor[ha][1]), 0.0f);
        T::Conventions conv;   // DEFAULTS: whatever ships must be correct without options
        conv.mirror_x = mir != 0; // mir=1: the override must swap the sides (it stays available for a calibration run)
        const float h = deg2rad(hd);
        const float f[2] = {-std::sin(h), -std::cos(h)};
        const float cam[3] = {3000.0f, 2500.0f, -7000.0f};
        const float eye_xz[2] = {cam[0], cam[2]};
        T::StageData st = orient_stage(f, eye_xz, 8000.0f, 5000.0f);
        T::Atlas at; T::build_atlas(st, at);
        BlitJob j = base_job(s, true); job_world(j, s, v, false);
        j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at; j.terrain_gen = 1000 + uint64_t(ha) * 1000 + uint64_t(mir) * 5000 + uint64_t(std::lround(hd + 360));
        T::CamFrame cf{};
        CHECK(T::camera_frame(cam, deg2rad(-14.0f), h, 0.0f, conv, cf), "orientation: camera frame");
        for (int e = 0; e < 2; ++e) {
            T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
        }
        j.terrain_haze_l = 55000.0f; j.terrain_far = 160000.0f;
        Result r = run_job(ch, j, 3u);
        CHECK(r.ok && r.facts.terrain_used, "orientation heading %.0f: ran", hd);
        if (!r.ok) continue;
        long redL = 0, redR = 0, grnL = 0, grnR = 0;
        for (int e = 0; e < 2; ++e) for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const uint8_t* p = &r.px[e][(size_t(y) * W + x) * 4];
            const bool red = p[0] > 150 && p[1] < 80 && p[2] < 80, grn = p[1] > 150 && p[0] < 80 && p[2] < 80;
            if (x < W / 2) { redL += red; grnL += grn; } else { redR += red; grnR += grn; }
        }
        INFO("mirror=%d head/anchor %.0f heading %6.1f deg: red(left marker) L=%ld R=%ld ; green(right marker) L=%ld R=%ld", mir, head_anchor[ha][0], hd, redL, redR, grnL, grnR);
        if (!mir) {
            CHECK(redL > 100 && redR <= 5, "heading %.0f: the marker LEFT of the heading must be on the LEFT of the eye image (L=%ld R=%ld)", hd, redL, redR);
            CHECK(grnR > 100 && grnL <= 5, "heading %.0f: the marker RIGHT of the heading must be on the RIGHT of the eye image (L=%ld R=%ld)", hd, grnL, grnR);
        } else {
            CHECK(redR > 100 && redL <= 5 && grnL > 100 && grnR <= 5, "mirror override swaps the sides (heading %.0f: red L=%ld R=%ld, green L=%ld R=%ld)", hd, redL, redR, grnL, grnR);
        }
    }
}

// WORLD-1 step 4: a replayed world object (eye pixels with a depth in the xr_world_objects_depth_fix band) is hidden where the terrain along the pixel's ray is
// nearer than the object, kept where the object is nearer, and untouched with the switch off. Object depth = 2^-10 (1/w - 255 2^-24), w = view depth.
static void case_terrain_occlusion(Chains& ch, const Scene& s) {
    std::printf("[t] terrain occludes replayed world objects (depth-fix band)\n");
    namespace T = acz::terrain;
    T::StageData st = T::make_synthetic_stage(1, false);
    std::fill(st.heights.begin(), st.heights.end(), uint8_t(5)); st.hmax_byte = 5;   // flat ground at 400 units
    T::Atlas at; T::build_atlas(st, at);
    const float origin[3] = {0, 0, 0};
    const ViewSetup v = make_view("occl", 0.0f, 0.0f, origin, 0.0f, 0.0f);
    const float cam[3] = {3000.0f, 2400.0f, -7000.0f};
    T::Conventions conv; T::CamFrame cf{}; T::camera_frame(cam, -0.3f, 0.0f, 0.0f, conv, cf);   // 17 deg nose down: the ground is 5600..8600 units along the patch rays
    const int x0 = 150, y0 = 80, x1 = 170, y1 = 100;
    auto band = [](double w) { return float((1.0 / w - 255.0 / 16777216.0) / 1024.0); };
    auto job_for = [&](Scene& o, float wobj, bool occl, uint64_t gen) {
        for (int e = 0; e < 2; ++e) {
            o.eye[e] = Img(W, H); fill(o.eye[e], 0, 0, W, H, 0, 0, 0, 1.0f); fill(o.eye[e], x0, y0, x1, y1, 1.0f, 0.0f, 1.0f, 1.0f); quantise(o.eye[e]);
            o.depth[e] = Depth{}; if (wobj > 0) fill_depth(o.depth[e], x0, y0, x1, y1, band(wobj));
            o.eye_t[e] = make_color(o.eye[e]); o.depth_t[e] = make_depth(o.depth[e]);
        }
        BlitJob j = base_job(o, false); job_world(j, o, v, false);
        j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at; j.terrain_gen = gen; j.terrain_occlude = occl;
        for (int e = 0; e < 2; ++e) {
            T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
        }
        return j;
    };
    CHECK(band(3000.0) > 0.0f && band(3000.0) < 0.0009765625f && band(20000.0) > 0.0f, "object depths are inside the depth-fix band");
    Scene sn = s, sf = s, so = s, sb = s;
    const Result near_r = run_job(ch, job_for(sn, 3000.0f, true, 30001), 3u);    // object 3 km away, ground 5.6-8.6 km: visible
    const Result far_r = run_job(ch, job_for(sf, 20000.0f, true, 30001), 3u);    // object 20 km away, behind the ground: hidden
    const Result off_r = run_job(ch, job_for(so, 20000.0f, false, 30001), 3u);   // switch off: the object stays (pre-WORLD-1 behaviour)
    const Result bare_r = run_job(ch, job_for(sb, 0.0f, true, 30001), 3u);       // no object at all: the terrain-only reference
    CHECK(near_r.ok && far_r.ok && off_r.ok && bare_r.ok, "occlusion jobs ran");
    if (!(near_r.ok && far_r.ok && off_r.ok && bare_r.ok)) return;
    for (int e = 0; e < 2; ++e) {
        long mag_near = 0, mag_off = 0, mag_far = 0, n = 0; int worst = 0;
        for (int y = y0 + 1; y < y1 - 1; ++y) for (int x = x0 + 1; x < x1 - 1; ++x) {
            const size_t i = (size_t(y) * W + x) * 4;
            auto mag = [&](const std::vector<uint8_t>& p) { return p[i] > 200 && p[i + 1] < 60 && p[i + 2] > 200; };
            mag_near += mag(near_r.px[e]); mag_off += mag(off_r.px[e]); mag_far += mag(far_r.px[e]); ++n;
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(far_r.px[e][i + c]) - int(bare_r.px[e][i + c])));
        }
        INFO("eye%d: magenta near=%ld off=%ld far=%ld of %ld; hidden object vs terrain-only max diff %d/255", e, mag_near, mag_off, mag_far, n, worst);
        CHECK(mag_near == n, "an object NEARER than the terrain stays visible (eye %d)", e);
        CHECK(mag_off == n, "occlusion switch off: the object is drawn over the terrain as before (eye %d)", e);
        CHECK(mag_far == 0 && worst <= 1, "an object BEHIND the terrain is hidden: the pixels are the terrain-only image (eye %d)", e);
        CHECK(far_r.px[e] != off_r.px[e], "the occlusion changed the image (eye %d)", e);
    }
    // WORLD-CLOUDS: a replayed object 3 km away with our puffs IN FRONT of it: the puffs nearer than the object are composited over the object pixel
    // (wc_accum(0, min(r_obj, terrain)) over the replay colour), the ones behind it stay hidden; vs the CPU mirror per pixel. The camera is chosen on the CPU
    // so that a puff covers the patch's centre ray inside 3 km (the field is a hash; nothing is hand-placed).
    {
        std::printf("[t] WORLD-CLOUDS: puffs in front of a replayed object are drawn over it\n");
        T::StageData sw = st; T::add_synthetic_env(sw, 2400.0f);
        T::Atlas aw; T::build_atlas(sw, aw);
        T::Conventions wcv; wcv.wcloud = true; wcv.wc_thick = 0.0f; wcv.wc_size = 4800.0f; wcv.wc_cell = 12000.0f;   // WCLOUD-FIX: hand-built (no parse): cell >= 2.5 x size by hand
        std::vector<float> envw; T::build_env_texture(sw, wcv, envw);
        const T::EnvView ev{envw.data(), T::kEnvSky};
        const float wobj = 3000.0f;
        auto pixel_ray = [&](const WorldComposite& wc, const TerrainEye& te, int x, int y, float dw[3], float& r_obj) {
            const float u = (float(x) + 0.5f) / float(W), vv = (float(y) + 0.5f) / float(H);
            float d[3];
            for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + u * wc.ux[i] + vv * wc.vy[i];
            float fw[3] = {wc.ux[1] * wc.vy[2] - wc.ux[2] * wc.vy[1], wc.ux[2] * wc.vy[0] - wc.ux[0] * wc.vy[2], wc.ux[0] * wc.vy[1] - wc.ux[1] * wc.vy[0]};
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), fl = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
            float cs = (fw[0] * d[0] + fw[1] * d[1] + fw[2] * d[2]) / (dl * fl);
            if (cs < 0) cs = -cs;
            const double dz = double(band(wobj));
            r_obj = float((1.0 / (dz * 1024.0 + 255.0 / 16777216.0)) / std::max(double(cs), 1e-3));
            for (int i = 0; i < 3; ++i) dw[i] = te.right[i] * d[0] + te.up[i] * d[1] + te.fwd[i] * (-d[2]);
            const float l = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
            for (int i = 0; i < 3; ++i) dw[i] /= l;
        };
        // find a camera x where the patch centre ray meets a puff in front of the object
        float camx = 0; bool found = false;
        for (int k = 0; k < 400 && !found; ++k) {
            const float c2[3] = {3000.0f + 1500.0f * float(k), 2400.0f, -7000.0f};
            T::CamFrame f2{}; T::camera_frame(c2, -0.3f, 0.0f, 0.0f, conv, f2);
            WorldComposite wc0; float hl0 = 0, far0 = 0; { Scene tmp = s; BlitJob jj = base_job(tmp, false); job_world(jj, tmp, v, false); wc0 = jj.world_eye[0]; hl0 = jj.terrain_haze_l; far0 = jj.terrain_far; }
            TerrainEye te0{}; T::eye_world(f2, wc0.origin, conv.head_units_per_metre, te0.eye);
            for (int i = 0; i < 3; ++i) { te0.right[i] = f2.right[i]; te0.up[i] = f2.up[i]; te0.fwd[i] = f2.fwd[i]; }
            float dw[3], r; pixel_ray(wc0, te0, (x0 + x1) / 2, (y0 + y1) / 2, dw, r);
            T::WcAccum a; T::world_clouds(ev, te0.eye, dw, 0.0f, r, hl0, far0, a);
            if (a.a > 0.25f) { camx = c2[0]; found = true; }
        }
        CHECK(found, "WORLD-CLOUDS: a camera with a puff in front of the object patch exists");
        if (found) {
            const float c2[3] = {camx, 2400.0f, -7000.0f};
            T::CamFrame f2{}; T::camera_frame(c2, -0.3f, 0.0f, 0.0f, conv, f2);
            Scene sc = s;
            for (int e = 0; e < 2; ++e) {
                sc.eye[e] = Img(W, H); fill(sc.eye[e], 0, 0, W, H, 0, 0, 0, 1.0f); fill(sc.eye[e], x0, y0, x1, y1, 1.0f, 0.0f, 1.0f, 1.0f); quantise(sc.eye[e]);
                sc.depth[e] = Depth{}; fill_depth(sc.depth[e], x0, y0, x1, y1, band(wobj));
                sc.eye_t[e] = make_color(sc.eye[e]); sc.depth_t[e] = make_depth(sc.depth[e]);
            }
            BlitJob j = base_job(sc, false); job_world(j, sc, v, false);
            j.terrain = true; j.terrain_stage = &sw; j.terrain_atlas = &aw; j.terrain_gen = 30077; j.terrain_occlude = true;
            j.terrain_env = &envw; j.terrain_env_bits = T::kEnvSky;
            for (int e = 0; e < 2; ++e) {
                T::eye_world(f2, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
                for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = f2.right[i]; j.terrain_eye[e].up[i] = f2.up[i]; j.terrain_eye[e].fwd[i] = f2.fwd[i]; }
            }
            const Result r = run_job(ch, j, 3u);
            CHECK(r.ok, "WORLD-CLOUDS object job ran");
            if (r.ok) for (int e = 0; e < 2; ++e) {
                int worst = 0; long covered = 0, n = 0;
                for (int y = y0 + 1; y < y1 - 1; ++y) for (int x = x0 + 1; x < x1 - 1; ++x) {
                    float dw[3], ro; pixel_ray(j.world_eye[e], j.terrain_eye[e], x, y, dw, ro);
                    const T::Trace tr = T::trace_env(sw, aw, ev, j.terrain_eye[e].eye, dw, j.terrain_haze_l, j.terrain_far);
                    const float th = tr.hit ? tr.t : 3.0e38f;
                    T::WcAccum a; T::world_clouds(ev, j.terrain_eye[e].eye, dw, 0.0f, std::min(ro, th), j.terrain_haze_l, j.terrain_far, a);
                    float rgb[3] = {1.0f, 0.0f, 1.0f};
                    T::world_clouds_apply(a, rgb);
                    const size_t i = (size_t(y) * W + x) * 4;
                    for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(r.px[e][i + c]) - int(std::lround(T::linear_to_srgb(rgb[c]) * 255.0f))));
                    covered += a.a > 0.05f; ++n;
                }
                INFO("eye%d world clouds over the object: %ld of %ld patch pixels behind a puff, max err %d/255", e, covered, n, worst);
                CHECK(covered > 0, "WORLD-CLOUDS: some object pixels are behind a puff (eye %d)", e);
                CHECK(worst <= 13, "WORLD-CLOUDS: the puff over the object matches the CPU mirror (eye %d, err %d/255)", e, worst);
            }
        }
    }
    // PROPS-SHADING objdom:display: the replayed object band holds DISPLAY values; with the bit the stored sRGB byte equals the display value (decoded once,
    // encoded once by the RTV); without it the byte is srgb_encode(value) (the old double encode). Every non-object pixel is byte-identical either way.
    {
        std::printf("[t] PROPS-SHADING objdom:display decodes the replayed object band once\n");
        const float col[3] = {0.20f, 0.50f, 0.05f};
        auto job_col = [&](Scene& o, bool objdom) {
            for (int e = 0; e < 2; ++e) {
                o.eye[e] = Img(W, H); fill(o.eye[e], 0, 0, W, H, 0, 0, 0, 1.0f); fill(o.eye[e], x0, y0, x1, y1, col[0], col[1], col[2], 1.0f); quantise(o.eye[e]);
                o.depth[e] = Depth{}; fill_depth(o.depth[e], x0, y0, x1, y1, band(3000.0));
                o.eye_t[e] = make_color(o.eye[e]); o.depth_t[e] = make_depth(o.depth[e]);
            }
            BlitJob j = base_job(o, false); job_world(j, o, v, false);
            j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at; j.terrain_gen = 30001; j.terrain_occlude = true; j.terrain_obj_display = objdom;
            for (int e = 0; e < 2; ++e) {
                T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
                for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
            }
            return j;
        };
        Scene sa = s, sd = s;
        const Result raw_r = run_job(ch, job_col(sa, false), 3u), dsp_r = run_job(ch, job_col(sd, true), 3u);
        CHECK(raw_r.ok && dsp_r.ok, "objdom jobs ran");
        if (raw_r.ok && dsp_r.ok) for (int e = 0; e < 2; ++e) {
            int worst_dsp = 0, worst_raw = 0; size_t outside_diff = 0;
            auto enc = [](float x) { return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f; };
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const size_t i = (size_t(y) * W + x) * 4;
                const bool inside = x > x0 && x < x1 - 1 && y > y0 && y < y1 - 1, edge = x >= x0 - 1 && x <= x1 && y >= y0 - 1 && y <= y1;
                if (inside) for (int c = 0; c < 3; ++c) {
                    worst_dsp = std::max(worst_dsp, std::abs(int(dsp_r.px[e][i + c]) - int(std::lround(col[c] * 255.0f))));
                    worst_raw = std::max(worst_raw, std::abs(int(raw_r.px[e][i + c]) - int(std::lround(enc(col[c]) * 255.0f))));
                } else if (!edge) for (int c = 0; c < 4; ++c) outside_diff += dsp_r.px[e][i + c] != raw_r.px[e][i + c];
            }
            INFO("eye%d objdom: display max err %d/255 (expect the display value), raw max err %d/255 (expect srgb_encode), %zu bytes differ outside", e, worst_dsp, worst_raw, outside_diff);
            CHECK(worst_dsp <= 2, "objdom:display: object byte = display value (eye %d, err %d)", e, worst_dsp);
            CHECK(worst_raw <= 2, "objdom:raw: object byte = srgb_encode(value), the old double encode (eye %d, err %d)", e, worst_raw);
            CHECK(outside_diff == 0, "objdom: non-object pixels byte-identical (eye %d)", e);
        }
    }
}

// OBJECT-INTERP (interp:1): an interpolated replayed object under camera motion stays registered with the terrain. The PRODUCER path (FramePlanner: the
// tracked object moved to t_display, the camera dead-reckoned to the same t_display, the rigid delta folded into the object's model matrix) is run on a
// moving + turning camera and a moving ground vehicle; the object is then "drawn" the way the replay draws it -- through the FRAME camera of the eye (hand
// projection, head offset at 65 u/m), painted as a magenta object-band square -- and the terrain is rendered by the real shader with the CONSUMER camera
// (the frame camera moved by the published delta, Output::interp_camera). A red landmark tile is centred on the vehicle's display-time ground position:
// red centroid and magenta object must coincide in both eyes; the terrain left at the frame camera (no delta) must NOT (the check discriminates).
static void case_object_interp(Chains& ch, const Scene& s) {
    std::printf("[t] OBJECT-INTERP: interpolated object registered with the terrain under camera motion\n");
    namespace T = acz::terrain;
    namespace I = acz::interp;
    T::Conventions conv; conv.head_units_per_metre = 65.0f;
    const float ground = 5.0f * 80.0f;
    // the camera: 150 units/frame along a heading turning 3 deg/frame (4.5 deg extrapolated, inside the 6 deg clamp), 2,000 units up, 8 deg nose down; the vehicle: 100 units/frame along +x
    const float pitch = -0.14f, h0 = 0.35f, dh = 3.0f * 3.14159265f / 180.0f, speed = 150.0f;
    const float veh_v[3] = {100.0f, 0.0f, 0.0f};
    auto cam_at = [&](int n, float M[3][3], float eye[3]) {
        hand_camera(pitch, h0 + dh * float(n), 0.0f, M);
        float x = 3000.0f, z = -7000.0f;
        for (int i = 0; i < n; ++i) { const float hh = h0 + dh * float(i + 1); x += -std::sin(hh) * speed; z += -std::cos(hh) * speed; }
        eye[0] = x; eye[1] = 2000.0f; eye[2] = z;
    };
    const int N = 24;   // >= 250 ms of frames: the game frame rate is measured, the camera and the vehicle have two consistent steps
    float Mf[3][3], Cf[3];
    cam_at(N - 1, Mf, Cf);
    // the landmark tile (one 2,048-unit tile): ~15,000 units ahead of the last frame camera; the vehicle's display-time position is chosen ON the tile centre (k = 1.5 exactly:
    // horizon 40 ms clamped to max_extrap_frames = 1.5 at 60 fps)
    T::StageData st = T::make_synthetic_stage(1, false);
    std::fill(st.heights.begin(), st.heights.end(), uint8_t(5)); st.hmax_byte = 5;
    std::fill(st.words.begin(), st.words.end(), uint16_t(0));
    for (uint32_t t = 0; t < 4; ++t) std::fill(st.tiles.begin() + t * 4096, st.tiles.begin() + (t + 1) * 4096, uint8_t(t));
    const uint8_t pal[4][3] = {{110, 110, 110}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    for (int c = 0; c < 4; ++c) { for (int k = 0; k < 3; ++k) st.clut[c * 4 + k] = pal[c][k]; st.clut[c * 4 + 3] = 255; }
    const float fwd_xz[2] = {-Mf[0][2], -Mf[2][2]};
    const float lx = Cf[0] + 15000.0f * fwd_xz[0] + 1500.0f * fwd_xz[1], lz = Cf[2] + 15000.0f * fwd_xz[1] - 1500.0f * fwd_xz[0];
    const int tx = int(std::floor((lx + st.half) / (2.0f * st.cell))), tz = int(std::floor((lz + st.half) / (2.0f * st.cell)));
    st.words[size_t(tz) * T::kTGrid + size_t(tx)] = 1;
    const double centre[3] = {(double(tx) + 0.5) * 2.0 * st.cell - st.half, double(ground), (double(tz) + 0.5) * 2.0 * st.cell - st.half};
    T::Atlas at; T::build_atlas(st, at);
    // the producer: N frames of camera + vehicle through FramePlanner
    I::FramePlanner planner; I::Config oc; acz::pacing::ExtrapConfig cc; planner.configure(oc, cc);
    I::FramePlan plan; I::Xform veh_out; double veh_disp[3] = {0, 0, 0}; I::Result vr;
    for (int n = 0; n < N; ++n) {
        float M[3][3], C[3]; cam_at(n, M, C);
        acz::pacing::Frame fr;
        for (int k = 0; k < 3; ++k) { fr.eye[k] = C[k]; fr.right[k] = M[k][0]; fr.up[k] = M[k][1]; fr.fwd[k] = -M[k][2]; }
        plan = planner.begin(uint64_t(n + 1), int64_t(n) * 16667, fr, n < 3 ? -1.0 : 40.0);
        I::Xform veh;   // identity rows (a level vehicle), translation along +x, on the ground
        const double back = 1.5 + double(N - 1 - n);
        for (int k = 0; k < 3; ++k) veh.t[k] = centre[k] - double(veh_v[k]) * back;
        planner.object(I::DrawKey{0x7e41u, 0xa000, 0xb000}, veh, veh_out, veh_disp, vr);
    }
    CHECK(plan.valid && std::strcmp(plan.why, "ok") == 0 && plan.cam_translated && plan.cam_rotated && std::fabs(plan.k - 1.5) < 1e-6,
          "interp: the planner moved camera and objects 1.5 frames ahead (k %.6f, why %s)", plan.k, plan.why);
    CHECK(vr.moved && vr.translated && std::fabs(veh_disp[0] - centre[0]) < 1e-3 && std::fabs(veh_disp[2] - centre[2]) < 1e-3,
          "interp: the vehicle's display-time position is the landmark centre (%.3f,%.3f vs %.3f,%.3f)", veh_disp[0], veh_disp[2], centre[0], centre[2]);
    // the published delta (StereoFrame::interp) and the consumer's terrain camera
    I::Facts fi; fi.valid = true; fi.cam_moved = !plan.delta.identity;
    for (int k = 0; k < 3; ++k) fi.dt[k] = float(plan.delta.dt[k]);
    for (int k = 0; k < 9; ++k) fi.dR[k] = float(plan.delta.dR[k]);
    T::CamFrame cf_frame{};
    { const float gr[3] = {Mf[0][0], Mf[1][0], Mf[2][0]}, gu[3] = {Mf[0][1], Mf[1][1], Mf[2][1]}, gfw[3] = {-Mf[0][2], -Mf[1][2], -Mf[2][2]};
      CHECK(T::camera_frame_axes(Cf, gr, gu, gfw, conv, cf_frame), "interp: frame camera accepted"); }
    T::CamFrame cf_disp = cf_frame;
    I::apply_to_camera(fi.dt, fi.dR, cf_disp.eye, cf_disp.right, cf_disp.up, cf_disp.fwd);
    const float shift = std::sqrt(fi.dt[0] * fi.dt[0] + fi.dt[1] * fi.dt[1] + fi.dt[2] * fi.dt[2]);
    INFO("camera delta %.1f units, %.2f deg; vehicle display (%.1f, %.1f, %.1f)", shift, T::frame_angle_deg(cf_frame, cf_disp), veh_disp[0], veh_disp[1], veh_disp[2]);
    const float origin[3] = {0, 0, 0};
    const ViewSetup v = make_view("interp", 0.0f, 0.0f, origin, 0.0f, 0.0f);
    const int half = 4;
    auto band = [](double w) { return float((1.0 / w - 255.0 / 16777216.0) / 1024.0); };
    struct Obj { float u = -1, v = -1, w = 0; bool ok = false; } obj[2];
    auto job_for = [&](Scene& o, const T::CamFrame& tcam, bool paint, uint64_t gen) {
        BlitJob j0 = base_job(s, false); job_world(j0, s, v, false);   // the eye origins of this view
        for (int e = 0; e < 2; ++e) {
            // the replay draws the object through the FRAME camera of this eye (A_e: frame camera + head offset) with the compensated model matrix
            const float* og = j0.world_eye[e].origin;
            float he[3]; for (int k = 0; k < 3; ++k) he[k] = Cf[k] + 65.0f * (Mf[k][0] * og[0] + Mf[k][1] * og[1] + Mf[k][2] * og[2]);
            const float wv[3] = {float(veh_out.t[0] - he[0]), float(veh_out.t[1] - he[1]), float(veh_out.t[2] - he[2])};
            float c[3]; for (int k = 0; k < 3; ++k) c[k] = Mf[0][k] * wv[0] + Mf[1][k] * wv[1] + Mf[2][k] * wv[2];
            obj[e].ok = hand_project(j0.world_eye[e], c, obj[e].u, obj[e].v); obj[e].w = -c[2];
            o.eye[e] = Img(W, H); fill(o.eye[e], 0, 0, W, H, 0, 0, 0, 1.0f);
            o.depth[e] = Depth{};
            if (paint && obj[e].ok) {
                const int px = int(obj[e].u * W), py = int(obj[e].v * H);
                fill(o.eye[e], px - half, py - half, px + half, py + half, 1.0f, 0.0f, 1.0f, 1.0f);
                fill_depth(o.depth[e], px - half, py - half, px + half, py + half, band(obj[e].w));
            }
            quantise(o.eye[e]);
            o.eye_t[e] = make_color(o.eye[e]); o.depth_t[e] = make_depth(o.depth[e]);
        }
        BlitJob j = base_job(o, false); job_world(j, o, v, false);
        j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at; j.terrain_gen = gen; j.terrain_occlude = false;   // the object is a ground vehicle ON the terrain: the occlusion margin is not what is tested here
        for (int e = 0; e < 2; ++e) {
            T::eye_world(tcam, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = tcam.right[i]; j.terrain_eye[e].up[i] = tcam.up[i]; j.terrain_eye[e].fwd[i] = tcam.fwd[i]; }
        }
        j.terrain_haze_l = 55000.0f; j.terrain_far = 160000.0f;
        return j;
    };
    auto centroid = [&](const std::vector<uint8_t>& px, bool magenta, float& mu, float& mv) {
        double sx = 0, sy = 0; long n = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const uint8_t* p = &px[(size_t(y) * W + x) * 4];
            const bool hit = magenta ? (p[0] > 200 && p[1] < 60 && p[2] > 200) : (p[0] > 150 && p[0] > p[1] + 80 && p[0] > p[2] + 80);
            if (hit) { sx += (x + 0.5) / W; sy += (y + 0.5) / H; ++n; }
        }
        mu = n ? float(sx / n) : -1.0f; mv = n ? float(sy / n) : -1.0f;
        return n;
    };
    Scene s1 = s, s2 = s, s3 = s;
    const Result with_delta = run_job(ch, job_for(s1, cf_disp, false, 40001), 3u);   // consumer: terrain camera + the producer's delta
    const Result no_delta = run_job(ch, job_for(s2, cf_frame, false, 40002), 3u);     // terrain left at the frame camera
    const Result painted = run_job(ch, job_for(s3, cf_disp, true, 40001), 3u);        // the replayed object painted over the terrain
    CHECK(with_delta.ok && no_delta.ok && painted.ok, "interp jobs ran");
    if (!(with_delta.ok && no_delta.ok && painted.ok)) return;
    for (int e = 0; e < 2; ++e) {
        CHECK(obj[e].ok && obj[e].u > 0.1f && obj[e].u < 0.9f && obj[e].v > 0.1f && obj[e].v < 0.9f, "interp eye%d: the object projects inside the image (%.3f,%.3f)", e, obj[e].u, obj[e].v);
        float ru, rv, nu, nv, mu, mv;
        const long rn = centroid(with_delta.px[e], false, ru, rv), nn = centroid(no_delta.px[e], false, nu, nv), mn = centroid(painted.px[e], true, mu, mv);
        INFO("eye%d: object (replay, frame camera) at (%.4f,%.4f) w=%.0f; landmark with delta %ld px at (%.4f,%.4f); without delta %ld px at (%.4f,%.4f); painted object %ld px at (%.4f,%.4f)",
             e, obj[e].u, obj[e].v, obj[e].w, rn, ru, rv, nn, nu, nv, mn, mu, mv);
        CHECK(rn > 10 && std::fabs(ru - obj[e].u) < 0.01f && std::fabs(rv - obj[e].v) < 0.01f,
              "interp eye%d: the terrain landmark (consumer camera + delta) sits on the interpolated object", e);
        CHECK(nn > 10 && (std::fabs(nu - obj[e].u) > 0.02f || std::fabs(nv - obj[e].v) > 0.02f),
              "interp eye%d: without the delta the landmark is elsewhere (the check discriminates: %.4f,%.4f)", e, nu - obj[e].u, nv - obj[e].v);
        CHECK(mn >= (2 * half - 2) * (2 * half - 2) && std::fabs(mu - obj[e].u) < 0.01f && std::fabs(mv - obj[e].v) < 0.01f,
              "interp eye%d: the painted object is composited over the terrain at its projection", e);
    }
}

// ----------------------------------------------------------------- COCKPIT-2: the model cockpit pass (cockpit_model.hlsl) vs the CPU reference rasteriser
// Eye composite with the replayed cockpit band dropped (an object-band rectangle kept), then the synthetic model (asymmetric landmarks, back face, depth pair,
// alpha-tested checker, glass, exterior tail) drawn over it per eye with the submitted view: compared per pixel with acz::cockpit::render_reference.
static void case_cockpit_model(Chains& ch, const Scene& s, bool docs) {
    std::printf("[c] cockpit model (vs_model/ps_model vs CPU reference, replay drop, 6DoF, key-off identity)\n");
    namespace K = acz::cockpit;
    const K::ModelData model = K::make_synthetic_model();
    // cases 1-10: the COCKPIT-2 colour path (raw domain, gain 1.15, no floor) keeps the saturated landmark classes; case 11 runs the COCKPIT-3 defaults
    K::Options opt; opt.display = false; opt.gain = 1.15f; opt.floor = false; opt.interior = false; K::Mesh mesh; std::string why;   // COCKPIT-4 interior off: the COCKPIT-2 scene
    CHECK(K::build_mesh(model, opt, mesh, why), "synthetic mesh: %s", why.c_str());
    const float* light_in = nullptr; const float* tint_in = nullptr;   // case 11: a stage light/tint through make_constants and render_reference
    const K::ReflBasis* refl_in = nullptr; const K::Texture* refl_map_in = nullptr; uint64_t refl_gen_in = 0;   // case 13: SKIN-REFL basis + t5 map
    // depth with a replayed OBJECT (depth-fix band) inside the cockpit panel: it must survive the drop
    Depth dobj[2]; ComPtr<ID3D12Resource> dobj_t[2];
    for (int e = 0; e < 2; ++e) { dobj[e] = s.depth[e]; fill_depth(dobj[e], 230, 150, 250, 170, 0.0005f); dobj_t[e] = make_depth(dobj[e]); }
    auto make_job = [&](const ViewSetup& v, const K::Mesh& m, uint64_t gen, bool keep) {
        BlitJob j = base_job(s); job_world(j, s, v, false);
        for (int e = 0; e < 2; ++e) j.depth[e] = {dobj_t[e].Get(), 39u, W, H};
        j.cockpit = true; j.cockpit_keep_replay = keep; j.cockpit_mesh = &m; j.cockpit_gen = gen;
        for (int e = 0; e < 2; ++e) {
            const float fov[4] = {v.eye[e].fov.left, v.eye[e].fov.right, v.eye[e].fov.up, v.eye[e].fov.down};
            K::EyeCam cam{};
            CHECK(K::eye_camera(v.eye[e].pose.orientation, v.eye[e].pose.position, fov, v.anchor, v.yaw, m.eye, opt.upm, opt.near_u, cam), "eye camera");
            j.cockpit_eye[e] = K::make_constants(cam, opt, j.exposure, uint32_t(j.tonemap), light_in, tint_in, nullptr, refl_in, m.shadow ? m.shadow_box : nullptr);
        }
        if (refl_map_in) { j.cockpit_refl = refl_map_in->rgba.data(); j.cockpit_refl_w = refl_map_in->w; j.cockpit_refl_h = refl_map_in->h; j.cockpit_refl_gen = refl_gen_in; }
        return j;
    };
    auto ref_for = [&](const BlitJob& j, const ViewSetup& v, const K::Mesh& m, int e) {
        Cfg c = cfg_for(j, e); c.model = !j.cockpit_keep_replay;
        Img img = ref_eye(s, c, e, dobj[e]);
        K::Target t; t.w = W; t.h = H; t.rgba = img.p; t.depth.assign(size_t(W) * H, 0.0f);
        const float fov[4] = {v.eye[e].fov.left, v.eye[e].fov.right, v.eye[e].fov.up, v.eye[e].fov.down};
        K::EyeCam cam{}; K::eye_camera(v.eye[e].pose.orientation, v.eye[e].pose.position, fov, v.anchor, v.yaw, m.eye, opt.upm, opt.near_u, cam);
        K::render_reference(m, opt, j.exposure, uint32_t(j.tonemap), cam, t, light_in, tint_in, nullptr, refl_in, refl_map_in);
        img.p = t.rgba;
        return img;
    };
    auto run_case = [&](const char* name, const ViewSetup& v, const K::Mesh& m, uint64_t gen, bool keep, bool expect_upload) {
        const BlitJob j = make_job(v, m, gen, keep);
        Result r = run_job(ch, j, 3u);
        CHECK(r.ok && r.facts.ok, "%s: job did not run", name);
        if (!r.ok) return r;
        CHECK(r.facts.cockpit_used && r.facts.cockpit_uploaded == expect_upload, "%s: cockpit_used=%d uploaded=%d", name, int(r.facts.cockpit_used), int(r.facts.cockpit_uploaded));
        CHECK(r.facts.sources_restored && r.facts.swapchain_untouched && r.facts.barriers_out == r.facts.barriers_back, "%s: barrier contract", name);
        for (int e = 0; e < 2; ++e) {
            const Img ref = ref_for(j, v, m, e);
            const Cmp c = compare(r.px[e], ref, 0.04f);
            INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu", name, e, c.max_err, c.bad, c.n);
            CHECK(double(c.bad) <= 0.01 * double(c.n), "%s eye%d: %zu pixels differ from the CPU reference by > 0.04 (first at %d,%d)", name, e, c.bad, c.fx, c.fy);
            if (e == 0) save_sheet(name, r.px[0], ref, docs);
        }
        return r;
    };
    auto count_class = [](const std::vector<uint8_t>& px, int e, float& cx) {   // yellow (near quad) pixels and their mean column
        (void)e; size_t n = 0; double sx = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float p[4]; px_lin(px, x, y, p); if (p[0] > 0.6f && p[1] > 0.6f && p[2] < 0.18f) { ++n; sx += x; } }
        cx = n ? float(sx / n) : 0.0f; return n;
    };
    const float pos0[3] = {0, 0, 0};
    // 1) forward view: first use uploads; landmarks; the panel's cockpit band is replaced, the object rectangle in it is kept
    const ViewSetup fwd = make_view("cockpit", 0.0f, 0.0f, pos0, 0.0f, 0.0f);
    Result r1 = run_case("c_model_fwd", fwd, mesh, 1, false, true);
    if (r1.ok) {
        float p[4];
        px_lin(r1.px[0], 240, 160, p); CHECK(near4(p, q16(0.8f), q16(0.2f), q16(0.1f), 1.0f, 0.01f), "object band kept: %.3f %.3f %.3f a=%.3f", p[0], p[1], p[2], p[3]);
        px_lin(r1.px[0], 70, 170, p);  CHECK(!(std::fabs(p[0] - 0.8f) < 0.02f && std::fabs(p[1] - 0.2f) < 0.02f), "cockpit band dropped at (70,170): %.3f %.3f %.3f", p[0], p[1], p[2]);
        float c0 = 0, c1 = 0; const size_t n0 = count_class(r1.px[0], 0, c0), n1 = count_class(r1.px[1], 1, c1);
        INFO("yellow quad: eye0 n=%zu x=%.2f eye1 n=%zu x=%.2f (disparity %.2f px)", n0, c0, n1, c1, c0 - c1);
        CHECK(n0 > 50 && n1 > 50 && c0 - c1 > 0.5f, "per-eye parallax: the near quad sits further right in the left eye");
    }
    // 2) same generation, 6DoF head pose (yaw -10, pitch 6, roll 25, translated 5/-3/10 cm): no re-upload
    { const float p[3] = {0.05f, -0.03f, 0.10f}; const ViewSetup v = make_view_roll("cockpit6", -10.0f, 6.0f, 25.0f, p, 0.0f, 0.0f);
      run_case("c_model_6dof", v, mesh, 1, false, false); }
    // 3) seat heading yaw0 = 40 deg with the head turned the same way: identical picture to the forward case (the cockpit is fixed in the seat frame)
    { ViewSetup v = make_view("cockpit_yaw0", 40.0f, 0.0f, pos0, deg2rad(40.0f), 0.0f);
      const BlitJob a = make_job(fwd, mesh, 1, false), b = make_job(v, mesh, 1, false);
      bool same = true; for (int e = 0; e < 2; ++e) for (int i = 0; i < 16; ++i) if (i % 4 != 3) same = same && std::fabs(a.cockpit_eye[e].m[i] - b.cockpit_eye[e].m[i]) < 1e-4f;
      CHECK(same, "seat yaw0 + the same head yaw = the forward model view orientation (the eye offsets are LOCAL-x in this fixture)"); }
    // 4) looking back: the exterior tail (skin alpha ignored: opaque) fills the centre
    { const ViewSetup v = make_view("cockpit_back", 180.0f, 0.0f, pos0, 0.0f, 0.0f);
      Result r = run_case("c_model_back", v, mesh, 1, false, false);
      if (r.ok) { float p[4]; px_lin(r.px[0], W / 2, H / 2, p); CHECK(p[3] > 0.99f, "tail opaque behind the eye a=%.3f", p[3]); } }
    // 5) A/B replay:1: the replayed cockpit stays under the model
    { Result r = run_case("c_model_replay", fwd, mesh, 1, true, false);
      if (r.ok) { float p[4]; px_lin(r.px[0], 70, 170, p); CHECK(near4(p, q16(0.8f), q16(0.2f), q16(0.1f), 1.0f, 0.01f), "replay kept at (70,170)"); } }
    // 6) png_once with the model: the private final image equals the submitted eye 0
    { BlitJob j = make_job(fwd, mesh, 1, false); j.png_capture = true;
      Result r = run_job(ch, j, 3u, true);
      CHECK(r.ok && r.png_taken && r.png.w == uint32_t(W) && r.png.final_rgba.size() == r.px[0].size(), "png capture with the model");
      if (r.ok && r.png_taken) { size_t diff = 0; for (size_t i = 0; i < r.px[0].size(); i += 4) for (int k = 0; k < 3; ++k) diff += r.png.final_rgba[i + k] != r.px[0][i + k];
                                 CHECK(diff == 0, "png final image = the submitted eye 0 (%zu channel differences)", diff); } }
    // 7) determinism
    { const BlitJob j = make_job(fwd, mesh, 1, false); Result a = run_job(ch, j, 3u), b = run_job(ch, j, 3u);
      CHECK(a.ok && b.ok && a.px[0] == b.px[0] && a.px[1] == b.px[1], "cockpit model: GPU output deterministic"); }
    // 8) key off after model use: byte-identical to a job that never had the cockpit fields
    { BlitJob a = base_job(s); job_world(a, s, fwd, false); for (int e = 0; e < 2; ++e) a.depth[e] = {dobj_t[e].Get(), 39u, W, H};
      BlitJob b = make_job(fwd, mesh, 1, false); b.cockpit = false;
      Result ra = run_job(ch, a, 3u), rb = run_job(ch, b, 3u);
      CHECK(ra.ok && rb.ok && !ra.facts.cockpit_used && !rb.facts.cockpit_used && ra.px[0] == rb.px[0] && ra.px[1] == rb.px[1], "cockpit switch off: images byte-identical");
      // 9) a mesh the backend refuses (broken stream): cockpit_used=false, the replayed cockpit stays (byte-identical to key off), the dead generation is not retried
      K::Mesh bad = mesh; bad.v.pop_back();
      const BlitJob jb = make_job(fwd, bad, 7, false);
      Result rbad = run_job(ch, jb, 3u), rbad2 = run_job(ch, jb, 3u);
      CHECK(rbad.ok && !rbad.facts.cockpit_used && rbad2.ok && !rbad2.facts.cockpit_used, "refused mesh: used=false (twice)");
      if (rbad.ok && ra.ok) CHECK(rbad.px[0] == ra.px[0] && rbad.px[1] == ra.px[1], "refused mesh: eye images byte-identical to the key-off job"); }
    // 10) back to the good generation after the refusal: rebuilt and uploaded again
    run_case("c_model_again", fwd, mesh, 1, false, true);
    // 11) COCKPIT-3 defaults: display domain (sRGB EOTF once in ps_model), the stage light + tint in b0 (32 root constants), the floor closure quad;
    //     forward and looking down (the floor covers the view below the panels), a new generation (re-upload of the larger stream)
    { opt = K::Options{}; opt.interior = false; K::Mesh m3; CHECK(K::build_mesh(model, opt, m3, why) && m3.floor, "COCKPIT-3 mesh with the floor: %s", why.c_str());
      static const float kL[3] = {0.64f, 0.766f, -0.056f}, kT[3] = {1.016f, 1.074f, 0.911f};
      light_in = kL; tint_in = kT;
      Result r = run_case("c_model_cockpit3", fwd, m3, 11, false, true);
      if (r.ok) { float p[4]; px_lin(r.px[0], W / 2, H / 2, p); INFO("cockpit3 centre %.3f %.3f %.3f a=%.3f", p[0], p[1], p[2], p[3]); }
      const ViewSetup down = make_view("cockpit_down", 0.0f, -22.0f, pos0, 0.0f, 0.0f);   // the centre ray meets the synthetic floor (y -0.5) at z ~ -45.9
      Result d = run_case("c_model_cockpit3_down", down, m3, 11, false, false);
      if (d.ok) { float p[4]; px_lin(d.px[0], W / 2, H / 2, p);
                  const float fl = K::srgb_to_linear(float(K::kFloorRgba[1]) / 255.0f * 1.074f);   // floor green: display 42/255 x tint x light <= 1
                  CHECK(p[3] > 0.99f && p[1] > 0.2f * fl && p[1] < 1.2f * fl, "floor closure below the panels: %.4f (unlit..lit %.4f)", p[1], fl); }
      light_in = nullptr; tint_in = nullptr; opt.display = false; opt.gain = 1.15f; opt.floor = false; }
    // 12) COCKPIT-4 defaults on the c4 synthetic model: the canopy glass through the REAL ps_glass (dual-source: dst * transmission + Fresnel sky) over the
    //     composite, the procedural interior (tub, seat, consoles, stick, throttle, pedals) and anim:1 (the posed range copied into the VB each recording);
    //     every case vs the CPU reference (render_reference with the same posed mesh). Forward, looking down at the stick, and a posed + 6DoF view.
    { const K::ModelData m4 = K::make_synthetic_model_c4();
      opt = K::Options{}; opt.anim = true; K::Mesh me4; CHECK(K::build_mesh(m4, opt, me4, why) && me4.glass_vertices == 6 && me4.interior && me4.anim_surfaces == 1,
          "COCKPIT-4 mesh: %s glass=%u interior=%d anim=%u", why.c_str(), me4.glass_vertices, int(me4.interior), me4.anim_surfaces);
      auto run4 = [&](const char* name, const ViewSetup& v, const K::AnimInputs* in, uint64_t gen, bool expect_upload) {
          std::vector<K::GpuVertex> posed; K::Mesh ref_mesh = me4;
          BlitJob j = make_job(v, me4, gen, false);
          if (in) { K::pose_anim(me4, *in, posed); K::apply_pose(ref_mesh, posed); j.cockpit_anim = posed.data(); j.cockpit_anim_count = uint32_t(posed.size()); }
          Result r = run_job(ch, j, 3u);
          CHECK(r.ok && r.facts.ok && r.facts.cockpit_used && r.facts.cockpit_uploaded == expect_upload && r.facts.cockpit_anim_uploaded == (in != nullptr),
                "%s: used=%d uploaded=%d anim=%d", name, int(r.facts.cockpit_used), int(r.facts.cockpit_uploaded), int(r.facts.cockpit_anim_uploaded));
          if (!r.ok) return r;
          for (int e = 0; e < 2; ++e) {
              const Img ref = ref_for(j, v, ref_mesh, e);
              const Cmp c = compare(r.px[e], ref, 0.04f);
              INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu", name, e, c.max_err, c.bad, c.n);
              CHECK(double(c.bad) <= 0.01 * double(c.n), "%s eye%d: %zu pixels differ from the CPU reference by > 0.04 (first at %d,%d)", name, e, c.bad, c.fx, c.fy);
              if (e == 0) save_sheet(name, r.px[0], ref, docs);
          }
          return r;
      };
      Result g = run4("c_model_c4_glass", fwd, nullptr, 12, true);
      if (g.ok) {   // the canopy (z -40, x +-2.5) covers the centre: the GPU pixel equals the dual-source rule over the composite below it (tight: 8-bit quantisation)
          BlitJob j = make_job(fwd, me4, 12, false); K::Options og = opt; og.canopy = false;   // the same eye WITHOUT the canopy = the background of the glass
          K::Mesh bare; CHECK(K::build_mesh(m4, og, bare, why), "c4 bare mesh");
          Cfg c = cfg_for(j, 0); c.model = true; Img img = ref_eye(s, c, 0, dobj[0]);
          K::Target t; t.w = W; t.h = H; t.rgba = img.p; t.depth.assign(size_t(W) * H, 0.0f);
          for (uint32_t i = 0; i < bare.blend_vertices; ++i)   // drop the (now straight-alpha) canopy quad (z -40) out of view; the HUD glass stays under it
              if (bare.v[bare.opaque_vertices + i].pos[2] == -40.0f) bare.v[bare.opaque_vertices + i].pos[1] += 1000.0f;
          const float fov[4] = {fwd.eye[0].fov.left, fwd.eye[0].fov.right, fwd.eye[0].fov.up, fwd.eye[0].fov.down};
          K::EyeCam cam{}; K::eye_camera(fwd.eye[0].pose.orientation, fwd.eye[0].pose.position, fov, fwd.anchor, fwd.yaw, me4.eye, opt.upm, opt.near_u, cam);
          K::render_reference(bare, og, j.exposure, uint32_t(j.tonemap), cam, t);
          const K::Constants kk = K::make_constants(cam, opt, j.exposure, uint32_t(j.tonemap));
          // the model-space point of the centre pixel on the canopy plane z = -40
          // every pixel whose view ray meets the canopy quad (z -40, x +-2.5, y 1.5..6) over a lit background: GPU = add + transmission * background
          size_t tested = 0; float worst = 0; float tr0[3] = {0, 0, 0};
          for (int y = 0; y < H; y += 3) for (int x = 0; x < W; x += 3) {
              const float* bg = &t.rgba[(size_t(y) * W + x) * 4];
              if (bg[0] + bg[1] + bg[2] < 0.15f) continue;
              float dir[3];
              { const float nx = (x + 0.5f) / W * 2 - 1, ny = 1 - (y + 0.5f) / H * 2;   // view ray through the pixel (camera basis from eye_camera)
                const float tx = std::tan(fov[0]) + (nx + 1) * 0.5f * (std::tan(fov[1]) - std::tan(fov[0])), ty = std::tan(fov[3]) + (ny + 1) * 0.5f * (std::tan(fov[2]) - std::tan(fov[3]));
                for (int a = 0; a < 3; ++a) dir[a] = cam.right[a] * tx + cam.up[a] * ty - cam.back[a]; }
              const float sdist = (-40.0f - cam.eye[2]) / dir[2];
              const float pm[3] = {cam.eye[0] + dir[0] * sdist, cam.eye[1] + dir[1] * sdist, -40.0f};
              if (!(sdist > 0 && std::fabs(pm[0]) < 2.4f && pm[1] > 1.6f && pm[1] < 5.9f)) continue;
              float add[3], tr[3], ga; const float n[3] = {0, 0, 1};
              K::shade_glass(kk, pm, n, add, tr, ga);
              float pg[4]; px_lin(g.px[0], x, y, pg);
              for (int a = 0; a < 3; ++a) { worst = std::max(worst, std::fabs(pg[a] - (add[a] + tr[a] * bg[a]))); tr0[a] = tr[a]; }
              ++tested;
          }
          INFO("canopy glass over a lit background: %zu pixels, worst |GPU - (add + trans * bg)| = %.4f, trans %.4f %.4f %.4f", tested, worst, tr0[0], tr0[1], tr0[2]);
          CHECK(tested > 100 && worst < 0.012f, "canopy glass: GPU = add + transmission * background (%zu px, worst %.4f)", tested, worst);
          CHECK(tr0[0] > tr0[2] && tr0[0] < 0.92f, "canopy transmission is the measured green-grey tint in linear (r %.4f > b %.4f)", tr0[0], tr0[2]);
      }
      const ViewSetup down = make_view("cockpit_c4_down", 0.0f, -60.0f, pos0, 0.0f, 0.0f);
      Result d = run4("c_model_c4_down", down, nullptr, 12, false);
      if (d.ok) { float p[4]; px_lin(d.px[0], W / 2, H / 2, p); CHECK(p[3] > 0.99f, "looking down: the interior (floor/stick) is opaque a=%.3f", p[3]); }
      const K::AnimInputs in{1.0f, 0.5f, -0.4f, 0.9f};
      run4("c_model_c4_anim", down, &in, 12, false);
      { const float p6[3] = {0.04f, -0.02f, 0.06f}; const ViewSetup v6 = make_view_roll("cockpit_c4_6dof", 12.0f, -35.0f, 15.0f, p6, 0.0f, 0.0f);
        run4("c_model_c4_anim_6dof", v6, &in, 12, false); }
      // a stale/foreign posed range (count mismatch) is ignored: no copy, rest pose
      { BlitJob j = make_job(fwd, me4, 12, false); std::vector<K::GpuVertex> wrong(3); j.cockpit_anim = wrong.data(); j.cockpit_anim_count = 3;
        Result r = run_job(ch, j, 3u); CHECK(r.ok && r.facts.cockpit_used && !r.facts.cockpit_anim_uploaded, "anim count mismatch: not copied"); }
      opt = K::Options{}; opt.display = false; opt.gain = 1.15f; opt.floor = false; opt.interior = false; }
    // 13) SKIN-REFL (srefl:1): the exterior tail (skin, kMatTexAlphaNotCov) looking back, the t5 map (8x8 gradient: u -> red, v -> green) sampled at the
    //     per-vertex world-normal uv with the facing mask, added in the display domain (COCKPIT-3 defaults) - through the REAL vs_model/ps_model vs
    //     render_reference; first use copies the map, the same key does not, a new key copies again; a job without the map = the COCKPIT-3 image.
    { opt = K::Options{}; opt.interior = false; opt.skin_refl = true; opt.skin_refl_k = 1.5f;
      K::Mesh mr; CHECK(K::build_mesh(model, opt, mr, why), "skin-refl mesh: %s", why.c_str());
      K::Texture map; map.w = 8; map.h = 8; map.rgba.resize(8 * 8 * 4);
      for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) { uint8_t* p = &map.rgba[(y * 8 + x) * 4]; p[0] = uint8_t(x * 32); p[1] = uint8_t(y * 32); p[2] = 120; p[3] = 255; }
      auto tilted = [](float pitch_deg, float yaw_deg) {   // the game's axes in model space: up tilted toward model -Z by `pitch`, then turned about it by `yaw`
          const float b = deg2rad(pitch_deg), c = deg2rad(yaw_deg); K::ReflBasis r;
          const float rx[3] = {1, 0, 0}, ry[3] = {0, std::cos(b), -std::sin(b)}, rz[3] = {0, std::sin(b), std::cos(b)};
          for (int i = 0; i < 3; ++i) { r.x[i] = std::cos(c) * rx[i] + std::sin(c) * rz[i]; r.y[i] = ry[i]; r.z[i] = -std::sin(c) * rx[i] + std::cos(c) * rz[i]; }
          return r; };
      const ViewSetup back = make_view("cockpit_refl_back", 180.0f, 0.0f, pos0, 0.0f, 0.0f);
      // the tail face seen from the eye has the normal model -Z: uv (0.22, 0.17) mask 1 / uv (0.71, 0.05) mask 0.52
      const K::ReflBasis b1 = tilted(30.0f, 40.0f), b2 = tilted(3.0f, -25.0f);
      Result plain = run_case("c_model_refl_off", back, mr, 13, false, true);   // no map in the job: kCFlagRefl cleared by the backend
      CHECK(plain.ok && !plain.facts.cockpit_refl_used && !plain.facts.cockpit_refl_uploaded, "no map: no refl term");
      refl_in = &b1; refl_map_in = &map; refl_gen_in = 501;
      Result r1 = run_case("c_model_refl_b1", back, mr, 13, false, false);
      CHECK(r1.ok && r1.facts.cockpit_refl_uploaded && r1.facts.cockpit_refl_used, "first map use: copied and used");
      if (r1.ok && plain.ok) { float p[4], q[4]; px_lin(r1.px[0], W / 2, H / 2, p); px_lin(plain.px[0], W / 2, H / 2, q);
          INFO("skin-refl tail centre: off %.3f %.3f %.3f on %.3f %.3f %.3f", q[0], q[1], q[2], p[0], p[1], p[2]);
          CHECK(p[0] > q[0] + 0.01f && p[1] > q[1] + 0.01f && p[2] > q[2] + 0.01f, "the reflection adds light on the skin"); }
      Result r1b = run_case("c_model_refl_b1_again", back, mr, 13, false, false);
      CHECK(r1b.ok && !r1b.facts.cockpit_refl_uploaded && r1b.facts.cockpit_refl_used, "same map key: no copy");
      if (r1.ok && r1b.ok) CHECK(r1.px[0] == r1b.px[0], "same map key: identical image");
      refl_in = &b2; refl_gen_in = 502;
      Result r2 = run_case("c_model_refl_b2", back, mr, 13, false, false);
      CHECK(r2.ok && r2.facts.cockpit_refl_uploaded, "new map key: copied again");
      { opt.skin_refl = false; Result r0 = run_case("c_model_refl_optoff", back, mr, 13, false, false);   // option off with the map attached: the COCKPIT-3 image
        CHECK(r0.ok && !r0.facts.cockpit_refl_used && plain.ok && r0.px[0] == plain.px[0], "srefl:0 = the image without the map"); }
      refl_in = nullptr; refl_map_in = nullptr; refl_gen_in = 0;
      opt = K::Options{}; opt.display = false; opt.gain = 1.15f; opt.floor = false; opt.interior = false; }
    // 14) CFID (cfid:1, default): the synthetic model + a glare shield (y 4.3, z -40..-37.5), two side rails and an up-facing knee panel under the glare
    //     shield's aft edge; looking down at the panel with the stage sun ahead-high (the glare shield shades the panel) and behind-high (lit), plus the exterior
    //     tail's 2 (n.L)^3 response looking back - all through the REAL vs_model/ps_model vs render_reference; cfid:0 = the pre-CFID image.
    { K::ModelData mc = model;
      auto quad = [&](const float q[4][3], const float n[3], uint32_t mflags, uint8_t grey) {
          K::Part p; p.name_off = 0; p.node = uint32_t(mc.parts.size()); p.mflags = mflags; p.texture = -1;
          for (int i = 0; i < 4; ++i) p.local_to_model[i * 5] = 1.0f;
          p.first_vertex = uint32_t(mc.vertices.size()); p.vertex_count = 4; p.first_index = uint32_t(mc.indices.size()); p.index_count = 6;
          for (int i = 0; i < 4; ++i) { K::Vertex v{}; for (int a = 0; a < 3; ++a) { v.pos[a] = q[i][a]; v.nrm[a] = int16_t(std::lround(n[a] * 32767.0f)); }
                                        v.part = uint16_t(mc.parts.size()); v.rgba[0] = v.rgba[1] = v.rgba[2] = grey; v.rgba[3] = 255; mc.vertices.push_back(v); }
          const uint32_t b = p.first_vertex; for (uint32_t ix : {b, b + 1, b + 2, b, b + 2, b + 3}) mc.indices.push_back(ix);
          mc.parts.push_back(p); };
      const float nU[3] = {0, 1, 0}, nX[3] = {1, 0, 0}, nXm[3] = {-1, 0, 0};
      const float gs[4][3] = {{-1.5f, 4.3f, -37.5f}, {0.0f, 4.3f, -37.5f}, {0.0f, 4.3f, -40.0f}, {-1.5f, 4.3f, -40.0f}}, gs2[4][3] = {{0.0f, 4.3f, -37.5f}, {1.5f, 4.3f, -37.5f}, {1.5f, 4.3f, -40.0f}, {0.0f, 4.3f, -40.0f}};   // two halves: centre vertices
      const float rl[4][3] = {{-1.8f, 3.6f, -33.0f}, {-1.6f, 4.1f, -33.0f}, {-1.6f, 4.1f, -37.0f}, {-1.8f, 3.6f, -37.0f}};
      const float rr[4][3] = {{1.6f, 4.1f, -33.0f}, {1.8f, 3.6f, -33.0f}, {1.8f, 3.6f, -37.0f}, {1.6f, 4.1f, -37.0f}};
      const float kp[4][3] = {{-1.2f, 2.6f, -36.4f}, {1.2f, 2.6f, -36.4f}, {1.2f, 2.6f, -37.4f}, {-1.2f, 2.6f, -37.4f}};
      quad(gs, nU, 0, 180); quad(gs2, nU, 0, 180); quad(rl, nX, 0, 180); quad(rr, nXm, 0, 180); quad(kp, nU, 0, 220);
      opt = K::Options{}; opt.interior = false; opt.floor = false;
      K::Mesh mcm; CHECK(K::build_mesh(mc, opt, mcm, why) && mcm.shadow, "CFID mesh with a shadow box: %s", why.c_str());
      INFO("CFID box sill %.2f half %.2f z %.2f..%.2f", mcm.shadow_box[0], mcm.shadow_box[1], mcm.shadow_box[2], mcm.shadow_box[3]);
      static const float kAhead[3] = {0.0f, 0.7f, -0.714143f}, kBehind[3] = {0.0f, 0.7f, 0.714143f}, kTail[3] = {0.2f, 0.5f, -0.84f};
      const ViewSetup knee = make_view("cockpit_cfid_knee", 0.0f, -55.0f, pos0, 0.0f, 0.0f);
      light_in = kAhead; Result a = run_case("c_model_cfid_shadow", knee, mcm, 14, false, true);
      light_in = kBehind; Result b = run_case("c_model_cfid_lit", knee, mcm, 14, false, false);
      if (a.ok && b.ok) { size_t darker = 0; for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float p[4], q[4]; px_lin(a.px[0], x, y, p); px_lin(b.px[0], x, y, q);
                                                                                                        if (q[1] > p[1] + 0.05f) ++darker; }
                          INFO("CFID knee panel: %zu pixels darker under the sun ahead (shadowed) than behind", darker);
                          CHECK(darker > 200, "the glare shield shades the knee panel under a sun ahead-high (%zu px)", darker); }
      light_in = kAhead; opt.fidelity = false; Result z = run_case("c_model_cfid_off", knee, mcm, 14, false, false);
      CHECK(z.ok && a.ok && z.px[0] != a.px[0], "cfid:0 has no shadow (image differs)");
      opt.fidelity = true; light_in = kTail;
      const ViewSetup back = make_view("cockpit_cfid_back", 180.0f, 0.0f, pos0, 0.0f, 0.0f);
      Result t1 = run_case("c_model_cfid_tail", back, mcm, 14, false, false);
      opt.fidelity = false; Result t0 = run_case("c_model_cfid_tail_off", back, mcm, 14, false, false);
      if (t1.ok && t0.ok) { float p[4], q[4]; px_lin(t1.px[0], W / 2, H / 2, p); px_lin(t0.px[0], W / 2, H / 2, q);
                            INFO("CFID tail centre: cfid:1 %.3f %.3f %.3f cfid:0 %.3f %.3f %.3f", p[0], p[1], p[2], q[0], q[1], q[2]);
                            CHECK(std::fabs(p[1] - q[1]) > 0.005f, "the skin takes the 2 (n.L)^3 response"); }
      light_in = nullptr;
      opt = K::Options{}; opt.display = false; opt.gain = 1.15f; opt.floor = false; opt.interior = false; }
}

// ----------------------------------------------------------------- WORLD-3 props/trees: the REAL props pass (xr_props.hlsl, instanced, depth-tested) merged by the REAL
// composite (xr_props_composite.hlsli) vs a CPU reference: analytic ray/box intersection per pixel, kept where acz::props::prop_visible (nearer than the terrain hit of
// acz::terrain::trace), coloured with acz::props::prop_color (WORLD-1 display domain + haze, linear); everything else = acz::terrain::trace (exactly ref_terrain).
struct BoxRef { float lo[3], hi[3]; };
static bool ray_box(const float o[3], const float d[3], const BoxRef& b, float& t, float n[3]) {
    float t0 = 0.0f, t1 = 1e30f; int ax = -1; float sg = 0;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12f) { if (o[i] < b.lo[i] || o[i] > b.hi[i]) return false; continue; }
        float a = (b.lo[i] - o[i]) / d[i], c = (b.hi[i] - o[i]) / d[i];
        float s = -1.0f;                       // entering through the lo face
        if (a > c) { std::swap(a, c); s = 1.0f; }
        if (a > t0) { t0 = a; ax = i; sg = s; }
        t1 = std::min(t1, c);
        if (t0 > t1) return false;
    }
    if (ax < 0) return false;                  // eye inside
    t = t0; n[0] = n[1] = n[2] = 0; n[ax] = sg;
    return true;
}
static Img ref_props(const acz::terrain::StageData& st, const acz::terrain::Atlas& at, const acz::props::Runtime& pr, const BlitJob& j, int eye, float size, const float tex[3]) {
    const Cfg c = cfg_for(j, eye);
    Img out(W, H);
    const WorldComposite& wc = j.world_eye[eye];
    const TerrainEye& te = j.terrain_eye[eye];
    std::vector<BoxRef> boxes;
    for (const acz::props::Instance& in : pr.set.inst) {
        const float h = 0.5f * size * in.scale;
        boxes.push_back(BoxRef{{in.pos[0] - h, in.pos[1], in.pos[2] - h}, {in.pos[0] + h, in.pos[1] + 2.0f * h, in.pos[2] + h}});
    }
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const float u = (float(x) + 0.5f) / float(W), v = (float(y) + 0.5f) / float(H);
        float d[3], dw[3];
        for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + u * wc.ux[i] + v * wc.vy[i];
        for (int i = 0; i < 3; ++i) dw[i] = te.right[i] * d[0] + te.up[i] * d[1] + te.fwd[i] * (-d[2]);
        const float l = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
        for (int i = 0; i < 3; ++i) dw[i] /= l;
        float best = 1e30f, bn[3] = {0, 1, 0};
        for (const BoxRef& b : boxes) { float t, n[3]; if (ray_box(te.eye, dw, b, t, n) && t < best && t >= 1.0f) { best = t; std::memcpy(bn, n, sizeof bn); } }
        float rgb[3];
        const acz::terrain::Trace tr = acz::terrain::trace(st, at, te.eye, dw, j.terrain_haze_l, j.terrain_far);
        if (best < 1e29f && acz::props::prop_visible(best, j.terrain_far, tr.hit, tr.t)) {
            if (pr.opt.shade_game) acz::props::prop_color_game(tex, best, rgb);   // PROPS-SHADING propshade:game (default)
            else acz::props::prop_color(tex, bn, dw, best, j.terrain_haze_l, j.terrain_far, rgb);
        }
        else std::memcpy(rgb, tr.rgb, sizeof rgb);
        float* o = out.at(x, y); o[0] = sat(rgb[0] * c.exposure); o[1] = sat(rgb[1] * c.exposure); o[2] = sat(rgb[2] * c.exposure); o[3] = 1.0f;
    }
    return out;
}
static void case_props(Chains& ch, const Scene& s, bool docs) {
    std::printf("[t] WORLD-3 props/trees pass (instanced, depth-tested against the ray-marched terrain) vs CPU reference\n");
    namespace T = acz::terrain; namespace P = acz::props;
    T::StageData st = T::make_synthetic_stage(3, false);
    T::Atlas at; T::build_atlas(st, at);
    const float origin[3] = {0, 0, 0};
    const ViewSetup v = make_view("props", 20.0f, -8.0f, origin, 0.1f, 0);
    T::Conventions conv;
    const float campos[3] = {2000.0f, 6500.0f, -3000.0f};
    T::CamFrame cf{};
    CHECK(T::camera_frame(campos, deg2rad(-25.0f), deg2rad(-30.0f), 0.0f, conv, cf), "props: camera frame");
    auto make_job = [&](uint64_t gen) {
        BlitJob j = base_job(s, true); job_world(j, s, v, false);
        j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at; j.terrain_gen = gen;
        for (int e = 0; e < 2; ++e) {
            T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
        }
        return j;
    };
    // ground points of a few eye-0 pixels (CPU trace) -> instance anchors
    BlitJob probe = make_job(1);
    auto ground = [&](float u, float vv, float out[3]) {
        const WorldComposite& wc = probe.world_eye[0]; const TerrainEye& te = probe.terrain_eye[0];
        float d[3], dw[3];
        for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + u * wc.ux[i] + vv * wc.vy[i];
        for (int i = 0; i < 3; ++i) dw[i] = te.right[i] * d[0] + te.up[i] * d[1] + te.fwd[i] * (-d[2]);
        const float l = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]); for (float& q : dw) q /= l;
        const T::Trace tr = T::trace(st, at, te.eye, dw, 20000.0f, 58000.0f);
        for (int i = 0; i < 3; ++i) out[i] = tr.p[i];
        return tr.hit;
    };
    float pa[3], pb[3], pc[3];
    CHECK(ground(0.45f, 0.55f, pa) && ground(0.25f, 0.70f, pb) && ground(0.70f, 0.45f, pc), "props: probe rays hit the ground");
    const float size = 2500.0f;
    const uint8_t red[4] = {200, 40, 40, 255};
    const float tex[3] = {200.0f / 255.0f, 40.0f / 255.0f, 40.0f / 255.0f};
    std::vector<P::Instance> inst = {
        P::Instance{{pa[0], 0.0f, pa[2]}, 1.0f, 0, P::kInstGroundRelative},                                        // A: on the slope (snapped)
        P::Instance{{pb[0], T::height_bilinear(st, pb[0], pb[2]) - 3.0f * size, pb[2]}, 1.0f, 0, 0},             // B: buried: must be invisible
        P::Instance{{pc[0], T::height_bilinear(st, pc[0], pc[2]) - 0.5f * size, pc[2]}, 1.0f, 0, 0},             // C: half sunk into the slope
    };
    auto rt = std::make_shared<P::Runtime>();
    rt->set = P::make_box_set(size, red, inst);
    P::snap_to_terrain(rt->set, st);
    rt->opt.props = true; rt->opt.prop_range = 58000.0f; rt->generation = 1;
    P::build_grid(rt->set, rt->opt, rt->grid);
    auto check_case = [&](const char* name, const BlitJob& j, const P::Runtime& r, bool expect_props, float tol, double allow_frac) {
        Result res = run_job(ch, j, 3u);
        CHECK(res.ok && res.facts.ok && res.facts.terrain_used, "%s: job did not run", name);
        if (!res.ok) return res;
        CHECK(res.facts.props_used == expect_props, "%s: props_used=%d", name, int(res.facts.props_used));
        CHECK(res.facts.sources_restored && res.facts.barriers_out == res.facts.barriers_back, "%s: barrier contract", name);
        for (int e = 0; e < 2; ++e) {
            const Img ref = expect_props ? ref_props(st, at, r, j, e, size, tex) : ref_terrain(st, at, j, e, s);
            const Cmp m = compare(res.px[e], ref, tol);
            INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu instances=%u draws=%u", name, e, m.max_err, m.bad, m.n, res.facts.props_instances, res.facts.props_draws);
            CHECK(double(m.bad) <= allow_frac * double(m.n), "%s eye%d: %zu pixels differ from the CPU reference by > %.3f (first at %d,%d)", name, e, m.bad, tol, m.fx, m.fy);
            if (e == 0) save_sheet(name, res.px[0], ref, docs);
        }
        return res;
    };
    // 1) props on: GPU = reference (box edges and the contact line may differ by a pixel); first use uploads the texture
    BlitJob j = make_job(1); j.props = rt.get();
    Result r1 = check_case("t_props_slope_depth", j, *rt, true, 0.06f, 0.03);
    CHECK(r1.ok && r1.facts.props_uploaded && r1.facts.props_instances == 3 && r1.facts.props_draws == 2, "props: 3 instances in one batch per eye (draws=%u)", r1.facts.props_draws);
    if (r1.ok) {
        // A really is visible (red dominates near its projected centre) and B is not (its projection shows terrain, no red)
        const TerrainEye& te = j.terrain_eye[0];
        float m[16]; CHECK(P::make_clip(j.world_eye[0].base, j.world_eye[0].ux, j.world_eye[0].vy, te.right, te.up, te.fwd, m), "props: clip");
        auto at_px = [&](const P::Instance& in, float yoff, float o[4]) {
            const float p[3] = {in.pos[0] - te.eye[0], in.pos[1] + yoff - te.eye[1], in.pos[2] - te.eye[2]};
            float u, vv, c; if (!P::project(m, p, u, vv, c)) return false;
            const int x = int(u * W), y = int(vv * H); if (x < 0 || y < 0 || x >= W || y >= H) return false;
            px_lin(r1.px[0], x, y, o); return true;
        };
        float a4[4], b4[4];
        CHECK(at_px(rt->set.inst[0], 0.5f * size, a4) && a4[0] > 2.0f * a4[1] && a4[0] > 0.2f, "props: A (on the slope) is drawn: rgb %.3f %.3f %.3f", a4[0], a4[1], a4[2]);
        CHECK(at_px(rt->set.inst[1], 0.5f * size, b4) && !(b4[0] > 2.0f * b4[1] && b4[0] > 0.2f), "props: B (buried) is hidden by the terrain: rgb %.3f %.3f %.3f", b4[0], b4[1], b4[2]);
        // the buried box alone gives exactly the terrain-only picture
        auto rb = std::make_shared<P::Runtime>(*rt);
        rb->set.inst = {rt->set.inst[1]}; rb->generation = 2; P::build_grid(rb->set, rb->opt, rb->grid);
        BlitJob jb = make_job(1); jb.props = rb.get();
        BlitJob jt = make_job(1);
        Result x1 = run_job(ch, jb, 3u), x0 = run_job(ch, jt, 3u);
        CHECK(x1.ok && x0.ok && x1.facts.props_used && x1.facts.props_instances == 1, "buried-only: ran with 1 instance");
        if (x1.ok && x0.ok) CHECK(x1.px[0] == x0.px[0] && x1.px[1] == x0.px[1], "buried-only: eye images byte-identical to the terrain-only job (depth against terrain)");
    }
    // 1b) PROPS-SHADING propshade:w3 = the WORLD-3/4 shading (stage-free default light + exponential haze) still matches its own CPU reference, and differs
    //     from the default game shading only on prop pixels (terrain byte-identical)
    {
        auto rw = std::make_shared<P::Runtime>(*rt); rw->opt.shade_game = false; rw->generation = 5;
        BlitJob jw = make_job(1); jw.props = rw.get();
        Result rw3 = check_case("t_props_shade_w3", jw, *rw, true, 0.06f, 0.03);
        if (rw3.ok && r1.ok) {
            CHECK(rw3.px[0] != r1.px[0], "propshade: game and w3 shading differ on the props");
            BlitJob jt = make_job(1); Result t0 = run_job(ch, jt, 3u);
            size_t diff_game = 0, diff_w3 = 0, both = 0;
            for (size_t i = 0; t0.ok && i < t0.px[0].size(); i += 4) {
                const bool a = std::memcmp(&t0.px[0][i], &r1.px[0][i], 3) != 0, b = std::memcmp(&t0.px[0][i], &rw3.px[0][i], 3) != 0;
                diff_game += a; diff_w3 += b; both += a && b;
            }
            INFO("propshade: pixels differing from terrain-only: game %zu, w3 %zu, both %zu", diff_game, diff_w3, both);
            CHECK(t0.ok && diff_game > 0 && both >= diff_game * 9 / 10, "propshade: the shading mode changes only prop pixels");
        }
    }
    // 2) instance cap: 1 -> only the nearest is drawn
    {
        auto rc = std::make_shared<P::Runtime>(*rt); rc->opt.cap = 1; rc->generation = 3;
        BlitJob jc = make_job(1); jc.props = rc.get();
        Result r = run_job(ch, jc, 3u);
        CHECK(r.ok && r.facts.props_used && r.facts.props_instances == 1, "cap 1: instances=%u", r.facts.props_instances);
    }
    // 3) culling: nothing in range -> the pass does not run and the picture is the terrain-only one, byte for byte
    {
        auto rr = std::make_shared<P::Runtime>(*rt); rr->opt.prop_range = 100.0f; rr->generation = 4;
        BlitJob jr = make_job(1); jr.props = rr.get();
        BlitJob jt = make_job(1);
        Result a = run_job(ch, jr, 3u), b = run_job(ch, jt, 3u);
        CHECK(a.ok && b.ok && !a.facts.props_used && a.facts.props_instances == 0, "out of range: props_used=%d", int(a.facts.props_used));
        if (a.ok && b.ok) CHECK(a.px[0] == b.px[0] && a.px[1] == b.px[1], "out of range: byte-identical to terrain only");
    }
    // 4) key off (options off, or no runtime) after props use: byte-identical to the terrain-only job
    {
        auto ro = std::make_shared<P::Runtime>(*rt); ro->opt.props = false; ro->opt.trees = false;
        BlitJob jo = make_job(1); jo.props = ro.get();
        BlitJob jt = make_job(1);
        Result a = run_job(ch, jo, 3u), b = run_job(ch, jt, 3u);
        CHECK(a.ok && b.ok && !a.facts.props_used, "props key off: not used");
        if (a.ok && b.ok) CHECK(a.px[0] == b.px[0] && a.px[1] == b.px[1], "props key off: byte-identical to terrain only");
        // and without the terrain the props pointer is inert (plain world layer)
        BlitJob jw = base_job(s, true); job_world(jw, s, v, false); jw.props = rt.get();
        BlitJob jw0 = base_job(s, true); job_world(jw0, s, v, false);
        Result c = run_job(ch, jw, 3u), d = run_job(ch, jw0, 3u);
        CHECK(c.ok && d.ok && !c.facts.props_used && c.px[0] == d.px[0] && c.px[1] == d.px[1], "props without terrain: inert, byte-identical");
    }
    // 5) determinism
    {
        Result a = run_job(ch, j, 3u), b = run_job(ch, j, 3u);
        CHECK(a.ok && b.ok && a.px[0] == b.px[0] && a.px[1] == b.px[1] && !b.facts.props_uploaded, "props: GPU output deterministic, no re-upload on reuse");
    }
    // 6) PERF props_async (what the consumer uses): the first job only STARTS the init worker and draws the terrain without props (byte-identical to the
    //    terrain-only job); once the worker is done the props appear, exactly the synchronous picture. A 1.2M-instance set keeps the first job cheap.
    {
        std::printf("[t] PERF async props init: first frame never waits, props appear after ready\n");
        auto ra = std::make_shared<P::Runtime>(*rt); ra->generation = 20;
        BlitJob ja = make_job(1); ja.props = ra.get(); ja.props_async = true;
        BlitJob jt = make_job(1);
        const int enter0 = acz::g_scope_enter.load();
        Result a0 = run_job(ch, ja, 3u), t0 = run_job(ch, jt, 3u);
        CHECK(a0.ok && t0.ok && !a0.facts.props_used && (a0.facts.props_pending || a0.facts.props_ready), "async: first job runs without props (pending=%d)", int(a0.facts.props_pending));
        if (a0.ok && t0.ok && !a0.facts.props_used) CHECK(a0.px[0] == t0.px[0] && a0.px[1] == t0.px[1], "async: pending job byte-identical to terrain only");
        Result ready{}; int polls = 0;
        for (; polls < 600; ++polls) {   // <= ~6 s on WARP
            ready = run_job(ch, ja, 3u);
            if (!ready.ok || ready.facts.props_used || ready.facts.props_failed) break;
            Sleep(10);
        }
        CHECK(ready.ok && ready.facts.props_used && ready.facts.props_ready && !ready.facts.props_pending, "async: props drawn after the worker finished (polls %d)", polls);
        INFO("async init ready after %d polls, worker %u ms; first job record %.2f ms", polls, ready.facts.props_init_ms, a0.record_ms);
        CHECK(acz::g_scope_enter.load() > enter0 && acz::g_scope_enter.load() == acz::g_scope_leave.load(), "async: worker ran inside the proxy creation scope");
        if (ready.ok && r1.ok) CHECK(ready.px[0] == r1.px[0] && ready.px[1] == r1.px[1], "async: after ready, byte-identical to the synchronous props picture");
        // a big forest: 1.2M ground-relative instances spread over the synthetic stage; the first async job does no GPU build and a bounded selection
        auto rbig = std::make_shared<P::Runtime>();
        std::vector<P::Instance> big; big.reserve(1200000);
        uint32_t sd = 99u; auto rnd = [&]() { sd = sd * 1664525u + 1013904223u; return float(sd >> 8) / float(1u << 24); };
        for (int k = 0; k < 1200000; ++k) big.push_back(P::Instance{{(rnd() - 0.5f) * 400000.0f, 0.0f, (rnd() - 0.5f) * 400000.0f}, 1.0f, 0, P::kInstGroundRelative});
        rbig->set = P::make_box_set(300.0f, red, big);
        rbig->set.models[0].kind = P::kKindTree;
        CHECK(P::snap_to_terrain(rbig->set, st), "big: snapped");
        rbig->opt.trees = true; rbig->opt.tree_range = 16000.0f; rbig->generation = 30;
        P::build_grid(rbig->set, rbig->opt, rbig->grid);
        BlitJob jb = make_job(1); jb.props = rbig.get(); jb.props_async = true;
        Result b0 = run_job(ch, jb, 3u);
        CHECK(b0.ok && !b0.facts.props_used && b0.facts.props_pending, "big async: first job pending, no props");
        if (b0.ok && t0.ok) CHECK(b0.px[0] == t0.px[0], "big async: first job = terrain only");
        INFO("big async first job: record %.2f ms (terrain-only record %.2f ms)", b0.record_ms, t0.record_ms);
        CHECK(b0.record_ms < 1000.0, "big async: first job record bounded (%.2f ms)", b0.record_ms);
        if (b0.record_ms > 50.0) std::printf("  SOFT-EXCEEDED big async first job record %.2f ms > 50 ms\n", b0.record_ms);
        Result b1{};
        for (polls = 0; polls < 600; ++polls) { b1 = run_job(ch, jb, 3u); if (!b1.ok || b1.facts.props_used || b1.facts.props_failed) break; Sleep(10); }
        CHECK(b1.ok && b1.facts.props_used && b1.facts.props_instances > 0 && b1.facts.props_instances <= 16384, "big async: trees drawn after ready (instances %u)", b1.facts.props_instances);
        INFO("big async: ready after %d polls, %u instances drawn, record %.2f ms", polls, b1.facts.props_instances, b1.record_ms);
    }
}

// ----------------------------------------------------------------- GPU-COST (docs/world-render/gpu-cost.md): zpre identity, q1/q2 error budget, pass timestamps, options
static void case_gpu_cost(Chains& ch, const Scene& s, bool docs) {
    std::printf("[g] GPU-COST: zpre (model-first + depth-tested composite) byte identity, q1/q2 vs the CPU trace, pass marks, options\n");
    namespace T = acz::terrain; namespace K = acz::cockpit;
    T::StageData st = T::make_synthetic_stage(3, false); T::add_synthetic_env(st, 9000.0f);
    st.flags |= T::kFlagSeaPresent | T::kFlagSeaGuess; st.sea_level = 14.0f * st.hstep;
    T::Atlas at; T::build_atlas(st, at);
    T::Conventions lc; lc.look = true;
    std::vector<float> env; T::build_env_texture(st, lc, env);
    const float pos0[3] = {0, 0, 0};
    T::Conventions conv;
    auto terrain_job = [&](const ViewSetup& v, float yaw_deg, float pitch_deg, float alt, uint32_t q) {
        BlitJob j = base_job(s, true); job_world(j, s, v, false);
        j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at; j.terrain_gen = 61;
        j.terrain_env = &env; j.terrain_env_bits = T::kEnvAll; j.terrain_haze_l = T::kLookHazeL; j.terrain_quality = q;
        const float campos[3] = {8192.0f, alt, 2000.0f};
        T::CamFrame cf{};
        T::camera_frame(campos, deg2rad(pitch_deg), deg2rad(yaw_deg), 0.0f, conv, cf);
        for (int e = 0; e < 2; ++e) {
            T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
        }
        return j;
    };
    // 1) zpre: model cockpit (c4 synthetic: opaque + alpha-tested + blend + canopy glass + interior) over (a) the terrain with the full environment and
    //    (b) the plain world-image composite; zpre:1 must give the SAME bytes in both eyes (the opaque model overwrote those pixels anyway), and facts report it.
    {
        const K::ModelData m4 = K::make_synthetic_model_c4();
        K::Options opt; K::Mesh mesh; std::string why;
        CHECK(K::build_mesh(m4, opt, mesh, why) && mesh.glass_vertices > 0 && mesh.blend_vertices + mesh.opaque_vertices > 0, "zpre: c4 mesh %s", why.c_str());
        const ViewSetup views[3] = {make_view("zpre_fwd", 0.0f, -6.0f, pos0, 0.0f, 0.0f), make_view("zpre_down", 15.0f, -25.0f, pos0, 0.0f, 0.0f),
                                    make_view_roll("zpre_6dof", -10.0f, 6.0f, 25.0f, pos0, 0.0f, 0.0f)};
        for (int vi = 0; vi < 3; ++vi) for (int terrain = 0; terrain < 2; ++terrain) {
            const ViewSetup& v = views[vi];
            BlitJob j = terrain ? terrain_job(v, -30.0f, -8.0f, 5000.0f, 0) : base_job(s);
            if (!terrain) job_world(j, s, v, false);
            j.cockpit = true; j.cockpit_mesh = &mesh; j.cockpit_gen = 31;
            for (int e = 0; e < 2; ++e) {
                const float fov[4] = {v.eye[e].fov.left, v.eye[e].fov.right, v.eye[e].fov.up, v.eye[e].fov.down};
                K::EyeCam cam{};
                CHECK(K::eye_camera(v.eye[e].pose.orientation, v.eye[e].pose.position, fov, v.anchor, v.yaw, mesh.eye, opt.upm, opt.near_u, cam), "zpre: eye camera");
                j.cockpit_eye[e] = K::make_constants(cam, opt, j.exposure, uint32_t(j.tonemap));
            }
            BlitJob jz = j; jz.cockpit_prepass = true;
            Result a = run_job(ch, j, 3u), b = run_job(ch, jz, 3u);
            CHECK(a.ok && b.ok && a.facts.cockpit_used && b.facts.cockpit_used && !a.facts.cockpit_prepass && b.facts.cockpit_prepass, "zpre %s/%s: ran (prepass=%d)", v.name,
                terrain ? "terrain" : "world", int(b.facts.cockpit_prepass));
            if (a.ok && b.ok) {
                size_t diff = 0, model_px = 0;
                for (int e = 0; e < 2; ++e) for (size_t i = 0; i < a.px[e].size(); ++i) diff += a.px[e][i] != b.px[e][i];
                for (size_t i = 0; i < a.px[0].size(); i += 4) model_px += a.px[0][i + 3] == 255;
                INFO("zpre %-10s %-7s eye bytes differing: %zu (opaque eye-0 pixels %zu of %d)", v.name, terrain ? "terrain" : "world", diff, model_px, W * H);
                CHECK(diff == 0, "zpre %s/%s: %zu bytes differ from the composite-first order", v.name, terrain ? "terrain" : "world", diff);
                CHECK(b.facts.barriers_out == b.facts.barriers_back && b.facts.sources_restored, "zpre: barrier contract");
                if (vi == 0 && terrain) save_sheet("g_zpre_terrain", b.px[0], [&] { Img im(W, H); for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float p[4]; px_lin(a.px[0], x, y, p); for (int k = 0; k < 4; ++k) im.at(x, y)[k] = p[k]; } return im; }(), docs);
            }
            // debug views never use zpre (the model is not drawn there); png_once with zpre still equals the submitted eye 0
            if (vi == 0 && terrain) {
                BlitJob jd = jz; jd.debug_view = 3;
                Result d = run_job(ch, jd, 3u);
                CHECK(d.ok && !d.facts.cockpit_prepass, "zpre: not used with a debug view");
                BlitJob jp = jz; jp.png_capture = true;
                Result p = run_job(ch, jp, 3u, true);
                CHECK(p.ok && p.png_taken && p.facts.cockpit_prepass, "zpre + png_once ran");
                if (p.ok && p.png_taken) { size_t dd = 0; for (size_t i = 0; i < p.px[0].size(); i += 4) for (int k = 0; k < 3; ++k) dd += p.png.final_rgba[i + k] != p.px[0][i + k];
                                           CHECK(dd == 0, "zpre: png final image = the submitted eye 0 (%zu differences)", dd); }
            }
        }
    }
    // 2) q1 / q2 vs the CPU reference (trace_env = the q0 march): the same 0.05 linear tolerance as the q0 cases, with a documented pixel budget (q1 <= 1 %,
    //    q2 <= 3 % of the pixels; the differences are silhouette pixels where the coarser far-field step lands on another ridge and near-saturated haze).
    {
        const ViewSetup vdown = make_view("q-down", 20.0f, -8.0f, pos0, 0.1f, 0);
        const ViewSetup vhor = make_view("q-horizon", 0.0f, 0.0f, pos0, 0.1f, 0);
        struct C { const char* name; const ViewSetup* v; float yaw, pitch, alt; };
        const C cases[] = {{"g_q_down", &vdown, 30.0f, -12.0f, 6500.0f}, {"g_q_horizon", &vhor, 120.0f, -1.0f, 1500.0f}, {"g_q_low", &vhor, -60.0f, -3.0f, 900.0f}};
        for (const C& c : cases) {
            double frac[3] = {0, 0, 0};
            for (uint32_t q = 0; q <= 2; ++q) {
                const BlitJob j = terrain_job(*c.v, -c.yaw, c.pitch, c.alt, q);
                Result r = run_job(ch, j, 3u);
                CHECK(r.ok && r.facts.terrain_used, "%s q%u: ran", c.name, q);
                if (!r.ok) continue;
                for (int e = 0; e < 2; ++e) {
                    const Cfg cf = cfg_for(j, e);
                    Img ref(W, H);
                    const WorldComposite& wc = j.world_eye[e];
                    const TerrainEye& te = j.terrain_eye[e];
                    const T::EnvView ev{env.data(), T::kEnvAll};
                    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                        const float u = (float(x) + 0.5f) / float(W), vv = (float(y) + 0.5f) / float(H);
                        float d[3], dw[3];
                        for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + u * wc.ux[i] + vv * wc.vy[i];
                        for (int i = 0; i < 3; ++i) dw[i] = te.right[i] * d[0] + te.up[i] * d[1] + te.fwd[i] * (-d[2]);
                        const float l = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]); for (float& k : dw) k /= l;
                        const T::Trace tr = T::trace_env(st, at, ev, te.eye, dw, j.terrain_haze_l, j.terrain_far);
                        float* o = ref.at(x, y); for (int k = 0; k < 3; ++k) o[k] = sat(tr.rgb[k] * cf.exposure); o[3] = 1.0f;
                    }
                    const Cmp m = compare(r.px[e], ref, 0.05f);
                    frac[q] = std::max(frac[q], double(m.bad) / double(m.n));
                    INFO("%-14s q%u eye%d max_err=%.4f bad=%zu/%zu", c.name, q, e, m.max_err, m.bad, m.n);
                    const double budget = q == 0 ? 0.01 : q == 1 ? 0.01 : 0.03;
                    CHECK(double(m.bad) <= budget * double(m.n), "%s q%u eye%d: %zu pixels over 0.05 (budget %.0f %%)", c.name, q, e, m.bad, budget * 100.0);
                    if (e == 0 && q == 1) save_sheet(c.name, r.px[0], ref, docs);
                }
            }
            INFO("%-14s worst-eye fraction over 0.05: q0 %.4f q1 %.4f q2 %.4f", c.name, frac[0], frac[1], frac[2]);
        }
        // q0 explicitly = the default job (no other field): byte-identical
        BlitJob a = terrain_job(vdown, -30.0f, -12.0f, 6500.0f, 0), b = a; b.terrain_quality = 0;
        Result ra = run_job(ch, a, 3u), rb = run_job(ch, b, 3u);
        CHECK(ra.ok && rb.ok && ra.px[0] == rb.px[0] && ra.px[1] == rb.px[1], "q0 = the default march (byte-identical)");
        BlitJob c3 = a; c3.terrain_quality = 7;   // out of range: clamped to q2
        BlitJob c2 = a; c2.terrain_quality = 2;
        Result r7 = run_job(ch, c3, 3u), r2 = run_job(ch, c2, 3u);
        CHECK(r7.ok && r2.ok && r7.px[0] == r2.px[0], "q > 2 is clamped to q2");
    }
    // 3) pass timestamps: monotone marks, available after completion only; a job without pass_timing reports none
    {
        const ViewSetup v = make_view("marks", 20.0f, -8.0f, pos0, 0.1f, 0);
        BlitJob j = terrain_job(v, -30.0f, -12.0f, 6500.0f, 0); j.pass_timing = true;
        Result r = run_job(ch, j, 3u);
        PassTiming pt{};
        const bool got = r.ok && ch.blit->pass_timing(pt);
        if (!got) INFO("pass timing unavailable on this adapter (timestamp queries); marks not checked");
        else {
            bool mono = pt.hz > 0;
            for (unsigned m = 1; m < kPassMarks; ++m) mono = mono && pt.ticks[m] >= pt.ticks[m - 1];
            CHECK(mono, "pass marks monotone (hz %llu, total %llu ticks)", (unsigned long long)pt.hz, (unsigned long long)(pt.ticks[kPassEnd] - pt.ticks[kPassStart]));
        }
        BlitJob j0 = j; j0.pass_timing = false;
        Result r0 = run_job(ch, j0, 3u);
        PassTiming p0{};
        CHECK(r0.ok && !ch.blit->pass_timing(p0) && !p0.available, "no pass marks without pass_timing");
        if (r0.ok && r.ok) CHECK(r0.px[0] == r.px[0] && r0.px[1] == r.px[1], "pass_timing does not change the image");
    }
    // 4) options: q:0..2 inside xr_terrain_stage, zpre:0|1 inside xr_cockpit_model
    {
        std::string id, why; T::Conventions c;
        CHECK(T::parse_stage_option("m02a;env:1;q:1", id, c, why) && c.quality == 1, "q:1 parsed");
        CHECK(T::parse_stage_option("m02a;q:2", id, c, why) && c.quality == 2, "q:2 parsed");
        CHECK(T::parse_stage_option("m02a", id, c, why) && c.quality == 0, "default q0");
        CHECK(!T::parse_stage_option("m02a;q:3", id, c, why) && !T::parse_stage_option("m02a;q:x", id, c, why), "q out of range refused");
        K::Options o;
        CHECK(K::parse_options("j35jk_vrcockpit;zpre:1", id, o, why) && o.zpre, "zpre:1 parsed");
        CHECK(K::parse_options("j35jk_vrcockpit", id, o, why) && !o.zpre, "zpre default off");
        CHECK(!K::parse_options("j35jk_vrcockpit;zpre:2", id, o, why), "zpre:2 refused");
    }
}

// ----------------------------------------------------------------- DECOUPLE (decouple:1; docs/world-render/decouple.md)
// The consumer composes EVERY XR frame at its own head pose P1 from producer images rendered at the game frame's pose P0: the eye source (replayed objects)
// is re-projected through H = M0^-1 Qsrc (rotation only), faded at its edges (nothing stretched), the world-window / effects rays rotated by the camera delta.
// CPU reference = ref_eye with the source sampled at reproject_uv (cover x fade) and the world layer through the rotated window rays.
static WorldComposite rotated_window(const WorldComposite& w, const double Q[3][3]) {   // the same plane, every ray of the pixel map turned by Q
    WorldComposite o = w;
    for (int i = 0; i < 3; ++i) {
        o.base[i] = float(Q[i][0] * w.base[0] + Q[i][1] * w.base[1] + Q[i][2] * w.base[2]);
        o.ux[i] = float(Q[i][0] * w.ux[0] + Q[i][1] * w.ux[1] + Q[i][2] * w.ux[2]);
        o.vy[i] = float(Q[i][0] * w.vy[0] + Q[i][1] * w.vy[1] + Q[i][2] * w.vy[2]);
    }
    return o;
}
static Img ref_eye_dc(const Scene& s, Cfg c, int eye, const Depth& d, const Reproject& rp, const WorldComposite& wc1, const double Q[3][3]) {
    Img out(W, H);
    Cfg cw = c; cw.wc = (rp.flags & kReprojWindow) ? rotated_window(wc1, Q) : wc1;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const float u = (float(x) + 0.5f) / float(W), v = (float(y) + 0.5f) / float(H);
        const ReprojectSample rs = reproject_uv(rp, wc1, u, v);
        float t[4]; bilinear(s.eye[eye], rs.u, rs.v, t);
        const Shade sc = shade(t, false, 0.0f, c.tm, c.exposure);
        const int dx = std::min(int(rs.u * float(d.w)), d.w - 1), dy = std::min(int(rs.v * float(d.h)), d.h - 1);
        float cover = d.d[size_t(dy) * d.w + dx] > 0.0f ? 1.0f : 0.0f;
        if (rp.flags & kReprojSource) cover *= rs.fade;
        Shade p{sc.r * cover, sc.g * cover, sc.b * cover, cover};
        if (c.mode >= 2) p = composite_over_world(p, world_layer_ref(s, cw, u, v));
        float* o = out.at(x, y); o[0] = p.r; o[1] = p.g; o[2] = p.b; o[3] = p.a;
    }
    return out;
}
static void case_decouple(Chains& ch, const Scene& s, bool docs) {
    std::printf("[d] DECOUPLE: the XR-frame composite at P1 from P0 images (rotation-only re-projection, camera delta, edge fade, no-op, fresh terrain/cockpit)\n");
    namespace T = acz::terrain; namespace K = acz::cockpit;
    double I[3][3]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) I[i][j] = i == j ? 1.0 : 0.0;
    const double qa = deg2rad(3.0f);
    const double Q[3][3] = {{std::cos(qa), 0, std::sin(qa)}, {0, 1, 0}, {-std::sin(qa), 0, std::cos(qa)}};   // a 3 deg camera turn about the anchor up axis
    const float pos0[3] = {0, 0, 0}, pos1[3] = {0.10f, 0.03f, -0.05f};
    const ViewSetup v0 = make_view("dc_p0", 0.0f, 0.0f, pos0, 0.0f, 0.0f);
    const ViewSetup v1 = make_view("dc_p1", 4.0f, 2.0f, pos1, 0.0f, 0.0f);   // the head turned 4 deg left, 2 deg up and moved 10/3/-5 cm
    WorldComposite w0[2], w1[2];
    CHECK(params_for(v0, w0) && params_for(v1, w1), "dc: view parameters");
    auto band = [](double w) { return float((1.0 / w - 255.0 / 16777216.0) / 1024.0); };
    // the P0 objects image: a cyan replayed-object patch (object band) at pixel (200, 90) in both eyes (the world image has a magenta marker)
    const int px = 200, py = 90, hp = 5;
    Scene so = s;
    for (int e = 0; e < 2; ++e) {
        so.eye[e] = Img(W, H); fill(so.eye[e], 0, 0, W, H, 0, 0, 0, 1.0f); fill(so.eye[e], px - hp, py - hp, px + hp, py + hp, 0.0f, 1.0f, 1.0f, 1.0f); quantise(so.eye[e]);
        so.depth[e] = Depth{}; fill_depth(so.depth[e], px - hp, py - hp, px + hp, py + hp, band(3000.0));
        so.eye_t[e] = make_color(so.eye[e]); so.depth_t[e] = make_depth(so.depth[e]);
    }
    auto centroid = [](const std::vector<uint8_t>& p, float& mu, float& mv) {
        double sx = 0, sy = 0; long n = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { const uint8_t* q = &p[(size_t(y) * W + x) * 4]; if (q[0] < 60 && q[1] > 200 && q[2] > 200) { sx += (x + 0.5) / W; sy += (y + 0.5) / H; ++n; } }
        mu = n ? float(sx / n) : -1.0f; mv = n ? float(sy / n) : -1.0f; return n;
    };
    auto expected = [&](int e, const double Qs[3][3], float& eu, float& ev) {   // the P0 patch direction, turned by Qs^T, seen from P1 (rotation only)
        const float u0 = float(px) / W, v0p = float(py) / H;
        double c0[3]; for (int i = 0; i < 3; ++i) c0[i] = w0[e].base[i] + u0 * w0[e].ux[i] + v0p * w0[e].vy[i];
        float c1[3]; for (int i = 0; i < 3; ++i) c1[i] = float(Qs[0][i] * c0[0] + Qs[1][i] * c0[1] + Qs[2][i] * c0[2]);
        return hand_project(w1[e], c1, eu, ev);
    };
    // (1) rotation-only re-projection (no camera delta): the patch lands on the P1 pixel of its P0 direction; without the re-projection it does not
    // (2) with the camera delta Qsrc = Qfx = Q: the patch follows Q, the world window rays are turned by Q
    for (int qcase = 0; qcase < 2; ++qcase) {
        const double (*qs)[3] = qcase ? Q : I;
        BlitJob j = base_job(so); job_world(j, so, v1, false);
        for (int e = 0; e < 2; ++e) CHECK(reproject_params(w0[e], qs, qs, true, qcase == 1, 0.02f, j.reproject[e]), "dc params (case %d)", qcase);
        BlitJob jn = j; jn.reproject = {};
        const Result r = run_job(ch, j, 3u), rn = run_job(ch, jn, 3u);
        CHECK(r.ok && rn.ok, "dc re-projection jobs ran (case %d)", qcase);
        if (!(r.ok && rn.ok)) continue;
        for (int e = 0; e < 2; ++e) {
            float eu = 0, ev = 0, mu, mv, nu, nv;
            CHECK(expected(e, qs, eu, ev), "dc: expected projection (case %d)", qcase);
            const long n = centroid(r.px[e], mu, mv), nn = centroid(rn.px[e], nu, nv);
            INFO("dc case %d eye%d: patch at (%.4f,%.4f) %ld px, expected (%.4f,%.4f); without re-projection (%.4f,%.4f)", qcase, e, mu, mv, n, eu, ev, nu, nv);
            CHECK(n > 40 && std::fabs(mu - eu) < 0.006f && std::fabs(mv - ev) < 0.008f, "dc case %d eye%d: the re-projected object sits on its P0 direction seen from P1", qcase, e);
            CHECK(std::fabs(nu - eu) > 0.03f, "dc case %d eye%d: the check discriminates (no re-projection: %.4f off)", qcase, e, nu - eu);
            Cfg c = cfg_for(j, e);
            const Img ref = ref_eye_dc(so, c, e, so.depth[e], j.reproject[e], w1[e], qs);
            const Cmp m = compare(r.px[e], ref, 0.01f);
            INFO("%-28s eye%d max_err=%.4f bad=%zu/%zu", qcase ? "d_reproject_camdelta" : "d_reproject", e, m.max_err, m.bad, m.n);
            CHECK(double(m.bad) <= 0.005 * double(m.n), "dc case %d eye%d: %zu pixels differ from the CPU mirror (first at %d,%d)", qcase, e, m.bad, m.fx, m.fy);
            if (e == 0) save_sheet(qcase ? "d_reproject_camdelta" : "d_reproject", r.px[0], ref, docs);
        }
    }
    // (3) edge fade, no stretch: a full-frame object image, the head turned 20 deg: the uncovered side shows the world only, the fade band is partial
    {
        Scene sf = s;
        for (int e = 0; e < 2; ++e) {
            sf.eye[e] = Img(W, H); fill(sf.eye[e], 0, 0, W, H, 0.0f, 0.8f, 0.2f, 1.0f); quantise(sf.eye[e]);
            sf.depth[e] = Depth{}; fill_depth(sf.depth[e], 0, 0, W, H, band(3000.0));
            sf.eye_t[e] = make_color(sf.eye[e]); sf.depth_t[e] = make_depth(sf.depth[e]);
        }
        const ViewSetup v2 = make_view("dc_p2", 20.0f, 0.0f, pos0, 0.0f, 0.0f);
        WorldComposite w2[2]; CHECK(params_for(v2, w2), "dc fade: params");
        BlitJob j = base_job(sf); job_world(j, sf, v2, false);
        for (int e = 0; e < 2; ++e) CHECK(reproject_params(w0[e], I, I, true, false, 0.05f, j.reproject[e]), "dc fade params");
        const Result r = run_job(ch, j, 3u);
        CHECK(r.ok, "dc fade job ran");
        if (r.ok) for (int e = 0; e < 2; ++e) {
            Cfg c = cfg_for(j, e);
            const Img ref = ref_eye_dc(sf, c, e, sf.depth[e], j.reproject[e], w2[e], I);
            const Cmp m = compare(r.px[e], ref, 0.01f);
            size_t partial = 0, world_only = 0, object = 0;
            for (int x = 0; x < W; ++x) {
                const ReprojectSample rs = reproject_uv(j.reproject[e], w2[e], (x + 0.5f) / W, 0.5f);
                if (rs.fade <= 0.0f) ++world_only; else if (rs.fade < 1.0f) ++partial; else ++object;
            }
            INFO("d_edge_fade eye%d max_err=%.4f bad=%zu/%zu; centre row: %zu world-only, %zu fading, %zu object columns", e, m.max_err, m.bad, m.n, world_only, partial, object);
            CHECK(double(m.bad) <= 0.005 * double(m.n), "dc fade eye%d: %zu pixels differ from the CPU mirror (first at %d,%d)", e, m.bad, m.fx, m.fy);
            CHECK(world_only > 20 && partial > 4 && object > 100, "dc fade eye%d: uncovered side, a fade band and the covered part all present", e);
            float p[4]; px_lin(r.px[e], 2, H / 2, p);
            const Shade wl = world_layer_ref(s, c, (2 + 0.5f) / W, 0.5f);
            CHECK(!(p[1] > 0.7f && p[0] < 0.1f) && std::fabs(p[0] - wl.r) < 0.02f, "dc fade eye%d: the uncovered edge is NOT the stretched object colour (%.3f %.3f %.3f)", e, p[0], p[1], p[2]);
            if (e == 0) save_sheet("d_edge_fade", r.px[0], ref, docs);
        }
    }
    // (4) no-op: P1 == P0 -> the consumer sets no flags = the exact previous job (byte-identical by construction: shader path with flags 0); forcing the
    //     source flag with the identity mapping changes at most rounding
    {
        BlitJob a = base_job(so); job_world(a, so, v0, false);
        BlitJob b = a;
        for (int e = 0; e < 2; ++e) CHECK(reproject_params(w0[e], I, I, true, false, 0.0f, b.reproject[e]), "dc identity params");
        BlitJob z = a; z.reproject = {};
        const Result ra = run_job(ch, a, 3u), rb = run_job(ch, b, 3u), rz = run_job(ch, z, 3u);
        CHECK(ra.ok && rb.ok && rz.ok, "dc no-op jobs ran");
        if (ra.ok && rb.ok && rz.ok) {
            CHECK(ra.px[0] == rz.px[0] && ra.px[1] == rz.px[1], "dc no-op: flags 0 = byte-identical to the job without the field");
            int worst = 0; size_t diff = 0;
            for (int e = 0; e < 2; ++e) for (size_t i = 0; i < ra.px[e].size(); ++i) { const int dd = std::abs(int(ra.px[e][i]) - int(rb.px[e][i])); worst = std::max(worst, dd); diff += dd != 0; }
            INFO("dc identity mapping forced on: %zu bytes differ, max %d/255", diff, worst);
            CHECK(worst <= 1, "dc identity mapping: rounding only (max %d/255)", worst);
        }
    }
    // (5) fresh terrain + model cockpit at P1: the composite with the re-projection on equals the plain P1 composite outside the object band (here: no objects),
    //     and differs from the P0 composite (the head turned and moved: the cockpit and terrain moved with P1, not with the images' pose)
    {
        const float fwd_xz[2] = {0.0f, -1.0f}, eye_xz[2] = {0.0f, 0.0f}; float centres[4];
        T::StageData st = orient_stage(fwd_xz, eye_xz, 6000.0f, 1500.0f, 1, centres);
        T::Atlas at; T::build_atlas(st, at);
        T::Conventions conv; T::CamFrame cf{};
        const float cam[3] = {0.0f, 2400.0f, 0.0f};
        T::camera_frame(cam, -0.3f, 0.0f, 0.0f, conv, cf);
        const K::ModelData model = K::make_synthetic_model(); K::Options opt; opt.interior = false; K::Mesh mesh; std::string why;
        CHECK(K::build_mesh(model, opt, mesh, why), "dc: synthetic cockpit %s", why.c_str());
        auto job_at = [&](const ViewSetup& v, const WorldComposite* wsrc, bool cockpit = true) {
            BlitJob j = base_job(s, true); job_world(j, s, v, false);
            j.terrain = true; j.terrain_stage = &st; j.terrain_atlas = &at; j.terrain_gen = 50001;
            for (int e = 0; e < 2; ++e) {
                T::eye_world(cf, j.world_eye[e].origin, conv.head_units_per_metre, j.terrain_eye[e].eye);
                for (int i = 0; i < 3; ++i) { j.terrain_eye[e].right[i] = cf.right[i]; j.terrain_eye[e].up[i] = cf.up[i]; j.terrain_eye[e].fwd[i] = cf.fwd[i]; }
                const float fov[4] = {v.eye[e].fov.left, v.eye[e].fov.right, v.eye[e].fov.up, v.eye[e].fov.down};
                K::EyeCam kc{}; K::eye_camera(v.eye[e].pose.orientation, v.eye[e].pose.position, fov, v.anchor, v.yaw, mesh.eye, opt.upm, opt.near_u, kc);
                j.cockpit_eye[e] = K::make_constants(kc, opt, j.exposure, uint32_t(j.tonemap));
                if (wsrc) reproject_params(wsrc[e], I, I, true, false, 0.02f, j.reproject[e]);
            }
            j.cockpit = cockpit; j.cockpit_mesh = &mesh; j.cockpit_gen = 50002;
            return j;
        };
        const Result p1dc = run_job(ch, job_at(v1, w0), 3u), p1 = run_job(ch, job_at(v1, nullptr), 3u), p0 = run_job(ch, job_at(v0, nullptr), 3u);
        CHECK(p1dc.ok && p1.ok && p0.ok && p1dc.facts.cockpit_used && p1dc.facts.terrain_used, "dc terrain+cockpit jobs ran");
        if (p1dc.ok && p1.ok && p0.ok) {
            CHECK(p1dc.px[0] == p1.px[0] && p1dc.px[1] == p1.px[1], "dc: terrain + model cockpit at P1 are rendered fresh at P1 (identical to the plain P1 composite)");
            size_t moved = 0; for (size_t i = 0; i < p1.px[0].size(); ++i) moved += p1.px[0][i] != p0.px[0][i];
            INFO("dc: P1 vs P0 composite: %zu of %zu bytes differ (head 4 deg / 2 deg / 10 cm)", moved, p1.px[0].size());
            CHECK(moved > p1.px[0].size() / 10, "dc: the P1 composite is not the P0 image");
            float u1, v1c, u0, v0c; long n1 = 0, n0 = 0;
            auto red = [&](const std::vector<uint8_t>& px, float& mu, float& mv) {
                double sx = 0, sy = 0; long n = 0;
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { const uint8_t* q = &px[(size_t(y) * W + x) * 4]; if (q[0] > 150 && q[0] > q[1] + 80 && q[0] > q[2] + 80) { sx += (x + 0.5) / W; sy += (y + 0.5) / H; ++n; } }
                mu = n ? float(sx / n) : -1; mv = n ? float(sy / n) : -1; return n;
            };
            const Result t1 = run_job(ch, job_at(v1, w0, false), 3u), t0 = run_job(ch, job_at(v0, nullptr, false), 3u);   // terrain only (no cockpit in front of the landmark)
            CHECK(t1.ok && t0.ok, "dc terrain-only jobs ran");
            n1 = red(t1.px[0], u1, v1c); n0 = red(t0.px[0], u0, v0c);
            INFO("dc: red landmark P0 (%.4f,%.4f) %ld px, P1 (%.4f,%.4f) %ld px", u0, v0c, n0, u1, v1c, n1);
            CHECK(n0 > 3 && n1 > 3 && u1 > u0 + 0.02f, "dc: the head turned left: the terrain landmark moved right in the P1 composite");
            save_sheet("d_p1_terrain_cockpit", p1dc.px[0], [&] { Img im(W, H); for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float q[4]; px_lin(p0.px[0], x, y, q); for (int k = 0; k < 4; ++k) im.at(x, y)[k] = q[k]; } return im; }(), docs);
        }
    }
}

// ----------------------------------------------------------------- STAGE-FILE probe (manual, not in ctest): a REAL baked stage (.zvrs) through the REAL shader at a fixed
// pose with an xr_terrain_stage option string, exactly as xr_stereo_output builds the job (atlas, dtex, env/look texture, haze, quality). Writes eye-0 PNGs of the GPU image
// and the CPU trace_env reference for a set of variants. Usage: --stage <file.zvrs> --stage-opts "<id>;env:1;look:1;...;cam:x,y,z,yaw,pitch" [--stage-w 960 --stage-h 540]
static std::string g_stage, g_stage_opts;
static int g_stage_w = 960, g_stage_h = 540;
static void write_rgba_png(const std::string& path, const std::vector<uint8_t>& px, int w, int h) {
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i) for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = px[i * 4 + c];
    write_png_rgb(path, rgb, w, h);
}
static int run_stage_probe(const Scene& s) {
    namespace T = acz::terrain;
    std::string id, why; T::Conventions conv;
    if (!T::parse_stage_option(g_stage_opts, id, conv, why)) { std::printf("stage-opts refused: %s\n", why.c_str()); return 2; }
    T::StageData st;
    const T::LoadStatus ls = T::load_stage_file(g_stage, st);
    if (ls != T::LoadStatus::ok) { std::printf("load %s: %s\n", g_stage.c_str(), T::status_name(ls)); return 2; }
    T::apply_sea_convention(st, conv);
    T::Atlas at; T::build_atlas(st, at, conv.clut_raw);
    T::Atlas at_dt = at;
    if (conv.detail) { std::string dw; T::attach_detail(st.id, conv, at_dt, dw); std::printf("dtex: %s\n", dw.c_str()); }
    std::vector<float> env; if (conv.env_bits) T::build_env_texture(st, conv, env);
    std::printf("stage %s tiles=%u env=%zu bits=%u look=%d haze_l=%.0f far=%.0f q=%u cam=%.0f,%.0f,%.0f,%.1f,%.1f\n", st.id, st.ntiles, env.size(), conv.env_bits,
        int(conv.look), double(conv.haze_l), double(conv.far_dist), conv.quality, double(conv.cam[0]), double(conv.cam[1]), double(conv.cam[2]), double(conv.cam[3]), double(conv.cam[4]));
    const int ow = g_stage_w, oh = g_stage_h;
    auto chp = make_chains(rgba8_unorm, ow, oh);
    if (!chp->ok) { std::printf("chains failed\n"); return 1; }
    const float origin[3] = {0, 0, 0};
    const ViewSetup v = make_view("stage", 0.0f, 0.0f, origin, 0.0f, 0);
    T::CamFrame cf{};
    const float campos[3] = {conv.cam[0], conv.cam[1], conv.cam[2]};
    T::camera_frame(campos, deg2rad(conv.cam[4]), deg2rad(conv.cam[3]), deg2rad(conv.cam[5]), conv, cf);
    auto make = [&](uint64_t gen, const T::Atlas* atlas, const std::vector<float>* e, uint32_t bits) {
        BlitJob j = base_job(s, true, ow, oh); job_world(j, s, v, false);
        j.terrain = true; j.terrain_blend = conv.blend_window; j.terrain_stage = &st; j.terrain_atlas = atlas; j.terrain_gen = gen;
        if (e && !e->empty() && bits) { j.terrain_env = e; j.terrain_env_bits = bits; }
        j.terrain_haze_l = conv.haze_l; j.terrain_far = conv.far_dist; j.terrain_quality = conv.quality;
        for (int k = 0; k < 2; ++k) {
            T::eye_world(cf, j.world_eye[k].origin, conv.head_units_per_metre, j.terrain_eye[k].eye);
            for (int i = 0; i < 3; ++i) { j.terrain_eye[k].right[i] = cf.right[i]; j.terrain_eye[k].up[i] = cf.up[i]; j.terrain_eye[k].fwd[i] = cf.fwd[i]; }
        }
        return j;
    };
    CreateDirectoryA(g_out.c_str(), nullptr);
    auto shot = [&](const char* name, const BlitJob& j) {
        Result r = run_job(*chp, j, 1u);
        if (!r.ok) { std::printf("%s: run failed\n", name); return; }
        write_rgba_png(g_out + "\\" + name + ".png", r.px[0], ow, oh);
        double m[3] = {0, 0, 0};
        for (size_t i = 0; i < size_t(ow) * oh; ++i) for (int c = 0; c < 3; ++c) m[c] += r.px[0][i * 4 + c];
        std::printf("%-24s used=%d mean=%.1f,%.1f,%.1f\n", name, int(r.facts.terrain_used), m[0] / (ow * oh), m[1] / (ow * oh), m[2] / (ow * oh));
    };
    auto cpu = [&](const char* name, const BlitJob& j, const T::Atlas& atlas) {   // CPU trace_env (no detail term) at the same pixels
        const WorldComposite& wc = j.world_eye[0]; const TerrainEye& te = j.terrain_eye[0];
        const T::EnvView ev{j.terrain_env ? j.terrain_env->data() : nullptr, j.terrain_env ? j.terrain_env_bits : 0u};
        std::vector<uint8_t> px(size_t(ow) * oh * 4, 255);
        for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) {
            const float u = (float(x) + 0.5f) / float(ow), vv = (float(y) + 0.5f) / float(oh);
            float d[3], dw[3];
            for (int i = 0; i < 3; ++i) d[i] = wc.base[i] + u * wc.ux[i] + vv * wc.vy[i];
            for (int i = 0; i < 3; ++i) dw[i] = te.right[i] * d[0] + te.up[i] * d[1] + te.fwd[i] * (-d[2]);
            const float l = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
            for (int i = 0; i < 3; ++i) dw[i] /= l;
            const T::Trace tr = T::trace_env(st, atlas, ev, te.eye, dw, j.terrain_haze_l, j.terrain_far);
            for (int c = 0; c < 3; ++c) px[(size_t(y) * ow + x) * 4 + c] = uint8_t(std::lround(srgb_enc(sat(tr.rgb[c])) * 255.0f));
        }
        write_rgba_png(g_out + "\\" + name + ".png", px, ow, oh);
        std::printf("%-24s (cpu)\n", name);
    };
    const T::Atlas* base_atlas = conv.detail ? &at_dt : &at;
    uint64_t gen = 100;
    const BlitJob jday = make(gen++, base_atlas, &env, conv.env_bits);
    shot("p_day", jday);
    shot("p_nodtex", make(gen++, &at, &env, conv.env_bits));
    shot("p_env0", make(gen++, &at, &env, 0u));
    { T::Conventions c2 = conv; c2.look = false; std::vector<float> e2; T::build_env_texture(st, c2, e2); shot("p_look0", make(gen++, &at, &e2, conv.env_bits)); }
    shot("p_envsky", make(gen++, &at, &env, T::kEnvSky));
    cpu("c_day", make(gen++, &at, &env, conv.env_bits), at);
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: a crash must not swallow the progress lines
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--out")) g_out = argv[i + 1];
        if (!std::strcmp(argv[i], "--docs")) g_docs = argv[i + 1];
        if (!std::strcmp(argv[i], "--stage")) g_stage = argv[i + 1];
        if (!std::strcmp(argv[i], "--stage-opts")) g_stage_opts = argv[i + 1];
        if (!std::strcmp(argv[i], "--stage-w")) g_stage_w = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--stage-h")) g_stage_h = std::atoi(argv[i + 1]);
    }
    std::string why;
    if (!g.init(why)) { std::printf("SKIP xr_composite_warp_harness: WARP D3D12 device unavailable (%s); no GPU assertions were executed.\n", why.c_str()); return 0; }
    if (!g_stage.empty()) { Scene s; build_scene(s); return run_stage_probe(s); }
    auto ch28 = make_chains(rgba8_unorm);
    if (!ch28->ok) { std::printf("FAIL: real BlitBackend setup/bind on WARP failed\n"); return 1; }
    auto ch29 = make_chains(rgba8_srgb);
    CHECK(ch29->ok, "format-29 chains");
    Scene s; build_scene(s);
    const bool docs = !g_docs.empty();
    case_mode0(*ch28, s);
    if (ch29->ok) case_srgb(*ch28, *ch29, s);
    case_world(*ch28, s, docs);
    case_effects(*ch28, s, docs);
    case_effects_dual_cpu();
    case_effects_dual(*ch28, s, docs);
    case_sprite(*ch28, s);   // EFFECTS-OUTSIDE (F) fxmem:6
    case_hold_extend(*ch28, s, docs);
    case_debug_png(*ch28, s, docs);
    case_hud_markers(*ch28, s);
    case_radar_bg(*ch28, s);
    case_depth_formats(*ch28, s);
    case_special_values(*ch28, s);
    case_downscale(s);
    case_occlude(*ch28, s);
    case_world_filter(*ch28, s);   // WORLD-WINDOW FILTER (eyeres)
    case_world_cache_cpu();
    case_terrain(*ch28, s, docs);
    case_terrain_detail(*ch28, s, docs);
    case_terrain_orientation(*ch28, s);
    case_props(*ch28, s, docs);   // WORLD-3
    case_terrain_roll(*ch28, s);
    case_terrain_occlusion(*ch28, s);
    case_object_interp(*ch28, s);
    case_terrain_env(*ch28, s, docs);
    case_cockpit_model(*ch28, s, docs);
    case_gpu_cost(*ch28, s, docs);   // GPU-COST
    case_decouple(*ch28, s, docs);   // DECOUPLE
    {
        auto cc = make_chains(rgba8_unorm);
        if (cc->ok) { case_world_cache(*cc, s, docs); cc->blit->destroy(); } else CHECK(false, "world-cache chains");
    }
    ch28->blit->destroy(); ch29->blit->destroy();
    std::printf("\nxr_composite_warp_harness: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
