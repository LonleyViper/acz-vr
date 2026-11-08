#pragma once
#include "probe_core.h"
#include <algorithm>
#include <optional>
#include <vector>

namespace acz::probe {
constexpr int64_t rgba8_unorm = 28;
constexpr int64_t rgba8_srgb = 29;

// LATER colour phases only; phase 1 never invokes this selector. An offered
// sRGB swapchain alone does NOT encode arbitrary memory writes. A compatible
// sRGB RTV encodes linear render/clear values once; never pre-encode those values.
// Never silently substitute a BGRA or floating-point format.
inline std::optional<int64_t> select_later_colour_format(const std::vector<int64_t>& formats) {
    for (auto preferred : {rgba8_srgb, rgba8_unorm})
        if (std::find(formats.begin(), formats.end(), preferred) != formats.end()) return preferred;
    return std::nullopt;
}

// Shared real/fake two-call seam. enumerate must report whether XrResult
// succeeded; the real caller retains its timing and sticky-loss observations.
// Availability is informational, including an empty list. Invalid/changed
// counts and call failures are protocol failures, with no retry or selection.
template<class Enumerate>
bool observe_phase1_formats(Enumerate enumerate, const Log& log) {
    uint32_t count = 0;
    if (!enumerate(0, &count, nullptr) || count > Caps::enumeration_limit) return false;
    std::vector<int64_t> formats(count);
    if (count) {
        uint32_t actual = 0;
        if (!enumerate(count, &actual, formats.data()) || actual != count) return false;
    }
    log("enumeration xrEnumerateSwapchainFormats count=" + std::to_string(count));
    bool unorm = false, srgb = false;
    for (auto format : formats) {
        log("swapchain_format=" + std::to_string(format));
        unorm |= format == rgba8_unorm;
        srgb |= format == rgba8_srgb;
    }
    log(std::string("format_observation rgba8_unorm_28=") + (unorm ? "1" : "0") +
        " rgba8_srgb_29=" + (srgb ? "1" : "0") + " informational_only=1");
    return true;
}
}
