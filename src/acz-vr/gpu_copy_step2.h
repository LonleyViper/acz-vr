#pragma once
// J-3b6: design section 6 "Step 2 - one selected game-buffer copy, read-only".
// SDK-free pieces shared by the hook adapter (gpu_copy_step2_hooks.inl, included
// by gpu_copy_step1_hooks.inl) and its offline fixture. Default OFF: nothing
// here runs unless gpu_copy_step2_experiment plus every opt-in is latched.
//
// Step 2 reuses ALL step-1 machinery (owner registry, timeline, witness,
// ledger, Draw/Execute/Close/Reset seams, worker, disposition). Only the
// prepared private resource (one READBACK buffer, no UPLOAD/pattern), the Draw
// recipe (up to three read-only CopyBufferRegion regions sourced from the
// draw's CURRENTLY BOUND game buffers) and the completed-bytes analysis differ.
#include <cstddef>
#include <cstdint>

namespace acz::gpu_copy::step2 {
enum Role : unsigned {role_ib=0,role_vb=1,role_srt=2};
constexpr unsigned role_count=3;
constexpr const char* role_names[role_count]={"ib","vb","srt"};
// One region per role in ONE proxy READBACK buffer. The per-region cap equals
// the existing owner recipe cap (owner::recipe_byte_cap, asserted by the
// adapter), so recipe_valid/read_back are reused unchanged.
constexpr uint64_t region_bytes=4096;
constexpr uint64_t readback_bytes=region_bytes*role_count;
constexpr uint64_t total_cap=65536; // all regions together; refuses above
static_assert(readback_bytes<=total_cap,"step-2 copy total stays bounded");
// Conservative logical charge: the committed 12 KiB READBACK buffer is assumed
// to occupy one 64 KiB placement (D3D12 default resource alignment). Not measured.
constexpr uint64_t charged_bytes=65536;
constexpr uint64_t private_token=0x52e1, source_token=0x52e2; // opaque ledger tokens
// Cited live shape (L3f 1078-1079, D-172; E10 IA contract; E11 t10 SrtData).
constexpr uint32_t ib_indices=9, ib_stride=2, vb_stride=64, srt_bytes=416;
// E11: the bytes of SrtData element zero the VS/GS actually read. The copy
// reads all 416; this mask is logged so the analysis never claims more.
constexpr const char* srt_read_mask="0-175,192-303,352-367,400-415";
// Numeric D3D12 values (Windows-header independent, like index_range.h).
constexpr unsigned heap_default=1, heap_upload=2, heap_readback=3, heap_custom=4;
constexpr unsigned kind_committed=1, kind_placed=2, kind_reserved=3;

inline const char* heap_name(unsigned heap) noexcept {
    return heap==heap_default?"DEFAULT":heap==heap_upload?"UPLOAD":heap==heap_readback?"READBACK":heap==heap_custom?"CUSTOM":"unknown";
}
// COPY_SOURCE legality of a game buffer as a CopyBufferRegion source WITHOUT
// any barrier on the game's resource (the proxy never adds one). Returns the
// static refusal, or nullptr when the copy is legal for the WHOLE list.
//  - UPLOAD: resources must be created in and can never leave GENERIC_READ,
//    which includes COPY_SOURCE (and VERTEX_AND_CONSTANT_BUFFER, INDEX_BUFFER,
//    NON_PIXEL_SHADER_RESOURCE); read-only states combine, so the copy is legal
//    before, between or after the game's own read uses in the same list.
//  - READBACK: permanently COPY_DEST; never a legal copy source.
//  - DEFAULT: the state is per-resource, carried across ExecuteCommandLists and
//    changed by the game's barriers, implicit common-state promotion (buffers
//    promote from COMMON to any state, including combined read states) and
//    decay to COMMON after a submission completes. The proxy tracks none of the
//    resource's state at the start of the list, so COPY_SOURCE cannot be shown.
//  - CUSTOM / unknown: no fixed state rule is known here.
inline const char* copy_source_rule(unsigned heap) noexcept {
    switch(heap) {
    case heap_upload:return nullptr;
    case heap_default:return "default-heap-state-unknown";
    case heap_readback:return "readback-heap-copy-dest-only";
    case heap_custom:return "custom-heap-state-unknown";
    default:return "heap-unknown";
    }
}
inline const char* state_rule_name(unsigned heap) noexcept {
    return heap==heap_upload?"upload-generic-read-has-copy-source":
        heap==heap_default?"default-state-untracked":heap==heap_readback?"readback-copy-dest-only":"no-known-rule";
}
// Facts gathered by the adapter under the hook-state mutex at the Draw seam.
// Addresses/routes are opaque comparison keys; nothing is dereferenced here.
struct Facts {
    const char* shape=nullptr;          // role-specific static refusal decided by the adapter
    uint64_t address=0,va=0,view_bytes=0;
    uint32_t stride=0;
    // L3 creation-time observation record of the resolved resource.
    bool record=false,description=false,buffer=false,retired=false;
    uint64_t serial=0,width=0,gpu_va=0,cpu_events=0; // cpu_events: observed Map/Unmap/Write count
    unsigned kind=0,heap=0;
    // J-142 creation-time ownership pin (held reference) for the same incarnation.
    bool pin=false,pin_finished=false;
    uint64_t pin_creation=0,pin_lifetime=0,pin_held=0;
};
struct RolePlan {
    const char* reason="not-evaluated"; // static literal; nullptr only while evaluating
    bool copy=false;
    uint64_t address=0,serial=0,lifetime=0,va=0,gpu_va=0,width=0,view_bytes=0;
    uint64_t offset=0,bytes=0,dest_offset=0,held=0,cpu_events=0;
    unsigned heap=0,kind=0;
    uint32_t stride=0;
};
// Decides one role. `relative`/`span`: byte range inside the bound view.
// Order: shape -> resolution -> creation record -> committed buffer -> heap
// state rule -> ownership pin -> span. The first failing rule is the reason.
inline void evaluate(const Facts& f,unsigned role,uint64_t relative,uint64_t span,RolePlan& out) noexcept {
    out=RolePlan{};
    out.address=f.address;out.serial=f.serial;out.lifetime=f.pin_lifetime;out.va=f.va;out.gpu_va=f.gpu_va;
    out.width=f.width;out.view_bytes=f.view_bytes;out.heap=f.heap;out.kind=f.kind;out.stride=f.stride;
    out.bytes=span;out.dest_offset=uint64_t(role)*region_bytes;out.held=f.pin_held;out.cpu_events=f.cpu_events;
    auto refuse=[&](const char* why){out.reason=why;out.copy=false;};
    if(role>=role_count)return refuse("role-invalid");
    if(f.shape)return refuse(f.shape);
    if(!f.va||!f.view_bytes)return refuse("view-unbound");
    if(!f.address)return refuse("resource-unresolved");
    if(!f.record||!f.description)return refuse("no-creation-record");
    if(!f.buffer)return refuse("not-buffer");
    if(f.retired)return refuse("resource-retired");
    if(!f.serial)return refuse("creation-serial-unknown");
    if(f.kind==kind_placed)return refuse("placed-resource-unsupported");
    if(f.kind==kind_reserved)return refuse("reserved-resource-unsupported");
    if(f.kind!=kind_committed)return refuse("creation-kind-unknown");
    if(const char* why=copy_source_rule(f.heap))return refuse(why);
    if(!f.pin)return refuse("no-ownership-pin");
    if(f.pin_creation!=f.serial)return refuse("pin-generation-mismatch");
    if(!f.pin_finished)return refuse("pin-pending");
    if(!f.pin_held)return refuse("pin-not-held");
    if(f.pin_held!=f.address)return refuse("pin-pointer-mismatch");
    if(!span)return refuse("empty-span");
    if(span>region_bytes)return refuse("over-cap");
    if(relative>f.view_bytes||span>f.view_bytes-relative)return refuse("span-exceeds-view");
    if(!f.gpu_va||f.va<f.gpu_va)return refuse("gpu-va-unknown");
    const uint64_t base=f.va-f.gpu_va;
    if(base>f.width||relative>f.width-base)return refuse("span-exceeds-resource");
    const uint64_t offset=base+relative;
    if(span>f.width-offset)return refuse("span-exceeds-resource");
    out.offset=offset;out.reason=nullptr;out.copy=true;
}
struct Plan {
    RolePlan roles[role_count]{};
    unsigned copyable=0;
    // Draw facts for index decoding (copied at the seam).
    uint32_t topology=0,strip_cut=UINT32_MAX,index_start=0,index_count=0;
    int32_t base=0;
    uint64_t vb_view_bytes=0;uint32_t vb_stride=0; // metadata, even when VB is refused
    uint64_t list=0,generation=0,draw=0;
};
// Little-endian decode of `count` indices (stride 2 or 4) into `out`.
inline void decode(const uint8_t* bytes,uint32_t stride,uint32_t count,uint32_t* out) noexcept {
    for(uint32_t i=0;i<count;++i) {
        uint32_t v=0;for(uint32_t j=0;j<stride;++j)v|=uint32_t(bytes[size_t(i)*stride+j])<<(8*j);
        out[i]=v;
    }
}
} // namespace acz::gpu_copy::step2
