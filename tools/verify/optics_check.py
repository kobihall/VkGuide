"""Renders the optical element scenes (tools/verify/optics_scenes.py, assets/scenes/tests/optics/optics_*.gltf) and checks what each
must show. Exact RGB, and the spectral builds where the answer does not depend on the wavelength:

  optics_furnace     a lossless lens in a white furnace: 1 everywhere it is polished (the ground edge loses what a
                     single-scattering rough dielectric loses, and is left out)
  optics_nd_filter   OD 1 through ideal AR faces: 0.1 through the filter, 1 around it
  optics_cube_split  an ideal 50:50 cube: half the red emitter behind it, half the green one beside it
  optics_lens_focus  the focused spot is brighter than the screen around it by the ratio the aperture gives
  optics_prism       the spectral render spreads the slit into colors: red and blue at opposite ends

    python3 tools/verify/optics_check.py [--quick]
"""

import argparse
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402

SCENES = vkscene.TEST_SCENES / "optics"
failures = []


def check(ok, message):
    print(("  ok   " if ok else "  FAIL ") + message)
    if not ok:
        failures.append(message)


def scene_with(name, edit):
    """A copy of a saved scene with `edit` applied to its payload, written to a temporary file."""
    data = json.loads((SCENES / f"{name}.gltf").read_text())
    edit(data["scenes"][0]["extras"])
    path = Path(tempfile.mkdtemp(prefix="vkoptics_")) / f"{name}.gltf"
    path.write_text(json.dumps(data))
    return path


def spectral(extras, representation, lanes=4):
    extras["render"]["spectral"] = {"representation": representation, "lanes": lanes}


def polished_edge(extras):
    extras["optics"][0]["materials"]["edge"] = {"type": "optical_interface", "coating": "uncoated", "finish": 0}


def disk(image, radius_fraction):
    h, w, _ = image.shape
    y, x = np.mgrid[0:h, 0:w]
    r = np.hypot(x - (w - 1) / 2, y - (h - 1) / 2) / (min(w, h) / 2)
    return r < radius_fraction


def region_ratio(image, inner=0.5, outer=1.2):
    """The mean of the middle of the view against the mean of its corners, per channel: a spectral build films a white
    background to a little under or over 1, which the optic must keep as it is"""
    return image[disk(image, inner)].mean(axis=0) / image[~disk(image, outer)].mean(axis=0)


def furnace(samples):
    print("furnace: a lossless lens in a white furnace")
    for label, edits in [("exact RGB", []), ("hero 4", [lambda e: spectral(e, "hero", 4)]), ("bands 8", [lambda e: spectral(e, "bands", 8)]),
                         ("monochromatic", [lambda e: spectral(e, "monochromatic", 1)]), ("exact RGB, exact coatings", [lambda e: e["render"].update({"optics": {"exactCoatings": True}})])]:
        path = scene_with("optics_furnace", lambda e: [polished_edge(e)] + [f(e) for f in edits])
        image, _ = vkscene.run_render(path, samples)
        ratio = region_ratio(image)
        # a spectral path keeps one lane at each dispersive refraction, which is noisier than exact RGB's three channels
        tolerance = 1e-2 if ("hero" in label or "bands" in label) else 5e-3
        check(np.all(np.abs(ratio - 1.0) < tolerance), f"{label}: through the lens against the background {np.round(ratio, 4)}")


def nd_filter(samples):
    print("ND filter: optical density 1 through ideal AR faces")
    for label, edits in [("exact RGB", []), ("hero 4", [lambda e: spectral(e, "hero", 4)])]:
        path = scene_with("optics_nd_filter", lambda e: [f(e) for f in edits])
        image, _ = vkscene.run_render(path, samples)
        ratio = region_ratio(image)
        check(np.all(np.abs(ratio - 0.1) < 2e-3), f"{label}: through against around {np.round(ratio, 4)}, expected 0.1")


def exact_coatings(samples):
    print("coatings: an N-BK7 window in front of a red emitter, bare and -A coated, by the table and by the transfer matrix")
    results = {}
    for coating, exact in (("uncoated", False), ("thorlabs_A", False), ("thorlabs_A", True)):
        def edit(e, coating=coating, exact=exact):
            # the cube scene's emitters, the cube swapped for a window: only what the window transmits reaches the camera
            e["optics"] = [{"type": "circular_filter", "name": "W1", "size": "1in", "params": {"thickness": 0.003, "glass": "N-BK7"},
                            "position": [0.0, 0.0, 0.0], "orientation": [0.0, 0.0, 0.0, 1.0],
                            "materials": {"front": {"type": "optical_interface", "coating": coating, "finish": 0},
                                          "back": {"type": "optical_interface", "coating": coating, "finish": 0}}}]
            e["render"]["optics"] = {"exactCoatings": exact}
        image, _ = vkscene.run_render(scene_with("optics_cube_split", edit), samples)
        results[(coating, exact)] = image[disk(image, 0.3)].mean(axis=0)[0]
    bare = results[("uncoated", False)]
    table = results[("thorlabs_A", False)]
    exact = results[("thorlabs_A", True)]
    # bare N-BK7 at 630 nm: two faces of R = 0.0420, with their inter-reflections, and the 3 mm of glass
    expected_bare = (1 - 0.0420) ** 2 / (1 - 0.0420 ** 2)
    check(abs(bare - expected_bare) < 0.01, f"bare window transmits {bare:.4f}, Fresnel {expected_bare:.4f} (less the glass's absorption)")
    check(table > bare + 0.06 and abs(table - exact) < 3e-3, f"-A coated: table {table:.4f}, transfer matrix {exact:.4f}")


