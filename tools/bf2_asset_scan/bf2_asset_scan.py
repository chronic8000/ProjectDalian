#!/usr/bin/env python3
"""
Battlefield 2 full-install asset inventory scanner.

Walks every mod, archive (.zip), and loose file under a BF2 root directory.
Produces a categorized manifest, per-level breakdowns, night-map hints, and an
engine support gap report for Project Dalian (bf2respawn).

Usage:
  python bf2_asset_scan.py "C:/Program Files (x86)/Battlefield2"
  python bf2_asset_scan.py --root "..." --out reports/bf2_assets --quick
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import zipfile
from collections import Counter, defaultdict
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set, Tuple

# ---------------------------------------------------------------------------
# Taxonomy — extension + path heuristics
# ---------------------------------------------------------------------------

EXTENSION_CATEGORY: Dict[str, str] = {
    ".staticmesh": "mesh_static",
    ".bundledmesh": "mesh_bundled",
    ".skinnedmesh": "mesh_skinned",
    ".collisionmesh": "collision",
    ".dds": "texture",
    ".tga": "texture",
    ".jpg": "texture",
    ".png": "texture",
    ".baf": "animation_clip",
    ".ske": "skeleton",
    ".con": "script_con",
    ".tweak": "script_tweak",
    ".inc": "script_include",
    ".wav": "audio",
    ".ogg": "audio",
    ".wma": "audio",
    ".fx": "shader",
    ".fxh": "shader_header",
    ".raw": "terrain_height",
    ".ahm": "terrain_height_meta",
    ".mat": "terrain_material",
    ".emi": "terrain_emit",
    ".cfg": "config",
    ".dat": "binary_data",
    ".tai": "terrain_index",
    ".ai": "nav_ai",
    ".clb": "nav_collision",
    ".qtr": "nav_quadtree",
    ".cls": "nav_class",
    ".qti": "nav_quad_index",
    ".vbf": "nav_vbf",
    ".desc": "descriptor",
    ".occ": "occlusion",
    ".mesh": "mesh_compiled",
    ".md5": "checksum",
}

# Path substring rules (checked on lowercased virtual path). First match wins.
PATH_CATEGORY_RULES: List[Tuple[str, str]] = [
    (r"effects/", "effects"),
    (r"vehicles/air/", "vehicles_air"),
    (r"vehicles/land/", "vehicles_land"),
    (r"vehicles/sea/", "vehicles_sea"),
    (r"vehicles/", "vehicles"),
    (r"weapons/handheld/", "weapons_handheld"),
    (r"weapons/", "weapons"),
    (r"soldiers/", "soldiers"),
    (r"overgrowth", "vegetation"),
    (r"vegetation", "vegetation"),
    (r"roads/", "roads"),
    (r"colormaps/", "terrain_colormap"),
    (r"lightmaps/", "terrain_lightmap"),
    (r"detailmaps/", "terrain_detailmap"),
    (r"lowdetailmaps/", "terrain_lowdetail"),
    (r"envmaps/", "envmap"),
    (r"water/", "water"),
    (r"sky", "sky"),
    (r"menu/", "ui_menu"),
    (r"fonts/", "ui_font"),
    (r"shaders/", "shaders"),
    (r"common/", "common"),
]

# Project Dalian / bf2respawn loader status (update when engine gains support).
ENGINE_SUPPORT: Dict[str, str] = {
    "mesh_static": "full",
    "mesh_bundled": "full",
    "mesh_skinned": "full",
    "mesh_compiled": "full",
    "animation_clip": "full",
    "skeleton": "full",
    "collision": "full",
    "texture": "full",
    "terrain_height": "full",
    "terrain_height_meta": "partial",
    "terrain_colormap": "full",
    "terrain_lightmap": "full",
    "terrain_detailmap": "full",
    "terrain_lowdetail": "partial",
    "terrain_material": "partial",
    "terrain_emit": "partial",
    "terrain_index": "partial",
    "vegetation": "partial",
    "roads": "full",
    "water": "partial",
    "sky": "partial",
    "envmap": "partial",
    "script_con": "partial",
    "script_tweak": "partial",
    "script_include": "partial",
    "effects": "partial",
    "vehicles_air": "partial",
    "vehicles_land": "partial",
    "vehicles_sea": "partial",
    "vehicles": "partial",
    "weapons_handheld": "partial",
    "weapons": "partial",
    "soldiers": "partial",
    "audio": "partial",
    "shader": "missing",
    "shader_header": "missing",
    "nav_ai": "missing",
    "nav_collision": "missing",
    "nav_quadtree": "missing",
    "nav_class": "missing",
    "nav_quad_index": "missing",
    "nav_vbf": "missing",
    "ui_menu": "partial",
    "ui_font": "missing",
    "common": "partial",
    "config": "partial",
    "binary_data": "partial",
    "descriptor": "partial",
    "occlusion": "missing",
    "checksum": "n/a",
}

KNOWN_NIGHT_MAPS = {
    "night_flight",
    "devils_perch",
    "ghost_town",
    "iron_gator",
}

EFFECT_TEMPLATE_RE = re.compile(
    r"ObjectTemplate\.(?:activeSafe|create)\s+(\w+)\s+(\S+)", re.I
)
OBJECT_CREATE_RE = re.compile(r"Object\.create\s+(\S+)", re.I)
RUN_LINE_RE = re.compile(r"^run\s+(.+)$", re.I | re.M)


def categorize(path: str) -> str:
    """Return primary category for a virtual archive path."""
    low = path.replace("\\", "/").lower()
    ext = Path(low).suffix
    if ext == ".mesh" and "_compiled" in low:
        return "mesh_compiled"
    if ext in EXTENSION_CATEGORY:
        base = EXTENSION_CATEGORY[ext]
    else:
        base = "other"
    for pattern, cat in PATH_CATEGORY_RULES:
        if pattern in low:
            return cat
    return base


def support_status(cat: str) -> str:
    return ENGINE_SUPPORT.get(cat, "unknown")


# ---------------------------------------------------------------------------
# Archive scanning
# ---------------------------------------------------------------------------


@dataclass
class FileRecord:
    vpath: str
    category: str
    size: int
    archive: str
    mod: str


@dataclass
class LevelInfo:
    mod: str
    name: str
    client_zip: Optional[str] = None
    server_zip: Optional[str] = None
    placements: List[str] = field(default_factory=list)
    sky_hints: Dict[str, str] = field(default_factory=dict)
    is_night: bool = False
    asset_counts: Counter = field(default_factory=Counter)
    categories: Set[str] = field(default_factory=set)


@dataclass
class ScanResult:
    bf2_root: str
    scanned_at: str
    mods: List[str]
    archives: List[str]
    total_files: int
    by_category: Dict[str, int]
    by_extension: Dict[str, int]
    by_mod: Dict[str, int]
    engine_gaps: Dict[str, Dict[str, int]]
    effect_bundles: List[str]
    levels: List[LevelInfo]
    sample_paths: Dict[str, List[str]]


def iter_zip_members(zpath: Path, mod: str) -> Iterable[FileRecord]:
    try:
        with zipfile.ZipFile(zpath, "r") as zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                vpath = info.filename.replace("\\", "/")
                cat = categorize(vpath)
                yield FileRecord(
                    vpath=vpath,
                    category=cat,
                    size=info.file_size,
                    archive=str(zpath),
                    mod=mod,
                )
    except (zipfile.BadZipFile, OSError) as exc:
        print(f"  WARN: cannot read {zpath}: {exc}", file=sys.stderr)


def read_zip_text(zpath: Path, member: str) -> Optional[str]:
    try:
        with zipfile.ZipFile(zpath, "r") as zf:
            low_map = {n.lower(): n for n in zf.namelist()}
            key = member.replace("\\", "/").lower()
            if key not in low_map:
                return None
            return zf.read(low_map[key]).decode("utf-8", errors="replace")
    except (zipfile.BadZipFile, OSError, KeyError):
        return None


def find_member_ci(zf: zipfile.ZipFile, suffix: str) -> Optional[str]:
    want = suffix.replace("\\", "/").lower()
    for name in zf.namelist():
        if name.replace("\\", "/").lower().endswith(want):
            return name
    return None


def parse_sky_night_hints(sky_text: str) -> Tuple[Dict[str, str], bool]:
    hints: Dict[str, str] = {}
    is_night = False
    for line in sky_text.splitlines():
        line = line.strip()
        if not line or line.startswith("rem"):
            continue
        parts = line.split(None, 1)
        if len(parts) < 2:
            continue
        key, val = parts[0], parts[1].strip()
        low = key.lower()
        if "sun" in low or "sky" in low or "moon" in low or "fog" in low or "cloud" in low:
            hints[key] = val
        if low.endswith("suncolor") or low.endswith("skycolor"):
            try:
                nums = [float(x) for x in re.split(r"[/\s]+", val) if x][:3]
                if nums and sum(nums) / len(nums) < 0.35:
                    is_night = True
            except ValueError:
                pass
        if "moon" in low:
            is_night = True
    return hints, is_night


def parse_static_objects(con_text: str) -> List[str]:
    names: List[str] = []
    for m in OBJECT_CREATE_RE.finditer(con_text):
        names.append(m.group(1))
    return names


def collect_effect_bundle_names(text: str) -> List[str]:
    out: List[str] = []
    for m in EFFECT_TEMPLATE_RE.finditer(text):
        typ, name = m.group(1), m.group(2)
        if typ.lower() == "effectbundle":
            out.append(name)
    return out


def scan_level(mod: str, level_dir: Path) -> LevelInfo:
    info = LevelInfo(mod=mod, name=level_dir.name)
    client = level_dir / "client.zip"
    server = level_dir / "server.zip"
    if client.is_file():
        info.client_zip = str(client)
        for rec in iter_zip_members(client, mod):
            info.asset_counts[rec.category] += 1
            info.categories.add(rec.category)
    if server.is_file():
        info.server_zip = str(server)
        for rec in iter_zip_members(server, mod):
            info.asset_counts[rec.category] += 1
            info.categories.add(rec.category)
        st = read_zip_text(server, "StaticObjects.con")
        if st:
            info.placements = parse_static_objects(st)
        sky = read_zip_text(server, "Sky.con")
        if sky:
            info.sky_hints, night = parse_sky_night_hints(sky)
            info.is_night = night
    if info.name.lower().replace(" ", "_") in KNOWN_NIGHT_MAPS:
        info.is_night = True
    return info


def scan_bf2_root(root: Path, quick: bool = False) -> ScanResult:
    mods_dir = root / "mods"
    if not mods_dir.is_dir():
        raise SystemExit(f"No mods/ folder under {root}")

    mods = sorted(p.name for p in mods_dir.iterdir() if p.is_dir())
    all_records: List[FileRecord] = []
    archives_scanned: List[str] = []
    effect_bundles: Set[str] = set()
    levels: List[LevelInfo] = []
    by_ext: Counter = Counter()

    print(f"Scanning BF2 root: {root}")
    print(f"Mods: {', '.join(mods)}")

    for mod in mods:
        mod_path = mods_dir / mod
        print(f"\n== Mod: {mod} ==")

        # Level folders
        levels_dir = mod_path / "Levels"
        if levels_dir.is_dir():
            for level_dir in sorted(levels_dir.iterdir()):
                if not level_dir.is_dir():
                    continue
                print(f"  Level: {level_dir.name}")
                levels.append(scan_level(mod, level_dir))

        # Zip archives at mod root and one level down
        zip_paths: List[Path] = []
        for pattern in ("*.zip", "*/*.zip"):
            zip_paths.extend(mod_path.glob(pattern))
        zip_paths = sorted(set(zip_paths))

        for zpath in zip_paths:
            # Skip level archives (handled above)
            if "Levels" in zpath.parts and zpath.name in ("client.zip", "server.zip"):
                continue
            print(f"  Archive: {zpath.relative_to(root)}")
            archives_scanned.append(str(zpath))
            for rec in iter_zip_members(zpath, mod):
                all_records.append(rec)
                ext = Path(rec.vpath).suffix.lower() or "(none)"
                by_ext[ext] += 1
                if quick:
                    continue
                if rec.category == "effects" and rec.vpath.lower().endswith(".tweak"):
                    text = read_zip_text(zpath, rec.vpath)
                    if text:
                        for name in collect_effect_bundle_names(text):
                            effect_bundles.add(name)

        # Loose files (rare — DefaultEnvMap.tweak, etc.)
        for loose in mod_path.rglob("*"):
            if not loose.is_file():
                continue
            if loose.suffix.lower() == ".zip":
                continue
            rel = str(loose.relative_to(mod_path)).replace("\\", "/")
            cat = categorize(rel)
            try:
                size = loose.stat().st_size
            except OSError:
                size = 0
            all_records.append(
                FileRecord(vpath=rel, category=cat, size=size, archive="(loose)", mod=mod)
            )
            ext = loose.suffix.lower() or "(none)"
            by_ext[ext] += 1

    by_category: Counter = Counter(r.category for r in all_records)
    by_mod: Counter = Counter(r.mod for r in all_records)

    engine_gaps: Dict[str, Dict[str, int]] = defaultdict(lambda: {"count": 0, "bytes": 0})
    for rec in all_records:
        st = support_status(rec.category)
        if st in ("missing", "partial", "unknown"):
            engine_gaps[rec.category]["count"] += 1
            engine_gaps[rec.category]["bytes"] += rec.size

    # Sample paths per category (up to 8)
    sample_paths: Dict[str, List[str]] = defaultdict(list)
    for rec in sorted(all_records, key=lambda r: r.vpath.lower()):
        if len(sample_paths[rec.category]) < 8:
            sample_paths[rec.category].append(f"{rec.mod}:{rec.vpath}")

    return ScanResult(
        bf2_root=str(root),
        scanned_at=datetime.now(timezone.utc).isoformat(),
        mods=mods,
        archives=archives_scanned,
        total_files=len(all_records),
        by_category=dict(by_category.most_common()),
        by_extension=dict(by_ext.most_common()),
        by_mod=dict(by_mod),
        engine_gaps={k: dict(v) for k, v in sorted(engine_gaps.items())},
        effect_bundles=sorted(effect_bundles),
        levels=levels,
        sample_paths=dict(sample_paths),
    )


def write_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, default=str)
    print(f"Wrote {path}")


def write_gap_report(path: Path, result: ScanResult) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: List[str] = [
        "# Battlefield 2 — Engine Support Gap Report",
        "",
        f"Generated: {result.scanned_at}",
        f"BF2 root: `{result.bf2_root}`",
        f"Total indexed files: **{result.total_files:,}**",
        f"EffectBundle templates found: **{len(result.effect_bundles)}**",
        "",
        "## Category counts",
        "",
        "| Category | Files | Engine status |",
        "| --- | ---: | --- |",
    ]
    for cat, count in sorted(result.by_category.items(), key=lambda x: -x[1]):
        lines.append(f"| `{cat}` | {count:,} | {support_status(cat)} |")

    lines += [
        "",
        "## Priority gaps (missing / partial)",
        "",
        "| Category | Files | Approx bytes | Status |",
        "| --- | ---: | ---: | --- |",
    ]
    for cat, data in sorted(result.engine_gaps.items(), key=lambda x: -x[1]["count"]):
        lines.append(
            f"| `{cat}` | {data['count']:,} | {data['bytes']:,} | {support_status(cat)} |"
        )

    night_levels = [lv for lv in result.levels if lv.is_night]
    lines += [
        "",
        "## Night / low-light maps",
        "",
        f"Detected **{len(night_levels)}** night or dusk maps (Sky.con heuristics + known list).",
        "",
    ]
    for lv in night_levels:
        lines.append(f"- **{lv.mod}/{lv.name}**")
        if lv.sky_hints:
            for k, v in list(lv.sky_hints.items())[:6]:
                lines.append(f"  - `{k}` = `{v}`")

    lines += [
        "",
        "## Per-mod file counts",
        "",
    ]
    for mod, count in sorted(result.by_mod.items(), key=lambda x: -x[1]):
        lines.append(f"- `{mod}`: {count:,} files")

    lines += [
        "",
        "## Effect bundles (sample)",
        "",
    ]
    for name in result.effect_bundles[:40]:
        lines.append(f"- `{name}`")
    if len(result.effect_bundles) > 40:
        lines.append(f"- … and {len(result.effect_bundles) - 40} more (see manifest JSON)")

    lines += [
        "",
        "## Recommended engine work (ordered)",
        "",
        "1. **EffectBundle + SpriteParticleSystem** — parse `.tweak` effects; surface `e_mexp_*` / `e_vexp_*`",
        "2. **Tracer meshes** — `p_tracer_g.bundledmesh` + `tracer_mg.dds`",
        "3. **Night atmosphere** — moon lighting, dark sky gradient, cloud layers from Sky.con",
        "4. **Nav mesh** — `.ai` / `.clb` for bot pathfinding",
        "5. **Shaders** — Refractor `.fx` (water, foliage wind) — optional with GL replacements",
        "6. **Soldier weapon overlays** — full `.baf` state machine per stance",
        "",
        "## Level-specific notes",
        "",
    ]
    for lv in result.levels:
        gap_cats = [c for c in lv.categories if support_status(c) in ("missing", "partial")]
        flag = " 🌙" if lv.is_night else ""
        lines.append(
            f"- **{lv.mod}/{lv.name}**{flag} — {sum(lv.asset_counts.values()):,} assets, "
            f"{len(lv.placements)} static placements"
        )
        if gap_cats:
            lines.append(f"  - partial/missing: {', '.join(sorted(gap_cats)[:12])}")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {path}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Scan all Battlefield 2 assets")
    ap.add_argument(
        "root",
        nargs="?",
        default=r"C:\Program Files (x86)\Battlefield2",
        help="BF2 installation root",
    )
    ap.add_argument("--out", default="reports/bf2_assets", help="Output directory")
    ap.add_argument(
        "--quick",
        action="store_true",
        help="Skip per-tweak EffectBundle parsing (faster)",
    )
    args = ap.parse_args()

    root = Path(args.root)
    out = Path(args.out)
    if not out.is_absolute():
        out = Path(__file__).resolve().parents[2] / out

    result = scan_bf2_root(root, quick=args.quick)

    write_json(out / "bf2_summary.json", {
        "bf2_root": result.bf2_root,
        "scanned_at": result.scanned_at,
        "mods": result.mods,
        "total_files": result.total_files,
        "by_category": result.by_category,
        "by_extension": result.by_extension,
        "by_mod": result.by_mod,
        "engine_gaps": result.engine_gaps,
        "effect_bundle_count": len(result.effect_bundles),
        "night_levels": [f"{lv.mod}/{lv.name}" for lv in result.levels if lv.is_night],
    })

    write_json(out / "bf2_levels.json", [
        {
            **{k: v for k, v in asdict(lv).items() if k not in ("asset_counts", "categories")},
            "asset_counts": dict(lv.asset_counts),
            "categories": sorted(lv.categories),
        }
        for lv in result.levels
    ])
    write_json(out / "bf2_effect_bundles.json", result.effect_bundles)
    write_json(out / "bf2_samples.json", result.sample_paths)

    # Full manifest can be huge — write compact category index only
    manifest = {
        "meta": {
            "bf2_root": result.bf2_root,
            "scanned_at": result.scanned_at,
            "archives": len(result.archives),
            "total_files": result.total_files,
        },
        "by_category": result.by_category,
        "by_extension": result.by_extension,
        "engine_support": ENGINE_SUPPORT,
    }
    write_json(out / "bf2_manifest.json", manifest)
    write_gap_report(out / "bf2_engine_gaps.md", result)

    print(f"\nDone. {result.total_files:,} files across {len(result.mods)} mods.")
    print(f"Night maps: {sum(1 for lv in result.levels if lv.is_night)}")
    print(f"Reports: {out}")


if __name__ == "__main__":
    main()
