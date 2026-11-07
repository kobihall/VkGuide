"""Scene files for the optical elements (docs/plans/completed/physoptics-optical-elements.md): payload version 23, with the
Optical bench capability on and the elements in "optics", unmounted where the scene needs them bare and in mid-air, on
the breadboard in their mounts in optics_bench (docs/plans/completed/physoptics-bench-and-mounts.md). tools/verify/optics_check.py
renders the first five and checks what each must show; assets/scenes/tests/optics/optics_*.gltf are these, written by

    python3 tools/verify/optics_scenes.py

Each scene has an answer:

  optics_furnace      a fused silica lens (no absorption) in a white furnace: every pixel 1, whatever the lens does
  optics_nd_filter    an ND-OD1 filter with ideal AR faces in front of a white background: 0.1 through it, 1 around it
  optics_cube_split   an ideal 50:50 cube between a red emitter behind it and a green one beside it: (0.5, 0.5, 0)
  optics_lens_focus   a plano-convex lens imaging a bright square 1 m away onto a screen at the image distance
  optics_prism        a white slit seen through an F2 prism at minimum deviation: a spectrum, in the spectral modes
  optics_bench        one of every kind in its default mount on posts on a breadboard, for the eye and the save round trip
"""

import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402

SCENES = vkscene.TEST_SCENES / "optics"


def look(position, target, fov=30.0):
    return vkscene.camera(position, target, fov)


def quat_from_z(z_axis, up=(0.0, 1.0, 0.0)):
    """The orientation whose body +z is `z_axis`, with +y as near `up` as it can be: (x, y, z, w)."""
    z = vkscene.normalize(z_axis)
    x = np.cross(up, z)
    if np.linalg.norm(x) < 1e-9:
        x = np.cross([1.0, 0.0, 0.0], z)
    x = vkscene.normalize(x)
    y = np.cross(z, x)
    return vkscene.quat_from_matrix(np.column_stack([x, y, z]))


def optic(kind, name, position, z_axis=(0.0, 0.0, 1.0), size=None, params=None, materials=None, up=(0.0, 1.0, 0.0)):
    """An element record. A size preset decides the values it sets (a lens's diameter and center thickness): to set those,
    pass size="custom" and give them in `params`"""
    mount = {"kind": "none", "position": [float(c) for c in position], "orientation": [float(c) for c in quat_from_z(z_axis, up)]}
    params = dict(params or {})
    if kind == "spherical_lens":
        # a lens's position is its origin's (scene file version 31): the front vertex is the body frame's origin
        params.setdefault("originAt", "front_vertex")
    record = {"type": kind, "name": name, "params": params, "mount": mount, "materials": materials or {}}
    if size is not None:
        record["size"] = size
    return record


def mounted(kind, name, mount, origin, yaw, height=0.1, size=None, params=None):
    """An element standing on the bench in a mount of `mount`'s kind: its origin (a mirror's front face, the center of
    anything else) at (x, z), turned by `yaw` about it. The post stands where that puts it, and the app seats the fork
    in a hole when it opens the file"""
    record = {"type": kind, "name": name, "params": params or {}, "materials": {},
              "mount": {"kind": mount, "origin": [float(origin[0]), float(origin[1])], "yaw": float(yaw), "height": float(height),
                        "fork": {"angle": math.pi, "reach": 0.05}}}
    if size is not None:
        record["size"] = size
    return record


def interface(coating="uncoated", finish=0):
    return {"type": "optical_interface", "coating": coating, "finish": finish}


def scene(shapes, materials, optics, cam, width, height, *, background=None, samples=64, ray_depth=16, render_extra=None, bench=None):
    render = {"width": width, "height": height, "matchViewport": False, "antialiasing": True, "maxSamples": samples,
              "unlimitedSamples": False, "rayDepth": ray_depth, "useFixedSeed": True, "seed": 1, "samplesPerFrame": 16,
              "russianRoulette": False, "minBouncesBeforeRoulette": 4, "restartOnChange": True}
    render.update(render_extra or {})
    extras = {"version": 23, "models": [], "capabilities": ["optical_bench"], "environmentIntensity": 1.0, "render": render,
              "renderCamera": 0, "cameras": [cam], "lights": [], "materials": materials, "materialOverrides": [],
              "geometry": shapes, "meshObjects": [], "optics": optics, "lengthUnit": "mm"}
    if background is not None:
        extras["background"] = list(background)
    if bench is not None:
        extras["bench"] = bench
    # the Viewport where the render camera is, with a bench's 1 mm near plane: editing an optic puts the view right up
    # against it
    extras["viewport"] = {"position": cam["position"], "orientation": cam["orientation"], "pivot": [0.0, 0.0, 0.0],
                          "speed": 0.5, "fov": float(cam["fov"]), "near": 0.001, "far": 100.0}
    return {"asset": {"generator": "vulkan_guide", "version": "2.0"}, "nodes": [{"name": "vulkan_guide_scene"}],
            "scene": 0, "scenes": [{"nodes": [0], "extras": extras, "name": "vulkan_guide scene"}], "extensions": {}}


