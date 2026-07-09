# Project Dalian v0.5.9-alpha

## Multiplayer netcode overhaul — snapshot interpolation + input replication

**Symptoms in v0.5.8:** second player still moved very slowly (host could lap them); remote soldiers looked orange/tan, small in the distance, with guns sticking out of shoulders and deformed arms.

**Root cause:** v0.5.8 added velocity and vehicle replication but still used exponential position smoothing (wrong model for fast movement). Remote facing used velocity instead of look yaw; team colour tints distorted appearance.

**Fixes (industry-standard listen-server netcode):**

### Phase 1 — Movement sync
- **Snapshot interpolation** (Gaffer model): 64-entry timestamped ring buffer per player; render ~100 ms in the past by lerping two known snapshots
- **Dead reckoning** for short gaps (`pos + vel × age`, capped at 250 ms)
- **Hard snap** if render position drifts >2 m from authoritative state
- **Immediate relay:** host rebroadcasts on every client `MSG_STATE` / `MSG_INPUT` (not only host frame)
- **Fixed 60 Hz snapshot timer** decoupled from host render FPS

### Phase 2 — Visual fixes
- **Measured velocity** from position delta each send (not just `desired_velocity`)
- **Remote facing = look yaw** (matches local third-person; fixes gun-through-shoulder)
- **Neutral soldier tint** (no orange/blue team wash on remotes)
- **Interpolated `anim_time`** from snapshot buffer

### Phase 3 — Input replication foundation
- New **`NetInput`** + `MSG_INPUT` wire message (seq, move, yaw/pitch, buttons)
- Sent every frame alongside state; stored server-side per player
- Wire format **v3** adds `input_seq` (backward compatible with v2)

**Both players must use this build.**

## Includes all v0.5.8 fixes

- Vehicle transform replication, spawn-locked unoccupied vehicles
- v0.5.7 viewport fix, v0.5.6 UI/kill feed polish
