"""The detectors' verification scenes (docs/plans/completed/physoptics-detectors.md), written to assets/scenes/tests/detectors/ with a
file of the values each must show (<name>.expected.json). tools/verify/detector_check.py renders them with light tracing
and checks what their detectors read.

  detector_power_meter     Thorlabs' HNL050LB and a 10 mm power meter 1 m out, which catches the whole beam: 5 mW
  detector_splitter        the HNL050LB through a probe plane into an ideal 50:50 cube on a prism mount, a power meter on each of its two
                           outputs: half the power each, less the 25.4 mm of N-BK7 it crosses, within the noise the meters
                           report; without the probe plane the same
  detector_profiler_focus  the HNL050LB through an f = 500 mm N-BK7 plano-convex lens, and a 10 mm beam profiler at
                           the focus ABCD puts the waist at: its second-moment radius
  detector_card            an alignment card 0.8 m out, and a camera on the axis 50 mm in front of it: the spot on the card's
                           film and in the camera's image, both of radius w(0.8 m), the image 0.8 of the power
  detector_diffuse_wall    the HNL050LB on a diffuse transmission wall (reflectance 0.5, transmittance 0.3), a power meter
                           50 mm from the spot at 45 degrees on each side: what a Lambertian spot sends into each meter
  detector_spectrometer    the HNL050LB and a 2 mW, 532 nm laser into a spectrometer of 0.1 nm bins: two lines, at their
                           wavelengths and of their powers, in the hero wavelength mode
  detector_dump            a beam dump 0.5 m out, and a power meter behind it: the dump absorbs all of it, the meter reads 0
  detector_no_camera       detector_power_meter with no camera at all, the meter the render target: light tracing emits a
                           set number of paths a sample, and the meter reads the same 5 mW
  detector_sensor_emitter  a 10 mm camera sensor 30 mm from a glowing 20 mm square, the render target with no camera: the
                           path tracer renders it from its pixels, light tracing deposits on it, and both read the power
                           the square's closed form sends
  detector_sensor_sky      the camera sensor alone, facing up, under a uniform gray sky, rendered from its pixels: irradiance
                           pi times the gray at every pixel

Each beam runs along +z from (0, 0.1, 0), 100 mm above a breadboard under the whole scene, as a bench's beam does, and
each optic stands in its mount on a post.

Run:  python3 tools/verify/detector_scenes.py
"""

import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import beam_probe as bp  # noqa: E402
import laser_scenes as ls  # noqa: E402
import vkscene  # noqa: E402

SCENES = vkscene.TEST_SCENES / "detectors"
ORIGIN = np.array([0.0, 0.1, 0.0])
AXIS = (0.0, 0.0, 1.0)
HNL = ls.HNL
LAMBDA = HNL.wavelength
AR = {"type": "optical_interface", "coating": "ideal_ar", "finish": 0}

# the splitter cube's center, and the wall's and the meters' geometry
CUBE_Z = 0.3
SPLIT_DISTANCE = 0.15
WALL_Z = 0.3
WALL_REFLECTANCE = 0.5
WALL_TRANSMITTANCE = 0.3
WALL_DISTANCE = 0.05
WALL_ANGLE = math.radians(45.0)
METER_RADIUS = 5e-3
# the lens, and the 532 nm laser beside the HNL050LB
LENS_Z = 0.2
LENS_F = 0.5
LENS_THICKNESS = 3.6e-3
GREEN = {"wavelength": 532e-9, "power": 2e-3, "waistRadius": 0.5e-3, "waistPosition": 0.0, "m2": 1.0}
GREEN_POWER = 2e-3
SPECTROMETER_Z = 0.5
CARD_Z = 0.8
DUMP_Z = 0.5


def at(z, x=0.0):
    return ORIGIN + np.array([x, 0.0, z])


def side_camera():
    return vkscene.camera((0.3, 0.3, -0.3), (0.0, 0.1, 0.3), 40.0)


def meter(name, position, toward):
    """a 10 mm power meter at `position` facing the light arriving along `toward`"""
    return bp.free_optic("power_meter", name, position, toward)


def scene(optics, *, shapes=(), materials=(), cam=None, samples=64, size=256, render_extra=None):
    return bp.scene(list(shapes), list(materials), optics, cam or side_camera(), size, size, samples=samples, render_extra=render_extra)


def detector_power_meter(samples=64, **kw):
    return scene([bp.laser("HNL050LB", ORIGIN, AXIS), meter("meter", at(1.0), AXIS)], samples=samples, **kw)


def cube():
    """an N-BK7 cube with an ideal 50:50 diagonal and ideal AR faces on a prism mount, which stands its x up: it reflects
    toward +x"""
    return bp.free_optic("cube_beamsplitter", "splitter", at(CUBE_Z), AXIS, up=(-1.0, 0.0, 0.0), size="25.4mm", materials={"faces": AR})


