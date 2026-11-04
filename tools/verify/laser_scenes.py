"""The laser sources' verification scenes (docs/plans/completed/physoptics-laser-sources.md), written to assets/scenes/optics/
with a file of the values each must show (<name>.expected.json). tools/verify/laser_check.py renders them, moving the
screen to each plane it measures at, and compares them with the closed forms and with the wave reference of
tools/verify/wave_optics.py.

  laser_power          Thorlabs' HNL050LB 1 m from a white screen: 5 mW on it, and the radius w(1 m) = 0.643 mm
  laser_m2             an M^2 = 2 beam whose waist is 30 mm inside its housing, on screens at 50 mm, 0.4 m and 1.2 m
  laser_diode          an astigmatic diode beam, x and y of their own waists, waist positions and M^2, at 0.3 m and 1.5 m
  laser_into_aperture  a camera on the beam's axis 0.5 m away looking into the laser, which sees the irradiance
                       2 P / (pi w^2) there; and one 5 radii off the axis, which sees nothing
  laser_spectral       laser_power in the spectral modes: hero wavelengths drawn from the lights' spectra, and the
                       monochromatic mode at the line's center
  laser_iris           an iris 1 mm across, 0.2 m out, clipping the HNL050LB's beam: the power through it, and the beam
                       2 m out, where the rays and the waves part (the rays carry no diffraction)

Each beam runs along +z from (0, 0.1, 0), 100 mm above the table, as a bench's beam does.

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

SCENES = vkscene.ROOT / "assets" / "scenes" / "optics"
ORIGIN = (0.0, 0.1, 0.0)
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


def screen_scene(beam_optics, beam, z, *, samples=64, size=256, render_extra=None, extra_optics=()):
    """A screen `z` along the beam, seeing four times the beam's larger radius either side of its center"""
    half = 4.0 * max(beam.x.radius(z, beam.wavelength), beam.y.radius(z, beam.wavelength))
    # the camera between the laser and the screen, or the laser's aperture hides the spot from it
    p, screen, cam = bp.probe(on_axis(z), AXIS, half, width=size, height=size, max_distance=min(0.1, 0.5 * z))
    return p, bp.scene([screen], [bp.WHITE], list(beam_optics) + list(extra_optics), cam, size, size, samples=samples, render_extra=render_extra)


def laser_power(z=1.0, **kw):
    return screen_scene([bp.laser("HNL050LB", ORIGIN, AXIS)], HNL, z, **kw)


def laser_m2(z=0.4, **kw):
    return screen_scene([bp.laser("M2", ORIGIN, AXIS, preset=None, params=M2_PARAMS)], M2_BEAM, z, **kw)


def laser_diode(z=0.3, **kw):
    return screen_scene([bp.laser("diode", ORIGIN, AXIS, preset=None, params=DIODE_PARAMS)], DIODE_BEAM, z, **kw)


def laser_spectral(representation="hero", **kw):
    spectral = {"representation": representation, "lanes": 4, "sampling": "lights", "emittersShare": 0.9, "wavelength": 632.8}
    return laser_power(render_extra={"spectral": spectral}, **kw)


def laser_into_aperture(off_axis=0.0, integrator="path", size=384, samples=64):
    """A camera 0.5 m out on the beam's axis (or `off_axis` meters beside it) looking back into the laser, 1 degree across"""
    distance = 0.5
    position = (ORIGIN[0] + off_axis, ORIGIN[1], ORIGIN[2] + distance)
    cam = vkscene.camera(position, (ORIGIN[0] + off_axis, ORIGIN[1], ORIGIN[2]), bp.MIN_FOV_DEGREES)
    return bp.scene([], [], [bp.laser("HNL050LB", ORIGIN, AXIS)], cam, size, size, samples=samples, integrator=integrator)


def iris():
    return bp.free_optic("iris", "ID1", on_axis(IRIS_Z), AXIS, size="ID8", params={"opening": IRIS_OPENING})


def laser_iris(z=2.0, **kw):
    p, scene = laser_power(z=z, extra_optics=[iris()], **kw)
    if z > IRIS_Z:
        # the clipped beam spreads by diffraction beyond the rays: a field twice as wide shows the rings
        half = 4.0 * max(HNL.x.radius(z, HNL.wavelength), 2.0 * 1.22 * HNL.wavelength * (z - IRIS_Z) / IRIS_OPENING)
        p, screen, cam = bp.probe(on_axis(z), AXIS, half, max_distance=min(0.1, 0.5 * (z - IRIS_Z)))
        scene = bp.scene([screen], [bp.WHITE], [bp.laser("HNL050LB", ORIGIN, AXIS), iris()], cam, 256, 256, samples=kw.get("samples", 64))
    return p, scene


def expected():
    """what each scene must show, from the closed forms"""
    lam = HNL.wavelength
    w05 = HNL.x.radius(0.5, lam)
    a = IRIS_OPENING / 2.0
    w_iris = HNL.x.radius(IRIS_Z, lam)
    return {
        "laser_power": {"power": HNL.power, "radius": HNL.x.radius(1.0, lam), "tolerance": {"power": 0.005, "radius": 0.01},
                        "notes": "HNL050LB 1 m from a white screen: all of its power, at its 1/e^2 radius there"},
        "laser_m2": {"planes": [0.05, 0.4, 1.2], "radius": [M2_BEAM.x.radius(z, lam) for z in (0.05, 0.4, 1.2)], "tolerance": {"radius": 0.01},
                     "notes": "M^2 = 2, its waist 30 mm inside the housing: w(z) with the divergence twice a Gaussian's"},
        "laser_diode": {"planes": [0.3, 1.5], "radius_x": [DIODE_BEAM.x.radius(z, DIODE_BEAM.wavelength) for z in (0.3, 1.5)],
                        "radius_y": [DIODE_BEAM.y.radius(z, DIODE_BEAM.wavelength) for z in (0.3, 1.5)], "tolerance": {"radius": 0.01},
                        "notes": "x and y each follow their own w(z)"},
        "laser_into_aperture": {"irradiance": 2.0 * HNL.power / (math.pi * w05 ** 2), "spot_half_angle": HNL.x.waist * HNL.x.half_angle(lam) / w05,
                                "tolerance": {"irradiance": 0.01}, "notes": "the image integrates to the irradiance at the camera; off the axis by 5 radii, none"},
        "laser_spectral": {"power": HNL.power, "tolerance": {"power": 0.01}, "notes": "the same 5 mW from the luminance in the spectral modes"},
        "laser_iris": {"transmitted": HNL.power * (1.0 - math.exp(-2.0 * a * a / (w_iris * w_iris))), "tolerance": {"transmitted": 0.01},
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
