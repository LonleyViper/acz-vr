#include "srt_payload.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace acz::srt;
static int checks = 0, failures = 0;
static void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", label); }
}

static Matrix matrix(float base) {
    Matrix result{};
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = base + float(i) * 0.25f;
    result[3] = -0.0f;
    return result;
}

static void verify_copy(std::size_t stride, bool unaligned) {
    std::vector<std::uint8_t> storage(stride + 1, 0xa5);
    auto* source = storage.data() + (unaligned ? 1 : 0);
    // Matrix bytes all differ from the little-endian bytes of these small floats.
    // Tail sentinel changes at every offset, including all six trailing float4s.
    for (std::size_t i = 128; i < stride; ++i) source[i] = std::uint8_t(i * 37 + 11);
    const std::vector<std::uint8_t> original(source, source + stride);
    const auto first = matrix(1), second = matrix(-20);
    const auto other_first = matrix(50), other_second = matrix(-70);
    auto left = replace_matrices(source, stride, stride, first, second);
    auto right = replace_matrices(source, stride, stride, other_first, other_second);
    check(bool(left) && bool(right), "complete source accepted");
    if (!left || !right) return;
    check(left.bytes.size() == stride && right.bytes.size() == stride, "entire stride owned");
    check(std::equal(original.begin(), original.end(), source), "both calls leave source immutable");
    // Independent expected encoding, using known little-endian MSVC float32.
    std::vector<std::uint8_t> expected = original;
    std::memcpy(expected.data(), first.data(), 64);
    std::memcpy(expected.data() + 64, second.data(), 64);
    check(left.bytes == expected, "exact first and second matrix bytes in row order");
    std::memcpy(expected.data(), other_first.data(), 64);
    std::memcpy(expected.data() + 64, other_second.data(), 64);
    check(right.bytes == expected, "right uses its own matrices and original bytes");
    bool changed = true, preserved = true;
    for (std::size_t i = 0; i < stride; ++i) {
        if (i < 128) changed &= left.bytes[i] != original[i] && right.bytes[i] != original[i];
        else preserved &= left.bytes[i] == original[i] && right.bytes[i] == original[i];
    }
    check(changed, "exactly all 128 sentinel matrix bytes replaced");
    check(preserved, "all remaining bytes including trailing 96 and extra stride preserved");
    const auto saved_left = left.bytes, saved_right = right.bytes;
    std::fill(source, source + stride, std::uint8_t{0});
    check(left.bytes == saved_left && right.bytes == saved_right, "later source mutation cannot alter owned copies");
    left.bytes[400] ^= 0xff;
    check(right.bytes == saved_right, "eye payloads have independent storage");
}

static void invalid_inputs() {
    std::vector<std::uint8_t> source(416, 0x79);
    const auto snapshot = source;
    auto first = matrix(1), second = matrix(2);
    auto rejects = [&](const void* data, std::size_t length, std::size_t stride,
                       const Matrix& a, const Matrix& b, Error error) {
        const auto result = replace_matrices(data, length, stride, a, b);
        check(!result && result.error == error && result.bytes.empty(), error_message(error));
        check(source == snapshot, "failure leaves original unmodified");
    };
    rejects(nullptr, 416, 416, first, second, Error::null_source);
    for (std::size_t n : {std::size_t(0), std::size_t(128), std::size_t(384), std::size_t(415)})
        rejects(source.data(), n, n, first, second, Error::unsupported_stride);
    rejects(source.data(), 384, 416, first, second, Error::inconsistent_length);
    rejects(source.data(), 415, 416, first, second, Error::inconsistent_length);
    rejects(source.data(), 417, 416, first, second, Error::inconsistent_length);
    rejects(source.data(), 416, 512, first, second, Error::inconsistent_length);
    const auto max_size = (std::numeric_limits<std::size_t>::max)();
    rejects(source.data(), max_size, max_size, first, second, Error::size_overflow);
    const auto too_large = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) + 1;
    rejects(source.data(), too_large, too_large, first, second, Error::size_overflow);
    // Fabricated range is rejected before dereference; this is not runtime memory access.
    const auto high = reinterpret_cast<const void*>((std::numeric_limits<std::uintptr_t>::max)() - 100);
    rejects(high, 416, 416, first, second, Error::size_overflow);
    for (float value : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()}) {
        for (std::size_t i = 0; i < 16; ++i) {
            auto invalid = first;
            invalid[i] = value;
            rejects(source.data(), 416, 416, invalid, second, Error::nonfinite_matrix);
            rejects(source.data(), 416, 416, first, invalid, Error::nonfinite_matrix);
        }
    }
    first[0] = (std::numeric_limits<float>::max)();
    second[15] = std::numeric_limits<float>::denorm_min();
    check(bool(replace_matrices(source.data(), 416, 416, first, second)), "finite extreme values accepted without calibration");
}

int main() {
    verify_copy(416, false);
    verify_copy(416, true);
    verify_copy(512, true);
    verify_copy(433, true);
    invalid_inputs();
    std::printf("SrtData offline checks: %d passed, %d failed. Synthetic byte tests only.\n",
                checks - failures, failures);
    return failures ? 1 : 0;
}
