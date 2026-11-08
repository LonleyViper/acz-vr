#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace acz::metadata {
enum class ShaderStage : uint16_t { pixel=0, vertex=1, geometry=2, hull=3, domain=4, compute=5 };
enum class HashSemantics { binary, includes_source };
using ShaderIdentifier = std::array<uint8_t,16>;
inline constexpr ShaderIdentifier drawcpt_geometry_identifier{
    0x06,0x08,0x4b,0x62,0x62,0x83,0x9d,0x7b,0x33,0x6b,0x1d,0x8d,0x0d,0x0b,0x86,0xb8};
struct ShaderMetadata {
    bool valid = false;
    std::string rejection;
    ShaderStage stage = ShaderStage::pixel;
    uint8_t major = 0, minor = 0;
    HashSemantics hash_semantics = HashSemantics::binary;
    ShaderIdentifier identifier{};
    // Opaque header checksum, NOT the disassembler identifier. Neither digest
    // is recomputed/authenticated here. Valid means structurally valid metadata,
    // not valid executable LLVM bitcode or trusted shader content.
    ShaderIdentifier container_checksum{};
    bool matches_drawcpt_geometry() const;
};
// DXBC-container/DXIL-program/HASH metadata only; SHDR/SHEX are unsupported.
// Supports only SM6.0 with DXIL1.0. Newer models/versions fail closed until
// their format correspondence is independently established; valid is not
// a claim that all DXIL shader models are supported.
// Input must be readable for size bytes. Output owns all retained values.
ShaderMetadata decode_shader_metadata(const void* bytes, size_t size);
} // namespace acz::metadata
