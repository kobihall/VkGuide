"""Renders the scenes of tools/verify/bidirectional_scenes.py with the bidirectional integrators of
docs/plans/completed/pbrt-bidirectional-path-tracing.md and checks them:

  convergence      light, lvc and bdpt against the path tracer on the Cornell box and Veach's MIS scene: each image's
                   mean over the path tracer's (light tracing leaves out what only a specular camera path sees, which
                   neither scene has in view). Veach's tiny bright lights, seen in its glossy plates, are fireflies to the
                   connections to the camera, so its means converge slowly and its tolerance is wider
  MIS weights      bdpt's strategy images (bd_strategies.gltf): the weighted ones sum to the picture, and each depth's sum
                   matches its light-sampling strategy (s = 1) alone, which estimates all of that depth's light
  outputs          bdpt's auxiliary outputs and its normal debug view are the path tracer's, from the same camera rays
  spectral         lvc and bdpt in hero wavelengths (4 lanes) against the path tracer in exact RGB, on the Cornell box.
                   lvc's frames share one hero wavelength sample, so its mean has one wavelength sample a frame and its
                   tolerance follows the frame count
  caustic          the relative MSE of path, light, lvc and bdpt at equal GPU time on bd_caustic.gltf, over the floor,
                   which holds the caustic, against an lvc reference of two seeds whose own noise is taken out; lvc must
                   beat the path tracer, and bdpt's error is printed. The sphere is left out: light seen through it after
                   the floor (camera, glass, floor, glass, light) is fireflies that no strategy connects. Camera paths
                   here meet rare fireflies that pbrt's do not (the path tracer's noise is 460 times pbrt's on a pbrt
                   version of this scene, in builds from before the bidirectional integrators too), and bdpt inherits
                   them; without its 16 worst pixels its noise is pbrt's own BDPT's

bdpt against pbrt-v4's own BDPT is tools/verify/pbrt_compare.py --cornell --integrator bdpt.

Run:  python3 tools/verify/bidirectional_check.py [--quick]
"""

import argparse
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402
from aov_check import read_exr  # noqa: E402
from bidirectional_scenes import SCENES, with_integrator  # noqa: E402

FAILURES = []


def check(condition, message):
    print(("  ok    " if condition else "  FAIL  ") + message)
    if not condition:
        FAILURES.append(message)


def load(name):
    return json.loads((SCENES / f"{name}.gltf").read_text())


def gpu_ms(log):
    for line in log:
        if " ms of GPU time" in line:
            return float(line.split(" dropped, ")[1].split(" ms")[0])
    return float("nan")


def render(scene, spp, workdir, name, seed=1, extra_args=()):
    log = []
    image, dropped = vkscene.run_render(scene, spp, seed=seed, workdir=workdir, out_name=f"{name}.pfm", log=log, extra_args=list(extra_args), timeout=3000)
    return image, gpu_ms(log), dropped


def relative_mse(image, reference):
    return float(np.mean((image - reference) ** 2 / (reference ** 2 + 0.01)))


def convergence(workdir, quick):
    print("convergence against the path tracer")
    samples = {"path": 512 if quick else 4096, "light": 128 if quick else 1024, "lvc": 128 if quick else 1024, "bdpt": 64 if quick else 256}
    for name, tolerance in (("bd_cornell", 0.015 if quick else 0.004), ("bd_veach", 0.06 if quick else 0.02)):
        base = load(name)
        reference, _, _ = render(with_integrator(base, "path"), samples["path"], workdir, f"{name}_path", seed=11)
        for integrator in ("light", "lvc", "bdpt"):
            image, ms, dropped = render(with_integrator(base, integrator), samples[integrator], workdir, f"{name}_{integrator}", seed=3)
            ratio = image.mean() / reference.mean()
            check(abs(ratio - 1) < tolerance and dropped == 0,
                  f"{name} {integrator}: mean {ratio:.4f} of the path tracer's ({samples[integrator]} spp, {ms:.0f} ms, {dropped} dropped)")


def strategies(workdir, quick):
    print("bdpt's MIS weights through its strategy images")
    base = load("bd_strategies")
    spp = 64 if quick else 512
    weighted, _ = vkscene.run_render(base, spp, seed=5, workdir=workdir, out_name="strategies_weighted.exr", timeout=3000)
    unweighted_scene = json.loads(json.dumps(base))
    unweighted_scene["scenes"][0]["extras"]["render"]["bidirectional"]["visualizeWeights"] = False
    unweighted, _ = vkscene.run_render(unweighted_scene, spp, seed=5, workdir=workdir, out_name="strategies_unweighted.exr", timeout=3000)
    w, _ = read_exr(weighted, workdir)
    u, _ = read_exr(unweighted, workdir)
    rgb = lambda channels, stem: np.stack([channels[f"{stem}.{c}"] for c in "RGB"], axis=-1)
    beauty = np.stack([w[c] for c in "RGB"], axis=-1)
    stems = sorted({key[:-2] for key in w if key.startswith("bdpt_d")})
    total = sum(rgb(w, stem) for stem in stems)
    check(len(stems) > 0 and abs(total.mean() / beauty.mean() - 1) < 1e-4, f"the {len(stems)} weighted strategy images sum to the picture: {total.mean() / beauty.mean():.6f}")
    depths = sorted({int(stem[6:8]) for stem in stems})
    for depth in depths[1:]:
        weighted_sum = sum(rgb(w, stem).mean() for stem in stems if int(stem[6:8]) == depth)
        light_sampling = rgb(u, f"bdpt_d{depth:02}_s01_t{depth + 1:02}").mean()
        ratio = weighted_sum / light_sampling
        check(abs(ratio - 1) < (0.05 if quick else 0.02), f"depth {depth}: the weighted strategies sum to {ratio:.4f} of light sampling alone")


