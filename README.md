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

**Simulation / gameplay:**
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
- **6-DoF FPV drone** (`B` to toggle): true rigid-body quad with independent
  per-rotor thrust, gravity, and simulated battery voltage sag, plus a
  render-to-texture FPV feed with signal degradation

---

## Build

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

Third-party libraries used: SDL2, GLM, Dear ImGui, GLEW, miniz, stb. Small
vendored copies (GLEW / miniz / stb) live under `third_party/` and are
permissively licensed.

---

## Run

Point the app at a BF2 level archive plus your `Objects_client.zip`:

```powershell
$bf2 = "C:\Program Files (x86)\Battlefield2\mods\bf2"

.\build\apps\dalian\project_dalian.exe `
    "$bf2\Levels\Dalian_plant\server.zip" `
    "$bf2\Objects_client.zip"
```

### Controls

| Input | Action |
|---|---|
| `W` `A` `S` `D` | Move |
| `Shift` | Sprint |
| `Space` | Jump |
| Mouse | Look |
| `Tab` | Toggle mouse capture |
| `LMB` | Fire (hold for automatic) |
| `F` | Toggle ballistic ⇄ hitscan |
| `Q` / Mouse wheel | Cycle weapon |
| `V` | Toggle 1st / 3rd person |
| `B` | Toggle FPV drone mode |
| Drone: `W`/`S` throttle, `A`/`D` yaw, mouse pitch/roll | Fly the quad |
| `Esc` | Quit |

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
  physics/   physics world, character controller, 6-DoF drone
  script/    .con/.tweak interpreter + gameplay script sandbox
  net/       networking stubs
apps/
  dalian/    the war-sim game client
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
  6-DoF FPV drones, PBR + cascaded shadow maps, enemy AI ✔ (rough / alpha)
- **Next** — vehicle entry/driving, missiles/munitions, better animation
  retargeting, networking, audio, in-engine editor

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
