# What ac0vr teaches us (read 2026-11-07)

Source: https://github.com/kulakovri/ac0vr (Apache-2.0). It is a static recompile of the PS2 US disc (SLUS-21346)
with its own Vulkan renderer. It hooks game functions by PS2 address (`ps2_hook_before/replace`). Read: README,
runtime/src/zero_vr.c, runtime/shaders/smooth_warp.frag and xr_depth.frag. Nothing was copied. [W] = read in their
source; [I] = our inference.

## The constraint that decides what carries over
- D-295: in the PC port, PS2 addresses do not map. Their absolute addresses (functions, 0x3B3810 save base and so on)
  are not usable here.
- Their *relative* struct layouts might still hold if the PC port keeps the game's data structures [I, untested].
  Examples:
  - target system TS = player+0x1880;
  - unit records of 0x4A0 bytes, with +0x260 position, +0x270 angles and +0x1F8 flags;
  - save slot: +0x5F48 mission, +0x5F4C stage, +0x5F50 branch [W].
- We do not patch game code, so their approach of wrapping a game call (camera update, FX camera, target selection)
  has no direct equivalent for us.

## Leads ranked by value to our open items
1. **Unit table by signature scan** [I]. Scan the E6 block for an array with a 0x4A0 stride that has float position
   triples at +0x260 and moves like the radar contacts. A hit would give true world positions for every contact. That
   unlocks native target boxes in 360 (no inference from the HUD), lock and target state (TS+0x98/0x99 current target),
   and a read-only gaze-target indicator. This test is read-only and fits our rules.
2. **Save-slot mission index** [I]. If the PC save keeps the slot layout (mission +0x5F48, stage +0x5F4C), editing a
   *copy* of the save offline could start other missions. That would unblock the kill, plume and lock-diamond checks
   that mission 03 cannot provide. We need to find the PC save format first. Keep the original save and restore it
   byte-identically, as in the chain186 wrappers.
3. **Depth layer to the compositor** (XR_KHR_composition_layer_depth) [W: xr_depth.frag]. They decode game depth to
   metres per region:
   - cockpit near;
   - HUD combiner at infinity (collimated);
   - sky and 2D overlays at infinity;
   - world through the game's projection.
   This gives runtime reprojection real depth and may reduce the briefing-table and box "swim" the user reported.
4. **In-between frames** [W: smooth_warp.frag]. 60 fps game frames are reprojected per pixel with depth to 90/120 Hz
   using a fixed-point inverse search, with separate handling for moving objects and the cockpit layer. This is
   relevant to the headset frame-rate complaint, and it is GPU design work (opus).
5. **FX sprite size and billboard angles in a head-turned view** [W]. Their effects were 2.46x too large because the
   sprite size used the game's projection distance while the matrix used the eye's, and billboards and the snow box
   followed the nose, not the head. Check our re-projected smoke and explosions for the same two mistakes (sprite
   scale from the game's lens; facing the nose instead of the eye).
6. **HUD at infinity** [W]. They treat combiner symbols as collimated (infinite depth) while the glass stays at cockpit
   depth. This is a reference for our HUD and box depth choice.
7. **Mission/stage table** [W]: mission index -> stage index (with ace and operation branches), found in DATA.PAC.
   Useful to cross-check our stage fingerprinting (stages-and-detection.md).
8. Smaller ideas:
   - head-tracked 3D sound (rotate the listener by head yaw);
   - gaze targeting (the target button picks the contact nearest the gaze within ~20 deg, and re-presses cycle through
     that cone);
   - G effects;
   - an ENHANCEMENTS menu where every feature can be switched off;
   - a debug boot straight into mission N.

## What we already do better
We use the game's own cockpit and exterior models for all 36 aircraft (they use FlightGear stand-ins for 21 of them).
We have measured eye points. Our smoke and explosions are re-projected in world space (they keep flat sprites at a
slightly wrong depth). We have test-HMD scripted head poses for unattended runs. See [the issue we posted](https://github.com/kulakovri/ac0vr/issues/1).
