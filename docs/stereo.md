# Stereo 3D

The scene is rendered twice per frame, once per eye, and shown side by side or woven for a Leia SR
3D monitor. Use it with 3D TVs, projectors, SBS viewers and glasses-free 3D monitors such as the
Samsung Odyssey 3D.

## Use
In-game menu (Insert or R3+L2), tab **Image**, section **Stereo 3D (side by side)**. All options
are live and saved to `gow3.ini`:

| Key | Values | Default | Meaning |
|---|---|---|---|
| `stereo` | 0 / 1 | 0 | on/off |
| `stereo_mode` | `half` / `full` / `leia` | half | half: each eye squeezed into half the width; full: an image twice as wide; leia: woven for a Leia SR monitor |
| `stereo_separation` | 0-10 | 3.0 | Depth: parallax at infinity, in percent of the image width |
| `stereo_convergence` | 1-1000 | 20 | Screen distance: objects this far away sit on the screen (20 is about Kratos in gameplay) |
| `stereo_popout` | 0-5 | 2.0 | Pop-out limit: how far near objects may come out of the screen, in units of the depth at infinity |

The menu warns about uncomfortable combinations and says which setting to change. The ranges follow
NVIDIA's 3D Vision guidelines (SIGGRAPH 2009): keep the scene between a third of the screen distance
and 100 times it.

Cost: about twice the GPU time of the scene; 60 FPS on an RX 7900 XTX at 1080p per eye.

## Leia SR 3D monitors
Format **Leia SR 3D monitor** (`stereo_mode=leia`) weaves the side-by-side frame (2 x 1920x1080)
for the display's lenses, with eye tracking, and switches the lenses on while frames are woven.
- Install the Leia SR platform (its runtime DLLs). `gow3_leia.dll` goes next to `gow3-probe.exe`.
- Use the panel's native size: `output_res=3840x2160`, borderless or fullscreen.
- Build the bridge (MSVC, needs the Leia SR SDK, which is not part of this repository):
  `LEIASR_SDK_ROOT=<sdk with include/sr and lib> bash packaging/windows/build_leia.sh`. The SDK's
  headers also need OpenCV's, by default in `<sdk>/../opencv/build/include`.

The SR SDK has no Vulkan weaver, so `gow3_leia.dll` (`gpu/leia_bridge`) runs its OpenGL weaver in a
hidden context. The presenter copies both eyes into an exportable Vulkan image and signals a shared
semaphore; the bridge imports the images (`GL_EXT_memory_object_win32`), weaves into an output of
the window's size and signals back (`GL_EXT_semaphore_win32`). The output is shown 1:1, without
FSR 1 or RCAS. OpenGL counts rows from the bottom, so the bridge weaves an upright copy of the input
and flips the result back; the lens pattern follows the panel's rows.

## How it works
- **Eye offset in the vertex shaders** (`EmitStereo`, `emit_spirv_special.cpp`). Every vertex
  shader gets an epilogue `x += sep * w * P`, with `P = 1 - conv / d`, the parallax of NVIDIA 3D
  Vision. The view depth `d` comes from z/w with the game's projection (`z = 1.00007 d - 0.5`), so
  shaders that write a scaled clip position get the same parallax. In front of the screen `P` is
  compressed smoothly towards `-popout`, so close-ups stay inside the image and keep their depth.
  Draws with `w == 1` (full-screen passes, HUD, orthographic) keep zero parallax. The game has no
  camera buffer to change: each object gets its own World x View x Proj from the CPU.
- **Draws recorded twice.** A draw whose targets are all screen-sized (1920x1080 or 3840x2160,
  halved down to 1/16, MSAA included) is an eye draw. The left eye records into the game's images,
  the right eye into twins (`EyeTargets`, `vk_stereo.cpp`) and samples the twins. Shadow maps, the
  1x1 exposure and other small targets are drawn once and shared.
- **Batches.** Consecutive eye draws are recorded for the left eye, then again for the right eye at
  the first single-eye draw, dispatch or task, so render passes change once per batch.
- **Twin coherence.** Twins never go back to guest memory. Any other write to the game's image
  (clears, copies, the MLAA compute) marks the twin stale, and it is refilled from the game's image
  before the next eye draw writes it. The game's MSAA resolve is repeated between the twins.
- **Shadows.** The screen-space shadow mask rebuilds positions with the center camera. Its
  constants are patched per eye (`Stereo::PatchShadowMask`).
- **Presentation** (`Presenter::ComposeSideBySide`, `WeaveLeia`). The display image and its twin
  are copied side by side in the game's storage format, so BGRA buffers keep their colors.
- **HUD, menus, videos** are drawn with `w == 1`: both eyes get the same image, on the screen plane.

## Limits
- MLAA runs once; the right eye reuses the left eye's edges.
- Shadows of objects inside the pop-out compression (nearer than `conv / (1 + popout / 2)`) are
  approximate.
- The projection constants (1.00007, -0.5) are the gameplay camera's; scenes with another near plane
  get slightly different depth.
- Not tested: `full` mode on a double-width display, the temporal upscalers (stereo expects
  `upscaler=none`) and the 4K render patch.
