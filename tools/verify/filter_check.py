#!/usr/bin/env python3
"""Pixel filter checks (docs/plans/completed/pbrt-film-and-reconstruction.md), on the renderer itself:

  box equivalence   kernel 00's filtered variant with a box filter draws a sample's place from the same
                    distribution as the unfiltered one, through a table instead of directly. The Cornell box
                    rendered both ways must agree as two renders of one estimator do: the same mean, and a
                    difference no larger than the difference between two seeds of the unfiltered one
  aliasing          a checkerboard plane running to the horizon. The tiles shrink below a pixel, and the box
                    filter lets them alias where a Gaussian smooths them, as in PBR 4ed figure 8.48. The
                    filter acts through the average of a pixel's samples, so one sample per pixel shows
                    nothing: a pixel is then a single point sample whatever the filter, and the render uses
                    64. The mean squared difference between neighboring pixels in the band near the horizon
                    measures it; the images go to the output directory as PNG for a look

Run:  python3 tools/verify/filter_check.py [--out DIR]
"""

import argparse
import sys
import tempfile
from pathlib import Path

import numpy as np

from vkscene import ROOT, box, camera, make_scene, quad, run_render, save_png, LAMBERTIAN

CORNELL = ROOT / "assets" / "scenes" / "cornell_box.gltf"


def kernel_variant(scene, camera_id, film_id, pixel_filter=None):
    """A copy of a scene dict that renders with the given kernel 00 and 09 variants."""
    import copy
    scene = copy.deepcopy(scene)
    extras = scene["scenes"][0]["extras"]
    extras.setdefault("kernels", {}).update({"00": camera_id, "09": film_id})
    if pixel_filter:
        extras["render"]["pixelFilter"] = pixel_filter
    return scene


def load_scene(path):
    import json
    return json.loads(Path(path).read_text())


def with_render(scene, **settings):
    scene["scenes"][0]["extras"]["render"].update(settings)
    return scene


def box_equivalence(workdir, spp):
    print(f"box equivalence: the Cornell box at 64 x 64, {spp} spp")
    base = with_render(load_scene(CORNELL), samplesPerFrame=8, useFixedSeed=True, seed=1)
    thin = kernel_variant(base, "thin_lens", "compensated_mean")
    table = kernel_variant(base, "filter_importance", "compensated_mean", {"kind": "box"})
    a, _ = run_render(thin, spp, size=(64, 64), seed=1, workdir=workdir / "a")
    b, _ = run_render(thin, spp, size=(64, 64), seed=2, workdir=workdir / "b")
    c, _ = run_render(table, spp, size=(64, 64), seed=1, workdir=workdir / "c")
    seed_noise = np.sqrt(((a - b) ** 2).mean())
    table_difference = np.sqrt(((a - c) ** 2).mean())
    mean_difference = abs(a.mean() - c.mean())
    standard_error = seed_noise / np.sqrt(2) / np.sqrt(a.size)
    print(f"  mean {a.mean():.5f} unfiltered, {c.mean():.5f} through the table; two seeds differ by rms {seed_noise:.5f}, the table by {table_difference:.5f}")
    ok = True
    if mean_difference > 5 * standard_error + 1e-5:
        print(f"    FAIL: the means differ by {mean_difference:.2g}, beyond the noise {standard_error:.2g}")
        ok = False
    if table_difference > 1.2 * seed_noise:
        print(f"    FAIL: the table's render differs from the unfiltered one by {table_difference:.4f}, more than two seeds do ({seed_noise:.4f})")
        ok = False
    return ok


def checker_scene(size=(240, 160)):
    shapes = []
    tile = 2.0
    columns = 24
    rows = 70
    for r in range(rows):
        for c in range(columns):
            white = (r + c) % 2 == 0
            albedo = (0.9, 0.9, 0.9) if white else (0.03, 0.03, 0.03)
            center = ((c - columns / 2 + 0.5) * tile, 0.0, -(r + 0.5) * tile)
            shapes.append(quad(f"tile_{r}_{c}", center, (1, 0, 0), (0, 0, 1), tile, tile, LAMBERTIAN(albedo)))
    cam = camera((0.0, 1.5, 2.0), (0.0, 1.5, -50.0), 40.0)
    return make_scene(shapes, cam, size[0], size[1], background=(1.0, 1.0, 1.0), samples=64, ray_depth=2, samples_per_frame=8)


def aliasing(workdir, out_dir):
    print("aliasing: a checkerboard to the horizon at 64 spp")
    scene = checker_scene()
    images = {}
    for label, camera_id, film_id, pixel_filter in [
        ("box", "thin_lens", "compensated_mean", None),
        ("gaussian", "filter_importance", "compensated_mean", {"kind": "gaussian"}),
        ("mitchell", "filter_importance", "compensated_mean", {"kind": "mitchell"}),
    ]:
        variant = kernel_variant(scene, camera_id, film_id, pixel_filter)
        image, _ = run_render(variant, 64, workdir=workdir / label, timeout=600)
        images[label] = image
        save_png(out_dir / f"checker_{label}.png", image)

    # the band from just below the horizon (row 80, at the camera's height) down to where tiles are a few pixels
    def roughness(image):
        band = image[82:112, :, 1]
        return float(((band[:, 1:] - band[:, :-1]) ** 2).mean() + ((band[1:, :] - band[:-1, :]) ** 2).mean())

    r = {label: roughness(image) for label, image in images.items()}
    print("  mean squared difference between neighboring pixels near the horizon: " + ", ".join(f"{k} {v:.4f}" for k, v in r.items()))
    print(f"  images written to {out_dir}")
    ok = True
    if not r["gaussian"] < r["box"]:
        print("    FAIL: the Gaussian filter should alias less than the box filter")
        ok = False
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=None, help="where the aliasing images go (default: a temporary directory)")
    parser.add_argument("--spp", type=int, default=1024)
    args = parser.parse_args()
    workdir = Path(tempfile.mkdtemp(prefix="vkfilter_"))
    out_dir = Path(args.out) if args.out else workdir
    out_dir.mkdir(parents=True, exist_ok=True)
    ok = box_equivalence(workdir, args.spp)
    ok &= aliasing(workdir, out_dir)
    print("filter check: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
