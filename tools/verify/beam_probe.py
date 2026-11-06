"""Measuring a laser beam in a render: with a detector (docs/plans/completed/physoptics-detectors.md), a probe plane or a
beam profiler square to the beam, whose film the batch mode writes (detector()); or, as before detectors existed and as
a cross-check of them, with a white Lambertian screen square to the beam and a pinhole camera on the beam's axis a
short way in front of it, looking at it (probe()). The camera has no
geometry, so the beam passes it and the screen's light reaches it unobstructed; light tracing (the only integrator that
starts paths at a laser) connects every point the beam lights to it.

Seen square-on through a pinhole, the screen maps to the image linearly, every pixel covering the same area, so the
image is the screen's radiance L = E / pi on a uniform grid: its sum times pi times a pixel's area is the power on the
screen, and its second moments are the beam's (ISO 11146). The film's pixel filter is a box of one pixel, so a light
path's splat lands in the one pixel its point maps to.

Also the closed forms the checks compare with: a Gaussian beam of any M^2 through an ABCD system (Siegman 1986, the
embedded Gaussian of M^2 lambda), and a hyperbola fit of w(z)^2 for a waist, its position and M^2.
"""

import math
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402

# the narrowest view the scene file keeps: it clamps a camera to 1 degree
MIN_FOV_DEGREES = 1.0
SCENE_VERSION = 31


def unit(v):
    return vkscene.normalize(v)


def frame_from_z(z_axis, up=(0.0, 1.0, 0.0)):
    """(x, y, z) of the frame whose z is `z_axis` and whose y is as near `up` as it can be: an optic's body axes"""
    z = unit(z_axis)
    x = np.cross(up, z)
    if np.linalg.norm(x) < 1e-9:
        x = np.cross([1.0, 0.0, 0.0], z)
    x = unit(x)
    return x, np.cross(z, x), z


# the mount each kind stands in on the bench, its default (OPTIC_TRAITS in src/optic_catalog.cpp): a laser and the
# detectors are placed freely and have none
MOUNTS = {"planar_mirror": "kinematic", "concave_mirror": "kinematic", "off_axis_parabola": "kinematic", "circular_filter": "fixed",
          "square_filter": "plate_holder", "iris": "iris", "plate_beamsplitter": "rotation", "cube_beamsplitter": "prism", "spherical_lens": "fixed",
          "right_angle_prism": "prism", "equilateral_prism": "prism", "plate_wedge": "plate_holder", "round_wedge": "rotation"}


def free_optic(kind, name, position, z_axis, up=(0.0, 1.0, 0.0), size=None, params=None, materials=None, mount=None):
    """An optic with its body origin at `position` and its +z along `z_axis`. A kind that stands in a mount (MOUNTS, or
    `mount`, a mount kind's key; "none" for none) is written in it by where its body is (scene file version 30): the
    renderer stands it on a post on the bench there, and refuses the file if its mount turns the body from this
    orientation, so `up` must be the body's y as the mount holds it (up for every mount but a prism mount's, which stands
    the body's x up)"""
    x, y, z = frame_from_z(z_axis, up)
    mount = mount or MOUNTS.get(kind, "none")
    params = dict(params or {})
    if kind == "spherical_lens":
        # a lens's position is its origin's (scene file version 31): the front vertex is the body frame's origin
        params.setdefault("originAt", "front_vertex")
    record = {"type": kind, "name": name, "params": params, "materials": materials or {},
              "mount": {"kind": mount, "position": [float(c) for c in position], "orientation": [float(c) for c in vkscene.quat_from_matrix(np.column_stack([x, y, z]))]}}
    if size is not None:
        record["size"] = size
    return record


def laser(name, position, axis, up=(0.0, 1.0, 0.0), preset="HNL050LB", params=None):
    """A laser whose output is at `position`, its beam along `axis`. With `params` the size is custom unless a preset is
    named, and the params override it"""
    return free_optic("laser", name, position, axis, up, size=preset if preset else "custom", params=params)


