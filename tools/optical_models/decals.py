"""Decals for tools/optical_models/convert.py: lettering, scales, springs and threads as a texture instead of
geometry.

Run inside FreeCAD's Python, as convert.py is. The steps, in the order the converter calls them:

  Lettering    flatten() moves every vertex of the white faces (engraved lettering, scale graduations) onto
               the analytic surface they are cut into (a plane, cylinder or cone), merges their triangles into
               that face's mesh, and decimates the face, so that only the vertices its outline and curvature
               need are left. A vertex shared with another face never moves, so the mesh stays watertight.
  Proxies      convert.py replaces a spring or a modeled thread by a capped cylinder; the faces it replaces
               become the texture on its side.
  Charts       each face that carries a decal gets a chart: a map from the surface into millimeters in a plane,
               (u, v), and the rectangles of it that hold the decals.
  Bake         the texels of each rectangle are ray traced (bin/mesh_visibility --trace) against the geometry
               they stand for, and written into one atlas per model, an sRGB PNG.
  Split        when the model is written, each triangle of a decal face is cut along its rectangles' edges, so
               that the pieces inside take the atlas's uvs and the rest keep the plain finish.

Everything here is in the STEP file's millimeters; the converter maps into the canonical frame's meters.
"""

import collections
import math
import os
import struct
import subprocess
import zlib

import numpy as np

import FreeCAD

V = FreeCAD.Vector

ANALYTIC_PARENTS = ("Plane", "Cylinder", "Cone")
# how far apart two decals' rectangles can be and still share one, and the margin around each (mm)
MERGE_GAP = 1.0
MARGIN = 0.3
# texels of padding around each rectangle in the atlas, for bilinear filtering and mip levels
PADDING = 4
# the texel size the atlas aims for (mm), and the largest atlas; decals that do not fit get larger texels
TEXEL = 0.04
MAX_ATLAS = 2048
# the longest rectangle the atlas holds in one piece (mm)
MAX_STRIP = 20.0
# supersamples per texel along each side
SUPERSAMPLE = 2
# the ink of engraved lettering, and how much of the finish's color shows between a spring's coils
INK = (0.80, 0.80, 0.80)
GAP_SHADE = 0.25


# ---------------------------------------------------------------------------------------------------------
# Analytic surfaces


def _perpendicular(a):
    t = V(1, 0, 0) if abs(a.x) < 0.9 else V(0, 1, 0)
    return t.cross(a).normalize()


class Surface:
    """A plane, cylinder or cone with the orientation of the face it came from: project() puts a point on
    it, normal() is the face's outward normal there."""

    def __init__(self, face):
        s = face.Surface
        self.kind = type(s).__name__
        if self.kind not in ANALYTIC_PARENTS:
            raise ValueError("not an analytic surface: %s" % self.kind)
        p = face.CenterOfMass
        n0 = face.normalAt(*s.parameter(p))
        self.sign = 1.0
        if self.kind == "Plane":
            self.origin, self.axis = V(s.Position), V(n0).normalize()
            return
        self.axis = V(s.Axis).normalize()
        self.center = V(s.Center)
        if self.kind == "Cylinder":
            self.radius = s.Radius
        else:
            self.apex = V(s.Apex)
            self.ref_radius = s.Radius
        self.sign = 1.0 if self._raw_normal(p).dot(n0) >= 0 else -1.0

    def _radial(self, p):
        d = p - self.center
        r = d - self.axis * d.dot(self.axis)
        return (r if r.Length > 1e-12 else _perpendicular(self.axis)).normalize()

    def _generator(self, p):
        """A cone's generator at the point's azimuth, a unit direction from the apex."""
        return (self.center + self._radial(p) * self.ref_radius - self.apex).normalize()

    def project(self, p):
        if self.kind == "Plane":
            return p - self.axis * (p - self.origin).dot(self.axis)
        if self.kind == "Cylinder":
            return self.center + self.axis * (p - self.center).dot(self.axis) + self._radial(p) * self.radius
        g = self._generator(p)
        return self.apex + g * max(0.0, (p - self.apex).dot(g))

    def distance(self, p):
        return (p - self.project(p)).Length

    def _raw_normal(self, p):
        if self.kind == "Plane":
            return V(self.axis)
        r = self._radial(p)
        if self.kind == "Cylinder":
            return r
        g = self._generator(p)
        return (r - g * r.dot(g)).normalize()

    def normal(self, p):
        return self._raw_normal(p) * self.sign


# ---------------------------------------------------------------------------------------------------------
# Lettering


