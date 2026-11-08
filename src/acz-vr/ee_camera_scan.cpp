// J-w1-ee-camera: relation maths, E6 hex dumps and the relative-geometry scan
// (worker thread in production; synchronous in the fixtures). SDK-free.
#include "ee_camera_probe.h"
#include <cstdio>

namespace acz::ee_camera {
namespace {
template<size_t N,class... A> void appendf(char (&buf)[N],size_t& len,const char* fmt,A... a) noexcept {
    if(len>=N-1)return;
    const int w=std::snprintf(buf+len,N-len,fmt,a...);
    if(w>0)len=len+size_t(w)<N?len+size_t(w):N-1;
}
inline double fast_wrap(double r) noexcept {return std::fabs(r)<=pi?r:wrap(r);}
const char* mask_text(uint32_t m,char (&b)[64]) noexcept {
    size_t n=0;b[0]=0;
    static constexpr const char* names[]={"sane","yaw","vert","horiz","eye","degrees"};
    for(unsigned i=0;i<6;++i)if(m&(1u<<i))appendf(b,n,"%s%s",n?"+":"",names[i]);
    if(!n)appendf(b,n,"none");
    return b;
}
const char* fail_text(uint32_t m,char (&b)[64]) noexcept {
    size_t n=0;b[0]=0;
    static constexpr uint32_t need[]={rel_sane,rel_yaw,rel_vert,rel_horiz};
    static constexpr const char* names[]={"sane","yaw","vert","horiz"};
    for(unsigned i=0;i<4;++i)if(!(m&need[i]))appendf(b,n,"%s%s",n?"+":"",names[i]);
    if(!n)appendf(b,n,"none");
    return b;
}
}

uint32_t relations(const float* v,const Basis* basis,double extra_yaw,double slack,RelFacts* facts) noexcept {
    uint32_t m=0;RelFacts f;
    bool finite=true;
    for(unsigned i=0;i<field_count;++i)if(i!=look_yaw_copy&&!std::isfinite(v[i])){finite=false;break;}
    const double L=v[look_yaw],H=v[heading],C=v[cam_yaw];
    const double ax=v[air_x],ay=v[air_y],az=v[air_z],cx=v[cam_x],cy=v[cam_y],cz=v[cam_z];
    if(finite) {
        const bool aircraft=std::fabs(ax)+std::fabs(az)>1.0;
        bool sane=aircraft&&std::fabs(ax)<=1e7&&std::fabs(az)<=1e7&&std::fabs(cx)<=1e7&&std::fabs(cz)<=1e7&&ay>=-2000&&ay<=200000;
        for(unsigned a:{look_pitch,look_yaw,look_roll,heading,cam_pitch,cam_yaw,cam_roll})sane=sane&&std::fabs(v[a])<=2*pi+1;
        if(sane)m|=rel_sane;
        const bool nondeg=std::fabs(H)+std::fabs(L)+std::fabs(C)>1e-4;
        f.yaw_res=fast_wrap(C-(H+L));
        if(nondeg&&std::fabs(f.yaw_res)<=2e-3+extra_yaw)m|=rel_yaw;
        f.vert=cy-ay;
        if((std::fabs(ay)+std::fabs(cy))>0&&f.vert>=1.0-slack&&f.vert<=8.0+slack)m|=rel_vert;
        f.horiz=std::hypot(cx-ax,cz-az);
        if(aircraft&&f.horiz>=20.0-4*slack&&f.horiz<=60.0+4*slack)m|=rel_horiz;
        if(basis) {
            const double en=std::sqrt(basis->eye[0]*basis->eye[0]+basis->eye[1]*basis->eye[1]+basis->eye[2]*basis->eye[2]);
            f.eye_err=1e30;
            for(double s:{16.0,1.0,1.0/16}) {
                const double d[3]={s*(cx-ax)-basis->eye[0],s*(cy-ay)-basis->eye[1],s*(cz-az)-basis->eye[2]};
                const double e=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
                if(e<f.eye_err){f.eye_err=e;f.eye_scale=s;}
            }
            if(aircraft&&en>0&&f.eye_err<=0.05*en+0.5)m|=rel_eye;
        }
        const double mx=std::fmax(std::fabs(H),std::fmax(std::fabs(L),std::fabs(C)));
        if(nondeg&&mx>2*pi+1&&mx<=361&&std::fabs(v[cam_pitch])<=361&&std::fabs(v[cam_roll])<=361&&std::fabs(wrap_deg(C-(H+L)))<=0.12)
            m|=rel_degrees;
    }
    if(facts)*facts=f;
    return m;
}

void dump_e6(Memory& mem,uint64_t base,Sink& log,const char* tag) noexcept {
    static constexpr uint32_t windows[]={0xFDD400,0xFDD440,0xFDD850,0xFDD880,0x312E20};
    for(uint32_t off:windows) {
        char t[448];size_t n=0;
        appendf(t,n,"ee-camera;event=hexdump;tag=%s;base=%llx;off=+%X;",tag,static_cast<unsigned long long>(base),off);
        uint8_t b[64];
        if(!mem.read(base+off,b,64)){appendf(t,n,"unreadable");log.line(false,t);continue;}
        appendf(t,n,"hex=");for(uint8_t x:b)appendf(t,n,"%02x",x);
        appendf(t,n,";f=");
        for(int i=0;i<16;++i){float f;std::memcpy(&f,b+4*i,4);appendf(t,n,i?",%.5g":"%.5g",double(f));}
        log.line(false,t);
    }
}

void run_relative_scan(Memory& mem,uint64_t block,uint64_t bytes,const Basis* basis,float* scratch,ScanResult& out,
                       void (*yield)()) noexcept {
    // Every 4-byte position i of [0, bytes) is a look-yaw candidate.
    if(!out.regions)out.block=block;
    ++out.regions;out.bytes+=bytes;out.basis=basis!=nullptr;
    const double en=basis?std::sqrt(basis->eye[0]*basis->eye[0]+basis->eye[1]*basis->eye[1]+basis->eye[2]*basis->eye[2]):0;
    int k=0;if(basis)for(int j=1;j<3;++j)if(std::fabs(basis->eye[j])>std::fabs(basis->eye[k]))k=j;
    for(uint64_t pos=0;pos<bytes;pos+=scan_chunk) {
        const uint64_t start=pos>=rel_lo?pos-rel_lo:0;
        uint64_t end=pos+scan_chunk+rel_hi;if(end>bytes)end=bytes;
        ++out.chunks;
        if(!mem.read(block+start,scratch,uint32_t(end-start))){++out.unreadable_chunks;if(yield)yield();continue;}
        for(uint64_t i=pos;i<pos+scan_chunk&&i+rel_hi<=bytes;i+=4) {
            if(i<rel_lo)continue;
            const float* L=scratch+(i-start)/4;
            // Route 2 (diagnostic): consecutive float triples proportional to the recovered eye.
            if(basis&&en>0&&std::fabs(basis->eye[k])>0) {
                const double a[3]={L[0],L[1],L[2]};
                if(std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2])&&std::fabs(a[0])+std::fabs(a[1])+std::fabs(a[2])>1e-3) {
                    const double r=a[k]/basis->eye[k];
                    if(std::fabs(r)>=1.0/64&&std::fabs(r)<=64) {
                        bool ok=true;for(int j=0;j<3&&ok;++j)ok=std::fabs(a[j]-r*basis->eye[j])<=0.01*std::fabs(r)*en;
                        if(ok) {
                            ++out.prop;
                            const double ar=std::fabs(r);
                            if(std::fabs(ar-16)<=0.48||std::fabs(ar-1.0/16)<=0.0019)++out.prop16;
                            if(out.nprop<ScanResult::prop_cap){out.prop_off[out.nprop]=block+i;out.prop_lo[out.nprop]=block;out.prop_ratio[out.nprop]=float(r);++out.nprop;}
                        }
                    }
                }
            }
            // Exact fast skip: with look, heading, camera yaw, both heights and the
            // aircraft x/z all +-0, no relation can hold.
            {
                constexpr int d=int(field_offset[look_yaw]);
                uint32_t z=0;
                for(unsigned f:{look_yaw,heading,cam_yaw,air_x,air_y,air_z,cam_y}){uint32_t u;std::memcpy(&u,L+(int(field_offset[f])-d)/4,4);z|=u&0x7FFFFFFFu;}
                if(!z)continue;
            }
            float v[field_count];
            for(unsigned f=0;f<field_count;++f)v[f]=L[(int(field_offset[f])-int(field_offset[look_yaw]))/4];
            RelFacts facts;const uint32_t m=relations(v,basis,0,0,&facts);
            if(!m)continue;
            if(m&rel_sane)++out.counts[0];
            if(m&rel_yaw)++out.counts[1];
            if(m&rel_vert)++out.counts[2];
            if(m&rel_horiz)++out.counts[3];
            if(m&rel_eye)++out.counts[4];
            if(m&rel_degrees)++out.counts[5];
            const unsigned core=unsigned(bool(m&rel_yaw))+unsigned(bool(m&rel_vert))+unsigned(bool(m&rel_horiz));
            if((m&rel_sane)&&core==3){++out.full;if(out.nfull<ScanResult::full_cap){out.full_off[out.nfull]=block+i;out.full_lo[out.nfull]=block;out.full_hi[out.nfull]=block+bytes;++out.nfull;}}
            else if((m&rel_sane)&&core==2) {
                ++out.near;
                if(out.nnear<ScanResult::near_cap){ScanHit& h=out.nearHits[out.nnear++];h.offset=block+i;h.lo=block;h.hi=block+bytes;h.mask=m;h.f=facts;std::memcpy(h.v,v,sizeof v);}
            }
            if((m&rel_degrees)&&(m&rel_vert)&&(m&rel_horiz)){++out.degrees;if(out.ndeg<ScanResult::deg_cap){out.deg_off[out.ndeg]=block+i;out.deg_lo[out.ndeg]=block;++out.ndeg;}}
            int score=0;for(uint32_t b:{rel_sane,rel_yaw,rel_vert,rel_horiz,rel_eye})score+=(m&b)?1:0;
            for(int r=0;r<3;++r) {
                ScanHit& b=out.best[r];
                if(score>b.score||(score==b.score&&std::fabs(facts.yaw_res)<std::fabs(b.f.yaw_res))) {
                    for(int q=2;q>r;--q)out.best[q]=out.best[q-1];
                    b=ScanHit{};b.offset=block+i;b.lo=block;b.hi=block+bytes;b.mask=m;b.f=facts;b.score=score;std::memcpy(b.v,v,sizeof v);
                    break;
                }
            }
        }
        if(yield)yield();
    }
}

