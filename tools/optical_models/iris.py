"""An iris's blades as parts that move: their pivots, and how far each turns as the lever does.

Run inside FreeCAD's Python, from convert.py. Every iris in assets/optical_models/ is built the same way: each
blade turns about a pin fixed in the housing (its pivot), and a second pin on the blade rides in a radial slot
of the actuator ring that the lever turns (ID36/M has a pocket instead of a slot, which this treats as one). A
radial slot keeps the drive pin at the ring's angle, so when the lever turns by phi the blade turns by the theta
that moves its drive pin's polar angle by phi. The aperture is the largest circle about the axis that no blade
covers. The lever's range is where the aperture reaches the smallest and largest diameters Thorlabs gives
(the manifest's "aperture_range"): the files model no stop that the lever reaches first.

Everything is in the canonical frame (millimeters here, x and y across the axis), angles right-handed about
the iris's axis, +z.
"""

import math

import FreeCAD

V = FreeCAD.Vector

# the samples of the drive's table, from closed to open
SAMPLES = 17


def _pin(solid, frame, near):
    """The axis of the blade's pin (or round corner) nearest a point, in frame millimeters (x, y, z)."""
    best = None
    for f in solid.Faces:
        s = f.Surface
        if type(s).__name__ != "Cylinder" or s.Radius > 2.0 or abs(V(s.Axis).dot(frame.z)) < 0.999:
            continue
        p = f.CenterOfMass
        c = V(s.Center)
        radial = (p - c) - frame.z * (p - c).dot(frame.z)
        if f.normalAt(*s.parameter(p)).dot(radial) <= 0:
            continue  # a hole, not a pin
        d = ((c - near) - frame.z * (c - near).dot(frame.z)).Length
        if best is None or d < best[0] - 1e-9:
            best = (d, c, s.Radius)
    if best is None or best[0] > 0.5:
        raise ValueError("no pin near %s" % list(near))
    c = best[1]
    q = c - frame.origin
    return (q.dot(frame.x), q.dot(frame.y)), best[2]


def _rotate(p, about, angle):
    c, s = math.cos(angle), math.sin(angle)
    x, y = p[0] - about[0], p[1] - about[1]
    return (about[0] + c * x - s * y, about[1] + s * x + c * y)


def _polar(p):
    return math.atan2(p[1], p[0])


def _segment_distance(p, a, b):
    ax, ay = b[0] - a[0], b[1] - a[1]
    t = max(0.0, min(1.0, ((p[0] - a[0]) * ax + (p[1] - a[1]) * ay) / max(1e-30, ax * ax + ay * ay)))
    return math.hypot(a[0] + ax * t - p[0], a[1] + ay * t - p[1])


def _inside(p, a, b, c):
    def side(u, v, w):
        return (v[0] - u[0]) * (w[1] - u[1]) - (v[1] - u[1]) * (w[0] - u[0])
    d1, d2, d3 = side(a, b, p), side(b, c, p), side(c, a, p)
    return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))


