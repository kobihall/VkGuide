"""Measuring a laser beam in a render, until detectors exist (docs/plans/physoptics-detectors.md): a white Lambertian screen
square to the beam, and a pinhole camera on the beam's axis a short way in front of it, looking at it. The camera has no
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
SCENE_VERSION = 28


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


def free_optic(kind, name, position, z_axis, up=(0.0, 1.0, 0.0), size=None, params=None, materials=None):
    """An optic placed freely: its body origin at `position`, its +z along `z_axis`"""
    x, y, z = frame_from_z(z_axis, up)
    record = {"type": kind, "name": name, "params": params or {}, "materials": materials or {},
              "mount": {"kind": "none", "position": [float(c) for c in position], "orientation": [float(c) for c in vkscene.quat_from_matrix(np.column_stack([x, y, z]))]}}
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
    them (u to the image's right, v up), the half-field across u and v, and the image's size"""
    center: np.ndarray
    direction: np.ndarray
    u: np.ndarray
    v: np.ndarray
    half_u: float
    half_v: float
    width: int
    height: int
    distance: float

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


def scene(shapes, materials, optics, cam, width, height, *, samples=64, integrator="light", render_extra=None, bench=None):
    """A scene file for a measurement: light tracing, a fixed seed, a box pixel filter, no background light"""
    render = {"width": width, "height": height, "matchViewport": False, "antialiasing": True, "maxSamples": samples,
              "unlimitedSamples": False, "rayDepth": 16, "useFixedSeed": True, "seed": 1, "samplesPerFrame": 16,
              "russianRoulette": False, "minBouncesBeforeRoulette": 4, "restartOnChange": True,
              "pixelFilter": {"kind": "box", "radius": 0.5}}
    render.update(render_extra or {})
    extras = {"version": SCENE_VERSION, "models": [], "capabilities": ["optical_bench"], "environmentIntensity": 1.0, "render": render,
              "renderCamera": 0, "cameras": [cam], "lights": [], "materials": materials, "materialOverrides": [], "geometry": shapes,
              "meshObjects": [], "optics": optics, "lengthUnit": "mm", "background": [0.0, 0.0, 0.0], "backgroundMode": "solid",
              "integrator": integrator}
    if bench is not None:
        extras["bench"] = bench
    extras["viewport"] = {"position": cam["position"], "orientation": cam["orientation"], "pivot": [0.0, 0.0, 0.0], "speed": 0.5,
                          "fov": max(float(cam["fov"]), 10.0), "near": 0.001, "far": 100.0}
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
