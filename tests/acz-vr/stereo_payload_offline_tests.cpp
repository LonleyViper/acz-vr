// Synthetic freeze/provider data only. No native device, DLL or XR session.
#include "stereo_payload.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

// Fail each owned-output allocation deterministically, without large buffers.
static int allocation_countdown = -1;
void* operator new(std::size_t n) {
    if (allocation_countdown == 0) throw std::bad_alloc();
    if (allocation_countdown > 0) --allocation_countdown;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace acz;
using namespace acz::capture;
using namespace acz::metadata;
namespace sp = acz::stereo_payload;

static void write_float(std::vector<uint8_t>& bytes, size_t offset, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    for (unsigned b = 0; b < 4; ++b) bytes[offset+b] = uint8_t(bits >> (8*b));
}
static float read_float(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t bits = 0;
    for (unsigned b = 0; b < 4; ++b) bits |= uint32_t(bytes[offset+b]) << (8*b);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
static const std::array<Mat4,2> eyes{{
    {1,2,0,1, 0,1,3,0, 4,0,1,2, -1,5,2,1},
    {2,0,1,0, 1,3,0,2, 0,2,4,1, 6,-2,3,1}
}};
// Explicit synthetic mapping: has no claim to native PS2 screen/depth values.
static const Mat4 K{2,1,0,0, 0,3,1,0, 1,0,4,0, 7,-2,5,1};
// Hand-calculated products, independent of production multiply helpers.
static const Mat4 clips[2][2] = {
    {{2,4,0,2, 0,3,9,0, 16,0,4,8, 32,21,27,20},
     {1,2,0,1, 0,2,6,0, 12,0,3,6, 1,9,27,1}},
    {{4,0,2,0, 3,9,0,6, 0,8,16,4, 22,30,36,20},
     {2,0,1,0, 2,6,0,4, 0,6,12,3, 10,24,5,18}}
};
static std::shared_ptr<const Snapshot> frozen(size_t stride) {
    // Two complete elements at an unaligned origin; indices repeat element 1.
    std::vector<uint8_t> bytes(13+2*stride);
    for (size_t i=0; i<bytes.size(); ++i) bytes[i]=uint8_t(i*17+31);
    const Mat4 worlds[2] = {
        {2,0,0,0, 0,3,0,0, 0,0,4,0, 5,6,7,1},
        {1,0,0,0, 0,2,0,0, 0,0,3,0, -2,8,1,1}
    };
    for (size_t j=0; j<2; ++j) for (size_t i=0; i<16; ++i)
        write_float(bytes,13+j*stride+256+i*4,float(worlds[j][i]));
    State state;
    state.tracking_complete=true;
    state.pipeline.drawcpt=true; state.pipeline.requirements_proven=true;
    state.pipeline.identity=new_lifetime_identity();
    state.pipeline.explicit_root_identity=new_lifetime_identity();
    state.pipeline.resources={{BindingKind::srv,10,0,ShaderStage::geometry}};
    RootLayout layout;
    RootParameter root; root.kind=ParameterKind::srv;
    root.shader_register=10; root.visibility=Visibility::geometry;
    layout.parameters={root};
    state.set_root(7,RootBindingMetadata::from_adapter(layout),state.pipeline.explicit_root_identity);
    state.set_value(0,ParameterKind::srv,123);
    Inputs in;
    in.list_identity=state.list_identity; in.generation=state.generation; in.revision=state.revision;
    in.root_descriptors[123]={BindingKind::srv,{1,13,2*stride,uint32_t(stride)},0,2,true};
    in.resources={{1,state.generation,bytes.size(),bytes.data(),bytes.size(),true,true,state.revision,state.list_identity}};
    in.indexing_proven=true; in.srt_indices={1,0,1}; in.vertex_fetch_proven=true;
    auto result=freeze(state,Command{false,3,3,0,0,77},in);
    CHECK(result);
    CHECK(result.snapshot->cpu_data_complete && !result.snapshot->gpu_replay_complete);
    std::fill(bytes.begin(),bytes.end(),0); // Provider lifetime has ended.
    return result.snapshot;
}
static void failed(const sp::Result& r, sp::Error expected) {
    CHECK(!r); CHECK(r.error==expected); CHECK(!r.batch.has_value());
}
static void success_tests(size_t stride) {
    auto source=frozen(stride);
    const auto original_elements=source->srt_elements;
    const auto original_resources=source->resources;
    const auto identity=source->state.list_identity;
    auto result=sp::prepare(source,eyes,K);
    CHECK(result); CHECK(result.batch->source.get()==source.get());
    CHECK(result.batch->eyes[0].size()==3 && result.batch->eyes[1].size()==3);
    for (size_t eye=0; eye<2; ++eye) for (size_t instance=0; instance<3; ++instance) {
        const auto& output=result.batch->eyes[eye][instance];
        const auto& clip=clips[eye][instance==1 ? 0 : 1];
        CHECK(output.size()==stride);
        for (size_t row=0; row<4; ++row) {
            double a=clip[4*row], b=clip[4*row+1], c=clip[4*row+2], d=clip[4*row+3];
            const double screen[4]={2*a+c+7*d,a+3*b-2*d,b+4*c+5*d,d};
            for (size_t col=0; col<4; ++col) {
                CHECK(read_float(output,64+16*row+4*col)==float(clip[4*row+col]));
                CHECK(read_float(output,16*row+4*col)==float(screen[col]));
            }
        }
        CHECK(std::equal(output.begin()+128,output.end(),original_elements[instance].begin()+128));
        CHECK(output.data()!=source->srt_elements[instance].data());
        CHECK(output.data()!=result.batch->eyes[1-eye][instance].data());
    }
    CHECK(result.batch->eyes[0][0]==result.batch->eyes[0][2]);
    CHECK(result.batch->eyes[0][0].data()!=result.batch->eyes[0][2].data());
    CHECK(result.batch->eyes[0][0]!=result.batch->eyes[1][0]);
    CHECK(source->srt_elements==original_elements && source->resources==original_resources);
    CHECK(source->state.list_identity==identity && source->command.start_instance==77);
    CHECK(source->state.roots.at(0).value==123 && source->bindings.size()==1);
    CHECK(!source->gpu_replay_complete);
    std::weak_ptr<const Snapshot> provenance=source;
    source.reset();
    CHECK(!provenance.expired());
    const auto right=result.batch->eyes[1][0];
    result.batch->eyes[0][0][128]^=255;
    CHECK(result.batch->eyes[1][0]==right);
    CHECK(result.batch->source->srt_elements==original_elements);
    result.batch.reset(); CHECK(provenance.expired());
}
static void failure_tests() {
    auto source=frozen(416);
    failed(sp::prepare({},eyes,K),sp::Error::missing_source);
    auto changed=std::make_shared<Snapshot>(*source);
    changed->cpu_data_complete=false;
    failed(sp::prepare(changed,eyes,K),sp::Error::incomplete_source);
    changed->cpu_data_complete=true; changed->command.instances=0;
    failed(sp::prepare(changed,eyes,K),sp::Error::inconsistent_count);
    changed->command.instances=2;
    failed(sp::prepare(changed,eyes,K),sp::Error::inconsistent_count);
    changed->command.instances=3; changed->srt_elements.clear();
    failed(sp::prepare(changed,eyes,K),sp::Error::inconsistent_count);
    for (size_t stride : {size_t(0),size_t(384),size_t(415),size_t(417),size_t(2052)}) {
        changed=std::make_shared<Snapshot>(*source);
        for (auto& element:changed->srt_elements) element.resize(stride);
        failed(sp::prepare(changed,eyes,K),sp::Error::unsupported_stride);
    }
    changed=std::make_shared<Snapshot>(*source);
    changed->srt_elements.back().resize(512);
    failed(sp::prepare(changed,eyes,K),sp::Error::inconsistent_stride);
    const double bad_values[]={std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),
        double((std::numeric_limits<float>::max)())*2, (std::numeric_limits<double>::max)()};
    for (double bad:bad_values) for (size_t i=0;i<16;++i) {
        for (size_t eye=0;eye<2;++eye) {
            auto invalid=eyes; invalid[eye][i]=bad;
            failed(sp::prepare(source,invalid,K),sp::Error::invalid_matrix);
        }
        auto invalid=K; invalid[i]=bad;
        failed(sp::prepare(source,eyes,invalid),sp::Error::invalid_matrix);
    }
    const float source_bad[]={std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};
    for (float bad:source_bad) for (size_t i=0;i<16;++i) {
        changed=std::make_shared<Snapshot>(*source);
        write_float(changed->srt_elements.back(),256+4*i,bad);
        failed(sp::prepare(changed,eyes,K),sp::Error::invalid_matrix);
    }
    // Finite inputs overflow float32 range during multiplication, only on the
    // right eye after left output exists; finite double result still rejected.
    auto overflow=std::array<Mat4,2>{identity(),identity()};
    overflow[1][0]=(std::numeric_limits<float>::max)();
    failed(sp::prepare(source,overflow,identity()),sp::Error::invalid_matrix);
    // Finite terms whose accumulating sum exceeds float32 range.
    changed=std::make_shared<Snapshot>(*source);
    for (size_t i=0;i<16;++i) write_float(changed->srt_elements.back(),256+4*i,0);
    write_float(changed->srt_elements.back(),256,(std::numeric_limits<float>::max)());
    write_float(changed->srt_elements.back(),260,(std::numeric_limits<float>::max)());
    auto sum=std::array<Mat4,2>{identity(),identity()}; sum[1][4]=1;
    failed(sp::prepare(changed,sum,identity()),sp::Error::invalid_matrix);
    // K multiplication fails after valid M64; no partial result survives.
    auto hugeK=identity(); hugeK[0]=(std::numeric_limits<float>::max)();
    failed(sp::prepare(source,{identity(),identity()},hugeK),sp::Error::invalid_matrix);
    bool reached_success=false;
    for (int n=0;n<20;++n) {
        allocation_countdown=n;
        auto r=sp::prepare(source,eyes,K);
        allocation_countdown=-1;
        if (r) { CHECK(n>=8); reached_success=true; break; }
        failed(r,sp::Error::allocation_failed);
    }
    CHECK(reached_success);
}
static void accounting_tests() {
    size_t bytes=999;
    CHECK(sp::output_size(4096,2048,bytes)==sp::Error::none);
    CHECK(bytes==capture::max_copy_bytes);
    CHECK(sp::output_size(4097,2048,bytes)==sp::Error::output_budget && bytes==0);
    const size_t count=capture::max_copy_bytes/(2*416);
    CHECK(sp::output_size(count,416,bytes)==sp::Error::none && bytes==count*832);
    CHECK(sp::output_size(count+1,416,bytes)==sp::Error::output_budget && bytes==0);
    CHECK(sp::output_size((std::numeric_limits<size_t>::max)(),416,bytes)==sp::Error::size_overflow && bytes==0);
    CHECK(sp::output_size((std::numeric_limits<size_t>::max)()/416,416,bytes)==sp::Error::size_overflow && bytes==0);
    CHECK(sp::output_size(0,416,bytes)==sp::Error::inconsistent_count && bytes==0);
    CHECK(sp::output_size(1,433,bytes)==sp::Error::unsupported_stride && bytes==0);
    // Just over the two-eye bound, with only ~8 MiB of synthetic source.
    // Arming allocation failure proves rejection precedes output allocation.
    auto oversized=std::make_shared<Snapshot>();
    oversized->cpu_data_complete=true; oversized->command.instances=4097;
    oversized->srt_elements.resize(4097,std::vector<uint8_t>(2048));
    allocation_countdown=0;
    auto rejected=sp::prepare(oversized,eyes,K);
    allocation_countdown=-1;
    failed(rejected,sp::Error::output_budget);
}
int main() {
    success_tests(416); success_tests(512); success_tests(2048);
    failure_tests(); accounting_tests();
    std::printf("stereo payload: %d checks passed (synthetic CPU only)\n",checks);
}
