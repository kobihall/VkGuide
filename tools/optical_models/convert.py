#!/Applications/FreeCAD.app/Contents/Resources/bin/python
"""Converts the Thorlabs STEP files in assets/optical_models/ into the meshes the renderer draws.

Run with FreeCAD's Python, which imports FreeCAD as a module:

  /Applications/FreeCAD.app/Contents/Resources/bin/python tools/optical_models/convert.py [--model KM100 ...]
  ... convert.py --describe Kinematic_mount-KM100.step [--solid 9]     the faces a manifest entry refers to
  ... convert.py --check-example                                       the mates against example mirror.FCStd

For each manifest entry it writes assets/optical_models/meshes/<model>.glb and <model>.mates.json. The steps
follow docs/plans/completed/physoptics-optomechanical-models.md ("As built"): import with the STEP file's colors,
canonical frame, parts (an iris's blades each its own, driven by its lever: iris.py), finishes, proxies (a spring
or a thread becomes a cylinder), tessellation with the lettering flattened into the faces it is cut into
(decals.py), hidden-geometry removal (bin/mesh_visibility), coarser levels for the solids that save the most until
the budget is met, the decal atlas the lettering, springs and threads are baked into, the write, and two
silhouette checks: that removing hidden geometry changed no view, and that flattening left no hole.
"""

import argparse
import collections
import json
import math
import os
import struct
import subprocess
import sys

FREECAD_LIB = "/Applications/FreeCAD.app/Contents/Resources/lib"
if FREECAD_LIB not in sys.path:
    sys.path.append(FREECAD_LIB)

import FreeCAD  # noqa: E402
import Part  # noqa: E402
import MeshPart  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from step_styles import StepStyles, match_solids  # noqa: E402
import decals  # noqa: E402
import glb  # noqa: E402
import iris  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
STEP_DIR = os.path.join(ROOT, "assets", "optical_models")
OUT_DIR = os.path.join(STEP_DIR, "meshes")
BUILD_DIR = os.path.join(ROOT, "build", "optical_models")
REVIEW_DIR = os.path.join(BUILD_DIR, "review")
WHITE = (1.0, 1.0, 1.0)

V = FreeCAD.Vector


# ---------------------------------------------------------------------------------------------------------
# Import


class Source:
    """One STEP file: its solids in FreeCAD's order, their STEP names, and each face's color."""

    def __init__(self, step):
        self.path = os.path.join(STEP_DIR, step)
        shape = Part.read(self.path)
        self.solids = list(shape.Solids)
        styles = match_solids(StepStyles(self.path), self.solids)
        self.names = [s["name"] if s else "" for s in styles]
        self.colors = [s["colors"] if s else [None] * len(so.Faces) for s, so in zip(styles, self.solids)]

    def solid_index(self, ref):
        """A solid named by its index in the file or by its STEP name."""
        if isinstance(ref, int):
            return ref
        matches = [i for i, n in enumerate(self.names) if n == ref]
        if len(matches) != 1:
            raise ValueError("%s: solid name %r matches %d solids" % (self.path, ref, len(matches)))
        return matches[0]


def color_name(c):
    if c is None:
        return "none"
    if c == WHITE:
        return "white"
    return "%.2f,%.2f,%.2f" % c


def describe_face(face):
    s = face.Surface
    t = type(s).__name__
    if t == "Plane":
        n = face.normalAt(*face.Surface.parameter(face.CenterOfMass))
        return "%-8s n=(%.3f,%.3f,%.3f)" % (t, n.x, n.y, n.z)
    if t in ("Cylinder", "Cone"):
        a, c = s.Axis, s.Center
        return "%-8s r=%.4f axis=(%.3f,%.3f,%.3f) at=(%.3f,%.3f,%.3f)" % (t, s.Radius, a.x, a.y, a.z, c.x, c.y, c.z)
    if t == "Sphere":
        c = s.Center
        return "%-8s r=%.4f at=(%.3f,%.3f,%.3f)" % (t, s.Radius, c.x, c.y, c.z)
    if t == "Toroid":
        a, c = s.Axis, s.Center
        return "%-8s R=%.3f r=%.3f axis=(%.3f,%.3f,%.3f) at=(%.3f,%.3f,%.3f)" % (
            t, s.MajorRadius, s.MinorRadius, a.x, a.y, a.z, c.x, c.y, c.z)
    return t


def describe(step, solid=None, min_area=0.0):
    src = Source(step)
    for i, so in enumerate(src.solids):
        if solid is not None and i != solid:
            continue
        b = so.BoundBox
        print("[%d] %s vol=%.2f faces=%d bbox=(%.3f,%.3f,%.3f)..(%.3f,%.3f,%.3f)" % (
            i, src.names[i], so.Volume, len(so.Faces), b.XMin, b.YMin, b.ZMin, b.XMax, b.YMax, b.ZMax))
        if solid is None:
            continue
        for j, f in enumerate(so.Faces):
            if f.Area < min_area:
                continue
            fb, c = f.BoundBox, f.CenterOfMass
            print("   f%-4d %-6s area=%9.3f com=(%.3f,%.3f,%.3f) bbox=(%.2f,%.2f,%.2f)..(%.2f,%.2f,%.2f) %s" % (
                j, color_name(src.colors[i][j])[:6], f.Area, c.x, c.y, c.z,
                fb.XMin, fb.YMin, fb.ZMin, fb.XMax, fb.YMax, fb.ZMax, describe_face(f)))


# ---------------------------------------------------------------------------------------------------------
# Tessellation


LINEAR_DEFLECTION = 0.02            # mm
ANGULAR_DEFLECTION = math.radians(15)


def mesh_solid(solid):
    """Runs Open CASCADE's mesher over a solid, which stores a triangulation on each of its faces."""
    MeshPart.meshFromShape(Shape=solid, LinearDeflection=LINEAR_DEFLECTION,
                           AngularDeflection=ANGULAR_DEFLECTION, Relative=False)


def tessellate_face(face):
    """The face's triangles and normals, as tessellate_face_points() gives them. Call mesh_solid() on the face's
    solid first; face.tessellate() then reads the stored triangulation rather than meshing again."""
    points, tris = face.tessellate(LINEAR_DEFLECTION)
    if not tris:
        return [], [], []
    return tessellate_face_points(face, points, tris)


# ---------------------------------------------------------------------------------------------------------
# Finishes


# glTF material for each finish: base color (linear RGBA), metallic, roughness, alpha mode. Anodized black
# is the diffuse reflectance 0.04 of the plan; the metals are pbrt's gray reflectance 0.55 at roughness 0.3.
# Kept in step with FINISHES in src/optic_models.cpp, which looks the finishes up by name.
FINISHES = {
    "anodized_black": ((0.04, 0.04, 0.04, 1.0), 0.0, 0.5, None),
    "stainless": ((0.55, 0.55, 0.55, 1.0), 1.0, 0.3, None),
    "brass": ((0.78, 0.60, 0.30, 1.0), 1.0, 0.3, None),
    "glass": ((0.90, 0.95, 1.00, 0.25), 0.0, 0.05, "BLEND"),
    "engraving": ((0.80, 0.80, 0.80, 1.0), 0.0, 0.6, None),
}


