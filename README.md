# acz-vr: VR for the PC release of Ace Combat Zero

A research-grade VR mod for **Ace Combat Zero: The Belkan War** as shipped on PC (the copy bundled with ACE COMBAT 8). It is a
DXGI/D3D12 proxy DLL that sits between the game and the GPU. It renders the game's world, cockpit, HUD and effects in stereo
with head tracking through OpenXR. The game's executable and files are never patched.

**Status: early and unfinished.** This is a working research tree, not a release. There is no installer and no
build to download. The docs are engineering notes written while the work happened.

**This repository holds no game data.** It contains no models, textures, sounds, archives, executables or captures from
the game. The decoders below read *your own* installed copy at run time or offline. Do not open issues or pull requests
that contain extracted game files. This is a free fan project, not affiliated with or endorsed by Bandai Namco
Entertainment.

## What is here

| Path | What |
|---|---|
| `src/acz-vr/` | the proxy DLL (C++17, D3D12, OpenXR): draw capture, world/cockpit/HUD stereo re-render, effects re-projection, 360 target markers, 3D briefing table |
| `src/skip-intro/`, `src/cockpit-math/` | a small helper DLL and the JS reference maths for the camera and projection |
| `tests/acz-vr/` | offline unit tests (CTest, no game or GPU needed for most) |
| `research/world-render-data/` | Python decoders and bakers for the game's own formats: ACM v5 models from the VIF packet stream, `.mpl` part tables, GIM textures with CSM1 palettes, stage terrain, world objects |
| `research/acz-arc-mpl/` | archive and cockpit-part tools and notes |
| `docs/world-render/` | design notes and measured results for each feature (start with `model-decode.md`, `effects-outside-window.md`, `markers-360.md`, `testhmd-first-flights.md`) |
| `docs/acz-arc-mpl/` | format notes for archives and cockpit parts |
| `tools/testhmd/` | `acz_testhmd`, a SteamVR test-headset driver whose pose is set from a text file, for scripted head turns in unattended runs |
| `research/experiments/step83-verify-run1/*.ps1` | the autopilot and batch flight runner used for repeatable test flights (Windows, PowerShell 7) |
| `tools/analysis/`, `tools/ee-block-dump.ps1` | offline log analysis; a read-only memory snapshot tool for research |

Paths inside notes and scripts were written on one machine. `<repo>` stands for this repository's root, and
`D:\acz-evidence-archive` is where local run evidence was kept (not included).

## Building

Windows 10/11 and Visual Studio 2022 (MSVC, CMake 3.20+). Dependencies are not downloaded. Point CMake at local copies:

- OpenXR SDK 1.1.43: `-DFETCHCONTENT_SOURCE_DIR_OPENXR=<path>`
- OpenVR SDK 2.5.1 headers for the test headset: `-DOPENVR_SDK_DIR=<path>`

```
cmake -S src/acz-vr -B build -G "Visual Studio 17 2022" -A x64 -DFETCHCONTENT_SOURCE_DIR_OPENXR=<openxr-sdk>
cmake --build build --config Release
ctest --test-dir build -C Release
```

Two tests (`diagnostics_offline`, `shader_access_facts_offline`) need `tools/evidence/e10-drawcpt-pso.json`, which is
derived from a local frame capture and is not included. They fail without it.

## Licence

Apache-2.0 (see `LICENSE`). Third-party notices: `tools/testhmd/THIRD_PARTY.txt` (OpenVR SDK sample structure, BSD-3-Clause).
