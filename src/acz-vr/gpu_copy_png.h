#pragma once
// J-hud-colour: gpu_copy_step5_png_once (default OFF), SDK-free helpers shared by the step-5
// producer (gpu_copy_step5_hooks.inl) and the offline fixtures.
//
// Once per process (two samples) the producer copies three of its own private FP16 images
// into one READBACK buffer at the END of a frame recording (just before the game's Close):
//   0 hud  : the slot's private HUD target after the replay (what the consumer blits, mode 1)
//   1 world: the slot's world snapshot (what the consumer blits into the world quad)
//   2 eye0 : the slot's eye-0 cockpit image (what the consumer blits into eye 0)
// and, after the private fence proves completion, the worker (no producer lock) writes
// 8-bit PNG files and one statistics line per image.
//
// Colour order (documented, no swizzle anywhere): the sources are DXGI 10
// R16G16B16A16_FLOAT, texel = 4 IEEE halves in the order R, G, B, A (byte 0..1 = R). Every
// PNG is colour type 6 (RGBA, 8 bit), byte order R, G, B, A, row 0 = the TOP row of the D3D12
// texture (texture origin top-left, as the OpenXR D3D12 swapchain images). The conversion is
// the CONSUMER'S own (xr_stereo::shade / shade_hud with its production settings: tonemap
// clamp, exposure 1; the sRGB swapchain RTV encodes linear once, here srgb8()):
//   hud-on-black   : sRGB(shade_hud(hud).rgb), alpha 255 (the premultiplied HUD over black)
//   hud-alpha-raw  : the STORED alpha channel, linear clamp01 x 255 as grey, alpha 255
//   hud-over-world : sRGB(shade_hud(hud).rgb + shade(world).rgb * (1 - shade_hud(hud).a)),
//                    alpha 255: the HUD quad composited over the world quad in linear light
//                    (what a compositor shows where both layers overlap, without feather)
//   world          : sRGB(shade(world).rgb), alpha 255
//   eye0           : sRGB(shade(eye0).rgb), alpha 255 (colour only; the depth coverage
//                    alpha of the projection layer is not read back)
#include "gpu_copy_step3.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace acz::gpu_copy::png {
enum Image : unsigned {image_hud=0,image_world=1,image_eye0=2,image_count=3,
                       image_effects=3,image_count_fx=4,
                       // J-scene-end: a 5th footprint (the game's scene render target at the START of the HUD region, copied directly into the readback
                       // only for file-triggered burst sets; the readback is sized for it whenever it is effects-capable).
                       image_scene_end=4,image_count_se=5};
inline const char* image_name(unsigned i) noexcept {
    static constexpr const char* n[]={"hud","world","eye0","effects","scene-end"};return i<image_count_se?n[i]:"?";
}
enum Output : unsigned {out_hud_on_black=0,out_hud_alpha_raw=1,out_hud_over_world=2,out_world=3,out_eye0=4,out_count=5,
                        // J-effects-content (effects image present): the effects target over the world (the consumer's mode-3 maths),
                        // premultiplied rgb on black, the stored alpha as grey, and a mask (white = any channel non-zero).
                        out_effects=5,out_effects_on_black=6,out_effects_alpha_raw=7,out_effects_nonzero=8,out_count_fx=9,
                        // J-scene-end: the scene render target after all scene draws (cockpit included), sRGB(shade(scene)).
                        out_scene_end=9,out_count_se=10};