def nbk7_internal_transmittance(length):
    """N-BK7's internal transmittance over `length` meters at the laser's wavelength, from the bundled catalog's IT lines,
    their absorption coefficients interpolated linearly in wavelength as the renderer's glass reads them"""
    lines = []
    inside = False
    for line in (vkscene.ROOT / "assets" / "glass" / "bundled.agf").read_text().splitlines():
        if line.startswith("NM "):
            inside = line.split()[1] == "N-BK7"
        elif inside and line.startswith("IT "):
            lam_um, transmittance, thickness_mm = (float(v) for v in line.split()[1:4])
            lines.append((lam_um * 1e3, -math.log(transmittance) / (thickness_mm * 1e-3)))
    lines.sort()
    alpha = float(np.interp(LAMBDA * 1e9, [l for l, _ in lines], [a for _, a in lines]))
    return math.exp(-alpha * length)


# the cube's side: each output crosses one side of N-BK7
CUBE_SIDE = 0.0254
# one sample per pixel a frame: 16 frames or more, whose spread the standard error is measured from
NOISE_FRAMES = {"samplesPerFrame": 1}


def detector_splitter(samples=64, probe=True, **kw):
    optics = [bp.laser("HNL050LB", ORIGIN, AXIS)]
    if probe:
        _, record = bp.detector("probe", at(CUBE_Z - 0.05), AXIS, 4.0 * HNL.x.radius(CUBE_Z - 0.05, LAMBDA))
        optics.append(record)
    optics += [cube(), meter("transmitted", at(CUBE_Z + SPLIT_DISTANCE), AXIS), meter("reflected", at(CUBE_Z, SPLIT_DISTANCE), (1.0, 0.0, 0.0))]
    return scene(optics, samples=samples, render_extra=NOISE_FRAMES, **kw)


def lens():
    """an N-BK7 plano-convex lens of f = 500 mm, its curved face to the laser, ideal AR on both faces"""
    radius = LENS_F * (bp_nbk7() - 1.0)
    return bp.free_optic("spherical_lens", "lens", at(LENS_Z), AXIS, size="custom",
                         params={"diameter": 0.0254, "form": "plano_convex", "r1": radius, "centerThickness": LENS_THICKNESS, "glass": "N-BK7"},
                         materials={"front": AR, "back": AR})


def bp_nbk7():
    """N-BK7 relative to air at the laser's wavelength (Schott's Sellmeier coefficients)"""
    l2 = (LAMBDA * 1e6) ** 2
    b = (1.03961212, 0.231792344, 1.01046945)
    c = (0.00600069867, 0.0200179144, 103.560653)
    return math.sqrt(1.0 + sum(bi * l2 / (l2 - ci) for bi, ci in zip(b, c)))


def focus():
    """the waist after the lens by ABCD: its radius and its distance past the lens's back vertex"""
    n = bp_nbk7()
    radius = LENS_F * (n - 1.0)
    system = bp.chain(bp.refraction(radius, 1.0, n), bp.free_space(LENS_THICKNESS), bp.refraction(math.inf, n, 1.0))
    return bp.propagate_q(HNL.x.waist, HNL.x.m2, LAMBDA, LENS_Z, system)


def detector_profiler_focus(samples=64, **kw):
    _, z = focus()
    profiler = bp.free_optic("beam_profiler", "profiler", at(LENS_Z + LENS_THICKNESS + z), AXIS)
    return scene([bp.laser("HNL050LB", ORIGIN, AXIS), lens(), profiler], samples=samples, **kw)


def detector_card(samples=64, **kw):
    """the card, and a probe of the screen check (beam_probe.probe()): a camera on the axis 50 mm before the card"""
    half = 4.0 * HNL.x.radius(CARD_Z, LAMBDA)
    p, _, cam = bp.probe(at(CARD_Z), AXIS, half, max_distance=0.05)
    card = bp.free_optic("alignment_card", "card", at(CARD_Z), AXIS)
    return p, scene([bp.laser("HNL050LB", ORIGIN, AXIS), card], cam=cam, samples=samples, **kw)


def wall_meters():
    spot = at(WALL_Z)
    back = np.array([-math.sin(WALL_ANGLE), 0.0, -math.cos(WALL_ANGLE)])
    front = np.array([math.sin(WALL_ANGLE), 0.0, math.cos(WALL_ANGLE)])
    # each meter's front toward the spot: the light it records travels away from the spot
    return [meter("reflected", spot + WALL_DISTANCE * back, back), meter("transmitted", spot + WALL_DISTANCE * front, front)]


