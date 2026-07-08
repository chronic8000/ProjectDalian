# Reverse-engineered Refractor 2 format notes

This folder supersedes the upstream `File_Headers.md` from
[BattlefieldRespawn](https://github.com/rigred/BattlefieldRespawn). All formats
below were verified byte-for-byte against real retail Battlefield 2 assets
extracted from `mods/bf2/Objects_client.zip` and level archives.

## Implemented & verified loaders

| Format | Status | Verified against |
| --- | --- | --- |
| `.staticmesh` / `.bundledmesh` / `.skinnedmesh` | full geometry extract | flag (154v), barrel, F/A-18 (23k v) |
| `.dds` | dims + format (DXT1/3/5, RGBA8) | `ch_ghille_b.dds` 512x512 DXT1 |
| `.ske` | full (bones, hierarchy, quat+pos) | `1p_setup.ske` 70 bones |
| `.baf` | full (RLE 16-bit keyframe decode) | `1p_at_mine_idle1.baf` 48 bones / 91 frames |
| `.collisionmesh` | full (cols, faces, verts, BSP skip) | `flagpole.collisionmesh` 5 cols |
| heightmap `.raw` | 16-bit little-endian | Dalian Plant 1025x1025 |
| `.con` / `.tweak` | templates + level placements | DummyObjects.con, StaticObjects.con (907 objects) |

## Mesh vertex indices (important)

Triangle indices in `.staticmesh` / `.bundledmesh` / `.skinnedmesh` are stored
**relative to each material's `vertex_start`**. The true global vertex index is
`material.vertex_start + stored_index`. Ignoring this produces sheared geometry
on any mesh whose second (and later) material has a non-zero `vertex_start`
(e.g. destroyable props with intact/wreck geoms).

## `.ske` skeleton

```
dword version            (2)
dword node_count
repeat node_count:
    word  name_length     (includes trailing null)
    char  name[name_length]
    int16 parent          (-1 / 0xFFFF = root)
    float rotation[4]     (quaternion x,y,z,w)
    float position[3]
```

## `.baf` animation

```
dword version            (4)
word  bone_count
word  bone_id[bone_count]
dword frame_count
byte  precision
repeat bone_count:
    word data_size
    repeat 7 channels (rot x,y,z,w then pos x,y,z):
        word data_left
        while data_left > 0:
            byte head            (bit7 = RLE, bits0-6 = frame count)
            byte next_header
            if RLE: one word value applied to all frames
            else:   one word value per frame
            data_left -= next_header
```

16-bit fixed point decode: `mult = 32767 / (1 << (15 - precision))`, treat words
> 32767 as negative (`word - 0xFFFF`), then `value / mult`. Rotations use
precision 15; positions use the file's `precision` byte. Rotation channels are
negated on read.

## `.collisionmesh`

```
dword version_major      (0)
dword version_minor      (9 or 10)
dword geom_part_count
repeat geom_part_count:
    dword geom_count
    repeat geom_count:
        dword col_count
        repeat col_count:
            dword col_type          (0 proj / 1 vehicle / 2 soldier / 3 AI)
            dword face_count
            face[face_count]        (word v1,v2,v3, word material)
            dword vert_count
            vec3[vert_count]
            word vert_material[vert_count]
            vec3 bounds_min, vec3 bounds_max
            byte bsp_marker          (ASCII '0'/'1')
            if '1': BSP tree (min,max, nodes, face refs)
            if version_minor >= 10: dword adj_count; int32 adj[adj_count]
```

## Terrain heightmaps

`HeightmapPrimary.raw` is a `(size x size)` grid of unsigned 16-bit
little-endian samples. World height = `sample * setScale.y`; horizontal cell
spacing = `setScale.x` (== `setScale.z`). Parameters live in `Heightdata.con`
(`heightmap.setSize`, `heightmap.setScale`, `heightmap.setBitResolution`).

## `.con` / `.tweak` scripting

- `ObjectTemplate.create <type> <name>` opens a template; subsequent
  `ObjectTemplate.*` lines set properties or add child templates.
- `.tweak` files use `ObjectTemplate.activeSafe <type> <name>` to re-open an
  existing template and override tuning values.
- Level `StaticObjects.con` uses `Object.create <template>` followed by
  `Object.absolutePosition x/y/z`, `Object.rotation yaw/pitch/roll`,
  `Object.layer n` for world placement.
- `rem` / `beginRem`..`endRem` are comments.

## Memory model (AI tuning)

Soldier target memory in vanilla BF2 is controlled by `aiTemplate.degeneration`
in `soldiers/Common/ai/Objects.ai` (server-side). At 30 Hz server tick rate,
`15` ticks ~= 0.5 seconds and `45` ticks ~= 1.5 seconds.

## Archive mounting

BF2 mods mount zip archives into namespaces such as `Objects`. Later mounts
override earlier ones, matching `ServerArchives.con` behavior. Decompression
uses miniz (real DEFLATE), so compressed retail archives load directly.
