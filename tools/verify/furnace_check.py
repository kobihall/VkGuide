#!/usr/bin/env python3
"""White furnace tests (docs/plans/completed/pbrt-verification-harness.md): one sphere of a material in
a uniform environment of radiance 1 and nothing else. A convex object has no interreflection, so what
it renders is known:

  exact     a material that reflects or transmits everything it receives renders exactly 1 (a white
            mirror, glass), and a white Lambertian of reflectance rho renders rho
  at most   any other passive material renders at most 1: a pixel above 1 by more than the noise is
            energy the material made. A microfacet material shows the energy it loses to single
            scattering, which is below 1 by design

and the converged value must not depend on how the renderer got there, so the same sphere is also
rendered under every direct-lighting strategy, every sampler, both background kinds, the
intersection precision flags and every pixel filter with the compensated film, and the means must
agree within their noise. A filter only blurs, and a furnace is uniform, so a material that is exact
must stay exactly 1 in every pixel whatever the filter, even where its negative lobes weigh a sample -1.

The mean is over the pixels of the sphere's disc less a 1.5 pixel margin (an edge pixel blends the
sphere with the background). The noise is the standard error of that mean from the pixels themselves,
which are independent samples of it.

Run:  python3 tools/verify/furnace_check.py [--spp N] [--quick] [--only NAME]
"""

import argparse
import math
import sys
import tempfile
from pathlib import Path

import numpy as np

from make_scenes import FURNACE_MATERIALS, furnace_scene, write_uniform_hdr
from vkscene import ROOT, run_render

UNIFORM_MAP = ROOT / "assets" / "environment_maps" / "uniform_white.hdr"
SIZE = 64
DISTANCE = 4.0
FOV = 40.0

STRATEGIES = ["bsdf", "light", "mis_balance", "mis_power"]
FILTERS = ["box", "triangle", "gaussian", "mitchell", "lanczos"]
# src/film.cpp's default radius of each: the disc keeps this far from its edge when a filter blurs it
FILTER_RADIUS = {"box": 0.5, "triangle": 2.0, "gaussian": 1.5, "mitchell": 2.0, "lanczos": 4.0}
SAMPLERS = ["pcg_hash", "independent", "stratified", "halton", "sobol", "padded_sobol", "zsobol", "pmj02bn"]
TRAVERSALS = ["bvh_binary", "bvh_cwbvh", "linear"]

# what each material must render: ("exact", value) or ("at most", 1)
EXPECT = {
    "lambertian 1": ("exact", 1.0),
    "lambertian 0.5": ("exact", 0.5),
    "metal mirror": ("exact", 1.0),
    "dielectric 1.5": ("exact", 1.0),
}


def disc_mask(margin=1.5):
    t = math.tan(math.radians(FOV) / 2)
    ys, xs = np.mgrid[0:SIZE, 0:SIZE]
    px = ((xs + 0.5) / SIZE * 2 - 1) * t
    py = (1 - (ys + 0.5) / SIZE * 2) * t
    radius = math.tan(math.asin(1.0 / DISTANCE))
    return np.hypot(px, py) < radius - margin * (2 * t / SIZE)


MASK = disc_mask()


def measure(image, margin=1.5):
    """The disc's mean, its standard error, and the largest pixel. A filter that reaches `margin` pixels
    from the disc's edge blends the sphere with the background there, so the disc shrinks to match."""
    mask = MASK if margin == 1.5 else disc_mask(margin)
    v = image[mask][:, 1]
    return v.mean(), v.std() / math.sqrt(len(v)), v.max()


class Runner:
    def __init__(self, spp, workdir):
        self.spp = spp
        self.workdir = workdir
        self.materials = dict(FURNACE_MATERIALS)
        self.failures = []
        self.cache = {}

    def render(self, name, *, strategy="mis_power", sampler=None, background="solid", traversal=None, flags=None, pixel_filter=None, film="compensated_mean"):
        key = (name, strategy, sampler, background, traversal, tuple(sorted((flags or {}).items())), pixel_filter, film)
        if key in self.cache:
            return self.cache[key]
        kernels = {"06": strategy}
        if sampler:
            kernels["01"] = sampler
        kwargs = {"kernels": kernels, "samples": self.spp}
        if background == "solid":
            kwargs["background"] = (1.0, 1.0, 1.0)
        else:
            kwargs["environment_map"] = UNIFORM_MAP
        if traversal:
            kwargs["traversal"] = traversal
        if flags:
            kwargs["render_extra"] = dict(flags)
        margin = 1.5
        if pixel_filter:
            # a scene that names kernel 00 and 09 and a filter renders with them; one that names none
            # renders as scenes before pixel filters did, with box pixels and a running mean
            kernels["00"] = "filter_importance"
            kernels["09"] = film
            kwargs.setdefault("render_extra", {})["pixelFilter"] = {"kind": pixel_filter}
            margin = max(margin, FILTER_RADIUS[pixel_filter] + 0.5)
        scene = furnace_scene(self.materials[name], **kwargs)
        image, dropped = run_render(scene, self.spp, workdir=self.workdir)
        result = measure(image, margin) + (dropped, float(image.min()), float(image.max()))
        self.cache[key] = result
        return result

    def fail(self, message):
        print(f"    FAIL: {message}")
        self.failures.append(message)


def check_expectation(runner, name, label, result):
    mean, se, peak, dropped = result[:4]
    kind, value = EXPECT.get(name, ("at most", 1.0))
    slack = 4 * se + 1e-3
    if dropped:
        runner.fail(f"{name} [{label}]: {dropped} sample(s) dropped as non-finite")
    if kind == "exact" and abs(mean - value) > slack:
        runner.fail(f"{name} [{label}]: renders {mean:.5f} +- {se:.5f}, must be {value}")
    if kind == "at most" and mean > 1.0 + slack:
        runner.fail(f"{name} [{label}]: renders {mean:.5f} +- {se:.5f}, above 1: the material made energy")


