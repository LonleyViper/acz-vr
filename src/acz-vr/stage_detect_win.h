#pragma once
// stage_detect_win.h -- xr_terrain_stage=auto: READ-ONLY scan of this process's committed, readable, non-GPU memory for the stage
// fingerprints (stage_fingerprint.h). No writes, no code patching, no hooks: VirtualQuery + SEH-guarded copies into a private buffer
// (the same discipline as the ee_camera/lens scans). The scan is bounded and prioritised (stage_scan.h: likely regions first,
// decisive early stop, byte/time caps, yields, resident pages only). docs/world-render/stages-and-detection.md section 3.
#include "stage_fingerprint.h"
#include "stage_scan.h"
#include <cstdint>
#include <string>

namespace acz::stagefp {

// Enumerates, orders and scans once; the Matcher accumulates (call decide() afterwards). `st` is reset first.
void scan_process(Matcher& m, const ScanLimits& lim, ScanStats& st) noexcept;

} // namespace acz::stagefp
