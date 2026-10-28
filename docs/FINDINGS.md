# Findings: Life is Strange: Before the Storm Remastered

## The game

- Steam app 1265930. Folder `LIS - Before the Storm Remastered`, exe
  `Life is Strange - Before the Storm.exe`. **Unity 2019.4.36f1**, IL2CPP (global-metadata v24),
  built-in render pipeline.
- DirectX 11, feature level 11_1. `boot.config`: `hdr-display-enabled=0`; `nolog` (no Player.log).
- Swap chain: **R8G8B8A8_UNORM**, 2 buffers, flip discard, flags 0x842. Unity creates views of it
  as R8G8B8A8_UNORM_SRGB (render target) and UNORM (render target and shader resource).
- Post-processing: Unity Post-Processing Stack v1 (colour grading with a user LUT, ACES/Neutral
  tonemapping, eye adaptation, bloom, depth of field, grain, vignette, chromatic aberration, TAA,
  FXAA/SMAA) plus legacy image effects (SunShafts, GlobalFog, CameraMotionBlur).
- No console variables. Graphics settings would need IL2CPP calls (QualitySettings and the game's
  profile fields) on the main thread. The prefs file `PrefData.Save` is encrypted; the registry key
  `HKCU\Software\Square Enix\Life is Strange: Before the Storm` holds only Screenmanager keys.

## The swap chain upgrade

In `create_swapchain` the add-on changes the format to R16G16B16A16_FLOAT when the requested
format is 8-bit and the window's display has Windows HDR on (DXGI output colour space G2084).
In `create_resource_view`, 8-bit views of the back buffers become R16G16B16A16_FLOAT. The game's
blit goes through its sRGB view, so on the float back buffer the frame arrives as linear light
(the sRGB curve undone). Unity accepts the change: no black screen and no errors. The swap chain
is then set to scRGB (`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`) and ReShade is told
`extended_srgb_linear`.

## The HDR pass

At present: copy the back buffer, re-encode to sRGB, decode with gamma 2.2, then
`y -> knee + (y - knee) * (1 + s * t^2)` above the knee on luminance (continuous with slope 1;
SDR white lands on peak / paper white), RGB scaled together to keep hue, output
`rgb * paperWhite / 80`. The pass swaps in its own `ID3DDeviceContextState`
(`CreateDeviceContextState`) and swaps back after, so Unity's cached state is untouched.

Measured (2026-10-28, 4K, display peak 3700 nits, paper white 200): the parking lot scene keeps
the game's look; lamp posts, parking lines, signs and studs reach about 3,500 nits.

## Where the UI is drawn

Two cases, both handled; pixels that count as UI skip the highlight stretch.

**Menus**: the UI is drawn into its own multisampled 8-bit target, resolved into a screen-size
texture, then composited before the blit. The add-on takes the last screen-size 8-bit resolve
destination of the frame as the UI layer and uses its alpha.

**In play** (draw log, 2026-10-28): the scene is rendered into a screen-size **R16G16B16A16**
buffer H. The frame ends like this:

1. post-processing into other targets (bloom, LUT and others);
2. one quad into H: the post-processing composite;
3. the UI into H (the "Announcement" prompt: 6, 84 and 6 indices), with target binds but no draws
   elsewhere in between;
4. one quad from H into an 8-bit picture L (tonemap and grade);
5. one quad from L into the back buffer.

L is also bound at the very start of each frame, before anything is drawn. The add-on learns H
and L at the back buffer blit (the last two targets drawn into before it, H being 16-bit float).
Each frame it counts the run of draws into H since the last draw elsewhere (draws into L don't
end the run). At a run's second draw it copies H, which is then the picture without UI. At the
blit, if the last run had two or more draws, the copy is compared with H's final state in the
pass, and pixels that differ count as UI. Without UI the last run is the composite quad alone,
and no pixel is held back.

**Unity keeps H upside down**: the final blit flips it. The pass reads H and the copy with the y
axis flipped. The first test showed a mirrored "Announcement" in the tint view.

Result (2026-10-28, paper white 203): "Use WASD to walk" and "Announcement" went from 3,700 nits
to 203; the tint view marks only the prompts; scene highlights still stretch. Cost: a
screen-size FP16 copy for each run of two or more draws into H (about three per frame in the
logged scene, including the transparent pass), plus the pass's own back buffer copy (not measured).
