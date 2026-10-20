"""The Cornell box of cornell.py as a pbrt-v4 scene, rendered by pbrt (the second independent reference).

pbrt is the user's own build: set PBRT to its binary, or put `pbrt` and `imgtool` on the PATH (the
default is ../pbr-book-website/pbrt-v4/build/ beside this repository). Its coordinate system is left
handed, so the scene starts with `Scale -1 1 1`, which mirrors the right-handed geometry back.
pbrt writes an EXR; imgtool converts it to the PFM the scripts read.

Matching choices: the box filter of radius 0.5 (a pixel's average, as the renderer's jitter makes
it), two-sided emission with a black light (the renderer's emitters absorb), and diffuse walls.
"""

import os
import shutil
import subprocess
from pathlib import Path

import numpy as np

import cornell
from vkscene import ROOT, read_pfm

DEFAULT_BUILD = ROOT.parent / "pbr-book-website" / "pbrt-v4" / "build"


def tool(name, env):
    if os.environ.get(env):
        return os.environ[env]
    candidate = DEFAULT_BUILD / name
    if candidate.exists():
        return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    raise FileNotFoundError(f"{name} not found: set {env} or put it on the PATH")


def quad_mesh(points):
    p = " ".join(f"{c:.6f}" for point in points for c in point)
    return f'Shape "trianglemesh" "integer indices" [0 1 2 0 2 3] "point3 P" [{p}]\n'


def box_corners(center, size, yaw_degrees):
    import math
    a = math.radians(yaw_degrees)
    c, s = math.cos(a), math.sin(a)
    rotation = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    half = np.asarray(size) / 2
    corners = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]) * half
    return (rotation @ corners.T).T + np.asarray(center)


def box_mesh(center, size, yaw_degrees):
    v = box_corners(center, size, yaw_degrees)
    # corner index = 4 sx + 2 sy + sz with each s in {0, 1}; faces as quads, two triangles each
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    indices = []
    for a, b, c, d in faces:
        indices += [a, b, c, a, c, d]
    p = " ".join(f"{x:.6f}" for point in v for x in point)
    i = " ".join(str(k) for k in indices)
    return f'Shape "trianglemesh" "integer indices" [{i}] "point3 P" [{p}]\n'


def scene_text(with_boxes, width, height, samples, exr_name, light_height=55.4, max_depth=60):
    out = []
    out.append("Scale -1 1 1\n")
    px, py, pz = cornell.CAMERA_POSITION
    tx, ty, tz = cornell.CAMERA_TARGET
    out.append(f"LookAt {px} {py} {pz}  {tx} {ty} {tz}  0 1 0\n")
    out.append(f'Camera "perspective" "float fov" {cornell.FOV}\n')
    out.append(f'Film "rgb" "integer xresolution" {width} "integer yresolution" {height} "string filename" "{exr_name}" "bool savefp16" false\n')
    out.append(f'Sampler "zsobol" "integer pixelsamples" {samples}\n')
    out.append('PixelFilter "box" "float xradius" 0.5 "float yradius" 0.5\n')
    out.append(f'Integrator "path" "integer maxdepth" {max_depth}\n')
    out.append("WorldBegin\n")
    for name, center, u, v, w, length, rho in cornell.WALLS:
        out.append("AttributeBegin\n")
        out.append(f'Material "diffuse" "rgb reflectance" [{rho[0]} {rho[1]} {rho[2]}]\n')
        out.append(quad_mesh(cornell.corners(center, u, v, w, length)))
        out.append("AttributeEnd\n")
    if with_boxes:
        for box in (cornell.TALL_BOX, cornell.SHORT_BOX):
            out.append("AttributeBegin\n")
            out.append(f'Material "diffuse" "rgb reflectance" [{cornell.WHITE[0]} {cornell.WHITE[1]} {cornell.WHITE[2]}]\n')
            out.append(box_mesh(*box))
            out.append("AttributeEnd\n")
    name, center, u, v, w, length = cornell.LIGHT
    center = (center[0], light_height, center[2])
    out.append("AttributeBegin\n")
    L = cornell.LIGHT_RADIANCE
    out.append(f'AreaLightSource "diffuse" "rgb L" [{L} {L} {L}] "bool twosided" true\n')
    out.append('Material "diffuse" "rgb reflectance" [0 0 0]\n')
    out.append(quad_mesh(cornell.corners(center, u, v, w, length)))
    out.append("AttributeEnd\n")
    return "".join(out)


def render_pbrt(with_boxes, width, height, samples, workdir, light_height=55.4, threads=None):
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    scene = workdir / "cornell.pbrt"
    exr = workdir / "pbrt_out.exr"
    scene.write_text(scene_text(with_boxes, width, height, samples, "pbrt_out.exr", light_height))
    if exr.exists():
        exr.unlink()
    cmd = [tool("pbrt", "PBRT"), "--outfile", str(exr), "--quiet"]
    if threads:
        cmd += ["--nthreads", str(threads)]
    subprocess.run(cmd + [str(scene)], check=True, cwd=workdir, capture_output=True, text=True)
    pfm = workdir / "pbrt_out.pfm"
    subprocess.run([tool("imgtool", "IMGTOOL"), "convert", "--outfile", str(pfm), str(exr)], check=True, capture_output=True, text=True)
    return read_pfm(pfm)
