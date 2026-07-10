# Export MIM-23 Hawk as game OBJs.
# - mim23_launcher.obj : full static mesh (MVP draw)
# - Also reports bone names for later turret aim.
import bpy
from pathlib import Path

fbx = r"c:\Users\chron\Downloads\uploads_files_878396_Mim23_Rigged.fbx"
out_dir = Path(r"C:\Projects\bf2respawn\apps\dalian\content\emplacements\mim23_hawk")
out_dir.mkdir(parents=True, exist_ok=True)

bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
bpy.ops.import_scene.fbx(filepath=fbx)

# Apply armature modifier as shape for a static drawable (MVP).
mesh_obj = next(o for o in bpy.data.objects if o.type == "MESH")
bpy.context.view_layer.objects.active = mesh_obj
mesh_obj.select_set(True)

# Make single-user and apply modifiers
bpy.ops.object.convert(target="MESH")

# Center on XY, keep Y up; scale so longest horizontal ≈ 6 m (trailer footprint)
bpy.ops.object.origin_set(type="ORIGIN_GEOMETRY", center="BOUNDS")
mesh_obj.location = (0, 0, 0)
dims = mesh_obj.dimensions
# Blender Y-up after FBX import; our game is Y-up too after convert script convention.
# Scale so max(X,Z) ~ 6 meters for a Hawk trailer.
span = max(dims.x, dims.y, dims.z)
target = 6.0
if span > 1e-4:
    s = target / span
    mesh_obj.scale = (s, s, s)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

# Rotate so trailer sits on XZ ground (FBX often has Z-up). After import, check dims.
# Prefer putting the short axis as height (Y).
dims = list(mesh_obj.dimensions)
# If Y is not the middle dimension, rotate to make height the smallest-ish vertical.
# Hawk: width~4.4 length~5.6 height~3 — after earlier inspect dims=(4.4, 5.6, 3.0) in Blender.
# That was X,Y,Z with Y longest — so Y was length. Rotate -90 X so length goes to -Z, height to Y.
mesh_obj.rotation_euler[0] = -1.5707963
bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
bpy.ops.object.origin_set(type="ORIGIN_GEOMETRY", center="BOUNDS")
mesh_obj.location = (0, 0, 0)
# Drop so lowest point is y=0
# (origin at center — offset up by half height after final dims)
dims = mesh_obj.dimensions
mesh_obj.location.y = dims.y * 0.5
bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)

out = out_dir / "mim23_launcher.obj"
bpy.ops.object.select_all(action="DESELECT")
mesh_obj.select_set(True)
bpy.context.view_layer.objects.active = mesh_obj
bpy.ops.wm.obj_export(
    filepath=str(out),
    export_selected_objects=True,
    export_materials=True,
    export_uv=True,
    export_normals=True,
    forward_axis="Z",
    up_axis="Y",
)
print("Wrote", out, "dims", tuple(mesh_obj.dimensions), "verts", len(mesh_obj.data.vertices))
