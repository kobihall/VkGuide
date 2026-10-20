#!/usr/bin/env python3
"""Writes the verification scenes and the uniform environment map they use:

  assets/scenes/white_furnace.gltf     a gallery of spheres of every material under a white background
  assets/scenes/cornell_reference.gltf the Cornell box, diffuse, with its two boxes
  assets/scenes/cornell_empty.gltf     the Cornell box without them, which radiosity.py solves exactly
  assets/environment_maps/uniform_white.hdr   radiance 1 in every direction

Run from anywhere:  python3 tools/verify/make_scenes.py
"""

import struct
from pathlib import Path

import numpy as np

import cornell
from vkscene import ROOT, camera, make_scene, sphere, write_scene

# the materials of the gallery and of furnace_check.py, all with a white base colour
FURNACE_MATERIALS = [
    ("lambertian 1", {"type": "lambertian", "albedo": [1.0, 1.0, 1.0]}),
    ("lambertian 0.5", {"type": "lambertian", "albedo": [0.5, 0.5, 0.5]}),
    ("metal mirror", {"type": "metal", "albedo": [1.0, 1.0, 1.0], "fuzz": 0.0}),
    ("metal fuzz 0.3", {"type": "metal", "albedo": [1.0, 1.0, 1.0], "fuzz": 0.3}),
    ("phong 0.5", {"type": "phong", "albedo": [1.0, 1.0, 1.0], "smoothness": 0.5}),
    ("dielectric 1.5", {"type": "dielectric", "ir": 1.5}),
    ("pbr rough 0.2 metal 0", {"type": "pbr", "albedo": [1.0, 1.0, 1.0], "metallic": 0.0, "roughness": 0.2, "emission": [0, 0, 0], "strength": 1.0}),
    ("pbr rough 0.5 metal 0", {"type": "pbr", "albedo": [1.0, 1.0, 1.0], "metallic": 0.0, "roughness": 0.5, "emission": [0, 0, 0], "strength": 1.0}),
    ("pbr rough 1 metal 0", {"type": "pbr", "albedo": [1.0, 1.0, 1.0], "metallic": 0.0, "roughness": 1.0, "emission": [0, 0, 0], "strength": 1.0}),
    ("pbr rough 0.2 metal 1", {"type": "pbr", "albedo": [1.0, 1.0, 1.0], "metallic": 1.0, "roughness": 0.2, "emission": [0, 0, 0], "strength": 1.0}),
    ("pbr rough 0.5 metal 1", {"type": "pbr", "albedo": [1.0, 1.0, 1.0], "metallic": 1.0, "roughness": 0.5, "emission": [0, 0, 0], "strength": 1.0}),
    ("pbr rough 1 metal 1", {"type": "pbr", "albedo": [1.0, 1.0, 1.0], "metallic": 1.0, "roughness": 1.0, "emission": [0, 0, 0], "strength": 1.0}),
]


def write_uniform_hdr(path):
    """A 64 x 32 Radiance file whose every texel is (1, 1, 1): RGBE (128, 128, 128, 129)."""
    width, height = 64, 32
    with open(path, "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n")
        f.write(f"-Y {height} +X {width}\n".encode())
        f.write(bytes([128, 128, 128, 129]) * (width * height))


def furnace_scene(material, **kwargs):
    """One sphere of `material` at the origin, seen from z = 4."""
    cam = camera((0.0, 0.0, 4.0), (0.0, 0.0, 0.0), 40.0)
    return make_scene([sphere("ball", (0.0, 0.0, 0.0), 1.0, material)], cam, 64, 64, ray_depth=16, **kwargs)


def gallery():
    columns = 4
    shapes = []
    for i, (name, material) in enumerate(FURNACE_MATERIALS):
        row, column = divmod(i, columns)
        x = (column - (columns - 1) / 2) * 3.0
        y = (1 - row) * 3.0
        shapes.append(sphere(name, (x, y, 0.0), 1.0, material))
    cam = camera((0.0, 0.0, 16.0), (0.0, 0.0, 0.0), 40.0)
    return make_scene(shapes, cam, 640, 480, background=(1.0, 1.0, 1.0), samples=256, ray_depth=16)


def main():
    scenes = ROOT / "assets" / "scenes"
    write_uniform_hdr(ROOT / "assets" / "environment_maps" / "uniform_white.hdr")
    write_scene(scenes / "white_furnace.gltf", gallery())
    write_scene(scenes / "cornell_reference.gltf", cornell.scene(True))
    write_scene(scenes / "cornell_empty.gltf", cornell.scene(False))
    print("wrote white_furnace.gltf, cornell_reference.gltf, cornell_empty.gltf and uniform_white.hdr")


if __name__ == "__main__":
    main()
