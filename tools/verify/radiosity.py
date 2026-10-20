"""An independent solution of the empty Cornell box (assets/scenes/cornell_empty.gltf): classical
radiosity with exact point-to-polygon form factors, and nothing from the renderer. It shares the scene's
geometry (cornell.py) and nothing else, so an error the renderer's own tests share (a wrong factor of pi
in a light's power, a cosine in the wrong place) shows up as a difference here.

The method: split every wall into n x n patches; solve B = E + rho F B for the patch radiosities by
iteration (the box is open at the front, so what leaves through it is lost); then evaluate each pixel
with a final gather, the exact form factor from the pixel's hit point to every patch, which removes
most of the error of treating a patch as a point. Radiance of a diffuse surface is B / pi.

The light is an opaque black emitter 0.1 below the ceiling. It is seen only from below (a point at or
above its plane gets nothing from it) and it hides the ceiling behind it (those patches reflect nothing
into the room).

Modelling differences from the renderer, all small and stated by the check that uses this:
 - the light emits on its underside only; the renderer emits on both sides, and the upward half lands in
   the 0.1 gap above the light, where it is absorbed, apart from what escapes at the edge
 - patches are flat-shaded for the solve, so the result is exact only as n grows
"""

import numpy as np

import cornell


def polygon_form_factor(points, normals, polygons):
    """The form factor from each of R receivers (a point and a normal) to each of S convex
    polygons (S, 4, 3), as an (R, S) array: (1/2pi) * |sum of edge angle * (n . edge plane normal)|
    (Nusselt's polygon formula). The polygons must lie in front of the receivers' tangent planes."""
    v = polygons[None, :, :, :] - points[:, None, None, :]          # (R, S, 4, 3)
    v = v / np.linalg.norm(v, axis=-1, keepdims=True)
    v_next = np.roll(v, -1, axis=2)
    cos_angle = np.clip(np.sum(v * v_next, axis=-1), -1.0, 1.0)
    angle = np.arccos(cos_angle)                                     # (R, S, 4)
    cross = np.cross(v, v_next)
    cross = cross / np.maximum(np.linalg.norm(cross, axis=-1, keepdims=True), 1e-300)
    weight = np.sum(cross * normals[:, None, None, :], axis=-1)      # (R, S, 4)
    return np.abs(np.sum(angle * weight, axis=-1)) / (2.0 * np.pi)