def detector_diffuse_wall(samples=256, **kw):
    material = {"name": "wall", "type": "diffuse_transmission", "albedo": [WALL_REFLECTANCE] * 3, "transmittance": [WALL_TRANSMITTANCE] * 3, "scale": 1.0}
    wall = vkscene.quad("wall", at(WALL_Z), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 0.1, 0.1, 0)
    return scene([bp.laser("HNL050LB", ORIGIN, AXIS)] + wall_meters(), shapes=[wall], materials=[material], samples=samples, render_extra={"samplesPerFrame": 8}, **kw)


def lambertian_share(distance, angle, radius, steps=400):
    """the share of a Lambertian spot's power a disk of `radius` collects, `distance` from it at `angle` from its normal and
    facing it: the integral over the disk of cos(phi) cos(beta) / (pi r^2), phi from the spot's normal, beta at the disk"""
    c = np.array([math.sin(angle), 0.0, math.cos(angle)]) * distance
    n = -c / distance
    e1 = np.array([0.0, 1.0, 0.0])
    e2 = np.cross(n, e1)
    rho = (np.arange(steps) + 0.5) / steps * radius
    psi = (np.arange(steps) + 0.5) / steps * 2.0 * math.pi
    rr, pp = np.meshgrid(rho, psi)
    points = c[None, None, :] + rr[..., None] * (np.cos(pp)[..., None] * e1 + np.sin(pp)[..., None] * e2)
    r = np.linalg.norm(points, axis=-1)
    cos_phi = points[..., 2] / r
    cos_beta = -(points @ n) / r
    area = rr * (radius / steps) * (2.0 * math.pi / steps)
    return float((cos_phi * cos_beta / (math.pi * r * r) * area).sum())


def detector_spectrometer(samples=64, **kw):
    entrance = at(SPECTROMETER_Z)
    green_origin = ORIGIN + np.array([0.01, 0.0, 0.0])
    green = bp.laser("green", green_origin, entrance - green_origin, preset=None, params=GREEN)
    spectrometer = bp.free_optic("spectrometer", "spectrometer", entrance, AXIS)
    spectral = {"representation": "hero", "lanes": 4, "sampling": "lights", "emittersShare": 0.9}
    return scene([bp.laser("HNL050LB", ORIGIN, AXIS), green, spectrometer], samples=samples, render_extra={"spectral": spectral}, **kw)


def detector_dump(samples=64, **kw):
    dump = bp.free_optic("beam_dump", "dump", at(DUMP_Z), AXIS)
    return scene([bp.laser("HNL050LB", ORIGIN, AXIS), dump, meter("behind", at(DUMP_Z + 0.1), AXIS)], samples=samples, **kw)


def detector_no_camera(samples=64, **kw):
    optics = [bp.laser("HNL050LB", ORIGIN, AXIS), meter("meter", at(1.0), AXIS)]
    return bp.scene([], [], optics, None, 256, 256, samples=samples, render_detector="meter", **kw)


SENSOR_Z = 0.3
SENSOR_SIZE = 0.01
SENSOR_PIXELS = 64
EMITTER_SIZE = 0.02
EMITTER_DISTANCE = 0.03
EMITTER_RADIANCE = 2.0
SKY = 0.5


def camera_sensor(facing=AXIS):
    """the sensor, recording light that travels along `facing`"""
    return bp.free_optic("camera_sensor", "sensor", at(SENSOR_Z), facing,
                         params={"width": SENSOR_SIZE, "height": SENSOR_SIZE, "pixelsX": SENSOR_PIXELS, "pixelsY": SENSOR_PIXELS})


def detector_sensor_emitter(integrator="path", samples=64, **kw):
    """the square glows toward the sensor, which faces it along +z"""
    material = {"name": "glow", "type": "emissive", "albedo": [1.0, 1.0, 1.0], "strength": EMITTER_RADIANCE}
    square = vkscene.quad("glow", at(SENSOR_Z - EMITTER_DISTANCE), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), EMITTER_SIZE, EMITTER_SIZE, 0)
    return bp.scene([square], [material], [camera_sensor()], None, 256, 256, samples=samples, integrator=integrator, render_detector="sensor", **kw)


def detector_sensor_sky(samples=64, **kw):
    """the sensor facing up, so that the breadboard under it hides none of the sky it sees"""
    scene = bp.scene([], [], [camera_sensor(facing=(0.0, -1.0, 0.0))], None, 256, 256, samples=samples, integrator="path", render_detector="sensor", **kw)
    scene["scenes"][0]["extras"]["background"] = [SKY, SKY, SKY]
    return scene


