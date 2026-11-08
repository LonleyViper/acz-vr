// J-3b8 (additive to the D-230 contract in stereo_frame_source.h; that header is
// unchanged). The producer side (gpu-copy step 5, continuous per-frame cockpit
// replay) exposes its FrameSource and accepts a pose provider implemented by the
// XR owner (its try_read_pose). Both functions are safe to call from any thread.
#pragma once
#include "stereo_frame_source.h"

namespace acz::stereo {

// Implemented by the consumer side. Called by the producer on the GAME thread at
// the first replayed cockpit draw of a frame, so it must be try-only: return
// false immediately when no qualified pose snapshot is available. `out[0]` left,
// `out[1]` right, raw located OpenXR eye poses in LOCAL space (metres,
// quaternion x,y,z,w) with each eye's asymmetric FOV angles (radians).
class PoseProvider {
public:
    virtual ~PoseProvider() = default;
    virtual bool try_read(EyePose out[2], uint64_t* predicted_display_time) = 0;
    // Issue-29 additive: as try_read, plus the LOCAL space generation of the returned
    // poses, ENCODED as (PoseSnapshot::space_generation + 1); 0 = unknown. The producer
    // stores it in StereoFrame::space_generation. Default: unknown (0).
    virtual bool try_read_ex(EyePose out[2], uint64_t* predicted_display_time, uint32_t* space_generation) {
        if (space_generation) *space_generation = 0;
        return try_read(out, predicted_display_time);
    }
    // Timing-anchor diagnostics: PoseSnapshot::sequence of the snapshot the LAST successful try_read/try_read_ex returned on the
    // calling (game) thread; 0 = unknown. Diagnostic only.
    virtual uint64_t last_sequence() const noexcept { return 0; }
};

// nullptr clears. The provider must outlive its registration (clear before
// destroying it); the producer reads the pointer once per frame.
void set_pose_provider(PoseProvider* provider) noexcept;

// The producer's FrameSource while gpu_copy_step5_continuous is active, else
// nullptr. The object lives for the process lifetime once created.
FrameSource* frame_source() noexcept;

}  // namespace acz::stereo
