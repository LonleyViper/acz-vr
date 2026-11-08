# Ace Combat Zero VR feasibility — preliminary inspection

Inspected 11 October 2026. Target supplied by the user:
`D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero`.

**Current direction, 12 October 2026:** the user selected the native PC release
and requires full six-degree head tracking, stereo and headset-correct FOV.
See [native VR reference projects and approach](Native-6DOF-reference-projects.md).
The [earlier emulation comparison](Existing-work-and-emulation.md) is retained
as background research; it is not the selected implementation direction.

## Assessment

REA is a suitable investigation aid, subject to configuring a native analysis
provider. It can expose assembly, pseudocode, references, imports and types.
It is not a VR injector. A successful result would still require a custom camera /
rendering modification and an OpenXR integration.

This first pass supports further investigation, not a claim that VR is feasible
or that a camera hook has already been found. The best next milestone is controlled
cockpit camera rotation, followed by a two-eye desktop proof of concept.

## Evidence from this installation

| Observation | Implication / limit |
| --- | --- |
| `acz.exe`, 4,037,304 bytes, version 1.0.2.1, product ACE COMBAT ZERO: THE BELKAN WAR | Identifies the inspected build. |
| PE32+, machine 0x8664, no CLR directory | Native x64 Windows executable; this alone does not distinguish a native port from an emulator. |
| Imports `d3d12.dll` ordinal 101 and `dxgi.dll!CreateDXGIFactory2` | D3D12/DXGI graphics initialization. Ordinal 101 resolves to `D3D12CreateDevice` in this computer's system DLL; runtime use has not been observed. |
| Native C++ RTTI names including `Unit`, `WeaponExPlayerLaserCtrl`, aircraft selection classes, `PS2Model`, `BaseViewer` | Strong evidence of game-specific systems in the native binary, consistent with a port or hybrid rather than only a generic emulator launcher. Exact architecture remains unverified. |
| `ps2AtestAZ`, `ps2AtestCA`, `ps2Line`, `shader/` | Suggests retained PS2 rendering concepts. Does not establish whether GPU draws receive world-space geometry or already projected vertices. |
| `briefing_isCameraMoving` | A possible static-reference starting point. This is a briefing clue, not a located flight camera. |
| Build path ending in `Gaming.Desktop.x64_Release\acz.pdb` | The executable contains a PDB path; no PDB was present in the supplied folder. |
| `.text` entropy approximately 8 bits/byte, `.bind` entry point and high-entropy `.bind` data | Code appears packed/encrypted. Steam wrapping is a plausible explanation, not a confirmed packer identification. Static decompilation of disk `.text` may be misleading. |
| Five `.arc` files with common first bytes `c3 58 47 45` | Shared archive format; not identified or extracted. Archive extraction is not the first dependency for camera experiments. |
| `settings/dinputSample.ini` | DirectInput mapping examples. No VR settings found in this folder. |

Executable SHA-256:
`8aa8ffc9a185c58d45b34bc1116d8e1880ea785cc6fa3fa7d6a421fffeb9c739`.

The detailed import inventory, sections, selected string offsets, RTTI class
offsets and archive header samples are in `aczero-triage.json`. All recorded
string/RTTI offsets are file offsets, not hook addresses. RVAs and preferred
image base are recorded separately; runtime ASLR must be accounted for.

## Where REA fits

The project's Windows Ghidra guide documents experimental, read-only native PE
analysis with Ghidra 12.1.x and a full x64 JDK 21 or newer. Its memory operations
refer to the imported analysis image, not a live game process. Treat runtime
attachment, camera intervention and graphics interception as separate tooling.

Local Node is v24.16.0, which satisfies the published package's engine range.
The npm registry reported `rea-agents` 6.4.0 during this inspection. REA and Java
were not on PATH, and a limited check of common installation directories did not
find Ghidra. This was not an exhaustive disk search. No REA/native provider was
installed or executed in this pass.

