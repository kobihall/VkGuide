"""The laser sources' verification scenes (docs/plans/completed/physoptics-laser-sources.md), written to assets/scenes/tests/lasers/
with a file of the values each must show (<name>.expected.json). tools/verify/laser_check.py renders them and compares
what their detectors (docs/plans/completed/physoptics-detectors.md) read with the closed forms and with the wave reference
of tools/verify/wave_optics.py. A probe plane records the beam where it crosses without changing it, so one render
measures every plane of a scene; a power meter ends the beam.

  laser_power          Thorlabs' HNL050LB and a power meter 1 m out: 5 mW on it, and on a probe plane just before it the
                       radius w(1 m) = 0.643 mm
  laser_m2             an M^2 = 2 beam whose waist is 30 mm inside its housing, on a stack of probe planes at 50 mm,
                       0.625 m and 1.2 m
  laser_diode          an astigmatic diode beam, x and y of their own waists, waist positions and M^2, on a stack of
                       probe planes at 0.3 m and 1.5 m
  laser_into_aperture  a camera on the beam's axis 0.5 m away looking into the laser, which sees the irradiance
                       2 P / (pi w^2) there; and one 5 radii off the axis, which sees nothing
  laser_spectral       laser_power in the spectral modes: hero wavelengths drawn from the lights' spectra, the spectral
                       bands, and the monochromatic mode at the line's center
  laser_iris           an iris 1 mm across, 0.2 m out, clipping the HNL050LB's beam: the power through it on a probe plane
                       5 mm past it, and the beam 2 m out, where the rays and the waves part (the rays carry no diffraction),
                       the two planes a stack

Before detectors these checks measured each plane on a white screen seen by a pinhole camera (beam_probe.probe()), one
render a plane; tools/verify/detector_check.py keeps that measurement as a cross-check of the detectors.

Several probe planes along one beam are a sensor stack (src/optic_detectors.h): evenly spaced, each the size of the
widest beam among them, as one sensor slid along a rail would be. Each beam runs along +z from (0, 0.1, -0.9), 100 mm
above a breadboard under the whole scene, as a bench's beam does, and the iris stands in its housing on a post.

Run:  python3 tools/verify/laser_scenes.py
"""

import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import beam_probe as bp  # noqa: E402
import vkscene  # noqa: E402
import wave_optics as wo  # noqa: E402

SCENES = vkscene.TEST_SCENES / "lasers"
# 0.9 m back from the breadboard's center, which a 2 m beam then fits on
ORIGIN = (0.0, 0.1, -0.9)
AXIS = (0.0, 0.0, 1.0)
HNL = wo.LaserBeam.hnl050lb()

# the custom beams, as the laser element's parameters and as wave_optics beams
M2_PARAMS = {"waistRadius": 0.405e-3, "waistPosition": -0.03, "m2": 2.0}
M2_BEAM = wo.LaserBeam.round(632.8e-9, 0.405e-3, -0.03, 2.0, 5e-3)
DIODE_PARAMS = {"housing": "diode_module", "wavelength": 635e-9, "power": 1.2e-3, "astigmatic": True, "waistRadius": 1.2e-3, "waistPosition": 0.02,
                "m2": 1.3, "waistRadiusY": 0.4e-3, "waistPositionY": -0.03, "m2Y": 1.8}
DIODE_BEAM = wo.LaserBeam(635e-9, wo.AxisBeam(1.2e-3, 0.02, 1.3), wo.AxisBeam(0.4e-3, -0.03, 1.8), 1.2e-3)
IRIS_Z = 0.2
IRIS_OPENING = 1.0e-3


def on_axis(z):
    return (ORIGIN[0], ORIGIN[1], ORIGIN[2] + z)


def half_field(beam, z):
    """four times the beam's larger radius either side of its center"""
    return 4.0 * max(beam.x.radius(z, beam.wavelength), beam.y.radius(z, beam.wavelength))


def side_camera():
    """a camera beside the bench, which light tracing needs and no check reads"""
    return vkscene.camera((0.4, 0.4, -1.2), (0.0, 0.1, 0.0), 40.0)


def plane_scene(beam_optics, planes, *, samples=64, size=256, render_extra=None, extra_optics=(), meter_at=None, kind="probe_plane"):
    """Probe planes at the (z, half field) of `planes` along the beam, named plane_0, plane_1, ..., and a power meter
    named meter at `meter_at` when it is given. More than one plane is a sensor stack, the planes evenly spaced and each
    the widest one's half field"""
    optics = list(beam_optics) + list(extra_optics)
    stacks = []
    if len(planes) > 1:
        zs = [z for z, _ in planes]
        step = (zs[-1] - zs[0]) / (len(zs) - 1)
        assert all(abs(z - (zs[0] + i * step)) < 1e-12 for i, z in enumerate(zs)), "a stack's planes are evenly spaced"
        probes, record = bp.detector_stack("plane", on_axis(zs[0]), on_axis(zs[-1]), len(zs), max(h for _, h in planes), kind=kind, pixels=size)
        stacks.append(record)
    else:
        (z, half), = planes
        p, record = bp.detector("plane_0", on_axis(z), AXIS, half, kind=kind, pixels=size)
        probes = [p]
        optics.append(record)
    if meter_at is not None:
        optics.append(bp.free_optic("power_meter", "meter", on_axis(meter_at), AXIS))
    return probes, bp.scene([], [], optics, side_camera(), size, size, samples=samples, render_extra=render_extra, stacks=stacks)


