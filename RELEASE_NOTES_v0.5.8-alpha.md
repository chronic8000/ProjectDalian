# Project Dalian v0.5.8-alpha

## Multiplayer sync — movement, animation, and vehicles

**Symptoms in v0.5.7:** the second player moved very slowly (host could run laps around them); remote soldiers slid with frozen legs; helicopters showed as a floating soldier with no vehicle model; parked vehicles could drift out of sync between clients.

**Fixes:**

- Extended player replication with velocity, animation time, pose, and vehicle id/transform (backward-compatible wire format)
- Faster remote smoothing with velocity extrapolation so other players keep up at run speed
- Full soldier animation set for remote players (walk/run/crouch/prone via synced `anim_time` + pose)
- Vehicle transform replication when driving — remotes see the actual helicopter/tank mesh, not a hovering body
- Unoccupied vehicles lock to their map spawn point; spawn anchors stored at load time
- Vehicle draw culling uses live position instead of stale spawn origin

**Test with two clients on the same build:** on-foot movement speed, crouch/prone visibility, helicopter pilot + observer, parked vehicles staying put.

## Includes all v0.5.7 fixes

- In-game viewport no longer stretched/skewed after shadow pass
- v0.5.6 UI clipping, kill feed, join/leave toasts
