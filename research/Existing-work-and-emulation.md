# Existing work and PC port versus PS2 emulation

Research checked 11 October 2026. No tools or mods installed and no game launched.

**Superseded target choice, 12 October 2026:** the user chose the native PC
release with full 6 DOF. The comparison below remains historical research.
See [the selected native approach](Native-6DOF-reference-projects.md).

## Recommendation

Prototype Ace Combat Zero against PCSX2 first. This is an engineering judgment
based on available source, debugging access and reusable stereo/head-tracking
work, not a benchmark result. Keep the native PC release as a second target for
performance and distribution convenience once the camera requirements are known.

## What already exists

### PenguinScreen2: the closest relevant camera work

[PenguinVRLab/PenguinScreen2](https://github.com/PenguinVRLab/PenguinScreen2) is
a PCSX2 fork with OpenXR, stereo rendering and per-game camera profiles. Its
documented deployment targets desktop Linux/SteamOS using WiVRn. The public
release API reports `v1.0-rc2`, published 3 August 2026, as a prerelease, with a
Flatpak asset. Do not treat this as a verified turnkey Windows solution.

The current source includes an
[Ace Combat 5 US profile](https://github.com/PenguinVRLab/PenguinScreen2/blob/main/bin/resources/vr-profiles/SLUS-20851.yaml)
that uses a memory signature, validation and cockpit-state guards, and writes
floating-point head pitch/yaw/roll to camera-relative fields. It also contains
stereo, cockpit-depth and HUD tuning. This is concrete reusable design evidence;
its addresses, signatures and HUD rules cannot simply be renamed for Zero.

The checked shipped profile directory contains only `SLUS-20090.yaml`,
`SLUS-20851.yaml` and `SLUS-20919.yaml`, plus documentation/license files. There
is no shipped Zero profile there. That is a directory observation, not proof
that nobody has an unpublished or out-of-tree profile.

The
[quickstart](https://github.com/PenguinVRLab/PenguinScreen2/blob/main/QUICKSTART.md)
distinguishes virtual-screen, stereo and head-camera tiers. The current
[XRCompositor.cpp](https://github.com/PenguinVRLab/PenguinScreen2/blob/main/pcsx2/VR/XRCompositor.cpp)
submits eye-specific quad or cylinder surfaces for game images. A stereoscopic
screen with game-camera tracking is useful, but does not by itself establish
full per-eye, runtime-FOV cockpit rendering or positional tracking. We have not
tested the headset result. Current main also differs from the older release;
use a pinned revision when evaluating it.

### AndrewGDX/pcsx2-stereoscopic: practical Windows stereo baseline

[Version 0.1.1](https://github.com/AndrewGDX/pcsx2-stereoscopic/releases/tag/v0.1.1),
published 6 March 2026, includes a `PCSX2-win-x64.zip` asset. The author's release
notes describe stereo across rendering backends, adjustable UI depth, and
per-game tuning, with known effects/shadow issues. It is explicitly work in
progress. It establishes a Windows stereo starting point, not verified Zero
compatibility or complete headset camera support.

### Geo-11 / earlier GSdx stereo work

There is an older
[PCSX2 DX11 stereo modification by masterotaku](https://helixmod.blogspot.com/2018/01/pcsx2-dx11.html)
and a creator-maintained
[GSdx stereoscopy discussion](https://forums.pcsx2.net/Thread-Gsdx-3D-Stereoscopy-Patch?pid=588371).
[Geo-11](https://github.com/ThreeDeeJay/geo-11/releases) targets Direct3D 11 and
offers stereo output. These are useful alternatives for a stereo-screen trial;
their exact current emulator-version and Zero compatibility need testing. The
blog body could not be retrieved in this pass, so no claim is made about a
Zero-specific profile in its downloadable archive.

### Zero-specific existing knowledge

An
[ultrawide patch author's post](https://forums.pcsx2.net/Thread-PCSX2-Ultrawide-Eyefinity-Patches?page=12)
identifies first/third-person FOV and aspect-ratio fields for the US Zero build,
`SLUS-21346`. The author also reports cockpit culling limitations. These are
search leads rather than a finished VR camera patch. The retrieved search
extract includes first-person FOV at EE `0x003FA350`; this was not validated
against a game image or used to write memory.

Searches for Zero VR, PCSX2 VR, Zero stereo, geo-11 and the new PC executable
found no verified public finished Zero cockpit VR mod for either target.
This is a bounded search result, not a claim of exhaustive absence. AC5's
camera implementation is the strongest close precedent found.

## Comparison

| Factor | PS2 game in PCSX2 | Native ACZero PC executable |
| --- | --- | --- |
| Renderer access | Source available; can alter stereo rendering and presentation directly. | D3D12 observed locally; hooks and renderer behavior need reverse engineering. |
| Camera investigation | Guest-memory search, disassembly and state inspection in the built-in debugger; controlled comparisons and savestates help repeat experiments. | Game-specific RTTI is promising, but disk code appears opaque and runtime camera recovery is still required. |
| Existing work | Two stereo forks, legacy stereo wrappers, and AC5 camera-profile design. | No verified Zero-specific VR solution found in this search. |
| Initial work | Validate stereo, locate Zero camera fields, build its profile, then integrate/test headset view. | Discover camera, intercept native renderer, build stereo and headset presentation. |
| Performance | Additional emulation cost; actual frame budget must be measured. | Potentially more CPU headroom, but no comparison benchmark performed. |
| Setup | Requires a PS2 BIOS and a PS2 game disc/image. The PC `.arc` files are not established as an emulator-ready game image. | Uses the supplied installed release directly. |
| Final VR quality | Game camera/culling still require intervention. Existing stereo does not imply six-degree tracking or correct headset projections. | Also needs camera/culling/HUD intervention; a usable native render loop could be advantageous. |

The
[official PCSX2 debugger guide](https://pcsx2.net/docs/advanced/debugger/)
documents guest memory searches, disassembly, registers and ELF symbol import.
The
[patch guide](https://pcsx2.net/docs/advanced/writing-patches/)
documents writes to the EE/IOP address spaces. This allows work against the
PS2 game's state rather than discovering PCSX2's host JIT addresses.

Emulation does not make stereo universal: its rasterizer receives the results
of the game's earlier geometry work. Moving projected vertices for stereo
cannot restore geometry the game has already culled. For an immersive view,
find camera/projection/culling upstream and confirm visibility when looking
around and, eventually, moving the head.

## Revised next experiment

On this Windows machine, use an isolated AndrewGDX stereo build to establish
whether Zero's cockpit, terrain, sky, clouds and HUD can produce acceptable
desktop stereo. Pair that with the PCSX2 debugger to locate Zero's cockpit
camera angles and mode guards. Study PenguinScreen2's AC5 profile and generic
camera driver as the design reference. A port of its OpenXR/camera integration
to Windows, or a Linux trial, is a separate choice after stereo is validated.

For full cockpit VR, the success criterion is head rotation with stable stereo,
correct field of view, visibility and HUD behavior. A virtual-screen demo is
a useful intermediate result and should be identified as such.

REA remains potentially useful for static game-code analysis. Its documented
Windows Ghidra provider admits native PE targets, not PS2 MIPS ELF binaries, so
it is not the primary entry point for this emulator experiment. Start with
PCSX2's guest debugger; investigate a suitable PS2-aware static analysis setup
only as needed.
