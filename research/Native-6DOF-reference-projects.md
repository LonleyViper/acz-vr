# Native Ace Combat Zero: lessons for full 6 DOF VR

Selected direction on 12 October 2026: modify the installed native x64/D3D12
ACZero release. Acceptance requires head orientation and translation, correct
stereo and headset-specific eye projections. Research only so far; no camera
hook, runtime injection or VR output has been demonstrated.

## The AC7 stack and transferable lessons

[UEVR](https://github.com/praydog/UEVR) supplies engine stereo rendering and
headset movement through OpenXR/OpenVR. Its synchronized sequential mode renders
both eyes on one engine tick. The relevant lesson is to produce both views from
one simulation state; alternating different simulation frames is a poor target
for this project. Unreal-specific entry points do not transfer to Zero merely
because both games are Ace Combat.

The open-source
[Keton AC7 compatibility plugin](https://github.com/keton/ace-combat-uevr)
addresses game integration on top of UEVR. Inspection of
[Plugin.cpp](https://github.com/keton/ace-combat-uevr/blob/master/src/Plugin.cpp)
found a camera setup state machine, cockpit/cutscene transitions, pawn-change
cache refresh and a manual recovery button. Its particular fix stops and
restarts a camera-shake routine through reflected Unreal functions. Transfer the
transition/recovery design, not the AC7-specific shake workaround. The plugin's
own UI rendering callbacks are not an independent stereo engine.

[Kosnag's Nexus compatibility mod](https://www.nexusmods.com/acecombat7skiesunknown/mods/2387)
documents cockpit/MFD/HUD work and camera-shake fixes, plus reused PSVR-specific
features. This shows that usable instruments, cockpit geometry and camera
behaviour need separate treatment beyond the injector. Zero has no established
equivalent VR-ready assets or code. Its existing cockpit mesh must be inspected
for visibility, missing surfaces and clipping when leaning.

## Native graphics and runtime references

[REFramework](https://github.com/praydog/REFramework) is a useful reference for
VR integration in another native engine with D3D11/D3D12 support. It remains
engine-specific, not a Zero-ready injector. Relevant source boundaries:

- [D3D12Hook.cpp](https://github.com/praydog/REFramework/blob/master/src/D3D12Hook.cpp): native graphics interception.
- [D3D12Component.cpp](https://github.com/praydog/REFramework/blob/master/src/mods/vr/D3D12Component.cpp): VR graphics integration.
- [OpenXR.cpp](https://github.com/praydog/REFramework/blob/master/src/mods/vr/runtimes/OpenXR.cpp): frame lifecycle, predicted-time poses, runtime eye FOVs and projection-layer submission.

The smaller
[Khronos hello_xr D3D12 sample](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/tests/hello_xr/graphicsplugin_d3d12.cpp)
is a useful baseline for graphics binding, compatible adapter selection,
swapchain resources and drawing. It does not discover or rerender Zero's scene.

## Architecture to investigate in Zero

Separate three responsibilities:

1. **Game adapter:** identify the aircraft/seat transform, the flight camera's
   orientation and position, projection, cockpit pass and visibility decisions.
   Recognize gameplay, cockpit changes, cutscenes and mission/checkpoint reloads.
   Apply changes only with valid game state and refresh stale references.
2. **Stereo renderer:** render the scene for two eye poses with independent
   asymmetric projections from one simulation state. Preserve resource lifetimes
   and synchronization, and ensure cockpit/world/effects use consistent views.
   A Present hook provides access to the finished image, but is insufficient
   on its own to generate missing eye views.
3. **OpenXR adapter:** obtain predicted-time eye poses/FOVs, convert units and
   handedness, apply seated recentering, coordinate frame lifecycle and submit
   completed eye images as a projection layer.

Conceptually, an eye's world pose is the aircraft pose composed with the fixed
seat pose and the recentered tracked eye pose. If OpenXR eye poses already
contain head movement, do not apply that head transform a second time.
Head translation moves the viewpoint within the aircraft reference frame;
it must not translate the aircraft or drag the cockpit mesh with the viewer.

The game must render with the same eye poses/FOVs represented in the submitted
OpenXR layer. Increasing a single desktop FOV or copying one image to two
swapchains does not fulfill this requirement.

## First experiments and completion criteria

1. Capture one cockpit frame and determine whether transforms are GPU matrices
   or CPU-projected vertices. Locate the camera before culling/projection.
2. On the desktop, control all six camera components independently. Translate
   forward, sideways and vertically; confirm parallax between cockpit, canopy
   and scenery while the aircraft state remains unchanged. Also test rotations.
3. Render two simultaneous eye views with correct scale and distinct projections.
   Confirm terrain/sky/effect visibility for both eyes and near cockpit geometry.
4. Replace synthetic poses with recentered OpenXR poses. Verify physical leaning,
   looking over a shoulder and stable cockpit instruments. Measure frame timing.
5. Re-test after mission restarts, cutscenes, camera-mode switches and aircraft
   changes. Test independent cockpit rendering if the original game uses a
   special near-camera or projection path.

The decisive unresolved risk is access to an early enough scene/camera path.
Runtime head poses are the standard part; making this renderer produce the
correct scene from those poses is the game-specific work. REA/Ghidra can help
map that path, complemented by a runtime debugger and graphics captures.

This plan learns from existing projects without assuming their executables,
Unreal properties, hooks, asset formats or camera fixes apply to Zero.
