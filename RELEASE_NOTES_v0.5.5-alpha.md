# Project Dalian v0.5.5-alpha

## Highlights

- **BF2-style loading screen** with progress bar, phase text, and tip/art panel
- **Before the First Volley** plays during load only (from Downloads or `music/` folder)
- **Ready gate** — listen to the full track, then press Ready before deployment
- **Resolution fix** — changing resolution in Options now refreshes the GL viewport immediately (no Alt+Enter workaround)
- **Multiplayer polish** — ticket bleed grace until 2 humans / 90s, teammate minimap blips, yaw sync fix, auto round restart, clearer net logs

## Loading music

Place `Before_the_First_Volley.mp3` in your Downloads folder, or set:

```
BF2_LOADING_MUSIC=C:\path\to\Before_the_First_Volley.mp3
```

Skip the Ready button in automation: `BF2_SKIP_LOADING_READY=1`

## Multiplayer testing

1. Host: Multiplayer → **CREATE TAILSCALE SERVER** → pick map → wait on loading Ready screen
2. Joiner: scan browser → join → same loading flow
3. Both deploy; HUD shows `N in match` and cyan minimap dots for teammates
4. Host console logs `Net: player joined` when someone connects

## Known gaps

See [docs/PARITY.md](docs/PARITY.md) for full status — networking replication, synchronized match start, and dedicated server polish remain in progress.
