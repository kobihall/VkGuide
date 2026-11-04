"""Renders the laser sources' verification scenes (tools/verify/laser_scenes.py) and checks them against the closed forms and
against the wave reference (tools/verify/wave_optics.py), which propagates the same beam by diffraction.

Every screen is measured twice. Its power, centroid and second-moment radii are checked against w(z) and the power the
beam carries. Its whole irradiance is compared, pixel by pixel, with the wave reference's at the same plane: in free
space the renderer's rays are the beam's Wigner function, so the two must agree to within the rays' noise. Past a mask
they need not, and the check reports how they differ (laser_iris): the rays clip the beam and carry on as rays, the waves
diffract.

  python3 tools/verify/laser_check.py [--quick] [--out dir]

--quick renders a quarter of the samples. --out keeps each screen's render, the wave reference's irradiance, their
difference and (where matplotlib is importable) a plot of their profiles. Exit 0 when every check passes.
"""

import argparse
import json
import math
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import beam_probe as bp  # noqa: E402
import laser_scenes as ls  # noqa: E402
import vkscene  # noqa: E402
import wave_optics as wo  # noqa: E402

failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)


def render(scene, samples, out, name):
    image, dropped = vkscene.run_render(scene, samples, workdir=out / name)
    return image


def wave_irradiance(beam, probe, z, masks=()):
    """The wave reference's irradiance at the probe's pixels: closed form in free space, the angular spectrum past masks.
    The probe's u is the beam's -x (the camera looks along +z with +y up) and v its y"""
    uu, vv = probe.coordinates()
    if not masks:
        ix = wo.beam_intensity_1d(beam.x, beam.wavelength, -uu[0], z)
        iy = wo.beam_intensity_1d(beam.y, beam.wavelength, vv[:, 0], z)
        return beam.power * np.outer(iy, ix)
    extent = 2.4 * max(probe.half_u, probe.half_v)
    grid = wo.Grid(1024, extent)
    result = wo.propagate(beam, grid, [z], masks=masks)
    return wo.resample(result.intensity[0], grid, -uu[0], vv[:, 0])


# the side of the blocks of pixels the rays and the waves are compared over: a light-traced screen is a histogram of
# where its paths landed, whose noise blocks of 8 x 8 pixels average away while keeping any difference in shape
BLOCK = 8


def blocks(image):
    h, w = image.shape
    return image[: h - h % BLOCK, : w - w % BLOCK].reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK).sum(axis=(1, 3))


def compare(name, measured, wave, probe, out):
    """The rays' irradiance against the waves': the share of the power where they differ (half the L1 distance) over
    blocks of BLOCK x BLOCK pixels. Saves both and their difference when `out` is kept"""
    difference = 0.5 * np.abs(blocks(measured.irradiance) - blocks(wave)).sum() / max(blocks(wave).sum(), 1e-30)
    peak = max(wave.max(), measured.irradiance.max())
    if out is not None:
        folder = out / name
        folder.mkdir(parents=True, exist_ok=True)
        np.save(folder / "rays.npy", measured.irradiance)
        np.save(folder / "waves.npy", wave)
        gray = lambda a: np.repeat(np.clip(a / peak, 0, 1)[..., None], 3, axis=2)
        vkscene.save_png(folder / "rays.png", gray(measured.irradiance))
        vkscene.save_png(folder / "waves.png", gray(wave))
        signed = (measured.irradiance - wave) / peak
        vkscene.save_png(folder / "difference.png", np.stack([np.clip(signed, 0, 1), np.zeros_like(signed), np.clip(-signed, 0, 1)], axis=2) * 4.0)
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            uu, vv = probe.coordinates()
            row = probe.height // 2
            fig, ax = plt.subplots(figsize=(7, 4))
            ax.plot(uu[row] * 1e3, measured.irradiance[row], label="rays (renderer)", lw=1)
            ax.plot(uu[row] * 1e3, wave[row], label="waves (wave_optics.py)", lw=1)
            ax.set_xlabel("u (mm)")
            ax.set_ylabel("irradiance (W/m^2)")
            ax.set_title(name)
            ax.legend()
            fig.tight_layout()
            fig.savefig(folder / "profile.png", dpi=120)
            plt.close(fig)
        except ImportError:
            pass
    return difference


