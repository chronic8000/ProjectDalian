#!/usr/bin/env python3
"""
List every texture in a Battlefield 2 install (mods + maps).

Writes JSON + CSV under the output folder so you can see what to remaster
before spending GPU time.

Usage:
  python inventory.py
  python inventory.py --root "C:/Program Files (x86)/Battlefield2"
  python inventory.py --out C:/BF2_HD/inventory --maps-only
"""

from __future__ import annotations

import argparse
import csv
import json
import zipfile
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from dds_io import parse_dds_header

DEFAULT_ROOT = Path(r"C:\Program Files (x86)\Battlefield2")

TEXTURE_EXTS = {".dds", ".tga", ".png", ".jpg", ".jpeg"}

# Zips that usually hold world/object textures worth remastering.
CLIENT_ZIP_NAMES = {
    "objects_client.zip",
    "common_client.zip",
    "booster_client.zip",
    "menu_client.zip",
    "client.zip",
}


def classify_texture(virt: str) -> str:
    p = virt.replace("\\", "/").lower()
    rules = [
        ("colormaps/", "terrain_colormap"),
        ("detailmaps/", "terrain_detailmap"),
        ("lowdetailmaps/", "terrain_lowdetail"),
        ("lightmaps/", "terrain_lightmap"),
        ("envmaps/", "envmap"),
        ("water/", "water"),
        ("roads/", "roads"),
        ("textures/sky", "sky"),
        ("sky/", "sky"),
        ("terrain/textures", "terrain_shared"),
        ("effects/", "effects"),
        ("menu/", "ui"),
        ("vehicles/", "vehicles"),
        ("weapons/", "weapons"),
        ("soldiers/", "soldiers"),
        ("staticobjects/", "staticobjects"),
        ("common/", "common_objects"),
    ]
    for needle, cat in rules:
        if needle in p:
            return cat
    if p.endswith(".dds"):
        return "texture_other"
    return "image_other"


def should_skip_upscale(category: str) -> bool:
    # Lightmaps are baked; AI upscale usually looks worse.
    # Env cubemaps / UI can be handled in a second pass.
    return category in {"terrain_lightmap", "envmap", "ui"}


def iter_mod_zips(root: Path, maps_only: bool) -> Iterable[tuple[str, Path]]:
    mods = root / "mods"
    if not mods.is_dir():
        return
    for mod_dir in sorted(mods.iterdir()):
        if not mod_dir.is_dir():
            continue
        mod = mod_dir.name
        if not maps_only:
            for zname in (
                "Objects_client.zip",
                "Common_client.zip",
                "Booster_client.zip",
                "Menu_client.zip",
            ):
                zp = mod_dir / zname
                if zp.is_file():
                    yield mod, zp
        levels = mod_dir / "Levels"
        if levels.is_dir():
            for level in sorted(levels.iterdir()):
                if not level.is_dir():
                    continue
                client = level / "client.zip"
                if client.is_file():
                    yield f"{mod}/Levels/{level.name}", client


def probe_dds(data: bytes) -> Dict[str, Any]:
    try:
        info = parse_dds_header(data)
        return {
            "width": info.width,
            "height": info.height,
            "format": info.format_name,
            "mips": info.mipmap_count,
        }
    except Exception as exc:  # noqa: BLE001
        return {"error": str(exc)}


def scan(root: Path, out: Path, maps_only: bool, probe: bool) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    by_cat: Counter = Counter()
    by_zip: Counter = Counter()
    bytes_total = 0

    for scope, zip_path in iter_mod_zips(root, maps_only):
        rel_zip = str(zip_path.relative_to(root)).replace("\\", "/")
        try:
            zf = zipfile.ZipFile(zip_path, "r")
        except zipfile.BadZipFile:
            continue
        with zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                name = info.filename.replace("\\", "/")
                ext = Path(name).suffix.lower()
                if ext not in TEXTURE_EXTS:
                    continue
                cat = classify_texture(name)
                by_cat[cat] += 1
                by_zip[rel_zip] += 1
                bytes_total += info.file_size
                row: Dict[str, Any] = {
                    "mod_scope": scope,
                    "zip": rel_zip,
                    "path": name,
                    "ext": ext,
                    "category": cat,
                    "skip_upscale_default": should_skip_upscale(cat),
                    "size": info.file_size,
                }
                if probe and ext == ".dds" and info.file_size < 64 * 1024 * 1024:
                    try:
                        # Header is enough for width/height/format
                        with zf.open(info) as fp:
                            header = fp.read(128)
                        row.update(probe_dds(header))
                    except Exception as exc:  # noqa: BLE001
                        row["error"] = str(exc)
                rows.append(row)

    summary = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "bf2_root": str(root),
        "texture_count": len(rows),
        "bytes_total": bytes_total,
        "by_category": dict(by_cat),
        "zip_count": len(by_zip),
        "upscale_candidates": sum(1 for r in rows if not r["skip_upscale_default"]),
        "skipped_by_default": sum(1 for r in rows if r["skip_upscale_default"]),
    }

    out.mkdir(parents=True, exist_ok=True)
    (out / "textures_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    (out / "textures_manifest.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")

    with (out / "textures_manifest.csv").open("w", newline="", encoding="utf-8") as f:
        fields = [
            "mod_scope",
            "zip",
            "path",
            "ext",
            "category",
            "skip_upscale_default",
            "size",
            "width",
            "height",
            "format",
            "mips",
            "error",
        ]
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)

    # Per-zip rollup for planning GPU time
    zip_rollup: Dict[str, Any] = defaultdict(lambda: {"count": 0, "bytes": 0, "categories": Counter()})
    for r in rows:
        z = zip_rollup[r["zip"]]
        z["count"] += 1
        z["bytes"] += r["size"]
        z["categories"][r["category"]] += 1
    zip_out = {
        k: {"count": v["count"], "bytes": v["bytes"], "categories": dict(v["categories"])}
        for k, v in sorted(zip_rollup.items(), key=lambda kv: -kv[1]["bytes"])
    }
    (out / "textures_by_zip.json").write_text(json.dumps(zip_out, indent=2), encoding="utf-8")
    return summary


def main() -> int:
    ap = argparse.ArgumentParser(description="Inventory BF2 textures across mods/maps")
    ap.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="BF2 install root")
    ap.add_argument("--out", type=Path, default=Path("reports/texture_remaster"))
    ap.add_argument("--maps-only", action="store_true", help="Only Levels/*/client.zip")
    ap.add_argument(
        "--probe",
        action="store_true",
        help="Read DDS headers for width/height/format (slower)",
    )
    args = ap.parse_args()
    if not args.root.is_dir():
        print(f"BF2 root not found: {args.root}")
        return 1
    print(f"Scanning {args.root} ...")
    summary = scan(args.root, args.out, args.maps_only, args.probe)
    print(json.dumps(summary, indent=2))
    print(f"Wrote {args.out / 'textures_manifest.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