def lambertian(name, albedo):
    return {"name": name, "type": "lambertian", "albedo": list(albedo)}


def emissive(name, color, strength):
    return {"name": name, "type": "emissive", "albedo": list(color), "strength": strength}


def furnace():
    # the camera at -z looks along +z through a bi-convex fused silica lens: a lossless interface conserves radiance
    lens = optic("spherical_lens", "L1", (0.0, 0.0, 0.0), size="custom",
                 params={"diameter": 0.0254, "form": "bi_convex", "r1": 0.03, "r2": 0.03, "centerThickness": 0.008, "glass": "F_SILICA"})
    cam = look((0.0, 0.0, -0.08), (0.0, 0.0, 0.0), fov=24.0)
    return scene([], [], [lens], cam, 128, 128, background=(1.0, 1.0, 1.0), samples=64)


def nd_filter():
    nd = optic("circular_filter", "ND1", (0.0, 0.0, 0.0), size="1in", params={"thickness": 0.002, "glass": "ND-OD1"},
               materials={"front": interface("ideal_ar"), "back": interface("ideal_ar")})
    cam = look((0.0, 0.0, -0.06), (0.0, 0.0, 0.0), fov=30.0)
    return scene([], [], [nd], cam, 128, 128, background=(1.0, 1.0, 1.0), samples=32)


def cube_split():
    # red behind the cube (transmitted, along +z), green on its -y side (reflected by the diagonal into +z... seen from the
    # camera, the cube's Reflected port is the camera's view turned to -y)
    cube = optic("cube_beamsplitter", "BS1", (0.0, 0.0, 0.0), size="25.4mm", materials={"faces": interface("ideal_ar"), "diagonal": interface("ideal_50_50")})
    red = vkscene.quad("red", (0.0, 0.0, 0.2), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 0.4, 0.4, 0)
    green = vkscene.quad("green", (0.0, -0.2, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), 0.4, 0.4, 1)
    materials = [emissive("red_light", (1.0, 0.0, 0.0), 1.0), emissive("green_light", (0.0, 1.0, 0.0), 1.0)]
    cam = look((0.0, 0.0, -0.1), (0.0, 0.0, 0.0), fov=8.0)
    return scene([red, green], materials, [cube], cam, 96, 96, background=(0.0, 0.0, 0.0), samples=64)


def lens_focus():
    # a plano-convex lens images a bright square 1 m in front of it onto a screen at the image distance: the image is a
    # bright square on the screen, as large as the magnification makes it. A diffuse screen reaches the source only
    # through the lens, which a path tracer finds by chance, so the source is large and the image well lit
    r1 = 0.0515
    t = 0.0036
    n = 1.5168
    efl = r1 / (n - 1.0)
    bfl = efl - t / n
    distance = 1.0
    image = 1.0 / (1.0 / efl - 1.0 / distance)
    screen_z = t + bfl + (image - efl)
    lens = optic("spherical_lens", "L1", (0.0, 0.1, 0.0), size="1in", params={"form": "plano_convex", "r1": r1},
                 materials={"front": interface("thorlabs_A"), "back": interface("thorlabs_A")})
    source = vkscene.quad("source", (0.0, 0.1, -distance), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 0.15, 0.15, 0)
    screen = vkscene.quad("screen", (0.0, 0.1, screen_z), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 0.06, 0.06, 1)
    # a black baffle in the lens's plane with a 24 mm hole the lens fills, so the screen is lit through the lens alone
    hole = 0.012
    frame = 0.3
    baffle = []
    for i, (cx, cy, w, h) in enumerate([(0.0, hole + (frame - hole) / 2, 2 * frame, frame - hole), (0.0, -hole - (frame - hole) / 2, 2 * frame, frame - hole),
                                        (hole + (frame - hole) / 2, 0.0, frame - hole, 2 * hole), (-hole - (frame - hole) / 2, 0.0, frame - hole, 2 * hole)]):
        baffle.append(vkscene.quad(f"baffle_{i}", (cx, 0.1 + cy, -0.002), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), w, h, 2))
    materials = [emissive("source", (1.0, 1.0, 1.0), 200.0), lambertian("screen", (0.8, 0.8, 0.8)), lambertian("baffle", (0.0, 0.0, 0.0))]
    cam = look((0.03, 0.1, screen_z - 0.08), (0.0, 0.1, screen_z), fov=40.0)
    return scene([source, screen] + baffle, materials, [lens], cam, 192, 192, background=(0.0, 0.0, 0.0), samples=512)