def outputs(workdir):
    print("bdpt's outputs and debug views against the path tracer's")
    base = load("bd_cornell")
    aov = ["--aov", "normal,albedo,depth,ids"]
    here, _ = vkscene.run_render(with_integrator(base, "bdpt"), 4, seed=2, workdir=workdir, out_name="aov_bdpt.exr", extra_args=aov)
    # one sample a frame, as bdpt takes them, for the same camera rays
    path = with_integrator(base, "path")
    path["scenes"][0]["extras"]["render"]["samplesPerFrame"] = 1
    there, _ = vkscene.run_render(path, 4, seed=2, workdir=workdir, out_name="aov_path.exr", extra_args=aov)
    a, _ = read_exr(here, workdir)
    b, _ = read_exr(there, workdir)
    for name in ("N.X", "N.Y", "N.Z", "Albedo.R", "Z", "id.instance", "id.material"):
        difference = float(np.max(np.abs(a[name] - b[name]))) if name in a and name in b else float("inf")
        check(difference < 1e-4, f"{name}: largest difference {difference:.2e}")
    normal_bdpt, _, _ = render(with_integrator(base, "bdpt"), 4, workdir, "normal_bdpt", seed=2, extra_args=["--debug-view", "normal"])
    normal_path, _, _ = render(path, 4, workdir, "normal_path", seed=2, extra_args=["--debug-view", "normal"])
    difference = float(np.mean(np.abs(normal_bdpt - normal_path)))
    check(difference < 1e-4, f"the normal debug view: mean difference {difference:.2e}")


def spectral(workdir, quick):
    print("hero wavelengths (4 lanes) against exact RGB")
    base = load("bd_cornell")
    reference, _, _ = render(with_integrator(base, "path"), 512 if quick else 4096, workdir, "spectral_path", seed=11)
    # lvc's frames share one wavelength sample: about 30 % between frames' means on this box, over the root of the frames
    for integrator, spp, tolerance in (("lvc", 128 if quick else 1024, 4 * 0.3 / np.sqrt(128 if quick else 1024)), ("bdpt", 64 if quick else 256, 0.02 if quick else 0.006)):
        image, ms, _ = render(with_integrator(base, integrator), spp, workdir, f"spectral_{integrator}", seed=4, extra_args=["--spectral", "hero", "--lanes", "4"])
        ratio = image.mean() / reference.mean()
        check(abs(ratio - 1) < tolerance, f"{integrator} hero: mean {ratio:.4f} of the path tracer's in RGB ({spp} spp, {ms:.0f} ms)")


def caustic(workdir, quick):
    budget = 1000.0 if quick else 4000.0
    print(f"the caustic at equal GPU time ({budget / 1000:g} s), over the floor")
    base = load("bd_caustic")
    reference_spp = 2048 if quick else 8192
    halves = []
    floor = None
    for seed in (21, 22):
        exr, _ = vkscene.run_render(with_integrator(base, "lvc"), reference_spp, seed=seed, workdir=workdir, out_name=f"caustic_reference_{seed}.exr",
                                    extra_args=["--aov", "ids"], timeout=3000)
        channels, _ = read_exr(exr, workdir)
        halves.append(np.stack([channels[c] for c in "RGB"], axis=-1))
        # the floor: the instance the bottom row's middle sees
        ids = channels["id.instance"]
        floor = ids == ids[-1, ids.shape[1] // 2]
    reference = (halves[0] + halves[1]) / 2
    masked = lambda image: image[floor]
    reference_noise = relative_mse(masked(halves[0]), masked(halves[1])) / 4
    errors = {}
    for integrator in ("path", "light", "lvc", "bdpt"):
        scene = with_integrator(base, integrator)
        # the time a sample takes, past the first frames' pipelines
        _, short, _ = render(scene, 8, workdir, f"caustic_{integrator}_8", seed=3)
        _, long, _ = render(scene, 40, workdir, f"caustic_{integrator}_40", seed=3)
        per_sample = max((long - short) / 32.0, 1e-3)
        spp = max(int(budget / per_sample), 1)
        image, ms, _ = render(scene, spp, workdir, f"caustic_{integrator}", seed=7)
        errors[integrator] = max(relative_mse(masked(image), masked(reference)) - reference_noise, 0.0)
        print(f"        {integrator:6s} {spp:6d} spp in {ms:6.0f} ms: relative MSE {errors[integrator]:.3e}")
    print(f"        the reference's own noise, taken out: {reference_noise:.3e}")
    check(errors["lvc"] < errors["path"], f"lvc renders the caustic with {errors['path'] / max(errors['lvc'], 1e-12):.0f} times less error than the path tracer in the same time")
    print(f"        bdpt's error is {errors['bdpt'] / max(errors['path'], 1e-12):.2f} of the path tracer's in the same time (reported, not checked)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    workdir = Path(tempfile.mkdtemp(prefix="bidirectional_check_"))
    convergence(workdir, args.quick)
    strategies(workdir, args.quick)
    outputs(workdir)
    spectral(workdir, args.quick)
    caustic(workdir, args.quick)
    if FAILURES:
        print(f"bidirectional_check: {len(FAILURES)} check(s) failed")
        return 1
    print("bidirectional_check: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
