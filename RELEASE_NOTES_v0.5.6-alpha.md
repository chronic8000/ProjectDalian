# Project Dalian v0.5.6-alpha

## Highlights

- **UI text polish** — menus, multiplayer lobby, deploy screen, loading screen, controls panel, and in-game HUD now clip/truncate text so labels never overlap or render off-screen
- **Kill feed** — top-right toasts for kills using retail BF2 bot names from `BotNames.ai` (`Killer killed Victim with Weapon`)
- **Join / leave notifications** — gold join toast and red leave toast when humans connect or disconnect (Tailscale/LAN)
- **Multiplayer HUD cleanup** — removed floating 3D name tags and minimap teammate blips; MP roster capped so it does not overlap the minimap

## UI fixes (by area)

**Main menu:** map list columns no longer collide; map details and BF2 path truncate; resolution popup scrolls instead of extending off-screen.

**Multiplayer:** scrollable server list and lobby player list; faction picker and name field separated; slider values stay inside columns; Connecting/Map lines no longer stack.

**In-game:** deploy headers and spawn labels clip; ticket bar faction names sit beside ticket counts; minimap grace text and round timer on separate lines.

## Kill feed & bot names

Bot display names are loaded from your BF2 install (`mods/bf2/AI/BotNames.ai`, with fallbacks for xpack/bf264). Human players show their lobby name.

## Known gaps

See [docs/PARITY.md](docs/PARITY.md) — full player replication, synchronized match start, and dedicated server polish remain in progress.
