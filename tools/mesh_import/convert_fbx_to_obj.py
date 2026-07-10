# Convert an FBX/OBJ to a game-ready OBJ for Project Dalian (headless Blender).
# Usage:
#   blender --background --python convert_fbx_to_obj.py -- in.fbx out.obj
#
# Applies: join meshes, origin to geometry, scale to ~unit length along longest axis,
# optional rotate so longest axis = +Z (missile nose convention).

import bpy
import sys
from mathutils import Vector
from pathlib import Path


def argv_after_double_dash():
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1 :]
    return []


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for block in bpy.data.meshes:
        bpy.data.meshes.remove(block)


def import_model(path: Path):
    ext = path.suffix.lower()
    if ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=str(path))
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=str(path))
    else:
        raise SystemExit(f"unsupported input: {ext}")


def main():
    args = argv_after_double_dash()
    if len(args) < 2:
        raise SystemExit("need: -- <input.fbx|obj> <output.obj>")
    src = Path(args[0])
    dst = Path(args[1])
    dst.parent.mkdir(parents=True, exist_ok=True)

    clear_scene()
    import_model(src)

    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        raise SystemExit("no mesh objects in file")

    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()

    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    # Center at origin
    bpy.ops.object.origin_set(type="ORIGIN_GEOMETRY", center="BOUNDS")
    obj.location = (0, 0, 0)

    # Scale so longest axis ≈ 1.0 (game applies its own draw scale)
    dims = obj.dimensions
    longest = max(dims.x, dims.y, dims.z)
    if longest > 1e-6:
        s = 1.0 / longest
        obj.scale = (s, s, s)
        bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

    # Rotate so longest axis aligns with +Z (missile forward)
    dims = Vector(obj.dimensions)
    axis = "X"
    if dims.y >= dims.x and dims.y >= dims.z:
        axis = "Y"
    elif dims.z >= dims.x and dims.z >= dims.y:
        axis = "Z"
    if axis == "X":
        obj.rotation_euler[1] = 1.5707963  # +90° Y
    elif axis == "Y":
        obj.rotation_euler[0] = -1.5707963  # -90° X
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)

    # Recenter after rotate
    bpy.ops.object.origin_set(type="ORIGIN_GEOMETRY", center="BOUNDS")
    obj.location = (0, 0, 0)
    bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)

    # Flat normals help untextured CAD-ish meshes
    bpy.ops.object.shade_smooth()

    bpy.ops.wm.obj_export(
        filepath=str(dst),
        export_selected_objects=True,
        export_materials=True,
        export_uv=True,
        export_normals=True,
        forward_axis="Z",
        up_axis="Y",
    )
    print(f"Wrote {dst}  verts={len(obj.data.vertices)}  dims={tuple(obj.dimensions)}")


if __name__ == "__main__":
    main()
