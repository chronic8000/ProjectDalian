# BF2 texture remaster (optional, local only)

Project Dalian **does not ship AI-upscaled Battlefield 2 textures**.

Those files are derived from EA/DICE assets. We cannot redistribute them.
If you want sharper level textures, you run this pipeline yourself against **your own**
legitimate BF2 install.

**Expect a long wait.** A full maps pass at ×2 with Real-ESRGAN (ncnn) took
**about two days on an RTX 3090**. Slower GPUs take longer. Leave the machine
running; use `--skip-existing` to resume after a crash or Ctrl+C.

---

## What this does

1. Finds texture-bearing zips under your BF2 `mods/` tree (mainly level `client.zip`)
2. Backs up each zip once under a work folder
3. Decodes DDS → PNG, upscales with Real-ESRGAN (or Lanczos fallback)
4. Re-encodes to DDS and writes an HD zip tree
5. Optionally installs HD zips over the live game paths (backups kept)

Default skip list: lightmaps / UI / envmaps (usually look worse when AI-upscaled).
Pass `--include-skipped` only if you know you want those too.

---

## Requirements

- A legitimate BF2 install (EA App / retail / compatible mod tree)
- Python 3.10+
- Pillow + NumPy (`pip install -r requirements.txt`)
- An upscaler backend (pick one):
  - **Recommended:** [`realesrgan-ncnn-vulkan`](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/releases)
    — drop `realesrgan-ncnn-vulkan.exe` next to this folder (or on `PATH`), with its `models/`
  - Or PyTorch Real-ESRGAN (CUDA) — slower to set up; see comments in `requirements.txt`
- DDS re-encode tool (one of):
  - [DirectXTex `texconv.exe`](https://github.com/microsoft/DirectXTex) on `PATH` or in this folder
  - or `NVDXT.EXE` from your BF2 install root

Vendored Real-ESRGAN usage notes (upstream): see `README_windows.md`.

---

## Quick start (maps only, ×2)

From a Developer PowerShell / terminal:

```powershell
cd C:\Projects\bf2respawn\tools\texture_remaster
pip install -r requirements.txt

# Work folder holds backup/, hd/, logs/, extract/ — use a fast drive with free space
python pipeline.py `
  --root "C:\Program Files (x86)\Battlefield2" `
  --work "C:\BF2_TextureRemaster" `
  --maps-only `
  --scale 2 `
  --backend ncnn `
  --skip-existing
```

When finished (or when you have enough HD zips):

```powershell
# Copy HD zips over the live install (originals already under work\backup)
python pipeline.py `
  --root "C:\Program Files (x86)\Battlefield2" `
  --work "C:\BF2_TextureRemaster" `
  --install-only
```

Or build + install in one go with `--install` on the first command.

### Useful flags

| Flag | Meaning |
|------|---------|
| `--maps-only` | Level `client.zip` only (recommended first pass) |
| `--zip mods/.../client.zip` | Single zip relative to BF2 root |
| `--limit N` | Cap textures per zip (smoke test) |
| `--skip-existing` | Resume: skip zips that already have HD output |
| `--categories terrain_colormap,roads,...` | Filter by inventory category |
| `--install` / `--install-only` | Write HD over live paths after backup |

Progress / per-zip stats land in `<work>\run_summary.json`.

---

## Point Project Dalian at the result

After install-over-live, launch Dalian and set **Options → BF2 INSTALL PATH** to the
same install root (Change / Rescan). No special “HD pack” toggle is required — the
engine reads whatever is in those zips.

**Performance:** remasters are heavier. Use Options → Graphics → **Max Texture Size**,
mip LOD bias, render scale, and Laptop Compat if FPS drops. We do not claim remastered
maps will stay at retail performance.

---

## Legal reminder

- You must own Battlefield 2.
- Do **not** upload or redistribute the HD zip tree, backups of retail zips, or extracted DDS/PNG.
- This tool is for personal use on your own install only.
