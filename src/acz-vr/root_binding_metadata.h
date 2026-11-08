#pragma once
#include "shader_metadata.h"
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <array>

namespace acz::metadata {
inline constexpr uint32_t append_range=0xffffffffu;
// Values map to SDK D3D12 enums. Serialized input is decoded only by the SDK's
// CPU deserializer; only header extents are checked before the SDK call.
enum class RootVersion : uint32_t { v1_0=1, v1_1=2 };
enum class Visibility : uint32_t { all=0, vertex=1, hull=2, domain=3, geometry=4, pixel=5 };
enum class BindingKind : uint32_t { srv=0, uav=1, cbv=2, sampler=3 };
enum class ParameterKind : uint32_t { table=0, constants=1, cbv=2, srv=3, uav=4 };
struct DescriptorRange {
    BindingKind kind=BindingKind::srv;
    uint32_t count=1, base_register=0, space=0, flags=0, offset=append_range;
};
struct RootParameter {
    ParameterKind kind=ParameterKind::table;
    Visibility visibility=Visibility::all;
    uint32_t shader_register=0, space=0, flags=0, constant_count=0;
    std::vector<DescriptorRange> ranges;
};
struct RootLayout {
    RootVersion version=RootVersion::v1_0;
    // SDK root-signature flags: IA/stream-output and classic stage deny flags
    // supported, including AS/MS deny flags (no AS/MS query support).
    // Local, directly indexed and unknown flags rejected.
    uint32_t flags=0, static_sampler_count=0;
    std::vector<RootParameter> parameters;
};
struct BindingResolution {
    bool valid=false;
    std::string rejection;
    ParameterKind parameter_kind=ParameterKind::table;
    uint32_t root_index=0, range_index=0, table_offset=0;
};
struct SerializedRootResult;
class RootBindingMetadata {
public:
    static RootBindingMetadata from_adapter(const RootLayout& input);
    // Readable, stable caller storage is required for this call only. No input
    // or SDK pointers survive. Bound: 64 KiB bytes, 64 parameters, 64 ranges per
    // table. Versions 1.0/1.1 only; adapter restrictions remain unchanged.
    // Header checks reject impossible extents, not arbitrary malformed bodies.
    // Default static-sampler rejection is unchanged. The hook's explicit
    // diagnostic opt-in supports only one serialized PS-visible s0, space 0.
    // No device/GPU is involved.
    static constexpr size_t max_serialized_bytes=64*1024;
    static SerializedRootResult from_serialized(const void* bytes, size_t length, bool diagnostic_static_s0=false) noexcept;
    // Only SDK-decoded, serialized single s0 (space 0, PS-visible) is retained.
    // from_adapter cannot manufacture this provenance. Constant-time Draw query.
    const std::array<uint32_t,13>* serialized_ps_s0() const noexcept {
        return valid()&&serialized_s0_?&static_s0_words_:nullptr;
    }
    bool valid() const { return rejection_.empty(); }
    const std::string& rejection() const { return rejection_; }
    const RootLayout& layout() const { return layout_; }
    BindingResolution resolve(BindingKind kind, uint32_t reg, uint32_t space, ShaderStage stage) const;
    BindingResolution resolve_srt_data() const { return resolve(BindingKind::srv,10,0,ShaderStage::geometry); }
private:
    RootBindingMetadata() = default;
    bool serialized_s0_=false;
    std::array<uint32_t,13> static_s0_words_{};
    RootLayout layout_;
    std::string rejection_="metadata not initialized";
};
enum class SerializedRootFailure {
    none, null_input, empty_input, oversized_input, sdk_rejected,
    missing_description, unsupported_version, parameter_budget,
    missing_parameters, range_budget, missing_ranges, unsupported_layout,
    allocation_failure, exception_failure,
    // Appended to preserve every existing failure's numeric value.
    truncated_header, header_bounds
};
struct SerializedRootResult {
    // The code remains usable even when allocation prevents a detail string.
    SerializedRootFailure failure=SerializedRootFailure::none;
    // SDK failure is reported literally: the SDK does not expose a finer
    // parse diagnostic. sdk_rejected includes unsupported input representation
    // (e.g. a bare RTS0 part payload). No layout facts are inferred on failure.
    int32_t sdk_hresult=0;
    std::string detail; // Exact adapter rejection when unsupported_layout.
    std::optional<RootBindingMetadata> metadata; // Published only on success.
    bool valid() const { return failure==SerializedRootFailure::none && metadata.has_value(); }
};
} // namespace acz::metadata