class Cornell:
    def __init__(self, n=24):
        self.n = n
        self.polygons = []       # (4, 3) per patch
        self.centers = []
        self.normals = []
        self.reflectance = []
        self.wall_of = []        # which wall a patch belongs to
        self.emission = []       # radiosity emitted
        for wall_index, (name, center, u, v, width, length, rho) in enumerate(cornell.WALLS):
            u = np.asarray(u, dtype=np.float64)
            v = np.asarray(v, dtype=np.float64)
            normal = np.cross(v, u)
            # the normal faces into the box: toward the middle of it
            middle = np.array([cornell.SIZE / 2] * 3)
            if np.dot(normal, middle - np.asarray(center)) < 0:
                normal = -normal
            for i in range(n):
                for j in range(n):
                    c = np.asarray(center) + u * ((i + 0.5) / n - 0.5) * width + v * ((j + 0.5) / n - 0.5) * length
                    self.polygons.append(cornell.corners(c, u, v, width / n, length / n))
                    self.centers.append(c)
                    self.normals.append(normal)
                    self.reflectance.append(np.asarray(rho))
                    self.wall_of.append(wall_index)
                    self.emission.append(np.zeros(3))
        # the light: one more patch, facing down, a black absorber with radiosity pi * L
        name, center, u, v, width, length = cornell.LIGHT
        self.light_polygon = cornell.corners(center, u, v, width, length)
        self.polygons.append(self.light_polygon)
        self.centers.append(np.asarray(center))
        self.normals.append(np.array([0.0, -1.0, 0.0]))
        self.reflectance.append(np.zeros(3))
        self.wall_of.append(len(cornell.WALLS))
        self.emission.append(np.full(3, np.pi * cornell.LIGHT_RADIANCE))
        # the ceiling behind the light is hidden by it: those patches reflect nothing into the room
        lo = np.minimum.reduce(self.light_polygon)
        hi = np.maximum.reduce(self.light_polygon)
        ceiling = [w for w, wall in enumerate(cornell.WALLS) if wall[0] == "ceiling"][0]
        for k in range(len(self.centers) - 1):
            c = self.centers[k]
            if self.wall_of[k] == ceiling and lo[0] <= c[0] <= hi[0] and lo[2] <= c[2] <= hi[2]:
                self.reflectance[k] = np.zeros(3)
        self.polygons = np.array(self.polygons)
        self.centers = np.array(self.centers)
        self.normals = np.array(self.normals)
        self.reflectance = np.array(self.reflectance)
        self.wall_of = np.array(self.wall_of)
        self.emission = np.array(self.emission)
        self.radiosity = self.solve()

    def form_factors_from(self, points, normals, own_wall=None, chunk=64):
        """(R, S) form factors from points to every patch, zero for patches of the point's own wall."""
        out = np.zeros((len(points), len(self.polygons)))
        for start in range(0, len(points), chunk):
            stop = min(start + chunk, len(points))
            out[start:stop] = polygon_form_factor(points[start:stop], normals[start:stop], self.polygons)
        if own_wall is not None:
            out[self.wall_of[None, :] == np.asarray(own_wall)[:, None]] = 0.0
        # the light is seen from below only
        out[points[:, 1] >= self.light_polygon[0, 1], -1] = 0.0
        return out

    def solve(self, iterations=80):
        count = len(self.polygons)
        patches = count - 1
        F = self.form_factors_from(self.centers[:patches], self.normals[:patches], self.wall_of[:patches])
        B = self.emission.copy()
        rho = self.reflectance
        for _ in range(iterations):
            B[:patches] = self.emission[:patches] + rho[:patches] * (F @ B)
        return B

    def radiance_at(self, points, normals, walls):
        """The outgoing radiance of the wall at each point (diffuse: rho/pi times the irradiance gathered)."""
        F = self.form_factors_from(points, normals, walls)
        irradiance_over_pi = F @ self.radiosity                      # (R, 3): sum F_ij B_j
        rho = np.array([cornell.WALLS[w][6] for w in walls])
        return rho * irradiance_over_pi / np.pi

    def render(self, width, height):
        """The empty Cornell box as the renderer's camera sees it: radiance per pixel, (height, width, 3)."""
        import math
        position = np.asarray(cornell.CAMERA_POSITION)
        forward = np.asarray(cornell.CAMERA_TARGET) - position
        forward /= np.linalg.norm(forward)
        right = np.cross(forward, [0.0, 1.0, 0.0])
        right /= np.linalg.norm(right)
        up = np.cross(right, forward)
        t = math.tan(math.radians(cornell.FOV) / 2)
        ys, xs = np.mgrid[0:height, 0:width]
        sx = ((xs + 0.5) / width * 2 - 1) * t
        sy = (1 - (ys + 0.5) / height * 2) * t
        d = forward[None, None, :] + sx[..., None] * right + sy[..., None] * up
        d = d.reshape(-1, 3)
        d /= np.linalg.norm(d, axis=1, keepdims=True)
        o = position

        best_t = np.full(len(d), np.inf)
        best_wall = np.full(len(d), -1)
        # the five walls are axis-aligned planes: (axis, coordinate, lo/hi extents on the other axes)
        planes = []
        for w, (name, center, u, v, width_, length, rho) in enumerate(cornell.WALLS):
            axis = int(np.argmax(np.abs(np.cross(u, v))))
            planes.append((w, axis, center[axis]))
        for w, axis, coordinate in planes:
            with np.errstate(divide="ignore", invalid="ignore"):
                t_hit = (coordinate - o[axis]) / d[:, axis]
            p = o + t_hit[:, None] * d
            inside = np.all((p >= -1e-9) & (p <= cornell.SIZE + 1e-9), axis=1) & (t_hit > 0)
            closer = inside & (t_hit < best_t)
            best_t[closer] = t_hit[closer]
            best_wall[closer] = w
        # the light, a horizontal quad at y = 55.4
        light = self.light_polygon
        y = light[0, 1]
        with np.errstate(divide="ignore", invalid="ignore"):
            t_light = (y - o[1]) / d[:, 1]
        p = o + t_light[:, None] * d
        lo = light.min(axis=0)
        hi = light.max(axis=0)
        in_light = (t_light > 0) & (p[:, 0] >= lo[0]) & (p[:, 0] <= hi[0]) & (p[:, 2] >= lo[2]) & (p[:, 2] <= hi[2]) & (t_light < best_t)
        best_t[in_light] = t_light[in_light]
        best_wall[in_light] = len(cornell.WALLS)

        #which wall each pixel shows (len(WALLS) for the light, -1 for none), for the checks that
        #compare walls one at a time
        self.wall_map = best_wall.reshape(height, width)
        image = np.zeros((len(d), 3))
        light_pixels = best_wall == len(cornell.WALLS)
        image[light_pixels] = cornell.LIGHT_RADIANCE
        wall_pixels = (best_wall >= 0) & ~light_pixels
        index = np.nonzero(wall_pixels)[0]
        hit = o + best_t[index, None] * d[index]
        walls = best_wall[index]
        normals = np.array([self.normals[np.nonzero(self.wall_of == w)[0][0]] for w in walls])
        image[index] = self.radiance_at(hit, normals, walls)
        return image.reshape(height, width, 3)


if __name__ == "__main__":
    import sys
    from vkscene import save_png, write_pfm
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 24
    box = Cornell(n)
    img = box.render(128, 128)
    print("mean radiance", img.mean(axis=(0, 1)))
    write_pfm("radiosity.pfm", img)
    save_png("radiosity.png", img, 2.0)
