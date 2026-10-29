"""A minimal, deterministic glTF 2.0 binary writer: nodes, meshes, materials, one embedded PNG texture, and one
buffer.

Plain Python with no FreeCAD import. The same input gives the same bytes: keys are written in a fixed
order, floats are packed as float32, and nothing records a time or a path.
"""

import json
import struct


class GlbWriter:
    def __init__(self):
        self.buffer = bytearray()
        self.accessors = []
        self.views = []
        self.meshes = []
        self.nodes = []
        self.materials = []
        self.material_index = {}
        self.images = []
        self.textures = []

    def _view(self, data, target=None):
        while len(self.buffer) % 4:
            self.buffer.append(0)
        view = {"buffer": 0, "byteOffset": len(self.buffer), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.views.append(view)
        self.buffer += data
        return len(self.views) - 1

    def _accessor(self, view, component, count, kind, minmax=None):
        a = {"bufferView": view, "componentType": component, "count": count, "type": kind}
        if minmax is not None:
            a["min"], a["max"] = minmax
        self.accessors.append(a)
        return len(self.accessors) - 1

    def texture(self, png):
        """An sRGB PNG embedded in the buffer, sampled linearly with mipmaps and clamped. Returns its index."""
        self.images.append({"bufferView": self._view(png), "mimeType": "image/png"})
        self.textures.append({"sampler": 0, "source": len(self.images) - 1})
        return len(self.textures) - 1

    def material(self, name, base_color, metallic, roughness, alpha_mode=None, texture=None):
        if name in self.material_index:
            return self.material_index[name]
        pbr = {"baseColorFactor": [float(c) for c in base_color], "metallicFactor": float(metallic),
               "roughnessFactor": float(roughness)}
        if texture is not None:
            pbr["baseColorTexture"] = {"index": texture}
        m = {"name": name, "pbrMetallicRoughness": pbr}
        if alpha_mode:
            m["alphaMode"] = alpha_mode
        if base_color[3] >= 1.0 and not alpha_mode:
            m["doubleSided"] = False
        self.materials.append(m)
        self.material_index[name] = len(self.materials) - 1
        return self.material_index[name]

    def mesh(self, name, primitives):
        """primitives: a list of (positions [(x,y,z)], normals [(x,y,z)], indices [i], material index), with
        uvs [(u, v)] as a fifth element for a textured one."""
        prims = []
        for primitive in primitives:
            positions, normals, indices, material = primitive[:4]
            uvs = primitive[4] if len(primitive) > 4 else None
            pos = struct.pack("<%df" % (3 * len(positions)), *[c for p in positions for c in p])
            nrm = struct.pack("<%df" % (3 * len(normals)), *[c for n in normals for c in n])
            # Bounds from the float32 values as stored, which glTF validators compare against.
            stored = struct.unpack("<%df" % (3 * len(positions)), pos)
            lo = [min(stored[k::3]) for k in range(3)]
            hi = [max(stored[k::3]) for k in range(3)]
            p_acc = self._accessor(self._view(pos, 34962), 5126, len(positions), "VEC3", (lo, hi))
            n_acc = self._accessor(self._view(nrm, 34962), 5126, len(normals), "VEC3")
            if len(positions) < 65536:
                idx = struct.pack("<%dH" % len(indices), *indices)
                component = 5123
            else:
                idx = struct.pack("<%dI" % len(indices), *indices)
                component = 5125
            i_acc = self._accessor(self._view(idx, 34963), component, len(indices), "SCALAR")
            attributes = {"POSITION": p_acc, "NORMAL": n_acc}
            if uvs is not None:
                uv = struct.pack("<%df" % (2 * len(uvs)), *[c for t in uvs for c in t])
                attributes["TEXCOORD_0"] = self._accessor(self._view(uv, 34962), 5126, len(uvs), "VEC2")
            prims.append({"attributes": attributes, "indices": i_acc, "material": material, "mode": 4})
        self.meshes.append({"name": name, "primitives": prims})
        return len(self.meshes) - 1

    def node(self, name, mesh=None, children=None, extras=None):
        n = {"name": name}
        if mesh is not None:
            n["mesh"] = mesh
        if children:
            n["children"] = children
        if extras:
            n["extras"] = extras
        self.nodes.append(n)
        return len(self.nodes) - 1

    def write(self, path, root, extras=None):
        while len(self.buffer) % 4:
            self.buffer.append(0)
        doc = {"asset": {"version": "2.0", "generator": "tools/optical_models/convert.py"},
               "scene": 0, "scenes": [{"nodes": [root]}], "nodes": self.nodes, "meshes": self.meshes,
               "materials": self.materials, "accessors": self.accessors, "bufferViews": self.views,
               "buffers": [{"byteLength": len(self.buffer)}]}
        if self.textures:
            # linear filtering with mipmaps, clamped at the edges
            doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}]
            doc["images"], doc["textures"] = self.images, self.textures
        if extras:
            doc["scenes"][0]["extras"] = extras
        text = json.dumps(doc, separators=(",", ":"), sort_keys=False).encode("utf-8")
        while len(text) % 4:
            text += b" "
        total = 12 + 8 + len(text) + 8 + len(self.buffer)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))
            f.write(struct.pack("<II", len(text), 0x4E4F534A))
            f.write(text)
            f.write(struct.pack("<II", len(self.buffer), 0x004E4942))
            f.write(self.buffer)
