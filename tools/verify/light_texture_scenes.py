"""Writes the verification scenes of docs/plans/completed/pbrt-light-types.md and docs/plans/completed/pbrt-textures.md, and the images they
read, into assets/ (committed, like the optics scenes):

  assets/textures/checker.png        a 512 x 512 black and white checker of 8 x 8 squares
  assets/textures/white.png          a white 64 x 32 image, a projection light's
  assets/ies/isotropic.ies           an IES LM-63 file whose every candela value is 3415 (5 W/sr)

  assets/scenes/lt_point_reference.gltf     a floor and a wall lit by a point light of 5 W/sr
  assets/scenes/lt_goniometric.gltf         the same lit by a goniometric light of isotropic.ies, which must match it
  assets/scenes/lt_projection.gltf          a wall lit by a white projection light with a 90 degree field of view
  assets/scenes/lt_projection_point.gltf    the same wall lit by a point light of the same intensity
  assets/scenes/lt_one_sided.gltf           a one-sided emissive quad seen from behind, over a floor on both its sides
  assets/scenes/lt_sphere_point.gltf        a 1000 lm point light inside a diffuse sphere of albedo 0.5
  assets/scenes/lt_sphere_spot.gltf         a 1000 lm spot light (smoothstep falloff) aimed away from the camera, the same
  assets/scenes/tx_checker_plane.gltf       a checkered plane receding to the horizon (PBR 4ed Figure 10.1)
  assets/scenes/tx_mirror_checker.gltf      a mirror sphere reflecting the checkered plane
  assets/scenes/tx_textured_emitter.gltf    a checker-textured emissive quad over a floor, lit by it alone

tools/verify/light_texture_check.py renders and checks them.

Run:  python3 tools/verify/light_texture_scenes.py
"""

import json
import math
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402

ROOT = vkscene.ROOT
SCENES = ROOT / "assets" / "scenes"
TEXTURES = ROOT / "assets" / "textures"
IES = ROOT / "assets" / "ies"

PI = math.pi
PHOTOMETRIC = 683.0


