"""Renders the verification scenes of docs/plans/completed/pbrt-light-types.md and docs/plans/completed/pbrt-textures.md
(tools/verify/light_texture_scenes.py writes them) and checks what each plan's Verification section asks:

  goniometric   a goniometric light of an isotropic IES file renders as a point light of the same intensity
  projection    a white projection light lights a wall as a point light does inside its frustum, and nothing outside
  one-sided     a one-sided emitter seen from behind is black and lights nothing on that side, under every strategy
  photometric   1000 lm from a point light and from a smoothstep spot inside a diffuse sphere: an integrating sphere's
                wall away from the light shows rho^2 Phi / (pi A (1 - rho)), and everywhere rho Phi / (pi A (1 - rho)) for
                the point light, which sees all of it
  filtering     a checkered plane to the horizon at 1 spp: trilinear and anisotropic filtering much nearer the converged
                image than point and bilinear sampling of the finest level, anisotropic nearest
  reflection    a mirror sphere reflecting the plane at 1 spp: ray cones keep the reflection filtered
  emitter       a checker-textured emissive quad: light sampling and BSDF sampling light the floor the same

Run:  python3 tools/verify/light_texture_check.py [--quick]
"""

import argparse
import copy
import json
import math
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402

SCENES = vkscene.TEST_SCENES / "lights_and_textures"
failures = []


def check(ok, message):
    print(("  ok   " if ok else "  FAIL ") + message)
    if not ok:
        failures.append(message)


def render(name, samples, *, seed=1, edit=None):
    """Renders a scene of assets/scenes/tests/lights_and_textures, after `edit` changes its payload's extras when given (written beside it in a
    temporary folder, its relative paths made absolute first)"""
    path = SCENES / name
    if edit is None:
        image, _ = vkscene.run_render(path, samples, seed=seed)
        return image
    scene = json.loads(path.read_text())
    extras = scene["scenes"][0]["extras"]
    for texture in extras.get("textures", []):
        texture["path"] = str((SCENES / texture["path"]).resolve())
    for light in extras.get("lights", []):
        if light.get("image"):
            light["image"] = str((SCENES / light["image"]).resolve())
    edit(extras)
    image, _ = vkscene.run_render(scene, samples, seed=seed)
    return image


def luminance(image):
    return image @ np.array([0.2126, 0.7152, 0.0722])


def goniometric(samples):
    print("goniometric: an isotropic IES file against a point light of its 5 W/sr")
    point = luminance(render("lt_point_reference.gltf", samples))
    gonio = luminance(render("lt_goniometric.gltf", samples))
    ratio = gonio.mean() / point.mean()
    worst = np.abs(gonio - point).max() / max(point.max(), 1e-9)
    check(abs(ratio - 1) < 1e-3 and worst < 0.02, f"mean ratio {ratio:.5f}, worst difference {worst:.4f} of the brightest pixel")


def projection(samples):
    print("projection: a white 90 degree projection against a point light of the same intensity")
    projected = luminance(render("lt_projection.gltf", samples))
    point = luminance(render("lt_projection_point.gltf", samples))
    inside = projected > 1e-6
    # an edge pixel is partly in the frustum; the comparison is over the pixels wholly inside it
    core = inside & np.roll(inside, 1, 0) & np.roll(inside, -1, 0) & np.roll(inside, 1, 1) & np.roll(inside, -1, 1)
    ratio = np.median(projected[core] / point[core]) if core.any() else 0.0
    check(abs(ratio - 1) < 0.01, f"inside the frustum ({core.sum()} pixels): median ratio to the point light {ratio:.4f}")
    outside = ~inside & (point > 1e-6)
    check(outside.sum() > 0.2 * inside.size and projected[~inside].max() == 0.0, f"outside it ({outside.sum()} lit by the point light): nothing")
    # 2:1, 90 degrees across its shorter side: the frustum is 2 x 1 tan(45) at the wall's distance (2 m) and the camera
    # sees 2.5 + 2 m away with 90 degrees vertically
    rows = np.nonzero(inside.any(axis=1))[0]
    cols = np.nonzero(inside.any(axis=0))[0]
    aspect = (cols.max() - cols.min() + 1) / (rows.max() - rows.min() + 1)
    check(abs(aspect - 2.0) < 0.05, f"the lit patch is {aspect:.3f} times as wide as it is tall (2 expected)")


