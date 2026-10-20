#!/usr/bin/env python3
"""The Cornell box against two references that share nothing with the renderer's code
(docs/plans/completed/pbrt-verification-harness.md):

  1. radiosity.py, an exact diffuse solve of the empty box. Its light is 0.001 below the ceiling so
     that what the renderer's two-sided emitter sends up is absorbed, as the solve assumes.
  2. pbrt-v4, the user's own build (pbrt_cornell.py), on the box with its two blocks. pbrt's
     light is two-sided and black, like the renderer's, so the model matches at the real 0.1 gap.
     pbrt's spectral transport and the renderer's RGB transport differ slightly for coloured walls.

Each image is compared in 16 x 16 pixel blocks. The renderer's noise in a block comes from two renders
with different seeds; a block passes when it is within 4 standard errors plus the stated modelling
tolerance of the reference, and the whole image's mean must agree to the stated percentage.

Run:  python3 tools/verify/cornell_check.py [--spp N] [--size N] [--skip-pbrt]
"""

import argparse
import math
import sys
import tempfile
from pathlib import Path

import numpy as np

import cornell
from radiosity import Cornell
from vkscene import ROOT, run_render, save_png, write_pfm

BLOCK = 16


def interior_mask(wall_map):
    """The pixels whose 3 x 3 neighbourhood shows one wall. The solve takes one sample at a pixel's
    centre and the renderer averages the whole pixel, so they differ along every edge (the light's,
    the walls' corners); an interior pixel has no edge to differ on."""
    h, w = wall_map.shape
    padded = np.pad(wall_map, 1, mode="edge")
    stacked = np.stack([padded[dy:dy + h, dx:dx + w] for dy in range(3) for dx in range(3)], axis=-1)
    return np.all(stacked == wall_map[..., None], axis=-1) & (wall_map >= 0)


def blocks(image, mask=None):
    """The mean of each block over the pixels of `mask` (all, if None), and how many there were."""
    h, w, c = image.shape
    if mask is None:
        mask = np.ones((h, w), dtype=bool)
    weight = mask[..., None].astype(np.float64)
    summed = (image * weight).reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK, c).sum(axis=(1, 3))
    count = weight.reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK, 1).sum(axis=(1, 3))
    return summed / np.maximum(count, 1), count[..., 0]


def block_noise(a, b, mask=None):
    """The standard error of the block means of the average of two independent renders: the pixels of
    the average have a variance of (a - b)^2 / 4."""
    h, w, c = a.shape
    if mask is None:
        mask = np.ones((h, w), dtype=bool)
    weight = mask[..., None].astype(np.float64)
    variance = ((a - b) ** 2 / 4.0 * weight).reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK, c).sum(axis=(1, 3))
    count = weight.reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK, 1).sum(axis=(1, 3))
    return np.sqrt(variance) / np.maximum(count, 1)


def render_pair(scene, spp, size, workdir):
    a, dropped_a = run_render(scene, spp, size=(size, size), seed=1, workdir=workdir)
    b, dropped_b = run_render(scene, spp, size=(size, size), seed=2, workdir=workdir)
    return a, b, dropped_a + dropped_b


