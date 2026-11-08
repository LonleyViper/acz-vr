// J-w1-ee-camera rev 3: region census, wide relative pass, differential scan.
// SDK-free, read-only; see ee_camera_diff.h.
#include "ee_camera_diff.h"
#include <cstdio>

namespace acz::ee_camera {
namespace {
template<size_t N,class... A> void appendf(char (&buf)[N],size_t& len,const char* fmt,A... a) noexcept {
    if(len>=N-1)return;
    const int w=std::snprintf(buf+len,N-len,fmt,a...);
    if(w>0)len=len+size_t(w)<N?len+size_t(w):N-1;
}
using ull=unsigned long long;
}

const RegionInfo* find_region(const RegionInfo* r,unsigned n,uint64_t a) noexcept {
    unsigned lo=0,hi=n;
    while(lo<hi){const unsigned m=(lo+hi)/2;if(r[m].base+r[m].size<=a)lo=m+1;else hi=m;}
    return lo<n&&a>=r[lo].base&&a<r[lo].base+r[lo].size?&r[lo]:nullptr;
}

void log_region_table(const RegionInfo* r,unsigned n,const RegionCensus& c,Sink& log,bool full) noexcept {
    char t[448];size_t k=0;
    uint64_t cnt[4]{},bytes[4]{},wcnt[4]{},wbytes[4]{};
    for(unsigned i=0;i<n;++i){const uint8_t ty=r[i].type<4?r[i].type:0;++cnt[ty];bytes[ty]+=r[i].size;if(r[i].writable){++wcnt[ty];wbytes[ty]+=r[i].size;}}
    appendf(t,k,"ee-camera;event=region-table;cols=type:count/MiB/writable-count/writable-MiB;seen=%u;kept=%u;small=%u;own=%u;gpu=%u;unreadable=%u;overflow=%u;MiB=%.1f",
        c.seen,c.kept,c.tiny,c.own,c.gpu,c.unreadable,c.overflow,double(c.bytes)/1048576.0);
    for(unsigned ty=1;ty<4;++ty)appendf(t,k,";%s=%llu/%.1f/%llu/%.1f",region_type_name(uint8_t(ty)),ull(cnt[ty]),double(bytes[ty])/1048576.0,
        ull(wcnt[ty]),double(wbytes[ty])/1048576.0);
    log.line(false,t);
    if(!full)return;
    // the 24 largest regions, 6 per line: base:size:type:protect
    unsigned idx[24];unsigned m=0;
    for(unsigned i=0;i<n;++i) {
        unsigned p=m<24?m++:24;
        if(p==24){if(r[i].size<=r[idx[23]].size)continue;p=23;}
        while(p>0&&r[idx[p-1]].size<r[i].size){idx[p]=idx[p-1];--p;}
        idx[p]=i;
    }
    for(unsigned i=0;i<m;i+=6) {
        k=0;appendf(t,k,"ee-camera;event=region-largest;rank=%u;cols=base:MiB:type:protect:writable;",i);
        for(unsigned j=i;j<i+6&&j<m;++j){const RegionInfo& x=r[idx[j]];
            appendf(t,k,"%s%llx:%.2f:%s:%x:%u",j>i?",":"",ull(x.base),double(x.size)/1048576.0,region_type_name(x.type),x.protect,unsigned(x.writable));}
        log.line(false,t);
    }
}

void wide_pass(Memory& mem,const RegionInfo* r,unsigned n,const Basis* basis,float* scratch,uint64_t budget,WideCursor& cur,
               ScanResult& out,Sink& log,void (*yield)()) noexcept {
    uint64_t done=0;unsigned lines=0;out.complete=false;
    if(cur.region>=n)cur.region=0;
    while(cur.region<n) {
        const RegionInfo& g=r[cur.region];
        if(done&&done+g.size>budget)return; // resume here next pass
        uint64_t before[6];std::memcpy(before,out.counts,sizeof before);
        const uint64_t f0=out.full,n0=out.near,d0=out.degrees;
        run_relative_scan(mem,g.base,g.size,basis,scratch,out,yield);
        done+=g.size;
        bool any=out.full!=f0||out.near!=n0||out.degrees!=d0;
        for(int i=0;i<6;++i)any=any||out.counts[i]!=before[i];
        if(any&&lines<32) {
            ++lines;char t[448];
            std::snprintf(t,sizeof t,"ee-camera;event=region-scan;base=%llx;MiB=%.2f;type=%s;protect=%x;alone=sane:%llu,yaw:%llu,vert:%llu,horiz:%llu,eye:%llu,degrees:%llu;"
                "full=%llu;near=%llu;degrees=%llu",ull(g.base),double(g.size)/1048576.0,region_type_name(g.type),g.protect,
                ull(out.counts[0]-before[0]),ull(out.counts[1]-before[1]),ull(out.counts[2]-before[2]),ull(out.counts[3]-before[3]),
                ull(out.counts[4]-before[4]),ull(out.counts[5]-before[5]),ull(out.full-f0),ull(out.near-n0),ull(out.degrees-d0));
            log.line(false,t);
        }
        ++cur.region;
    }
    cur.region=0;++cur.passes;out.complete=true;
}

// ------------------------------------------------------------------ differential
bool encoded_delta(uint8_t enc,uint32_t a,uint32_t b,double& d) noexcept {
    switch(enc) {
        case enc_rad:case enc_deg: {
            const float fa=from_bits(a),fb=from_bits(b);
            if(!std::isfinite(fa)||!std::isfinite(fb))return false;
            const double lim=enc==enc_rad?4*pi+1:721.0;
            if(std::fabs(fa)>lim||std::fabs(fb)>lim)return false;
            d=enc==enc_rad?wrap(double(fb)-fa):wrap_deg(double(fb)-fa)*pi/180;
            return true;
        }
        case enc_fx4096:case enc_fx65536: {
            const int32_t ia=int32_t(a),ib=int32_t(b);
            if(ia<-(1<<20)||ia>(1<<20)||ib<-(1<<20)||ib>(1<<20))return false; // small integers only
            const double U=enc==enc_fx4096?4096.0:65536.0;
            d=wrap(double(int64_t(ib)-int64_t(ia))*2*pi/U);
            return true;
        }
        default:return false;
    }
}
namespace {
inline bool matches(double d,double expect) noexcept {return std::fabs(d-expect)<=0.2*std::fabs(expect)+0.01;}
}
int DiffScanner::find(uint64_t addr,uint8_t enc,uint8_t axis,int8_t sign) const noexcept {
    for(unsigned i=0;i<n_;++i)if(t_[i].addr==addr&&t_[i].enc==enc&&t_[i].axis==axis&&t_[i].sign==sign)return int(i);
    return -1;
}
bool DiffScanner::insert(const DiffCandidate& c) noexcept {
    if(n_<cap){t_[n_++]=c;++created;return true;}
    // evict the worst non-positive entry (never one born this window)
    int worst=-1;
    for(unsigned i=0;i<n_;++i)if(t_[i].born!=window_&&t_[i].score()<=0&&(worst<0||t_[i].score()<t_[unsigned(worst)].score()))worst=int(i);
    if(worst<0){++dropped;return false;}
    t_[unsigned(worst)]=c;++evicted;++created;return true;
}
void DiffScanner::analyse(uint64_t addr,const uint32_t* A,const uint32_t* B,size_t words,double dyaw,double dpitch) noexcept {
    ++windows;++window_;
    const bool my=std::fabs(dyaw)>diff_trigger_rad,mp=std::fabs(dpitch)>diff_trigger_rad;
    const double turn=std::fmax(std::fabs(dyaw),std::fabs(dpitch));if(turn>maxTurn)maxTurn=turn;
    if(my||mp)++moving;else ++steady;
    // 1. existing candidates inside the window: hit / miss
    const uint64_t end=addr+uint64_t(words)*4;
    for(unsigned i=0;i<n_;++i) {
        DiffCandidate& c=t_[i];
        if(c.addr<addr||c.addr>=end)continue;
        const size_t w=size_t((c.addr-addr)/4);
        const uint32_t a=A[w],b=B[w];c.last=b;
        const double dx=c.axis?dpitch:dyaw;const bool axisMoving=c.axis?mp:my;
        if(a==b){if(axisMoving)++c.misses;continue;}           // should have changed
        double d;
        if(axisMoving&&encoded_delta(c.enc,a,b,d)&&matches(d,c.sign*dx)) {
            ++c.hits;c.ratio+=d/(c.sign*dx);const double v=decoded_value(c.enc,b);c.vmin=std::fmin(c.vmin,v);c.vmax=std::fmax(c.vmax,v);
        }
        else ++c.misses;                                         // changed while steady / inconsistently
    }
    if(!(my||mp))return;
    // 2. new candidates from changed words
    unsigned fresh=0;
    for(size_t w=0;w<words&&fresh<new_per_window;++w) {
        const uint32_t a=A[w],b=B[w];
        if(a==b)continue;
        ++changed;
        for(uint8_t enc=0;enc<enc_count;++enc) {
            double d;if(!encoded_delta(enc,a,b,d))continue;
            for(uint8_t axis=0;axis<2;++axis) {
                const double dx=axis?dpitch:dyaw;if(!(axis?mp:my))continue;
                for(int8_t sign:{int8_t(1),int8_t(-1)}) {
                    if(!matches(d,sign*dx))continue;
                    const uint64_t at=addr+uint64_t(w)*4;
                    if(find(at,enc,axis,sign)>=0)continue;
                    DiffCandidate c;c.addr=at;c.enc=enc;c.axis=axis;c.sign=sign;c.hits=1;c.born=window_;c.ratio=d/(sign*dx);c.last=b;
                    c.vmin=std::fmin(decoded_value(enc,a),decoded_value(enc,b));c.vmax=std::fmax(decoded_value(enc,a),decoded_value(enc,b));
                    if(insert(c))++fresh;
                }
            }
        }
    }
}
unsigned DiffScanner::top(DiffCandidate* out,unsigned k,uint32_t minHits) const noexcept {
    unsigned m=0;
    for(unsigned i=0;i<n_;++i) {
        const DiffCandidate& c=t_[i];if(c.hits<minHits||c.score()<=0)continue;
        unsigned p=m<k?m++:k;
        if(p==k){if(c.score()<=out[k-1].score())continue;p=k-1;}
        while(p>0&&(out[p-1].score()<c.score()||(out[p-1].score()==c.score()&&out[p-1].hits<c.hits))){out[p]=out[p-1];--p;}
        out[p]=c;
    }
    return m;
}

bool diff_window(Memory& mem,BasisSource& bs,Waiter& w,uint64_t addr,uint32_t bytes,uint32_t* A,uint32_t* B,DiffScanner& d) noexcept {
    Basis b0;if(!bs.latest(b0))return false;
    if(!mem.read(addr,A,bytes))return false;
    const double y0=basis_yaw(b0),p0=basis_pitch(b0);
    double dy=0,dp=0,prevTurn=0;unsigned waited=0,moved=0,stillAfter=0;
    while(waited<1200) {
        w.wait_ms(10);waited+=10;
        Basis b1;if(!bs.latest(b1))continue;
        dy=wrap(basis_yaw(b1)-y0);dp=basis_pitch(b1)-p0;
        const double turn=std::fmax(std::fabs(dy),std::fabs(dp));
        if(!moved){if(turn>diff_trigger_rad)moved=waited;else if(waited>=400)break;}
        else if(waited-moved>=250)break; // rev 4: finer windows, read B <= 250 ms after the trigger
        else {
            // keep going while the camera still moves (> 0.5 deg per 10 ms step)
            if(std::fabs(turn-prevTurn)<0.5*pi/180){if(++stillAfter>=5)break;}else stillAfter=0;
        }
        prevTurn=turn;
    }
    if(!mem.read(addr,B,bytes))return false;
    if(!moved){dy=0;dp=0;} // steady control pair (noise marking only)
    d.analyse(addr,A,B,bytes/4,dy,dp);
    return true;
}

void DiffDriver::build_order(const RegionInfo* r,unsigned n) noexcept {
    orderN=n;orderFirst=n?r[0].base:0;norder=0;region=0;offset=0;
    auto tier0=[&](const RegionInfo& g){return g.size==block_bytes||g.size==block_bytes/2;};
    auto nearTier0=[&](const RegionInfo& g){
        for(unsigned j=0;j<n;++j)if(tier0(r[j])){const uint64_t a=r[j].base,b=r[j].base+r[j].size;
            if(g.base+g.size+near_bytes>=a&&g.base<=b+near_bytes)return true;}
        return false;};
    for(int tier=0;tier<3;++tier)for(unsigned i=0;i<n&&norder<region_cap;++i) {
        const RegionInfo& g=r[i];if(!g.writable)continue;
        const int t=tier0(g)?0:nearTier0(g)?1:2;
        if(t==tier)order[norder++]=uint16_t(i);
    }
}
bool DiffDriver::next(const RegionInfo* r,unsigned n,const DiffScanner& d,uint32_t window,uint64_t& addr,uint32_t& bytes) noexcept {
    if(!n||window<4096)return false;
    ++step;rng=rng*6364136223846793005ull+1442695040888963407ull; // random interleave: no phase lock with periodic motion
    if((rng>>33)&1u) {
        DiffCandidate best[10];const unsigned k=d.top(best,10,1); // hot list: any positive candidate
        if(k) {
            const DiffCandidate& c=best[hot++%k];
            if(const RegionInfo* g=find_region(r,n,c.addr)) {
                uint64_t start=c.addr>window/2?c.addr-window/2:0;
                start&=~uint64_t(0xFFF);if(start<g->base)start=g->base;
                const uint64_t end=start+window<g->base+g->size?start+window:g->base+g->size;
                addr=start;bytes=uint32_t(end-start);return bytes>=4;
            }
        }
    }
    if(n!=orderN||r[0].base!=orderFirst)build_order(r,n);
    for(unsigned guard=0;guard<=norder;++guard) {
        if(region>=norder){region=0;offset=0;++passes;}
        if(!norder)return false;
        const RegionInfo& g=r[order[region]];
        if(!g.writable||offset>=g.size){++region;offset=0;continue;}
        addr=g.base+offset;const uint64_t left=g.size-offset;bytes=uint32_t(left<window?left:window);offset+=bytes;
        return true;
    }
    return false;
}

double decoded_value(uint8_t enc,uint32_t w) noexcept {return enc<=enc_deg?double(from_bits(w)):double(int32_t(w));}
void log_diff(const DiffScanner& d,const RegionInfo* r,unsigned n,Sink& log,const char* tag) noexcept {
    char t[448];
    std::snprintf(t,sizeof t,"ee-camera;event=diff-summary;tag=%s;windows=%llu;moving=%llu;steady=%llu;changed-words=%llu;table=%u;created=%llu;evicted=%llu;"
        "dropped=%llu;max-turn-deg=%.1f;trigger-deg=5;read-only",tag,ull(d.windows),ull(d.moving),ull(d.steady),ull(d.changed),d.size(),ull(d.created),
        ull(d.evicted),ull(d.dropped),d.maxTurn*180/pi);
    log.line(false,t);
    DiffCandidate best[10];const unsigned k=d.top(best,10,1);
    for(unsigned i=0;i<k;++i) {
        const DiffCandidate& c=best[i];const RegionInfo* g=find_region(r,n,c.addr);
        double v=0;
        v=decoded_value(c.enc,c.last);
        std::snprintf(t,sizeof t,"ee-camera;event=diff-top;rank=%u;addr=%llx;region=%llx+%llX;type=%s;enc=%s;axis=%s;sign=%+d;hits=%u;misses=%u;score=%d;"
            "mean-ratio=%.3f;current=%.6g;min=%.6g;max=%.6g;tag=%s",i,ull(c.addr),ull(g?g->base:0),ull(g?c.addr-g->base:0),g?region_type_name(g->type):"?",encoding_name(c.enc),
            c.axis?"pitch":"yaw",int(c.sign),c.hits,c.misses,c.score(),c.hits?c.ratio/c.hits:0,v,c.vmin,c.vmax,tag);
        log.line(false,t);
    }
}

} // namespace acz::ee_camera