@dataclass
class Probe:
    """Where a beam is measured: the screen's center, the beam's direction there, the screen's axes as the image shows
    them (u to the image's right, v up), the half-field across u and v, and the image's size. A screen seen by a camera
    looking along the beam shows the beam's -x to its right (`mirrored`); a detector's film its +x"""
    center: np.ndarray
    direction: np.ndarray
    u: np.ndarray
    v: np.ndarray
    half_u: float
    half_v: float
    width: int
    height: int
    distance: float
    mirrored: bool = True
    name: str = ""

    def beam_x(self):
        """the beam's x coordinate (the laser's x axis) of each column"""
        uu, _ = self.coordinates()
        return -uu[0] if self.mirrored else uu[0]

    def pixel_area(self):
        return (2.0 * self.half_u / self.width) * (2.0 * self.half_v / self.height)

    def coordinates(self):
        """the screen coordinates (along u and v) of every pixel's center, row 0 at the top"""
        cu = ((np.arange(self.width) + 0.5) / self.width * 2.0 - 1.0) * self.half_u
        cv = (1.0 - (np.arange(self.height) + 0.5) / self.height * 2.0) * self.half_v
        return np.meshgrid(cu, cv)


def probe(center, direction, half_field, up=(0.0, 1.0, 0.0), width=256, height=256, max_distance=0.1, screen_size=None):
    """A screen at `center` square to the beam travelling along `direction`, and a camera on the axis looking at it,
    seeing `half_field` either side of the center vertically (and as much across as the image's shape gives). The camera
    stands at most `max_distance` in front, closer where the 1 degree field needs it. Returns the probe and the screen
    and camera records (the screen's material is index 0 of the scene's materials)"""
    center = np.asarray(center, dtype=np.float64)
    forward = unit(direction)
    half_v = half_field
    half_u = half_field * width / height
    distance = min(max_distance, half_v / math.tan(math.radians(MIN_FOV_DEGREES)))
    fov = 2.0 * math.degrees(math.atan(half_v / distance))
    cam = vkscene.camera(center - distance * forward, center, fov)
    # vkscene.camera's right is forward x up and its up is right x forward: the image's axes on the screen
    right = unit(np.cross(forward, up))
    image_up = np.cross(right, forward)
    side = screen_size or max(8.0 * max(half_u, half_v), 0.004)
    screen = vkscene.quad("screen", center, right, image_up, side, side, 0)
    return Probe(center, forward, right, image_up, half_u, half_v, width, height, distance), screen, cam


def detector(name, center, direction, half_field, *, kind="probe_plane", up=(0.0, 1.0, 0.0), pixels=256, half_v=None):
    """A detector at `center` square to the beam travelling along `direction`, its film `pixels` across and covering
    `half_field` either side of its center (a probe plane's or a screen's size; a profiler or camera keeps its own
    sensor). Its x axis is the laser's, so the beam's x is the film's columns. Returns the probe and the optic record"""
    center = np.asarray(center, dtype=np.float64)
    x, y, z = frame_from_z(direction, up)
    half_v = half_field if half_v is None else half_v
    params = None
    size = None
    if kind in ("probe_plane", "screen", "alignment_card"):
        params = {"width": 2.0 * half_field, "height": 2.0 * half_v, "pixelsX": pixels, "pixelsY": int(round(pixels * half_v / half_field))}
        size = "custom"
    # a camera sensor or a profiler keeps its own size and pixels; a power meter, a spectrometer and a beam dump are one pixel
    record = free_optic(kind, name, center, direction, up, size=size, params=params)
    height = params["pixelsY"] if params else pixels
    return Probe(center, z, x, y, half_field, half_v, pixels, height, 0.0, mirrored=False, name=name), record


def detector_stack(name, start, end, count, half_field, *, kind="probe_plane", up=(0.0, 1.0, 0.0), pixels=256):
    """A sensor stack (scene file version 30): `count` detectors of one size, `half_field` either side of each center,
    spaced evenly from `start` to `end` and facing along that axis, its x the laser's as detector() has it. Its members
    are named <name>_0, <name>_1, ... Returns their probes and the stack's record"""
    start = np.asarray(start, dtype=np.float64)
    end = np.asarray(end, dtype=np.float64)
    x, y, z = frame_from_z(end - start, up)
    params = None
    if kind in ("probe_plane", "screen", "alignment_card"):
        params = {"width": 2.0 * half_field, "height": 2.0 * half_field, "pixelsX": pixels, "pixelsY": pixels}
    probes = []
    for i in range(count):
        center = start + (end - start) * (i / (count - 1) if count > 1 else 0.0)
        probes.append(Probe(center, z, x, y, half_field, half_field, pixels, pixels, 0.0, mirrored=False, name=f"{name}_{i}"))
    record = {"name": name, "sensor": {"type": kind, "params": params or {}, "materials": {}}, "start": [float(c) for c in start],
              "end": [float(c) for c in end], "count": count, "orientation": [float(c) for c in vkscene.quat_from_matrix(np.column_stack([x, y, z]))],
              "alignToAxis": True}
    return probes, record


