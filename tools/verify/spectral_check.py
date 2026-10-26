#!/usr/bin/env python3
"""The spectral modes against what they must give (docs/plans/completed/pbrt-spectral-rendering.md):

  furnace       one sphere in the uniform white environment, rendered in every representation and lane count. The
                sphere must look like the background seen around it times its albedo: a white Lambertian, a mirror,
                the classic dielectric and pbrt's glass vanish, and a 0.5 Lambertian is half. A DISPERSIVE glass
                sphere (pbrt's dense flint, glass-F11) must vanish too: its refraction keeps one wavelength of the
                path and weighs it by the lanes it drops, and any bias there shows as a tint or a change in
                brightness. A rough one matches exact RGB, and the colored metals (copper, an RGB index) the 32-lane
                hero render. Every representation's background must be neutral
  convergence   the Cornell box in every representation against exact RGB, channel by channel: the spectral modes
                differ from RGB by the uplift's round trip (under 1 percent), the bands by their resolution, the
                RGB Gaussians by their three bands
  monochrome    the furnace in the monochromatic mode at three wavelengths, the infrared one beyond the RGB data
  dispersion    assets/scenes/prism_dispersion.gltf, a dense flint prism against a white slit: exact RGB shows one
                white slit, and hero and the bands spread it with red deviated least and blue most, the bands as
                one slit per band, the two agreeing on each channel's light

Run:  python3 tools/verify/spectral_check.py [--spp N] [--quick]
"""

import argparse
import math
import sys
from pathlib import Path

import numpy as np

from make_scenes import furnace_scene, write_uniform_hdr
from vkscene import ROOT, run_render

UNIFORM_MAP = ROOT / "assets" / "environment_maps" / "uniform_white.hdr"
SIZE = 64
FOV = 40.0
DISTANCE = 4.0

MATERIALS = [
    ("lambertian 1", {"type": "lambertian", "albedo": [1.0, 1.0, 1.0]}, 1.0),
    ("lambertian 0.5", {"type": "lambertian", "albedo": [0.5, 0.5, 0.5]}, 0.5),
    ("metal mirror", {"type": "metal", "albedo": [1.0, 1.0, 1.0], "fuzz": 0.0}, 1.0),
    ("dielectric 1.5", {"type": "dielectric", "ir": 1.5}, 1.0),
    ("glass 1.5", {"type": "glass", "eta": {"constant": 1.5}, "uRoughness": 0.0, "vRoughness": 0.0}, 1.0),
    ("glass F11 (dispersive)", {"type": "glass", "eta": {"named": "glass-F11", "scale": 1.0}, "uRoughness": 0.0, "vRoughness": 0.0}, 1.0),
    # a rough dielectric loses the energy single scattering loses, in exact RGB as much as in the spectral modes, so it
    # is measured against exact RGB's render
    ("rough glass F11", {"type": "glass", "eta": {"named": "glass-F11", "scale": 1.0}, "uRoughness": 0.1, "vRoughness": 0.1}, "exact RGB"),
    # colored metals, channel by channel against the 32-lane hero render: pbrt's measured copper, and an index given as
    # RGB, which exact RGB reads per channel and the spectral modes uplift as an unbounded spectrum. Exact RGB evaluates
    # the Fresnel equations at three wavelengths and lands within EXACT_RGB_COLOR of the spectral result
    ("copper", {"type": "conductor", "eta": {"named": "metal-Cu-eta", "scale": 1.0}, "k": {"named": "metal-Cu-k", "scale": 1.0}}, "hero 32"),
    ("conductor RGB eta, k", {"type": "conductor", "eta": [0.2, 0.9, 1.1], "k": [3.9, 2.4, 2.2]}, "hero 32"),
]

REPRESENTATIONS = [
    ("exact RGB", []),
    ("RGB Gaussians", ["--spectral", "gaussians"]),
    ("bands 8 box", ["--spectral", "bands", "--lanes", "8"]),
    ("bands 16 Gaussian", ["--spectral", "bands", "--lanes", "16"]),
    ("hero 4 visible", ["--spectral", "hero"]),
    ("hero 8 uniform", ["--spectral", "hero", "--lanes", "8", "--sampling", "uniform"]),
    ("hero 32", ["--spectral", "hero", "--lanes", "32"]),
    ("bands 32", ["--spectral", "bands", "--lanes", "32"]),
]