inline const char* output_name(unsigned i) noexcept {
    static constexpr const char* n[]={"hud-on-black","hud-alpha-raw","hud-over-world","world","eye0",
                                      "effects","effects-on-black","effects-alpha-raw","effects-nonzero","scene-end"};return i<out_count_se?n[i]:"?";
}
constexpr unsigned samples=2;            // two samples per process
constexpr unsigned second_gap_frames=10; // the second sample: >= 10 published frames after the first was read
constexpr uint32_t fp16_format=10;       // DXGI_FORMAT_R16G16B16A16_FLOAT, the only accepted source format
constexpr unsigned commands_per_sample=3*image_count; // per image: Barrier(RT->COPY_SOURCE), Copy, Barrier(back)
// D3D12 placed-footprint rules: row pitch a multiple of 256 B, footprint offsets of 512 B.
inline uint64_t row_pitch(uint32_t w) noexcept {return (uint64_t(w)*8+255)/256*256;}
inline uint64_t footprint_stride(uint32_t w,uint32_t h) noexcept {return (row_pitch(w)*h+511)/512*512;}
inline uint64_t readback_bytes(uint32_t w,uint32_t h) noexcept {return footprint_stride(w,h)*image_count;}
inline uint64_t readback_bytes_fx(uint32_t w,uint32_t h) noexcept {return footprint_stride(w,h)*image_count_fx;}
inline uint64_t readback_bytes_se(uint32_t w,uint32_t h) noexcept {return footprint_stride(w,h)*image_count_se;}
// J-effects-content: after the base samples, up to `burst_samples` more sets are taken at the first frame whose COMMITTED effects draws reach
// the threshold (default 6; file %LOCALAPPDATA%\acz-vr\png-effects.threshold holds a decimal 1..96), at least `burst_gap_frames` published frames apart.
constexpr unsigned burst_samples=3,burst_gap_frames=30,threshold_default=6,threshold_max=96;
// Parse the threshold file text: optional whitespace, 1..4 decimal digits, optional trailing whitespace; 0 = invalid / out of range.
inline unsigned parse_threshold(const char* t,size_t n) noexcept {
    auto ws=[](char c){return c==' '||c=='\t'||c=='\r'||c=='\n';};
    size_t i=0;while(i<n&&ws(t[i]))++i;
    if(i>=n||t[i]<'0'||t[i]>'9')return 0;
    unsigned v=0;size_t d=0;
    while(i<n&&t[i]>='0'&&t[i]<='9'){v=v*10+unsigned(t[i]-'0');++i;if(++d>4)return 0;}
    while(i<n&&ws(t[i]))++i;
    if(i!=n||v<1||v>threshold_max)return 0;
    return v;
}

