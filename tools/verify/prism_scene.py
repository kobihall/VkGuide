"""The dispersion scene (docs/plans/completed/pbrt-spectral-rendering.md): a 60 degree prism of pbrt's dense flint,
glass-F11, seen against a thin white slit, the way you look at a lamp through a prism. The camera looks through the
prism at minimum deviation for the design wavelength, 587.6 nm, and the slit stands where that ray leaves it. A
spectral mode sends each wavelength its own way, so the slit's image spreads into a spectrum across the image: red
deviated least, blue most. Exact RGB refracts every channel at the design wavelength's index and shows one white slit.

The camera finds the slit through two specular refractions, which a path tracer does without light sampling, so the
image converges in a few samples. A spotlight under a prism, the classic picture, makes its spectrum as a caustic
on the floor, which a path tracer cannot find from a delta light.

The prism is a glTF model, assets/models/prism.gltf, which prism_model() writes.
"""

import base64
import json
import math
import struct

import numpy as np

from vkscene import camera, make_scene, quad

APEX_DEGREES = 60.0
SIDE = 1.0
HEIGHT = 1.2
DESIGN_INDEX = 1.784702  # glass-F11 at 587.6 nm (pbrt's Sellmeier coefficients; spectrum_test checks n_d)
CAMERA_DISTANCE = 4.0
SLIT_DISTANCE = 3.0
SLIT_WIDTH = 0.01

MATERIAL = {"type": "glass", "eta": {"named": "glass-F11", "scale": 1.0}, "uRoughness": 0.0, "vRoughness": 0.0}
SLIT = {"type": "emissive", "color": [1.0, 1.0, 1.0], "strength": 4.0}


def _rotate(v, angle):
    c, s = math.cos(angle), math.sin(angle)
    return np.array([c * v[0] - s * v[1], s * v[0] + c * v[1]])


def geometry():
    """The prism's cross-section in world (x, z), the point where the camera's central ray leaves it, and that ray's
    direction. In the prism's own frame the base lies along u at v = 0 and the apex points up +v; a ray at minimum
    deviation runs parallel to the base inside, so it enters rising at half the deviation plus half the apex angle
    above the entry face's inward normal, and leaves falling as much. The frame is turned so that ray arrives along
    the camera's -z, and moved so it enters at the middle of the entry face, at the origin; it leaves at the middle
    of the other face."""
    apex = math.radians(APEX_DEGREES)
    deviation = 2 * math.asin(DESIGN_INDEX * math.sin(apex / 2)) - apex
    incidence = (apex + deviation) / 2
    inward_normal = -apex / 2                           # the left face's inward normal, below +u
    arrive = inward_normal + incidence                  # the incoming ray's angle in the prism's frame
    turn = -math.pi / 2 - arrive                        # (x, z) angle of -z is -90 degrees
    half = SIDE / 2
    tip = half / math.tan(apex / 2)                     # the apex's height above the base
    local = [np.array([-half, 0.0]), np.array([half, 0.0]), np.array([0.0, tip])]
    entry = (local[0] + local[2]) / 2
    corners = [_rotate(p - entry, turn) for p in local]
    exit_point = _rotate((local[1] + local[2]) / 2 - entry, turn)
    leave_angle = arrive - deviation                    # deviated toward the base
    leave = _rotate(np.array([math.cos(leave_angle), math.sin(leave_angle)]), turn)
    return corners, exit_point, leave, math.degrees(deviation)


def prism_model():
    """The prism as a glTF 2.0 file: three side faces and two caps, flat-shaded, in one embedded buffer."""
    corners, _, _, _ = geometry()
    y0, y1 = -HEIGHT / 2, HEIGHT / 2
    p = [np.array([c[0], y, c[1]]) for y in (y0, y1) for c in corners]   # 0-2 bottom, 3-5 top
    centroid = np.mean(p, axis=0)
    faces = [(0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5), (0, 2, 1), (3, 4, 5)]
    positions, normals, indices = [], [], []
    for face in faces:
        a, b, c = (p[i] for i in face[:3])
        n = np.cross(b - a, c - a)
        n /= np.linalg.norm(n)
        ordered = list(face)
        if np.dot(n, a - centroid) < 0:
            ordered.reverse()
            n = -n
        base = len(positions)
        for i in ordered:
            positions.append(p[i])
            normals.append(n)
        for k in range(1, len(ordered) - 1):
            indices += [base, base + k, base + k + 1]
    pos = np.asarray(positions, dtype=np.float32)
    nor = np.asarray(normals, dtype=np.float32)
    idx = np.asarray(indices, dtype=np.uint16)
    blob = pos.tobytes() + nor.tobytes() + idx.tobytes()
    blob += b"\0" * (-len(blob) % 4)
    return {
        "asset": {"version": "2.0", "generator": "tools/verify/prism_scene.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "prism", "mesh": 0}],
        "meshes": [{"name": "prism", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
        "materials": [{"name": "glass", "pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.95, 1.0, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.0}}],
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": pos.nbytes, "target": 34962},
            {"buffer": 0, "byteOffset": pos.nbytes, "byteLength": nor.nbytes, "target": 34962},
            {"buffer": 0, "byteOffset": pos.nbytes + nor.nbytes, "byteLength": idx.nbytes, "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": pos.min(axis=0).tolist(), "max": pos.max(axis=0).tolist()},
            {"bufferView": 1, "componentType": 5126, "count": len(nor), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
        ],
    }


def write_prism_model(path):
    path.write_text(json.dumps(prism_model(), indent=1))


def prism_scene(model_path, width=320, height=160, fov=24.0):
    """The scene, naming the model by `model_path` as the file will hold it (relative to the scene file)."""
    _, exit_point, leave, _ = geometry()
    exit_dir = np.array([leave[0], 0.0, leave[1]])
    slit_center = np.array([exit_point[0], 0.0, exit_point[1]]) + SLIT_DISTANCE * exit_dir
    across = np.cross(exit_dir, [0.0, 1.0, 0.0])
    cam = camera((0.0, 0.0, CAMERA_DISTANCE), (0.0, 0.0, 0.0), fov)
    # the slit faces the prism: its quad's width runs across the beam, its length up the prism's axis
    slit = quad("slit", slit_center, across, [0.0, 1.0, 0.0], SLIT_WIDTH, HEIGHT * 2, SLIT)
    scene = make_scene([slit], cam, width, height, background=(0.0, 0.0, 0.0), samples=64, ray_depth=8)
    extras = scene["scenes"][0]["extras"]
    extras["models"] = [{"path": str(model_path)}]
    extras["meshObjects"] = [{"name": "prism", "model": 0, "node": 0, "transform": [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0],
                              "visible": True, "shading": "override", "material": MATERIAL}]
    return scene
