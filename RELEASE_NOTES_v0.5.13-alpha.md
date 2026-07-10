## v0.5.13-alpha — Laptop LDR fix, Compat Mode, Hawk polish

**Download `ProjectDalian-v0.5.13-alpha-win64.zip`**

### Fix: green sky / purple world on weak laptops

This was **not** “monitor has no HDR.” The engine was always rendering the 3D scene into a **float colour buffer** (`RGB16F`), even when Options → HDR was off. Many Intel / old laptop GPUs corrupt that path → neon green sky, magenta terrain, scanline noise. The HUD stayed normal because UI is LDR.

**v0.5.13:**
- **HDR off = real LDR (`RGBA8`)** scene + bloom buffers
- Float buffers only when HDR tonemap is on **and** the GPU float-FBO probe passes
- Automatic fallback to LDR if the float FBO is incomplete
- Options label clarified: **HDR TONEMAP (float GPU)** — not Windows/display HDR

### Laptop / Compat Mode

Options → Graphics → **Laptop / Compat Mode** (then Apply):
- HDR / bloom / SSAO / MSAA off
- Upscaling → Bilinear (safer than FSR on weak GPUs)
- Lower render scale, shadows, aniso; grass off

Or set env `BF2_COMPAT=1` before launch (overrides `settings.cfg` graphics).

Default upscaling is now **Bilinear** (was FSR 1.0).

### Hawk SAM (Dalian Plant)

- Orientation / ground sit fix (Z-up → Y-up)
- Solid MTL colours so the launcher is not flat grey when textures fail
- Relocated toward the solitary Chinese heli area (Z-8)
- Bigger SAM explosion VFX + heavier sound

### If colours are still wrong

1. Options → Graphics → **Laptop / Compat Mode** → Apply  
2. Or: `set BF2_COMPAT=1` then run `project_dalian.exe`  
3. Or delete `%APPDATA%\ProjectDalian\ProjectDalian\settings.cfg`

### Requirements

- Windows 10/11 x64 + Battlefield 2 install (set BF2 path in Options)
