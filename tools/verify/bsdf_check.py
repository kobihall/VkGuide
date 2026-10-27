#!/usr/bin/env python3
"""The BSDF library's materials against what they must give (docs/plans/completed/pbrt-bsdf-library.md), in exact RGB:

  furnace      one sphere in the uniform white environment. A white diffuse, a thin pane, a diffuse transmission whose
               reflectance and transmittance sum to 1, and a mix of white Lambertian and a thin pane all vanish (so does a mix of two glasses); a 0.5
               diffuse is half, and a mix of Lambertian 0.4 and 0.8 at amount 0.5 is 0.6
  inside glass a camera inside a closed glass sphere in the white environment sees n^2 = 2.25: radiance mode scales
               what refracts by 1 / eta^2, and the equilibrium inside the glass is n^2 times the outside. Without the
               scaling the sphere would look white

Run:  python3 tools/verify/bsdf_check.py [--spp N]
"""

import argparse
import math
import sys

import numpy as np

from make_scenes import furnace_scene, write_uniform_hdr
from vkscene import ROOT, camera, make_scene, run_render, sphere

UNIFORM_MAP = ROOT / "assets" / "environment_maps" / "uniform_white.hdr"
SIZE = 64
FOV = 40.0
DISTANCE = 4.0


def disc_and_background():
    t = math.tan(math.radians(FOV) / 2)
    ys, xs = np.mgrid[0:SIZE, 0:SIZE]
    px = ((xs + 0.5) / SIZE * 2 - 1) * t
    py = (1 - (ys + 0.5) / SIZE * 2) * t
    angle = np.arctan(np.sqrt(px * px + py * py))
    radius = math.asin(1.0 / DISTANCE)
    pixel = 2 * t / SIZE
    return angle < radius - 1.5 * pixel, angle > radius + 1.5 * pixel


def luminance(image):
    return image @ np.array([0.2126, 0.7152, 0.0722])


def mix_scene(first, second, amount):
    """A sphere whose material is a mix of two library materials: a version 19 payload, which names materials by index."""
    scene = furnace_scene({"type": "lambertian", "albedo": [1, 1, 1]}, environment_map=str(UNIFORM_MAP))
    extras = scene["scenes"][0]["extras"]
    extras["version"] = 19
    extras["materials"] = [
        {"name": "mix", "type": "mix", "materialA": 1, "materialB": 2, "amount": amount},
        dict(first, name="first"),
        dict(second, name="second"),
    ]
    extras["geometry"][0]["material"] = 0
    return scene


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spp", type=int, default=256)
    args = parser.parse_args()
    if not UNIFORM_MAP.exists():
        write_uniform_hdr(UNIFORM_MAP)
    disc, background = disc_and_background()
    failures = 0

    def check(name, scene, expected, tolerance):
        nonlocal failures
        image, dropped = run_render(scene, args.spp, size=(SIZE, SIZE), seed=3)
        values = luminance(image)
        bg = values[background].mean()
        got = values[disc].mean() / bg
        error = values[disc].std(ddof=1) / math.sqrt(values[disc].size) / bg
        ok = abs(got - expected) <= tolerance + 4 * error
        failures += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: {got:.4f} +- {error:.4f}, expected {expected} (tolerance {tolerance})")

    print("furnace")
    for name, material, expected in [
        ("diffuse 1", {"type": "diffuse", "albedo": [1, 1, 1]}, 1.0),
        ("diffuse 0.5", {"type": "diffuse", "albedo": [0.5, 0.5, 0.5]}, 0.5),
        ("thin glass 1.5", {"type": "thin_glass", "eta": {"constant": 1.5}}, 1.0),
        ("thin glass 1.1", {"type": "thin_glass", "eta": {"constant": 1.1}}, 1.0),
        ("diffuse transmission 0.5 + 0.5", {"type": "diffuse_transmission", "albedo": [0.5, 0.5, 0.5], "transmittance": [0.5, 0.5, 0.5], "scale": 1.0}, 1.0),
        ("diffuse transmission scale 2", {"type": "diffuse_transmission", "albedo": [0.25, 0.25, 0.25], "transmittance": [0.25, 0.25, 0.25], "scale": 2.0}, 1.0),
    ]:
        check(name, furnace_scene(material, environment_map=str(UNIFORM_MAP)), expected, 0.002)
    white = {"type": "lambertian", "albedo": [1, 1, 1]}
    glass = {"type": "glass", "eta": {"constant": 1.5}, "uRoughness": 0.0, "vRoughness": 0.0}
    # a closed cavity of Lambertian and refracting glass traps light by total internal reflection for more bounces than
    # the renderer's depth limit of 16 (CRT_MAX_DEPTH) allows, so the mixes here are of surfaces that do not trap it
    thin = {"type": "thin_glass", "eta": {"constant": 1.5}}
    check("mix of white Lambertian and a thin pane, amount 0.7", mix_scene(white, thin, 0.7), 1.0, 0.002)
    check("mix of glass 1.5 and glass 1.5, amount 0.3", mix_scene(glass, glass, 0.3), 1.0, 0.002)
    check("mix of Lambertian 0.4 and 0.8, amount 0.5", mix_scene({"type": "lambertian", "albedo": [0.4] * 3}, {"type": "lambertian", "albedo": [0.8] * 3}, 0.5), 0.6, 0.01)
    check("mix of Lambertian 0.4 and 0.8, amount 0", mix_scene({"type": "lambertian", "albedo": [0.4] * 3}, {"type": "lambertian", "albedo": [0.8] * 3}, 0.0), 0.4, 0.002)
    check("mix of Lambertian 0.4 and 0.8, amount 1", mix_scene({"type": "lambertian", "albedo": [0.4] * 3}, {"type": "lambertian", "albedo": [0.8] * 3}, 1.0), 0.8, 0.002)

    print("inside glass")
    cam = camera((0.0, 0.0, 0.0), (0.0, 0.0, -1.0), FOV)
    scene = make_scene([sphere("ball", (0.0, 0.0, 0.0), 1.0, dict(glass))], cam, SIZE, SIZE, ray_depth=64, environment_map=str(UNIFORM_MAP))
    image, dropped = run_render(scene, args.spp, size=(SIZE, SIZE), seed=3)
    got = luminance(image).mean()
    ok = abs(got - 2.25) < 0.05
    failures += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} the view from inside a glass sphere: {got:.4f}, expected n^2 = 2.25")

    print("bsdf_check: " + ("all checks passed" if failures == 0 else f"{failures} check(s) FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
