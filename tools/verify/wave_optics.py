"""A scalar wave-optics reference for the laser sources (docs/plans/completed/physoptics-laser-sources.md): the beam a laser
element describes, propagated along its axis by diffraction, through masks and thin lenses, for comparing with what the
renderer's rays do.

The renderer draws a laser's rays from the beam's Wigner function (src/light_sampling.h LaserEmitter), which is exact for
a Gaussian beam in free space and through paraxial optics, and wrong wherever diffraction matters: a hard aperture's
rings, the edge of a mask. This module is the other side of that comparison. It follows diffractsim's angular spectrum
method (Rafael de la Fuente, MPL 2.0), with three additions:

  - M^2 above 1. A beam of M^2 > 1 is a Gaussian Schell-model beam (Gori 1980; Mandel and Wolf 1995, section 5.8): an
    incoherent sum of Hermite-Gaussian modes of one waist with weights (1 - q) q^n. Its intensity follows
    w(z)^2 = w0^2 + (M^2 lambda / (pi w0))^2 z^2 exactly, and its Wigner function is the Gaussian the renderer samples,
    so in free space the rays and the waves must agree to within the rays' noise.
  - astigmatism: x and y are independent beams, each with its waist, waist position and M^2, as the laser element has.
  - the band limit of Matsushima and Shimobaba 2009 on the transfer function, which keeps a long propagation on a coarse
    grid from aliasing.

Units are SI throughout: meters, the wavelength too. The field is sampled on a square grid centered on the beam's axis.
Free-space intensity has a closed form (beam_intensity_1d()); a mask or a lens needs the numerical propagation
(propagate()), mode by mode in 2D.

Run the self-test, which checks this module against closed forms (and against diffractsim when it can be imported):

    python3 tools/verify/wave_optics.py
    /Users/kobihall/Documents/Code/Python-venvs/Laser-Beam-Sims/.venv/bin/python tools/verify/wave_optics.py
"""

from dataclasses import dataclass, field
import math
import sys

import numpy as np


# ---------------------------------------------------------------- the beam

@dataclass
class AxisBeam:
    """One transverse axis of a beam: the 1/e^2 intensity radius at the waist, the waist's position along the beam from
    the aperture plane (negative inside the housing), and M^2"""
    waist: float
    waist_position: float = 0.0
    m2: float = 1.0

    def half_angle(self, wavelength):
        return self.m2 * wavelength / (math.pi * self.waist)

    def radius(self, z, wavelength):
        """the 1/e^2 (second-moment) radius at `z` from the aperture plane"""
        d = z - self.waist_position
        return math.sqrt(self.waist ** 2 + (self.half_angle(wavelength) * d) ** 2)


@dataclass
class LaserBeam:
    """A laser element's beam, as optic_laser.cpp reads its parameters: x and y may differ"""
    wavelength: float
    x: AxisBeam
    y: AxisBeam
    power: float = 1.0

    @staticmethod
    def round(wavelength, waist, waist_position=0.0, m2=1.0, power=1.0):
        axis = AxisBeam(waist, waist_position, m2)
        return LaserBeam(wavelength, axis, AxisBeam(waist, waist_position, m2), power)

    @staticmethod
    def hnl050lb():
        """Thorlabs' HNL050LB: 0.81 mm and 1.0 mrad at 632.8 nm, its waist at the output, 5 mW"""
        w0 = 0.405e-3
        return LaserBeam.round(632.8e-9, w0, 0.0, math.pi * w0 * 0.5e-3 / 632.8e-9, 5e-3)

    def axes(self):
        return (self.x, self.y)


# ---------------------------------------------------------------- the coherent modes

def hermite_functions(n_max, xi):
    """h_0 .. h_n_max at xi: the normalized Hermite functions, (2^n n! sqrt(pi))^-1/2 H_n(xi) exp(-xi^2 / 2), by their
    stable recurrence"""
    h = np.empty((n_max + 1,) + np.shape(xi))
    h[0] = math.pi ** -0.25 * np.exp(-0.5 * xi * xi)
    if n_max >= 1:
        h[1] = math.sqrt(2.0) * xi * h[0]
    for n in range(2, n_max + 1):
        h[n] = math.sqrt(2.0 / n) * xi * h[n - 1] - math.sqrt((n - 1) / n) * h[n - 2]
    return h


