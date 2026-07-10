import bpy

fbx = r"c:\Users\chron\Downloads\uploads_files_878396_Mim23_Rigged.fbx"
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
bpy.ops.import_scene.fbx(filepath=fbx)

mesh_obj = next(o for o in bpy.data.objects if o.type == "MESH")
me = mesh_obj.data
print("vertex_groups:", [g.name for g in mesh_obj.vertex_groups])
print("material_slots:", len(me.materials))
for i, mat in enumerate(me.materials):
    print(f"  slot {i}: {mat.name if mat else None}")

from collections import Counter
c = Counter(p.material_index for p in me.polygons)
for i, n in sorted(c.items()):
    name = me.materials[i].name if me.materials[i] else "?"
    print(f"  faces mat[{i}] {name}: {n}")