def compare(name, rendered_a, rendered_b, reference, *, mean_tolerance, block_tolerance, extra_noise=None, outdir=None, mask=None):
    mean_image = (rendered_a + rendered_b) / 2
    failures = []
    selected = np.ones(mean_image.shape[:2], dtype=bool) if mask is None else mask
    mean_rendered = mean_image[selected].mean(axis=0)
    mean_reference = reference[selected].mean(axis=0)
    ratio = mean_rendered / mean_reference
    print(f"  {name}: mean radiance over {selected.sum()} pixels: renderer {mean_rendered.round(4)}, reference {mean_reference.round(4)}, ratio {ratio.round(4)}")
    if np.any(np.abs(ratio - 1.0) > mean_tolerance):
        failures.append(f"{name}: the mean radiance differs from the reference by {np.abs(ratio - 1).max() * 100:.2f}%, more than {mean_tolerance * 100:.1f}%")
    noise = block_noise(rendered_a, rendered_b, mask)
    if extra_noise is not None:
        noise = np.hypot(noise, extra_noise)
    rendered_blocks, counts = blocks(mean_image, mask)
    reference_blocks, _ = blocks(reference, mask)
    # a block with few interior pixels says little
    valid = (counts >= 32)[..., None] & np.ones_like(rendered_blocks, dtype=bool)
    deviation = np.where(valid, rendered_blocks - reference_blocks, 0.0)
    allowed = 4 * noise + block_tolerance * np.abs(reference_blocks) + 1e-3
    bad = np.abs(deviation) > allowed
    worst = (np.abs(deviation) / allowed).max()
    print(f"    {bad.sum()} of {valid.sum()} block-channels outside 4 standard errors + {block_tolerance * 100:.1f}%; the worst is {worst:.2f} of its allowance")
    if bad.any():
        failures.append(f"{name}: {bad.sum()} of {bad.size} blocks differ from the reference by more than the noise and {block_tolerance * 100:.1f}%")
    relative = np.abs(mean_image - reference) / np.maximum(np.abs(reference), 0.01)
    print(f"    MAPE {relative[selected].mean() * 100:.2f}%  RMSE {math.sqrt(((mean_image - reference)[selected] ** 2).mean()):.5f}")
    if outdir:
        save_png(Path(outdir) / f"{name}_renderer.png", mean_image, 2.0)
        save_png(Path(outdir) / f"{name}_reference.png", reference, 2.0)
        save_png(Path(outdir) / f"{name}_difference.png", np.abs(mean_image - reference) * 5, 1.0)
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spp", type=int, default=1024)
    parser.add_argument("--size", type=int, default=128)
    parser.add_argument("--skip-pbrt", action="store_true")
    parser.add_argument("--patches", type=int, default=24)
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    workdir = Path(tempfile.mkdtemp(prefix="vkcornell_"))
    outdir = Path(args.out) if args.out else None
    if outdir:
        outdir.mkdir(parents=True, exist_ok=True)
    failures = []

    print(f"1. the empty box against radiosity ({args.patches} x {args.patches} patches per wall), {args.size}^2 at {args.spp} spp")
    scene = cornell.scene(False, args.size, args.size, args.spp)
    for shape in scene["scenes"][0]["extras"]["geometry"]:
        if shape["name"] == "light":
            shape["position"][1] = 55.499
    a, b, dropped = render_pair(scene, args.spp, args.size, workdir / "empty")
    if dropped:
        failures.append(f"{dropped} sample(s) dropped")
    solved = Cornell(args.patches)
    truth = solved.render(args.size, args.size)
    interior = interior_mask(solved.wall_map) & (solved.wall_map < len(cornell.WALLS))
    failures += compare("empty_vs_radiosity", a, b, truth, mean_tolerance=0.01, block_tolerance=0.02, outdir=outdir, mask=interior)

    if not args.skip_pbrt:
        import pbrt_cornell
        print(f"\n2. pbrt-v4 calibration and the empty box, {args.size}^2")
        try:
            pbrt_empty = pbrt_cornell.render_pbrt(False, args.size, args.size, 256, workdir / "pbrt_empty", light_height=55.499)
        except Exception as error:
            print(f"  pbrt could not run: {error}")
            failures.append("pbrt could not run")
        else:
            # pbrt's emitter seen directly gives the scale pbrt's film puts on radiance
            # pbrt's emitter seen directly gives the scale pbrt's film puts on radiance: the pixels
            # that are all light, not the edge pixels that blend it with the ceiling
            peak = pbrt_empty[..., 1].max()
            light_pixels = pbrt_empty[..., 1] > 0.95 * peak
            scale = pbrt_empty[light_pixels][:, 1].mean() / cornell.LIGHT_RADIANCE
            print(f"  pbrt renders the light as {pbrt_empty[light_pixels][:, 1].mean():.4f} for L = {cornell.LIGHT_RADIANCE} ({light_pixels.sum()} pixels): scale {scale:.4f}")
            failures += compare("pbrt_empty_vs_radiosity", pbrt_empty / scale, pbrt_empty / scale, truth, mean_tolerance=0.02, block_tolerance=0.04, extra_noise=0.0, outdir=outdir, mask=interior)

            print(f"\n3. the box with its two blocks against pbrt-v4, {args.size}^2")
            scene = cornell.scene(True, args.size, args.size, args.spp)
            a, b, dropped = render_pair(scene, args.spp, args.size, workdir / "boxes")
            if dropped:
                failures.append(f"{dropped} sample(s) dropped")
            pbrt_boxes = pbrt_cornell.render_pbrt(True, args.size, args.size, 1024, workdir / "pbrt_boxes") / scale
            failures += compare("boxes_vs_pbrt", a, b, pbrt_boxes, mean_tolerance=0.02, block_tolerance=0.04, outdir=outdir)

    print(f"\ncornell: {'ok' if not failures else str(len(failures)) + ' failure(s)'}")
    for message in failures:
        print(f"  FAIL: {message}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
