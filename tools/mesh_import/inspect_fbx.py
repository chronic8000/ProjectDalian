import bpy

fbx = r"c:\Users\chron\Downloads\uploads_files_878396_Mim23_Rigged.fbx"
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
bpy.ops.import_scene.fbx(filepath=fbx)

print("=== OBJECTS ===")
for o in sorted(bpy.data.objects, key=lambda x: x.name):
    parent = o.parent.name if o.parent else "-"
    if o.type == "MESH":
        dims = tuple(round(x, 3) for x in o.dimensions)
        print(f"MESH     {o.name:40} parent={parent:30} verts={len(o.data.vertices):6d} dims={dims}")
    elif o.type == "ARMATURE":
        print(f"ARMATURE {o.name:40} parent={parent:30} bones={len(o.data.bones)}")
    else:
        print(f"{o.type:8} {o.name:40} parent={parent:30}")

print("=== BONES ===")
for o in bpy.data.objects:
    if o.type != "ARMATURE":
        continue
    for b in o.data.bones:
        bp = b.parent.name if b.parent else "-"
        print(f"  {b.name:40} parent={bp}")

print("=== MATERIALS ===")
for m in bpy.data.materials:
    print(" ", m.name)