def classify_color(c):
    """The finish a STEP color stands for: dark grays are anodized aluminum, warm hues brass, the rest steel."""
    r, g, b = c
    if max(c) < 0.35:
        return "anodized_black"
    if r - b > 0.3 and r >= g >= b:
        return "brass"
    return "stainless"


def face_finishes(colors, default):
    """The finish of each face of a solid. White marks a cosmetic thread or lettering, which takes the
    finish of the solid's other faces; a face with no color takes the solid's default."""
    named = [classify_color(c) for c in colors if c is not None and c != WHITE]
    # sorted, so that a tie goes the same way every run (a set of strings iterates in a per-run hash order)
    host = max(sorted(set(named)), key=named.count) if named else default
    return [default if c is None else (host if c == WHITE else classify_color(c)) for c in colors]


# ---------------------------------------------------------------------------------------------------------
# References: points and directions on the solids, in the STEP file's millimeters


def _vec(v):
    return V(float(v[0]), float(v[1]), float(v[2]))


def _nearest_face(src, solid, kind, accept, near, what, slack=0.5):
    best, best_d = None, None
    for f in src.solids[src.solid_index(solid)].Faces:
        if type(f.Surface).__name__ != kind or not accept(f):
            continue
        d = f.distToShape(Part.Vertex(near))[0]
        if best is None or d < best_d - 1e-9:
            best, best_d = f, d
    if best is None:
        raise ValueError("no %s on solid %r" % (what, solid))
    if best_d > slack:
        raise ValueError("the nearest %s on solid %r is %.3f mm from %s" % (what, solid, best_d, list(near)))
    return best


def resolve(src, ref):
    """A reference as (point, direction or None), in source millimeters.

    [x, y, z]                                       a point
    {"sphere": solid}                               the center of the solid's largest spherical face
    {"cylinder": {"solid", "radius", "near"}}       the axis of the cylindrical face of that radius nearest
                                                    `near`: the point on it nearest `near`, its direction
    {"plane": {"solid", "normal", "near"}}          the planar face facing `normal` nearest `near`: `near`
                                                    projected onto it, and its outward normal
    {"circle": {"solid", "radius", "near"}}         a circular edge's center and axis
    {"axis": ref, "plane": ref}                     where the axis meets the plane, in the axis's direction
    {"point": ref, "offset": [dx, dy, dz]}          a point moved by an offset
    """
    if isinstance(ref, list):
        return _vec(ref), None
    if "sphere" in ref:
        spheres = [f for f in src.solids[src.solid_index(ref["sphere"])].Faces
                   if type(f.Surface).__name__ == "Sphere"]
        if not spheres:
            raise ValueError("no spherical face on solid %r" % ref["sphere"])
        return V(max(spheres, key=lambda f: f.Area).Surface.Center), None
    if "cylinder" in ref:
        c = ref["cylinder"]
        # The cylinder whose axis passes nearest `near`, which may be anywhere along it
        near = _vec(c["near"])
        best, best_d = None, None
        for f in src.solids[src.solid_index(c["solid"])].Faces:
            if type(f.Surface).__name__ != "Cylinder" or abs(f.Surface.Radius - c["radius"]) >= 0.01:
                continue
            axis, base = V(f.Surface.Axis).normalize(), V(f.Surface.Center)
            d = ((near - base) - axis * (near - base).dot(axis)).Length
            if best is None or d < best_d - 1e-9:
                best, best_d = (axis, base), d
        if best is None or best_d > c["radius"] + 0.5:
            raise ValueError("no cylinder of radius %g on solid %r has its axis near %s" % (
                c["radius"], c["solid"], list(near)))
        axis, base = best
        return base + axis * (near - base).dot(axis), axis
    if "plane" in ref and "axis" not in ref:
        c = ref["plane"]
        near, n = _vec(c["near"]), _vec(c["normal"]).normalize()

        # The face must lie in the plane through `near` (within 0.01 mm), which may be inside a hole in it.
        def facing(f):
            return f.normalAt(*f.Surface.parameter(f.CenterOfMass)).dot(n) > 0.9999 and \
                abs((near - V(f.CenterOfMass)).dot(n)) < 0.01
        f = _nearest_face(src, c["solid"], "Plane", facing, near, "plane facing %s" % list(c["normal"]),
                          slack=1e9)
        on = V(f.CenterOfMass)
        return near - n * (near - on).dot(n), n
    if "circle" in ref:
        c = ref["circle"]
        near = _vec(c["near"])
        best, best_d = None, None
        for e in src.solids[src.solid_index(c["solid"])].Edges:
            if type(e.Curve).__name__ != "Circle" or abs(e.Curve.Radius - c["radius"]) > 0.01:
                continue
            d = (V(e.Curve.Center) - near).Length
            if best is None or d < best_d - 1e-9:
                best, best_d = e, d
        if best is None:
            raise ValueError("no circle of radius %g on solid %r" % (c["radius"], c["solid"]))
        return V(best.Curve.Center), V(best.Curve.Axis).normalize()
    if "axis" in ref:
        p, d = resolve(src, ref["axis"])
        q, n = resolve(src, ref["plane"])
        t = (q - p).dot(n) / d.dot(n)
        return p + d * t, d
    if "point" in ref:
        p, d = resolve(src, ref["point"])
        return p + _vec(ref["offset"]), d
    raise ValueError("unknown reference %r" % (ref,))


def resolve_direction(src, ref):
    """A direction: [x, y, z], or {"along": ref} for a reference's direction, with "flip": true to negate."""
    if isinstance(ref, list):
        return _vec(ref).normalize()
    d = resolve(src, ref["along"])[1]
    if d is None:
        raise ValueError("reference %r has no direction" % (ref,))
    return (d * -1 if ref.get("flip") else d).normalize()


class Frame:
    """A right-handed frame given by an origin and its y and z axes in source millimeters. to_canonical()
    maps source millimeters into the frame's meters."""

    def __init__(self, origin, y, z):
        z = z.normalize()
        x = y.cross(z).normalize()
        y = z.cross(x).normalize()
        self.origin, self.x, self.y, self.z = origin, x, y, z

    def point(self, p):
        d = p - self.origin
        return [d.dot(self.x) * 1e-3, d.dot(self.y) * 1e-3, d.dot(self.z) * 1e-3]

    def direction(self, v):
        return [v.dot(self.x), v.dot(self.y), v.dot(self.z)]


