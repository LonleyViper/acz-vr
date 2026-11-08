#pragma once
#include "root_binding_metadata.h"
#include <memory>

namespace acz::pso {
using metadata::ShaderStage;
enum class StageState { unknown, explicitly_absent, present };
enum class RootProvenance { unknown, explicit_root, embedded_container };
struct SignatureElement {
    std::string semantic;
    uint32_t stream=0, index=0, system_value=0, component_type=0, reg=0, min_precision=0;
    uint8_t mask=0, read_write_mask=0;
};
struct ResourceBinding {
    uint32_t type=0, space=0, lower=0, upper=0;
    std::optional<uint32_t> kind, flags;
};
struct PsvElement {
    std::string semantic;
    std::vector<uint32_t> indices;
    uint8_t rows=0, start_row=0, cols_and_start=0, semantic_kind=0;
    uint8_t component_type=0, interpolation=0, dynamic_mask_and_stream=0;
};
struct DependencyTable { std::string name; std::vector<uint32_t> words; };
struct RuntimeStageInfo {
    std::optional<uint32_t> input_primitive, output_topology, output_stream_mask;
    std::optional<uint32_t> input_control_points, output_control_points, tessellator_domain, tessellator_output;
    std::optional<uint8_t> output_position, depth_output, sample_frequency;
};
struct Psv {
    uint32_t version=0, runtime_size=0, binding_stride=0;
    // Owned literal union bytes (VSInfo/HSInfo/DSInfo/GSInfo/PSInfo), not interpreted code.
    std::array<uint8_t,16> stage_info{};
    RuntimeStageInfo runtime_stage;
    uint32_t minimum_wave=0, maximum_wave=0;
    std::optional<uint8_t> shader_stage, uses_view_id;
    uint16_t max_vertices_or_patch_vectors=0;
    uint8_t input_vectors=0;
    std::array<uint8_t,4> output_vectors{};
    std::optional<std::array<uint32_t,3>> threads;
    std::optional<std::string> entry;
    std::vector<uint8_t> string_table;
    std::vector<uint32_t> semantic_indices;
    std::vector<ResourceBinding> bindings;
    std::array<std::vector<PsvElement>,3> signatures; // input/output/patch
    std::vector<DependencyTable> dependencies;
};
struct Stage {
    StageState state=StageState::unknown;
    std::vector<uint8_t> container;
    metadata::ShaderMetadata shader;
    std::optional<Psv> psv;
    std::optional<std::vector<SignatureElement>> input, output;
    std::optional<std::vector<uint8_t>> rts0;
    std::optional<metadata::SerializedRootResult> root;
};
struct InputElement {
    std::string semantic;
    uint32_t index=0, format=0, slot=0, offset=0, classification=0, step_rate=0;
};
struct StageInput {
    ShaderStage stage=ShaderStage::vertex;
    StageState state=StageState::unknown;
    const void* bytes=nullptr;
    size_t size=0;
};
struct Request {
    std::vector<StageInput> stages;
    std::vector<InputElement> input_layout;
    RootProvenance root_provenance=RootProvenance::unknown;
    uint64_t explicit_root_identity=0; // caller lifetime assertion, never a pointer
    ShaderStage embedded_root_stage=ShaderStage::vertex;
};
struct Inventory {
    std::array<Stage,5> stages; // VS, PS, DS, HS, GS
    std::vector<InputElement> input_layout;
    RootProvenance root_provenance=RootProvenance::unknown;
    uint64_t explicit_root_identity=0;
    ShaderStage embedded_root_stage=ShaderStage::vertex;
    const Stage& stage(ShaderStage) const; // throws on unsupported stage
};
enum class Failure { none, invalid_input, malformed, allocation, exception };
struct Result {
    std::shared_ptr<const Inventory> inventory;
    Failure failure=Failure::none;
    std::string rejection;
    explicit operator bool() const noexcept { return bool(inventory); }
};
// CPU-only caller seam. Stable readable inputs required during call only.
// Bounds: five stages, 64 KiB/container, 32 IA elements, 255 bytes/IA semantic.
// Publishes a single immutable owned value or nothing, including on allocation failure.
// No resource-use/indexing proof, capture readiness, hooks, GPU or XR operations.
Result build(const Request&) noexcept;
}
