"""Writes hand-made pbrt-v4 scenes for the features of docs/plans/completed/pbrt-light-types.md and docs/plans/completed/pbrt-textures.md
that no scene in pbrt-v4-scenes uses, so tools/verify/pbrt_compare.py can check the importer against pbrt itself:

  assets/models/pbrt_lights/projection.pbrt    a projection light throwing an asymmetric image onto a floor, turned
  assets/models/pbrt_lights/goniometric.pbrt   a goniometric light whose distribution is brighter in one quadrant
  assets/models/pbrt_lights/textured.pbrt      imagemap textures with uv scale and a planar mapping, a one-sided area
                                               light with an image, and a spot light's smoothstep falloff

and their images (projection.png, goniometric.pfm, checker.png). Every image is asymmetric, so a flip or a turn of
any lookup shows in the comparison.

Run:  python3 tools/verify/pbrt_light_scenes.py
      python3 tools/verify/pbrt_compare.py assets/models/pbrt_lights/projection.pbrt
"""

import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402
from light_texture_scenes import write_png  # noqa: E402

OUT = vkscene.ROOT / "assets" / "models" / "pbrt_lights"

HEADER = """LookAt 0 4 5.5  0 0 -0.5  0 1 0
Camera "perspective" "float fov" 50
Sampler "zsobol" "integer pixelsamples" 64
Integrator "volpath" "integer maxdepth" {depth}
Film "rgb" "integer xresolution" 320 "integer yresolution" 240 "string filename" "{name}.exr"
PixelFilter "gaussian"
WorldBegin
"""

ROOM = """Material "diffuse" "rgb reflectance" [0.7 0.7 0.7]
Shape "trianglemesh" "integer indices" [0 1 2 0 2 3] "point3 P" [-5 0 -5  5 0 -5  5 0 5  -5 0 5]
Shape "trianglemesh" "integer indices" [0 1 2 0 2 3] "point3 P" [-5 0 -3  5 0 -3  5 5 -3  -5 5 -3]
"""


def equal_area_direction(u, v):
    """pbrt-v4's EqualAreaSquareToSphere, for the goniometric image"""
    uu, vv = 2 * u - 1, 2 * v - 1
    up, vp = abs(uu), abs(vv)
    signed = 1 - (up + vp)
    r = 1 - abs(signed)
    phi = (1 if r == 0 else (vp - up) / r + 1) * math.pi / 4
    z = math.copysign(1 - r * r, signed)
    s = r * math.sqrt(max(2 - r * r, 0))
    return (math.copysign(math.cos(phi), uu) * s, math.copysign(math.sin(phi), vv) * s, z)


def main():
    OUT.mkdir(parents=True, exist_ok=True)

    # the projection: red on the left, green on the right, a blue band along the top
    write_png(OUT / "projection.png", 64, 32, lambda x, y: (0, 0, 255) if y < 8 else ((255, 40, 40) if x < 32 else (40, 255, 40)))
    (OUT / "projection.pbrt").write_text(HEADER.format(depth=1, name="projection") + """AttributeBegin
  Translate 0 2.5 0
  Rotate 90 1 0 0
  Rotate 30 0 0 1
  LightSource "projection" "string filename" "projection.png" "float fov" 60 "float scale" 6
AttributeEnd
""" + ROOM)

    # the goniometric light: pbrt's equal-area square in its frame with y and z swapped, bright where that direction's
    # x and its z (pbrt's swapped y) are both positive, and a ring of bands around the axis
    size = 64
    image = np.zeros((size, size, 3))
    for row in range(size):
        for col in range(size):
            d = equal_area_direction((col + 0.5) / size, (row + 0.5) / size)
            value = 0.3 + (2.0 if d[0] > 0 and d[2] > 0.2 else 0.0) + 0.5 * (math.sin(6 * math.atan2(d[1], d[0])) > 0)
            image[row, col] = (value, value * 0.8, value * 0.6)
    vkscene.write_pfm(OUT / "goniometric.pfm", image)
    (OUT / "goniometric.pbrt").write_text(HEADER.format(depth=1, name="goniometric") + """AttributeBegin
  Translate 0 2.5 0
  Rotate 35 0 1 0
  LightSource "goniometric" "string filename" "goniometric.pfm" "float scale" 3 "rgb I" [1 1 1]
AttributeEnd
""" + ROOM)

    # textures and the area light
    write_png(OUT / "checker.png", 128, 128, lambda x, y: (230, 60, 40) if ((x // 16) + (y // 32)) % 2 == 0 else (30, 30, 200))
    (OUT / "textured.pbrt").write_text(HEADER.format(depth=3, name="textured") + """Texture "tiles" "spectrum" "imagemap" "string filename" "checker.png" "float uscale" 3 "float vscale" 2 "float udelta" 0.25
Texture "planar" "spectrum" "imagemap" "string filename" "checker.png" "string mapping" "planar" "vector3 v1" [0.5 0 0] "vector3 v2" [0 0 0.5] "float udelta" 0.1
LightSource "spot" "point3 from" [-2 4 2] "point3 to" [0 0 0] "float coneangle" 35 "float conedeltaangle" 20 "rgb I" [8 8 8]
AttributeBegin
  Material "diffuse" "texture reflectance" "planar"
  Shape "trianglemesh" "integer indices" [0 1 2 0 2 3] "point3 P" [-5 0 -5  5 0 -5  5 0 5  -5 0 5]
AttributeEnd
AttributeBegin
  Material "diffuse" "texture reflectance" "tiles"
  Shape "trianglemesh" "integer indices" [0 1 2 0 2 3] "point3 P" [-5 0 -3  5 0 -3  5 5 -3  -5 5 -3] "point2 uv" [0 0 1 0 1 1 0 1]
AttributeEnd
AttributeBegin
  Translate 1.5 1.2 -1
  Rotate -60 1 0 0
  AreaLightSource "diffuse" "string filename" "checker.png" "float scale" 3
  Material "diffuse" "rgb reflectance" [0.5 0.5 0.5]
  Shape "trianglemesh" "integer indices" [0 1 2 0 2 3] "point3 P" [-0.6 -0.6 0  0.6 -0.6 0  0.6 0.6 0  -0.6 0.6 0] "point2 uv" [0 0 1 0 1 1 0 1]
AttributeEnd
""")
    print("wrote assets/models/pbrt_lights/: projection.pbrt, goniometric.pbrt, textured.pbrt and their images")
    return 0


if __name__ == "__main__":
    sys.exit(main())