def _components(items, neighbors):
    seen, out = set(), []
    for i in items:
        if i in seen:
            continue
        stack, comp = [i], []
        seen.add(i)
        while stack:
            x = stack.pop()
            comp.append(x)
            for n in neighbors(x):
                if n not in seen:
                    seen.add(n)
                    stack.append(n)
        out.append(sorted(comp))
    return out


class LetteringTopology:
    """The lettering of one solid: its faces, the islands they enclose (the middle of an O), the groups they
    form, and the faces each group is cut into (its parents)."""

    def __init__(self, solid, lettering):
        faces = solid.Faces
        self.faces = faces
        owner = collections.defaultdict(set)
        for j, f in enumerate(faces):
            for e in f.Edges:
                owner[e.hashCode()].add(j)
        nbr = collections.defaultdict(set)
        for fs in owner.values():
            for a in fs:
                nbr[a] |= fs - {a}
        lettering = set(lettering)
        # Islands: each group of other faces that lettering cuts off from the rest of the solid, which is the
        # group with the most area
        rest = [j for j in range(len(faces)) if j not in lettering]
        groups = _components(rest, lambda j: [n for n in nbr[j] if n not in lettering])
        groups.sort(key=lambda g: (-sum(faces[j].Area for j in g), g[0]))
        islands = {j for g in groups[1:] for j in g}
        gone = lettering | islands
        self.islands = islands
        self.components = []
        # groups cut into a free-form face, which stay geometry
        self.skipped = []
        for comp in _components(sorted(gone), lambda j: [n for n in nbr[j] if n in gone]):
            parents = sorted({n for j in comp for n in nbr[j] if n not in gone})
            if not parents or any(type(faces[k].Surface).__name__ not in ANALYTIC_PARENTS for k in parents):
                self.skipped.append(comp)
            else:
                self.components.append((comp, parents))
        self.parents = sorted({k for _, ps in self.components for k in ps})
        self.surfaces = {k: Surface(faces[k]) for k in self.parents}
        self.flattened = {j for comp, _ in self.components for j in comp}

    def assignment(self, tess, ink_only=False):
        """Each lettering triangle's parent, the one whose surface its centroid is nearest:
        {parent: [(a, b, c) points]}, and for each vertex (by key) the parents of its triangles. With ink_only,
        the islands are left out: they are the face's own color, not the lettering's."""
        assigned = {k: [] for k in self.parents}
        owners = collections.defaultdict(set)
        # which parents' own meshes each vertex is on
        pinned = {}
        for k in self.parents:
            for p in tess[k][0]:
                pinned.setdefault(_key(p), set()).add(k)
        for comp, parents in self.components:
            for j in comp:
                if ink_only and j in self.islands:
                    continue
                pts, _, tris = tess[j]
                for a, b, c in tris:
                    tri = (pts[a], pts[b], pts[c])
                    centroid = (tri[0] + tri[1] + tri[2]) * (1.0 / 3.0)
                    k = min(parents, key=lambda k: (self.surfaces[k].distance(centroid), k))
                    assigned[k].append(tri)
                    # A vertex that shares a triangle with a vertex of another parent's mesh (the end of a tick
                    # that runs out through a chamfer) goes onto both surfaces, their intersection: projected
                    # onto this parent alone, the triangle would fold back over the edge between them.
                    others = {o for q in tri for o in pinned.get(_key(q), ()) if o != k}
                    for p in tri:
                        owners[_key(p)].add(k)
                        owners[_key(p)].update(others)
        return assigned, owners


def _key(p):
    return (round(p.x, 6), round(p.y, 6), round(p.z, 6))


