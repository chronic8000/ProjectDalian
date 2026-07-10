## v0.5.14-alpha — Colour / HDR audit, OBS SDR, Hawk blast dial-back

**Download `ProjectDalian-v0.5.14-alpha-win64.zip`**

### Colour / HDR / brightness audit

Research (LearnOpenGL HDR/gamma, Narkowicz ACES): tonemap in linear, **gamma only after** ACES. Our textures are **not** sRGB→linear, so the LDR path must **not** apply `pow(1/2.2)`.

**Fixes:**
- **HDR off:** removed the mistaken display-gamma that washed the sun white; soft Reinhard highlight compression instead
- **Sun:** slightly lower lighting gain + tighter/dimmer sky disc
- **Output Brightness** slider (Options → Graphics) — leave at **1.0** unless capture needs a nudge
- **HDR on:** still ACES + gamma (float path); label remains “HDR TONEMAP (float GPU)” — not Windows/monitor HDR

### Recording for YouTube / OBS

Dark OBS files were usually **Windows HDR** capture, not missing in-game gamma.

1. Turn **Windows HDR / Auto HDR off** while recording  
2. In-game: HDR off, Bloom off, Output Brightness **1.0**  
3. OBS: SDR (NV12 / Rec.709 / Partial) — no HDR tonemap capture  

### Hawk / Car-SAM blast

Was dialed like a nuke (`38m` / `320` damage + oversized FX). Now **`12m` / `145`** with sane VFX scale — still punchy, not map-clearing.

### Laptop note (from v0.5.13)

Green/magenta on weak GPUs → Options → **Laptop / Compat Mode**, or `BF2_COMPAT=1`.

### Requirements

- Windows 10/11 x64 + Battlefield 2 install (set BF2 path in Options)
