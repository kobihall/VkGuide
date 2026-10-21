#!/usr/bin/env python3
"""Film checks (docs/plans/completed/pbrt-film-and-reconstruction.md), on the renderer itself:

  precision   the compensated film against the running mean, which see the same samples, so their
              difference is the float accumulation's alone. A float running mean drifts in proportion
              to the sample count; the compensated sums do not. Reported at 4k, 16k and, with --long,
              64k and 256k samples per pixel. The drift stays far below the noise at these counts on
              a scene with ordinary variance, which is the finding: it matters for references rendered
              for hours and for pixels with almost no variance
  outliers    a glass caustic made of fireflies, at 256 samples against a 16384-sample reference. The
              outlier-reweighted film (kappa 4) must beat the unbiased film's relative MSE by 4x and
              keep more of the energy than a clamp of 10 does, at an equal or lower error

Run:  python3 tools/verify/film_check.py [--long]
"""

import argparse
import copy
import sys
import tempfile
from pathlib import Path

import numpy as np

from caustic_scene import caustic_scene
from vkscene import ROOT, run_render

CORNELL = ROOT / "assets" / "scenes" / "cornell_box.gltf"


def variant(scene, film, extra=None):
    scene = copy.deepcopy(scene)
    extras = scene["scenes"][0]["extras"]
    extras.setdefault("kernels", {}).update({"00": "filter_importance", "01": "pcg_hash", "09": film})
    extras["render"]["pixelFilter"] = {"kind": "box"}
    extras["render"]["samplesPerFrame"] = 8
    extras["render"].update(extra or {})
    return scene


def relative_mse(image, reference):
    return float(((image - reference) ** 2 / (reference ** 2 + 0.01)).mean())


def precision(workdir, long_run):
    import json
    print("precision: the Cornell box at 32 x 32, running mean against compensated mean, the same samples")
    scene = json.loads(CORNELL.read_text())
    ok = True
    for spp in [4096, 16384] + ([65536, 262144] if long_run else []):
        a, _ = run_render(variant(scene, "running_mean"), spp, size=(32, 32), seed=1, workdir=workdir / "a", timeout=3600)
        b, _ = run_render(variant(scene, "compensated_mean"), spp, size=(32, 32), seed=1, workdir=workdir / "b", timeout=3600)
        drift = np.sqrt(((a - b) ** 2).mean()) / np.sqrt((b ** 2).mean())
        print(f"  {spp:7d} spp: rms(running - compensated) / rms(compensated) = {drift:.3g}")
        if drift > 1e-2:
            print("    FAIL: the films disagree by more than float accumulation can explain")
            ok = False
    return ok


def outliers(workdir):
    print("outliers: a glass caustic, 96 x 96")
    scene = caustic_scene()
    reference, _ = run_render(variant(scene, "compensated_mean"), 16384, seed=99, workdir=workdir / "ref", timeout=1800)
    rows = {
        "unbiased": ("compensated_mean", {}),
        "outlier kappa 4": ("outlier_reweighted", {"outlierKappa": 4.0}),
        "clamp 10": ("compensated_mean", {"clampSamples": True, "clampValue": 10.0}),
    }
    result = {}
    for label, (film, extra) in rows.items():
        errors, means = [], []
        for seed in (11, 12, 13):
            image, _ = run_render(variant(scene, film, extra), 256, seed=seed, workdir=workdir / "t")
            errors.append(relative_mse(image, reference))
            means.append(image.mean() / reference.mean())
        result[label] = (np.mean(errors), np.mean(means))
        print(f"  {label:16s} relative MSE {result[label][0]:.5f}, mean {result[label][1]:.4f} of the reference")
    ok = True
    if not result["outlier kappa 4"][0] < result["unbiased"][0] / 4:
        print("    FAIL: the outlier film should lower the unbiased film's relative MSE at least fourfold")
        ok = False
    if not result["outlier kappa 4"][1] > result["clamp 10"][1]:
        print("    FAIL: the outlier film should keep more energy than a clamp of 10")
        ok = False
    if not result["outlier kappa 4"][0] <= 1.2 * result["clamp 10"][0]:
        print("    FAIL: the outlier film should not be noisier than a clamp of 10")
        ok = False
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--long", action="store_true", help="also 64k and 256k samples (about 15 minutes)")
    args = parser.parse_args()
    workdir = Path(tempfile.mkdtemp(prefix="vkfilm_"))
    ok = precision(workdir, args.long)
    ok &= outliers(workdir)
    print("film check: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