def flatten(topo, tess, tolerance):
    """The tessellation of a solid with its lettering flattened onto the faces it is cut into, each of those
    decimated to within `tolerance` (mm) of its surface. Returns the new per-face list, where the lettering and
    island faces are empty, and how many triangles came out folded by the projection (none, for engraving)."""
    out = list(tess)
    assigned, owners = topo.assignment(tess)
    on_parent = {}
    for k in topo.parents:
        for p in tess[k][0]:
            on_parent[_key(p)] = p
    moved = {}
    for key, ks in owners.items():
        if key in on_parent:
            moved[key] = on_parent[key]
            continue
        p = V(*key)
        surfaces = [topo.surfaces[k] for k in sorted(ks)]
        # onto its surface, or alternately onto several: a vertex where a groove crosses the edge between two
        # faces converges onto their intersection
        for _ in range(1 if len(surfaces) == 1 else 40):
            for s in surfaces:
                p = s.project(p)
        moved[key] = p
    folded = 0
    for k in topo.parents:
        pts, _, tris = tess[k]
        surface = topo.surfaces[k]
        positions, index = [], {}

        def vid(p):
            kk = _key(p)
            if kk not in index:
                index[kk] = len(positions)
                positions.append(p)
            return index[kk]
        new = [(vid(pts[a]), vid(pts[b]), vid(pts[c])) for a, b, c in tris]
        pinned = len(positions)
        # vertices on two parents' meshes stay where they are, so that both meshes keep them
        pinned_keys = {_key(moved[kk]) for kk, ks in owners.items() if len(ks) > 1}
        lettering = [tuple(vid(moved[_key(p)]) for p in tri) for tri in assigned[k]]
        # A glyph's wall that leans out by a little projects its floor a few micrometers outside its top: weld
        # each lettering vertex within WELD of another onto it, a vertex of the face itself first.
        remap = _weld(positions, pinned, {index[kk] for kk in pinned_keys if kk in index})
        new += [tuple(remap[v] for v in t) for t in lettering]
        faces_out = []
        for t in new:
            if len(set(t)) < 3 or _area(positions, t) < 1e-10:
                continue
            # A triangle still folded over lies on others: dropping it leaves no hole
            if _normal(positions, t).dot(surface.normal(_centroid(positions, t))) < 0:
                folded += 1
                continue
            faces_out.append(t)
        positions, faces_out = decimate(positions, faces_out, surface, tolerance)
        out[k] = (positions, [surface.normal(p) for p in positions], faces_out)
    for j in topo.flattened:
        out[j] = ([], [], [])
    return out, folded


WELD = 0.005


def _weld(positions, pinned, keep):
    """For each vertex, the vertex it is welded onto: the first `pinned` (the face's own) and those in `keep`
    stay; any other within WELD (mm) of a staying or earlier vertex joins the nearest, a staying one first."""
    grid = collections.defaultdict(list)

    def cell(p):
        return (int(math.floor(p.x / WELD)), int(math.floor(p.y / WELD)), int(math.floor(p.z / WELD)))

    def nearest(p, staying_only):
        cx, cy, cz = cell(p)
        best, best_d = None, WELD
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for v in grid[(cx + dx, cy + dy, cz + dz)]:
                        if staying_only and not (v < pinned or v in keep):
                            continue
                        d = (positions[v] - p).Length
                        if d <= best_d and (best is None or d < best_d or v < best):
                            best, best_d = v, d
        return best
    remap = list(range(len(positions)))
    for v in range(len(positions)):
        if v < pinned or v in keep:
            grid[cell(positions[v])].append(v)
    for v in range(pinned, len(positions)):
        if v in keep:
            continue
        w = nearest(positions[v], True)
        if w is None:
            w = nearest(positions[v], False)
        if w is None:
            grid[cell(positions[v])].append(v)
        else:
            remap[v] = w
    return remap


def _area(pos, t):
    return (pos[t[1]] - pos[t[0]]).cross(pos[t[2]] - pos[t[0]]).Length * 0.5


def _normal(pos, t):
    return (pos[t[1]] - pos[t[0]]).cross(pos[t[2]] - pos[t[0]])


def _centroid(pos, t):
    return (pos[t[0]] + pos[t[1]] + pos[t[2]]) * (1.0 / 3.0)