def write_png(path, width, height, rgb):
    """An 8-bit RGB PNG from a function of the texel (x, y) to (r, g, b) bytes."""
    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            row.extend(rgb(x, y))
        rows.append(bytes(row))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def write_assets():
    TEXTURES.mkdir(parents=True, exist_ok=True)
    IES.mkdir(parents=True, exist_ok=True)
    write_png(TEXTURES / "checker.png", 512, 512, lambda x, y: (255, 255, 255) if ((x // 64) + (y // 64)) % 2 == 0 else (0, 0, 0))
    write_png(TEXTURES / "white.png", 64, 32, lambda x, y: (255, 255, 255))
    candela = 5.0 * PHOTOMETRIC
    vertical = [0, 30, 60, 90, 120, 150, 180]
    (IES / "isotropic.ies").write_text(
        "IESNA:LM-63-2002\n[TEST] isotropic, the goniometric light's verification (tools/verify/light_texture_scenes.py)\n"
        "TILT=NONE\n"
        f"1 -1 1 {len(vertical)} 1 1 2 0 0 0\n1 1 100\n"
        + " ".join(str(v) for v in vertical) + "\n0\n"
        + " ".join(f"{candela:g}" for _ in vertical) + "\n")


# ---------------------------------------------------------------- the version 25 payload

def material(name, kind, **params):
    record = {"name": name, "type": kind}
    record.update(params)
    return record


def light(kind, name, position, intensity, color=(1.0, 1.0, 1.0), orientation=None, **extra):
    record = {"type": kind, "name": name, "position": list(position), "color": list(color), "intensity": intensity}
    if orientation is not None:
        record["orientation"] = orientation
    record.update(extra)
    return record


def looking(direction):
    """The orientation whose -z (where a light shines) is `direction`, with +y as near to up as it allows."""
    forward = vkscene.normalize(direction)
    helper = [0.0, 1.0, 0.0] if abs(forward[1]) < 0.9 else [1.0, 0.0, 0.0]
    right = vkscene.normalize(vkscene.np.cross(forward, helper))
    up = vkscene.np.cross(right, forward)
    return vkscene.quat_from_matrix(vkscene.np.column_stack([right, up, -forward]))


def scene(shapes, materials, lights, cam, width, height, samples=64, ray_depth=8, textures=None, render_extra=None, kernels=None):
    render = {"width": width, "height": height, "matchViewport": False, "antialiasing": True, "maxSamples": samples, "unlimitedSamples": False,
              "rayDepth": ray_depth, "useFixedSeed": True, "seed": 1, "samplesPerFrame": 16, "russianRoulette": False, "minBouncesBeforeRoulette": 4,
              "restartOnChange": False, "textureStorage": "native", "textureFilter": "trilinear", "maxTextureSize": 4096}
    render.update(render_extra or {})
    extras = {"version": 25, "models": [], "environmentIntensity": 1.0, "render": render, "renderCamera": 0, "cameras": [cam], "geometry": shapes,
              "meshObjects": [], "lights": lights, "materials": materials, "materialOverrides": [], "textures": textures or [],
              "backgroundMode": "solid", "background": [0.0, 0.0, 0.0]}
    if kernels is not None:
        extras["kernels"] = kernels
    return {"asset": {"generator": "vulkan_guide", "version": "2.0"}, "nodes": [{"name": "vulkan_guide_scene"}], "scene": 0,
            "scenes": [{"nodes": [0], "extras": extras, "name": "vulkan_guide scene"}], "extensions": {}}


def room(light_record):
    """A floor and a back wall in light grey, one light above."""
    materials = [material("grey", "lambertian", albedo=[0.6, 0.6, 0.6])]
    shapes = [vkscene.quad("floor", [0, 0, 0], [1, 0, 0], [0, 0, -1], 8, 8, 0), vkscene.quad("wall", [0, 2, -3], [1, 0, 0], [0, 1, 0], 8, 4, 0)]
    for shape in shapes:
        shape["material"] = 0
    return scene(shapes, materials, [light_record], vkscene.camera([0, 2.5, 4.5], [0, 0.8, -1], 45), 320, 240, samples=64, ray_depth=4)


def write_scenes():
    SCENES.mkdir(parents=True, exist_ok=True)
    def save(name, payload):
        vkscene.write_scene(SCENES / name, payload)

    # a goniometric light of an isotropic distribution and a point light of the same 5 W/sr
    save("lt_point_reference.gltf", room(light("point", "point", [0, 3, 0], 5.0)))
    save("lt_goniometric.gltf", room(light("goniometric", "goniometric", [0, 3, 0], 1.0, orientation=looking([0, -1, 0]), image="../ies/isotropic.ies")))

    # a white 2:1 projection with a 90 degree field of view (its shorter side) on a wall, against a point light. Direct
    # light only: the camera ray and the light sample
    wall = [material("white", "lambertian", albedo=[0.8, 0.8, 0.8])]
    wall_shapes = [vkscene.quad("wall", [0, 0, -2], [1, 0, 0], [0, 1, 0], 12, 8, 0)]
    wall_shapes[0]["material"] = 0
    cam = vkscene.camera([0, 0, 2.5], [0, 0, -2], 90)
    save("lt_projection.gltf", scene(wall_shapes, wall, [light("projection", "projector", [0, 0, 0], 4.0, orientation=looking([0, 0, -1]), image="../textures/white.png", fov=90.0)],
                                     cam, 320, 240, samples=16, ray_depth=2))
    save("lt_projection_point.gltf", scene(wall_shapes, wall, [light("point", "point", [0, 0, 0], 4.0)], cam, 320, 240, samples=16, ray_depth=2))

    # a one-sided emissive quad facing +z, seen from -z: from behind, it and the floor behind it are dark
    one_sided = [material("emitter", "emissive", albedo=[1.0, 1.0, 1.0], strength=5.0, twoSided=False), material("floor", "lambertian", albedo=[0.6, 0.6, 0.6])]
    panel = vkscene.quad("panel", [0, 1, 0], [1, 0, 0], [0, 1, 0], 1.5, 1.5, 0)
    panel["material"] = 0
    floor = vkscene.quad("floor", [0, 0, 0], [1, 0, 0], [0, 0, -1], 8, 8, 1)
    floor["material"] = 1
    # vkscene.quad's normal is v x u: for u = +x, v = +y that is -z. Reverse it, so it emits towards +z
    panel["reverseOrientation"] = True
    save("lt_one_sided.gltf", scene([panel, floor], one_sided, [], vkscene.camera([0, 1.5, -4], [0, 0.6, 0], 50), 320, 240, samples=64, ray_depth=4))

    # 1000 lm inside a diffuse sphere of radius 1 and albedo 0.5. A Lambertian sphere's inside spreads every reflected
    # watt evenly over itself, so a wall point the light does not reach directly shows rho^2 Phi / (pi A (1 - rho)); the
    # point light, at the center, lights the wall evenly, which then shows rho Phi / (pi A (1 - rho)) everywhere
    sphere_material = [material("integrating", "lambertian", albedo=[0.5, 0.5, 0.5])]
    shell = vkscene.sphere("shell", [0, 0, 0], 1.0, 0)
    shell["material"] = 0
    inside = vkscene.camera([0, 0, 0.0], [0, 0, -1], 60)
    point_intensity = 1000.0 / PHOTOMETRIC / (4 * PI)
    save("lt_sphere_point.gltf", scene([shell], sphere_material, [light("point", "lamp", [0, 0, 0], point_intensity, unit="lumen")],
                                       inside, 160, 120, samples=256, ray_depth=16))
    cos_outer = math.cos(math.radians(30.0))
    cos_inner = math.cos(math.radians(15.0))
    spot_solid_angle = 2 * PI * ((1 - cos_inner) + (cos_inner - cos_outer) * 0.5)
    spot_intensity = 1000.0 / PHOTOMETRIC / spot_solid_angle
    save("lt_sphere_spot.gltf", scene([shell], sphere_material, [light("spot", "spot", [0, 0, 0.3], spot_intensity, orientation=looking([0, 0, 1]), innerCone=15.0,
                                                                      outerCone=30.0, falloff="smoothstep", unit="lumen")], inside, 160, 120, samples=256, ray_depth=16))

    # a checkered plane to the horizon, the checker repeating every 2 m, lit by a sky-like solid background
    checker_texture = [{"path": "../textures/checker.png", "colorSpace": "srgb", "wrap": ["repeat", "repeat"], "nearest": [False, False], "encoding": "srgb", "gamma": 2.2, "invert": False}]
    plane_material = material("checker", "lambertian", albedo=[0.8, 0.8, 0.8],
                              textures={"albedo": {"texture": 0, "channel": "rgb", "mapping": "uv", "uvSet": 0, "offset": [0, 0], "scale": [0.5, 0.5], "rotation": 0}})
    plane = {"type": "plane", "name": "ground", "position": [0.0, 0.0, 0.0], "orientation": [0.0, 0.0, 0.0, 1.0], "material": 0}
    low = vkscene.camera([0, 1.0, 3], [0, 0.6, -10], 50)
    checker = scene([plane], [plane_material], [], low, 400, 200, samples=64, ray_depth=2, textures=checker_texture)
    checker["scenes"][0]["extras"]["background"] = [1.0, 1.0, 1.0]
    save("tx_checker_plane.gltf", checker)

    mirror = material("mirror", "metal", albedo=[0.95, 0.95, 0.95], fuzz=0.0)
    ball = vkscene.sphere("mirror", [0, 1.0, -1.5], 1.0, 1)
    ball["material"] = 1
    reflected = scene([plane, ball], [plane_material, mirror], [], vkscene.camera([0, 1.2, 3], [0, 1.0, -1.5], 30), 300, 300, samples=64, ray_depth=4, textures=checker_texture)
    reflected["scenes"][0]["extras"]["background"] = [1.0, 1.0, 1.0]
    save("tx_mirror_checker.gltf", reflected)

    # a checker-textured emitter over a floor, which next-event estimation must price and sample by its texels
    emitter = material("textured light", "emissive", albedo=[1.0, 1.0, 1.0], strength=4.0,
                       textures={"albedo": {"texture": 0, "channel": "rgb", "mapping": "uv", "uvSet": 0, "offset": [0, 0], "scale": [1, 1], "rotation": 0}})
    light_quad = vkscene.quad("light", [0, 1.5, 0], [1, 0, 0], [0, 0, 1], 1.5, 1.5, 0)
    light_quad["material"] = 0
    ground = vkscene.quad("floor", [0, 0, 0], [1, 0, 0], [0, 0, -1], 6, 6, 1)
    ground["material"] = 1
    save("tx_textured_emitter.gltf", scene([light_quad, ground], [emitter, material("floor", "lambertian", albedo=[0.7, 0.7, 0.7])], [],
                                           vkscene.camera([0, 2.5, 3.5], [0, 0.3, 0], 45), 320, 240, samples=256, ray_depth=4, textures=checker_texture))


def main():
    write_assets()
    write_scenes()
    print("wrote assets/textures/checker.png, white.png, assets/ies/isotropic.ies and the lt_*.gltf and tx_*.gltf scenes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
