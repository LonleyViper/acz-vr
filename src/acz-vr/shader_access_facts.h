#pragma once
#include "pso_stage_inventory.h"
#include "draw_capture.h"

namespace acz::shader_access {
// E11 main: VS %57 -> %58 extracts x; PS %17 -> %18 extracts y,
// %22 -> %23 extracts x. Literal register 0, no dynamic component selection.
// Valid ONLY after authenticating the corresponding E10 full container SHA-256
// and shader HASH (lookup below, or the hook's existing shader_identity match).
inline constexpr uint32_t e11_vs_cb10_words=0x1, e11_ps_cb10_words=0x3;
enum class ResourceClass { cbv, srv_structured, srv_typed, sampler };
struct Resource { ResourceClass resource_class; uint32_t reg, space, count, psv_type, psv_kind; };
struct ByteRange { uint32_t begin, end; }; // half-open; conservative 16-byte DXIL loads
struct SrtAccess { uint32_t reg=10, space=0, element=0, element_size=416; const ByteRange* ranges=nullptr; size_t count=0; };
struct Record {
    metadata::ShaderStage stage;
    metadata::ShaderIdentifier shader_hash;
    const char* container_sha256;
    const char* evidence_ids;
    const Resource* resources; size_t resource_count;
    const SrtAccess* srt;
    bool cbv_register_zero; // CBufferLoadLegacy register index 0, at binding cb10
    uint32_t cb10_consumed_words=0; // exact downstream consumed-component mask
};
struct Lookup {
    const Record* record=nullptr; // immutable process-owned table; never caller storage
    std::string rejection;
    explicit operator bool() const noexcept { return record!=nullptr; }
};
// No caller-supplied SHA is accepted. SHA-256 is recomputed over ALL owned bytes,
// the decoded HASH is checked, then the PSV0 resource multiset is checked exactly.
bool known_hash(const metadata::ShaderIdentifier&) noexcept;
Lookup lookup(const pso::Stage&) noexcept;
// Descriptor/whole-resource bounds for the constant element (FirstElement + 0).
// Empty string means valid. This checks bytes, not synchronization or GPU replay.
struct IndexedRead {
    std::vector<uint64_t> elements;
    std::string rejection;
    explicit operator bool() const noexcept { return !elements.empty(); }
};
// A partial indexing fact, never a complete capture/vertex-fetch proof. Uses
// only a lookup-owned record and current fixture/caller descriptor/resource bounds.
IndexedRead index_srt(const Record&,const capture::Descriptor&,uint64_t resource_bytes,
                      const capture::Command&) noexcept;
std::string validate_srt(const Record&,const capture::Descriptor&,uint64_t resource_bytes);
}
