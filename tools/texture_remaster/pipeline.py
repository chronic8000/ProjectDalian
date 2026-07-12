#!/usr/bin/env python3
"""
BF2 texture remaster pipeline.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Allow `python pipeline.py` from any cwd
sys.path.insert(0, str(Path(__file__).resolve().parent))

import argparse
import json
import shutil
import subprocess
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import List, Optional, Set

from dds_io import decode_dds_to_rgba, force_pot_rgba, write_png
from inventory import (
    DEFAULT_ROOT,
    classify_texture,
    iter_mod_zips,
    should_skip_upscale,
)
from upscale import Upscaler

TEXTURE_EXTS = {".dds", ".tga", ".png", ".jpg", ".jpeg"}


@dataclass
class EncodeTools:
    texconv: Optional[Path] = None
    nvdxt: Optional[Path] = None

    def describe(self) -> str:
        parts = []
        if self.texconv:
            parts.append(f"texconv={self.texconv}")
        if self.nvdxt:
            parts.append(f"nvdxt={self.nvdxt}")
        return ", ".join(parts) if parts else "NONE (will keep PNG sidecar only)"


def find_encode_tools(bf2_root: Path, texconv_arg: Optional[Path]) -> EncodeTools:
    tools = EncodeTools()
    if texconv_arg and texconv_arg.is_file():
        tools.texconv = texconv_arg
    else:
        which = shutil.which("texconv") or shutil.which("texconv.exe")
        if which:
            tools.texconv = Path(which)
        local = Path(__file__).resolve().parent / "texconv.exe"
        if local.is_file():
            tools.texconv = local
    nvdxt = bf2_root / "NVDXT.EXE"
    if nvdxt.is_file():
        tools.nvdxt = nvdxt
    return tools


def encode_dds(
    png_path: Path,
    dds_path: Path,
    format_name: str,
    tools: EncodeTools,
) -> bool:
    """Recompress PNG -> DDS with mipmaps. Prefer texconv, else NVDXT."""
    dds_path.parent.mkdir(parents=True, exist_ok=True)
    fmt = format_name.upper()
    if fmt not in {"DXT1", "DXT3", "DXT5", "RGBA"}:
        fmt = "DXT5"

    if tools.texconv:
        dx = "BC1_UNORM" if fmt == "DXT1" else ("BC2_UNORM" if fmt == "DXT3" else "BC3_UNORM")
        if fmt == "RGBA":
            dx = "R8G8B8A8_UNORM"
        cmd = [
            str(tools.texconv),
            "-nologo",
            "-y",
            "-m",
            "0",
            "-f",
            dx,
            "-o",
            str(dds_path.parent),
            str(png_path),
        ]
        r = subprocess.run(cmd, capture_output=True, text=True)
        produced = dds_path.parent / (png_path.stem + ".DDS")
        alt = dds_path.parent / (png_path.stem + ".dds")
        src = produced if produced.is_file() else alt
        if r.returncode == 0 and src.is_file():
            if src.resolve() != dds_path.resolve():
                if dds_path.exists():
                    dds_path.unlink()
                src.replace(dds_path)
            return True
        err = (r.stderr or r.stdout or "")[-200:]
        print(f"  [texconv fail] {png_path.name}: {err}")

    if tools.nvdxt:
        # Retail NVDXT wants -outdir; -output path is unreliable. Use -dxt1c / -32.
        out_dir = dds_path.parent / "_nvdxt_tmp"
        out_dir.mkdir(parents=True, exist_ok=True)
        if fmt == "DXT1":
            fmt_args = ["-dxt1c", "-32", "dxt1c"]
        elif fmt == "DXT3":
            fmt_args = ["-dxt3", "-32", "dxt3"]
        elif fmt == "RGBA":
            fmt_args = ["-32", "u8888"]
        else:
            fmt_args = ["-dxt5", "-32", "dxt5"]
        cmd = [
            str(tools.nvdxt),
            "-file",
            str(png_path),
            "-outdir",
            str(out_dir),
            *fmt_args,
        ]
        r = subprocess.run(cmd, capture_output=True, text=True)
        produced = out_dir / (png_path.stem + ".dds")
        if not produced.is_file():
            produced = out_dir / (png_path.stem + ".DDS")
        if produced.is_file():
            if dds_path.exists():
                dds_path.unlink()
            shutil.move(str(produced), str(dds_path))
            try:
                out_dir.rmdir()
            except OSError:
                pass
            return True
        print(f"  [nvdxt fail] {png_path.name}: {(r.stdout or r.stderr or '')[-120:]}")

    return False


def process_dds_bytes(
    data: bytes,
    out_dds: Path,
    work_png: Path,
    upscaler: Upscaler,
    tools: EncodeTools,
    category: str,
) -> dict:
    rgba, info = decode_dds_to_rgba(data)
    # Normals / bump: still upscale but note — Real-ESRGAN is color-trained.
    write_png(work_png, rgba)
    if should_skip_upscale(category):
        # Copy original DDS untouched
        out_dds.parent.mkdir(parents=True, exist_ok=True)
        out_dds.write_bytes(data)
        return {"action": "copied_skip", "format": info.format_name, "w": info.width, "h": info.height}

    up = upscaler.upscale_rgba(rgba)
    up = force_pot_rgba(up)
    up_png = work_png.with_name(work_png.stem + f"_x{upscaler.scale}.png")
    write_png(up_png, up)
    ok = encode_dds(up_png, out_dds, info.format_name, tools)
    if not ok:
        # Fallback: keep original so the zip still works
        out_dds.write_bytes(data)
        return {
            "action": "encode_failed_kept_original",
            "format": info.format_name,
            "w": info.width,
            "h": info.height,
            "up_w": up.shape[1],
            "up_h": up.shape[0],
        }
    return {
        "action": "upscaled",
        "format": info.format_name,
        "w": info.width,
        "h": info.height,
        "up_w": up.shape[1],
        "up_h": up.shape[0],
    }


def rebuild_zip(src_zip: Path, replace_root: Path, dest_zip: Path) -> None:
    """
    Copy every member from src_zip; if replace_root/<member> exists, use that file.
    Preserves all non-texture entries byte-for-byte.
    """
    dest_zip.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest_zip.with_suffix(".zip.tmp")
    if tmp.exists():
        tmp.unlink()
    with zipfile.ZipFile(src_zip, "r") as zin, zipfile.ZipFile(
        tmp, "w", compression=zipfile.ZIP_DEFLATED
    ) as zout:
        for info in zin.infolist():
            name = info.filename
            if info.is_dir():
                zout.writestr(info, b"")
                continue
            rel = name.replace("\\", "/")
            cand = replace_root / rel
            if cand.is_file():
                zout.write(cand, arcname=name)
            else:
                zout.writestr(info, zin.read(info))
    if dest_zip.exists():
        dest_zip.unlink()
    tmp.replace(dest_zip)


def process_zip(
    bf2_root: Path,
    zip_path: Path,
    work_root: Path,
    backup_root: Path,
    hd_root: Path,
    upscaler: Upscaler,
    tools: EncodeTools,
    limit: int,
    include_skipped: bool,
    categories: Optional[Set[str]],
) -> dict:
    rel = zip_path.relative_to(bf2_root)
    rel_s = str(rel).replace("\\", "/")
    print(f"\n=== {rel_s} ===")

    backup_zip = backup_root / rel
    backup_zip.parent.mkdir(parents=True, exist_ok=True)
    if not backup_zip.exists():
        print(f"  backup -> {backup_zip}")
        shutil.copy2(zip_path, backup_zip)
    else:
        print(f"  backup exists: {backup_zip}")

    extract_dir = work_root / "extract" / rel
    png_dir = work_root / "png" / rel
    dds_dir = work_root / "dds" / rel
    for d in (extract_dir, png_dir, dds_dir):
        d.mkdir(parents=True, exist_ok=True)

    log: List[dict] = []
    done = 0
    with zipfile.ZipFile(zip_path, "r") as zf:
        members = [i for i in zf.infolist() if not i.is_dir()]
        for info in members:
            name = info.filename.replace("\\", "/")
            ext = Path(name).suffix.lower()
            if ext not in TEXTURE_EXTS:
                continue
            cat = classify_texture(name)
            if categories and cat not in categories:
                continue
            if should_skip_upscale(cat) and not include_skipped:
                # Still copy original into dds_dir so rebuild is consistent? No —
                # rebuild falls back to source zip for missing files.
                continue
            if limit > 0 and done >= limit:
                break

            data = zf.read(info)
            out_dds = dds_dir / name
            work_png = png_dir / (Path(name).with_suffix(".png"))

            if ext != ".dds":
                # PNG/TGA/JPG: upscale and write same extension (no DDS encode)
                from PIL import Image
                import io
                import numpy as np

                try:
                    img = Image.open(io.BytesIO(data)).convert("RGBA")
                except Exception as exc:  # noqa: BLE001
                    log.append({"path": name, "action": "read_fail", "error": str(exc)})
                    continue
                rgba = np.array(img)
                up = upscaler.upscale_rgba(rgba)
                out_dds.parent.mkdir(parents=True, exist_ok=True)
                Image.fromarray(up, "RGBA").save(out_dds)
                log.append({"path": name, "action": "upscaled_image", "category": cat})
                done += 1
                continue

            try:
                result = process_dds_bytes(data, out_dds, work_png, upscaler, tools, cat)
                result["path"] = name
                result["category"] = cat
                log.append(result)
                done += 1
                if done % 25 == 0:
                    print(f"  ... {done} textures")
            except Exception as exc:  # noqa: BLE001
                log.append({"path": name, "action": "error", "error": str(exc)})
                print(f"  ! {name}: {exc}")

    hd_zip = hd_root / rel
    print(f"  rebuild -> {hd_zip}")
    rebuild_zip(zip_path, dds_dir, hd_zip)

    summary = {
        "zip": rel_s,
        "processed": done,
        "upscaled": sum(1 for x in log if x.get("action") == "upscaled"),
        "errors": sum(1 for x in log if x.get("action") == "error"),
        "log": log,
    }
    (work_root / "logs").mkdir(parents=True, exist_ok=True)
    (work_root / "logs" / (rel_s.replace("/", "__") + ".json")).write_text(
        json.dumps(summary, indent=2), encoding="utf-8"
    )
    print(f"  done: {summary['upscaled']} upscaled, {summary['errors']} errors")
    return summary


def install_hd_over_game(bf2_root: Path, hd_root: Path, backup_root: Path) -> None:
    """
    For each HD zip: ensure backup exists, then copy HD zip over the live path.
    """
    for hd_zip in hd_root.rglob("*.zip"):
        rel = hd_zip.relative_to(hd_root)
        live = bf2_root / rel
        if not live.exists():
            print(f"  skip missing live path: {live}")
            continue
        bak = backup_root / rel
        bak.parent.mkdir(parents=True, exist_ok=True)
        if not bak.exists():
            shutil.copy2(live, bak)
            print(f"  backed up {rel}")
        shutil.copy2(hd_zip, live)
        print(f"  installed HD -> {rel}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Upscale + repack BF2 textures")
    ap.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    ap.add_argument(
        "--work",
        type=Path,
        default=Path(r"C:\BF2_TextureRemaster"),
        help="Working folder (extract/png/dds/logs)",
    )
    ap.add_argument(
        "--backup",
        type=Path,
        default=None,
        help="Original zip backups (default: <work>/backup)",
    )
    ap.add_argument(
        "--hd-out",
        type=Path,
        default=None,
        help="HD zip output tree (default: <work>/hd)",
    )
    ap.add_argument("--maps-only", action="store_true")
    ap.add_argument("--zip", type=str, default="", help="Relative zip under BF2 root")
    ap.add_argument("--scale", type=int, default=2, choices=[2, 4])
    ap.add_argument(
        "--backend",
        choices=["auto", "realesrgan", "ncnn", "lanczos"],
        default="auto",
    )
    ap.add_argument("--limit", type=int, default=0, help="Max textures per zip (0=all)")
    ap.add_argument(
        "--include-skipped",
        action="store_true",
        help="Also process lightmaps/UI/envmaps",
    )
    ap.add_argument(
        "--categories",
        type=str,
        default="",
        help="Comma list filter e.g. terrain_colormap,roads,staticobjects",
    )
    ap.add_argument("--texconv", type=Path, default=None)
    ap.add_argument(
        "--install",
        action="store_true",
        help="After building HD zips, copy them over the live BF2 paths (backups first)",
    )
    ap.add_argument(
        "--install-only",
        action="store_true",
        help="Only copy existing HD zips over live paths",
    )
    ap.add_argument(
        "--skip-existing",
        action="store_true",
        help="Skip zips that already have an HD output (resume after Ctrl+C)",
    )
    args = ap.parse_args()

    bf2 = args.root
    work = args.work
    backup = args.backup or (work / "backup")
    hd_out = args.hd_out or (work / "hd")

    if not bf2.is_dir():
        print(f"BF2 root not found: {bf2}")
        return 1

    if args.install_only:
        install_hd_over_game(bf2, hd_out, backup)
        return 0

    tools = find_encode_tools(bf2, args.texconv)
    print(f"Encode tools: {tools.describe()}")
    if not tools.texconv and not tools.nvdxt:
        print(
            "WARNING: No texconv/NVDXT - DDS re-encode will fail and originals will be kept.\n"
            "  Install DirectXTex texconv.exe into tools/texture_remaster/ or on PATH.\n"
            f"  NVDXT looked for at: {bf2 / 'NVDXT.EXE'}"
        )

    print(f"Init upscaler backend={args.backend} scale={args.scale} ...")
    upscaler = Upscaler(scale=args.scale, backend=args.backend)
    print(f"Upscaler: {upscaler.name}")

    cats = {c.strip() for c in args.categories.split(",") if c.strip()} or None

    zips: List[Path] = []
    if args.zip:
        zp = bf2 / args.zip
        if not zp.is_file():
            print(f"zip not found: {zp}")
            return 1
        zips = [zp]
    else:
        for _scope, zp in iter_mod_zips(bf2, args.maps_only):
            zips.append(zp)

    if not zips:
        print("No zips found.")
        return 1

    print(f"Queued {len(zips)} zip(s). Work={work}")
    all_sum = {
        "started": datetime.now(timezone.utc).isoformat(),
        "upscaler": upscaler.name,
        "scale": args.scale,
        "zips": [],
    }
    for zp in zips:
        rel = zp.relative_to(bf2)
        hd_zip = hd_out / rel
        if args.skip_existing and hd_zip.is_file() and hd_zip.stat().st_size > 0:
            print(f"\n=== {str(rel).replace(chr(92), '/')} ===")
            print(f"  skip existing HD: {hd_zip}")
            all_sum["zips"].append({"zip": str(rel).replace("\\", "/"), "skipped": True})
            continue
        s = process_zip(
            bf2,
            zp,
            work,
            backup,
            hd_out,
            upscaler,
            tools,
            args.limit,
            args.include_skipped,
            cats,
        )
        all_sum["zips"].append({k: v for k, v in s.items() if k != "log"})

    (work / "run_summary.json").write_text(json.dumps(all_sum, indent=2), encoding="utf-8")
    print(f"\nSummary -> {work / 'run_summary.json'}")
    print(f"HD zips -> {hd_out}")
    print(f"Backups -> {backup}")

    if args.install:
        print("\nInstalling HD over live game...")
        install_hd_over_game(bf2, hd_out, backup)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
