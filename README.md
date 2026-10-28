# BtSTweaks 0.1.0

HDR add-on for **Life is Strange: Before the Storm Remastered** (PC, Steam), for ReShade with
add-on support. The game only outputs SDR. The add-on turns its finished picture into HDR that
keeps the game's colour grade, with settings in an in-game overlay.

- **HDR**: the game's frame is shown on a 16-bit float (scRGB) swap chain. Below **Highlights
  start at** (a share of SDR white, default 50%; lower is punchier) the picture stays as graded;
  above it, the brightest parts are stretched towards **Peak brightness**. **Paper white** sets
  how bright SDR white is (there's a button that uses the peak Windows reports for your display).
  Windows HDR must be on. Switching HDR on or off takes effect after a restart.
- **UI at paper white**: menus, prompts and subtitles aren't brightened, both in menus and during
  play.
- **Advanced**: status, a count of the frames where the UI was kept apart, debug views that tint
  what counts as UI, and a one-frame draw log for diagnostics.

No graphics settings yet: the game is Unity (IL2CPP) and has no console variables to work through.

## Install

1. Install ReShade 6.8 or later with full add-on support for
   `Life is Strange - Before the Storm.exe` (DirectX 10/11/12).
2. Copy `BtSTweaks.addon64` next to that exe.
3. Open ReShade (Home key): the BtSTweaks window opens beside it, and you can move and resize it.
   If you close it, turn it back on from the add-on's entry in ReShade's Add-ons tab.

Settings are saved in `ReShade.ini` under `[BtSTweaks]` (`Hdr`, `HdrPeak`, `HdrPaperWhite`,
`HdrHighlightStart`, `ShowWindow`). Log: `BtSTweaks.log` next to the game's exe.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools and the Windows SDK; the shader is compiled with fxc);
`.\build.ps1 -Install` copies the add-on into the game folder (game closed).

## How it works

The game renders to an 8-bit swap chain. With Windows HDR on, the add-on creates it as
R16G16B16A16_FLOAT instead and makes the game's 8-bit views of it float, so the game's final blit
writes linear light. At each present a full-screen pass (`src/hdr.hlsl`) re-encodes the frame,
decodes it with gamma 2.2, places it at paper white, stretches the highlights and writes scRGB.
The pass runs in its own D3D11 device context state, so Unity's cached bindings aren't disturbed.
`docs/FINDINGS.md` has the game's frame structure and how the UI is found.