# how far each representation's furnace may be from the background, beyond the noise: the bands approximate the
# white illuminant piecewise and the Gaussians with three bands, so a sphere that reflects everything is exact but a
# 0.5 Lambertian's band projection of the uplift is not quite half
SLACK = {"exact RGB": 1e-6, "RGB Gaussians": 0.01, "bands 8 box": 0.01, "bands 16 Gaussian": 0.01, "bands 32": 0.01}
# how far a colored material's brightness and channels may be from its reference render's: the bands' and the
# Gaussians' resolution, and exact RGB's three samples of each spectrum (at 630, 532 and 465 nm)
COLOR = {"RGB Gaussians": 0.05, "bands 8 box": 0.04, "bands 16 Gaussian": 0.02, "bands 32": 0.01}
EXACT_RGB_COLOR = 0.08
# how far from neutral a representation's white may be: the background's channels, the white environment through
# the representation and the film. The bands' and the Gaussians' reconstruction of the white illuminant is coarse
WHITE = {"RGB Gaussians": 0.05, "bands 8 box": 0.03, "bands 16 Gaussian": 0.02, "bands 32": 0.01}


def masks():
    t = math.tan(math.radians(FOV) / 2)
    ys, xs = np.mgrid[0:SIZE, 0:SIZE]
    px = ((xs + 0.5) / SIZE * 2 - 1) * t
    py = (1 - (ys + 0.5) / SIZE * 2) * t
    angle = np.arctan(np.sqrt(px * px + py * py))
    radius = math.asin(1.0 / DISTANCE)
    pixel = 2 * t / SIZE
    disc = angle < radius - 1.5 * pixel
    background = angle > radius + 1.5 * pixel
    return disc, background


def mean_and_error(values):
    return float(values.mean()), float(values.std(ddof=1) / math.sqrt(len(values)))


def furnace(args, failures):
    print("furnace")
    if not UNIFORM_MAP.exists():
        write_uniform_hdr(UNIFORM_MAP)
    disc, background = masks()
    for name, material, expect in MATERIALS:
        scene = furnace_scene(material, environment_map=str(UNIFORM_MAP))
        results = {}
        for rep, extra in REPRESENTATIONS:
            image, dropped = run_render(scene, args.spp, size=(SIZE, SIZE), seed=3, extra_args=extra)
            luminance = image @ np.array([0.2126, 0.7152, 0.0722])
            bg, _ = mean_and_error(luminance[background])
            m, e = mean_and_error(luminance[disc])
            # the sphere's channels against the background's own, and the background's white itself
            white = image[background].mean(axis=0)
            results[rep] = (m / bg, e / bg, image[disc].mean(axis=0) / np.maximum(white, 1e-9) / (m / bg), white, dropped)
        # a number is the sphere's albedo, gray; a representation's name is the render it must match, color included
        reference = results.get(expect) if isinstance(expect, str) else None
        for rep, _ in REPRESENTATIONS:
            ratio, error, channels, white, dropped = results[rep]
            if reference is None:
                target, targetError, targetChannels, slack, colorSlack = expect, 0.0, 1.0, SLACK.get(rep, 0.0), 0.0
            else:
                target, targetError, targetChannels = reference[0], reference[1], reference[2]
                colorSlack = (EXACT_RGB_COLOR if rep == "exact RGB" and expect != "exact RGB" else COLOR.get(rep, 0.0))
                slack = SLACK.get(rep, 0.0) + colorSlack * target
            ok = abs(ratio - target) <= 5 * (error + targetError) + slack + 1e-4
            tint = float(np.max(np.abs(channels - targetChannels)))
            whiteTint = float(np.max(np.abs(white / white.mean() - 1)))
            tint_ok = tint <= 6 * (error + targetError) + 0.005 + colorSlack and whiteTint <= WHITE.get(rep, 0.005)
            status = "ok  " if ok and tint_ok and dropped == 0 else "FAIL"
            print(f"  {status} {name:<24} {rep:<18} sphere/background {ratio:.4f} +- {error:.4f} (expect {target:.4f}), tint {tint:.4f}, white {whiteTint:.4f}, dropped {dropped}")
            if status == "FAIL":
                failures.append(f"furnace {name} in {rep}: {ratio:.4f} against {target:.4f}, tint {tint:.4f}, white {whiteTint:.4f}, dropped {dropped}")


