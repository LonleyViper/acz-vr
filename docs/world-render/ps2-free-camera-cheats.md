# PS2-era Ace Combat Zero free-camera cheats: what they tell us (research only)

Status: web reading plus existing repo evidence. No game launch, no build, no write.
Tags: [W] web evidence (URL given), [R] repo evidence, [I] inference.
Base URLs: wiki https://github.com/deaththed0g/ACZGCT/wiki ; scripts
https://github.com/deaththed0g/ACZGCT/tree/main/ACZGCT%20lua (files 00, 01, 02, 04, 06 read).

## 0. What exists
- ACZGCT is a Cheat Engine table plus a PNACH, for PCSX2, serial SLUS-21346 (US/NTSC-U) only [W: repo README, script var `SLUS_21346_check`]. No SLES/SLPM ids appear [W]. The wiki pages (00..03) list features but NO addresses [W]; all addresses are in the Lua files.
- All offsets are relative to the PCSX2 EE RAM base (EE address 0), so offset = PS2 virtual address [W: scripts use `getAddress(0x20000000)` / `pcsx2-qt.EEmem`].
- The PNACH is only in the Releases page (not fetched; no binary downloads). It holds code patches (max-LoD flags, "GAMEPLAY camera codes"); no values seen [W: wiki [00] PNACH]. No other ACZ freecam/FOV/widescreen pnach or CodeBreaker camera code was found: a search for a PCSX2 ACZ widescreen patch found nothing; almarsguides only lists aircraft/weapon/money CodeBreaker/AR codes (https://almarsguides.com/retro/walkthroughs/ps2/games/acecombatzerothebelkanwar/codebreaker/). PCSX2 replaced the old widescreen pnach system in 1.7.4546 (forums.pcsx2.net); I could not confirm an ACZ widescreen entry in pcsx2_patches [unverified].

## 1. Mechanism and fields
The freecam is NOT a PS2-side code patch and NOT a pnach conditional. It is a Cheat Engine host-side hack [W: 00/01 scripts]:
1. A CE write breakpoint on the camera rotation float (`struct+0xC20` gameplay, `+0x40` hangar) catches the host x86 instruction (PCSX2 JIT output) that writes it; its bytes are saved.
2. Those host instructions are overwritten with 0x90 (NOP), so the game's own camera update stops landing in EE RAM.
3. A 50 ms CE timer writes position/angles/projection directly. Game input (`+0x3F70B8`, 8 bytes) is zeroed; pause byte `+0x7651E8` (5 pause / 4 run) is used.
[I] So per-frame external writes alone lose to the game's rewrite (our D-269: 119/120 overwritten); the cheat authors had to kill the writer. A PC-port equivalent of step 2 is game-code patching, which our rules forbid.

Struct located by AOB inside EE range +0x800000..+0x1F00000 (not a fixed address; "ACZcoord_temp"). Offsets relative to that hit [W: 01 script]:
- `+0x00/+0x04` float 512.0 / 448.0 foreground screen W/H; `+0x1C0/+0x1C4` background W/H; `+0x198/+0x19C` "projection distortion factors" W/H (the arrow-key pincushion/barrel). Clamps: max (512//W)*4, min (512//W)//16.
- `+0xB30..+0xB50` nine floats of position (TPS B30/34/38, anchor B3C/40/44, cockpit B48/4C/50); `+0xC20/+0xC24/+0xC28` pitch, yaw, roll.
- Hangar variant: `+0x30/34/38` position, `+0x40/44/48` pitch/yaw/roll, same `+0x198/+0x19C` and `+0/+4` [W: 00 script].
- TPS chase distance: single float `struct+0xB38`, +-5 per step [W: 02 script].
- Gameplay AOB begins `00 00 ?? 44 00 00 ?? ??` and contains `00 02 C0 01 00 00 80 3F` (16-bit 0x0200 = 512 and 0x01C0 = 448, then 1.0f); hangar AOB `00 00 ?? 44 00 00 ?? 43 00 00 00 44 00 00 80 3F` [W].
Fixed EE offsets [W]: mission byte `+0x3FFD1C` (255 = not in mission in scripts 01/02/06; 3 = playing in 04), MP gate `+0x3ACEA0`==13 (single player), hangar id `+0x3FDF2C` (39..42), phase `+0x76587C` (3/15/23 allowed), pause `+0x7651E8`, lighting floats `+0x38F740`, `+0x38DCF0`, `+0x3F80E8`, shadow `+0x3FC030`, HUD byte `+0x3FFBCF`, game-id AOB `18 B7 3D 00 88 44 3F 00 18 B7 3D 00 68 45 3F 00` in +0x300000..+0x4000000. Free movement mode (06): `+0x765231` (0 enable, 2 default), speed `+0x3F7228` (255 default, higher = slower).

FOV / lens: NO field-of-view, near or far plane value exists in the camera scripts [W: hangar and TPS script reports state it]. The only projection knobs are the 512/448 screen scale pair and the `+0x198/+0x19C` distortion pair. [I] The 512x448 floats are the screen-space projection scale (cf. our shader constant (512,-448,2048,2048), E2); scaling them changes focal length in pixels, so they are the practical "lens" of the pre-projected world path. The authors call distortion "very unstable/crash-prone" (wiki [01] CAMERA).

## 2. Draw distance, LoD, clipping (what extends the world)
- No widescreen/FOV hack in ACZGCT. "Force maximum LoD for NPC objects" (check `+0x3F7800` = 0x3E000000), "terrain LoD" (check byte `+0x15AFDA`==19), hangar LoD flag `+0x13C664` are pnach flags; the patch values are not shown [W].
- **`+0x765320` float: script writes 262144.0 as "Terrain clipping fix?", default 0x477FFF00 (about 65535.0)** [W: 04 script tail]. [I] Best lead for a far clip / terrain distance; not a horizontal frustum.
- Per-stage 864-byte environment blocks (found via TOC entries 19, 38, 39 of the stage asset; AOB `27 00 00 00 A0 00 00 00 A0 01 00 00`, scan +0x700000..+0x1F00000) carry draw distance, foliage distance, light and cloud values. Known writes: `+0x00` cloud shadowmap, `+0x1FC` cloud transparency, `+0x200` cloud amount, `+0x300` dynamic shadow [W]. Draw-distance field offsets inside the block are not labelled (data arrays unread); the wiki says "Extended modifications" adjusts draw/foliage distance [W: wiki [02]].
- [I] Nothing widens the horizontal/vertical frustum or the sideways culling. No cheat solves "render outside 63x38 deg"; the free camera works because the game culls for the moved camera.

## 3. Mapping to our PC port
Evidence [R]: E6 (research/experiments/E6-2026-10-12.md) found a 64 MiB private RW block: look yaw `+0xFDD410`, aircraft pos `+0xFDD42C..34`, heading `+0xFDD448`, camera pos `+0xFDD860..68`, composed rot (pitch,yaw,roll) `+0xFDD870..78`, render-side copy `+0x312E30..50`. E7 external writes had no effect; D-269: look yaw overwritten 119/120 (an OUTPUT). The block base moves per launch (D-268); D-247 found static/stale copies in other 64 MiB blocks (`static-not-tracking`); the tracks-look locator is needed.
Inferences:
- The cheat struct lies in EE range 0x800000..0x1F00000 and our `0xFDD410..0xFDD878` (about 16.6 MB) is inside it, so our block offsets plausibly equal PS2 virtual addresses (EE RAM = offset 0).
- Our camera pos -> rot gap (`0xFDD860` -> `0xFDD870`) is 0x10, matching the cheat's cockpit pos/rot spacing if pos is struct+0xC10 and rot struct+0xC20 (guess: struct base about `0xFDCC50`; then 512/448 at `+0xFDCC50/54`, distortion `+0xFDCDE8/EC`, bg `+0xFDCE10/14`). Their documented cockpit position (+0xB48..B50) would be `0xFDD798`, which does not match, so treat as a hypothesis only.
- Our block is 64 MiB while EE RAM is 32 MiB: the port's image layout may differ. The fixed gates are a free test: if `+0x3FFD1C`, `+0x3ACEA0`, `+0x76587C` and the game-id bytes read as expected, PS2 addresses translate with base = block start; if not, the port uses another layout.
- Our render-side copy `+0x312E30` is outside the cheat's scan range (+0x800000..).
- The NOP mechanism does not map (x64 PC code, not PCSX2 JIT; no code patching).

## 4. Experiments (read-only first; rules respected)
R1 (read-only, extend dump_e6 / the planned `ee_lens_scan` in src/acz-vr/ee_camera_scan.cpp): in the tracks-look 64 MiB block read once the game-id 16 bytes in +0x300000..+0x4000000 and the bytes at `+0x3FFD1C`, `+0x3ACEA0`, `+0x76587C`, `+0x3FDF2C`, `+0x7651E8`. Match = PS2 addresses translate.
R2 (read-only): scan +0x800000..+0x1F00000 for the gameplay signature (floats 512.0 = 0x44000000 and 448.0 = 0x43E00000 pair at struct +0/+4, 16-bit 512/448 then 1.0f), log floats at struct `+0x0,+0x4,+0x198,+0x19C,+0x1C0,+0x1C4`, `+0xB30..B50`, `+0xC20..C28` for 300 frames (static / per-frame / 1.0 or not). Check whether struct+0xC20 equals `+0xFDD870` (ties E6 fields to the cheat struct).
R3 (read-only): log `+0x765320` (expect 0x477FFF00) and `+0x765231`, `+0x3F7228`; locate the stage asset by the 12-byte environment AOB and dump the 864-byte blocks (TOC entries 19, 38) to see which floats change per mission/draw-distance.
R4 (value hunt): besides tan 0.6152/0.3460, look near the struct for 512.0/448.0-scaled lens values; if `+0x198/+0x19C` read 1.0 and nothing in the cockpit SrtData lens changes, the lens is composed per frame.
Write experiments, only after user go-ahead (null driver, data only, bounded, restore on every exit): W1 `+0x765320` 65535 -> 262144.0 (the cheat value), compare world/terrain draw counts; W2 `+0x198/+0x19C` 1.0 -> 0.9 for a few ticks per docs/review/lens-fov-widen-plan.md, logging the overwrite ratio (expect the game to rewrite unless it is stored). Not allowed: NOPing camera writers, IAT, code patching.

## 5. Unverified
PNACH contents and LoD patch values; PS2-address translation in the port; whether `+0x198/+0x19C` is stored or per-frame; meaning of `+0x765320`; EU/JP addresses (cheats are SLUS-21346 only); the 04 script's draw-distance float offsets.
