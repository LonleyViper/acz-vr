#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace acz::srt {

// e2-drawcpt-gs declares five float4[4] plus six float4: 416 bytes.
inline constexpr std::size_t declared_size = 416;
inline constexpr std::size_t matrix_size = 64;
using Matrix = std::array<float, 16>; // Row-major, row-vector convention; no transpose.

enum class Error {
    none, null_source, unsupported_stride, inconsistent_length,
    size_overflow, nonfinite_matrix, allocation_failed
};

struct Result {
    Error error = Error::none;
    std::vector<std::uint8_t> bytes;
    explicit operator bool() const noexcept { return error == Error::none; }
};

const char* error_message(Error error) noexcept;

// Source must name exactly one complete element, byte_length == stride >= 416.
// Any larger byte stride is supported, including an odd stride; this is a byte
// library, not validation of a GPU descriptor. Caller guarantees readable memory
// and an immutable source for the duration of the call. No pointer-only API can
// verify allocation bounds. Unaligned source is supported.
// Returns an independent owned copy, replacing only [0,64) and [64,128).
// Float32 values are serialized little-endian as supplied, without calibration.
// Failure returns an empty payload; neither source nor matrices are modified.
// Does not resolve bindings, instance indices, GPU resources or missing bytes.
Result replace_matrices(const void* source, std::size_t byte_length,
                        std::size_t stride, const Matrix& first,
                        const Matrix& second);

} // namespace acz::srt
