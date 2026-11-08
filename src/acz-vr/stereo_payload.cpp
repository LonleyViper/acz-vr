#include "stereo_payload.h"
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace acz::stereo_payload {
namespace {
bool valid(double v) {
    return std::isfinite(v) && std::abs(v) <= (std::numeric_limits<float>::max)();
}
bool valid(const Mat4& m) {
    for (double v : m) if (!valid(v)) return false;
    return true;
}
Mat4 decode_world(const std::vector<uint8_t>& bytes) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "SrtData requires IEEE float32");
    Mat4 result{};
    for (size_t i = 0; i < 16; ++i) {
        uint32_t bits = 0;
        for (unsigned b = 0; b < 4; ++b)
            bits |= uint32_t(bytes[256 + 4*i + b]) << (8*b);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        result[i] = value;
    }
    return result;
}
// Deliberately checks each term and accumulation, including cancellation that
// could otherwise hide an out-of-range intermediate. Row-vector row order.
bool product(const Mat4& a, const Mat4& b, Mat4& out) {
    for (size_t r = 0; r < 4; ++r) for (size_t c = 0; c < 4; ++c) {
        double sum = 0;
        for (size_t k = 0; k < 4; ++k) {
            const double term = a[4*r+k] * b[4*k+c];
            if (!valid(term)) return false;
            sum += term;
            if (!valid(sum)) return false;
        }
        out[4*r+c] = sum;
    }
    return true;
}
Result fail(Error e) { return {e, std::nullopt}; }
}

Error output_size(size_t count, size_t stride, size_t& bytes) noexcept {
    bytes = 0;
    if (!count) return Error::inconsistent_count;
    if (stride < srt::declared_size || stride > 2048 || stride % 4)
        return Error::unsupported_stride;
    const size_t maximum = (std::numeric_limits<size_t>::max)();
    if (count > maximum / stride || count*stride > maximum / 2)
        return Error::size_overflow;
    const size_t total = count*stride*2;
    if (total > capture::max_copy_bytes) return Error::output_budget;
    bytes = total;
    return Error::none;
}

Result prepare(std::shared_ptr<const capture::Snapshot> source,
               const std::array<Mat4, 2>& eye_vp, const Mat4& clip_to_screen) {
    if (!source) return fail(Error::missing_source);
    if (!source->cpu_data_complete) return fail(Error::incomplete_source);
    const size_t count = source->srt_elements.size();
    if (!count || count != source->command.instances) return fail(Error::inconsistent_count);
    const size_t stride = source->srt_elements.front().size();
    size_t bytes;
    const auto budget = output_size(count, stride, bytes);
    if (budget != Error::none) return fail(budget);
    for (const auto& element : source->srt_elements)
        if (element.size() != stride) return fail(Error::inconsistent_stride);
    if (!valid(eye_vp[0]) || !valid(eye_vp[1]) || !valid(clip_to_screen))
        return fail(Error::invalid_matrix);
    try {
        Batch pending;
        pending.source = source;
        for (auto& eye : pending.eyes) eye.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto world = decode_world(source->srt_elements[i]);
            if (!valid(world)) return fail(Error::invalid_matrix);
            for (size_t eye = 0; eye < 2; ++eye) {
                Mat4 clip{}, screen{};
                if (!product(world, eye_vp[eye], clip) || !product(clip, clip_to_screen, screen))
                    return fail(Error::invalid_matrix);
                auto payload = capture::eye_payload(*source, i, toFloat32(screen), toFloat32(clip));
                if (!payload) return fail(payload.error == srt::Error::allocation_failed
                    ? Error::allocation_failed : Error::replacement_failed);
                pending.eyes[eye].push_back(std::move(payload.bytes));
            }
        }
        return {Error::none, std::move(pending)};
    } catch (const std::bad_alloc&) { return fail(Error::allocation_failed); }
      catch (const std::length_error&) { return fail(Error::allocation_failed); }
}
} // namespace acz::stereo_payload
