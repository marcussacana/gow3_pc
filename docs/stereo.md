# Stereo 3D side by side

The scene is rendered twice per frame, once per eye, and shown with the two eyes side by side.
Use it with 3D TVs, projectors and SBS viewers.

## Use
In-game menu (Insert or R3+L2), tab **Image**, section **Stereo 3D (side by side)**. All options
are live and saved to `gow3.ini`:

| Key | Values | Default | Meaning |
|---|---|---|---|
| `stereo` | 0 / 1 | 0 | on/off |
| `stereo_mode` | `half` / `full` | half | half: each eye squeezed into half the width; full: an image twice as wide |
| `stereo_separation` | 0-10 | 3.0 | parallax at infinity, in percent of the image width |
| `stereo_convergence` | 1-1000 | 20 | distance of the screen plane in game units (20 is about Kratos in gameplay) |

Cost: about twice the GPU time of the scene (60 to about 52 FPS on an RX 7900 XTX at 1080p).

## How it works
- **Eye offset in the vertex shaders** (`EmitStereo`, `emit_spirv_special.cpp`). Every vertex
  shader gets an epilogue `x += sep * w * max(1 - conv / d, -1)`. `sep` and `conv` are new push
  constants (`Shader::PushData::stereo_sep/stereo_conv`; the block grew from 128 to 136 bytes,
  and `Instance` checks the device limit).
  - The view depth `d` comes from z/w with the game's projection (`z = 1.00007 d - 0.5`). So
    shaders that write a scaled clip position still get the right parallax.
  - Draws with `w == 1` (full-screen passes, HUD, orthographic) keep zero parallax.
  - Pop-out is limited to the separation at infinity. Close-ups such as the menu's Kratos
    (d about 2) would otherwise leave the image.
  - This replaces any search for the camera: the game has no camera buffer. Each object gets
    its own World x View x Proj, computed on the CPU (see Research).
- **Draws recorded twice** (`Rasterizer::RecordEyes`).
  - `PrepareRenderState` classifies a draw as an eye draw when all its targets are screen-sized
    (`Stereo::EyeSized`: 1920x1080 or 3840x2160 halved down to 1/16, MSAA included).
  - An eye draw records first for the left eye, into the guest's images, then for the right
    eye, into twins (`EyeTargets`, `vk_stereo.cpp`).
  - In the right eye's pass, attachments and sampled textures that have a twin use the twin.
  - Shadow maps (2048x2048), the 1x1 exposure and the 512x256 target are drawn once and
    shared by both eyes.
  - Eye draws skip the render-state and texture-set memos and the texture bind helper thread.
- **Twin coherence.**
  - Twins never go back to guest memory.
  - A guest-image write that does not come from an eye draw (`Runtime::Transit` with write
    access) marks the twin stale. Clears, copies and the single-eye MLAA compute are such writes.
  - A stale twin is refilled from the guest image before the next eye draw writes it
    (`EyeTargets::Prepare`).
  - The guest's MSAA resolve (`Rasterizer::Resolve`) is repeated between the twins.
- **Presentation** (`Presenter::ComposeSideBySide`).
  - The display buffer and its twin are blitted side by side into one image, in the guest's
    storage format and read through the same view format, so BGRA buffers keep their colors.
  - FSR 1, RCAS and post-processing run on that image as before.
- **HUD, menus, videos.** They are drawn with w == 1, so both eyes get the same image and they
  sit on the screen plane.

## Known limits and work left
- **Screen-space shadow mask** (`3af97853811d0c07`, see Research) runs per eye but with the
  center camera's matrix. The right eye's shadows may be slightly off. The fix is a
  matrix-only patch per eye, derived below.
- **A bright spot seen only in the right eye** (a far doorway in the first level): cause not
  found yet.
- **MLAA** (compute `ccdebd80f02a77aa`) runs once. The right eye reuses the left eye's edges.
- **Performance:** both eyes alternate render passes draw by draw, and memos are off for eye
  draws.
- **Projection constants** (1.00007, -0.5) come from gameplay. Other scenes (cutscenes) may use
  another near plane.
- **Not tested:** `full` mode on a double-width display, the temporal upscalers (stereo
  expects `upscaler=none`, which `run.py` forces for this game) and the 4K render patch.

## Research tools (environment variables, off by default)
- `GOW3_STEREO_DEBUG=1`: prints each kind of target that keeps a draw to a single eye.
- `GOW3_CAMERA_DUMP=<path>`: once the file `<path>` exists, every constant buffer of 256-4096
  bytes bound in the next 240 frames goes to `<path>.bin`. Each record is frame, address,
  G-buffer flag and size (u64 each), then the data.
- `GOW3_CAPTURE_SEGMENTS=N`: the frame capture (F11 with `GOW3_FRAME_CAPTURE_KEY=1`) records
  across N display-buffer boundaries. The game draws into a display buffer several times per
  frame, so one segment misses the scene.

## Research (CUSA01623 01.02, first level)
- `CameraMotion::OnConstants` expects an 864-byte camera signature (far 3000). It never appears
  in gameplay.
- Each object has a 736-byte (or 224-byte) buffer: the world matrix, then World x View x Proj.
  The convention is row vectors, D3D-style depth.
- **Frame:**
  1. depth pre-pass, 2x MSAA, 1920x1080;
  2. 960x540 pass;
  3. linear depth to R32F (`782d705b`);
  4. about 10 shadow cascades: a 2048² shadow map, then a screen-space mask (`3af97853`);
  5. forward pass, 2x MSAA, followed by its resolve;
  6. fog (`475a4146`, screen UV and depth only);
  7. transparents;
  8. MLAA compute;
  9. DOF, bloom pyramid, 1x1 exposure, final composite into the display buffer.
- **Shadow mask `3af97853811d0c07`**, constants c[0..23]:
  - Inputs: `d` is the linear depth; `v = (d*ndc.x, d*ndc.y, z', w')` with `z' = -c20*d + c22`
    and `w' = -c21*d + c23`.
  - Output: shadow-map xyz = `v.x*c[0..2] + v.y*c[4..6] + z'*c[8..10] + w'*c[12..14]`.
  - For the right eye, `d*ndc_C.x = d*ndc_R.x - sep*d + sep*conv`. That is affine in `d`, so it
    folds into c[8..14] as a multiple of c[0..2].
