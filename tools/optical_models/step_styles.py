"""Reads the colors and solid names of an AP214 STEP file, which FreeCAD keeps only in its GUI.

FreeCAD's command-line importer (`Part.read`) returns the solids without their styles. This module parses
the file's text for what it needs: each MANIFOLD_SOLID_BREP's name, its faces in shell order, the type of
each face's surface, and the COLOUR_RGB that a STYLED_ITEM gives the solid or the face. `match_solids()`
then pairs FreeCAD's solids with the file's by the sequence of their face types, which Open CASCADE keeps
in shell order, so a face's color is looked up by its index.

Plain Python with no FreeCAD import, so it also runs outside freecadcmd.
"""

import re

_ENTITY = re.compile(r"#(\d+)\s*=\s*(.*)", re.S)
_REF = re.compile(r"#(\d+)")

# Surface entity names to the coarse kinds FreeCAD's surface classes map to. Anything else (a B-spline, a
# surface of revolution or extrusion, an offset surface) is "other", which matches any FreeCAD type.
_SURFACE_KIND = {
    "PLANE": "Plane",
    "CYLINDRICAL_SURFACE": "Cylinder",
    "CONICAL_SURFACE": "Cone",
    "SPHERICAL_SURFACE": "Sphere",
    "TOROIDAL_SURFACE": "Toroid",
}


def _split_entities(text):
    data = text[text.index("DATA;") + 5:]
    data = data[:data.rindex("ENDSEC;")]
    entities = {}
    # Statements end in ';' outside quotes. Names in these files never hold a ';', but scan quotes anyway.
    start, quoted = 0, False
    for i, ch in enumerate(data):
        if ch == "'":
            quoted = not quoted
        elif ch == ";" and not quoted:
            m = _ENTITY.match(data[start:i].strip())
            if m:
                entities[int(m.group(1))] = m.group(2).strip()
            start = i + 1
    return entities


def _type_name(body):
    body = body.lstrip()
    if body.startswith("("):
        # A complex entity, "( BOUNDED_SURFACE ( ) B_SPLINE_SURFACE ( ... ) ... )": the first name that
        # says what kind of surface it is.
        names = re.findall(r"([A-Z_0-9]+)\s*\(", body)
        for n in names:
            if "SURFACE" in n and n not in ("BOUNDED_SURFACE", "GEOMETRIC_REPRESENTATION_ITEM", "SURFACE"):
                return n
        return names[0] if names else ""
    return body[:body.index("(")].strip()


def _args(body):
    return body[body.index("(") + 1:body.rindex(")")]


class StepStyles:
    """The solids of one STEP file: name, face surface kinds, and an RGB color or None per face."""

    def __init__(self, path):
        with open(path, "r", errors="replace") as f:
            text = f.read()
        self.entities = _split_entities(text)
        self._color_cache = {}
        styled = {}
        for eid, body in self.entities.items():
            t = _type_name(body)
            if t in ("STYLED_ITEM", "OVER_RIDING_STYLED_ITEM"):
                refs = [int(r) for r in _REF.findall(_args(body))]
                item = refs[-1] if t == "STYLED_ITEM" else refs[-2]
                color = None
                for r in refs[:-1]:
                    color = color or self._color(r, 0)
                if color is not None:
                    styled[item] = color
        self.solids = []
        for eid in sorted(self.entities):
            body = self.entities[eid]
            if _type_name(body) not in ("MANIFOLD_SOLID_BREP", "BREP_WITH_VOIDS"):
                continue
            name = re.match(r"\w+\s*\(\s*'([^']*)'", body).group(1)
            faces = []
            for shell in _REF.findall(_args(body)):
                shell_body = self.entities[int(shell)]
                faces += [int(r) for r in _REF.findall(_args(shell_body))]
            kinds, colors = [], []
            solid_color = styled.get(eid)
            for fid in faces:
                face = self.entities[fid]
                surface = int(_REF.findall(_args(face))[-1])
                kinds.append(_SURFACE_KIND.get(_type_name(self.entities[surface]), "other"))
                colors.append(styled.get(fid, solid_color))
            self.solids.append({"id": eid, "name": name, "kinds": kinds, "colors": colors})

    def _color(self, eid, depth):
        """The COLOUR_RGB reached from a style entity, following references depth-first."""
        if eid in self._color_cache:
            return self._color_cache[eid]
        body = self.entities.get(eid)
        result = None
        if body is not None and depth < 12:
            t = _type_name(body)
            if t == "COLOUR_RGB":
                values = _args(body).split(",")[1:]
                result = tuple(round(float(v), 4) for v in values)
            elif t == "DRAUGHTING_PRE_DEFINED_COLOUR":
                named = {"white": (1.0, 1.0, 1.0), "black": (0.0, 0.0, 0.0), "red": (1.0, 0.0, 0.0),
                         "green": (0.0, 1.0, 0.0), "blue": (0.0, 0.0, 1.0), "yellow": (1.0, 1.0, 0.0)}
                result = named.get(re.search(r"'([^']*)'", body).group(1))
            else:
                for r in _REF.findall(_args(body)):
                    result = self._color(int(r), depth + 1)
                    if result is not None:
                        break
        self._color_cache[eid] = result
        return result


def _kinds_match(step_kinds, freecad_kinds):
    if len(step_kinds) != len(freecad_kinds):
        return False
    return all(s == "other" or s == f for s, f in zip(step_kinds, freecad_kinds))


def match_solids(styles, freecad_solids):
    """For each FreeCAD solid, the StepStyles solid it came from, or None.

    A solid instanced twice in an assembly is one brep in the file and two solids in FreeCAD; both match it.
    Among equal candidates the earliest unused one wins, which keeps the file's order for distinct solids
    with the same topology.
    """
    fc_kinds = [[type(f.Surface).__name__ for f in s.Faces] for s in freecad_solids]
    used = set()
    result = []
    for kinds in fc_kinds:
        candidates = [i for i, s in enumerate(styles.solids) if _kinds_match(s["kinds"], kinds)]
        fresh = [i for i in candidates if i not in used]
        pick = (fresh or candidates or [None])[0]
        if pick is not None:
            used.add(pick)
        result.append(styles.solids[pick] if pick is not None else None)
    return result
