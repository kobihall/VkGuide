"""The Cornell box of assets/scenes/cornell_box.gltf, as data: one description that the scene files,
the radiosity solve and the pbrt scene are all made from, so they cannot drift apart.

Units are the scene's. The box is open at z = 0 (the camera looks in from z = -80), has no front
wall, and its light is a quad 0.1 below the ceiling that emits on both sides (as every emitter of
this renderer does) and absorbs what lands on it.
"""

import numpy as np

from vkscene import LAMBERTIAN, EMISSIVE, box, camera, make_scene, quad

SIZE = 55.5
WHITE = (0.73, 0.73, 0.73)
RED = (0.65, 0.05, 0.05)
GREEN = (0.12, 0.45, 0.15)
LIGHT_RADIANCE = 15.0
CAMERA_POSITION = (27.8, 27.8, -80.0)
CAMERA_TARGET = (27.8, 27.8, 0.0)
FOV = 40.0

# name, center, u axis, v axis, width along u, length along v, reflectance
H = SIZE / 2
WALLS = [
    ("green_wall", (SIZE, H, H), (0, 1, 0), (0, 0, 1), SIZE, SIZE, GREEN),
    ("red_wall", (0.0, H, H), (0, 1, 0), (0, 0, 1), SIZE, SIZE, RED),
    ("floor", (H, 0.0, H), (1, 0, 0), (0, 0, 1), SIZE, SIZE, WHITE),
    ("ceiling", (H, SIZE, H), (1, 0, 0), (0, 0, 1), SIZE, SIZE, WHITE),
    ("back_wall", (H, H, SIZE), (1, 0, 0), (0, 1, 0), SIZE, SIZE, WHITE),
]
LIGHT = ("light", (27.8, 55.4, 27.95), (1, 0, 0), (0, 0, 1), 10.5, 13.0)

# the two boxes: center, size, yaw in degrees
TALL_BOX = ((36.604145, 16.5, 35.33363), (16.5, 33.0, 16.5), 15.0)
SHORT_BOX = ((18.296825, 8.25, 16.895607), (16.5, 16.5, 16.5), -18.0)


def corners(center, u_axis, v_axis, width, length):
    """The four corners of a quad, in order around it."""
    c = np.asarray(center, dtype=np.float64)
    u = np.asarray(u_axis, dtype=np.float64) * width / 2
    v = np.asarray(v_axis, dtype=np.float64) * length / 2
    return np.array([c - u - v, c + u - v, c + u + v, c - u + v])


def shapes(with_boxes):
    result = []
    for name, center, u, v, width, length, reflectance in WALLS:
        result.append(quad(name, center, u, v, width, length, LAMBERTIAN(reflectance)))
    name, center, u, v, width, length = LIGHT
    result.append(quad(name, center, u, v, width, length, EMISSIVE((1.0, 1.0, 1.0), LIGHT_RADIANCE)))
    if with_boxes:
        result.append(box("tall_box", *TALL_BOX[:2], TALL_BOX[2], LAMBERTIAN(WHITE)))
        result.append(box("short_box", *SHORT_BOX[:2], SHORT_BOX[2], LAMBERTIAN(WHITE)))
    return result


def scene(with_boxes, width=400, height=400, samples=256, **kwargs):
    cam = camera(CAMERA_POSITION, CAMERA_TARGET, FOV)
    return make_scene(shapes(with_boxes), cam, width, height, background=(0.0, 0.0, 0.0), samples=samples, ray_depth=16, **kwargs)
