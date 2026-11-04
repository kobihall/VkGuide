"""The geometric scenes of docs/plans/physoptics-verification-scenes.md that a laser and screens can check before detectors
and the beam solver exist: Gaussian beams through real optics, measured by light tracing on white screens
(tools/verify/beam_probe.py) and compared with the closed forms of ABCD optics (the embedded Gaussian, Siegman 1986).

  gaussian_train            HNL050LB, two 45 degree turning mirrors and an N-BK7 f = 100 mm plano-convex lens, 0.5 m of
                            beam before it; screens at seven planes through the focus, fitted for the waist, its position
                            and M^2
  telescope_kepler          f = 100 mm and f = 25 mm plano-convex lenses, afocal, after the HNL050LB: the beam 4 times
                            smaller and 4 times as divergent, on screens 0.1 m and 0.6 m past the second lens
  telescope_galilean        the same with an f = -25 mm plano-concave lens
  oap_focus                 a 2 mm collimated beam on a 90 degree off-axis parabola of 4 in reflected focal length: the
                            focal spot w0' = lambda f / (pi w), fitted through five planes; the spot at the nominal focus
                            with the parabola turned 0.1 and 0.5 degrees, reported
  tilted_spherical_mirror   the 2 mm beam on an R = 200 mm concave mirror at 10, 20 and 45 degrees: the tangential and
                            sagittal foci, R cos(theta) / 2 and R / (2 cos(theta)), each fitted through five planes

Every lens is N-BK7, whose index exact RGB reads at the laser's wavelength (the design wavelength follows the scene's one
laser). The scenes are written to assets/scenes/optics/<name>.gltf with their screen at the nominal plane, and the values
each must show to <name>.expected.json.

  python3 tools/verify/optics_verification.py --write       write the scenes
  python3 tools/verify/optics_verification.py [--quick]     render and check them
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
import vkscene  # noqa: E402

SCENES = vkscene.ROOT / "assets" / "scenes" / "optics"
LAMBDA = 632.8e-9
HNL_WAIST = 0.405e-3
HNL_M2 = math.pi * HNL_WAIST * 0.5e-3 / LAMBDA
Y = 0.1


def nbk7(lam):
    """N-BK7's index (Schott's Sellmeier coefficients), relative to air"""
    l2 = (lam * 1e6) ** 2
    b = (1.03961212, 0.231792344, 1.01046945)
    c = (0.00600069867, 0.0200179144, 103.560653)
    return math.sqrt(1.0 + sum(bi * l2 / (l2 - ci) for bi, ci in zip(b, c)))


N = nbk7(LAMBDA)

failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)


def rel(a, b):
    return abs(a / b - 1.0)


# ---------------------------------------------------------------- parts

def hnl(position, axis):
    return bp.laser("HNL050LB", position, axis)


def wide_laser(position, axis):
    """a collimated 2 mm beam: a waist of 1 mm at the output, M^2 = 1"""
    return bp.laser("wide", position, axis, preset=None, params={"waistRadius": 1e-3, "waistPosition": 0.0, "m2": 1.0})


def turning_mirror(name, position, incoming, outgoing):
    normal = bp.unit(np.asarray(outgoing, float) - np.asarray(incoming, float))
    return bp.free_optic("planar_mirror", name, position, normal, size="1in")


AR = {"type": "optical_interface", "coating": "ideal_ar", "finish": 0}


def plano_lens(name, position, axis, focal_length, diameter, center_thickness):
    """an N-BK7 plano-convex (f > 0) or plano-concave (f < 0) lens, its curved face toward the incoming light, the front
    vertex at `position`: R = |f| (n - 1). Both faces have an ideal AR coating: an uncoated lens's ghosts (two 4 %
    reflections, 0.16 % of the power, defocused far wider than the beam) widen a second-moment radius by about 1 %,
    which a real profiler sees too but which is not what these checks measure"""
    radius = abs(focal_length) * (N - 1.0)
    form = "plano_convex" if focal_length > 0 else "plano_concave"
    return bp.free_optic("spherical_lens", name, position, axis, size="custom",
                         params={"diameter": diameter, "form": form, "r1": radius, "centerThickness": center_thickness, "glass": "N-BK7"},
                         materials={"front": AR, "back": AR}), radius


def lens_matrix(focal_length, center_thickness):
    """the lens from its front vertex to its back vertex"""
    radius = abs(focal_length) * (N - 1.0) * (1.0 if focal_length > 0 else -1.0)
    return bp.chain(bp.refraction(radius, 1.0, N), bp.free_space(center_thickness), bp.refraction(math.inf, N, 1.0))


# ---------------------------------------------------------------- measuring

def measure_plane(name, optics, center, direction, half_field, samples, out, max_distance=0.1, up=(0.0, 1.0, 0.0)):
    p, screen, cam = bp.probe(center, direction, half_field, up=up, max_distance=max_distance)
    scene = bp.scene([screen], [bp.WHITE], optics, cam, p.width, p.height, samples=samples)
    image, _ = vkscene.run_render(scene, samples, workdir=out / name)
    return bp.measure(image, p), scene


def fit_planes(name, optics, origin, direction, distances, half_fields, samples, out, max_distance, axis=None):
    """screens at `distances` along `direction` from `origin`: the radii along u and v, and their fits"""
    rows = []
    nominal = None
    for i, (d, h) in enumerate(zip(distances, half_fields)):
        m, scene = measure_plane(f"{name}_{i}", optics, np.asarray(origin) + d * np.asarray(direction), direction, h, samples, out, max_distance)
        rows.append((d, m.radius[0], m.radius[1], m.power))
        if nominal is None and i == len(distances) // 2:
            nominal = scene
    d = [r[0] for r in rows]
    fit_u = bp.fit_waist(d, [r[1] for r in rows], LAMBDA)
    fit_v = bp.fit_waist(d, [r[2] for r in rows], LAMBDA)
    return rows, fit_u, fit_v, nominal


def planes_around(z0, z_r, multiples=(-2.0, -1.0, 0.0, 1.0, 2.0)):
    return [z0 + k * z_r for k in multiples]


# ---------------------------------------------------------------- the scenes

def gaussian_train(samples, out):
    """laser at the origin along +z; M1 at 0.15 m turns it to +x; M2 0.15 m on turns it back to +z; the lens 0.2 m on"""
    print("gaussian_train: HNL050LB, two turning mirrors, an f = 100 mm lens")
    origin = np.array([0.0, Y, 0.0])
    m1 = origin + [0.0, 0.0, 0.15]
    m2 = m1 + [0.15, 0.0, 0.0]
    lens_front = m2 + [0.0, 0.0, 0.2]
    lens, _ = plano_lens("L1", lens_front, (0.0, 0.0, 1.0), 0.1, 0.0254, 3.6e-3)
    optics = [hnl(origin, (0.0, 0.0, 1.0)), turning_mirror("M1", m1, (0.0, 0.0, 1.0), (1.0, 0.0, 0.0)), turning_mirror("M2", m2, (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)), lens]
    system = lens_matrix(0.1, 3.6e-3)
    w0, z0 = bp.propagate_q(HNL_WAIST, HNL_M2, LAMBDA, 0.5, system)
    z_r = math.pi * w0 ** 2 / (HNL_M2 * LAMBDA)
    back = lens_front + [0.0, 0.0, 3.6e-3]
    distances = [z0 + k * z_r for k in (-3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0)]
    halves = [4.0 * w0 * math.sqrt(1.0 + ((d - z0) / z_r) ** 2) for d in distances]
    rows, fit_u, fit_v, nominal = fit_planes("gaussian_train", optics, back, (0.0, 0.0, 1.0), distances, halves, samples, out, max_distance=0.03)
    expected = {"waist": w0, "waist_position_after_lens": z0, "m2": HNL_M2, "rayleigh_range": z_r,
                "tolerance": {"waist": 0.01, "position": 0.5e-3, "m2": 0.02}, "notes": "the focus after the lens: ABCD of the thick lens, the embedded Gaussian of the HNL050LB"}
    for label, (fw, fz, fm) in (("x", fit_u), ("y", fit_v)):
        check(rel(fw, w0) < 0.01, f"{label}: waist {fw * 1e6:.2f} um, ABCD {w0 * 1e6:.2f} um")
        check(abs(fz - z0) < 0.5e-3, f"{label}: waist {fz * 1e3:.3f} mm past the lens's back vertex, ABCD {z0 * 1e3:.3f} mm")
        check(rel(fm, HNL_M2) < 0.02, f"{label}: M^2 {fm:.4f}, the laser's {HNL_M2:.4f}")
    return nominal, expected


def telescope(name, f2, ct2, samples, out):
    """the HNL050LB 0.3 m before an f = 100 mm lens, then f2, spaced afocally (the system's C = 0)"""
    print(f"{name}: f = 100 mm then f = {f2 * 1e3:g} mm")
    origin = np.array([0.0, Y, 0.0])
    l1_front = origin + [0.0, 0.0, 0.3]
    lens1 = lens_matrix(0.1, 3.6e-3)
    lens2 = lens_matrix(f2, ct2)
    # the gap from the first lens's back vertex to the second's front vertex that makes the pair afocal
    gaps = np.linspace(0.0, 0.2, 200001)
    c = [bp.chain(lens1, bp.free_space(g), lens2)[1, 0] for g in (gaps[0], gaps[-1])]
    lo, hi = 0.0, 0.2
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        cm = bp.chain(lens1, bp.free_space(mid), lens2)[1, 0]
        if (cm > 0) == (c[0] > 0):
            lo = mid
        else:
            hi = mid
    gap = 0.5 * (lo + hi)
    l2_front = l1_front + [0.0, 0.0, 3.6e-3 + gap]
    l1, _ = plano_lens("L1", l1_front, (0.0, 0.0, 1.0), 0.1, 0.0254, 3.6e-3)
    l2, _ = plano_lens("L2", l2_front, (0.0, 0.0, 1.0), f2, 0.0127, ct2)
    optics = [hnl(origin, (0.0, 0.0, 1.0)), l1, l2]
    back = l2_front + [0.0, 0.0, ct2]
    expected = {"gap": gap, "planes_after_second_lens": [0.1, 0.6], "radius": [], "magnification": 0.1 / abs(f2), "tolerance": {"radius": 0.01}}
    measured = []
    for d in (0.1, 0.6):
        system = bp.chain(bp.free_space(0.3), lens1, bp.free_space(gap), lens2, bp.free_space(d))
        w = bp.beam_radius_after(HNL_WAIST, HNL_M2, LAMBDA, 0.0, system)
        expected["radius"].append(w)
        m, scene = measure_plane(f"{name}_{d:g}", optics, back + [0.0, 0.0, d], (0.0, 0.0, 1.0), 4.0 * w, samples, out, max_distance=0.5 * d)
        measured.append(m.radius)
        check(rel(m.radius[0], w) < 0.01 and rel(m.radius[1], w) < 0.01, f"{d:g} m past it: radii {m.radius[0] * 1e3:.4f} x {m.radius[1] * 1e3:.4f} mm, ABCD {w * 1e3:.4f} mm")
    # the input's divergence over the output's, from the growth between the planes, against f1 / |f2|: a divergence ratio,
    # which the second-moment growth gives for a beam far from its waist
    w_in = [bp.beam_radius_after(HNL_WAIST, HNL_M2, LAMBDA, 0.0, bp.free_space(z)) for z in (5.0, 10.0)]
    theta_in = (w_in[1] - w_in[0]) / 5.0
    w_out = [bp.beam_radius_after(HNL_WAIST, HNL_M2, LAMBDA, 0.0, bp.chain(bp.free_space(0.3), lens1, bp.free_space(gap), lens2, bp.free_space(z))) for z in (50.0, 100.0)]
    theta_out = (w_out[1] - w_out[0]) / 50.0
    print(f"  afocal gap {gap * 1e3:.3f} mm; ABCD's angular magnification {theta_out / theta_in:.4f}, f1 / |f2| = {0.1 / abs(f2):g}")
    check(rel(theta_out / theta_in, 0.1 / abs(f2)) < 0.01, "the ABCD system magnifies angles by f1 / |f2|")
    return scene, expected


def oap_focus(samples, out):
    """the 2 mm beam along +z onto a 90 degree OAP 0.3 m out, facing it; the focus 4 in along +x"""
    print("oap_focus: a 2 mm beam on a 90 degree off-axis parabola")
    origin = np.array([0.0, Y, 0.0])
    datum = origin + [0.0, 0.0, 0.3]
    rfl = 4.0 * 0.0254

    def optics_turned(degrees):
        # the parabola's body +z faces the incoming beam (world -z), turned about the vertical by `degrees`
        a = math.radians(degrees)
        z_axis = (math.sin(a), 0.0, -math.cos(a))
        return [wide_laser(origin, (0.0, 0.0, 1.0)), bp.free_optic("off_axis_parabola", "OAP1", datum, z_axis, size="1in")]

    w_in = 1e-3 * math.sqrt(1.0 + (0.3 / (math.pi * 1e-6 / LAMBDA)) ** 2)
    w0, z0 = bp.propagate_q(1e-3, 1.0, LAMBDA, 0.3, bp.mirror(rfl))
    z_r = math.pi * w0 ** 2 / LAMBDA
    distances = planes_around(z0, z_r)
    halves = [4.0 * w0 * math.sqrt(1.0 + ((d - z0) / z_r) ** 2) for d in distances]
    rows, fit_u, fit_v, nominal = fit_planes("oap_focus", optics_turned(0.0), datum, (1.0, 0.0, 0.0), distances, halves, samples, out, max_distance=0.02)
    for label, (fw, fz, fm) in (("tangential", fit_u), ("sagittal", fit_v)):
        check(rel(fw, w0) < 0.01, f"{label}: waist {fw * 1e6:.2f} um, lambda f / (pi w) = {LAMBDA * rfl / (math.pi * w_in) * 1e6:.2f} um, ABCD {w0 * 1e6:.2f} um")
        check(abs(fz - z0) < 0.1e-3, f"{label}: focus {fz * 1e3:.3f} mm from the datum, ABCD {z0 * 1e3:.3f} mm")
    misaligned = {}
    for degrees in (0.1, 0.5):
        # the reflected beam turns by twice the tilt: the screen square to the aligned axis at the nominal focus, which
        # the turned beam crosses off its own focus
        walk = 2.0 * math.radians(degrees) * z0
        m, _ = measure_plane(f"oap_turned_{degrees:g}", optics_turned(degrees), datum + z0 * np.array([1.0, 0.0, 0.0]), (1.0, 0.0, 0.0), max(40.0 * w0, 1.5 * walk), samples, out, max_distance=0.02)
        misaligned[f"{degrees:g}"] = {"radius_u": m.radius[0], "radius_v": m.radius[1], "centroid_u": m.centroid[0]}
        print(f"  turned {degrees:g} deg: spot {m.radius[0] * 1e6:.2f} x {m.radius[1] * 1e6:.2f} um at the nominal focus ({m.radius[0] / w0:.3f} and {m.radius[1] / w0:.3f} of the aligned waist), "
              f"moved {m.centroid[0] * 1e3:.4f} mm across")
    expected = {"waist": w0, "focus_from_datum": z0, "tolerance": {"waist": 0.01, "position": 0.1e-3},
                "misaligned_measured": misaligned, "notes": "aligned: lambda f / (pi w). Misaligned spot growth is recorded, not checked: its reference is OpticStudio's or Optiland's"}
    return nominal, expected


def tilted_mirror(samples, out):
    print("tilted_spherical_mirror: R = 200 mm at 10, 20 and 45 degrees")
    origin = np.array([0.0, Y, 0.0])
    vertex = origin + [0.0, 0.0, 0.3]
    radius = 0.2
    expected = {"angles": [], "tolerance": {"position": 1e-3}, "notes": "tangential focus R cos(theta) / 2, sagittal R / (2 cos(theta)), as Gaussian waists (ABCD)"}
    nominal = None
    for degrees in (10.0, 20.0, 45.0):
        t = math.radians(degrees)
        out_dir = np.array([math.sin(2 * t), 0.0, -math.cos(2 * t)])
        normal = bp.unit(out_dir - np.array([0.0, 0.0, 1.0]))
        optics = [wide_laser(origin, (0.0, 0.0, 1.0)), bp.free_optic("concave_mirror", "CM1", vertex, normal, size="1in")]
        entry = {"angle": degrees}
        for label, f, index in (("tangential", radius * math.cos(t) / 2.0, 1), ("sagittal", radius / (2.0 * math.cos(t)), 2)):
            w0, z0 = bp.propagate_q(1e-3, 1.0, LAMBDA, 0.3, bp.mirror(f))
            z_r = math.pi * w0 ** 2 / LAMBDA
            distances = planes_around(z0, z_r)
            halves = [4.0 * w0 * math.sqrt(1.0 + ((d - z0) / z_r) ** 2) for d in distances]
            rows, fit_u, fit_v, scene = fit_planes(f"tilted_{degrees:g}_{label}", optics, vertex, out_dir, distances, halves, samples, out, max_distance=0.02)
            # u is horizontal, in the plane of incidence: the tangential axis
            fw, fz, fm = fit_u if label == "tangential" else fit_v
            check(abs(fz - z0) < 1e-3 * z0, f"{degrees:g} deg {label}: focus {fz * 1e3:.3f} mm from the vertex, ABCD {z0 * 1e3:.3f} mm "
                  f"(f = {f * 1e3:.3f} mm), waist {fw * 1e6:.2f} um against {w0 * 1e6:.2f} um")
            entry[label] = {"focus": z0, "waist": w0, "measured_focus": fz, "measured_waist": fw}
            if degrees == 45.0 and label == "tangential":
                nominal = scene
        separation = entry["sagittal"]["measured_focus"] - entry["tangential"]["measured_focus"]
        print(f"  {degrees:g} deg: foci {separation * 1e3:.3f} mm apart, R / (2 cos) - R cos / 2 = {(radius / (2 * math.cos(t)) - radius * math.cos(t) / 2) * 1e3:.3f} mm")
        expected["angles"].append(entry)
    return nominal, expected


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--write", action="store_true", help="write the scenes to assets/scenes/optics/ with their expected values")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="optics_verification_"))
    samples = 16 if args.quick else 64
    results = {
        "gaussian_train": gaussian_train(samples, out),
        "telescope_kepler": telescope("telescope_kepler", 0.025, 4.0e-3, samples, out),
        "telescope_galilean": telescope("telescope_galilean", -0.025, 2.0e-3, samples, out),
        "oap_focus": oap_focus(samples, out),
        "tilted_spherical_mirror": tilted_mirror(samples, out),
    }
    if args.write:
        SCENES.mkdir(parents=True, exist_ok=True)
        for name, (scene, expected) in results.items():
            vkscene.write_scene(SCENES / f"{name}.gltf", scene)
            (SCENES / f"{name}.expected.json").write_text(json.dumps(expected, indent=2) + "\n")
            print(f"wrote assets/scenes/optics/{name}.gltf")
    print()
    print("PASS" if not failures else f"FAIL ({len(failures)})")
    for f in failures:
        print("  " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