def emitter_power(steps=200):
    """the power a Lambertian square of radiance L sends into a parallel, coaxial square sensor h away: the integral over
    both of L cos^2 / r^2 = L h^2 / r^4, by the midpoint rule"""
    h = EMITTER_DISTANCE
    s = (np.arange(steps) + 0.5) / steps - 0.5
    sx, sy = np.meshgrid(s * SENSOR_SIZE, s * SENSOR_SIZE)
    e = (np.arange(steps) + 0.5) / steps - 0.5
    total = 0.0
    for ex in e * EMITTER_SIZE:
        for ey in e[::4] * EMITTER_SIZE:
            r2 = (sx - ex) ** 2 + (sy - ey) ** 2 + h * h
            total += (h * h / (r2 * r2)).sum()
    area = (SENSOR_SIZE / steps) ** 2 * (EMITTER_SIZE / steps) * (EMITTER_SIZE / (steps / 4))
    return EMITTER_RADIANCE * total * area


def expected():
    w0, z = focus()
    share = lambertian_share(WALL_DISTANCE, WALL_ANGLE, METER_RADIUS)
    split = nbk7_internal_transmittance(CUBE_SIDE)
    return {
        "detector_power_meter": {"power": HNL.power, "tolerance": {"power": 0.005},
                                 "notes": "the whole beam on the meter: every path deposits the laser's power, so the reading is exact and its error 0"},
        "detector_splitter": {"transmitted": 0.5 * HNL.power * split, "reflected": 0.5 * HNL.power * split, "glass": split,
                              "tolerance": {"sigmas": 4.0, "probe": 1e-6, "spread": [0.6, 1.6]},
                              "notes": "an ideal 50:50 split, each half through 25.4 mm of N-BK7: each meter within 4 of its standard errors, the probe plane changes neither, and over seeds the readings spread as their errors say"},
        "detector_profiler_focus": {"radius": w0, "position": LENS_Z + LENS_THICKNESS + z, "tolerance": {"radius": 0.01},
                                    "notes": "ABCD's waist after an f = 500 mm lens, the embedded Gaussian of M^2 = 1.005"},
        "detector_card": {"power": HNL.power, "reflected": 0.8 * HNL.power, "radius": HNL.x.radius(CARD_Z, LAMBDA), "tolerance": {"power": 0.005, "radius": 0.01, "reflected": 0.02},
                          "notes": "the card's film reads the beam; the camera sees the card's white (0.8) spot of the same radius"},
        "detector_diffuse_wall": {"reflected": WALL_REFLECTANCE * HNL.power * share, "transmitted": WALL_TRANSMITTANCE * HNL.power * share, "share": share,
                                  "tolerance": {"sigmas": 4.0, "relative": 0.03},
                                  "notes": "a Lambertian spot's radiance into a 10 mm meter 50 mm away at 45 degrees, on the reflected and the transmitted side"},
        "detector_spectrometer": {"lines": [[632.8, HNL.power], [532.0, GREEN_POWER]], "tolerance": {"wavelength": 0.2, "power": 0.01},
                                  "notes": "two lines at their wavelengths, each of its laser's power"},
        "detector_dump": {"dump": HNL.power, "behind": 0.0, "tolerance": {"power": 0.005},
                          "notes": "the dump absorbs the beam; nothing reaches the meter behind it"},
        "detector_no_camera": {"power": HNL.power, "paths_per_sample": 512 * 512, "tolerance": {"power": 0.005},
                               "notes": "the meter as the render target and no camera: 512 x 512 light paths a sample, the same 5 mW"},
        "detector_sensor_emitter": {"power": emitter_power(), "tolerance": {"relative": 0.02},
                                    "notes": "camera paths from the sensor's pixels and light tracing's deposits each read the power a Lambertian square of radiance 2 sends into the sensor"},
        "detector_sensor_sky": {"power": math.pi * SKY * SENSOR_SIZE ** 2, "irradiance": math.pi * SKY, "tolerance": {"relative": 0.002},
                                "notes": "a uniform sky of 0.5: pi times 0.5 W/m^2 at every pixel"},
    }


SCENE_BUILDERS = {
    "detector_power_meter": lambda: detector_power_meter(),
    "detector_splitter": lambda: detector_splitter(),
    "detector_profiler_focus": lambda: detector_profiler_focus(),
    "detector_card": lambda: detector_card()[1],
    "detector_diffuse_wall": lambda: detector_diffuse_wall(),
    "detector_spectrometer": lambda: detector_spectrometer(),
    "detector_dump": lambda: detector_dump(),
    "detector_no_camera": lambda: detector_no_camera(),
    "detector_sensor_emitter": lambda: detector_sensor_emitter(),
    "detector_sensor_sky": lambda: detector_sensor_sky(),
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
