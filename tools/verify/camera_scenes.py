"""Writes the verification scenes of docs/plans/completed/pbrt-cameras.md, and the image one of them reads, into assets/
(committed, like the light and texture scenes):

  assets/textures/aperture_star.png    a 128 x 128 five-pointed star, white on black: an aperture image

  assets/scenes/tests/cameras/cam_ortho_box.gltf     an open box with red and green side walls, seen straight in by an orthographic
                                       camera: the side walls, parallel to its rays, show edge-on
  assets/scenes/tests/cameras/cam_dof_spheres.gltf   three spheres at three depths over a checkered floor, a wide aperture focused on
                                       the middle one, the concentric lens mapping
  assets/scenes/tests/cameras/cam_bokeh.gltf         one small emitter far behind the plane of focus on black, a six-bladed aperture:
                                       its out-of-focus image is the aperture's shape
  assets/scenes/tests/cameras/cam_tilt_ground.gltf   a checkered ground seen from 1 m up, looking 20 degrees down through a wide
                                       aperture, the plane of focus tilted 70 degrees onto the ground
  assets/scenes/tests/cameras/cam_equirect_room.gltf a closed room of six colors and a ceiling light, seen by an equirectangular
                                       spherical camera at the point P
  assets/scenes/tests/cameras/cam_mirror_room.gltf   the same room with a small mirror sphere at P, seen up close by a perspective camera
  assets/scenes/tests/cameras/cam_mirror_env.gltf    the mirror sphere alone, lit by cam_room_equirect.exr (beside it) as the environment
                                       map: tools/verify/camera_check.py renders that file from cam_equirect_room.gltf

tools/verify/camera_check.py renders and checks them.

Run:  python3 tools/verify/camera_scenes.py
"""

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402
from light_texture_scenes import write_png  # noqa: E402

ROOT = vkscene.ROOT
SCENES = vkscene.TEST_SCENES / "cameras"
TEXTURES = ROOT / "assets" / "textures"

# the point the spherical camera sees from and the mirror sphere stands at
ROOM_POINT = [0.3, 1.2, 0.2]
MIRROR_RADIUS = 0.04


def material(name, kind, **params):
    record = {"name": name, "type": kind}
    record.update(params)
    return record


def placed(shape, material_index):
    shape["material"] = material_index
    return shape


def scene(shapes, materials, cam, width, height, samples=64, ray_depth=8, textures=None, background="solid", environment_map=None):
    render = {"width": width, "height": height, "matchViewport": False, "antialiasing": True, "maxSamples": samples, "unlimitedSamples": False,
              "rayDepth": ray_depth, "useFixedSeed": True, "seed": 1, "samplesPerFrame": 16, "russianRoulette": False, "minBouncesBeforeRoulette": 4,
              "restartOnChange": False, "textureStorage": "native", "textureFilter": "trilinear", "maxTextureSize": 4096}
    extras = {"version": 26, "models": [], "environmentIntensity": 1.0, "render": render, "renderCamera": 0, "cameras": [cam], "geometry": shapes,
              "meshObjects": [], "lights": [], "materials": materials, "materialOverrides": [], "textures": textures or [],
              "backgroundMode": background, "background": [0.0, 0.0, 0.0]}
    if environment_map is not None:
        extras["environmentMap"] = environment_map
    return {"asset": {"generator": "vulkan_guide", "version": "2.0"}, "nodes": [{"name": "vulkan_guide_scene"}], "scene": 0,
            "scenes": [{"nodes": [0], "extras": extras, "name": "vulkan_guide scene"}], "extensions": {}}


def write_assets():
    TEXTURES.mkdir(parents=True, exist_ok=True)
    # a star: inside the polygon whose radius alternates between the outer and the inner one, five points up
    def star(x, y):
        px = (x + 0.5) / 64.0 - 1.0
        py = 1.0 - (y + 0.5) / 64.0
        angle = math.atan2(py, px) - math.pi / 2
        sector = (angle % (2 * math.pi / 5)) / (2 * math.pi / 5)
        # the radius of the star's edge at this angle, between a point (0.95) and a notch (0.4)
        edge = 0.95 + (0.4 - 0.95) * (1 - abs(2 * sector - 1))
        inside = math.hypot(px, py) <= edge
        return (255, 255, 255) if inside else (0, 0, 0)
    write_png(TEXTURES / "aperture_star.png", 128, 128, star)