def screen_case(name, build, beam, planes, samples, out, keep, *, masks=(), radius_tolerance=0.01, profile_tolerance=0.02, check_profile=True):
    """Renders a screen at each plane into `out`: radii against w(z), the irradiance against the wave reference's, the
    comparison saved in `keep` when it is given"""
    rows = []
    for z in planes:
        probe, scene = build(z=z, samples=samples)
        measured = bp.measure(render(scene, samples, out, f"{name}_{z:g}"), probe)
        wave = wave_irradiance(beam, probe, z, masks)
        difference = compare(f"{name}_{z:g}", measured, wave, probe, keep)
        _, _, wave_radius = wo.profile_moments(wave.sum(axis=0), probe.coordinates()[0][0])
        _, _, wave_radius_v = wo.profile_moments(wave.sum(axis=1), probe.coordinates()[1][:, 0])
        rows.append((z, measured, (wave_radius, wave_radius_v), difference))
        if not masks:
            wx = beam.x.radius(z, beam.wavelength)
            wy = beam.y.radius(z, beam.wavelength)
            worst = max(abs(measured.radius[0] / wx - 1.0), abs(measured.radius[1] / wy - 1.0))
            check(worst < radius_tolerance, f"{name} at {z:g} m: radii {measured.radius[0] * 1e3:.4f} x {measured.radius[1] * 1e3:.4f} mm, "
                  f"w(z) {wx * 1e3:.4f} x {wy * 1e3:.4f} mm ({100 * worst:.2f} %)")
        if check_profile:
            check(difference < profile_tolerance, f"{name} at {z:g} m: the rays' irradiance and the waves' differ over {100 * difference:.2f} % of the power ({BLOCK} x {BLOCK} pixel blocks)")
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="laser_check_"))
    keep = args.out
    spp = 16 if args.quick else 64
    expected = ls.expected()

    print("wave reference self-test")
    check(wo.self_test(verbose=False) == 0, "wave_optics.py matches its closed forms")

    print("laser_power: HNL050LB on a screen 1 m out")
    probe, scene = ls.laser_power(samples=spp)
    measured = bp.measure(render(scene, spp, out, "laser_power"), probe)
    e = expected["laser_power"]
    check(abs(measured.power / e["power"] - 1.0) < e["tolerance"]["power"], f"power {measured.power * 1e3:.5f} mW of {e['power'] * 1e3:g} mW")
    check(abs(measured.centroid[0]) < 0.02 * e["radius"] and abs(measured.centroid[1]) < 0.02 * e["radius"], f"centroid ({measured.centroid[0] * 1e6:.2f}, {measured.centroid[1] * 1e6:.2f}) um from the axis")
    screen_case("laser_power", ls.laser_power, ls.HNL, [1.0], spp, out, keep)

    print("laser_m2: M^2 = 2, its waist 30 mm inside the housing")
    screen_case("laser_m2", ls.laser_m2, ls.M2_BEAM, expected["laser_m2"]["planes"], spp, out, keep)

    print("laser_diode: an astigmatic beam")
    screen_case("laser_diode", ls.laser_diode, ls.DIODE_BEAM, expected["laser_diode"]["planes"], spp, out, keep)

    print("laser_into_aperture: looking into the laser from 0.5 m")
    e = expected["laser_into_aperture"]
    half = math.tan(math.radians(bp.MIN_FOV_DEGREES / 2.0))
    for integrator in ("path", "light"):
        image = render(ls.laser_into_aperture(integrator=integrator, samples=spp), spp, out, f"into_{integrator}")
        pixel = (2.0 * half / image.shape[1]) ** 2
        irradiance = image.mean(axis=2).sum() * pixel
        lum = image.mean(axis=2)
        n = lum.shape[0]
        angle = ((np.arange(n) + 0.5) / n * 2.0 - 1.0) * half
        _, _, spot = wo.profile_moments(lum.sum(axis=0), angle)
        check(abs(irradiance / e["irradiance"] - 1.0) < e["tolerance"]["irradiance"], f"{integrator}: irradiance {irradiance:.2f} W/m^2, 2 P / (pi w^2) = {e['irradiance']:.2f}")
        check(abs(spot / e["spot_half_angle"] - 1.0) < 0.02, f"{integrator}: the spot's half angle {spot * 1e3:.4f} mrad, w0 theta / w = {e['spot_half_angle'] * 1e3:.4f} mrad")
    off = render(ls.laser_into_aperture(off_axis=5.0 * ls.HNL.x.radius(0.5, ls.HNL.wavelength), integrator="path", samples=spp), spp, out, "into_off_axis")
    off_irradiance = off.mean(axis=2).sum() * (2.0 * half / off.shape[1]) ** 2
    check(off_irradiance < 1e-9 * e["irradiance"], f"5 radii off the axis: irradiance {off_irradiance:.3g} W/m^2")

    print("laser_spectral: the power from the luminance")
    e = expected["laser_spectral"]
    ybar = bp.cie_y(632.8)
    luminance = (0.2126, 0.7152, 0.0722)
    probe, scene = ls.laser_spectral("hero", samples=spp)
    hero = bp.measure(render(scene, spp, out, "spectral_hero"), probe, luminance, bp.CIE_Y_INTEGRAL / ybar)
    check(abs(hero.power / e["power"] - 1.0) < e["tolerance"]["power"], f"hero wavelengths: {hero.power * 1e3:.5f} mW of {e['power'] * 1e3:g} mW")
    probe, scene = ls.laser_spectral("monochromatic", samples=spp)
    mono_image = render(scene, spp, out, "spectral_mono")
    # the line's density at its center, which the monochromatic mode reads: a Gaussian 0.002 nm across at half maximum
    peak = 2.0 * math.sqrt(math.log(2.0) / math.pi) / 0.002
    mono = bp.measure(mono_image, probe, scale=1.0 / peak)
    check(abs(mono.power / e["power"] - 1.0) < e["tolerance"]["power"], f"monochromatic at 632.8 nm: {mono.power * 1e3:.5f} mW of {e['power'] * 1e3:g} mW")

    print("laser_iris: a 1 mm iris 0.2 m out")
    e = expected["laser_iris"]
    mask = wo.circular_aperture(ls.IRIS_Z, ls.IRIS_OPENING / 2.0)
    rows = screen_case("laser_iris", ls.laser_iris, ls.HNL, [ls.IRIS_Z + 0.005], spp, out, keep, masks=[mask], check_profile=True)
    z, measured, _, _ = rows[0]
    check(abs(measured.power / e["transmitted"] - 1.0) < e["tolerance"]["transmitted"], f"just past the iris: {measured.power * 1e3:.4f} mW, 1 - exp(-2 a^2 / w^2) of the beam = {e['transmitted'] * 1e3:.4f} mW")
    rows = screen_case("laser_iris", ls.laser_iris, ls.HNL, [2.0], spp, out, keep, masks=[mask], check_profile=False)
    z, measured, wave_radii, difference = rows[0]
    print(f"  2 m out, where the rays and the waves part: radii {measured.radius[0] * 1e3:.3f} mm (rays) and {wave_radii[0] * 1e3:.3f} mm (waves), "
          f"peaks {measured.irradiance.max():.4g} and {wave_irradiance(ls.HNL, ls.laser_iris(z=2.0)[0], 2.0, [mask]).max():.4g} W/m^2, "
          f"{100 * difference:.1f} % of the power differs")

    print()
    if failures:
        print(f"FAIL ({len(failures)})")
        for f in failures:
            print("  " + f)
    else:
        print("PASS")
    if keep:
        print(f"renders and comparisons in {keep}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
