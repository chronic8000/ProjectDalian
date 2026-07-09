#!/usr/bin/env python3
"""
Scan a Battlefield 2 install for gameplay mechanics data (conquest, vehicles, gamemode).

Complements bf2_asset_scan.py (file inventory) and level_validator (static mesh gaps).

Usage:
  python bf2_mechanics_scan.py "C:/Program Files (x86)/Battlefield2"
  python bf2_mechanics_scan.py --root ... --out reports/bf2_mechanics
"""

from __future__ import annotations

import argparse
import json
import re
import zipfile
from collections import Counter, defaultdict
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple

VEHICLE_PHYSICS_KEYS = (
    "setGearUpSpeed",
    "setGearDownSpeed",
    "setGearUpHeight",
    "setWingLift",
    "setFlapLift",
    "engine.maxThrust",
    "engine.sprintFactor",
    "physics.wingLift",
    "physics.mass",
    "physics.drag",
)

CP_PROPS = (
    "controlPointId",
    "areaValueTeam1",
    "areaValueTeam2",
    "unableToChangeTeam",
    "team",
    "radius",
    "enemyTicketLossWhenCaptured",
)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def read_zip_member(zpath: Path, member: str) -> str:
    try:
        with zipfile.ZipFile(zpath) as zf:
            if member not in zf.namelist():
                return ""
            return zf.read(member).decode("utf-8", errors="replace")
    except (OSError, zipfile.BadZipFile):
        return ""


def parse_game_logic_init(text: str) -> Dict:
    out: Dict = {"tickets": {}, "valid": False}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        if parts[0].lower() == "gamelogic.setdefaultnumberoftickets":
            team, tickets = int(parts[1]), int(parts[2])
            out["tickets"][str(team)] = tickets
            out["valid"] = True
    return out


def parse_control_points(text: str) -> List[Dict]:
    cps: List[Dict] = []
    cur: Optional[Dict] = None
    positions: Dict[str, Tuple[float, float, float]] = {}

    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.lower().startswith("rem"):
            continue
        parts = line.split()
        cmd = parts[0].lower()

        if cmd == "objecttemplate.create" and len(parts) >= 3:
            typ, name = parts[1].lower(), parts[2]
            if typ == "controlpoint":
                cur = {"name": name, "props": {}}
            else:
                cur = None
            continue

        if cur is not None and cmd.startswith("objecttemplate."):
            prop = cmd.split(".", 1)[1]
            if prop in {p.lower() for p in CP_PROPS} and len(parts) >= 2:
                cur["props"][prop] = parts[1]
            continue

        if cmd == "object.create" and len(parts) >= 2:
            cur_obj = parts[1]
            if cur and cur["name"] == cur_obj:
                pass  # placement follows
            continue

        if cmd == "object.absoluteposition" and len(parts) >= 2 and cur:
            triple = parts[1].split("/")
            if len(triple) == 3:
                try:
                    positions[cur["name"]] = tuple(float(x) for x in triple)
                except ValueError:
                    pass
            cps.append(cur)
            cur = None

    for cp in cps:
        if cp["name"] in positions:
            cp["position"] = positions[cp["name"]]
    return cps


def scan_vehicle_tweak(text: str) -> Dict[str, str]:
    found: Dict[str, str] = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        key = parts[0]
        for vk in VEHICLE_PHYSICS_KEYS:
            if key.lower() == vk.lower():
                found[vk] = parts[1]
    return found


def scan_mod(root: Path, mod: str) -> Dict:
    mod_dir = root / "mods" / mod
    report: Dict = {"mod": mod, "game_logic": {}, "levels": [], "air_vehicles": []}

    gli = parse_game_logic_init(read_text(mod_dir / "GameLogicInit.con"))
    report["game_logic"] = gli

    levels_dir = mod_dir / "Levels"
    if levels_dir.is_dir():
        for level_dir in sorted(levels_dir.iterdir()):
            if not level_dir.is_dir():
                continue
            server = level_dir / "server.zip"
            if not server.exists():
                continue
            gp = ""
            for rel in (
                "GameModes/gpm_cq/64/GamePlayObjects.con",
                "GameModes/gpm_cq/32/GamePlayObjects.con",
                "GameModes/gpm_cq/16/GamePlayObjects.con",
            ):
                gp = read_zip_member(server, rel)
                if gp:
                    break
            cps = parse_control_points(gp) if gp else []
            report["levels"].append(
                {
                    "level": level_dir.name,
                    "control_points": len(cps),
                    "sample_cp": cps[0] if cps else None,
                    "total_area_t1": sum(
                        int(cp.get("props", {}).get("areaValueTeam1", 0) or 0) for cp in cps
                    ),
                    "total_area_t2": sum(
                        int(cp.get("props", {}).get("areaValueTeam2", 0) or 0) for cp in cps
                    ),
                }
            )

    obj_server = mod_dir / "Objects_server.zip"
    if obj_server.exists():
        try:
            with zipfile.ZipFile(obj_server) as zf:
                air_cons = [
                    n
                    for n in zf.namelist()
                    if n.lower().startswith("vehicles/air/") and n.lower().endswith(".tweak")
                ][:40]
                for con in air_cons:
                    tweak = zf.read(con).decode("utf-8", errors="replace")
                    keys = scan_vehicle_tweak(tweak)
                    if keys:
                        report["air_vehicles"].append({"path": con, "physics_keys": keys})
        except zipfile.BadZipFile:
            pass

    return report


def main() -> int:
    ap = argparse.ArgumentParser(description="BF2 mechanics scanner")
    ap.add_argument("--root", default=r"C:\Program Files (x86)\Battlefield2")
    ap.add_argument("--out", default="reports/bf2_mechanics")
    ap.add_argument("--mods", nargs="*", default=["bf2", "xpack", "bf2sf64"])
    args = ap.parse_args()

    root = Path(args.root)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    reports = [scan_mod(root, mod) for mod in args.mods]
    summary = {
        "scanned_at": datetime.now(timezone.utc).isoformat(),
        "root": str(root),
        "mods": args.mods,
        "dalian_gaps": [
            "Ticket bleed now uses areaValue (gpm_cq.py parity)",
            "Starting tickets from GameLogicInit.con",
            "Vehicle maxThrust/sprintFactor parsed but not applied in flight sim",
            "Heli collective/RPM still hardcoded",
            "Emitter/light templates excluded from static mesh audit",
        ],
        "reports": reports,
    }

    out_json = out_dir / "mechanics_summary.json"
    out_json.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Wrote {out_json}")
    for rep in reports:
        gl = rep.get("game_logic", {})
        if gl.get("valid"):
            print(f"  {rep['mod']}: default tickets {gl.get('tickets', {})}")
        print(f"  {rep['mod']}: {len(rep['levels'])} levels, {len(rep['air_vehicles'])} air tweaks sampled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
