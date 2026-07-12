## v0.5.18-alpha — BF2 path picker, jet feel, HD texture triage

**Download `ProjectDalian-v0.5.18-alpha-win64.zip`**

### BF2 install path (EA App / friends)
- Options shows **BF2 INSTALL PATH** once with **Change** (Windows folder dialog) and **Rescan**
- Also on Play when no maps are found
- Auto-scan covers Program Files, **EA Games / Origin**, registry Install Dir, and common D:/E: paths
- Pick the folder that contains `mods\` — maps refresh immediately after Change/Rescan

### Jet controls
- Airborne **rudder (A/D)** actually yaws again (was ~dead at cruise)
- Mouse pitch less twitchy (softer elevator gain + per-frame stick cap)

### HD texture FPS triage (OpenGL)
- **Max texture size** option (default 2048) — skips oversized DDS mips at upload
- Cheaper cutout detection (no full decode of huge remasters)
- Live anisotropic updates; safer defaults (render scale / shadows)

### Notes
- 24GB VRAM machines: size cap is optional — set Max Texture Size to Off if you want full remaster res
- True DLSS still needs a Vulkan/DX backend later; FSR 1.0 remains the OpenGL upscaler
