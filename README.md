# Project Dalian

**A modern, open-source, from-scratch C++ recreation of the Refractor 2 engine —
evolving Battlefield 2's 2004 tech into a 2026-grade tactical war simulator.**

Project Dalian is a clean-room reimplementation of the Refractor 2 engine that
reads original Battlefield 2 data (meshes, terrain, textures, animations, level
scripts) from *your own* game installation and renders it in a modern OpenGL
pipeline — then goes well beyond the original with skeletal hit registration,
physical ballistics, 6-DoF FPV drone flight, and a PBR rendering path.

It began as a fork of the excellent
[BattlefieldRespawn](https://github.com/rigred/BattlefieldRespawn) project by
Rigo "Pixel" Reddig (which reverse-engineered the mesh/animation file headers),
and carries that reverse-engineering work forward into a fully running engine
with terrain, static geometry, collision, skinned characters, vehicles, weapons,
enemy AI and drones.

---

## ⚠️ BLEEDING-EDGE ALPHA — READ THIS FIRST

This is **pre-alpha / experimental** software. It is a hobby engine under very
active development.

- It **works, kind of.** You can load real BF2 levels, walk around, shoot,
  drive the camera, fly a drone, and fight bots.
- It is **full of bugs.** Expect crashes, visual glitches, broken collision in
  places, half-finished features, rough animation, and things that only work on
  one specific map/asset.
- **Test at your own risk.** No warranty of any kind. It may crash, hang, or
  behave unexpectedly. Save your work in other apps before running it.
- APIs, file layout, controls, and behaviour **will change without notice.**

If that sounds fun, welcome aboard. If you need something stable, come back
later.

---

## Legal / Assets

**No EA or DICE copyrighted material is included in this repository.** There are
no `.zip` archives, textures, meshes, sounds, or any other Battlefield 2 game
content committed here — only original source code written for this project
(plus small, permissively-licensed third-party libraries and a tiny
hand-authored cube used to test the mesh parser).

Project Dalian **requires a legitimate installation of Battlefield 2** (or a
compatible mod such as Project Reality). At runtime the engine reads assets
directly out of your local BF2 archives. You must own the game. This project is
not affiliated with or endorsed by EA or DICE. "Battlefield" is a trademark of
its respective owner.

---

## What works today

**Launcher & UI:**
- **Main menu** with map browser (scans your BF2 install), options, multiplayer tab,
  and quit — no command-line args required to start
- **BF2-style loading screen** — progress bar with phase text while archives load;
  **Before the First Volley** plays during load only; **Ready** button lets you listen
  to the full track before deployment opens
- **Resolution-agnostic UI** — all menus/HUD layout in a fixed 1600×900 design
  space with uniform scaling and letterboxing; long text clips instead of overlapping
- **Dynamic resolution list** from SDL (includes desktop ultrawide modes)
- **Settings persistence** (`%APPDATA%\ProjectDalian\ProjectDalian\settings.cfg`):
  video, graphics, audio, BF2 install path, player name, rebindable controls
- **Display recovery hotkeys:** `Alt+Enter` toggle windowed/borderless, `F11` cycle
  modes, `Ctrl+Shift+W` safe windowed 1920×1080; launch with `--windowed` or
  `BF2_WINDOWED=1` if stuck in bad fullscreen
- **Resolution changes apply immediately** — Options → Video → Apply updates the GL
  viewport without needing Alt+Enter

**Multiplayer (alpha):**
- Host/join flow with faction picker, ready states, and lobby start
- **Tailscale server browser** — host advertises on create; joiners auto-scan `100.x/24`
- LAN server browser + optional Tailscale subnet scan
- Late-join support (configurable)
- Session discovery/advertising over UDP
- **MP ticket grace** — bleed paused until 2 humans deployed or 90s elapse
- **Kill feed** — top-right kill toasts with retail BF2 bot names; join/leave toasts for human players
- Improved position/yaw sync for remote players
- **Auto round restart** — 12s countdown after victory, then deploy again

**Asset pipeline (verified byte-for-byte against retail BF2 assets):**
- `.staticmesh`, `.bundledmesh`, `.skinnedmesh` loaders (multi-geometry / multi-LOD)
- `.ske` skeleton + `.baf` animation loaders and hierarchy evaluation
- `.collisionmesh` (cols / faces / verts / BSP)
- `.dds` textures (DXT1/DXT3/DXT5)
- Zip archive mounting (miniz / DEFLATE) — a small virtual file system
- `.con` / `.tweak` interpreter (templates, `activeSafe`, object placement)
- 16-bit heightmap terrain + `StaticObjects.con` placement resolution

**Rendering:**
- Terrain colormap + detail splatting with a shared-detail blend
- Static geometry with BF2 material layers: `_c` base, `_de` detail, `_deb`
  normal map, `_di` dirt (tiling break), `_cr` crack decal
- Baked per-object lightmaps (UV5 / `Lightmaps/Objects` atlases)
- Water plane (Fresnel + specular glint), gradient sky, sun, distance fog
- Undergrowth / grass billboards with wind sway
- GPU skinning (bone-matrix palette evaluated in the vertex shader)
- **PBR surface path** (Cook-Torrance GGX; albedo from `_c`+`_de`, synthetic
  roughness from detail-normal intensity)
- **Cascaded Shadow Maps** depth pass (render occluders to a shadow array,
  sampled with PCF in the surface shader)
- Offscreen-FBO post-process pass (used for the FPV drone feed)

- **Vehicle geometry assembly** from `.con` hierarchy (hull + child parts such as
  rotors); internal geometry parts (wheels, turrets, landing gear) positioned via
  `geometryPart` indices baked from bundledmesh `BLENDINDICES`
- **Helicopter rotor blur** (static blades at low RPM → blur disc at speed)
- **Landing gear tuck animation** on jets (gear struts, hatches, wheels split from
  hull mesh and rotated from `.con` `LandingGear` data)

**Level / map fidelity (Dalian Plant and compatible maps):**
- **3×3 secondary heightmap cluster** — samples neighbouring `HeightmapSecondary_*`
  patches so props and vehicles sit on decks/ramps instead of bare primary terrain
- **Overgrowth trees** — `Overgrowth.raw` instancing for distant tree/bush cover
- **Compiled roads** — `CompiledRoads.con` spline data for road-aligned placement
- **Texture parent-folder resolve** — finds DDS colour maps when BF2 stores them
  one directory up (fixes purple/missing materials on pipes, etc.)
- **Template resolver** — nested `.tweak` / `.con` runs for multi-part statics
- **DummyObjects.con merge** — decorative props referenced only from dummy lists
- **Asset audit on load** — optional `BF2_TEXAUDIT=1` reports missing textures
- **Cheaper water shader** — distance fade, fewer waves, skips work when high above
  the surface
- **Boat buoyancy** — sea vehicles float at the water plane instead of driving the
  seabed

**Combat & vehicles (v0.5.0-alpha):**
- **Vehicle weapon profiles** — heli minigun/rockets, tank main gun from retail vehicle `.tweak`
- **Distinct missiles** — SAM vs AT/RPG projectile templates (`igla`, `at_predator`, `insgr_rpg`)
- **Handheld depth** — deviation ramp, burst fire, `e_bhit_*` impacts, tweak `addTemplate` chains
- **AI kit weapons** — defenders use retail rifle fire rate/damage/spread
- **Placement audit** — `placement_audit` tool flags floating/embedded props (`docs/PARITY.md`)
- **Collision** — bundledmesh + compiled-road collision; soldier+vehicle col merge; render-mesh fallback for roads/bridges

See **[docs/PARITY.md](docs/PARITY.md)** for the full gap list (afterburner particles, jet tuning,
multiplayer, conquest, static prop heights, etc.).

**Audio (SDL_mixer, reads retail BF2 archives):**
- Weapon fire / reload / deploy sounds parsed from weapon `.tweak` files
- Voice lines from `VoiceMessages*.con` (reload, grenade, out-of-ammo, etc.)
- Level ambient loop from `AmbientObjects.con`
- Menu music: **The Siege of Dalian** (main menu); **Before the First Volley** (loading only)
- Bundled OGG/MP3 under `apps/dalian/assets/music/` or your `Downloads/` folder
- **Vehicle engine / tire sounds** parsed from vehicle `.tweak` (loops on enter,
  volume follows throttle/speed) — coverage depends on per-vehicle tweak data

**Simulation / gameplay:**
- **Deploy screen** (Enter): kit picker, faction selection, spawn point map
- **Vehicle entry/exit** (`E`), multi-seat crew switching (`F1`–`F8`)
- **Ground vehicles:** throttle, steering, terrain-following chassis tilt, wheel
  spin/steer on separated geometry parts where `.con` defines wheel geometry
- **Helicopters:** BF2-style collective (W/S), yaw (A/D), mouse cyclic
  pitch/roll, rotor spool RPM, AI gunner seat, flares (`X`)
- **Fixed-wing jets (alpha):** BF2-inspired arcade flight with body-wing lift,
  engine spool before runway roll, V1 rotation (~72 km/h), double-tap W or
  `Ctrl` afterburner (sprint fuel + 1.6× thrust), `G` landing-gear stow with
  tuck animation, mouse pitch/roll, rudder (A/D), horizon damping
- **Conquest layer (alpha):** tickets, control points, capture logic, voice cues
- **Modular game sim** — `game_sim.cpp` + `.inl` shards (vehicles, missiles,
  interaction, conquest) with net-ready snapshot types
- **In-game pause menu** (Esc): resume, options, leave to main menu
- Character controller with bilinear terrain following and object collision
- First- and third-person camera, animated player soldier + held weapon
- Weapon switching (Q or mouse wheel), automatic fire, muzzle flash, recoil
- **Two hit-registration models, hot-swappable (F):**
  - Hitscan raycast
  - Physical projectiles with gravity + quadratic air drag + wind deflection
- **Skeletal capsule colliders** mapped to individual animated bones (head /
  torso / limbs) for per-zone hit detection instead of one big box
- **Enemy AI** spread across map objectives: idle patrol, advance-to-contact
  when spotted, reaction delay, burst fire, limited simultaneous attackers,
  player health regen (tuned to be survivable, not an instant-death swarm)
- **6-DoF FPV recon drone** (`F9` to toggle): true rigid-body quad with independent
  per-rotor thrust, gravity, and simulated battery voltage sag, plus a
  render-to-texture FPV feed with signal degradation
- **FPV kamikaze loitering munition** (`F10`): a one-way, player-steered terminal
  munition using a lighter/faster airframe. FPV link with faster signal decay;
  detonates on impact (terrain, buildings, enemies), manual trigger (LMB/Space),
  or dead battery. No recall.
- **Car-launched SAM** (`F8`): opens a tactical map; click a destination anywhere
  on the map. The Igla launches from the nearest ground vehicle roof with a lofted
  climb so you can watch it fly. Uses retail BF2 `igla_9k38` mesh plus
  `e_missile_ignition` / `e_missile_trail` / `e_vexp_igla` effect bundles when
  present. AT rockets use **RMB** on the Anti-Tank kit.

---

## Download (Windows, pre-built)

**No compile required** — grab the latest release from
[GitHub Releases](https://github.com/chronic8000/ProjectDalian/releases).

1. Download `ProjectDalian-*-win64.zip`
2. Unzip anywhere
3. Run `project_dalian.exe`
4. Point **Options → BF2 path** at your retail install (`C:\Program Files (x86)\Battlefield2`)

The zip contains only Project Dalian binaries, SDL2 runtime DLLs, and bundled menu
music/art. **No EA/DICE game data** — you must own Battlefield 2.

---

## Build from source

Requirements:
- CMake 3.24+
- A C++20 compiler (MSVC 2022, Clang, or GCC/MinGW-w64)
- Git (dependencies are fetched at configure time)

```powershell
cd C:\Projects\bf2respawn
cmake --preset default
cmake --build build
ctest --test-dir build      # optional: run parser unit tests
```

Third-party libraries used: SDL2, SDL_mixer, GLM, Dear ImGui, GLEW, miniz, stb.
Small vendored copies (GLEW / miniz / stb) live under `third_party/` and are
permissively licensed. On Windows, `SDL2.dll` and `SDL2_mixer.dll` are copied
next to `project_dalian.exe` at build time.

---

## Run

### Quick start (main menu)

Build, then launch with no arguments. The main menu scans maps under your BF2
install (set the path in Options if auto-detect fails):

```powershell
.\build\apps\dalian\project_dalian.exe
```

Recovery if display settings break the UI:

```powershell
.\build\apps\dalian\project_dalian.exe --windowed
# or delete/edit %APPDATA%\ProjectDalian\ProjectDalian\settings.cfg
```

Loading music: place `Before_the_First_Volley.mp3` in your Downloads folder, or set
`BF2_LOADING_MUSIC=C:\path\to\track.mp3`. Skip the Ready gate in automation with
`BF2_SKIP_LOADING_READY=1`.

### Direct map load (legacy / headless)

Point the app at a BF2 level archive plus your `Objects_client.zip`:

```powershell
$bf2 = "C:\Program Files (x86)\Battlefield2\mods\bf2"

.\build\apps\dalian\project_dalian.exe `
    "$bf2\Levels\Dalian_plant\server.zip" `
    "$bf2\Objects_client.zip"
```

Settings and the BF2 root path from the main menu are stored under
`%APPDATA%\ProjectDalian\ProjectDalian\settings.cfg`.

### Controls

| Input | Action |
|---|---|
| **Menu** | |
| `Alt+Enter` | Toggle windowed / borderless |
| `F11` | Cycle windowed → borderless → exclusive |
| `Ctrl+Shift+W` | Safe windowed 1920×1080 (display recovery) |
| **Loading** | |
| `Ready` button / Enter / Space | Continue to deploy after map load |
| **On foot** | |
| `W` `A` `S` `D` | Move |
| `Shift` | Sprint |
| `Space` | Jump |
| Mouse | Look |
| `Tab` | Toggle mouse capture |
| `LMB` | Fire (hold for automatic) |
| `RMB` | AT rocket (Anti-Tank kit) / vehicle secondary |
| Mouse wheel | Cycle weapon |
| `R` | Reload |
| `4` / `5` / `X` | Grenade / place C4 / detonate C4 (or vehicle flares) |
| `F8` | Open car-SAM map (near a ground vehicle); click destination to fire |
| `F9` | Launch / recall FPV recon drone |
| `F10` | Launch FPV kamikaze loitering munition (one-way) |
| `H` | Medkit self-heal (Medic kit) |
| `C` | Toggle 1st / 3rd person |
| `E` | Enter / exit vehicle |
| `Enter` | Deploy screen (kit, faction, spawn) |
| `Esc` | Pause menu (in-game) / back (deploy) |
| **Helicopter (pilot)** | |
| `W` / `S` | Collective up / down |
| `A` / `D` | Yaw |
| Mouse | Cyclic pitch / roll |
| `X` | Flares |
| `F1`–`F8` | Switch crew seat |
| **Jet (pilot)** | |
| `W` / `S` | Throttle / brake |
| `A` / `D` | Rudder |
| Mouse pull back | Pitch up (rotate on runway / climb in air) |
| Mouse left/right | Bank (in air) |
| Double-tap `W` or `Ctrl` | Toggle afterburner (sprint fuel) |
| `G` | Stow / deploy landing gear |
| `F1`–`F8` | Switch crew seat |
| **Drone** | |
| `W`/`S` throttle, `A`/`D` yaw, mouse pitch/roll | Fly the quad |

---

## Tools

Headless, windowless utilities for inspecting and rendering assets straight from
a retail archive (great for debugging the loaders):

- `meshinfo` — dump mesh header / geometry / material info
- `assetprobe` — load any asset (mesh / ske / baf / collisionmesh / dds / con /
  tweak) directly out of an archive and print stats
- `bf2snapshot` — offscreen renderer: render a mesh, a skinned+posed character,
  a level's terrain, or a full assembled level to a PNG with no window

```powershell
$zip = "C:\Program Files (x86)\Battlefield2\mods\bf2\Objects_client.zip"
$lvl = "C:\Program Files (x86)\Battlefield2\mods\bf2\Levels\Dalian_plant\server.zip"

.\build\tools\meshinfo\meshinfo.exe tests\fixtures\cube.staticmesh
.\build\tools\snapshot\bf2snapshot.exe --level $lvl level.png 1100
```

The main app also supports a scripted headless capture:
`project_dalian.exe <level.zip> <objects.zip> --shot out.png [frames] [--tp] [--drone]`.

---

## Architecture

```
engine/
  formats/   mesh, animation, archive, collision, dds, terrain loaders
  core/      level loader, resource manager, scene graph, template resolver,
             atmosphere, object lightmaps, undergrowth
  anim/      skeleton posing + CPU/GPU skinning (bone-matrix palette)
  render/    OpenGL renderer, PBR + shadow shaders, cascaded shadow maps,
             texture cache
  physics/   physics world, character controller, 6-DoF drone, missile flight,
             kamikaze loitering munition
  script/    .con/.tweak interpreter + gameplay script sandbox
  net/       UDP lobby protocol, player sync, server discovery hooks
apps/
  dalian/    war-sim client: main menu, deploy UI, multiplayer lobby, game audio,
             settings, factions — see apps/dalian/*.cpp
  viewer/    SDL2 + ImGui mesh/skeleton/animation viewer
  sandbox/   physics/gameplay/network smoke test
tools/       meshinfo, assetprobe, snapshot (headless renderer)
tests/       parser unit tests + hand-authored fixtures
docs/        file-format reverse-engineering notes
```

---

## Roadmap status

- **Phase 0** — project foundation, mesh loader, meshinfo CLI ✔
- **Phase 1** — zip archive mount, DDS loader, full geometry extraction, tests ✔
- **Phase 2** — OpenGL viewer + ImGui browser + headless PNG snapshot ✔
- **Phase 3** — `.ske`/`.baf` loaders, skeleton eval, CPU + GPU skinning ✔
- **Phase 4** — `.con`/`.tweak` interpreter (templates, placements) ✔
- **Phase 5** — heightmap terrain + static object placement + level assembly ✔
- **Phase 6** — physics world + `.collisionmesh` + character controller ✔
- **Phase 7** — gameplay script sandbox ✔
- **War-sim layer** — skeletal capsule hitboxes, physical ballistics,
  6-DoF FPV drones, vehicle-launched guided/ballistic missiles, PBR + cascaded
  shadow maps, enemy AI ✔ (rough / alpha)
- **War-sim layer (2026 sprint)** — main menu + map browser, settings/options,
  resolution-agnostic UI, deploy screen + factions, vehicle entry/driving,
  helicopter flight, wheel animation split, BF2 weapon/voice/ambient audio,
  vehicle engine sounds (partial), multiplayer lobby + LAN/Tailscale browser ✔
  (alpha / incomplete)
- **In progress** — jet polish (per-aircraft gear/exhaust), animation retargeting,
  full vehicle audio coverage, networking hardening, particle effects (afterburner
  flames), in-engine editor
- **Next** — more munitions, radio comms, live volume sliders, dedicated server
  polish, envmaps / extended terrain mesh

### Recent implementation notes (Jul 2026)

| Area | Status |
|------|--------|
| Loading screen | Progress bar, loading music, Ready gate before deploy |
| Main menu / options / pause | Working; UI scales to any resolution |
| Display / fullscreen | Dynamic mode list; resolution Apply refreshes viewport instantly |
| Multiplayer lobby | Host/join/ready/start; LAN + Tailscale discovery |
| Multiplayer in-match | Ticket grace, teammate minimap dots, round auto-restart |
| Deploy UI | Faction, kit, spawn map |
| BF2 controls | Retail defaults, rebindable Options tab, crouch/prone, scoreboard |
| Map fidelity | Heightmap cluster, overgrowth, roads, texture resolve, asset audit |
| Ground vehicles | Drive, terrain tilt, animated wheels (where meshed) |
| Boats / RHIBs | Water-surface buoyancy |
| Helicopters | Collective/yaw/cyclic, rotor RPM blur, AI gunner, flares |
| Jets | Spool, V1 rotate, body-wing lift, afterburner, gear tuck (`G`) — alpha |
| Game audio | Weapons, voice, ambient; vehicle loops on enter |
| Conquest | Tickets, CP capture, voice cues — alpha |
| Crash fix | SDL2 + SDL2_mixer DLLs copied next to exe on build |

---

## Attribution

- Mesh / animation file-format reverse engineering originally by
  Rigo "Pixel" Reddig — [BattlefieldRespawn](https://github.com/rigred/BattlefieldRespawn).
- Everything in this repository is original code released under the MIT License
  (see `LICENSE`), except the vendored third-party libraries under
  `third_party/`, which retain their own permissive licenses.

Battlefield 2 and the Refractor engine are the property of EA / DICE. This is an
independent, non-commercial, clean-room engine project and ships **none** of
their content.
