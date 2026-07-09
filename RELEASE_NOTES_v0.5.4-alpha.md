## v0.5.4-alpha — BF2 controls, Tailscale multiplayer, bot options

**Download `ProjectDalian-v0.5.4-alpha-win64.zip`**

### Multiplayer (Tailscale / LAN)

- **CREATE TAILSCALE SERVER** — ENet host + UDP discovery start immediately from the
  multiplayer browser (before map load / before you enter the match)
- Joiners **auto-scan** the Tailscale `100.x/24` subnet every few seconds; server
  list refreshes while browsing
- Host advertises on **LAN + Tailscale** through lobby, setup, and in-match
- **Late join** still supported when enabled
- Host setup: **AI bots on/off**, count (0–128), difficulty (1–5)

### Controls & options (BF2 parity — Tier 1)

- Retail **BF2 default keymap** with full **rebind UI** in Options → Controls
- Status column: **Ready** vs **Coming soon** for each action
- **Crouch** (Ctrl), **Prone** (Z), grenade on **4**, camera cycle on **C**
- **Tab hold** scoreboard; mouse look always on in-game
- Dalian extras on **F9/F10/H** so BF2 keys stay free
- Audio tab: Master / Effects / Music / Voice

### Gameplay & rendering fixes

- **Spawn fall-through** — heightmap cluster fallback, ground snap during deploy
- **Tank treads** — BF2 UV scroll (not geometry spin); only `c_ETTank` is tracked
- **Wheeled APCs** — wheel spin restored (`c_ETNewCar2` no longer treated as tracked)

### Sync note (multiplayer alpha)

Player movement, look, health, and shots replicate over **ENet/UDP**. Vehicles,
conquest, and bots are still **host-local** — fine for co-op foot-soldier testing.

### Requirements

- Windows 10/11 x64 + Battlefield 2 install
- **Tailscale** on both PCs for easy WAN co-op (or LAN / manual IP)
- Allow UDP **27015** (game) and **27016** (discovery) through Windows Firewall

### Quick multiplayer test

1. Host: Multiplayer → **CREATE TAILSCALE SERVER** → pick map → **OPEN LOBBY** → **START GAME**
2. Joiner: Multiplayer → wait for server in list (or enter host Tailscale IP) → **JOIN**