def hnl():
    return bp.laser("HNL050LB", ORIGIN, AXIS)


def laser_power(z=1.0, **kw):
    return plane_scene([hnl()], [(z - 1e-3, half_field(HNL, z))], meter_at=z, **kw)


M2_PLANES = (0.05, 0.625, 1.2)


def laser_m2(planes=M2_PLANES, **kw):
    return plane_scene([bp.laser("M2", ORIGIN, AXIS, preset=None, params=M2_PARAMS)], [(z, half_field(M2_BEAM, z)) for z in planes], **kw)


def laser_diode(planes=(0.3, 1.5), **kw):
    return plane_scene([bp.laser("diode", ORIGIN, AXIS, preset=None, params=DIODE_PARAMS)], [(z, half_field(DIODE_BEAM, z)) for z in planes], **kw)


def laser_spectral(representation="hero", **kw):
    spectral = {"representation": representation, "lanes": 8 if representation == "bands" else 4, "sampling": "lights", "emittersShare": 0.9, "wavelength": 632.8}
    return laser_power(render_extra={"spectral": spectral}, **kw)


def laser_into_aperture(off_axis=0.0, integrator="path", size=384, samples=64):
    """A camera 0.5 m out on the beam's axis (or `off_axis` meters beside it) looking back into the laser, 1 degree across"""
    distance = 0.5
    position = (ORIGIN[0] + off_axis, ORIGIN[1], ORIGIN[2] + distance)
    cam = vkscene.camera(position, (ORIGIN[0] + off_axis, ORIGIN[1], ORIGIN[2]), bp.MIN_FOV_DEGREES)
    return bp.scene([], [], [hnl()], cam, size, size, samples=samples, integrator=integrator)


def iris():
    return bp.free_optic("iris", "ID1", on_axis(IRIS_Z), AXIS, size="ID8", params={"opening": IRIS_OPENING})


IRIS_PLANES = (IRIS_Z + 0.005, 2.0)


def laser_iris(**kw):
    # 2 m out the clipped beam spreads by diffraction beyond the rays: a field twice as wide shows the rings
    far = IRIS_PLANES[1]
    halves = [half_field(HNL, IRIS_PLANES[0]), 4.0 * max(HNL.x.radius(far, HNL.wavelength), 2.0 * 1.22 * HNL.wavelength * (far - IRIS_Z) / IRIS_OPENING)]
    return plane_scene([hnl()], list(zip(IRIS_PLANES, halves)), extra_optics=[iris()], **kw)


def expected():
    """what each scene must show, from the closed forms"""
    lam = HNL.wavelength
    w05 = HNL.x.radius(0.5, lam)
    a = IRIS_OPENING / 2.0
    w_iris = HNL.x.radius(IRIS_Z, lam)
    return {
        "laser_power": {"power": HNL.power, "radius": HNL.x.radius(1.0, lam), "tolerance": {"power": 0.005, "radius": 0.01},
                        "notes": "HNL050LB 1 m from a power meter: all of its power, and on the probe plane before it its 1/e^2 radius"},
        "laser_m2": {"planes": list(M2_PLANES), "radius": [M2_BEAM.x.radius(z, lam) for z in M2_PLANES], "tolerance": {"radius": 0.01},
                     "notes": "M^2 = 2, its waist 30 mm inside the housing: w(z) with the divergence twice a Gaussian's"},
        "laser_diode": {"planes": [0.3, 1.5], "radius_x": [DIODE_BEAM.x.radius(z, DIODE_BEAM.wavelength) for z in (0.3, 1.5)],
                        "radius_y": [DIODE_BEAM.y.radius(z, DIODE_BEAM.wavelength) for z in (0.3, 1.5)], "tolerance": {"radius": 0.01},
                        "notes": "x and y each follow their own w(z)"},
        "laser_into_aperture": {"irradiance": 2.0 * HNL.power / (math.pi * w05 ** 2), "spot_half_angle": HNL.x.waist * HNL.x.half_angle(lam) / w05,
                                "tolerance": {"irradiance": 0.01}, "notes": "the image integrates to the irradiance at the camera; off the axis by 5 radii, none"},
        "laser_spectral": {"power": HNL.power, "tolerance": {"power": 0.01}, "notes": "the same 5 mW on the power meter in the spectral modes (W/nm at the line's peak density in the monochromatic one)"},
        "laser_iris": {"planes": list(IRIS_PLANES), "transmitted": HNL.power * (1.0 - math.exp(-2.0 * a * a / (w_iris * w_iris))), "tolerance": {"transmitted": 0.01},
                       "notes": "the power through the iris agrees; 2 m out the rays and the waves differ, which the check reports"},
    }


SCENE_BUILDERS = {
    "laser_power": lambda: laser_power()[1],
    "laser_m2": lambda: laser_m2()[1],
    "laser_diode": lambda: laser_diode()[1],
    "laser_into_aperture": lambda: laser_into_aperture(),
    "laser_spectral": lambda: laser_spectral()[1],
    "laser_iris": lambda: laser_iris()[1],
}


def main():
    SCENES.mkdir(parents=True, exist_ok=True)
    values = expected()
    for name, build in SCENE_BUILDERS.items():
        path = SCENES / f"{name}.gltf"
        vkscene.write_scene(path, build())
        (SCENES / f"{name}.expected.json").write_text(json.dumps(values[name], indent=2) + "\n")
        print(f"wrote {path.relative_to(vkscene.ROOT)}")


if __name__ == "__main__":
    main()