def checker_texture():
    return [{"path": "../../../textures/checker.png", "colorSpace": "srgb", "wrap": ["repeat", "repeat"], "nearest": [False, False], "encoding": "srgb", "gamma": 2.2, "invert": False}]


def checker_material(scale):
    return material("checker", "lambertian", albedo=[0.8, 0.8, 0.8],
                    textures={"albedo": {"texture": 0, "channel": "rgb", "mapping": "uv", "uvSet": 0, "offset": [0, 0], "scale": [scale, scale], "rotation": 0}})


def room_shapes(materials):
    """A closed 4 x 3 x 4 m room around the origin's floor: each wall its own color, all facing in, a light in the ceiling."""
    base = len(materials)
    colors = {"floor": [0.7, 0.7, 0.7], "ceiling": [0.9, 0.9, 0.9], "west": [0.75, 0.15, 0.1], "east": [0.12, 0.6, 0.15],
              "north": [0.15, 0.25, 0.75], "south": [0.8, 0.7, 0.2]}
    for name, albedo in colors.items():
        materials.append(material(name, "lambertian", albedo=albedo))
    materials.append(material("light", "emissive", albedo=[1.0, 0.95, 0.85], strength=8.0))
    m = {name: base + i for i, name in enumerate(colors)}
    light = base + len(colors)
    # vkscene.quad's normal is v x u
    return [
        placed(vkscene.quad("floor", [0, 0, 0], [1, 0, 0], [0, 0, -1], 4, 4, 0), m["floor"]),
        placed(vkscene.quad("ceiling", [0, 3, 0], [1, 0, 0], [0, 0, 1], 4, 4, 0), m["ceiling"]),
        placed(vkscene.quad("west", [-2, 1.5, 0], [0, 0, -1], [0, 1, 0], 4, 3, 0), m["west"]),
        placed(vkscene.quad("east", [2, 1.5, 0], [0, 0, 1], [0, 1, 0], 4, 3, 0), m["east"]),
        placed(vkscene.quad("north", [0, 1.5, -2], [-1, 0, 0], [0, 1, 0], 4, 3, 0), m["north"]),
        placed(vkscene.quad("south", [0, 1.5, 2], [1, 0, 0], [0, 1, 0], 4, 3, 0), m["south"]),
        placed(vkscene.quad("light", [0.5, 2.98, -0.4], [1, 0, 0], [0, 0, 1], 1.0, 0.8, 0), light),
    ]


