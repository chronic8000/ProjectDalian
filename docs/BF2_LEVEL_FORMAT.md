# Battlefield 2 Level Format (Refractor 2)

This document describes how a BF2 map packages terrain, static buildings, foliage, and
gameplay spawns. All scripts are plain text `.con` files inside `server.zip` (server data)
and `client.zip` (visual assets).

## Archive layout

| Archive | Contents |
|---|---|
| `Levels/<Map>/server.zip` | Heightmaps, `StaticObjects.con`, `Terrain.con`, `Overgrowth/`, gameplay cons |
| `Levels/<Map>/client.zip` | `Colormaps/`, `Detailmaps/`, `Lightmaps/`, object lightmaps |
| `Objects_server.zip` | Object `.con` / `.tweak` definitions (geometry names, physics) |
| `Objects_client.zip` | `.staticmesh`, textures, skinned meshes |
| `Common_client.zip` | Shared terrain detail textures (`Terrain/Textures/Detail/*.dds`) |

## StaticObjects.con — buildings and props

Two sections in one file:

### 1. Template registration (`run` lines)

Each unique mesh template is registered by running its object definition:

```
run /objects/staticobjects/industry/ind_buildings/lrgfactorybuilding/lrgfactorybuilding.con
```

The path is normalized to `staticobjects/.../*.con` inside `Objects_server.zip`.
That `.con` declares:

```
ObjectTemplate.create SimpleObject lrgfactorybuilding
ObjectTemplate.geometry lrgfactorybuilding
```

The geometry name maps to `meshes/<name>.staticmesh` in `Objects_client.zip`.

### 2. Placements (`Object.*` blocks)

Thousands of instances follow the run list:

```
rem *** lrgfactorybuilding ***
Object.create lrgfactorybuilding
Object.absolutePosition -59.987/176.238/-269.162
Object.rotation -66.6/0.0/0.0
Object.layer 1
```

| Command | Meaning |
|---|---|
| `Object.create <name>` | Begin a new instance using template `<name>` |
| `Object.absolutePosition x/y/z` | World position (meters, Y up) |
| `Object.rotation yaw/pitch/roll` | Euler rotation in degrees |
| `Object.layer <n>` | Render / physics layer |

Coordinate system matches terrain: X/Z horizontal, Y vertical. Placements share the same
origin as the centered heightfield mesh.

## Terrain.con — heightfield + texture layers

```
terrain.create Terrain
terrain.load Levels/Dalian_plant/terraindata.raw
terrain.primaryWorldScale 2/0.00640869/2
terrain.patchColormapSize 512
terrain.colormapBaseName "Levels/Dalian_plant/Colormaps/tx"
terrain.lightmapBaseName "Levels/Dalian_plant/Lightmaps/tx"
terrain.detailmapBaseName "Levels/Dalian_plant/Detailmaps/tx"
```

Client tiles live at `Colormaps/tx<COL>x<ROW>.dds` inside `client.zip`.
Detail blend masks use `Detailmaps/tx<COL>x<ROW>_1.dds` and `_2.dds`.
Tiling detail textures (dirt/grass/rock) come from `Common_client.zip`.

## Overgrowth/ — trees and bushes

`Overgrowth/Overgrowth.con` defines material types and geometry references:

```
Overgrowth.addMaterial bjorkcluster 3
Overgrowth.setActiveMaterial bjorkcluster
Overgrowth.addType bjork1
Overgrowth.setActiveType bjork1
OvergrowthType.geometry nc_birch_cluster01
OvergrowthType.density 5
```

Instance positions are stored in the binary `Overgrowth/Overgrowth.raw` (not plain text).
Geometry meshes resolve through `Objects_client.zip` like static objects.

## GamePlayObjects.con — vehicles and control points

Per game mode (`GameModes/gpm_cq/64/GamePlayObjects.con`):

```
Object.create CPNAME_US_...
Object.absolutePosition x/y/z
ObjectSpawner.create ...
ObjectSpawner.setObjectTemplate ...
```

These spawn vehicles and flag positions; they are separate from `StaticObjects.con`.

## Engine load order (Project Dalian)

1. Mount `server.zip`, `client.zip`, `Objects_client.zip`, `Objects_server.zip`, `Common_client.zip`
2. Parse `Heightdata.con` + `HeightmapPrimary.raw` → terrain heightfield
3. Parse `StaticObjects.con` → placements; follow `run` lines → mesh paths
4. Parse `Terrain.con` → stitch `client.zip` colormap / detail tiles
5. Parse `Overgrowth.con` + `Overgrowth.raw` → foliage instances (WIP)
6. Parse `GamePlayObjects.con` → vehicles, control points, player spawns

If buildings are missing, check console for `Resolved N templates; M placements` and
`Uploaded X unique meshes, Y instances`. Zero templates means `Objects_server.zip` is not
mounted; zero instances means mesh resolution failed.
