#include "srt_payload.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace acz::srt {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "SrtData requires IEEE-754 float32");

const char* error_message(Error error) noexcept {
    switch (error) {
    case Error::none: return "success";
    case Error::null_source: return "null source";
    case Error::unsupported_stride: return "stride is below the declared 416-byte layout";
    case Error::inconsistent_length: return "byte length must equal one complete element stride";
    case Error::size_overflow: return "payload size or source address range overflows";
    case Error::nonfinite_matrix: return "replacement matrix contains a nonfinite value";
    case Error::allocation_failed: return "payload allocation failed";
    }
    return "unknown error";
}

namespace {
bool finite(const Matrix& matrix) {
    for (float value : matrix) if (!std::isfinite(value)) return false;
    return true;
}

void write_matrix(std::uint8_t* target, const Matrix& matrix) {
    for (std::size_t i = 0; i < matrix.size(); ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, &matrix[i], sizeof(bits));
        for (unsigned byte = 0; byte < 4; ++byte)
            target[i * 4 + byte] = static_cast<std::uint8_t>(bits >> (byte * 8));
    }
}
} // namespace

Result replace_matrices(const void* source, std::size_t byte_length,
                        std::size_t stride, const Matrix& first,
                        const Matrix& second) {
    if (!source) return {Error::null_source, {}};
    if (stride < declared_size) return {Error::unsupported_stride, {}};
    if (byte_length != stride) return {Error::inconsistent_length, {}};
    // Check before allocation, copying or pointer arithmetic. Requiring a
    // representable one-past address avoids wrapping the source range.
    const auto address = reinterpret_cast<std::uintptr_t>(source);
    if (stride > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) ||
        stride > std::vector<std::uint8_t>{}.max_size() ||
        stride > (std::numeric_limits<std::uintptr_t>::max)() - address)
        return {Error::size_overflow, {}};
    if (!finite(first) || !finite(second)) return {Error::nonfinite_matrix, {}};

    Result result;
    try {
        result.bytes.resize(stride);
    } catch (const std::length_error&) {
        return {Error::size_overflow, {}};
    } catch (const std::bad_alloc&) {
        return {Error::allocation_failed, {}};
    }
    std::memcpy(result.bytes.data(), source, stride);
    write_matrix(result.bytes.data(), first);
    write_matrix(result.bytes.data() + matrix_size, second);
    return result;
}
} // namespace acz::srt
