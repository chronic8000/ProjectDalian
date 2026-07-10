## v0.5.12-alpha — Performance, FSR 1.0, Hawk SAM, FPS counter

**Download `ProjectDalian-v0.5.12-alpha-win64.zip`**

### Performance (HD textures / Dalian Plant)

- **Render Scale** — draw the world below native resolution (biggest FPS lever)
- **FSR 1.0 (EASU + RCAS)** — spatial upscaling that runs on every GPU (NVIDIA / AMD / Intel)
  - Options → Graphics → Upscaling: Bilinear / FSR 1.0 / Auto
  - FSR Sharpness (RCAS) slider
- **Mip LOD Bias**, **Shadow Distance**, softer defaults (MSAA off, SSAO off, shadows 2048, aniso 4)
- **Grass** on/off + distance
- Vendor-agnostic upscaling facade ready for future DLSS / XeSS / FSR3 behind the same Auto option

### MIM-23 Hawk emplacement (Dalian Plant 64 CQ)

- Custom launcher OBJ on the **Chinese airfield runway apron** (near the jets), not south of the strip
- Content pack is copied next to the exe / into the release zip (`content/emplacements/...`)
- Scaled + ground-snapped so it is visible; **orange minimap blip** when ammo remains
- Walk up → **[E]** / **[F8]** for map SAM fire (3 rounds)

### UI / QoL

- **Show FPS** toggle (Options → Video) — top-left overlay
- Graphics options **mouse-wheel scroll** (list no longer overlaps APPLY / BF2 path)
- Storm Shadow custom missile mesh path (Car-SAM)

### Flight feel

- Jet sweet-spot turn curve + mass-scaled thrust
- Heli J-hook / slide tuning

### Requirements

- Windows 10/11 x64 + Battlefield 2 install (set BF2 path in Options)