def write_scenes():
    SCENES.mkdir(parents=True, exist_ok=True)

    def save(name, payload):
        vkscene.write_scene(SCENES / name, payload)

    # an open box 2 m on a side: white back, red left, green right, grey floor and ceiling, a light under the ceiling
    box_materials = [material("white", "lambertian", albedo=[0.75, 0.75, 0.75]), material("red", "lambertian", albedo=[0.7, 0.08, 0.06]),
                     material("green", "lambertian", albedo=[0.1, 0.6, 0.12]), material("light", "emissive", albedo=[1.0, 1.0, 1.0], strength=6.0)]
    box = [placed(vkscene.quad("back", [0, 0, -1], [1, 0, 0], [0, 1, 0], 2, 2, 0), 0),
           placed(vkscene.quad("floor", [0, -1, 0], [1, 0, 0], [0, 0, -1], 2, 2, 0), 0),
           placed(vkscene.quad("ceiling", [0, 1, 0], [1, 0, 0], [0, 0, 1], 2, 2, 0), 0),
           placed(vkscene.quad("left", [-1, 0, 0], [0, 0, -1], [0, 1, 0], 2, 2, 0), 1),
           placed(vkscene.quad("right", [1, 0, 0], [0, 0, 1], [0, 1, 0], 2, 2, 0), 2),
           placed(vkscene.quad("lamp", [0, 0.99, 0], [1, 0, 0], [0, 0, 1], 0.6, 0.6, 0), 3)]
    save("cam_ortho_box.gltf", scene(box, box_materials, vkscene.camera([0, 0, 4], [0, 0, 0], 40, kind="orthographic", viewHeight=3.0,
                                                                         lensMapping="concentric"), 320, 240, samples=64, ray_depth=6))

    # three spheres at 2, 4 and 7 m over a checkered floor, focused on the middle one
    dof_materials = [checker_material(0.5), material("orange", "lambertian", albedo=[0.8, 0.4, 0.1]), material("blue", "lambertian", albedo=[0.15, 0.3, 0.8]),
                     material("white", "lambertian", albedo=[0.8, 0.8, 0.8])]
    plane = {"type": "plane", "name": "ground", "position": [0.0, 0.0, 0.0], "orientation": [0.0, 0.0, 0.0, 1.0], "material": 0}
    spheres = [plane, placed(vkscene.sphere("near", [-0.6, 0.35, 1.0], 0.35, 1), 1), placed(vkscene.sphere("middle", [0.2, 0.35, -1.0], 0.35, 2), 2),
               placed(vkscene.sphere("far", [1.0, 0.35, -4.0], 0.35, 3), 3)]
    dof = scene(spheres, dof_materials, vkscene.camera([0, 0.6, 3], [0.2, 0.35, -1.0], 40, aperture=0.3, focusDistance=4.0, lensMapping="concentric"),
                320, 200, samples=64, ray_depth=4, textures=checker_texture())
    dof["scenes"][0]["extras"]["background"] = [0.9, 0.95, 1.0]
    save("cam_dof_spheres.gltf", dof)

    # a 5 cm emitter 6 m away, the lens focused at 1 m: its blur is the aperture's shape, about 24 degrees across
    bokeh_materials = [material("light", "emissive", albedo=[1.0, 1.0, 1.0], strength=10.0)]
    bokeh = scene([placed(vkscene.sphere("point", [0, 0, -6], 0.05, 0), 0)], bokeh_materials,
                  vkscene.camera([0, 0, 0], [0, 0, -1], 40, aperture=0.5, focusDistance=1.0, lensMapping="concentric", apertureShape="polygon",
                                 apertureBlades=6, apertureRotation=0.0, apertureImage="../../../textures/aperture_star.png"),
                  256, 256, samples=256, ray_depth=1)
    save("cam_bokeh.gltf", bokeh)

    # a camera 1 m above a checkered ground, looking 20 degrees down: its axis meets the ground 1 / sin(20) m away, and a
    # tilt of 70 degrees about its x axis lays the plane of focus onto the ground
    down = math.radians(20.0)
    target = [0.0, 0.0, -1.0 / math.tan(down)]
    tilt_materials = [checker_material(0.5)]
    tilt = scene([dict(plane, material=0)], tilt_materials,
                 vkscene.camera([0, 1, 0], target, 50, aperture=0.25, focusDistance=1.0 / math.sin(down), lensMapping="concentric", tilt=[70.0, 0.0]),
                 320, 240, samples=256, ray_depth=2, textures=checker_texture())
    tilt["scenes"][0]["extras"]["background"] = [1.0, 1.0, 1.0]
    save("cam_tilt_ground.gltf", tilt)

    # the room seen from P in every direction, laid out as the environment map is
    room_materials = []
    room = room_shapes(room_materials)
    save("cam_equirect_room.gltf", scene(room, room_materials, vkscene.camera(ROOM_POINT, [ROOM_POINT[0], ROOM_POINT[1], ROOM_POINT[2] - 1], 90,
                                                                               kind="spherical", mapping="equirect"), 1024, 512, samples=512, ray_depth=6))
    # a mirror sphere at P, seen from half a meter away by a camera whose view it nearly fills
    mirror_materials = list(room_materials)
    mirror_materials.append(material("mirror", "metal", albedo=[1.0, 1.0, 1.0], fuzz=0.0))
    viewer = [ROOM_POINT[0], ROOM_POINT[1] + 0.1, ROOM_POINT[2] + 0.5]
    distance = math.dist(viewer, ROOM_POINT)
    fov = 2 * math.degrees(math.atan(1.15 * MIRROR_RADIUS / distance))
    close_up = vkscene.camera(viewer, ROOM_POINT, fov, lensMapping="concentric")
    mirror = placed(vkscene.sphere("mirror", ROOM_POINT, MIRROR_RADIUS, len(mirror_materials) - 1), len(mirror_materials) - 1)
    save("cam_mirror_room.gltf", scene(room + [mirror], mirror_materials, close_up, 200, 200, samples=1024, ray_depth=7))
    env_mirror = dict(mirror, material=0)
    save("cam_mirror_env.gltf", scene([env_mirror], [material("mirror", "metal", albedo=[1.0, 1.0, 1.0], fuzz=0.0)], close_up, 200, 200, samples=64,
                                      ray_depth=2, background="environment", environment_map="cam_room_equirect.exr"))


def main():
    write_assets()
    write_scenes()
    print("wrote assets/textures/aperture_star.png and the cam_*.gltf scenes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