def convergence(args, failures):
    print("convergence on the Cornell box")
    scene = ROOT / "assets" / "scenes" / "cornell_box.gltf"
    reference, _ = run_render(scene, args.spp * 4, size=(128, 128), seed=5)
    ref = reference.reshape(-1, 3).mean(axis=0)
    limits = {"RGB Gaussians": 0.06, "bands 8 box": 0.04, "bands 16 Gaussian": 0.02, "bands 32": 0.02, "hero 4 visible": 0.01, "hero 8 uniform": 0.01, "hero 32": 0.01}
    for rep, extra in REPRESENTATIONS[1:]:
        image, dropped = run_render(scene, args.spp * 4, size=(128, 128), seed=6, extra_args=extra)
        means = image.reshape(-1, 3).mean(axis=0)
        difference = float(np.max(np.abs(means / ref - 1)))
        ok = difference <= limits[rep] and dropped == 0
        print(f"  {'ok  ' if ok else 'FAIL'} {rep:<18} channel means {np.round(means, 4)} against RGB {np.round(ref, 4)}: {difference * 100:.2f} % (limit {limits[rep] * 100:.0f} %)")
        if not ok:
            failures.append(f"convergence {rep}: {difference * 100:.2f} % from exact RGB")


def monochrome(args, failures):
    print("monochromatic furnace")
    disc, background = masks()
    for wavelength in (450.0, 632.8, 1064.0):
        for name, material, albedo in (MATERIALS[0], MATERIALS[1], MATERIALS[5]):  # all gray
            scene = furnace_scene(material, environment_map=str(UNIFORM_MAP))
            image, dropped = run_render(scene, args.spp, size=(SIZE, SIZE), seed=3, extra_args=["--spectral", "monochromatic", "--wavelength", str(wavelength)])
            gray = image[..., 0]
            neutral = float(np.max(np.abs(image[..., 1] - gray)) + np.max(np.abs(image[..., 2] - gray)))
            bg, _ = mean_and_error(gray[background])
            m, e = mean_and_error(gray[disc])
            ok = abs(m / bg - albedo) <= 5 * e / bg + 1e-4 and neutral == 0.0 and dropped == 0
            print(f"  {'ok  ' if ok else 'FAIL'} {wavelength:7.1f} nm {name:<24} background {bg:.4f}, sphere/background {m / bg:.4f} +- {e / bg:.4f} (expect {albedo})")
            if not ok:
                failures.append(f"monochromatic {wavelength} nm {name}: {m / bg:.4f}")


def dispersion(args, failures):
    print("dispersion through a prism")
    scene = ROOT / "assets" / "scenes" / "prism_dispersion.gltf"
    results = {}
    for rep, extra in (("exact RGB", []), ("hero 8", ["--spectral", "hero", "--lanes", "8"]), ("bands 16", ["--spectral", "bands", "--lanes", "16"])):
        # the slit is a few pixels wide, and each wavelength lights only part of it: four times the samples
        image, dropped = run_render(scene, args.spp * 4, seed=3, extra_args=extra)
        columns = image.mean(axis=0)
        x = np.arange(image.shape[1])
        sums = columns.sum(axis=0)
        centroids = (columns * x[:, None]).sum(axis=0) / np.maximum(sums, 1e-9)
        results[rep] = (sums, centroids)
        spread = centroids[2] - centroids[0]
        if rep == "exact RGB":
            ok = float(np.max(np.abs(image - image[..., :1]))) == 0.0 and dropped == 0
        else:
            ok = centroids[0] < centroids[1] < centroids[2] and spread > 4.0 and dropped == 0
        print(f"  {'ok  ' if ok else 'FAIL'} {rep:<10} red, green, blue centroids {np.round(centroids, 2)} px (blue - red {spread:.2f} px), light {np.round(sums, 3)}")
        if not ok:
            failures.append(f"dispersion {rep}: centroids {np.round(centroids, 2)}")
    # the same light per channel, beyond the bands' resolution: hero integrates the index over wavelength, the bands
    # read it at their centers
    difference = float(np.max(np.abs(results["bands 16"][0] / results["hero 8"][0] - 1)))
    ok = difference <= 0.05
    print(f"  {'ok  ' if ok else 'FAIL'} hero 8 against bands 16: {difference * 100:.2f} % (limit 5 %)")
    if not ok:
        failures.append(f"dispersion: hero and bands differ by {difference * 100:.2f} %")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--spp", type=int, default=256)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    if args.quick:
        args.spp = 64
    failures = []
    furnace(args, failures)
    monochrome(args, failures)
    dispersion(args, failures)
    convergence(args, failures)
    print("spectral_check: " + ("ok" if not failures else f"{len(failures)} failure(s)"))
    for f in failures:
        print("  " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
