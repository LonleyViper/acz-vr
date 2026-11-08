#pragma once
#include "buffer_binding_registry.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace acz::native_evidence {
// This is an on-demand metadata inventory, never a copy, proof, or replay input.
// Callers must retain these expectations independently of this invocation.
enum class Outcome : uint8_t { Refused, MetadataObserved };
enum class Provenance : uint8_t { Unknown, ObservedMetadata, ProviderDeclared };
// A complete recording boundary was observed at list creation or at Reset.
// This does not assert that a Reset call occurred on the creation path.
enum class Coverage : uint8_t { Unknown, CreationOrResetBoundaryObserved };
enum class Category : uint8_t {
    TableBuffer, RootCbv, RootSrv, RootConstants,
    IA, Texture, Sampler, OpaqueWritable
};
enum UnknownDomain : uint32_t {
    ContentVersion=1u<<0, CompleteWriteHistory=1u<<1,
    WriterExclusion=1u<<2, DeviceEpoch=1u<<3,
    QueueLifetimeEpoch=1u<<4, SubmissionLinkageOrder=1u<<5,
    DrawOrdinal=1u<<6, CompleteShaderRequirements=1u<<7,
    StateRestoration=1u<<8, GpuCompletion=1u<<9,
    Ownership=1u<<10
};
inline constexpr uint32_t permanent_unknown_mask=(1u<<11)-1;
inline constexpr size_t max_resources=64, max_selections=128;
inline constexpr uint32_t max_root_words=64;

struct ExpectedPoint {
    uint64_t list=0, generation=0, revision=0, device=0;
    uint64_t root=0, pso=0;
};
struct Selection {
    Category category=Category::TableBuffer;
    uint64_t root=0; // independently retained root lifetime for this binding
    uint32_t parameter=0;
    // Table: heap is the bound destination heap, offset is relative to the
    // bound table. Root constants: offset/count is the exact requested range.
    uint32_t offset=0, count=0;
    binding::Token resource, heap;
    uint32_t slot=0;
    uint64_t revision=0;
    // Root descriptor provider declaration: bytes for CBV, elements/stride
    // for SRV. Address is the independently retained bound GPU VA token.
    uint64_t address=0;
    uint32_t bytes=0, elements=0, stride=0;
};
struct Limits { size_t resources=max_resources, selections=max_selections; uint32_t words=max_root_words; };
struct ResourceRecord {
    binding::Token token;
    uint64_t bytes=0, device=0;
    Provenance token_origin=Provenance::Unknown, extent_origin=Provenance::Unknown,
               device_origin=Provenance::Unknown;
};
struct Entry {
    Category category=Category::TableBuffer;
    uint32_t parameter=0, offset=0;
    binding::Token resource, heap, source_heap;
    uint32_t slot=0, source_slot=0;
    uint64_t revision=0, source_revision=0;
    capture::Descriptor descriptor;
    uint64_t resource_bytes=0, device=0;
    uint64_t address=0;
    uint32_t bytes=0, elements=0, stride=0;
    std::array<uint32_t,max_root_words> words{};
    uint32_t word_count=0;
    Provenance binding_origin=Provenance::Unknown,
               descriptor_origin=Provenance::Unknown,
               resource_origin=Provenance::Unknown,
               destination_origin=Provenance::Unknown,
               source_origin=Provenance::Unknown,
               layout_origin=Provenance::Unknown,
               containment_origin=Provenance::Unknown,
               words_origin=Provenance::Unknown;
};
struct Export {
    Outcome outcome=Outcome::Refused;
    const char* reason="Refused"; // static literal, never an owned logger string
    uint32_t unknown_mask=permanent_unknown_mask;
    ExpectedPoint recording;
    Provenance recording_origin=Provenance::Unknown;
    Coverage coverage=Coverage::Unknown;
    std::array<ResourceRecord,max_resources> resources{};
    std::array<Entry,max_selections> entries{};
    size_t resource_count=0, entry_count=0;
};
// No COM calls, GPU access, mutation, logging, or automatic production caller.
Export export_metadata(const ExpectedPoint&, const Selection*, size_t,
                       Limits limits={}) noexcept;
} // namespace acz::native_evidence