def scatter(samples):
    print("surface scatter: an ABg lobe (TIS 0.05, B 0.01) conserves energy, and light sampling agrees with BSDF sampling")
    def scattering(e):
        polished_edge(e)
        lobe = {"type": "optical_interface", "coating": "uncoated", "finish": 0, "tis": 0.05, "abgB": 0.01, "abgG": 2.0}
        e["optics"][0]["materials"]["front"] = lobe
        e["optics"][0]["materials"]["back"] = dict(lobe)
    image, _ = vkscene.run_render(scene_with("optics_furnace", scattering), samples)
    ratio = region_ratio(image)
    check(np.all(np.abs(ratio - 1.0) < 0.01), f"furnace: through the scattering lens against the background {np.round(ratio, 4)}")
    # a window lit from behind and to the side by a small light the camera cannot see through it: only the lobe brings it
    means = {}
    for strategy in ("bsdf", "mis_power"):
        def edit(e, strategy=strategy):
            e["optics"] = [{"type": "circular_filter", "name": "W1", "size": "1in", "params": {"thickness": 0.003, "glass": "F_SILICA"},
                            "position": [0.0, 0.0, 0.0], "orientation": [0.0, 0.0, 0.0, 1.0],
                            "materials": {"front": dict(lobe_material), "back": dict(lobe_material)}}]
            e["geometry"] = [vkscene.sphere("lamp", (0.03, 0.0, 0.08), 0.01, 0)]
            e["materials"] = [{"name": "lamp", "type": "emissive", "albedo": [1.0, 1.0, 1.0], "strength": 50.0}]
            e["kernels"] = {"06": strategy}
        lobe_material = {"type": "optical_interface", "coating": "uncoated", "finish": 0, "tis": 0.05, "abgB": 0.01, "abgG": 2.0}
        image, _ = vkscene.run_render(scene_with("optics_cube_split", edit), samples * 4)
        means[strategy] = image[disk(image, 0.3)].mean()
    difference = abs(means["bsdf"] - means["mis_power"]) / max(means["bsdf"], 1e-9)
    check(means["bsdf"] > 0.0 and difference < 0.05, f"the lobe's light: BSDF sampling {means['bsdf']:.4g}, MIS {means['mis_power']:.4g} ({difference:.1%} apart)")


def cube_split(samples):
    print("cube: an ideal 50:50 splitter")
    for label, edits in [("exact RGB", []), ("hero 4", [lambda e: spectral(e, "hero", 4)])]:
        path = scene_with("optics_cube_split", lambda e: [f(e) for f in edits])
        image, _ = vkscene.run_render(path, samples)
        middle = image[disk(image, 0.3)].mean(axis=0)
        check(abs(middle[0] - 0.5) < 0.01 and abs(middle[1] - 0.5) < 0.01 and middle[2] < 0.01, f"{label}: through the cube {np.round(middle, 4)}, expected (0.5, 0.5, 0)")


def lens_focus(samples):
    print("lens: a plano-convex lens images a bright square onto the screen")
    image, _ = vkscene.run_render(SCENES / "optics_lens_focus.gltf", samples)
    luminance = image @ np.array([0.2126, 0.7152, 0.0722])
    h, w = luminance.shape
    middle = luminance[h // 2 - 6:h // 2 + 6, w // 2 - 6:w // 2 + 6].mean()
    edge = np.concatenate([luminance[h // 2 - 6:h // 2 + 6, :12].ravel(), luminance[h // 2 - 6:h // 2 + 6, -12:].ravel()]).mean()
    vkscene.save_png(Path(tempfile.gettempdir()) / "optics_lens_focus.png", image, 1.0 / max(np.percentile(luminance, 99), 1e-6))
    check(middle > 5.0 * max(edge, 1e-6), f"the image on the screen {middle:.3g} against the screen beyond it {edge:.3g}")


def prism(samples):
    print("prism: F2 spreads a white slit into a spectrum (hero wavelengths)")
    image, _ = vkscene.run_render(SCENES / "optics_prism.gltf", samples)
    total = image.sum(axis=2)
    if total.max() <= 0.0:
        check(False, "the slit is seen through the prism")
        return
    rows = np.arange(image.shape[0])[:, None]
    red = (rows * image[:, :, 0]).sum() / image[:, :, 0].sum()
    blue = (rows * image[:, :, 2]).sum() / image[:, :, 2].sum()
    vkscene.save_png(Path(tempfile.gettempdir()) / "optics_prism.png", image, 1.0 / max(np.percentile(image, 99.5), 1e-6))
    check(abs(red - blue) > 3.0, f"red centroid row {red:.1f}, blue {blue:.1f}: the slit spread by wavelength")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true", help="fewer samples")
    args = parser.parse_args()
    scale = 0.25 if args.quick else 1.0
    furnace(int(256 * scale))
    nd_filter(int(64 * scale))
    exact_coatings(int(64 * scale))
    scatter(int(256 * scale))
    cube_split(int(256 * scale))
    lens_focus(int(256 * scale))
    prism(int(512 * scale))
    if failures:
        print(f"optics_check: {len(failures)} check(s) FAILED")
        return 1
    print("optics_check: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
