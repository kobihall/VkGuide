"""Renders the detectors' verification scenes (tools/verify/detector_scenes.py) with light tracing and checks what their
detectors read (docs/plans/completed/physoptics-detectors.md): the power a meter catches, a 50:50 split and its noise
against the standard error the meters report, a profiler's second-moment radius at a lens's focus against ABCD, a card's
spot on its own film and in a camera's picture of it (the screen check beam_probe.py made before detectors), a diffuse
wall read from both sides against a Lambertian spot's closed form, a spectrometer's two lines, and a beam dump.

  python3 tools/verify/detector_check.py [--quick] [--out dir]

--quick renders a quarter of the paths and 8 seeds instead of 20. Exit 0 when every check passes.
"""

import argparse
import math
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import beam_probe as bp  # noqa: E402
import detector_scenes as ds  # noqa: E402
import vkscene  # noqa: E402

failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)


def read(scene, samples, out, name, seed=None):
    readings, image, data = vkscene.run_detectors(scene, samples, workdir=out / name, seed=seed)
    return readings, image, data


def mw(watts):
    return f"{watts * 1e3:.5f} mW"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="detector_check_"))
    spp = 16 if args.quick else 64
    seeds = 8 if args.quick else 20
    expected = ds.expected()

    print("detector_power_meter: HNL050LB on a power meter 1 m out")
    e = expected["detector_power_meter"]
    readings, _, data = read(ds.detector_power_meter(samples=spp), spp, out, "power_meter")
    m = readings["meter"][0]
    check(abs(m["power"] / e["power"] - 1.0) < e["tolerance"]["power"], f"{mw(m['power'])} of {mw(e['power'])} after {data['pathsEmitted']:.0f} paths")
    check(m["pathsReached"] == data["pathsEmitted"], f"every path reached it: {m['pathsReached']:.0f} of {data['pathsEmitted']:.0f}")
    check(m["powerError"] < 1e-6 * e["power"], f"every path carries the laser's power, so the error is {m['powerError']:.3g} W")

    print("detector_splitter: an ideal 50:50 cube between two meters")
    e = expected["detector_splitter"]
    readings, _, data = read(ds.detector_splitter(samples=spp), spp, out, "splitter")
    without, _, _ = read(ds.detector_splitter(samples=spp, probe=False), spp, out, "splitter_no_probe")
    for name in ("transmitted", "reflected"):
        r = readings[name][0]
        sigmas = abs(r["power"] - e[name]) / max(r["powerError"], 1e-30)
        # a binomial split: each path goes one way with probability 1/2, so the error of a half is P T / (2 sqrt(N))
        binomial = 0.5 * ds.HNL.power * e["glass"] / math.sqrt(data["pathsEmitted"])
        check(sigmas < e["tolerance"]["sigmas"], f"{name}: {mw(r['power'])} +- {r['powerError'] * 1e6:.3f} uW, {sigmas:.2f} sigma from {mw(e[name])}")
        check(0.7 < r["powerError"] / binomial < 1.4, f"{name}: its error {r['powerError'] * 1e6:.3f} uW against the binomial {binomial * 1e6:.3f} uW")
        change = abs(without[name][0]["power"] / r["power"] - 1.0)
        check(change < e["tolerance"]["probe"], f"{name}: the probe plane before the cube changes the reading by {change:.2e}")
    probe = readings["probe"][0]
    check(abs(probe["power"] / ds.HNL.power - 1.0) < 1e-5, f"the probe plane reads {mw(probe['power'])} of {mw(ds.HNL.power)}")
    print(f"  over {seeds} seeds")
    values = []
    errors = []
    for seed in range(1, seeds + 1):
        r = read(ds.detector_splitter(samples=spp, probe=False), spp, out, f"splitter_seed{seed}", seed=seed)[0]["transmitted"][0]
        values.append(r["power"])
        errors.append(r["powerError"])
    spread = float(np.std(values, ddof=1))
    reported = float(np.mean(errors))
    lo, hi = e["tolerance"]["spread"]
    check(lo < spread / reported < hi, f"the readings spread by {spread * 1e6:.3f} uW over {seeds} seeds; the meters report {reported * 1e6:.3f} uW")
    check(abs(np.mean(values) - e["transmitted"]) < 4.0 * spread / math.sqrt(seeds), f"their mean {mw(np.mean(values))} of {mw(e['transmitted'])}")

    print("detector_profiler_focus: a beam profiler at an f = 500 mm lens's focus")
    e = expected["detector_profiler_focus"]
    readings, _, _ = read(ds.detector_profiler_focus(samples=spp), spp, out, "profiler_focus")
    r = readings["profiler"][0]
    radii = [0.5 * d for d in r["diameter"]]
    worst = max(abs(x / e["radius"] - 1.0) for x in radii)
    check(worst < e["tolerance"]["radius"], f"radii {radii[0] * 1e6:.2f} x {radii[1] * 1e6:.2f} um, ABCD's waist {e['radius'] * 1e6:.2f} um ({100 * worst:.2f} %)")
    check(abs(r["power"] / ds.HNL.power - 1.0) < 0.005, f"it absorbs {mw(r['power'])}")

    print("detector_card: an alignment card 0.8 m out, and a camera's picture of it")
    e = expected["detector_card"]
    probe, scene = ds.detector_card(samples=spp)
    readings, image, _ = read(scene, spp, out, "card")
    card = readings["card"][0]
    check(abs(card["power"] / e["power"] - 1.0) < e["tolerance"]["power"], f"the card's film reads {mw(card['power'])}")
    radii = [0.5 * d for d in card["diameter"]]
    worst = max(abs(x / e["radius"] - 1.0) for x in radii)
    check(worst < e["tolerance"]["radius"], f"the card's film: radii {radii[0] * 1e3:.4f} x {radii[1] * 1e3:.4f} mm, w(0.8 m) {e['radius'] * 1e3:.4f} mm ({100 * worst:.2f} %)")
    seen = bp.measure(image, probe)
    check(abs(seen.power / e["reflected"] - 1.0) < e["tolerance"]["reflected"], f"the camera sees {mw(seen.power)} leave the card, 0.8 of the beam {mw(e['reflected'])}")
    worst = max(abs(x / e["radius"] - 1.0) for x in seen.radius)
    check(worst < e["tolerance"]["radius"], f"the camera's spot: radii {seen.radius[0] * 1e3:.4f} x {seen.radius[1] * 1e3:.4f} mm ({100 * worst:.2f} %)")

    print("detector_diffuse_wall: a diffuse transmission wall read from both sides")
    e = expected["detector_diffuse_wall"]
    wall_spp = 64 if args.quick else 256
    readings, _, data = read(ds.detector_diffuse_wall(samples=wall_spp), wall_spp, out, "diffuse_wall")
    for name in ("reflected", "transmitted"):
        r = readings[name][0]
        sigmas = abs(r["power"] - e[name]) / max(r["powerError"], 1e-30)
        relative = abs(r["power"] / e[name] - 1.0)
        check(sigmas < e["tolerance"]["sigmas"] and relative < e["tolerance"]["relative"],
              f"{name}: {r['power'] * 1e6:.4f} +- {r['powerError'] * 1e6:.4f} uW, a Lambertian spot sends {e[name] * 1e6:.4f} uW ({100 * relative:.2f} %, {sigmas:.2f} sigma)")

    print("detector_spectrometer: a 632.8 nm and a 532 nm laser into a spectrometer")
    e = expected["detector_spectrometer"]
    readings, _, _ = read(ds.detector_spectrometer(samples=spp), spp, out, "spectrometer", seed=None)
    spectrum = readings["spectrometer"][0]["spectrum"]
    wavelength = np.array(spectrum["wavelength"])
    density = np.array(spectrum["wattsPerNm"])
    step = wavelength[1] - wavelength[0]
    for line, power in e["lines"]:
        window = np.abs(wavelength - line) < 1.0
        found = float(wavelength[window][np.argmax(density[window])])
        collected = float(density[window].sum() * step)
        check(abs(found - line) < e["tolerance"]["wavelength"] and abs(collected / power - 1.0) < e["tolerance"]["power"],
              f"the {line:g} nm line: its peak at {found:.3f} nm, {mw(collected)} of {mw(power)}")
    total = float(density.sum() * step)
    check(abs(total / sum(p for _, p in e["lines"]) - 1.0) < e["tolerance"]["power"], f"the whole spectrum: {mw(total)}")

    print("detector_dump: a beam dump, and a meter behind it")
    e = expected["detector_dump"]
    readings, _, _ = read(ds.detector_dump(samples=spp), spp, out, "dump")
    check(abs(readings["dump"][0]["power"] / e["dump"] - 1.0) < e["tolerance"]["power"], f"the dump absorbs {mw(readings['dump'][0]['power'])}")
    check(readings["behind"][0]["power"] == 0.0, f"the meter behind it reads {readings['behind'][0]['power']:.3g} W")

    print("detector_no_camera: the meter as the render target, no camera")
    e = expected["detector_no_camera"]
    readings, _, data = read(ds.detector_no_camera(samples=spp), spp, out, "no_camera")
    m = readings["meter"][0]
    check(abs(m["power"] / e["power"] - 1.0) < e["tolerance"]["power"], f"{mw(m['power'])} of {mw(e['power'])}")
    check(data["pathsEmitted"] == e["paths_per_sample"] * spp, f"{data['pathsEmitted']:.0f} light paths, {e['paths_per_sample']} a sample")

    print("detector_sensor_emitter: a sensor before a glowing square, from its pixels and by light tracing")
    e = expected["detector_sensor_emitter"]
    for integrator in ("path", "light"):
        # camera paths from 64 x 64 pixels: 256 samples are a million of them, as many as light tracing's 4 at 512 x 512
        samples = 256 if integrator == "path" else spp
        readings, _, data = read(ds.detector_sensor_emitter(integrator, samples=samples), samples, out, f"sensor_{integrator}")
        r = readings["sensor"][0]
        check(abs(r["power"] / e["power"] - 1.0) < e["tolerance"]["relative"], f"{integrator}: {r['power'] * 1e6:.4f} uW, the square sends {e['power'] * 1e6:.4f} uW "
              f"({100 * (r['power'] / e['power'] - 1):+.2f} %){', camera paths' if data.get('cameraPaths') else ''}")

    print("detector_sensor_sky: the sensor under a uniform sky")
    e = expected["detector_sensor_sky"]
    readings, _, _ = read(ds.detector_sensor_sky(samples=spp), spp, out, "sensor_sky")
    r, film = readings["sensor"]
    area = r["pixelSize"][0] * r["pixelSize"][1]
    irradiance = film / area
    check(abs(r["power"] / e["power"] - 1.0) < e["tolerance"]["relative"], f"{r['power'] * 1e6:.4f} uW of pi x {ds.SKY} x the sensor's area, {e['power'] * 1e6:.4f} uW")
    check(abs(irradiance.min() / e["irradiance"] - 1.0) < 0.02 and abs(irradiance.max() / e["irradiance"] - 1.0) < 0.02,
          f"every pixel's irradiance {irradiance.min():.4f} to {irradiance.max():.4f} W/m^2, pi x {ds.SKY} = {e['irradiance']:.4f}")

    print()
    if failures:
        print(f"FAIL ({len(failures)})")
        for f in failures:
            print("  " + f)
    else:
        print("PASS")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