def decimate(positions, tris, surface, tolerance):
    """Removes interior vertices, collapsing each onto a neighbor while the triangles it changes stay within
    `tolerance` (mm) of the surface at their centroids and edge midpoints and keep facing out. A vertex on the
    mesh's boundary never moves: it is shared with another face."""
    tris = [list(t) for t in tris]
    alive = [True] * len(tris)
    vt = collections.defaultdict(set)
    edges = collections.Counter()
    for i, t in enumerate(tris):
        for v in t:
            vt[v].add(i)
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            edges[(min(a, b), max(a, b))] += 1
    fixed = {v for (a, b), n in edges.items() if n != 2 for v in (a, b)}
    flat = surface.kind == "Plane"

    def neighbors(v):
        return {u for i in vt[v] for u in tris[i] if u != v}

    def acceptable(t):
        n = _normal(positions, t)
        a = n.Length
        if a < 1e-9:
            return False
        c = _centroid(positions, t)
        if n.dot(surface.normal(c)) < 0.05 * a:
            return False
        if not flat:
            for p in [c] + [(positions[t[i]] + positions[t[(i + 1) % 3]]) * 0.5 for i in range(3)]:
                if surface.distance(p) > tolerance:
                    return False
        return True

    for _ in range(16):
        changed = False
        for v in range(len(positions)):
            if v in fixed or not vt[v]:
                continue
            nv = neighbors(v)
            for w in sorted(nv, key=lambda u: ((positions[u] - positions[v]).Length, u)):
                shared = vt[v] & vt[w]
                if len(shared) != 2:
                    continue
                # the link condition: v and w have no common neighbor but the two across their edge
                if nv & neighbors(w) != {u for i in shared for u in tris[i] if u not in (v, w)}:
                    continue
                trial = []
                for i in sorted(vt[v] - shared):
                    t = [w if u == v else u for u in tris[i]]
                    if not acceptable(t):
                        trial = None
                        break
                    trial.append((i, t))
                if trial is None:
                    continue
                for i in shared:
                    alive[i] = False
                    for u in tris[i]:
                        vt[u].discard(i)
                for i, t in trial:
                    tris[i] = t
                    vt[w].add(i)
                vt[v] = set()
                changed = True
                break
        if not changed:
            break
    used = sorted({v for i, t in enumerate(tris) if alive[i] for v in t})
    remap = {v: n for n, v in enumerate(used)}
    return [positions[v] for v in used], [tuple(remap[v] for v in t) for i, t in enumerate(tris) if alive[i]]


# ---------------------------------------------------------------------------------------------------------
# Charts


class Chart:
    """A surface flattened into (u, v) millimeters. kind "plane": along two axes, which each rectangle may
    choose for itself (`frames`); "polar": a plane around a center, u the arc length at a reference radius and
    v the radius, which only groups lettering (lettering_charts() turns it into a plane chart, whose mapping is
    linear); "cylinder": u the arc length, v along the axis; "cone": u the arc length at a reference radius, v
    the distance from the apex. Angles run about the axis from the direction e1, in (-pi, pi]. `rects` hold the
    decals: (u0, u1, v0, v1, full), full meaning all the way round."""

    def __init__(self, kind, surface, center=None, ref_radius=None):
        self.kind, self.surface = kind, surface
        self.axis = surface.axis
        self.center = center if center is not None else (surface.origin if kind == "plane" else surface.center)
        self.e1 = _perpendicular(self.axis)
        self.e2 = self.axis.cross(self.e1)
        self.ref_radius = surface.radius if kind == "cylinder" else (ref_radius or 1.0)
        self.rects = []
        # a plane chart's axes per rectangle, (origin, e1, e2), or empty for the chart's own
        self.frames = []
        # a thread's root radius, for its shading
        self.inner = 0.0

    @property
    def period(self):
        return None if self.kind == "plane" else 2 * math.pi * self.ref_radius

    def frame(self, rect):
        return self.frames[rect] if rect is not None and self.frames else (self.center, self.e1, self.e2)

    def to_uv(self, p, near_u=None, rect=None):
        """A point's (u, v), in a plane chart in the axes of rectangle `rect`. For an angular chart, near_u picks
        the turn of u nearest it."""
        if self.kind == "plane":
            o, e1, e2 = self.frame(rect)
            return (p - o).dot(e1), (p - o).dot(e2)
        d = p - self.center
        u = math.atan2(d.dot(self.e2), d.dot(self.e1)) * self.ref_radius
        if near_u is not None:
            u += self.period * round((near_u - u) / self.period)
        if self.kind == "polar":
            return u, (d - self.axis * d.dot(self.axis)).Length
        if self.kind == "cylinder":
            return u, d.dot(self.axis)
        return u, (self.surface.project(p) - self.surface.apex).Length

    def from_uv(self, u, v, rect=None):
        """Arrays of points on the surface at (u, v) and the face's outward normals there."""
        u, v = np.asarray(u, float), np.asarray(v, float)
        e1, e2, ax, c = (np.array(tuple(x)) for x in (self.e1, self.e2, self.axis, self.center))
        s = self.surface
        if self.kind == "plane":
            o, e1, e2 = (np.array(tuple(x)) for x in self.frame(rect))
            p = o + np.outer(u, e1) + np.outer(v, e2)
            return p, np.tile(np.array(tuple(s.normal(self.center))), (len(u), 1))
        a = u / self.ref_radius
        radial = np.outer(np.cos(a), e1) + np.outer(np.sin(a), e2)
        if self.kind == "polar":
            return c + radial * v[:, None], np.tile(np.array(tuple(s.normal(self.center))), (len(u), 1))
        if self.kind == "cylinder":
            return c + np.outer(v, ax) + radial * s.radius, radial * s.sign
        apex = np.array(tuple(s.apex))
        g = c + radial * s.ref_radius - apex
        g /= np.linalg.norm(g, axis=1)[:, None]
        n = radial - g * np.sum(radial * g, axis=1)[:, None]
        n /= np.linalg.norm(n, axis=1)[:, None]
        return apex + g * v[:, None], n * s.sign


