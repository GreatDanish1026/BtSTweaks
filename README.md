# BtSTweaks 0.2.0

Graphics and HDR add-on for **Life is Strange: Before the Storm Remastered** (PC, Steam), for
ReShade with add-on support. Raises the game's graphics beyond its menu and turns its SDR picture
into HDR that keeps the game's colour grade, live, from an in-game overlay.

- **HDR**: the game's frame is shown on a 16-bit float (scRGB) swap chain. Below **Highlights
  start at** (a share of SDR white, default 50%; lower is punchier) the picture stays as graded;
  above it, the brightest parts are stretched towards **Peak brightness**. **Paper white** sets
  how bright SDR white is (there's a button that uses the peak Windows reports for your display).
  Windows HDR must be on. Switching HDR on or off takes effect after a restart. Menus, prompts and
  subtitles stay at paper white.
- **Graphics**: **16x texture filtering** (the game leaves anisotropic filtering off), **Shadow
  distance** and **Detail distance** (LOD bias) as 1x to 4x the game's own per-scene values, and
  the **Sun shadow map** at 4096 or 8192 (the game uses its "High" tier).
- **Effects**: **Ambient occlusion** at 16 samples and full resolution where a scene uses it, or
  in every scene (the game: 10 samples at half resolution, off in many scenes); **Sharper
  reflections** where a scene has screen-space reflections; **Temporal anti-aliasing** instead of
  FXAA; **High-resolution sun shafts**.
- **F8** switches every graphics and effects option off and on, to compare.
- **Advanced**: HDR status and debug views, a one-frame draw log, the main-thread hook's status,
  and read-only dumps of Unity's quality settings, the live camera and effect objects, and the
  game's settings classes.

Every option starts at the game's own value. Each one follows the game when it changes the value
(a new scene, the game's menu) and puts the game's value back when switched off. Tested at 4K with
everything on and a frame cap at the monitor's refresh rate: no frames lost (GPU at about 55%);
the uncapped cost isn't measured.

## Install

1. Install ReShade 6.8 or later with full add-on support for
   `Life is Strange - Before the Storm.exe` (DirectX 10/11/12).
2. Copy `BtSTweaks.addon64` next to that exe.
3. Open ReShade (Home key): the BtSTweaks window opens beside it, and you can move and resize it.
   If you close it, turn it back on from the add-on's entry in ReShade's Add-ons tab.

Settings are saved in `ReShade.ini` under `[BtSTweaks]` (`Hdr`, `HdrPeak`, `HdrPaperWhite`,
`HdrHighlightStart`, `Anisotropic`, `ShadowDistance`, `LodBias`, `SunShadow`, `Ao`, `Ssr`, `Taa`,
`SunShaftsHigh`, `ShowWindow`). Log: `BtSTweaks.log` next to the game's exe.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools and the Windows SDK; the shader is compiled with fxc);
`.\build.ps1 -Install` copies the add-on into the game folder (game closed).

## How it works

**HDR**: the game renders to an 8-bit swap chain. With Windows HDR on, the add-on creates it as
R16G16B16A16_FLOAT instead and makes the game's 8-bit views of it float, so the game's final blit
writes linear light. At each present a full-screen pass (`src/hdr.hlsl`) re-encodes the frame,
decodes it with gamma 2.2, places it at paper white, stretches the highlights and writes scRGB.
The pass runs in its own D3D11 device context state, so Unity's cached bindings aren't disturbed.

**Graphics**: the game is Unity 2019.4 with IL2CPP, and `GameAssembly.dll` exports the IL2CPP
runtime API. The add-on finds classes and Unity's internal calls by name, and runs its work on the
game's main thread by pointing the `Update` method of a few always-present classes (SteamManager
first) at its own function, which calls the original first. Only those entry points in the
runtime's metadata change; no game code is written, and they're put back when the add-on unloads.

`docs/FINDINGS.md` has the frame structure, the UI split, the settings classes and the values found.