# the breadboard's largest side, and how far past the scene's objects it reaches (BREADBOARD_MAX_SIZE, src/optic_bench.h)
BREADBOARD_MAX = 3.6
BREADBOARD_MARGIN = 0.1
# how far behind its output a laser's housing reaches, about the HNL050LB's
LASER_HOUSING = 0.35


def breadboard(optics, stacks=()):
    """A breadboard under every optic, laser housing and sensor stack, as a scene file's "bench" has it: centered on its
    hole at the origin, as Thorlabs' breadboards are, its sides at most 3.6 m"""
    points = []
    for o in optics:
        position = np.asarray(o["mount"]["position"], dtype=np.float64)
        points.append(position)
        if o["type"] == "laser":
            points.append(position - LASER_HOUSING * np.asarray(vkscene.quat_rotate(o["mount"]["orientation"], (0.0, 0.0, 1.0))))
    for s in stacks:
        points += [np.asarray(s["start"]), np.asarray(s["end"])]
    reach = np.max(np.abs(np.array(points)), axis=0) if points else np.zeros(3)
    size = [min(2.0 * (reach[axis] + BREADBOARD_MARGIN), BREADBOARD_MAX) for axis in (0, 2)]
    return {"table": True, "size": [round(v / 0.025) * 0.025 for v in size], "beamHeight": 0.1, "snap": True}


def measure_detector(readings, p):
    """A detector's Measurement from the batch mode's readings (vkscene.run_detectors()): its power, and the centroid and
    the second-moment radii the renderer's ISO 11146 code measured, along the probe's u and v"""
    entry, film = readings[p.name]
    area = entry["pixelSize"][0] * entry["pixelSize"][1]
    centroid = tuple(entry.get("centroid", (0.0, 0.0)))
    diameter = entry.get("diameter", (0.0, 0.0))
    return Measurement(entry["power"], centroid, (0.5 * diameter[0], 0.5 * diameter[1]), film / area)


def scene(shapes, materials, optics, cam, width, height, *, samples=64, integrator="light", render_extra=None, bench=None, render_detector=None, stacks=()):
    """A scene file for a measurement: light tracing, a fixed seed, a box pixel filter, no background light, on a
    breadboard under everything (breadboard(), or `bench`), with the sensor `stacks` (detector_stack()'s records). With
    `render_detector`, the name of a detector among `optics`, the render target is that detector (and `cam` may be None:
    a scene with no camera at all)"""
    render = {"width": width, "height": height, "matchViewport": False, "antialiasing": True, "maxSamples": samples,
              "unlimitedSamples": False, "rayDepth": 16, "useFixedSeed": True, "seed": 1, "samplesPerFrame": 16,
              "russianRoulette": False, "minBouncesBeforeRoulette": 4, "restartOnChange": True,
              "pixelFilter": {"kind": "box", "radius": 0.5}}
    render.update(render_extra or {})
    extras = {"version": SCENE_VERSION, "models": [], "capabilities": ["optical_bench"], "environmentIntensity": 1.0, "render": render,
              "renderCamera": 0 if cam is not None else -1, "cameras": [cam] if cam is not None else [], "lights": [], "materials": materials,
              "materialOverrides": [], "geometry": shapes,
              "meshObjects": [], "optics": optics, "lengthUnit": "mm", "background": [0.0, 0.0, 0.0], "backgroundMode": "solid",
              "integrator": integrator}
    extras["bench"] = bench if bench is not None else breadboard(optics, stacks)
    if stacks:
        extras["sensorStacks"] = list(stacks)
    if render_detector is not None:
        extras["renderDetector"] = [o["name"] for o in optics].index(render_detector)
    view = cam or vkscene.camera((0.3, 0.3, -0.3), (0.0, 0.1, 0.3), 40.0)
    extras["viewport"] = {"position": view["position"], "orientation": view["orientation"], "pivot": [0.0, 0.0, 0.0], "speed": 0.5,
                          "fov": max(float(view["fov"]), 10.0), "near": 0.001, "far": 100.0}
    return {"asset": {"generator": "vulkan_guide", "version": "2.0"}, "nodes": [{"name": "vulkan_guide_scene"}], "scene": 0,
            "scenes": [{"nodes": [0], "extras": extras, "name": "vulkan_guide scene"}], "extensions": {}}


WHITE = {"name": "white_screen", "type": "lambertian", "albedo": [1.0, 1.0, 1.0]}


