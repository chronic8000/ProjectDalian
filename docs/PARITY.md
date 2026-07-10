# Parity status (Project Dalian vs retail BF2)

Last updated: **2026-07-10** (v0.5.11-alpha line)

This document tracks what is implemented, what is partial, and what is still missing.
**Does not claim full BF2 parity** — several combat, flight, networking, and
world-placement areas remain open.

## Implemented

| Area | Status |
|------|--------|
| Vehicle weapons from tweak | Heli minigun/rockets, tank main gun parsed from vehicle child `.tweak` blocks |
| Distinct missiles | SAM (`igla_9k38`, **F8** car-launch) vs AT (`at_predator` / `insgr_rpg`, **RMB**) |
| Handheld weapons | Spread/deviation ramp, magazine/reload, tweak `addTemplate` inheritance, burst fire |
| Gadgets | C4 (5/X), grenades (4), medkit (H), FPV drones (F9/F10) |
| AI defenders | Retail weapon profile; `.ai` navmesh A*; squad roles (assault/flank/suppress) |
| Audio | SDL_mixer weapons/engines/voice/music |
| Ambient FX | Fountains/smoke/baselight placements; baselight glow billboards |
| Placement audit tool | `placement_audit` CLI — flags floating/embedded props vs terrain |
| Overgrowth foot snap | Trees/bushes offset by mesh min-Y so feet rest on terrain |

## Partial / alpha

| Area | Gap |
|------|-----|
| Afterburner FX | Uses exhaust smoke billboards + flash texture, not full `e_jetexhaust_AB_s` particle system |
| Jet flight | Per-aircraft tweak data wired; gear timing and feel still need tuning |
| Conquest | Tickets, CP capture, voice cues — not full retail ruleset |
| Multiplayer | Host/join/dedicated, player+shot+vehicle transform sync — seats/prediction/auth damage incomplete |
| Static prop height | Authored `StaticObjects.con` Y used as-is; deck/road props may float vs bare terrain |
| Vehicle ground contact | Clearance heuristic; tires can embed on roads — road/bridge collision improved |
| Walk/drive through props | **Fixed for most meshes** — bundledmesh collision + compiled-road render fallback; some gaps remain |
| Graphics post | **HDR RGB16F + ACES tone map + SSAO + bloom** shipped; water SSR still open |

## Not started / backlog

- Tank turret yaw + coax as separate seat weapon
- Hellfire TV guidance for gunner secondary
- Sniper zoom/optics
- Full `rem` / nested tweak inheritance beyond `addTemplate`
- Per-sprite emitter textures from effect bundles
- Refractor foliage wind shaders
- In-engine editor / Blender IO
- 85-map unresolved template sweep (carrier composites, windmills, etc.)
- Water SSR
- Client prediction / authoritative MP damage

## Audit tools

```powershell
# Asset resolution (textures, unresolved templates)
.\build\tools\level_validator\level_validator.exe "C:\Program Files (x86)\Battlefield2" --verbose

# Placement height (hovering plants, sunk meshes)
.\build\tools\placement_audit\placement_audit.exe "C:\Program Files (x86)\Battlefield2" `
  --mod bf2 --level Dalian_plant --verbose

# All levels → JSON
.\scripts\validate_all_levels.ps1
.\scripts\run_placement_audit.ps1
```

Thresholds (defaults): **float** if mesh foot is >0.35 m above terrain; **embed** if >0.25 m below.

## Known visual issues

- **Hovering foliage**: overgrowth instances now snap mesh foot to terrain; static props on
  slopes/decks may still look wrong until collision-surface snapping is added.
- **Sunk vehicle wheels**: clearance uses mesh bounds + downward raycast; compiled road
  collision vs terrain mismatch can bury tires — check `BF2_VEHLIST=1` spawn log.
