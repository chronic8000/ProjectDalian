# Project Dalian v0.5.10-alpha

## Multiplayer — joiners load the host's selected map

**Symptoms in v0.5.9:** when a second player joined, they always loaded Dalian Plant instead of the map the host picked. Both players were not on the same level.

**Cause:** the lobby only synced the map display name, not the `server.zip` path. If name lookup failed (manual IP join, case mismatch, etc.), the client fell back to `maps[0]` — the first map alphabetically (Dalian Plant).

**Fix:**
- Lobby wire format now includes `map_server_zip` (authoritative path to the level archive)
- Host publishes full map info when opening the lobby
- Joiners resolve the map from `server.zip` path first, then display/folder name
- Removed the silent fallback to map index 0; joiners wait for lobby sync instead

**Both players must use this build.**

## Includes all v0.5.9 fixes

- Snapshot interpolation netcode, input replication, measured velocity
- v0.5.8 vehicle/animation MP sync, v0.5.7 viewport fix
