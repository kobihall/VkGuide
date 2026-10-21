"""A glass sphere on a diffuse floor under a small bright sphere light: the light reaches the floor
through the glass only by paths a camera path finds by chance, so the floor's caustic is made of
fireflies. The scene for judging a firefly filter (docs/plans/completed/pbrt-film-and-reconstruction.md)."""
from vkscene import EMISSIVE, LAMBERTIAN, camera, make_scene, quad, sphere


def caustic_scene(width=96, height=96, **kwargs):
    shapes = [
        quad("floor", (0.0, -1.0, 0.0), (1, 0, 0), (0, 0, 1), 20.0, 20.0, LAMBERTIAN((0.7, 0.7, 0.7))),
        sphere("glass", (0.0, 0.0, 0.0), 1.0, {"type": "dielectric", "ir": 1.5}),
        sphere("lamp", (0.0, 3.0, 0.0), 0.12, EMISSIVE((1.0, 1.0, 1.0), 400.0)),
    ]
    cam = camera((0.0, 2.2, 5.5), (0.0, -0.6, 0.0), 40.0)
    return make_scene(shapes, cam, width, height, background=(0.02, 0.02, 0.02), ray_depth=12, samples_per_frame=8, **kwargs)