class Iris:
    """The blades of an iris, the pivot of each, and the drive: lever angle -> blade angle -> aperture."""

    def __init__(self, src, frame, spec, blade_solids):
        self.frame = frame
        first = src.solids[blade_solids[0]]
        pivot, pivot_r = _pin(first, frame, V(*spec["pivot_near"]))
        drive, _ = _pin(first, frame, V(*spec["drive_near"]))
        self.pivot0, self.drive0 = pivot, drive
        # each blade's angle about the axis from the first: where its pivot pin is
        self.blades = []
        for i in blade_solids:
            so = src.solids[i]
            found = None
            for f in so.Faces:
                s = f.Surface
                if type(s).__name__ != "Cylinder" or abs(s.Radius - pivot_r) > 1e-3 or \
                        abs(V(s.Axis).dot(frame.z)) < 0.999:
                    continue
                q = V(s.Center) - frame.origin
                xy = (q.dot(frame.x), q.dot(frame.y))
                if abs(math.hypot(*xy) - math.hypot(*pivot)) < 0.05:
                    found = xy
                    break
            if found is None:
                raise ValueError("blade solid %d has no pivot pin at the first blade's radius" % i)
            mid = so.BoundBox.Center
            # the blade's mid-plane: the middle of its two large flat faces
            flats = [f for f in so.Faces if type(f.Surface).__name__ == "Plane" and f.Area > 0.2 * so.Area / 2]
            if flats:
                mid = sum((f.CenterOfMass for f in flats), V()) * (1.0 / len(flats))
            z = (mid - frame.origin).dot(frame.z)
            self.blades.append({"solid": i, "pivot": found, "angle": _polar(found) - _polar(pivot), "z": z})
        self.blades.sort(key=lambda b: (round(b["angle"] % (2 * math.pi), 6), b["solid"]))
        # the first blade's outline across the axis, for the aperture
        mesh = first.tessellate(0.01)
        pts = [(p - frame.origin) for p in mesh[0]]
        self.outline = [((pts[a].dot(frame.x), pts[a].dot(frame.y)), (pts[b].dot(frame.x), pts[b].dot(frame.y)),
                         (pts[c].dot(frame.x), pts[c].dot(frame.y))) for a, b, c in mesh[1]]

    def blade_angle(self, phi):
        """The blade's turn about its pivot that moves its drive pin's polar angle by phi."""
        target = phi

        def f(theta):
            q = _rotate(self.drive0, self.pivot0, theta)
            d = _polar(q) - _polar(self.drive0)
            return math.atan2(math.sin(d), math.cos(d)) - target
        lo, hi = -1.5, 1.5
        flo = f(lo)
        if flo * f(hi) > 0:
            raise ValueError("the blade cannot follow a lever turn of %.3f rad" % phi)
        for _ in range(60):
            mid = (lo + hi) * 0.5
            fm = f(mid)
            if (fm < 0) == (flo < 0):
                lo, flo = mid, fm
            else:
                hi = mid
        return (lo + hi) * 0.5

    def aperture(self, theta):
        """The aperture's diameter (mm) with every blade turned by theta: twice the distance from the axis to the
        first blade, which the others repeat about the axis."""
        best = None
        for a, b, c in self.outline:
            a, b, c = (_rotate(p, self.pivot0, theta) for p in (a, b, c))
            if _inside((0.0, 0.0), a, b, c):
                return 0.0
            d = min(_segment_distance((0.0, 0.0), a, b), _segment_distance((0.0, 0.0), b, c),
                    _segment_distance((0.0, 0.0), c, a))
            best = d if best is None else min(best, d)
        return 2 * best

    def _reach(self, target, sign):
        """The smallest lever turn in the direction `sign` at which the aperture reaches `target` (mm), or, if it
        never does within 120 degrees, the turn at which it comes nearest. Returns (phi, aperture)."""
        def ap(phi):
            return self.aperture(self.blade_angle(phi))

        def reached(a):
            return a >= target if sign > 0 else a <= target
        step = math.radians(1.0)
        best = (0.0, ap(0.0))
        phi = 0.0
        while abs(phi) < math.radians(120):
            nxt = phi + sign * step
            try:
                a = ap(nxt)
            except ValueError:
                break
            if reached(a):
                lo, hi = phi, nxt
                for _ in range(30):
                    mid = (lo + hi) * 0.5
                    if reached(ap(mid)):
                        hi = mid
                    else:
                        lo = mid
                return hi, ap(hi)
            if (a > best[1]) if sign > 0 else (a < best[1]):
                best = (nxt, a)
            phi = nxt
        return best

    def lever_range(self, aperture_range):
        """The lever's turns (radians from rest) that close the iris to the smallest diameter and open it to the
        largest, and the apertures (mm) the model reaches there."""
        closed = self._reach(aperture_range[0], -1)
        opened = self._reach(aperture_range[1], 1)
        return closed, opened

    def table(self, aperture_range):
        """The drive: lever angles from closed to open, each blade's angle and the aperture, sorted from the
        smallest aperture to the largest. Returns (rows of (aperture mm, phi, theta), (closed, opened)) where
        each of those is (phi, the aperture the model reaches there)."""
        closed, opened = self.lever_range(aperture_range)
        phi0, phi1 = closed[0], opened[0]
        rows = []
        for n in range(SAMPLES):
            phi = phi0 + (phi1 - phi0) * n / (SAMPLES - 1)
            theta = self.blade_angle(phi)
            rows.append((self.aperture(theta), phi, theta))
        rows.sort()
        return rows, (closed, opened)
