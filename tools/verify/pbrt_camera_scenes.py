"""Writes hand-made pbrt-v4 scenes for the cameras of docs/plans/completed/pbrt-cameras.md that no scene in pbrt-v4-scenes
uses, so tools/verify/pbrt_compare.py can check the importer's camera against pbrt's own:

  assets/models/pbrt_cameras/orthographic.pbrt  an orthographic camera with a screen window off center. No lens: pbrt-v4's
                                                orthographic lens is a disk at the camera's origin, where this renderer's
                                                is centered on each film point's ray, and the importer reports it
  assets/models/pbrt_cameras/spherical.pbrt     a spherical camera with pbrt's equal-area mapping, on a square film
  assets/models/pbrt_cameras/shifted.pbrt       a perspective camera whose screen window is off center (a shift)
  assets/models/pbrt_cameras/thinlens.pbrt      a perspective camera with a lens radius and a focal distance

Each looks at the same asymmetric room (a red box on the left, a green sphere on the right, a light above), so a
mirrored or turned image shows in the comparison.

Run:  python3 tools/verify/pbrt_camera_scenes.py
      python3 tools/verify/pbrt_compare.py assets/models/pbrt_cameras/orthographic.pbrt
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402

OUT = vkscene.ROOT / "assets" / "models" / "pbrt_cameras"

HEADER = """{camera}
Sampler "zsobol" "integer pixelsamples" 64
Integrator "volpath" "integer maxdepth" 5
Film "rgb" "integer xresolution" {width} "integer yresolution" {height} "string filename" "{name}.exr"
PixelFilter "gaussian"
WorldBegin
"""

ROOM = """AttributeBegin
  Material "diffuse" "rgb reflectance" [0.7 0.7 0.7]
  Shape "trianglemesh" "integer indices" [0 1 2 0 2 3] "point3 P" [-4 0 -4  4 0 -4  4 0 4  -4 0 4]
  Shape "trianglemesh" "integer indices" [0 1 2 0 2 3] "point3 P" [-4 0 -2  4 0 -2  4 4 -2  -4 4 -2]
AttributeEnd
AttributeBegin
  Material "diffuse" "rgb reflectance" [0.7 0.1 0.08]
  Translate -1.1 0.5 -0.6
  Rotate 25 0 1 0
  Shape "trianglemesh" "integer indices" [0 1 2 0 2 3  4 6 5 4 7 6  0 4 5 0 5 1  1 5 6 1 6 2  2 6 7 2 7 3  3 7 4 3 4 0]
    "point3 P" [-0.5 -0.5 -0.5  0.5 -0.5 -0.5  0.5 -0.5 0.5  -0.5 -0.5 0.5  -0.5 0.5 -0.5  0.5 0.5 -0.5  0.5 0.5 0.5  -0.5 0.5 0.5]
AttributeEnd
AttributeBegin
  Material "diffuse" "rgb reflectance" [0.1 0.6 0.15]
  Translate 1.0 0.6 0.2
  Shape "sphere" "float radius" 0.6
AttributeEnd
AttributeBegin
  AreaLightSource "diffuse" "rgb L" [12 11 10]
  Shape "trianglemesh" "integer indices" [0 2 1 0 3 2] "point3 P" [-0.6 3 -0.8  0.6 3 -0.8  0.6 3 0.2  -0.6 3 0.2]
AttributeEnd
"""

LOOK = "LookAt 0 2 5  0 0.8 -0.5  0 1 0\n"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cameras = {
        # 4 wide and 3 tall, matching the film's 4:3, centered 0.5 right of and 0.3 above the axis
        "orthographic": (LOOK + 'Camera "orthographic" "float screenwindow" [-1.5 2.5 -1.2 1.8]', 320, 240),
        "spherical": ('LookAt 0 1.2 1.5  0 1.2 0.5  0 1 0\nCamera "spherical" "string mapping" "equalarea"', 256, 256),
        # the default window [-4/3, 4/3] x [-1, 1] moved a third right and a fifth up
        "shifted": (LOOK + 'Camera "perspective" "float fov" 45 "float screenwindow" [-1.0 1.6667 -0.8 1.2]', 320, 240),
        "thinlens": (LOOK + 'Camera "perspective" "float fov" 45 "float lensradius" 0.15 "float focaldistance" 4.5', 320, 240),
    }
    for name, (camera, width, height) in cameras.items():
        (OUT / f"{name}.pbrt").write_text(HEADER.format(camera=camera, width=width, height=height, name=name) + ROOM)
    print(f"wrote {', '.join(cameras)} into {OUT.relative_to(vkscene.ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
