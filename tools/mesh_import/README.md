# Custom mesh import (no Blender UI required)

Project Dalian can load **Wavefront OBJ** for custom missiles (and later vehicles).

## Convert an FBX from CGTrader (one command)

Portable Blender is under `tools/blender-portable/` (not committed — download once).

```powershell
cd C:\Projects\bf2respawn

# If blender-portable is missing, download Blender 4.2 zip and extract there.

$blender = ".\tools\blender-portable\blender.exe"
$in  = "$env:USERPROFILE\Downloads\your_model.fbx"
$out = ".\apps\dalian\content\missiles\storm_shadow\storm_shadow.obj"

& $blender --background --python .\tools\mesh_import\convert_fbx_to_obj.py -- $in $out
```

Then rebuild / run. Console should print:

```
Missile: custom OBJ ...\storm_shadow.obj
```

Override path anytime:

```powershell
$env:BF2_MISSILE_OBJ = "D:\models\my_rocket.obj"
```

## What you do NOT need

- Learning Blender’s UI
- BF2 `.bundledmesh` for this missile
- Hand-editing the mesh (the script centers, scales to length 1, aligns nose to **+Z**)

## Notes

- That Storm Shadow FBX was CAD/STL-derived (~90k verts, weak UVs) — fine for a visible rocket; a true game LO mesh would be lighter.
- Prefer **FBX** or **OBJ** downloads from CGTrader; avoid STL/3MF for textured game use.