Recommended setup for a subsequent investigation: a project-local pinned REA
package plus Ghidra and a JDK, then `rea doctor --json`. The CLI can be used
without changing agent configuration; MCP is useful for retaining an analysis
session across queries. Check the installed package's provider support rather
than assuming every feature on repository main is released.

## Experiments and success criteria

1. **Observe one normal cockpit frame.** Launch through the normal Steam route
   with capture instrumentation present before D3D12 device creation. Use a
   compatible graphics debugger such as PIX. Identify command queue, draw passes,
   shader inputs, constant buffers and depth. Determine whether 3D transforms
   happen on the GPU or in a retained CPU/PS2-style geometry path. This decides
   whether shader-level intervention is viable or an earlier camera hook is needed.

2. **Recover a usable camera path.** Observe normally loaded code after startup
   if disk code is opaque. Trace runtime camera data and its writers; use REA /
   Ghidra on a suitable analysis image where supported. Compare controlled
   look-left/look-right and camera-mode changes. Establish position, orientation,
   projection, timing and coordinate conventions. Make a small reversible yaw
   change. Success means the world rotates correctly, controls are unaffected,
   and visibility/culling behaves consistently. Briefing/model-viewer classes
   are search leads, not proven cockpit hooks.

3. **Prove stereo on the desktop.** Render two eye views of the same simulation
   state with distinct eye positions and asymmetric projections. Confirm that
   nearby cockpit geometry has appropriate disparity and distant scenery remains
   stable. Do not advance physics once per eye. Test frustum culling and effects.
   Replaying a D3D12 command list is insufficient if transforms were baked into
   uploaded vertices or buffer contents; that case needs earlier intervention.

4. **Integrate OpenXR.** Use runtime-predicted head poses and per-eye FOVs,
   compose poses with the aircraft/cockpit reference frame, render into OpenXR
   swapchains, synchronize D3D12 queues/resources and submit projection layers.
   Start with head rotation and recentering. Add positional tracking after world
   scale and cockpit limits are validated. Keep ordinary game controls initially.

5. **Handle presentation and performance.** Separate HUD/menus from the world,
   test cockpit near clipping, sky/fog/effects and cutscenes, and measure CPU/GPU
   frame cost. Two views can exceed the game's original frame budget. Cockpit
   rotation and stereo can be useful milestones even if complete six-degree
   positional tracking or high native refresh rates prove impractical.

The largest unresolved question is access to geometry/camera before projection
and visibility decisions. Capturing the final backbuffer in a headset establishes
a virtual screen, not stereoscopic cockpit VR. Depth-based reconstruction could
be investigated as a fallback, with separate quality limitations.

UEVR targets supported Unreal Engine games. Nothing in this inspection establishes
that this separate ACZero executable uses Unreal; do not choose an injector based
on the parent AC8 installation folder alone. Likewise, a DX11-specific wrapper
does not directly match the observed D3D12 imports.

## Reproduce this pass

From the project folder, using Python 3:

```powershell
python tools/inspect_aczero.py 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero' --output research/aczero-triage.json
```

The script reads the executable and 256-byte archive headers. It writes only the
requested JSON output. Its PE parser is a small triage utility for this input,
not a hardened general-purpose parser or decompiler. The run succeeded, and a
separate PowerShell `Get-FileHash` matched its executable digest. No game files
were changed, the game was not launched, and no process was injected or patched.

## Primary sources checked

- [REA repository](https://github.com/morluto/rea)
- [REA Windows Ghidra boundary and prerequisites](https://github.com/morluto/rea/blob/main/docs/windows-ghidra-p0.md)
- [REA native-analysis workflow](https://rea.tools/guides/native/)
- [Microsoft PIX capture prerequisites](https://devblogs.microsoft.com/pix/taking-a-capture/)
- [Khronos hello_xr D3D12 implementation](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/tests/hello_xr/graphicsplugin_d3d12.cpp)
- [Valve Steam wrapper documentation](https://partner.steamgames.com/doc/features/drm)
- [UEVR's supported engine scope](https://github.com/praydog/UEVR)
- [3Dmigoto DX11 scope](https://github.com/bo3b/3Dmigoto)