def schell_modes(axis, threshold=1e-7, max_modes=200):
    """The coherent-mode expansion of one axis as a Gaussian Schell-model beam: the weights (summing to 1) and the
    common 1/e^2 waist of its Hermite-Gaussian modes. The intensity's rms width is sigma_s = w0 / 2 and the coherence
    width sigma_g = 2 sigma_s / sqrt(M^4 - 1), so the far-field spread is M^2 times a coherent Gaussian's. With
    a = 1 / (4 sigma_s^2), b = 1 / (2 sigma_g^2) and c = sqrt(a^2 + 2 a b), the modes are HG_n of waist 1 / sqrt(c) and
    weights proportional to q^n, q = b / (a + b + c). M^2 = 1 is one mode, the Gaussian itself. The tail below
    `threshold` is dropped and the rest renormalized"""
    if axis.m2 <= 1.0 + 1e-12:
        return np.array([1.0]), axis.waist
    sigma_s = 0.5 * axis.waist
    sigma_g = 2.0 * sigma_s / math.sqrt(axis.m2 ** 2 - 1.0)
    a = 1.0 / (4.0 * sigma_s ** 2)
    b = 1.0 / (2.0 * sigma_g ** 2)
    c = math.sqrt(a * a + 2.0 * a * b)
    q = b / (a + b + c)
    count = min(max_modes, max(1, int(math.ceil(math.log(threshold) / math.log(q)))))
    weights = (1.0 - q) * q ** np.arange(count)
    return weights / weights.sum(), 1.0 / math.sqrt(c)


def mode_field(n_max, mode_waist, wavelength, x, distance):
    """The Hermite-Gaussian modes 0 .. n_max of one axis, each normalized to unit power per unit length of the other
    axis, at `distance` past their waist, sampled at x: a (n_max + 1, len(x)) complex array. The fields are
    exp(i (k z - omega t)) waves: a diverging beam's phase grows as k x^2 / (2 R), and the Gouy phase lags as
    (n + 1/2) atan(z / z_R) in one dimension"""
    k = 2.0 * math.pi / wavelength
    z_r = math.pi * mode_waist ** 2 / wavelength
    w = mode_waist * math.sqrt(1.0 + (distance / z_r) ** 2)
    curvature = distance / (distance ** 2 + z_r ** 2)
    gouy = math.atan2(distance, z_r)
    h = hermite_functions(n_max, math.sqrt(2.0) * np.asarray(x) / w)
    n = np.arange(n_max + 1)[:, None]
    phase = np.exp(1j * 0.5 * k * curvature * np.asarray(x) ** 2)[None, :] * np.exp(-1j * (n + 0.5) * gouy)
    return math.sqrt(math.sqrt(2.0) / w) * h * phase


def beam_intensity_1d(axis, wavelength, x, z):
    """The closed form of one axis's intensity profile at `z` from the aperture, integrating to 1: the mode weights
    times the modes' intensities, which free space leaves incoherent"""
    weights, mode_waist = schell_modes(axis)
    modes = mode_field(len(weights) - 1, mode_waist, wavelength, x, z - axis.waist_position)
    return (weights[:, None] * np.abs(modes) ** 2).sum(axis=0)


# ---------------------------------------------------------------- propagation