def _merge(rects, period):
    """Merges rectangles that overlap or nearly do. In an angular chart, intervals are compared a turn apart
    too, and one that spans the turn less the gap becomes a full turn."""
    rects = [list(r) for r in rects]
    changed = True
    while changed:
        changed = False
        for i in range(len(rects)):
            for j in range(i + 1, len(rects)):
                a, b = rects[i], rects[j]
                if b[2] > a[3] + MERGE_GAP or a[2] > b[3] + MERGE_GAP:
                    continue
                for s in ([0.0] if period is None else [0.0, period, -period]):
                    if a[4] or b[4] or (b[0] + s <= a[1] + MERGE_GAP and a[0] <= b[1] + s + MERGE_GAP):
                        full = a[4] or b[4]
                        u0, u1 = min(a[0], b[0] + s), max(a[1], b[1] + s)
                        if period is not None and u1 - u0 >= period - 2 * MERGE_GAP:
                            full = True
                        rects[i] = [u0, u1, min(a[2], b[2]), max(a[3], b[3]), full]
                        del rects[j]
                        changed = True
                        break
                if changed:
                    break
            if changed:
                break
    out = []
    for r in rects:
        if r[4]:
            r[0], r[1] = -period / 2, period / 2
        out.append(tuple(r))
    return sorted(out)


def lettering_charts(topo, tess):
    """For each parent face, a chart and the rectangles its lettering covers, from the finest tessellation. A
    plane whose lettering runs around a center (a rotation mount's scale) takes a polar chart when that covers
    less area."""
    assigned, _ = topo.assignment(tess, ink_only=True)
    charts = {}
    for k in topo.parents:
        surface = topo.surfaces[k]
        groups = []
        for comp, parents in topo.components:
            if k in parents:
                pts = [surface.project(p) for j in comp for p in tess[j][0]]
                if pts:
                    groups.append(pts)
        if not groups or not assigned[k]:
            continue
        everything = [p for g in groups for p in g]

        def mean_radius(center):
            return sum(((p - center) - surface.axis * (p - center).dot(surface.axis)).Length
                       for p in everything) / len(everything)
        candidates = []
        if surface.kind == "Plane":
            candidates.append(Chart("plane", surface))
            circles = [e.Curve for e in topo.faces[k].Edges if type(e.Curve).__name__ == "Circle"
                       and abs(abs(V(e.Curve.Axis).dot(surface.axis)) - 1) < 1e-6]
            if circles:
                center = surface.project(V(max(circles, key=lambda c: c.Radius).Center))
                candidates.append(Chart("polar", surface, center=center, ref_radius=mean_radius(center)))
        elif surface.kind == "Cylinder":
            candidates.append(Chart("cylinder", surface))
        else:
            candidates.append(Chart("cone", surface, ref_radius=mean_radius(surface.center)))
        best = None
        for chart in candidates:
            rects = []
            for g in groups:
                near = chart.to_uv(g[0])[0] if chart.period else None
                uv = [chart.to_uv(p, near_u=near) for p in g]
                us, vs = [x[0] for x in uv], [x[1] for x in uv]
                rects.append((min(us) - MARGIN, max(us) + MARGIN, min(vs) - MARGIN, max(vs) + MARGIN, False))
            chart.rects = _merge(rects, chart.period)
            if chart.kind == "polar":
                chart = _polar_to_plane(chart)
            area = sum((r[1] - r[0]) * (r[3] - r[2]) for r in chart.rects)
            if best is None or area < best[0] - 1e-9:
                best = (area, chart)
        chart = best[1]
        # only this face's own share of the lettering is baked into it
        chart.source = assigned[k]
        charts[k] = chart
    return charts


