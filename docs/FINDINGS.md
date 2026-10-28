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
- No console variables: the graphics options go through IL2CPP (see "Graphics" below). The prefs
  file `PrefData.Save` is encrypted; the registry key
  `HKCU\Software\Square Enix\Life is Strange: Before the Storm` holds only Screenmanager keys.
- **Deferred shading** (Camera.actualRenderingPath 3; the G-buffer is 4 render targets), so MSAA
  (QualitySettings.antiAliasing 2) and the pixel light count (8) hardly matter. Main camera: near
  0.01, far 2500, HDR and depth texture on.

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

## Graphics: IL2CPP access

`GameAssembly.dll` exports the full IL2CPP API (240 functions, not obfuscated):
`il2cpp_domain_get_assemblies`, `il2cpp_class_from_name`, field and method reflection,
`il2cpp_resolve_icall`, GC handles, `il2cpp_thread_attach`/`detach`.

- **Read-only probe** (`src/il2cpp_probe.hpp`, Advanced button): on its own attached thread,
  writes the settings-related classes (fields, offsets, enum values, method RVAs, static values and
  singletons) to `BtSTweaks-il2cpp.txt`.
- **Main thread**: Unity's API (QualitySettings and the rest) must be called there. Unity calls
  `MonoBehaviour.Update` through the runtime, which reads `MethodInfo::methodPointer` at each
  call, so pointing that at an add-on function runs it on the main thread. The game has no Unity
  EventSystem (its UI is NGUI): that hook never fired. Hooked instead (`src/il2cpp_main.hpp`):
  `SteamManager.Update` and `CriMonoBehaviourManager.Update` (once per frame), NGUI's
  `UICamera.Update` (3 per frame). The work runs once per frame, whichever comes first.
- **Live dump** (Advanced button): `Object.FindObjectsOfType(Type)` on the main thread for the
  camera and effect classes, profiles and their models followed, to `BtSTweaks-live.txt`.

## The game's settings

- `DawnSettingsManager.RenderSettingsManager`: quality levels Low, Medium, High, "Hella", Custom;
  keys for resolution, display mode, vsync, MSAA, FXAA, LOD, texture, effects and shadow quality,
  stored in `IdolPrefs` (float and string dictionaries).
- `IMGraphicsSettings.s_imGraphicsSettings` (the player's settings, 2026-10-28): main directional
  light shadows **High** (2), other lights Very High (3), lower lights Medium (1), sun shafts High
  (2), effects / shadows / AA Highest (3).
- Unity QualitySettings in play: level 1, shadows all, resolution Very High, stable fit, **shadow
  distance 60** in the first scene (100 in the menu: set per scene), 4 cascades, **LOD bias 2**,
  maximum LOD 0, **anisotropic filtering off** (0), texture limit 0, 8 pixel lights, MSAA 2,
  soft particles on, realtime reflection probes off, 4 skin weights, vsync 1.
- `RenderSettings.sun`: the main directional light, shadow resolution tier 2 (High), custom
  resolution -1 (unset).

Applied (main thread, each frame; a value that differs from what the add-on last set came from the
game and becomes its new base): shadow distance and LOD bias times the multipliers;
`anisotropicFiltering` ForceEnable with `Texture.SetGlobalAnisotropicFilteringLimits(16, 16)`
(back to 9, 16 when off); `Light.shadowCustomResolution` on the sun (4096 or 8192). Read back in
game: shadow distance 120 (game 60), LOD bias 4 (game 2), filtering 2 (game 0), sun 8192 (game -1);
F8 restores all four. 16x filtering is clearly visible.

## Post-processing

The game's own branch of Post-Processing Stack v1 (`UnityEngine.PostProcessing`): one
`PostProcessingBehaviour` on the main camera, profile per scene. Models: debug views, D9 skin
subsurface scattering, fog, antialiasing, ambient occlusion, screen-space reflections, depth of
field, colour strokes (light leaks), motion blur, eye adaptation, bloom, colour grading, user LUT,
chromatic aberration, vignette, volumetric fog. In the profile seen indoors (2026-10-28):

| Model | Game | Option |
|---|---|---|
| Ambient occlusion | off; 10 samples, downsampled | 16 samples, full res (where on, or everywhere) |
| Screen-space reflections | off; reflectionQuality Low (2), 256 iterations | High (0) where the scene has them |
| Antialiasing | FXAA (method 0, preset 4 Extreme); TAA settings present | TAA (method 1) |
| Depth of field | on, kernel Small | unchanged (artistic) |
| Motion blur, eye adaptation, skin SSS, vfog | off | unchanged |

Legacy `SunShafts` on the camera: resolution **Low** (0) in that scene although the game's
setting is High; the option sets High (2). In other scenes the game sets High itself.

**Screen-space reflections turned on in scenes without them break the picture** (tested): the
option only raises their resolution where a scene uses them. Ambient occlusion everywhere and TAA
looked good in testing (contact shadows under cars, bushes and the trailer outdoors).

Writes go to the profile objects (shared assets, in memory only). Each written field keeps the
game's value, and the objects written to are held with GC handles so their addresses stay valid.