def transfer_function(shape, dx, wavelength, z, band_limit=True):
    """The angular spectrum's transfer function over `z` on an FFT grid of spacing dx: exp(i kz z), the evanescent
    waves decaying, and frequencies beyond Matsushima's limit 1 / (lambda sqrt((2 df z)^2 + 1)) dropped, where the
    sampled phase would alias"""
    ny, nx = shape
    fx = np.fft.fftfreq(nx, dx)
    fy = np.fft.fftfreq(ny, dx)
    fxx, fyy = np.meshgrid(fx, fy)
    argument = 1.0 / wavelength ** 2 - fxx ** 2 - fyy ** 2
    root = np.sqrt(np.abs(argument))
    h = np.where(argument >= 0.0, np.exp(2j * math.pi * root * z), np.exp(-2.0 * math.pi * root * abs(z)))
    if band_limit and z != 0.0:
        for f, n in ((fxx, nx), (fyy, ny)):
            df = 1.0 / (n * dx)
            limit = 1.0 / (wavelength * math.sqrt((2.0 * df * z) ** 2 + 1.0))
            h = np.where(np.abs(f) <= limit, h, 0.0)
    return h


def angular_spectrum(field, dx, wavelength, z, band_limit=True):
    """The field `z` further along: diffractsim's angular_spectrum_method on a fixed grid, with the band limit"""
    if z == 0.0:
        return field
    return np.fft.ifft2(np.fft.fft2(field) * transfer_function(field.shape, dx, wavelength, z, band_limit))


