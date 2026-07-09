# BF2 Asset Scanner

Full-install inventory of every Battlefield 2 mod, level archive, and object zip.
Use this to see what Project Dalian still needs to load for 1:1 parity.

## Run

```powershell
cd C:\Projects\bf2respawn
python tools\bf2_asset_scan\bf2_asset_scan.py
```

Custom install path:

```powershell
python tools\bf2_asset_scan\bf2_asset_scan.py "D:\Games\Battlefield 2"
```

Fast scan (skips per-tweak EffectBundle name extraction):

```powershell
python tools\bf2_asset_scan\bf2_asset_scan.py --quick
```

## Output (`reports/bf2_assets/`)

| File | Contents |
| --- | --- |
| `bf2_summary.json` | Counts, gaps, night maps |
| `bf2_manifest.json` | Category + extension totals |
| `bf2_levels.json` | Per-level assets, placements, sky hints |
| `bf2_effect_bundles.json` | All `EffectBundle` template names |
| `bf2_samples.json` | Example paths per category |
| `bf2_engine_gaps.md` | Human-readable gap report |

## Categories

Assets are classified by file extension and path (e.g. `Effects/` → effects,
`vehicles/air/` → aircraft). Engine support status is compared against
bf2respawn loaders documented in `docs/formats/README.md`.

## Night maps

Special Forces and some custom maps use dark `Sky.con` lighting. The scanner
flags maps with low `sunColor`/`skyColor`, moon keywords, or known names like
`Night_Flight`.