@dataclass
class Measurement:
    power: float
    centroid: tuple
    radius: tuple
    # the intensity on the screen (W/m^2), row 0 at the top
    irradiance: np.ndarray


def measure(image, p, channel_weights=(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0), scale=1.0):
    """The beam on the probe's screen from the render's linear image: the irradiance pi L of the weighted channels (by
    default their mean, which exact RGB makes a watt per watt of a laser), times `scale`; the power, the centroid and the
    second-moment radii along u and v"""
    radiance = image[..., :3] @ np.asarray(channel_weights)
    irradiance = math.pi * radiance * scale
    uu, vv = p.coordinates()
    area = p.pixel_area()
    power = irradiance.sum() * area
    if not power > 0.0:
        return Measurement(0.0, (0.0, 0.0), (0.0, 0.0), irradiance)
    cu = (irradiance * uu).sum() * area / power
    cv = (irradiance * vv).sum() * area / power
    wu = 2.0 * math.sqrt((irradiance * (uu - cu) ** 2).sum() * area / power)
    wv = 2.0 * math.sqrt((irradiance * (vv - cv) ** 2).sum() * area / power)
    return Measurement(power, (cu, cv), (wu, wv), irradiance)


# ---------------------------------------------------------------- Gaussian beams through ABCD systems

def free_space(d):
    return np.array([[1.0, d], [0.0, 1.0]])


def thin_lens_matrix(f):
    return np.array([[1.0, 0.0], [-1.0 / f, 1.0]])


def refraction(radius, n1, n2):
    """a spherical surface of radius `radius` (positive with its center of curvature after it), from index n1 to n2, for
    the ray (height, angle)"""
    power = 0.0 if radius == math.inf else (n2 - n1) / radius
    return np.array([[1.0, 0.0], [-power / n2, n1 / n2]])


def mirror(focal_length):
    return np.array([[1.0, 0.0], [-1.0 / focal_length, 1.0]])


def chain(*matrices):
    """the system of `matrices` in the order light meets them"""
    m = np.eye(2)
    for step in matrices:
        m = step @ m
    return m


def propagate_q(waist, m2, wavelength, distance_from_waist, system):
    """The embedded Gaussian's complex beam parameter (lambda replaced by M^2 lambda, Siegman), `distance_from_waist` past
    its waist and then through `system`: its waist radius after the system and the waist's position after the system's
    last plane (negative: before it), with the system's output space index 1"""
    lam = m2 * wavelength
    q = distance_from_waist + 1j * math.pi * waist ** 2 / lam
    (a, b), (c, d) = system
    q2 = (a * q + b) / (c * q + d)
    # q = z + i z_R past the waist: the waist is -Re(q) further on
    z_r = q2.imag
    return math.sqrt(z_r * lam / math.pi), -q2.real


def beam_radius_after(waist, m2, wavelength, distance_from_waist, system):
    lam = m2 * wavelength
    q = distance_from_waist + 1j * math.pi * waist ** 2 / lam
    (a, b), (c, d) = system
    q2 = (a * q + b) / (c * q + d)
    inverse = 1.0 / q2
    return math.sqrt(-lam / (math.pi * inverse.imag))


def fit_waist(z, w, wavelength):
    """The waist radius, its position and M^2 from radii w at positions z: w^2 = w0^2 + theta^2 (z - z0)^2, by least
    squares on w^2 = a + b z + c z^2 (ISO 11146's fit)"""
    z = np.asarray(z, dtype=np.float64)
    w2 = np.asarray(w, dtype=np.float64) ** 2
    c, b, a = np.polyfit(z, w2, 2)
    z0 = -b / (2.0 * c)
    w0 = math.sqrt(max(a - b * b / (4.0 * c), 0.0))
    theta = math.sqrt(c)
    return w0, z0, math.pi * w0 * theta / wavelength


def cie_y(lambda_nm):
    """CIE 1931's y-bar at the nearest whole nanometer, as the film reads it (crt_spectrum.glsl cieAt()), from
    src/spectrum_tables.cpp"""
    text = (vkscene.ROOT / "src" / "spectrum_tables.cpp").read_text()
    start = text.index("CIE_Y[CIE_SAMPLES] = {") + len("CIE_Y[CIE_SAMPLES] = {")
    values = [float(v.strip().rstrip("f")) for v in text[start:text.index("}", start)].split(",") if v.strip()]
    return values[int(round(lambda_nm)) - 360]


CIE_Y_INTEGRAL = 106.856895