def _polar_to_plane(polar):
    """A polar chart's rectangles as a plane chart's: each cut into arcs of at most MAX_STRIP and given axes of
    its own, along and across its middle, and the box of it in those axes. A plane chart maps linearly, so a
    long triangle (a decimated face is mostly long triangles) carries the texture without bending it."""
    chart = Chart("plane", polar.surface)
    c, ax, ref = polar.center, polar.axis, polar.ref_radius
    for u0, u1, v0, v1, full in polar.rects:
        n = max(1, int(math.ceil((u1 - u0) / MAX_STRIP)))
        for i in range(n):
            a0, a1 = (u0 + (u1 - u0) * i / n) / ref, (u0 + (u1 - u0) * (i + 1) / n) / ref
            mid = (a0 + a1) / 2
            radial = (polar.e1 * math.cos(mid) + polar.e2 * math.sin(mid)).normalize()
            e1 = ax.cross(radial).normalize()
            pts = []
            for k in range(17):
                a = a0 + (a1 - a0) * k / 16
                d = polar.e1 * math.cos(a) + polar.e2 * math.sin(a)
                pts += [c + d * max(0.0, v0), c + d * v1]
            us = [(p - c).dot(e1) for p in pts]
            vs = [(p - c).dot(radial) for p in pts]
            chart.rects.append((min(us), max(us), min(vs), max(vs), False))
            chart.frames.append((c, e1, radial))
    return chart


def proxy_chart(surface, length):
    """The chart of a proxy cylinder's side, all of it, all the way round."""
    chart = Chart("cylinder", surface)
    chart.rects = [(-chart.period / 2, chart.period / 2, -MARGIN, length + MARGIN, True)]
    return chart


# ---------------------------------------------------------------------------------------------------------
# The atlas


class Atlas:
    """Every rectangle of every chart of a model, packed in rows into one square texture."""

    def __init__(self, charts):
        # A long strip (a rotation mount's whole scale) is cut into pieces, so that it packs into rows instead
        # of setting the atlas's width
        for chart in charts:
            rects, frames = [], []
            for r, (u0, u1, v0, v1, full) in enumerate(chart.rects):
                n = max(1, int(math.ceil((u1 - u0) / MAX_STRIP)))
                rects += [(u0 + (u1 - u0) * i / n, u0 + (u1 - u0) * (i + 1) / n, v0, v1, full and n == 1)
                          for i in range(n)]
                if chart.frames:
                    frames += [chart.frames[r]] * n
            chart.rects, chart.frames = rects, frames
        self.items = [(chart, i) for chart in charts for i in range(len(chart.rects))]
        self.texel = TEXEL
        while not self._fit():
            self.texel *= 1.25

    def _fit(self):
        self.size = 64
        while self.size <= MAX_ATLAS:
            if self._pack():
                return True
            self.size *= 2
        return False

    def _pack(self):
        # each block lies with its long side across the atlas, which packs the rows tighter
        sizes = []
        self.turned = []
        for chart, i in self.items:
            r = chart.rects[i]
            w = int(math.ceil((r[1] - r[0]) / self.texel)) + 2 * PADDING
            h = int(math.ceil((r[3] - r[2]) / self.texel)) + 2 * PADDING
            self.turned.append(h > w)
            sizes.append((max(w, h), min(w, h)))
        # tallest first, each on the first shelf with room left in it, or on a new one
        shelves = []  # [y, height, width used]
        top = 0
        self.place = [None] * len(sizes)
        for n in sorted(range(len(sizes)), key=lambda n: (-sizes[n][1], -sizes[n][0], n)):
            w, h = sizes[n]
            if w > self.size:
                return False
            shelf = next((s for s in shelves if s[1] >= h and s[2] + w <= self.size), None)
            if shelf is None:
                if top + h > self.size:
                    return False
                shelf = [top, h, 0]
                shelves.append(shelf)
                top += h
            self.place[n] = (shelf[2], shelf[0], w, h)
            shelf[2] += w
        return True

    def item(self, chart, rect):
        return self.items.index((chart, rect))

    def uv(self, item, u, v):
        """A chart point's texture coordinates, v running down the image as glTF's do. A turned block holds u
        down the image and v across it."""
        chart, i = self.items[item]
        r = chart.rects[i]
        x, y, _, _ = self.place[item]
        a, b = (u - r[0]) / self.texel, (v - r[2]) / self.texel
        if self.turned[item]:
            a, b = b, a
        return (x + PADDING + a) / self.size, (y + PADDING + b) / self.size


# ---------------------------------------------------------------------------------------------------------
# Baking