// Raw statistics over ALL texels of one FP16 image (not only covered ones): per channel
// min/max/mean of the finite values, texels with any non-finite channel, texels with
// alpha > 0, texels with max(r,g,b) > 0.
struct Stats {uint64_t texels=0,nonfinite=0,alpha_positive=0,lit=0;double min[4]{},max[4]{},mean[4]{};};
inline void texel(const uint8_t* p,float c[4]) noexcept {
    for(int i=0;i<4;++i){uint16_t v;std::memcpy(&v,p+2*i,2);c[i]=step3::half_to_float(v);}
}
inline void stats(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,Stats& out) noexcept {
    out=Stats{};double sum[4]{};uint64_t finite[4]{};
    for(int c=0;c<4;++c){out.min[c]=1e300;out.max[c]=-1e300;}
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        float c[4];texel(base+uint64_t(y)*pitch+uint64_t(x)*8,c);++out.texels;
        bool bad=false;
        for(int i=0;i<4;++i) {
            if(!std::isfinite(c[i])){bad=true;continue;}
            ++finite[i];sum[i]+=c[i];if(c[i]<out.min[i])out.min[i]=c[i];if(c[i]>out.max[i])out.max[i]=c[i];
        }
        out.nonfinite+=bad;
        out.alpha_positive+=c[3]>0.0f; // NaN compares false
        out.lit+=c[0]>0.0f||c[1]>0.0f||c[2]>0.0f;
    }
    for(int i=0;i<4;++i){if(finite[i])out.mean[i]=sum[i]/double(finite[i]);else out.min[i]=out.max[i]=0;}
}
// J-effects-content: coverage statistics of the effects target. non-zero = any channel (r,g,b,a) != 0 or non-finite; the bounding box (inclusive,
// -1 when empty) covers those texels; alpha statistics are over the finite alpha values of ALL texels; rgb_max over the finite colour values.
struct FxStats {uint64_t texels=0,nonzero=0,alpha_positive=0,rgb_positive=0,nonfinite=0;double alpha_max=0,alpha_mean=0,rgb_max=0;int x0=-1,y0=-1,x1=-1,y1=-1;};
inline void fx_stats(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,FxStats& out) noexcept {
    out=FxStats{};double sum=0,amax=0;uint64_t finiteA=0;bool any=false;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        float c[4];texel(base+uint64_t(y)*pitch+uint64_t(x)*8,c);++out.texels;
        bool nz=false,bad=false;
        for(int i=0;i<4;++i){if(!std::isfinite(c[i])){bad=true;nz=true;} else if(c[i]!=0.0f)nz=true;}
        out.nonfinite+=bad;
        if(std::isfinite(c[3])){if(!finiteA||c[3]>amax)amax=c[3];++finiteA;sum+=c[3];}
        out.alpha_positive+=c[3]>0.0f;
        out.rgb_positive+=c[0]>0.0f||c[1]>0.0f||c[2]>0.0f;
        for(int i=0;i<3;++i)if(std::isfinite(c[i])&&c[i]>out.rgb_max)out.rgb_max=c[i];
        if(nz) {
            ++out.nonzero;
            if(!any){any=true;out.x0=out.x1=int(x);out.y0=out.y1=int(y);}
            else{if(int(x)<out.x0)out.x0=int(x);if(int(x)>out.x1)out.x1=int(x);out.y1=int(y);}
        }
    }
    out.alpha_max=finiteA?amax:0.0;out.alpha_mean=finiteA?sum/double(finiteA):0.0;
}
// J-effects-smoke-fix: the same statistics restricted to an inclusive box (clamped to the image; an empty / inverted box gives zeros). rgb_max / alpha_max over the
// finite values, nonzero = any channel != 0 or non-finite, `area` = texels in the box.
struct RegionStats {uint64_t area=0,nonzero=0;double alpha_max=0,rgb_max=0;};
inline void region_stats(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,int x0,int y0,int x1,int y1,RegionStats& out) noexcept {
    out=RegionStats{};
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>=int(w))x1=int(w)-1;if(y1>=int(h))y1=int(h)-1;
    if(x1<x0||y1<y0)return;
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
        float c[4];texel(base+uint64_t(y)*pitch+uint64_t(x)*8,c);++out.area;
        bool nz=false;
        for(int i=0;i<4;++i)if(!std::isfinite(c[i])||c[i]!=0.0f)nz=true;
        out.nonzero+=nz;
        if(std::isfinite(c[3])&&c[3]>out.alpha_max)out.alpha_max=c[3];
        for(int i=0;i<3;++i)if(std::isfinite(c[i])&&c[i]>out.rgb_max)out.rgb_max=c[i];
    }
}
// J-scene-end: where the shaded (clamp, exposure 1) colour of two images differs by more than `eps` in any channel (non-finite = different).
// Count and inclusive bounding box (-1 when empty).
struct DiffStats {uint64_t texels=0,changed=0;int x0=-1,y0=-1,x1=-1,y1=-1;};
inline void diff_stats(const uint8_t* a,const uint8_t* b,uint32_t w,uint32_t h,uint64_t pitch,float eps,DiffStats& out) noexcept {
    out=DiffStats{};
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        float ca[4],cb[4];texel(a+uint64_t(y)*pitch+uint64_t(x)*8,ca);texel(b+uint64_t(y)*pitch+uint64_t(x)*8,cb);++out.texels;
        bool d=false;
        for(int i=0;i<3;++i) {
            if(!std::isfinite(ca[i])||!std::isfinite(cb[i])){d=d||!(ca[i]==cb[i]||(!std::isfinite(ca[i])&&!std::isfinite(cb[i])));continue;}
            const float va=ca[i]>0.0f?(ca[i]<1.0f?ca[i]:1.0f):0.0f,vb=cb[i]>0.0f?(cb[i]<1.0f?cb[i]:1.0f):0.0f;
            const float df=va>vb?va-vb:vb-va;d=d||df>eps;
        }
        if(!d)continue;
        ++out.changed;
        if(out.x0<0){out.x0=out.x1=int(x);out.y0=out.y1=int(y);}
        else{if(int(x)<out.x0)out.x0=int(x);if(int(x)>out.x1)out.x1=int(x);out.y1=int(y);}
    }
}
// Bit-exact mirrors of xr_stereo::shade (use_depth false) and xr_stereo::shade_hud
// (xr_stereo_output.cpp; cross-checked by xr_stereo_output_offline_tests). tonemap 0 = clamp,
// 1 = reinhard. The producer uses the consumer's production settings (clamp, exposure 1).
inline void shade_rgb(const float in[3],unsigned tonemap,float exposure,float out[3]) noexcept {
    for(int i=0;i<3;++i) {
        float v=in[i]*exposure;
        v=v>0.0f?v:0.0f;
        out[i]=tonemap==1?v/(1.0f+v):(v<1.0f?v:1.0f);
    }
}
inline void shade_hud(const float in[4],unsigned tonemap,float exposure,float out[4]) noexcept {
    const bool display=in[3]<0.0f; // HUD-LATE display marker (hud_late.h): alpha < 0 = display value, no exposure / tone map
    if(display)shade_rgb(in,0,1.0f,out);else shade_rgb(in,tonemap,exposure,out);
    float a=display?(-in[3]<1.0f?-in[3]:1.0f):(in[3]>0.0f?in[3]:0.0f);
    const float m=out[0]>out[1]?(out[0]>out[2]?out[0]:out[2]):(out[1]>out[2]?out[1]:out[2]);
    a=a>m?a:m;
    out[3]=a<1.0f?a:1.0f;
}
inline uint8_t unorm8(float v) noexcept {return step3::unorm8(v);}
// One RGBA8 output (tight rows, w*4 bytes) from the three read-back images (FP16, `pitch`).
// `images` = how many source images `src` points at (3, or 4 with the effects image: only then are the effects outputs valid).
inline bool build(const uint8_t* const* src,uint32_t w,uint32_t h,uint64_t pitch,unsigned which,
                  std::vector<uint8_t>& out,unsigned images=image_count) noexcept {
    try {
        if(which>=(images>=image_count_se?out_count_se:images>=image_count_fx?out_count_fx:out_count)||!w||!h||pitch<uint64_t(w)*8)return false;
        out.assign(size_t(w)*h*4,0);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
            const uint64_t at=uint64_t(y)*pitch+uint64_t(x)*8;
            uint8_t* o=&out[(size_t(y)*w+x)*4];
            float c[4]{},s[4]{};
            switch(which) {
            case out_hud_on_black:texel(src[image_hud]+at,c);shade_hud(c,0,1.0f,s);
                o[0]=step3::srgb8(s[0]);o[1]=step3::srgb8(s[1]);o[2]=step3::srgb8(s[2]);break;
            case out_hud_alpha_raw:texel(src[image_hud]+at,c);o[0]=o[1]=o[2]=unorm8(c[3]);break;
            case out_hud_over_world: {
                texel(src[image_hud]+at,c);shade_hud(c,0,1.0f,s);
                float wc[4]{},ws[3]{};texel(src[image_world]+at,wc);shade_rgb(wc,0,1.0f,ws);
                for(int i=0;i<3;++i)o[i]=step3::srgb8(s[i]+ws[i]*(1.0f-s[3]));
                break;}
            case out_world:texel(src[image_world]+at,c);shade_rgb(c,0,1.0f,s);
                o[0]=step3::srgb8(s[0]);o[1]=step3::srgb8(s[1]);o[2]=step3::srgb8(s[2]);break;
            case out_effects: { // the effects layer over the world quad: the consumer's mode 3 (HUD shade, alpha = max(a, max rgb))
                texel(src[image_effects]+at,c);shade_hud(c,0,1.0f,s);
                float wc[4]{},ws[3]{};texel(src[image_world]+at,wc);shade_rgb(wc,0,1.0f,ws);
                for(int i=0;i<3;++i)o[i]=step3::srgb8(s[i]+ws[i]*(1.0f-s[3]));
                break;}
            case out_effects_on_black:texel(src[image_effects]+at,c);shade_hud(c,0,1.0f,s);
                o[0]=step3::srgb8(s[0]);o[1]=step3::srgb8(s[1]);o[2]=step3::srgb8(s[2]);break;
            case out_effects_alpha_raw:texel(src[image_effects]+at,c);o[0]=o[1]=o[2]=unorm8(c[3]);break;
            case out_effects_nonzero: {
                texel(src[image_effects]+at,c);
                bool nz=false;for(int i=0;i<4;++i)nz=nz||!std::isfinite(c[i])||c[i]!=0.0f;
                o[0]=o[1]=o[2]=nz?255:0;break;}
            case out_scene_end:texel(src[image_scene_end]+at,c);shade_rgb(c,0,1.0f,s);
                o[0]=step3::srgb8(s[0]);o[1]=step3::srgb8(s[1]);o[2]=step3::srgb8(s[2]);break;
            default:texel(src[image_eye0]+at,c);shade_rgb(c,0,1.0f,s);
                o[0]=step3::srgb8(s[0]);o[1]=step3::srgb8(s[1]);o[2]=step3::srgb8(s[2]);break;
            }
            o[3]=255;
        }
        return true;
    } catch(...) {out.clear();return false;}
}
}  // namespace acz::gpu_copy::png
