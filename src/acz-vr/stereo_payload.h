#pragma once
#include "cockpit_math.h"
#include "draw_capture.h"
#include <optional>

namespace acz::stereo_payload {
enum class Error {
    none, missing_source, incomplete_source, inconsistent_count,
    unsupported_stride, inconsistent_stride, size_overflow, output_budget,
    invalid_matrix, allocation_failed, replacement_failed
};
struct Batch {
    // Keeps the exact immutable command/state/resource provenance alive.
    std::shared_ptr<const capture::Snapshot> source;
    // [left/right][instance], in the source's order, including duplicates.
    std::array<std::vector<std::vector<uint8_t>>, 2> eyes;
    // CPU preparation is never certification of GPU replay readiness.
};
struct Result {
    Error error;
    std::optional<Batch> batch;
    explicit operator bool() const noexcept { return error == Error::none && batch.has_value(); }
};

// Allocation-free accounting seam. Complete capture strides are 416..2048,
// multiples of four. Both eyes' payload bytes together may occupy at most
// capture::max_copy_bytes (16 MiB); equality is allowed. Metadata and the
// retained source are not new payload bytes. Failure sets bytes to zero.
Error output_size(size_t instances, size_t stride, size_t& bytes) noexcept;

// Requires explicit caller-provided row-vector eye VPs and clip->PS2-screen K.
// No viewport/depth defaults or inferred native calibration. Decodes +256,
// computes M64 = localToWorld * eyeVP and M0 = M64 * K without transposition,
// checks finite float32 range for inputs and every product/sum/output, then
// replaces only +0/+64. Ordinary float32 rounding/underflow is allowed.
// Publishes both eyes/all instances together, or no batch. Never changes the
// source, uploads bytes, schedules frames, or enables native capture/replay.
Result prepare(std::shared_ptr<const capture::Snapshot> source,
               const std::array<Mat4, 2>& eye_vp, const Mat4& clip_to_screen);
} // namespace acz::stereo_payload
