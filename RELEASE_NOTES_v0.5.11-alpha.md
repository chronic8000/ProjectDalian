# Project Dalian v0.5.11-alpha

## Map browser — all mods, clear labels

- Single-player **Play** and multiplayer **Host setup** list every map from every installed mod (no deduplication).
- Each row shows **MAP** + **MOD** columns; mod tags are highlighted as `[bf264]`, `[64coop20YA]`, etc.
- Selected-map detail panel shows mod folder and archive folder name.

## Multiplayer UI

- **Faction / army list** scrolls with the mouse wheel in host setup (wheel over the right panel scrolls armies; left panel scrolls maps).
- **Host disconnect:** when the host leaves a match or lobby, clients see **CONNECTION LOST** / *The host has ended the session* and return to the main menu.

## Tank tread rendering

- BF2 treads scroll on the **U** texture axis (was V — caused smeared “radial” strips).
- Tracked tanks: road-wheel pads use UV scroll; **drive sprockets** still rotate as geometry.
- Wheeled APCs unchanged (tires spin, no tread UV scroll).

## Includes all v0.5.10 fixes

- Lobby syncs `map_server_zip` so joiners load the host's map
- v0.5.9 snapshot interpolation, v0.5.8 MP vehicle sync

**Both players should use the same build for multiplayer.**