def agree(runner, what, results):
    """Every result of a group must match the first within the noise."""
    reference_label, (mean0, se0, *_) = results[0]
    for label, (mean, se, *_) in results[1:]:
        if abs(mean - mean0) > 4 * math.hypot(se, se0) + 1e-3:
            runner.fail(f"{what}: {label} renders {mean:.5f} +- {se:.5f} but {reference_label} renders {mean0:.5f} +- {se0:.5f}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spp", type=int, default=512)
    parser.add_argument("--quick", action="store_true", help="a fraction of the matrix")
    parser.add_argument("--only", default="", help="materials whose name contains this")
    args = parser.parse_args()

    # *.hdr files are not tracked, so the uniform map is written when it is missing
    if not UNIFORM_MAP.exists():
        write_uniform_hdr(UNIFORM_MAP)
    runner = Runner(args.spp, tempfile.mkdtemp(prefix="vkfurnace_"))
    names = [n for n, _ in FURNACE_MATERIALS if args.only in n]
    print(f"white furnace, {SIZE}x{SIZE} at {args.spp} spp, {MASK.sum()} disc pixels\n")

    print("every material, solid white background, MIS (power)")
    for name in names:
        result = runner.render(name)
        print(f"  {name:26s} {result[0]:.5f} +- {result[1]:.5f}   peak {result[2]:.3f}")
        check_expectation(runner, name, "solid, mis_power", result)

    print("\nevery strategy and both backgrounds: the means must agree")
    strategy_materials = ["lambertian 0.5", "metal mirror", "phong 0.5", "pbr rough 0.5 metal 0", "pbr rough 0.2 metal 1", "dielectric 1.5"]
    if args.quick:
        strategy_materials = strategy_materials[:2] + strategy_materials[3:4]
    for name in [n for n in strategy_materials if args.only in n]:
        group = []
        for background in ("solid", "env"):
            for strategy in STRATEGIES:
                result = runner.render(name, strategy=strategy, background=background)
                group.append((f"{background}/{strategy}", result))
                check_expectation(runner, name, f"{background}/{strategy}", result)
        print(f"  {name:26s} " + "  ".join(f"{label} {r[0]:.4f}" for label, r in group))
        agree(runner, f"{name}, strategies and backgrounds", group)

    print("\nevery sampler: the means must agree")
    sampler_materials = ["lambertian 0.5", "pbr rough 0.5 metal 0"]
    for name in [n for n in sampler_materials if args.only in n]:
        group = []
        for sampler in (SAMPLERS[:2] + SAMPLERS[6:7] if args.quick else SAMPLERS):
            result = runner.render(name, sampler=sampler, background="env")
            group.append((sampler, result))
            check_expectation(runner, name, sampler, result)
        print(f"  {name:26s} " + "  ".join(f"{label} {r[0]:.4f}" for label, r in group))
        agree(runner, f"{name}, samplers", group)

    print("\nevery traversal and the intersection flags: the means must agree")
    for name in [n for n in ["lambertian 0.5", "dielectric 1.5"] if args.only in n]:
        group = []
        for traversal in TRAVERSALS:
            group.append((traversal, runner.render(name, traversal=traversal)))
        for label, flags in [("no error bounds", {"errorBoundedOffsets": False}), ("no watertight", {"watertightTriangles": False}), ("neither", {"errorBoundedOffsets": False, "watertightTriangles": False})]:
            group.append((label, runner.render(name, flags=flags)))
        for label, result in group:
            check_expectation(runner, name, label, result)
        print(f"  {name:26s} " + "  ".join(f"{label} {r[0]:.4f}" for label, r in group))
        agree(runner, f"{name}, traversals and flags", group)

    print("\nevery pixel filter with the compensated film: a material that is exact stays exactly 1 in every pixel")
    for name in [n for n in ["metal mirror", "dielectric 1.5", "lambertian 1"] if args.only in n]:
        group = []
        for pixel_filter in (FILTERS[:3] if args.quick else FILTERS):
            result = runner.render(name, pixel_filter=pixel_filter, background="env")
            group.append((pixel_filter, result))
            check_expectation(runner, name, pixel_filter, result)
            low, high = result[4], result[5]
            if name != "lambertian 1" and (low < 1.0 - 1e-5 or high > 1.0 + 1e-5):
                runner.fail(f"{name} [{pixel_filter}]: pixels range over [{low:.6f}, {high:.6f}], and every one must be exactly 1")
        print(f"  {name:26s} " + "  ".join(f"{label} {r[0]:.5f}" for label, r in group))
    for name in [n for n in ["lambertian 0.5", "pbr rough 0.5 metal 0"] if args.only in n]:
        group = [("box, running mean", runner.render(name, background="env"))]
        for pixel_filter in (FILTERS[:3] if args.quick else FILTERS):
            result = runner.render(name, pixel_filter=pixel_filter, background="env")
            group.append((pixel_filter, result))
            check_expectation(runner, name, pixel_filter, result)
        print(f"  {name:26s} " + "  ".join(f"{label} {r[0]:.4f}" for label, r in group))
        agree(runner, f"{name}, pixel filters", group)

    print(f"\nwhite furnace: {'ok' if not runner.failures else str(len(runner.failures)) + ' failure(s)'}")
    return 1 if runner.failures else 0


if __name__ == "__main__":
    sys.exit(main())
