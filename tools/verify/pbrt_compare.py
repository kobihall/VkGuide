#!/usr/bin/env python3
"""Renders one pbrt-v4 scene with pbrt and with this renderer, and compares the two images
(docs/plans/completed/pbrt-scene-import.md).

This renderer reads the .pbrt file through its importer (the batch mode renders a .pbrt with the file's camera,
resolution, sampler, filter and depth) and writes the radiance; pbrt writes the radiance times its imaging ratio
(shutter x ISO / 100), which the comparison divides back out. Each renderer renders twice with different seeds, so
each image's own noise is measured beside the difference: two converged renders of the same scene differ by their
noise alone, and a difference well above it is a difference in what was rendered. The import report lists what the
importer substituted, which is where such a difference comes from.

What it prints, per channel and overall:
  mean ratio          the image's mean radiance here over pbrt's: 1 when both draw the same light
  relative MSE        mean of (here - pbrt)^2 / (pbrt^2 + 0.01) over the pixels, of the two seeds' averages
  noise floor         the same measure between each renderer's two seeds, halved: what the relative MSE would be
                      if the renderers agreed and only the noise were left

`--spectral` renders here in that representation (`--lanes` sets its lane count) instead of the one the file asks for:
a file with an "rgb" film renders in exact RGB, which reads a measured spectrum such as gold's index at three
wavelengths, and `--spectral hero` integrates it as pbrt does.

`--cornell` compares the Cornell box of tools/verify/cornell.py, written as a .pbrt file by pbrt_cornell.py, which
needs no substitution. pbrt is the user's build: set PBRT and IMGTOOL, or they are looked for beside this repository.

Run:  python3 tools/verify/pbrt_compare.py SCENE.pbrt [--spp 256] [--pbrt-spp N] [--out DIR] [--max-mean-error 0.005]
                                            [--spectral hero|bands|gaussians] [--lanes N]
      python3 tools/verify/pbrt_compare.py --cornell [--spp 4096]
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

import pbrt_cornell
from vkscene import read_pfm, run_render, save_png


def render_pbrt(scene, spp, seed, workdir):
    out = workdir / f"pbrt_{seed}.exr"
    command = [pbrt_cornell.tool("pbrt", "PBRT"), "--quiet", "--spp", str(spp), "--seed", str(seed), "--outfile", str(out), str(scene)]
    subprocess.run(command, check=True, cwd=scene.parent, capture_output=True, text=True)
    pfm = out.with_suffix(".pfm")
    subprocess.run([pbrt_cornell.tool("imgtool", "IMGTOOL"), "convert", "--outfile", str(pfm), str(out)], check=True, capture_output=True)
    return read_pfm(pfm)


def render_here(scene, spp, seed, workdir, extra_args):
    log = []
    image, dropped = run_render(scene, spp, seed=seed, workdir=workdir, out_name=f"here_{seed}.pfm", timeout=3000, log=log, extra_args=extra_args)
    ratio = 1.0
    for line in log:
        if line.startswith("render: pbrt imaging ratio "):
            ratio = float(line.split()[-1])
    report = [line[len("render: import: "):] for line in log if line.startswith("render: import: ")]
    return image, dropped, ratio, report


def relative_mse(a, b, reference):
    return float(np.mean((a - b) ** 2 / (reference ** 2 + 0.01)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scene", nargs="?", type=Path)
    parser.add_argument("--cornell", action="store_true", help="compare the Cornell box written as a .pbrt file")
    parser.add_argument("--spp", type=int, default=256, help="samples per pixel here, per seed")
    parser.add_argument("--pbrt-spp", type=int, default=None, help="samples per pixel in pbrt, per seed (default: --spp)")
    parser.add_argument("--size", type=int, default=256, help="--cornell only: the image's width and height")
    parser.add_argument("--out", type=Path, default=None, help="keep the images and PNGs of both renders and their difference here")
    parser.add_argument("--max-mean-error", type=float, default=None, help="fail when a channel's mean ratio is further than this from 1")
    parser.add_argument("--spectral", default=None, help="render here in this representation (hero, bands, gaussians, rgb)")
    parser.add_argument("--lanes", type=int, default=None, help="with --spectral: the lane count")
    args = parser.parse_args()
    if args.scene is None and not args.cornell:
        parser.error("give a .pbrt scene or --cornell")

    workdir = Path(args.out or tempfile.mkdtemp(prefix="pbrt_compare_"))
    workdir.mkdir(parents=True, exist_ok=True)
    scene = args.scene.resolve() if args.scene else workdir / "cornell.pbrt"
    if args.cornell:
        scene.write_text(pbrt_cornell.scene_text(True, args.size, args.size, args.spp, "pbrt_out.exr"))
    pbrt_spp = args.pbrt_spp or args.spp
    extra_args = (["--spectral", args.spectral] if args.spectral else []) + (["--lanes", str(args.lanes)] if args.lanes else [])

    print(f"{scene}: {args.spp} spp here and {pbrt_spp} spp in pbrt, two seeds each")
    here = []
    for seed in (1, 2):
        image, dropped, ratio, report = render_here(scene, args.spp, seed, workdir, extra_args)
        here.append(image)
        if dropped:
            print(f"  here, seed {seed}: {dropped} samples dropped as non-finite")
    there = [render_pbrt(scene, pbrt_spp, seed, workdir) / ratio for seed in (1, 2)]
    if here[0].shape != there[0].shape:
        sys.exit(f"the images differ in size: {here[0].shape} here, {there[0].shape} from pbrt")

    a = (here[0] + here[1]) / 2
    b = (there[0] + there[1]) / 2
    mean_a = a.reshape(-1, 3).mean(axis=0)
    mean_b = b.reshape(-1, 3).mean(axis=0)
    ratio_rgb = mean_a / np.maximum(mean_b, 1e-12)
    difference = relative_mse(a, b, b)
    noise_here = relative_mse(here[0], here[1], b) / 4
    noise_there = relative_mse(there[0], there[1], b) / 4
    print(f"  imaging ratio {ratio:g} divided out of pbrt's image")
    print(f"  mean radiance: here {np.round(mean_a, 5)}, pbrt {np.round(mean_b, 5)}, ratio {np.round(ratio_rgb, 4)}")
    print(f"  relative MSE {difference:.3e}; noise floor {noise_here + noise_there:.3e} (here {noise_here:.3e}, pbrt {noise_there:.3e})")
    if report:
        print(f"  the import substituted or left out ({len(report)} line(s)):")
        for line in report:
            print(f"    {line}")
    if args.out:
        save_png(workdir / "here.png", a)
        save_png(workdir / "pbrt.png", b)
        diff = np.abs(a - b).mean(axis=2, keepdims=True) / np.maximum(b.mean(axis=2, keepdims=True), 0.05)
        save_png(workdir / "difference.png", np.repeat(np.clip(diff, 0, 1), 3, axis=2))
        print(f"  images in {workdir}")
    if args.max_mean_error is not None and np.any(np.abs(ratio_rgb - 1) > args.max_mean_error):
        print(f"FAILED: a channel's mean is more than {args.max_mean_error:.2%} from pbrt's")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