@dataclass
class Grid:
    """A square grid of n x n samples, `extent` across, centered on the beam's axis; x runs along the beam's x axis and
    y along its y axis (row 0 at the most negative y)"""
    n: int
    extent: float

    @property
    def dx(self):
        return self.extent / self.n

    @property
    def x(self):
        return (np.arange(self.n) - self.n // 2) * self.dx

    def mesh(self):
        return np.meshgrid(self.x, self.x)


@dataclass
class Mask:
    """An amplitude (or complex) transmittance at `z` from the aperture, a function of the grid's (X, Y)"""
    z: float
    transmittance: object
    name: str = "mask"


def circular_aperture(z, radius, center=(0.0, 0.0)):
    return Mask(z, lambda xx, yy: ((xx - center[0]) ** 2 + (yy - center[1]) ** 2 <= radius ** 2).astype(np.float64), f"aperture r={radius:g}")


def thin_lens(z, focal_length, wavelength):
    """A paraxial thin lens of focal length f: the phase exp(-i k r^2 / (2 f))"""
    k = 2.0 * math.pi / wavelength
    return Mask(z, lambda xx, yy: np.exp(-0.5j * k * (xx ** 2 + yy ** 2) / focal_length), f"lens f={focal_length:g}")


@dataclass
class PropagationResult:
    """The intensity at each requested plane (W/m^2 for the beam's power), on the grid"""
    grid: Grid
    planes: list
    intensity: list = field(default_factory=list)


def propagate(beam, grid, planes, masks=(), weight_floor=1e-6, band_limit=True):
    """The beam's intensity at each distance in `planes` from the aperture, past the `masks` before it. Each pair of x and
    y coherent modes (schell_modes()) starts at the aperture plane from its closed form, propagates by the angular
    spectrum through every mask in order, and adds its weight times its intensity; pairs weighing less than
    `weight_floor` of the beam are left out (and the rest renormalized). Without masks, beam_intensity_1d() gives the
    same profiles without a grid"""
    wx, mwx = schell_modes(beam.x)
    wy, mwy = schell_modes(beam.y)
    ux = mode_field(len(wx) - 1, mwx, beam.wavelength, grid.x, -beam.x.waist_position)
    uy = mode_field(len(wy) - 1, mwy, beam.wavelength, grid.x, -beam.y.waist_position)
    pairs = [(i, j, wx[i] * wy[j]) for i in range(len(wx)) for j in range(len(wy)) if wx[i] * wy[j] >= weight_floor]
    total = sum(p[2] for p in pairs)
    xx, yy = grid.mesh()
    ordered = sorted(masks, key=lambda m: m.z)
    result = PropagationResult(grid, list(planes))
    result.intensity = [np.zeros((grid.n, grid.n)) for _ in planes]
    for i, j, weight in pairs:
        field0 = uy[j][:, None] * ux[i][None, :]
        for p, z_out in enumerate(planes):
            f = field0
            z = 0.0
            for mask in ordered:
                if mask.z >= z_out:
                    break
                f = angular_spectrum(f, grid.dx, beam.wavelength, mask.z - z, band_limit) * mask.transmittance(xx, yy)
                z = mask.z
            f = angular_spectrum(f, grid.dx, beam.wavelength, z_out - z, band_limit)
            result.intensity[p] += (weight / total) * np.abs(f) ** 2
    result.intensity = [beam.power * i for i in result.intensity]
    return result


# ---------------------------------------------------------------- measurements

def moments(intensity, grid):
    """The power, the centroid and the second-moment (1/e^2 for a Gaussian, ISO 11146) radii along x and y of an
    intensity on the grid"""
    xx, yy = grid.mesh()
    a = grid.dx ** 2
    power = intensity.sum() * a
    cx = (intensity * xx).sum() * a / power
    cy = (intensity * yy).sum() * a / power
    wx = 2.0 * math.sqrt((intensity * (xx - cx) ** 2).sum() * a / power)
    wy = 2.0 * math.sqrt((intensity * (yy - cy) ** 2).sum() * a / power)
    return power, (cx, cy), (wx, wy)


def profile_moments(profile, x):
    """one axis's power, centroid and second-moment radius from a 1D profile"""
    dx = x[1] - x[0]
    power = profile.sum() * dx
    c = (profile * x).sum() * dx / power
    return power, c, 2.0 * math.sqrt((profile * (x - c) ** 2).sum() * dx / power)


def resample(intensity, grid, x_out, y_out):
    """`intensity` bilinearly sampled at the points of another grid's axes x_out, y_out (for comparing with a render)"""
    from numpy import interp
    rows = np.array([interp(x_out, grid.x, row, left=0.0, right=0.0) for row in intensity])
    return np.array([interp(y_out, grid.x, rows[:, c], left=0.0, right=0.0) for c in range(rows.shape[1])]).T


# ---------------------------------------------------------------- the self-test

def self_test(verbose=True):
    """Checks this module against closed forms. Returns the number of failures"""
    failures = 0

    def check(ok, what):
        nonlocal failures
        if not ok:
            failures += 1
            print(f"  FAIL {what}")

    lam = 632.8e-9
    # the mode expansion: weights sum to 1, the intensity has the beam's second-moment radius at every distance, and
    # integrates to 1, for M^2 = 1, 2 and 5
    for m2 in (1.0, 2.0, 5.0):
        axis = AxisBeam(0.4e-3, -0.05, m2)
        z_r = axis.waist / axis.half_angle(lam)
        worst = 0.0
        for k in (0.0, 1.0, 10.0):
            z = axis.waist_position + k * z_r
            w = axis.radius(z, lam)
            x = np.linspace(-6 * w, 6 * w, 6001)
            power, centroid, radius = profile_moments(beam_intensity_1d(axis, lam, x, z), x)
            worst = max(worst, abs(radius / w - 1.0), abs(power - 1.0))
        if verbose:
            print(f"  closed form, M^2 = {m2:g}: {len(schell_modes(axis)[0])} modes, radii and power within {worst:.2e} of w(z)")
        check(worst < 1e-5, f"closed form M^2 = {m2}: radius or power {worst:.2e} off")

    # the numerical propagation against the closed form: a coherent Gaussian and an M^2 = 2 astigmatic beam, from the
    # aperture to one and four of x's Rayleigh ranges, on a 512 grid wide enough for both
    beam = LaserBeam(lam, AxisBeam(0.4e-3, 0.0, 1.0), AxisBeam(0.6e-3, -0.1, 2.0))
    grid = Grid(512, 16e-3)
    z_r = beam.x.waist / beam.x.half_angle(lam)
    planes = [z_r, 4.0 * z_r]
    result = propagate(beam, grid, planes)
    worst_profile = 0.0
    worst_radius = 0.0
    for z, intensity in zip(planes, result.intensity):
        power, centroid, (wx, wy) = moments(intensity, grid)
        worst_radius = max(worst_radius, abs(wx / beam.x.radius(z, lam) - 1.0), abs(wy / beam.y.radius(z, lam) - 1.0), abs(power - beam.power))
        exact = np.outer(beam_intensity_1d(beam.y, lam, grid.x, z), beam_intensity_1d(beam.x, lam, grid.x, z))
        worst_profile = max(worst_profile, np.abs(intensity - exact).max() / exact.max())
    if verbose:
        print(f"  angular spectrum, astigmatic M^2 = 1 x 2: radii and power within {worst_radius:.2e}, profiles within {worst_profile:.2e} of the peak")
    check(worst_radius < 1e-3, f"angular spectrum: a radius or the power is {worst_radius:.2e} off")
    check(worst_profile < 1e-4, f"angular spectrum: a profile is {worst_profile:.2e} of its peak off the closed form")

    # a mask: a plane wave through a circular hole, its far field's first dark ring at 1.22 lambda z / D, and the power
    # through the hole the power on it
    plane = LaserBeam.round(lam, 1.0, 0.0, 1.0)
    grid = Grid(1024, 20e-3)
    radius = 0.5e-3
    # five times a^2 / lambda past the hole: far enough for the Airy pattern's first zero to within a percent
    z = 2.0
    hole = circular_aperture(0.0, radius)
    result = propagate(plane, grid, [1e-9, z], masks=[Mask(0.0, hole.transmittance)])
    near, far = result.intensity
    xx, yy = grid.mesh()
    inside = hole.transmittance(xx, yy) > 0
    u = mode_field(0, plane.x.waist, lam, grid.x, 0.0)[0]
    power_in = (np.abs(np.outer(u, u)) ** 2)[inside].sum() * grid.dx ** 2
    power_near = near.sum() * grid.dx ** 2
    centre = grid.n // 2
    row = far[centre, centre:]
    r = grid.x[centre:]
    first = next(i for i in range(2, len(row) - 1) if row[i] <= row[i - 1] and row[i] <= row[i + 1])
    ring = r[first]
    expected = 1.22 * lam * z / (2 * radius)
    if verbose:
        print(f"  circular aperture: first dark ring at {ring * 1e3:.4f} mm, 1.22 lambda z / D = {expected * 1e3:.4f} mm; power through it {power_near / power_in:.6f} of the power on it")
    check(abs(ring / expected - 1.0) < 0.03, f"circular aperture: first dark ring {ring:.4e}, expected {expected:.4e}")
    check(abs(power_near / power_in - 1.0) < 1e-3, f"circular aperture: {power_near / power_in:.5f} of the power on it passes")

    # against diffractsim's own angular spectrum method, when it can be imported (its venv)
    try:
        import diffractsim
        from diffractsim.propagation_methods.angular_spectrum_method import angular_spectrum_method
    except Exception:
        if verbose:
            print("  diffractsim: not importable here, skipped (run with its venv's python to compare)")
    else:
        diffractsim.set_backend("CPU")
        n = 256
        grid = Grid(n, 8e-3)

        class Simulation:
            Nx = n
            Ny = n
            dx = grid.dx
            dy = grid.dx

        xx, yy = grid.mesh()
        e0 = np.exp(-(xx ** 2 + yy ** 2) / (0.4e-3) ** 2).astype(complex)
        z = 0.5
        theirs = np.abs(angular_spectrum_method(Simulation(), e0, z, lam)) ** 2
        ours = np.abs(angular_spectrum(e0, grid.dx, lam, z, band_limit=False)) ** 2
        difference = np.abs(theirs - ours).max() / theirs.max()
        if verbose:
            print(f"  diffractsim {getattr(diffractsim, '__version__', '')}: its angular spectrum and this one differ by {difference:.2e} of the peak")
        check(difference < 1e-9, f"diffractsim: intensities differ by {difference:.2e} of the peak")
    return failures


if __name__ == "__main__":
    print("wave optics self-test:")
    failed = self_test()
    print("PASS" if failed == 0 else f"FAIL ({failed})")
    sys.exit(0 if failed == 0 else 1)
