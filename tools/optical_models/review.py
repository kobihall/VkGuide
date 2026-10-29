"""Renders .glb models from a ring of directions into one contact sheet, for reviewing the converter.

Run with Blender in the background (Blender 2.91 or later, Workbench engine):

  blender -b --factory-startup --python tools/optical_models/review.py -- \
      --out sheet.png [--views 26|6|N] [--size 256] [--zoom Z] a.glb [b.glb ...]

Each cell of the sheet is one direction; within a cell the models are side by side, in the order given,
all framed by the first model's bounds so that their scales agree. The 26 directions are the face, edge and
corner directions of a cube. With --views 6 only the faces are rendered. Colors are the files' material
base colors and textures (the decals), flat-lit with a cavity outline so that edges read. --zoom magnifies
each view about the model's center, to read the decals.
"""

import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector


def directions(count):
    dirs = []
    for x in (-1, 0, 1):
        for y in (-1, 0, 1):
            for z in (-1, 0, 1):
                if (x, y, z) != (0, 0, 0):
                    dirs.append(Vector((x, y, z)).normalized())
    faces = [d for d in dirs if sum(abs(c) > 0.5 for c in d) == 1]
    # Order: the six faces first, then edges, then corners, each group in a fixed order.
    rest = sorted([d for d in dirs if d not in faces], key=lambda d: (sum(abs(c) > 0.1 for c in d), tuple(d)))
    faces = sorted(faces, key=lambda d: tuple(d))
    return (faces + rest)[:count]


def parse(argv):
    argv = argv[argv.index("--") + 1:] if "--" in argv else []
    opts = {"out": "sheet.png", "views": 26, "size": 256, "zoom": 1.0, "files": []}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--out", "--views", "--size", "--zoom"):
            opts[a[2:]] = argv[i + 1] if a == "--out" else (float(argv[i + 1]) if a == "--zoom" else int(argv[i + 1]))
            i += 2
        else:
            opts["files"].append(a)
            i += 1
    return opts


def import_model(path, offset):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    objs = [o for o in bpy.data.objects if o not in before]
    roots = [o for o in objs if o.parent is None]
    for r in roots:
        r.location = r.location + offset
    for m in bpy.data.materials:
        if m.use_nodes and "Principled BSDF" in m.node_tree.nodes:
            bc = m.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value
            m.diffuse_color = (bc[0], bc[1], bc[2], 1.0)
    return [o for o in objs if o.type == "MESH"]


def bounds(objs):
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            lo = Vector((min(lo[k], w[k]) for k in range(3)))
            hi = Vector((max(hi[k], w[k]) for k in range(3)))
    return lo, hi


def main():
    opts = parse(sys.argv)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    shading = scene.display.shading
    shading.light = "STUDIO"
    # the texture where a material has one, its color elsewhere
    shading.color_type = "TEXTURE"
    shading.show_cavity = True
    shading.cavity_type = "WORLD"
    shading.show_object_outline = False
    shading.show_backface_culling = False
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "Standard"
    world = bpy.data.worlds.new("w")
    scene.world = world
    world.color = (1.0, 1.0, 1.0)
    shading.background_type = "WORLD"

    models = []
    first_lo = first_hi = None
    spacing = 0.0
    for k, f in enumerate(opts["files"]):
        objs = import_model(f, Vector((0, 0, 0)))
        lo, hi = bounds(objs)
        if k == 0:
            first_lo, first_hi = lo, hi
            spacing = (hi - lo).length * 1.15
        models.append(objs)
    n = len(models)
    center = (first_lo + first_hi) * 0.5
    radius = (first_hi - first_lo).length * 0.5

    cam_data = bpy.data.cameras.new("cam")
    cam_data.type = "ORTHO"
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    size = opts["size"]
    scene.render.resolution_x = size * n
    scene.render.resolution_y = size
    scene.render.resolution_percentage = 100
    cam_data.ortho_scale = 2.1 * radius * n
    cam_data.clip_start = radius * 0.01
    cam_data.clip_end = radius * 20

    views = directions(opts["views"])
    cols = 7 if len(views) > 6 else 3
    rows = (len(views) + cols - 1) // cols
    sheet = np.ones((rows * size, cols * size * n, 4), dtype=np.float32)
    tmp = opts["out"] + ".cell.png"
    for vi, d in enumerate(views):
        # Blender's glTF import turns the model's +y up into +Z. The camera looks along -d with +Z up on
        # screen, or with -Y up when it looks straight down or up. Models sit side by side along its right.
        world_up = Vector((0, 0, 1)) if abs(d.z) < 0.9 else Vector((0, 1 if d.z > 0 else -1, 0))
        right = world_up.cross(d).normalized()
        up = d.cross(right).normalized()
        basis = Matrix((right, up, d)).transposed()
        for k, objs in enumerate(models):
            shift = right * (spacing * (k - (n - 1) * 0.5))
            for o in objs:
                if o.parent is None:
                    o.location = shift
                else:
                    root = o
                    while root.parent is not None:
                        root = root.parent
                    root.location = shift
        eye = center + d * radius * 4
        cam.location = eye
        cam.rotation_mode = "QUATERNION"
        cam.rotation_quaternion = basis.to_quaternion()
        cam_data.ortho_scale = (max(2.1 * radius, spacing) * n if n > 1 else 2.1 * radius) / opts["zoom"]
        scene.render.filepath = tmp
        bpy.ops.render.render(write_still=True)
        img = bpy.data.images.load(tmp)
        px = np.array(img.pixels[:], dtype=np.float32).reshape(size, size * n, 4)
        bpy.data.images.remove(img)
        r, c = vi // cols, vi % cols
        y0 = (rows - 1 - r) * size
        sheet[y0:y0 + size, c * size * n:(c + 1) * size * n, :] = px
        # A dark frame line between cells.
        sheet[y0:y0 + size, c * size * n, :3] = 0.3
        sheet[y0, c * size * n:(c + 1) * size * n, :3] = 0.3
    out = bpy.data.images.new("sheet", cols * size * n, rows * size, alpha=True)
    out.pixels = sheet.ravel()
    out.filepath_raw = opts["out"]
    out.file_format = "PNG"
    out.save()
    os.remove(tmp)
    print("wrote", opts["out"])


main()