def prism():
    # the camera looks back through an F2 prism at a white slit, along the prism's minimum-deviation path: each wavelength
    # deviates its own amount, so the slit is seen spread into a spectrum across the image's vertical
    n = 1.62
    tilt = math.asin(0.5 * n) - math.radians(30.0)
    center = np.array([0.0, 0.1, 0.0])
    into = np.array([0.0, math.sin(tilt), math.cos(tilt)])
    out = np.array([0.0, -math.sin(tilt), math.cos(tilt)])
    p = optic("equilateral_prism", "P1", tuple(center), size="25mm")
    slit_center = center - 0.4 * into
    slit = vkscene.quad("slit", tuple(slit_center), (1.0, 0.0, 0.0), tuple(np.cross(into, [1.0, 0.0, 0.0])), 0.02, 0.0015, 0)
    materials = [emissive("white_slit", (1.0, 1.0, 1.0), 20.0)]
    cam = look(tuple(center + 0.25 * out), tuple(center), fov=8.0)
    return scene([slit], materials, [p], cam, 160, 160, background=(0.0, 0.0, 0.0), samples=256,
                 render_extra={"spectral": {"representation": "hero", "lanes": 4}})


def bench():
    # one of every kind in its default mount, their origins in a row on holes 50 mm apart on a 700 x 300 mm breadboard,
    # each turned a little to show itself, lit by a soft overhead panel
    kinds = [("planar_mirror", "M1", "kinematic", {}), ("concave_mirror", "CM1", "kinematic", {}), ("off_axis_parabola", "OAP1", "kinematic", {}),
             ("circular_filter", "F1", "fixed", {"glass": "ND-OD1"}), ("square_filter", "SF1", "plate_holder", {}), ("iris", "ID1", "iris", {"opening": 0.008}),
             ("plate_beamsplitter", "BSP1", "rotation", {}), ("cube_beamsplitter", "BS1", "prism", {}), ("spherical_lens", "L1", "fixed", {}),
             ("right_angle_prism", "RAP1", "prism", {}), ("equilateral_prism", "EP1", "prism", {}), ("plate_wedge", "PW1", "plate_holder", {}),
             ("round_wedge", "RW1", "rotation", {})]
    optics = []
    # each mount's front toward the camera, turned 17 degrees
    yaw = math.atan2(0.3, -1.0)
    for i, (kind, name, mount, params) in enumerate(kinds):
        optics.append(mounted(kind, name, mount, (-0.3 + 0.05 * i, 0.0), yaw, params=params))
    panel = vkscene.quad("panel", (0.0, 0.6, 0.1), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), 0.8, 0.4, 0)
    back = vkscene.quad("backdrop", (0.0, 0.2, 0.2), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 1.2, 0.4, 1)
    materials = [emissive("panel", (1.0, 1.0, 1.0), 6.0), lambertian("backdrop", (0.6, 0.62, 0.65))]
    cam = look((0.0, 0.22, -0.55), (0.0, 0.07, 0.0), fov=45.0)
    return scene([panel, back], materials, optics, cam, 640, 300, background=(0.05, 0.05, 0.06), samples=128,
                 bench={"table": True, "size": [0.7, 0.3], "beamHeight": 0.1, "snap": True})


SCENE_BUILDERS = {
    "optics_furnace": furnace,
    "optics_nd_filter": nd_filter,
    "optics_cube_split": cube_split,
    "optics_lens_focus": lens_focus,
    "optics_prism": prism,
    "optics_bench": bench,
}


def main():
    for name, build in SCENE_BUILDERS.items():
        path = SCENES / f"{name}.gltf"
        vkscene.write_scene(path, build())
        print(f"wrote {path.relative_to(vkscene.ROOT)}")


if __name__ == "__main__":
    main()
