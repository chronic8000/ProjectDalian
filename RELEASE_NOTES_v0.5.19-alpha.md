## v0.5.19-alpha — Jet energy flight, Blinn materials, loading hang fix

**Download `ProjectDalian-v0.5.19-alpha-win64.zip`**

### Jets (energy / feel)
- Afterburner input fixed: double-tap / hold-W / Shift / Ctrl; AB no longer stalls you by running at idle RPM
- HUD: `THR` / `ENG` / `AB OFF` or `AB xx%` (was misleading sprint fuel always)
- Mouse stick inertia + reverse resistance — less twitchy flick-overs
- Soft G-load limits on hard pulls
- Idle / cut throttle: **glide then stall sink** (not hover forever, not brick drop)
- Nose-following flight path so pitch-down actually dives

### Materials (less “plastic”)
- Textured meshes use **BF2 Blinn-Phong**, not hot GGX/PBR
- Optional `*_s` specular maps + diffuse-alpha gloss fallback (`specularlut` still skipped)
- Softer default bloom so highlights don’t wrap everything in cellophane

### Loading screen
- Fixed Windows **Application Hang** when clicking during long loads (bar stuck at ~18%)
- Progress pumps through mesh / road / tree / collision uploads; clicks ignored until Ready

### Docs (already on master)
- README: AI-upscaled textures are **not distributed** — local remaster guide + multi-day GPU cost called out

### Notes
- Requires a legitimate Battlefield 2 install
- No remastered texture packs in the zip
- Helicopters still next on the flight pass