void log_scan(const ScanResult& r,Sink& log) noexcept {
    char t[448];size_t n=0;char mb[64],fb[64];
    appendf(t,n,"ee-camera;event=scan;regions=%u;first=%llx;bytes=%llx;ms=%.0f;chunks=%u;unreadable=%u;basis=%u;"
        "alone=sane:%llu,yaw:%llu,vert:%llu,horiz:%llu,eye:%llu,degrees:%llu;full=%llu;near=%llu;degrees=%llu;eye-proportional=%llu,x16:%llu",
        r.regions,static_cast<unsigned long long>(r.block),static_cast<unsigned long long>(r.bytes),r.ms,r.chunks,r.unreadable_chunks,unsigned(r.basis),
        static_cast<unsigned long long>(r.counts[0]),static_cast<unsigned long long>(r.counts[1]),static_cast<unsigned long long>(r.counts[2]),
        static_cast<unsigned long long>(r.counts[3]),static_cast<unsigned long long>(r.counts[4]),static_cast<unsigned long long>(r.counts[5]),
        static_cast<unsigned long long>(r.full),static_cast<unsigned long long>(r.near),static_cast<unsigned long long>(r.degrees),
        static_cast<unsigned long long>(r.prop),static_cast<unsigned long long>(r.prop16));
    log.line(false,t);
    if(r.nfull) {
        n=0;appendf(t,n,"ee-camera;event=scan-full;count=%llu;look-yaw-at=",static_cast<unsigned long long>(r.full));
        for(unsigned i=0;i<r.nfull;++i)appendf(t,n,"%s%llx+%llX(shift=%+lld)",i?",":"",static_cast<unsigned long long>(r.full_lo[i]),
            static_cast<unsigned long long>(r.full_off[i]-r.full_lo[i]),
            static_cast<long long>(r.full_off[i]-r.full_lo[i])-static_cast<long long>(field_offset[look_yaw]));
        log.line(false,t);
    }
    if(r.ndeg) {
        n=0;appendf(t,n,"ee-camera;event=scan-degrees;flag=angles-in-degrees-never-written;count=%llu;look-yaw-at=",static_cast<unsigned long long>(r.degrees));
        for(unsigned i=0;i<r.ndeg;++i)appendf(t,n,"%s%llx+%llX",i?",":"",static_cast<unsigned long long>(r.deg_lo[i]),
            static_cast<unsigned long long>(r.deg_off[i]-r.deg_lo[i]));
        log.line(false,t);
    }
    for(int k=0;k<3;++k) {
        const ScanHit& h=r.best[k];if(h.score<0)continue;const float* v=h.v;
        n=0;appendf(t,n,"ee-camera;event=scan-best;rank=%d;look-yaw-at=%llx+%llX;score=%d;pass=%s;fail=%s;yaw-res=%.4g;vert=%.4g;horiz=%.4g;eye-err=%.4g@x%g;"
            "L=%.5g;H=%.5g;air=%.6g,%.6g,%.6g;cam=%.6g,%.6g,%.6g;rot=%.5g,%.5g,%.5g",k,static_cast<unsigned long long>(h.lo),static_cast<unsigned long long>(h.offset-h.lo),h.score,
            mask_text(h.mask,mb),fail_text(h.mask,fb),h.f.yaw_res,h.f.vert,h.f.horiz,h.f.eye_err,h.f.eye_scale,v[look_yaw],v[heading],
            v[air_x],v[air_y],v[air_z],v[cam_x],v[cam_y],v[cam_z],v[cam_pitch],v[cam_yaw],v[cam_roll]);
        log.line(false,t);
    }
    for(unsigned i=0;i<r.nnear;i+=4) {
        n=0;appendf(t,n,"ee-camera;event=scan-near;total=%llu;",static_cast<unsigned long long>(r.near));
        for(unsigned j=i;j<i+4&&j<r.nnear;++j){const ScanHit& h=r.nearHits[j];
            appendf(t,n,"%s%llx+%llX:fail=%s:yaw-res=%.3g,vert=%.3g,horiz=%.4g",j>i?",":"",static_cast<unsigned long long>(h.lo),
                static_cast<unsigned long long>(h.offset-h.lo),fail_text(h.mask,fb),
                h.f.yaw_res,h.f.vert,h.f.horiz);}
        log.line(false,t);
    }
    if(r.nprop) {
        n=0;appendf(t,n,"ee-camera;event=scan-eye-proportional;count=%llu;x16=%llu;hits=",static_cast<unsigned long long>(r.prop),
            static_cast<unsigned long long>(r.prop16));
        for(unsigned i=0;i<r.nprop;++i)appendf(t,n,"%s%llx+%llX:r=%.5g",i?",":"",static_cast<unsigned long long>(r.prop_lo[i]),
            static_cast<unsigned long long>(r.prop_off[i]-r.prop_lo[i]),double(r.prop_ratio[i]));
        log.line(false,t);
    }
}

} // namespace acz::ee_camera