def trace(tool, work, name, triangles, origins, directions, tmax):
    """Traces rays against triangles [(a, b, c)] with bin/mesh_visibility --trace. Returns arrays t (negative
    for a miss), triangle, b1, b2."""
    scene = os.path.join(work, name + ".omv")
    with open(scene, "wb") as f:
        f.write(b"OMV1" + struct.pack("<II", 1, 1) + struct.pack("<12f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0))
        f.write(struct.pack("<I", len(triangles)))
        f.write(np.array([[tuple(p) for p in t] for t in triangles], dtype="<f4").tobytes())
    rays = os.path.join(work, name + ".omr")
    with open(rays, "wb") as f:
        f.write(b"OMR1" + struct.pack("<I", len(origins)))
        f.write(np.hstack([origins, directions, tmax[:, None]]).astype("<f4").tobytes())
    hits = os.path.join(work, name + ".omh")
    out = subprocess.run([tool, "--trace", scene, rays, hits], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("mesh_visibility --trace failed: " + out.stderr)
    with open(hits, "rb") as f:
        data = f.read()
    assert data[:4] == b"OMH1"
    rec = np.frombuffer(data, dtype=np.dtype([("t", "<f4"), ("part", "<u4"), ("tri", "<u4"), ("b1", "<f4"),
                                               ("b2", "<f4")]), offset=8)
    for path in (scene, rays, hits):
        os.remove(path)
    return rec["t"], rec["tri"].astype(np.int64), rec["b1"], rec["b2"]


def bake(atlas, sources, tool, work, name):
    """The atlas as an (size, size, 3) array of linear RGB. `sources` maps id(chart) to how its texels are
    colored: (kind, triangles, vertex normals per triangle, finish color), kind "lettering", "spring" or
    "thread". Lettering is ink where the ray down onto the face hits the lettering it replaced, else the
    finish; a spring or a thread is the finish, shaded by the replaced geometry's normal and depth."""
    size = atlas.size
    image = np.zeros((size, size, 3))
    image[:] = 0.5
    for item, (chart, i) in enumerate(atlas.items):
        kind, tris, normals, color = sources[id(chart)]
        r = chart.rects[i]
        x0, y0, w, h = atlas.place[item]
        if atlas.turned[item]:
            w, h = h, w
        cols, rows = np.meshgrid(np.arange(w), np.arange(h))
        sub = (np.arange(SUPERSAMPLE) + 0.5) / SUPERSAMPLE
        su, sv = np.meshgrid(sub, sub)
        u = (r[0] + (cols[..., None, None] - PADDING + su) * atlas.texel).reshape(-1)
        v = (r[2] + (rows[..., None, None] - PADDING + sv) * atlas.texel).reshape(-1)
        p, n = chart.from_uv(u, v, rect=i)
        color = np.array(color)
        if not tris:
            block = np.tile(color, (len(u), 1))
        elif kind == "lettering":
            depth = max(chart.surface.distance(q) for t in tris for q in t) + 0.05
            t, _, _, _ = trace(tool, work, "%s.%d" % (name, item), tris, p + n * depth, -n,
                               np.full(len(u), 2 * depth))
            ink = np.array(INK)
            block = np.where((t >= 0)[:, None], ink, color)
        else:
            radius = chart.surface.radius
            t, tri, b1, b2 = trace(tool, work, "%s.%d" % (name, item), tris, p + n * 0.1, -n,
                                   np.full(len(u), radius + 0.1))
            hit = t >= 0
            nv = np.array([[tuple(q) for q in ns] for ns in normals])  # (triangles, 3, 3)
            k = np.where(hit, tri, 0)
            m = (nv[k, 0] * (1 - b1 - b2)[:, None] + nv[k, 1] * b1[:, None] + nv[k, 2] * b2[:, None])
            m /= np.maximum(np.linalg.norm(m, axis=1), 1e-12)[:, None]
            # the replaced faces' normals may point either way: a free-form face's are its triangles' mean
            facing = np.clip(np.abs(np.sum(m * n, axis=1)), 0.0, 1.0)
            if kind == "spring":
                shade = 0.55 + 0.45 * facing
            else:
                q = p + n * 0.1 - n * t[:, None]
                ax, c = np.array(tuple(chart.axis)), np.array(tuple(chart.surface.center))
                d = q - c
                radial = np.linalg.norm(d - np.outer(d @ ax, ax), axis=1)
                depth = np.clip((radial - chart.inner) / max(1e-6, radius - chart.inner), 0.0, 1.0)
                shade = (0.35 + 0.65 * facing) * (0.55 + 0.45 * depth)
            shade = np.where(hit, shade, GAP_SHADE)
            block = color[None, :] * shade[:, None]
        block = block.reshape(h, w, SUPERSAMPLE * SUPERSAMPLE, 3).mean(axis=2)
        if atlas.turned[item]:
            block = block.transpose(1, 0, 2)
        image[y0:y0 + block.shape[0], x0:x0 + block.shape[1]] = block
    return image


def srgb8(linear):
    c = np.clip(linear, 0.0, 1.0)
    s = np.where(c <= 0.0031308, 12.92 * c, 1.055 * np.power(c, 1 / 2.4) - 0.055)
    return np.round(s * 255).astype(np.uint8)


def png(image):
    """An 8-bit sRGB PNG of a linear RGB array, row 0 at the top."""
    h, w, _ = image.shape
    pixels = srgb8(image)
    raw = np.zeros((h, 1 + 3 * w), dtype=np.uint8)
    raw[:, 1:] = pixels.reshape(h, 3 * w)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw.tobytes(), 9)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------------------------------------------------
# Splitting a face's triangles along its chart's rectangles


def _cut(poly, axis, value):
    """A convex polygon of vertices (position, u, v) cut where coordinate `axis` (1 for u, 2 for v) equals
    value: (the part at or below it, the part at or above it), each empty when it has no area. The cut
    point on an edge is computed from the edge's endpoints in a fixed order, so that two triangles sharing
    the edge agree on it exactly."""
    below, above = [], []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        da, db = a[axis] - value, b[axis] - value
        if da <= 0:
            below.append(a)
        if da >= 0:
            above.append(a)
        if (da < 0 < db) or (db < 0 < da):
            p, q = (a, b) if tuple(a[0]) < tuple(b[0]) else (b, a)
            t = (value - p[axis]) / (q[axis] - p[axis])
            x = [p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t, p[2] + (q[2] - p[2]) * t]
            x[axis] = value
            below.append(tuple(x))
            above.append(tuple(x))

    def solid(poly):
        if len(poly) < 3:
            return []
        area = sum(((poly[i][0] - poly[0][0]).cross(poly[i + 1][0] - poly[0][0])).Length
                   for i in range(1, len(poly) - 1))
        return poly if area > 1e-12 else []
    return solid(below), solid(above)


def split_triangle(chart, items, points):
    """A triangle of a decal face cut along its chart's rectangles: a list of (polygon, item), each polygon
    convex with vertices (position, u, v), item the atlas item whose rectangle holds it or None. `items`
    gives the atlas item of each of the chart's rectangles."""
    period = chart.period
    if period is None:
        # a plane chart: each rectangle in its own axes, in which the mapping is linear
        out, rest = [], [list(points)]
        for r, (u0, u1, v0, v1, _) in enumerate(chart.rects):
            remaining = []
            for q in rest:
                inside = [(p,) + chart.to_uv(p, rect=r) for p in q]
                for axis, value, keep_above in ((1, u0, True), (1, u1, False), (2, v0, True), (2, v1, False)):
                    lo, hi = _cut(inside, axis, value)
                    outside, inside = (lo, hi) if keep_above else (hi, lo)
                    if outside:
                        remaining.append([x[0] for x in outside])
                    if not inside:
                        break
                if inside:
                    out.append((inside, items[r]))
            rest = remaining
        return out + [([(p, 0.0, 0.0) for p in q], None) for q in rest]
    poly, near = [], None
    for p in points:
        u, v = chart.to_uv(p, near_u=near)
        near = u if near is None else near
        poly.append((p, u, v))
    # into one turn, (-period/2, period/2], cut where the triangle crosses the seam
    lo, hi = _cut(poly, 1, period / 2)
    pieces = [q for q in (lo, [(x[0], x[1] - period, x[2]) for x in hi]) if q]
    wrapped = []
    for q in pieces:
        lo, hi = _cut(q, 1, -period / 2)
        wrapped += [w for w in ([(x[0], x[1] + period, x[2]) for x in lo], hi) if w]
    pieces = wrapped
    # a rectangle that is not a full turn may reach past the seam: try it a turn either way too
    rects = []
    for r, rect in enumerate(chart.rects):
        shifts = [0.0] if rect[4] else [0.0, period, -period]
        rects += [(rect, s, items[r]) for s in shifts]
    out = []
    for piece in pieces:
        rest = [piece]
        for (u0, u1, v0, v1, full), s, item in rects:
            remaining = []
            for q in rest:
                inside = q
                bounds = ([] if full else [(1, u0 + s, True), (1, u1 + s, False)]) + [(2, v0, True), (2, v1, False)]
                for axis, value, keep_above in bounds:
                    lo, hi = _cut(inside, axis, value)
                    outside, inside = (lo, hi) if keep_above else (hi, lo)
                    if outside:
                        remaining.append(outside)
                    if not inside:
                        break
                if inside:
                    out.append(([(x[0], x[1] - s, x[2]) for x in inside], item))
            rest = remaining
        out += [(q, None) for q in rest]
    return out