def resolve_frame(src, spec, default_y=None):
    origin, axis = resolve(src, spec["origin"])
    z = resolve_direction(src, spec["z"]) if "z" in spec else axis
    if z is None:
        raise ValueError("frame %r has no z" % (spec,))
    if "y" in spec:
        y = resolve_direction(src, spec["y"])
    else:
        y = default_y if default_y is not None else V(0, 1, 0)
        if abs(y.dot(z)) > 0.99:
            y = V(0, 0, 1) if abs(z.z) < 0.99 else V(1, 0, 0)
        y = (y - z * y.dot(z)).normalize()
    return Frame(origin, y, z)


# ---------------------------------------------------------------------------------------------------------
# Small linear algebra on the canonical frame's 3x4 transforms (row-major lists)


def _rotation(axis, angle):
    x, y, z = axis
    c, s, t = math.cos(angle), math.sin(angle), 1 - math.cos(angle)
    return [[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
            [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
            [t * x * z - s * y, t * y * z + s * x, t * z * z + c]]


def _about(point, axis, angle):
    r = _rotation(axis, angle)
    t = [point[i] - sum(r[i][k] * point[k] for k in range(3)) for i in range(3)]
    return [r[0] + [t[0]], r[1] + [t[1]], r[2] + [t[2]]]


def _translation(d):
    return [[1, 0, 0, d[0]], [0, 1, 0, d[1]], [0, 0, 1, d[2]]]


IDENTITY = _translation([0, 0, 0])


def motion_poses(motion, mates):
    """The poses of a moving part that the visibility pass tries, besides rest: each end of its range."""
    if motion is None:
        return []
    m = mates[motion["mate"]]
    if motion["kind"] == "tilt":           # a kinematic plate: about the mate's x and y axes
        return [_about(m["origin"], m[a], s * motion["range"]) for a in ("x", "y") for s in (-1, 1)]
    if motion["kind"] == "screw":          # an adjuster: along the mate's z, by the range in meters
        return [_translation([c * d for c in m["z"]]) for d in motion["range"]]
    if motion["kind"] == "rotate":         # a rotor or a lever: about the mate's z, by the angles given
        return [_about(m["origin"], m["z"], a) for a in motion["range"]]
    if motion["kind"] == "slide":          # a jaw: along the mate's z
        return [_translation([c * d for c in m["z"]]) for d in motion["range"]]
    raise ValueError("unknown motion %r" % motion["kind"])


# ---------------------------------------------------------------------------------------------------------
# The conversion


LEVELS = 6                           # tessellation levels k < LEVELS; see deflections()


def is_cosmetic_thread(face):
    """A white cylinder or cone around a hole or shank: how these files mark a thread they do not model."""
    t = type(face.Surface).__name__
    return t in ("Cylinder", "Cone") and face.Surface.Radius >= 0.8 and face.Area > 3.0


def solid_finishes(src, i, entry):
    """Each face's finish, with the white faces that are lettering marked "engraving": decals.py flattens them
    into a texture, and any it cannot (lettering cut into a free-form face) stay white geometry."""
    colors = src.colors[i]
    overrides = {s: f for f, solids in entry.get("finishes", {}).items() for s in solids}
    default = overrides.get(i, entry.get("default_finish", "anodized_black"))
    if i in overrides:
        return [default] * len(colors)
    finishes = face_finishes(colors, default)
    # a file that prints its lettering in another color names it in the manifest
    inks = [tuple(c) for c in entry.get("lettering_colors", [])]
    for j, c in enumerate(colors):
        if (c == WHITE and not is_cosmetic_thread(src.solids[i].Faces[j])) or \
                (c is not None and any(max(abs(a - b) for a, b in zip(c, ink)) < 0.005 for ink in inks)):
            finishes[j] = "engraving"
    if entry.get("find_lettering"):
        for j in find_lettering(src.solids[i]):
            finishes[j] = "engraving"
    return finishes


# A glyph's faces are each under this across (mm), at least this many, and no deeper than this below the face they
# are cut into (mm)
GLYPH_EXTENT = 3.0
GLYPH_FACES = 3
GLYPH_DEPTH = 0.3


def find_lettering(solid):
    """The faces of engraved lettering in a file that does not color it (LMR2/M, CF125C/M), found from geometry:
    a connected group of at least GLYPH_FACES faces, each under GLYPH_EXTENT across, that borders one face only,
    an analytic one, and lies within GLYPH_DEPTH of its surface. A chamfer, a counterbore or a pin's end borders
    several faces. Faces lying on the surface itself (the middle of an O) are not lettering; decals.py finds them
    as islands."""
    faces = solid.Faces
    owner = collections.defaultdict(set)
    for j, f in enumerate(faces):
        for e in f.Edges:
            owner[e.hashCode()].add(j)
    nbr = collections.defaultdict(set)
    for fs in owner.values():
        for a in fs:
            nbr[a] |= fs - {a}
    small = {j for j, f in enumerate(faces)
             if max(f.BoundBox.XLength, f.BoundBox.YLength, f.BoundBox.ZLength) < GLYPH_EXTENT}
    found = []
    for comp in decals._components(sorted(small), lambda j: [n for n in nbr[j] if n in small]):
        parents = {n for j in comp for n in nbr[j] if n not in small}
        if len(comp) < GLYPH_FACES or len(parents) != 1:
            continue
        parent = faces[parents.pop()]
        if type(parent.Surface).__name__ not in decals.ANALYTIC_PARENTS:
            continue
        surface = decals.Surface(parent)
        depth = {j: max(surface.distance(V(p)) for p in faces[j].tessellate(0.05)[0]) for j in comp}
        if max(depth.values()) > GLYPH_DEPTH:
            continue
        found += [j for j in comp if depth[j] > 1e-4]
    return found


def host_finish(finishes):
    """The finish most of a solid's faces have, which a proxy that replaces part of it takes."""
    named = [f for f in finishes if f != "engraving"]
    return max(sorted(set(named)), key=named.count) if named else "anodized_black"


class Proxy:
    """A capped cylinder that stands in for a spring or a modeled thread (source millimeters): the faces it
    replaces, which become the texture on its side."""

    def __init__(self, solid, spec, src):
        kind = spec["kind"]
        if kind == "spring":
            base, axis, length, radius = fit_spring(solid)
        elif kind == "thread":
            a, b = resolve(src, spec["from"])[0], resolve(src, spec["to"])[0]
            base, axis, length, radius = a, (b - a).normalize(), (b - a).Length, float(spec["radius"])
        else:
            raise ValueError("unknown proxy kind %r" % kind)
        self.kind, self.base, self.axis, self.length, self.radius = kind, base, axis, length, radius
        self.solid = Part.makeCylinder(radius, length, base, axis)
        self.lateral = [j for j, f in enumerate(self.solid.Faces) if type(f.Surface).__name__ == "Cylinder"][0]
        self.surface = decals.Surface(self.solid.Faces[self.lateral])
        # The faces it replaces: a thread's every face that lies inside the cylinder, a spring's every face that
        # mostly does. An extension spring's hooks reach out past its body to the pins it hangs from, and stay.
        self.replaced = set()
        for j, f in enumerate(solid.Faces):
            pts = f.tessellate(0.05)[0] or [v.Point for v in f.Vertexes]
            inside = sum(self._inside(V(p), 0.05) for p in pts)
            if inside == len(pts) or (kind == "spring" and inside * 2 > len(pts)):
                self.replaced.add(j)

    def _inside(self, p, slack):
        d = p - self.base
        t = d.dot(self.axis)
        r = (d - self.axis * t).Length
        return -slack <= t <= self.length + slack and r <= self.radius + slack


def fit_spring(solid):
    """A coil spring's body as (base, axis, length, radius): its axis and length from the cylindrical faces
    inside the coil, which span exactly its turns (an end hook falls outside them), its radius the farthest any
    vertex in that span is from the axis."""
    groups = {}
    for f in solid.Faces:
        s = f.Surface
        if type(s).__name__ != "Cylinder" or s.Radius > 5.0:
            continue
        a, c = V(s.Axis).normalize(), V(s.Center)
        if a.dot(V(1, 2, 3)) < 0:
            a = a * -1
        c = c - a * c.dot(a)
        key = (round(a.x, 3), round(a.y, 3), round(a.z, 3), round(c.x, 2), round(c.y, 2), round(c.z, 2))
        g = groups.setdefault(key, [a, V(s.Center), 0.0, []])
        g[2] += f.Area
        g[3].append(f)
    if not groups:
        raise ValueError("a spring with no cylindrical face to fit its axis to")
    axis, center, _, faces = max(groups.values(), key=lambda g: g[2])
    ts = [(V(p) - center).dot(axis) for f in faces for p in f.tessellate(0.02)[0]]
    t0, t1 = min(ts), max(ts)
    radius = 0.0
    for p in solid.tessellate(0.02)[0]:
        d = V(p) - center
        t = d.dot(axis)
        if t0 <= t <= t1:
            radius = max(radius, (d - axis * t).Length)
    return center + axis * t0, axis, t1 - t0, radius


def deflections(level):
    """A tessellation level's linear (mm) and angular (radians) deflection. Coarser levels relax both: the
    angular limit is what sets the triangle count of small holes, screws and fillets."""
    return LINEAR_DEFLECTION * (2 ** level), min(ANGULAR_DEFLECTION * (1.4 ** level), math.radians(45))


def tessellate_at(solid, level):
    """Each face of a copy of the solid, meshed at a level. The copy drops any triangulation the solid
    already holds, which Open CASCADE's mesher would keep if it is finer than asked for."""
    linear, angular = deflections(level)
    copy = solid.copy()
    MeshPart.meshFromShape(Shape=copy, LinearDeflection=linear, AngularDeflection=angular, Relative=False)
    out = []
    for f in copy.Faces:
        points, tris = f.tessellate(linear)
        out.append(tessellate_face_points(f, points, tris) if tris else ([], [], []))
    return out


ANALYTIC = ("Plane", "Cylinder", "Cone", "Sphere", "Toroid")


def _exact_normal(face, p):
    try:
        n = face.normalAt(*face.Surface.parameter(p))
        return n if n.Length > 0.5 else None
    except Exception:
        return None


def tessellate_face_points(face, points, tris):
    """Normals for a face's triangulation, and its triangles wound counterclockwise about them.

    An analytic face takes the surface's exact normal at each vertex. A free-form face (a B-spline: lettering,
    a spring's helix) takes the area-weighted mean of its own triangles' normals, since projecting a point
    onto a B-spline costs milliseconds and a spring has tens of thousands of vertices; one exact normal at its
    middle orients it. Either way the face keeps its own vertices, so shading stays sharp across its edges.
    """
    acc = [V(0, 0, 0) for _ in points]
    for a, b, c in tris:
        g = (points[b] - points[a]).cross(points[c] - points[a])
        for k in (a, b, c):
            acc[k] = acc[k] + g
    mean = [acc[k].normalize() if acc[k].Length > 0 else None for k in range(len(points))]
    if type(face.Surface).__name__ in ANALYTIC:
        normals = [_exact_normal(face, p) for p in points]
    else:
        normals = list(mean)
        k = max(range(len(points)), key=lambda i: acc[i].Length)
        reference = _exact_normal(face, points[k])
        if reference is not None and mean[k] is not None and reference.dot(mean[k]) < 0:
            normals = [n * -1 if n is not None else None for n in normals]
    # Vertices where the surface normal is undefined (a cone's apex, a sphere's pole) take the mean.
    normals = [n if n is not None else (mean[k] if mean[k] is not None else V(0, 0, 1)) for k, n in enumerate(normals)]
    # The triangles must wind counterclockwise about the outward normal; flip a face that disagrees.
    agree = 0.0
    for a, b, c in tris:
        g = (points[b] - points[a]).cross(points[c] - points[a])
        agree += g.dot(normals[a] + normals[b] + normals[c])
    if agree < 0:
        tris = [(a, c, b) for a, b, c in tris]
    return list(points), normals, [tuple(t) for t in tris]


def write_omv(path, parts, configurations=None):
    """parts: [(poses [3x4], triangles [(a, b, c) of canonical points])], and optionally the configurations to
    try, each a pose index per part. The format mesh_visibility reads."""
    with open(path, "wb") as f:
        f.write((b"OMV2" if configurations else b"OMV1") + struct.pack("<I", len(parts)))
        for poses, triangles in parts:
            f.write(struct.pack("<I", len(poses)))
            for p in poses:
                f.write(struct.pack("<12f", *[p[r][c] for r in range(3) for c in range(4)]))
            f.write(struct.pack("<I", len(triangles)))
            f.write(b"".join(struct.pack("<9f", *a, *b, *c) for a, b, c in triangles))
        if configurations:
            f.write(struct.pack("<I", len(configurations)))
            for c in configurations:
                f.write(struct.pack("<%dI" % len(c), *c))


def read_mask(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"OMM1"
    count, = struct.unpack_from("<I", data, 4)
    off, masks = 8, []
    for _ in range(count):
        n, = struct.unpack_from("<I", data, off)
        masks.append(data[off + 4:off + 4 + n])
        off += 4 + n
    return masks


def run_visibility(omv, mask):
    tool = os.path.join(ROOT, "bin", "mesh_visibility")
    if not os.path.exists(tool):
        raise SystemExit("bin/mesh_visibility is missing: build it first (cd build && cmake --build .)")
    out = subprocess.run([tool, omv, mask], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("mesh_visibility failed: " + out.stderr)
    return out.stdout.strip()


class Model:
    """One manifest entry on its way to a .glb: solids grouped into parts, a finish and a tessellation
    level per solid, the proxies, lettering and drives, and which faces any view reaches.

    A solid's faces are numbered as in the STEP file, then each proxy's three (side and two caps) after them."""

    def __init__(self, entry):
        self.entry = entry
        self.name = entry["model"]
        self.src = Source(entry["step"])
        self.frame = resolve_frame(self.src, entry["frame"])
        removed = {self.src.solid_index(s) for s in entry.get("remove_solids", [])}
        parts = dict(entry["parts"])
        self.iris = None
        if "iris" in entry:
            spec = entry["iris"]
            blades = [self.src.solid_index(b) for b in spec["blades"]]
            self.iris = iris.Iris(self.src, self.frame, spec, blades)
            for n, b in enumerate(self.iris.blades):
                parts["blade_%02d" % (n + 1)] = [b["solid"]]
        self.parts = {}
        assigned = set()
        for part, solids in parts.items():
            idx = [self.src.solid_index(s) for s in solids]
            for i in idx:
                if i in assigned or i in removed:
                    raise ValueError("%s: solid %d is in two parts or removed" % (self.name, i))
                assigned.add(i)
            self.parts[part] = idx
        missing = set(range(len(self.src.solids))) - assigned - removed
        if missing:
            raise ValueError("%s: solids %s are in no part and not removed" % (self.name, sorted(missing)))
        self.mates = {}
        for name, spec in entry.get("mates", {}).items():
            f = resolve_frame(self.src, spec, default_y=self.frame.y)
            m = {"origin": self.frame.point(f.origin), "x": self.frame.direction(f.x),
                 "y": self.frame.direction(f.y), "z": self.frame.direction(f.z)}
            for k, v in spec.items():
                if k not in ("origin", "x", "y", "z"):
                    m[k] = v
            self.mates[name] = m
        self.motions = dict(entry.get("motions", {}))
        self.drives = {}
        if self.iris is not None:
            self._drive_iris()
        self.solids = sorted(assigned)
        self.part_of = {i: p for p, idx in self.parts.items() for i in idx}
        self.finishes = {i: solid_finishes(self.src, i, entry) for i in assigned}
        self.proxies = {i: [] for i in self.solids}
        for spec in entry.get("proxies", []):
            i = self.src.solid_index(spec["solid"])
            proxy = Proxy(self.src.solids[i], spec, self.src)
            self.proxies[i].append(proxy)
            self.finishes[i] += [host_finish(self.finishes[i])] * len(proxy.solid.Faces)
        self.lettering = {}
        # the triangles flattening dropped as folded, per (solid, level)
        self.folded = {}
        for i in self.solids:
            replaced = {j for p in self.proxies[i] for j in p.replaced}
            faces = [j for j, f in enumerate(self.finishes[i]) if f == "engraving" and j not in replaced]
            if faces:
                self.lettering[i] = decals.LetteringTopology(self.src.solids[i], faces)

    def _drive_iris(self):
        """The iris's drive: the lever and every blade, from the smallest aperture Thorlabs gives to the
        largest. Each blade turns about its pivot pin; its mate is the pin's axis at the blade's mid-plane."""
        spec = self.entry["iris"]
        rows, (closed, opened) = self.iris.table(spec["aperture_range"])
        self.iris_reached = (closed[1], opened[1])
        motions = {"actuator": [r[1] for r in rows]}
        for n, b in enumerate(self.iris.blades):
            name = "blade_%02d" % (n + 1)
            self.mates[name + "_pivot"] = {
                "comment": "the blade's pivot pin, fixed in the housing, at the blade's mid-plane",
                "origin": [b["pivot"][0] * 1e-3, b["pivot"][1] * 1e-3, b["z"] * 1e-3],
                "x": [1.0, 0.0, 0.0], "y": [0.0, 1.0, 0.0], "z": [0.0, 0.0, 1.0], "part": name}
            motions[name] = [r[2] for r in rows]
        for name, values in motions.items():
            self.motions[name] = {"kind": "rotate", "mate": "lever_axis" if name == "actuator" else name + "_pivot",
                                  "range": [min(values), max(values)], "drive": "aperture"}
        rest = self.iris.aperture(0.0)
        self.drives["aperture"] = {
            "comment": "the aperture's diameter: each motion's angle at each value, linear between them. The ends "
                       "are the smallest and largest Thorlabs gives, or the nearest the blades come to them",
            "values": [r[0] * 1e-3 for r in rows], "rest": rest * 1e-3,
            "published": [v * 1e-3 for v in spec["aperture_range"]],
            "motions": motions}

    def poses(self, part):
        """The poses of a part the visibility pass tries: rest first, then the ends of its motion, or for a part
        a drive moves, its pose at each of DRIVE_POSES values of the drive."""
        motion = self.motions.get(part)
        if motion is not None and "drive" in motion:
            values = self.drives[motion["drive"]]["motions"][part]
            m = self.mates[motion["mate"]]
            picks = [values[round(k * (len(values) - 1) / (DRIVE_POSES - 1))] for k in range(DRIVE_POSES)]
            return [IDENTITY] + [_about(m["origin"], m["z"], a) for a in picks]
        return [IDENTITY] + motion_poses(motion, self.mates)

    def configurations(self):
        """Pose indices per part for each configuration the visibility pass tries: all at rest, each part that
        moves alone at each of its other poses, and every part a drive moves at each of the drive's poses
        together. None when nothing is driven, for the visibility pass's own default."""
        if not self.drives:
            return None
        names = list(self.parts)
        out = [[0] * len(names)]
        for k, part in enumerate(names):
            motion = self.motions.get(part)
            if motion is not None and "drive" not in motion:
                for i in range(1, len(self.poses(part))):
                    c = [0] * len(names)
                    c[k] = i
                    out.append(c)
        for drive in sorted(self.drives):
            driven = [k for k, part in enumerate(names) if self.motions.get(part, {}).get("drive") == drive]
            for i in range(1, DRIVE_POSES + 1):
                c = [0] * len(names)
                for k in driven:
                    c[k] = i
                out.append(c)
        return out

    def canonical(self, p):
        return tuple(self.frame.point(p))

    def tessellate(self, i, level):
        """A solid's faces at a level: the proxies' replaced faces empty, the proxies' own faces after the
        solid's, and the lettering flattened into the faces it is cut into."""
        tess = tessellate_at(self.src.solids[i], level)
        if any(p.kind == "spring" for p in self.proxies[i]) and level < HOOK_LEVEL:
            # what a spring's cylinder leaves (its hooks) is bent wire, which needs no finer level than this
            tess = tessellate_at(self.src.solids[i], HOOK_LEVEL)
        for proxy in self.proxies[i]:
            for j in proxy.replaced:
                tess[j] = ([], [], [])
            tess += tessellate_at(proxy.solid, level)
        if i in self.lettering:
            tess, self.folded[(i, level)] = decals.flatten(self.lettering[i], tess, deflections(level)[0])
        return tess

    def omv_parts(self, tess, faces=None):
        """The parts as mesh_visibility reads them, and for each part the (solid, face, triangle) of each
        of its triangles. `faces`, when given, limits each solid to those face indices."""
        parts, owners = [], []
        for part, idx in self.parts.items():
            tris, own = [], []
            for i in idx:
                for j, (pts, _, t) in enumerate(tess[i]):
                    if faces is not None and j not in faces[i]:
                        continue
                    cp = [self.canonical(p) for p in pts]
                    for k, (a, b, c) in enumerate(t):
                        tris.append((cp[a], cp[b], cp[c]))
                        own.append((i, j, k))
            parts.append((self.poses(part), tris))
            owners.append(own)
        return parts, owners

    def charts(self, raw):
        """Every face that carries a decal, {(solid, face): chart}, and what each chart's texels are baked
        from, {id(chart): source} as decals.bake() reads it. `raw` is the finest tessellation of the STEP faces,
        before any flattening."""
        charts, sources = {}, {}
        for i in self.solids:
            if i in self.lettering:
                topo = self.lettering[i]
                for k, chart in decals.lettering_charts(topo, raw[i]).items():
                    color = FINISHES[self.finishes[i][k]][0][:3]
                    charts[(i, k)] = chart
                    sources[id(chart)] = ("lettering", chart.source, None, color)
            offset = len(self.src.solids[i].Faces)
            for proxy in self.proxies[i]:
                chart = decals.proxy_chart(proxy.surface, proxy.length)
                tris, normals = [], []
                for j in sorted(proxy.replaced):
                    pts, nrm, t = raw[i][j]
                    tris += [(pts[a], pts[b], pts[c]) for a, b, c in t]
                    normals += [(nrm[a], nrm[b], nrm[c]) for a, b, c in t]
                if proxy.kind == "thread":
                    chart.inner = min(((p - proxy.base) - proxy.axis * (p - proxy.base).dot(proxy.axis)).Length
                                      for t in tris for p in t)
                charts[(i, offset + proxy.lateral)] = chart
                sources[id(chart)] = (proxy.kind, tris, normals, FINISHES[self.finishes[i][offset]][0][:3])
                offset += len(proxy.solid.Faces)
        return charts, sources


# the drive values the visibility pass tries a driven part at, besides rest: the ends and the middle
DRIVE_POSES = 3
# the finest level a spring's hooks are meshed at (0.08 mm and 29 degrees)
HOOK_LEVEL = 2


def convert(entry, work):
    model = Model(entry)
    name = model.name
    stem = file_stem(name)
    budget = entry.get("budget", 20000)
    log = []
    configurations = model.configurations()

    # Pass 1: which faces any view reaches, a face being kept when any of its triangles is visible. A model
    # can run it at a coarser level ("survey_level"); but a coarse tessellation can close a narrow slit between
    # two parts and hide the face at its bottom, so the finest level is the default.
    cache = {}

    def tess(i, k):
        if (i, k) not in cache:
            cache[(i, k)] = model.tessellate(i, k)
        return cache[(i, k)]
    survey_level = entry.get("survey_level", 0)
    survey = {i: tess(i, survey_level) for i in model.solids}
    parts, owners = model.omv_parts(survey)
    write_omv(os.path.join(work, stem + ".fine.omv"), parts, configurations)
    log.append("faces: " + run_visibility(os.path.join(work, stem + ".fine.omv"), os.path.join(work, stem + ".fine.mask")))
    masks = read_mask(os.path.join(work, stem + ".fine.mask"))
    visible = {i: set() for i in model.solids}
    for own, mask in zip(owners, masks):
        for (i, j, _), m in zip(own, mask):
            if m:
                visible[i].add(j)

    # The budget: coarsen, one level at a time, the solid whose next level saves the most triangles.
    level = {i: 0 for i in model.solids}

    def count(i, k):
        return sum(len(tess(i, k)[j][2]) for j in visible[i])
    total = sum(count(i, 0) for i in model.solids)
    while total > budget:
        savings = {i: count(i, level[i]) - count(i, level[i] + 1) for i in model.solids
                   if level[i] < LEVELS - 1 and visible[i]}
        best = max(savings, key=lambda i: (savings[i], -i), default=None)
        if best is None or savings[best] <= 0:
            break
        level[best] += 1
        total -= savings[best]
    final = {i: tess(i, level[i]) for i in model.solids}

    # Pass 2: the kept faces at their levels; drop the triangles no view reaches.
    parts, owners = model.omv_parts(final, visible)
    write_omv(os.path.join(work, stem + ".kept.omv"), parts, configurations)
    log.append("kept faces: " + run_visibility(os.path.join(work, stem + ".kept.omv"), os.path.join(work, stem + ".kept.mask")))
    masks = read_mask(os.path.join(work, stem + ".kept.mask"))
    keep = set()
    for own, mask in zip(owners, masks):
        keep.update(o for o, m in zip(own, mask) if m)

    # The decals: a chart for every face that carries one, packed into one atlas and baked from the finest
    # tessellation of the geometry each stands for.
    raw = {i: tessellate_at(model.src.solids[i], 0) for i in model.solids}
    charts, sources = model.charts(raw)
    atlas = None
    if charts:
        atlas = decals.Atlas([charts[key] for key in sorted(charts)])
        image = decals.bake(atlas, sources, os.path.join(ROOT, "bin", "mesh_visibility"), work, stem)
        texture = decals.png(image)
        with open(os.path.join(REVIEW_DIR, stem + ".decals.png"), "wb") as f:
            f.write(texture)
    else:
        texture = None

    total, clean_parts = write_glb(os.path.join(OUT_DIR, glb_name(name)), model, final, visible, keep,
                                   charts, atlas, texture)

    # The review renders compare the finest tessellation of the STEP file's faces with the result. The first
    # silhouette check compares every face at the levels used with what was kept, so that it measures the removal
    # of hidden geometry alone; the coarser levels' own error is their deflection, which the log reports. The
    # second compares the STEP faces at the same levels with the result, counting only where the result lets a
    # ray farther: a hole that flattening the lettering left. Proxies only ever cover more.
    every = {i: set(range(len(raw[i]))) for i in model.solids}
    write_glb(os.path.join(REVIEW_DIR, stem + ".original.glb"), model, raw, every)
    write_omv(os.path.join(work, stem + ".original.omv"), [([IDENTITY], t) for _, t in model.omv_parts(final)[0]])
    write_omv(os.path.join(work, stem + ".clean.omv"), clean_parts)
    step = {i: raw[i] if level[i] == 0 else tessellate_at(model.src.solids[i], level[i]) for i in model.solids}
    write_omv(os.path.join(work, stem + ".step.omv"), [([IDENTITY], t) for _, t in model.omv_parts(step)[0]])

    mates = {"model": name, "step": entry["step"], "units": "meters",
             "frame": "+y up, +z along the optical axis toward the incoming light",
             "source_frame": {"comment": "the canonical frame's origin and axes in the STEP file's millimeters",
                              "origin": list(model.frame.origin), "x": list(model.frame.x),
                              "y": list(model.frame.y), "z": list(model.frame.z)},
             "parts": list(model.parts), "mates": model.mates,
             "motions": model.motions, "triangles": total, "budget": budget}
    if model.drives:
        mates["drives"] = model.drives
    if atlas is not None:
        mates["decals"] = {"comment": "the lettering, scales, springs and threads, baked into the .glb's texture",
                           "size": atlas.size, "texel": atlas.texel * 1e-3}
    for k in ("kind", "params"):
        if k in entry:
            mates[k] = entry[k]
    with open(os.path.join(OUT_DIR, mates_name(name)), "w") as f:
        f.write(dump_json(mates) + "\n")
    levels = sorted(set(level.values()))
    by_level = ", ".join("%d solids at %.2f mm and %.0f deg" % (list(level.values()).count(k), deflections(k)[0],
                                                               math.degrees(deflections(k)[1])) for k in levels)
    log.append("%d triangles (budget %d); %s" % (total, budget, by_level))
    kept = collections.Counter(i for (i, j, k) in keep)
    heavy = sorted(kept, key=lambda i: (-kept[i], i))[:4]
    log.append("heaviest: %s" % ", ".join("[%d] %s %d" % (i, model.src.names[i] or "", kept[i]) for i in heavy))
    flattened = sum(len(t.components) for t in model.lettering.values())
    skipped = sum(len(t.skipped) for t in model.lettering.values())
    if flattened or skipped or model.proxies:
        log.append("decals: %d lettering groups flattened (%d left as geometry: cut into a free-form face), "
                   "%d triangles dropped as folded, %d proxies%s" % (
                       flattened, skipped, sum(model.folded.get((i, level[i]), 0) for i in model.solids),
                       sum(len(p) for p in model.proxies.values()),
                       "; atlas %d px at %.3f mm" % (atlas.size, atlas.texel) if atlas else ""))
    if model.iris is not None:
        published = entry["iris"]["aperture_range"]
        reached = model.iris_reached
        lever = model.motions["actuator"]["range"]
        log.append("iris: aperture %.2f..%.2f mm (published %.1f..%.1f), lever %.1f..%.1f deg, %d blades" % (
            reached[0], reached[1], published[0], published[1], math.degrees(lever[0]), math.degrees(lever[1]),
            len(model.iris.blades)))
    return log


def write_glb(path, model, tess, faces, keep=None, charts=None, atlas=None, texture=None):
    """The model as a .glb: one node per part, one primitive per finish, and one per finish for the faces that
    carry decals, whose triangles are cut along their charts' rectangles. `faces` limits each solid to those
    faces, and `keep`, when given, to those (solid, face, triangle). Returns the triangle count and the parts
    as mesh_visibility reads them, at rest."""
    w = glb.GlbWriter()
    tex = w.texture(texture) if texture is not None else None
    children = []
    total = 0
    omv = []
    for part, idx in model.parts.items():
        groups = {}
        tris_out = []
        for i in idx:
            for j in sorted(faces[i]):
                pts, nrm, t = tess[i][j]
                tris = [k for k in range(len(t)) if keep is None or (i, j, k) in keep]
                if not tris:
                    continue
                finish = model.finishes[i][j]
                chart = charts.get((i, j)) if charts else None
                if chart is None:
                    g = groups.setdefault((finish, False), ([], [], [], [], {}))
                    for k in tris:
                        corners = [pts[v] for v in t[k]]
                        _emit(g, [(model.canonical(p), tuple(model.frame.direction(nrm[v])), None)
                                  for p, v in zip(corners, t[k])])
                        tris_out.append(tuple(model.canonical(p) for p in corners))
                    continue
                items = [atlas.item(chart, r) for r in range(len(chart.rects))]
                for k in tris:
                    for poly, item in decals.split_triangle(chart, items, [pts[v] for v in t[k]]):
                        g = groups.setdefault((finish, item is not None), ([], [], [], [], {}))
                        verts = [(model.canonical(p), tuple(model.frame.direction(chart.surface.normal(p))),
                                  atlas.uv(item, u, v) if item is not None else None) for p, u, v in poly]
                        for n in range(1, len(verts) - 1):
                            _emit(g, [verts[0], verts[n], verts[n + 1]])
                            tris_out.append((verts[0][0], verts[n][0], verts[n + 1][0]))
        prims = []
        for finish, textured in sorted(groups):
            pos, nrm, uv, ind, _ = groups[(finish, textured)]
            rgba, metallic, roughness, alpha = FINISHES[finish]
            if textured:
                # the texture holds the finish's color, so the factor is white
                material = w.material(finish + ".decal", (1.0, 1.0, 1.0, 1.0), metallic, roughness, alpha, tex)
                prims.append((pos, nrm, ind, material, uv))
            else:
                prims.append((pos, nrm, ind, w.material(finish, rgba, metallic, roughness, alpha)))
            total += len(ind) // 3
        if prims:
            children.append(w.node(part, w.mesh(part, prims)))
        omv.append(([IDENTITY], tris_out))
    root = w.node(model.name, children=children)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    w.write(path, root)
    return total, omv


def _emit(group, corners):
    """Adds a triangle of (position, normal, uv or None) corners to a primitive, sharing equal vertices."""
    pos, nrm, uv, ind, index = group
    for c in corners:
        key = (tuple(round(x, 10) for x in c[0]), tuple(round(x, 6) for x in c[1]),
               None if c[2] is None else tuple(round(x, 8) for x in c[2]))
        if key not in index:
            index[key] = len(pos)
            pos.append(c[0])
            nrm.append(c[1])
            if c[2] is not None:
                uv.append(c[2])
        ind.append(index[key])


def file_stem(name):
    return name.replace("/", "_")


def glb_name(name):
    return file_stem(name) + ".glb"


def mates_name(name):
    return file_stem(name) + ".mates.json"


def _round(v):
    if isinstance(v, float):
        r = round(v, 9)
        return 0.0 if r == 0 else r
    if isinstance(v, list):
        return [_round(x) for x in v]
    if isinstance(v, dict):
        return {k: _round(x) for k, x in v.items()}
    return v


def dump_json(obj):
    return json.dumps(_round(obj), indent=1)


def load_manifest():
    with open(os.path.join(HERE, "manifest.json")) as f:
        return json.load(f)["models"]


# ---------------------------------------------------------------------------------------------------------
# The mates against example mirror.FCStd


EXAMPLE = os.path.join(STEP_DIR, "example mirror.FCStd")
# Each model's container in the example, and the joints the user made, as (model, mate, model, mate). The
# joints are revolute: the rotation about the shared axis is free, and so is the axial offset where the
# example leaves a gap (the post between mount and holder, which the example does not model).
EXAMPLE_PARTS = {"PF05-03": "Part__Feature026", "KM05T/M": "_8347_E0W", "PH50E/M": "_3143_E0W",
                 "CF125C/M": "CF125C_M_E0W"}
EXAMPLE_JOINTS = [("PF05-03", "back_center", "KM05T/M", "optic_seat", "the mirror in its mount"),
                  ("KM05T/M", "post_tap", "PH50E/M", "post_entry", "the mount over the holder (the post between)"),
                  ("PH50E/M", "base_contact", "CF125C/M", "holder_seat", "the holder in the fork")]


def check_example():
    """Composes the example's placements with each model's mates and reports, for every joint, how far the
    two mates' axes are apart (lateral, mm), the angle between them (mrad), and their axial offset (mm)."""
    doc = FreeCAD.openDocument(EXAMPLE)
    ok = True
    world = {}
    for model, obj in EXAMPLE_PARTS.items():
        with open(os.path.join(OUT_DIR, mates_name(model))) as f:
            mates = json.load(f)
        sf = mates["source_frame"]
        o, x, y, z = (_vec(sf[k]) for k in ("origin", "x", "y", "z"))
        placement = doc.getObject(obj).Placement

        def to_world(m, o=o, x=x, y=y, z=z, placement=placement):
            p = o + (x * m["origin"][0] + y * m["origin"][1] + z * m["origin"][2]) * 1e3
            d = x * m["z"][0] + y * m["z"][1] + z * m["z"][2]
            return placement.multVec(p), placement.Rotation.multVec(d)
        world[model] = {name: to_world(m) for name, m in mates["mates"].items()}
    for a, ma, b, mb, what in EXAMPLE_JOINTS:
        pa, da = world[a][ma]
        pb, db = world[b][mb]
        angle = math.acos(max(-1.0, min(1.0, abs(da.dot(db)))))
        d = pb - pa
        axial = d.dot(da)
        lateral = (d - da * axial).Length
        good = lateral <= 0.05 and angle <= 1e-4
        ok &= good
        print("%-46s lateral %.4f mm, angle %.4f mrad, axial %.4f mm  %s" % (
            what, lateral, angle * 1e3, axial, "ok" if good else "FAIL"))
    print("axial offsets are the example's free parameters: report, not checked")
    return 0 if ok else 1


# ---------------------------------------------------------------------------------------------------------
# Preview


def preview(step, out, by_solid):
    """Every solid of a STEP file, in its own millimeters scaled to meters, one node per solid."""
    src = Source(step)
    w = glb.GlbWriter()
    children = []
    for i, solid in enumerate(src.solids):
        mesh_solid(solid)
        if by_solid:
            hue = (i * 0.61803398875) % 1.0
            rgb = [max(0.0, min(1.0, abs(hue * 6 - 3) - 1)), max(0.0, min(1.0, 2 - abs(hue * 6 - 2))),
                   max(0.0, min(1.0, 2 - abs(hue * 6 - 4)))]
            groups = {"s%d" % i: (rgb + [1.0], list(range(len(solid.Faces))))}
        else:
            groups = {}
            for j, c in enumerate(src.colors[i]):
                key = color_name(c)
                rgb = [0.8, 0.2, 0.8] if c is None else list(c)
                groups.setdefault(key, (rgb + [1.0], []))[1].append(j)
        prims = []
        for key, (rgba, faces) in sorted(groups.items()):
            pos, nrm, idx = [], [], []
            for j in faces:
                p, n, t = tessellate_face(solid.Faces[j])
                base = len(pos)
                pos += [(q.x * 1e-3, q.y * 1e-3, q.z * 1e-3) for q in p]
                nrm += [(q.x, q.y, q.z) for q in n]
                idx += [base + k for tri in t for k in tri]
            if idx:
                prims.append((pos, nrm, idx, w.material(key, rgba, 0.0, 0.6)))
        if prims:
            children.append(w.node("s%d %s" % (i, src.names[i]), w.mesh("s%d" % i, prims)))
    root = w.node(os.path.splitext(step)[0], children=children)
    w.write(out, root)
    print("wrote", out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preview", nargs=2, metavar=("STEP", "OUT"), help="write a raw .glb of a STEP file")
    parser.add_argument("--by-solid", action="store_true", help="with --preview, a color per solid")
    parser.add_argument("--model", action="append", help="convert only these manifest entries")
    parser.add_argument("--out", metavar="DIR", help="write the meshes and mates here instead (a determinism check)")
    parser.add_argument("--describe", metavar="STEP", help="print the solids of a STEP file")
    parser.add_argument("--check-example", action="store_true", help="check the mates against the example assembly")
    parser.add_argument("--solid", type=int, help="with --describe, print this solid's faces")
    parser.add_argument("--min-area", type=float, default=0.0, help="with --solid, skip smaller faces (mm^2)")
    args = parser.parse_args()
    if args.describe:
        describe(args.describe, args.solid, args.min_area)
        return 0
    if args.preview:
        preview(args.preview[0], args.preview[1], args.by_solid)
        return 0
    if args.check_example:
        return check_example()

    entries = load_manifest()
    if args.model:
        unknown = set(args.model) - {e["model"] for e in entries}
        if unknown:
            raise SystemExit("not in the manifest: %s" % ", ".join(sorted(unknown)))
        entries = [e for e in entries if e["model"] in args.model]
    global OUT_DIR
    work = os.path.join(BUILD_DIR, "work")
    if args.out:
        OUT_DIR = os.path.abspath(args.out)
        work = os.path.join(OUT_DIR, "work")
    os.makedirs(work, exist_ok=True)
    failed = []
    for entry in entries:
        name = entry["model"]
        print("%s (%s)" % (name, entry["step"]), flush=True)
        log = convert(entry, work)
        for line in log:
            print("   " + line)
        tool = os.path.join(ROOT, "bin", "mesh_visibility")
        stem = os.path.join(work, file_stem(name))
        out = subprocess.run([tool, "--compare", stem + ".original.omv", stem + ".clean.omv", "--size", "384",
                              "--depth-tolerance", "1e-5"], capture_output=True, text=True)
        print("   silhouettes: " + out.stdout.strip().splitlines()[-1])
        # engraving is at most a few tenths of a millimeter deep: farther than that is a hole
        holes = subprocess.run([tool, "--compare", stem + ".step.omv", stem + ".clean.omv", "--size", "384",
                                "--depth-tolerance", "5e-4", "--farther-only"], capture_output=True, text=True)
        print("   holes: " + holes.stdout.strip().splitlines()[-1])
        if out.returncode != 0 or holes.returncode != 0:
            failed.append(name)
    if failed:
        print("silhouette or hole check failed: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