def one_sided(samples):
    print("one-sided: an emissive quad seen from behind, under each direct-lighting strategy")
    for strategy in ("bsdf", "light", "mis_power"):
        image = luminance(render("lt_one_sided.gltf", samples, edit=lambda e, s=strategy: e.update({"kernels": {"06": s}})))
        h, w = image.shape
        near = image[int(0.8 * h):, :]
        far = image[int(0.45 * h):int(0.55 * h), :]
        panel = image[int(0.3 * h):int(0.42 * h), int(0.42 * w):int(0.58 * w)]
        check(near.max() == 0.0 and panel.max() == 0.0 and far.mean() > 1e-3,
              f"{strategy}: the floor on its back side {near.max():.3g}, its back {panel.max():.3g}, the floor it faces {far.mean():.4f}")


def photometric(samples):
    print("photometric: 1000 lm inside an integrating sphere of radius 1 and albedo 0.5")
    flux = 1000.0 / 683.0
    area = 4 * math.pi
    rho = 0.5
    expected_point = rho * flux / (math.pi * area * (1 - rho))
    expected_away = rho * rho * flux / (math.pi * area * (1 - rho))
    point = luminance(render("lt_sphere_point.gltf", samples)).mean()
    spot = luminance(render("lt_sphere_spot.gltf", samples)).mean()
    check(abs(point / expected_point - 1) < 0.02, f"point light: {point:.5f} against {expected_point:.5f}")
    check(abs(spot / expected_away - 1) < 0.02, f"spot light, the wall it does not face: {spot:.5f} against {expected_away:.5f}")


def aliasing(name, region, reference_samples):
    """Per filter, the 1 spp image's noise in `region` (the difference between two seeds, which is what aliasing is at
    one sample a pixel) and its error against the converged image"""
    reference = render(name, reference_samples, edit=lambda e: e["render"].update({"textureFilter": "bilinear"}))
    noise = {}
    error = {}
    for mode in ("point", "bilinear", "trilinear", "anisotropic"):
        a = render(name, 1, seed=7, edit=lambda e, m=mode: e["render"].update({"textureFilter": m}))[region]
        b = render(name, 1, seed=8, edit=lambda e, m=mode: e["render"].update({"textureFilter": m}))[region]
        noise[mode] = float(np.sqrt(np.mean((a - b) ** 2) / 2))
        error[mode] = float(np.sqrt(np.mean((a - reference[region]) ** 2)))
    print("    noise at 1 spp: " + ", ".join(f"{m} {e:.4f}" for m, e in noise.items()))
    print("    error against the converged image: " + ", ".join(f"{m} {e:.4f}" for m, e in error.items()))
    return noise, error


def filtering(reference_samples):
    print("filtering: a checkered plane to the horizon at 1 spp (PBR 4ed Figure 10.1)")
    noise, error = aliasing("tx_checker_plane.gltf", (slice(84, 124), slice(None)), reference_samples)
    check(noise["trilinear"] < 0.25 * noise["bilinear"] and noise["anisotropic"] < 0.5 * noise["bilinear"], "ray cones take the far plane's aliasing away")
    check(error["anisotropic"] < error["trilinear"], "anisotropic filtering is sharper than trilinear: nearer the converged image")


def reflection(reference_samples):
    print("reflection: a mirror sphere reflecting the plane at 1 spp")
    noise, _ = aliasing("tx_mirror_checker.gltf", (slice(75, 225), slice(75, 225)), reference_samples)
    check(noise["trilinear"] < 0.5 * noise["bilinear"], "ray cones carried through the mirror keep its reflection filtered")


def emitter(samples):
    print("emitter: a checker-textured emissive quad, by light sampling and by BSDF sampling")
    means = {}
    for strategy in ("bsdf", "light", "mis_power"):
        image = luminance(render("tx_textured_emitter.gltf", samples, edit=lambda e, s=strategy: e.update({"kernels": {"06": s}})))
        means[strategy] = image[int(0.6 * image.shape[0]):, :].mean()
    spread = max(means.values()) / min(means.values()) - 1
    check(spread < 0.02, "the floor below it: " + ", ".join(f"{s} {m:.4f}" for s, m in means.items()))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true", help="fewer samples")
    args = parser.parse_args()
    scale = 0.25 if args.quick else 1.0
    goniometric(int(64 * scale))
    projection(16)
    one_sided(int(64 * scale))
    photometric(int(1024 * scale))
    filtering(int(1024 * scale))
    reflection(int(1024 * scale))
    emitter(int(1024 * scale))
    if failures:
        print(f"light_texture_check: {len(failures)} check(s) FAILED")
        return 1
    print("light_texture_check: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
