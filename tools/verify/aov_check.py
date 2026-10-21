#!/usr/bin/env python3
"""Checks of the auxiliary outputs and the image export (docs/plans/completed/pbrt-aovs-and-image-output.md),
through the batch mode (`vulkan_guide --render`) and the files it writes:

  plane       a Lambertian quad facing the camera in a uniform light: its albedo, normals, depth, position, ids and
              sample count are known, a pixel that missed has none of them, and a sample that agrees with itself has
              no variance
  through     a glass sphere in front of that quad: the first surface is the glass (albedo 1, depth to its front),
              and capturing through specular records the quad behind it
  groups      a scene of several lights, each in its group: the groups sum to the picture, and are not empty
  ao          the ambient occlusion view, inside a sphere where the fraction of cosine-weighted rays that travel a
              length L without meeting the wall is 1 - (L / 2R)^2
  files       a PNG equals the display image of the same render (sqrt of the film, to 8 bits), an HDR file the PFM to
              RGBE's precision, a half-float EXR the float one to half's, and an EXR environment map renders what
              the Radiance map it was made from does
  relight     a render relit with a group at scale 0 equals the render without that group

and, with --baseline DIR (a copy of the repository's bin/, shaders/ and assets/ from before the change, whose
vulkan_guide still takes VKGUIDE_VERIFY), that a render that keeps no output is bit-identical to the one it made.

Run:  python3 tools/verify/aov_check.py [--spp N] [--baseline DIR] [--only NAME]
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import numpy as np

import vkscene
from vkscene import BIN, ROOT, LAMBERTIAN, EMISSIVE, camera, make_scene, quad, run_render, sphere, write_scene

EXR_DUMP = BIN / "exr_dump"
IMAGE_CONVERT = BIN / "image_convert"
failures = []


def check(ok, message):
    print(("  ok:     " if ok else "  FAILED: ") + message)
    if not ok:
        failures.append(message)
    return ok


# ---------------------------------------------------------------- reading what the renderer wrote

def read_exr(path, workdir):
    """Every channel of an EXR file as a float array (height, width), and its attributes as text."""
    out = Path(workdir) / (Path(path).stem + "_npy")
    result = subprocess.run([str(EXR_DUMP), str(path), "--npy", str(out)], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stdout + result.stderr)
    channels = {}
    for npy in out.glob("*.npy"):
        channels[npy.stem] = np.load(npy).astype(np.float64)
    attributes = {}
    for line in result.stdout.splitlines():
        if line.strip().startswith("attribute "):
            head, _, value = line.strip()[len("attribute "):].partition(": ")
            attributes[head.split(" ")[0]] = value
    return channels, attributes


def read_png(path):
    """An 8-bit RGB PNG as an array (height, width, 3): the chunks, then each scanline's filter undone."""
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    idat = b""
    width = height = 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and color == 2 and interlace == 0
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 3
    image = np.zeros((height, stride), dtype=np.int32)
    previous = np.zeros(stride, dtype=np.int32)
    offset = 0
    for y in range(height):
        kind = raw[offset]
        line = np.frombuffer(raw[offset + 1:offset + 1 + stride], dtype=np.uint8).astype(np.int32)
        offset += 1 + stride
        if kind == 0:
            current = line
        elif kind == 1:
            current = line.copy()
            for x in range(3, stride):
                current[x] = (current[x] + current[x - 3]) & 255
        elif kind == 2:
            current = (line + previous) & 255
        elif kind == 3:
            current = line.copy()
            for x in range(stride):
                left = current[x - 3] if x >= 3 else 0
                current[x] = (current[x] + ((left + previous[x]) >> 1)) & 255
        else:
            current = line.copy()
            for x in range(stride):
                a = current[x - 3] if x >= 3 else 0
                b = previous[x]
                c = previous[x - 3] if x >= 3 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                current[x] = (current[x] + predictor) & 255
        image[y] = current
        previous = current
    return image.reshape(height, width, 3)


def read_hdr(path):
    """A Radiance RGBE file (flat or run-length encoded scanlines) as floats (height, width, 3)."""
    data = Path(path).read_bytes()
    header_end = data.index(b"\n\n") + 2
    line_end = data.index(b"\n", header_end)
    _, height, _, width = data[header_end:line_end].decode().split()
    height, width = int(height), int(width)
    pos = line_end + 1
    rgbe = np.zeros((height, width, 4), dtype=np.uint8)
    for y in range(height):
        if width >= 8 and width < 32768 and data[pos] == 2 and data[pos + 1] == 2 and (data[pos + 2] << 8 | data[pos + 3]) == width:
            pos += 4
            for channel in range(4):
                x = 0
                while x < width:
                    count = data[pos]
                    pos += 1
                    if count > 128:
                        count -= 128
                        rgbe[y, x:x + count, channel] = data[pos]
                        pos += 1
                    else:
                        rgbe[y, x:x + count, channel] = np.frombuffer(data[pos:pos + count], dtype=np.uint8)
                        pos += count
                    x += count
        else:
            rgbe[y] = np.frombuffer(data[pos:pos + 4 * width], dtype=np.uint8).reshape(width, 4)
            pos += 4 * width
    exponent = rgbe[..., 3].astype(np.int32)
    scale = np.where(exponent > 0, np.ldexp(1.0, exponent - 136), 0.0)
    return rgbe[..., :3].astype(np.float64) * scale[..., None]


# ---------------------------------------------------------------- scenes

def library_scene(shapes, cam, width, height, *, groups=None, lights=None, background=None, light_group_names=None,
                  environment_group=0, environment_map=None, samples=64, ray_depth=8, kernels=None, render_extra=None):
    """A version-16 scene: the harness's shapes with their inline materials moved into the library (so a material can
    name a light group), the optional punctual lights, and the group names."""
    materials = []
    placed = []
    for shape in shapes:
        inline = dict(shape["material"])
        entry = {"name": shape["name"], "type": inline["type"]}
        for key, value in inline.items():
            if key == "type":
                continue
            entry["albedo" if (inline["type"] == "emissive" and key == "color") else key] = value
        if groups and shape["name"] in groups:
            entry["lightGroup"] = groups[shape["name"]]
        materials.append(entry)
        placed.append(dict(shape, material=len(materials) - 1))
    render = {"width": width, "height": height, "matchViewport": False, "antialiasing": True, "maxSamples": samples,
              "unlimitedSamples": False, "rayDepth": ray_depth, "useFixedSeed": True, "seed": 1, "samplesPerFrame": 1,
              "russianRoulette": False, "minBouncesBeforeRoulette": 4, "restartOnChange": False,
              "pixelFilter": {"kind": "gaussian", "radius": 1.5, "sigma": 0.5}}
    render.update(render_extra or {})
    extras = {"version": 16, "models": [], "environmentIntensity": 1.0, "render": render, "renderCamera": 0,
              "cameras": [cam], "geometry": placed, "meshObjects": [], "materials": materials, "materialOverrides": [],
              "lights": lights or [], "lightGroupNames": light_group_names or ["Default"], "environmentGroup": environment_group,
              "kernels": kernels or {"00": "filter_importance", "01": "zsobol", "09": "compensated_mean"}}
    if background is not None:
        extras["background"] = list(background)
    if environment_map is not None:
        extras["environmentMap"] = str(environment_map)
    return {"asset": {"generator": "vulkan_guide", "version": "2.0"}, "nodes": [{"name": "vulkan_guide_scene"}],
            "scene": 0, "scenes": [{"nodes": [0], "extras": extras, "name": "vulkan_guide scene"}], "extensions": {}}


def render(scene, spp, workdir, name, *, size=None, extra=(), out="render.exr", seed=1):
    path = Path(workdir) / f"{name}.gltf"
    write_scene(path, scene)
    out_dir = Path(workdir) / name
    out_dir.mkdir(exist_ok=True)
    result, dropped = run_render(path, spp, size=size, seed=seed, workdir=out_dir, extra_args=list(extra), out_name=out)
    return result, dropped


# ---------------------------------------------------------------- the checks

SIZE = 48
DISTANCE = 4.0
FOV = 40.0


def pixel_grid():
    """The world position of each pixel's center on the plane z = 0, for the camera at (0, 0, DISTANCE)."""
    t = math.tan(math.radians(FOV) / 2) * DISTANCE
    ys, xs = np.mgrid[0:SIZE, 0:SIZE]
    return ((xs + 0.5) / SIZE * 2 - 1) * t, (1 - (ys + 0.5) / SIZE * 2) * t, t * 2 / SIZE


def plane_scene(spp, albedo=(0.8, 0.5, 0.2)):
    # a quad of half-width 1 seen from 4 away at 40 degrees (half-height 1.46): a margin of misses all round
    shapes = [quad("plane", (0, 0, 0), (1, 0, 0), (0, -1, 0), 2.0, 2.0, LAMBERTIAN(albedo))]
    return library_scene(shapes, camera((0, 0, DISTANCE), (0, 0, 0), FOV), SIZE, SIZE, background=(1, 1, 1), samples=spp)


def check_plane(spp, workdir):
    print("plane")
    result, dropped = render(plane_scene(spp), spp, workdir, "plane", size=(SIZE, SIZE),
                             extra=["--aov", "all"])
    channels, attributes = read_exr(result, workdir)
    x, y, pixel = pixel_grid()
    # the pixels well inside the quad and well outside it, away from its edge by the filter's reach
    margin = 3 * pixel
    inside = (np.abs(x) < 1 - margin) & (np.abs(y) < 1 - margin)
    outside = (np.abs(x) > 1 + margin) | (np.abs(y) > 1 + margin)
    albedo = np.array([0.8, 0.5, 0.2])
    for c, name in enumerate("RGB"):
        check(np.abs(channels[f"Albedo.{name}"][inside] - albedo[c]).max() < 1e-5,
              f"Albedo.{name} is the Lambertian color {albedo[c]} on the quad (worst difference {np.abs(channels[f'Albedo.{name}'][inside] - albedo[c]).max():.2g})")
    normal = np.stack([channels["N.X"], channels["N.Y"], channels["N.Z"]], axis=-1)[inside]
    check(np.abs(normal - [0, 0, 1]).max() < 1e-5, f"N is the quad's normal (0, 0, 1) (worst difference {np.abs(normal - [0, 0, 1]).max():.2g})")
    shading = np.stack([channels["Ns.X"], channels["Ns.Y"], channels["Ns.Z"]], axis=-1)[inside]
    check(np.abs(shading - [0, 0, 1]).max() < 1e-5, "Ns is the same")
    check(np.abs(channels["Z"][inside] - DISTANCE).max() < 1e-4, f"Z is the distance along the camera's axis, {DISTANCE} (worst difference {np.abs(channels['Z'][inside] - DISTANCE).max():.2g})")
    check(np.abs(channels["P.Z"][inside]).max() < 1e-5, "P.Z is 0, the plane's")
    # a sample is anywhere in its pixel and weighs by the filter: the mean of a plane's position is the pixel's center
    error = max(np.abs(channels["P.X"][inside] - x[inside]).max(), np.abs(channels["P.Y"][inside] - y[inside]).max()) / pixel
    check(error < 0.12, f"P.X and P.Y are the pixel centers' (worst difference {error:.3f} of a pixel)")
    check(channels["u"][inside].min() >= -1e-4 and channels["u"][inside].max() <= 1 + 1e-4 and channels["v"][inside].min() >= -1e-4 and channels["v"][inside].max() <= 1 + 1e-4, "u and v lie in [0, 1] on the quad")
    # u runs across the quad and v down it, in glTF's orientation: one of each moves with x and with y
    u_slope = np.polyfit(x[inside], channels["u"][inside], 1)[0]
    v_slope = np.polyfit(y[inside], channels["v"][inside], 1)[0]
    check(abs(u_slope) > 0.4 and abs(v_slope) > 0.4, f"u and v vary across the quad as texture coordinates do (slopes {u_slope:.2f} and {v_slope:.2f})")
    check((channels["id.instance"][inside] == 0).all() and (channels["id.material"][inside] == 0).all(), "id.instance and id.material are 0, the only instance and material")
    check((channels["n"] == spp).all(), f"n is {spp} everywhere")
    variance = np.stack([channels[f"Variance.{c}"] for c in "RGB"], axis=-1)[inside]
    check(variance.max() < 1e-9, f"a quad in a uniform light has no variance: every sample is its albedo (largest {variance.max():.2g})")
    # what missed
    for name in ["P.X", "P.Y", "P.Z", "N.X", "N.Y", "N.Z", "Ns.X", "Z", "u", "v", "Albedo.R", "Albedo.G"]:
        check(np.abs(channels[name][outside]).max() == 0.0, f"{name} is 0 where nothing was hit")
    check((channels["id.instance"][outside] == -1).all(), "id.instance is -1 where nothing was hit")
    check(abs(np.mean(channels["R"][outside]) - 1.0) < 1e-5, "and the picture there is the background, 1")
    check(dropped == 0, "no sample was dropped")
    for key in ["renderTimeSeconds", "samplesPerPixel", "cameraFromWorld", "NDCFromWorld", "chromaticities", "vkguide:scene", "vkguide:kernels", "vkguide:settings"]:
        check(key in attributes, f"the header has {key}")
    check(attributes.get("samplesPerPixel") == str(spp), "samplesPerPixel is the render's")


def check_through(spp, workdir):
    print("through specular")
    shapes = [quad("plane", (0, 0, 0), (1, 0, 0), (0, -1, 0), 4.0, 4.0, LAMBERTIAN((0.8, 0.5, 0.2))),
              sphere("glass", (0, 0, 2), 0.5, {"type": "dielectric", "ir": 1.5})]
    scene = library_scene(shapes, camera((0, 0, DISTANCE), (0, 0, 0), FOV), SIZE, SIZE, background=(1, 1, 1), samples=spp)
    c = SIZE // 2
    first, _ = render(scene, spp, workdir, "glass_first", size=(SIZE, SIZE), extra=["--aov", "albedo,position,depth,normal"])
    through, _ = render(scene, spp, workdir, "glass_through", size=(SIZE, SIZE), extra=["--aov", "albedo,position,depth,normal", "--capture-through-specular"])
    a, _ = read_exr(first, workdir)
    b, _ = read_exr(through, workdir)
    window = (slice(c - 1, c + 1), slice(c - 1, c + 1))
    glass_albedo = np.array([a[f"Albedo.{n}"][window].mean() for n in "RGB"])
    check(np.abs(glass_albedo - 1.0).max() < 1e-5, f"the first surface is the glass, whose albedo is 1 (got {glass_albedo})")
    check(abs(a["Z"][window].mean() - 1.5) < 0.02 and abs(a["P.Z"][window].mean() - 2.5) < 0.02, f"at its front: Z {a['Z'][window].mean():.3f} (1.5), P.Z {a['P.Z'][window].mean():.3f} (2.5)")
    behind = np.array([b[f"Albedo.{n}"][window].mean() for n in "RGB"])
    share = behind / np.array([0.8, 0.5, 0.2])
    check(share.min() > 0.8 and share.max() <= 1.0 + 1e-5 and share.max() - share.min() < 0.01,
          f"capturing through specular records the quad behind it: albedo {behind} is the quad's (0.8, 0.5, 0.2) times {share.mean():.3f}, the share of samples that met it (the rest left the scene through the glass)")
    check(abs(b["Z"][window].mean() - DISTANCE) < 0.1 and abs(b["P.Z"][window].mean()) < 0.1, f"at its depth: Z {b['Z'][window].mean():.3f} (4), P.Z {b['P.Z'][window].mean():.3f} (0)")
    far = (slice(2, 6), slice(2, 6))
    check(np.abs(a["Albedo.R"][far] - b["Albedo.R"][far]).max() < 1e-5, "away from the glass the two agree")


def check_groups(spp, workdir):
    print("light groups")
    shapes = [quad("floor", (0, -1, 0), (1, 0, 0), (0, 0, 1), 8.0, 8.0, LAMBERTIAN((0.7, 0.7, 0.7))),
              sphere("ball", (0, 0, 0), 0.8, LAMBERTIAN((0.6, 0.6, 0.6))),
              sphere("red lamp", (-2, 1, 0), 0.4, EMISSIVE((1.0, 0.2, 0.1), 12.0)),
              sphere("blue lamp", (2, 1, 0), 0.4, EMISSIVE((0.1, 0.3, 1.0), 12.0))]
    lights = [{"type": "point", "name": "bulb", "position": [0, 3, 1.5], "color": [1, 1, 0.8], "intensity": 25.0, "lightGroup": 3}]
    scene = library_scene(shapes, camera((0, 1, 6), (0, 0, 0), 40.0), 96, 64, groups={"red lamp": 1, "blue lamp": 2}, lights=lights,
                          background=(0.1, 0.1, 0.15), light_group_names=["Sky", "Red", "Blue", "Bulb"], environment_group=0, samples=spp)
    result, dropped = render(scene, spp, workdir, "groups", size=(96, 64), extra=["--light-groups"])
    channels, attributes = read_exr(result, workdir)
    picture = np.stack([channels[c] for c in "RGB"], axis=-1)
    names = ["Sky", "Red", "Blue", "Bulb"]
    total = np.zeros_like(picture)
    for g, name in enumerate(names):
        image = np.stack([channels[f"LightGroup.{name}.{c}"] for c in "RGB"], axis=-1)
        total += image
        check(image.min() >= 0.0 and image.max() > 0.0, f"group {name} is not empty (mean {image.mean():.4f}, brightest {image.max():.3g}) and not negative")
        check(attributes.get(f"vkguide:lightGroup{g}") == name, f"the header names group {g} {name}")
    relative = np.abs(total - picture) / np.maximum(picture, 1e-3)
    check(np.abs(total - picture).max() / picture.max() < 1e-4, f"the groups sum to the picture within 1e-4 of its peak (worst absolute {np.abs(total - picture).max():.2g} of {picture.max():.3g})")
    check(np.median(relative) < 1e-5 and relative[picture > 0.05].max() < 1e-4, f"and within 1e-4 relative in every pixel above 0.05 (worst {relative[picture > 0.05].max():.2g}, median {np.median(relative):.2g})")
    # each lamp's group is where that lamp is: the red lamp's image is redder than blue, the blue lamp's the reverse
    red = np.stack([channels[f"LightGroup.Red.{c}"] for c in "RGB"], axis=-1)
    blue = np.stack([channels[f"LightGroup.Blue.{c}"] for c in "RGB"], axis=-1)
    check(red[..., 0].sum() > 2 * red[..., 2].sum() and blue[..., 2].sum() > 2 * blue[..., 0].sum(), "the red lamp's group is red and the blue lamp's blue")
    check(dropped == 0, "no sample was dropped")


def check_ao(workdir):
    print("ambient occlusion")
    radius = 2.0
    # a camera at the center of a sphere: every pixel meets the wall at distance R, and a cosine-weighted ray from there
    # travels 2 R cos(theta) before the far wall, so the fraction that goes on past L is 1 - (L / 2R)^2
    shapes = [sphere("room", (0, 0, 0), radius, LAMBERTIAN((0.7, 0.7, 0.7)))]
    scene = library_scene(shapes, camera((0, 0, 0), (0, 0, -1), 60.0), 32, 32, samples=256, background=(1, 1, 1))
    for length in [1.0, 2.0, 3.0, 4.5]:
        expected = max(0.0, 1.0 - (length / (2 * radius)) ** 2)
        result, _ = render(scene, 256, workdir, f"ao_{length}", size=(32, 32), extra=["--debug-view", "ao", "--ao-distance", str(length)], out="ao.pfm")
        mean = result[..., 1].mean()
        check(abs(mean - expected) < 0.012, f"AO with rays of length {length} inside a sphere of radius {radius}: {mean:.4f} (expected {expected:.4f})")
    # the default length is a tenth of the scene's size: the sphere's radius 2 gives 0.2, so almost nothing is occluded
    result, _ = render(scene, 64, workdir, "ao_default", size=(32, 32), extra=["--debug-view", "ao"], out="ao.pfm")
    check(result[..., 1].mean() > 0.98, f"the default length (a tenth of the scene) leaves the open interior unoccluded ({result[..., 1].mean():.3f})")
    # a miss is black
    shapes = [sphere("ball", (0, 0, -5), 0.5, LAMBERTIAN((0.7, 0.7, 0.7)))]
    scene = library_scene(shapes, camera((0, 0, 0), (0, 0, -1), 60.0), 32, 32, samples=16, background=(1, 1, 1))
    result, _ = render(scene, 16, workdir, "ao_miss", size=(32, 32), extra=["--debug-view", "ao", "--ao-distance", "1"], out="ao.pfm")
    check(result[0, 0, 1] == 0.0, "a pixel that hits nothing is black")


def check_files(spp, workdir):
    print("files")
    scene = plane_scene(spp)
    pfm, _ = render(scene, spp, workdir, "files_pfm", size=(SIZE, SIZE), out="render.pfm")
    png = read_png(render(scene, spp, workdir, "files_png", size=(SIZE, SIZE), out="render.png")[0])
    expected = np.round(np.sqrt(np.clip(pfm, 0, 1)) * 255)
    difference = np.abs(png - expected)
    check(difference.max() <= 1 and (difference == 0).mean() > 0.97, f"the PNG is the display image of the same render: sqrt of the film to 8 bits ({(difference == 0).mean() * 100:.1f}% of values exact, largest difference {difference.max():.0f})")
    hdr_path, _ = render(scene, spp, workdir, "files_hdr", size=(SIZE, SIZE), out="render.hdr")
    hdr = read_hdr(hdr_path)
    relative = np.abs(hdr - pfm) / np.maximum(pfm.max(axis=-1, keepdims=True), 1e-6)
    check(relative.max() < 0.005, f"the HDR file is the film to RGBE's precision (worst {relative.max():.4f} of the pixel's brightest channel)")
    full, _ = render(scene, spp, workdir, "files_full", size=(SIZE, SIZE), out="render.exr")
    half, _ = render(scene, spp, workdir, "files_half", size=(SIZE, SIZE), extra=["--half"], out="render.exr")
    f, _ = read_exr(full, workdir)
    h, _ = read_exr(half, workdir)
    ok = True
    for c in "RGB":
        keep = f[c] > 0.01
        ok &= (np.abs(h[c][keep] - f[c][keep]) / f[c][keep]).max() < 1.0 / 1024
    check(ok, "the half-float EXR is the float one to half's precision")
    # a noisy picture with every output is where the size shows: a flat one compresses to a few hundred bytes either way
    shapes = [quad("floor", (0, -1, 0), (1, 0, 0), (0, 0, 1), 8.0, 8.0, LAMBERTIAN((0.7, 0.7, 0.7))),
              sphere("ball", (0, 0, 0), 0.8, LAMBERTIAN((0.6, 0.6, 0.6))),
              sphere("lamp", (-2, 1, 0), 0.4, EMISSIVE((1.0, 0.5, 0.2), 15.0))]
    busy = library_scene(shapes, camera((0, 1, 6), (0, 0, 0), 40.0), 96, 64, background=(0.05, 0.05, 0.08), samples=8)
    big = render(busy, 8, workdir, "files_big", size=(96, 64), extra=["--aov", "all"], out="render.exr")[0]
    small = render(busy, 8, workdir, "files_small", size=(96, 64), extra=["--aov", "all", "--half"], out="render.exr")[0]
    check(os.path.getsize(small) < 0.75 * os.path.getsize(big), f"and smaller on a noisy picture with every output ({os.path.getsize(small)} against {os.path.getsize(big)} bytes)")
    # the picture of an EXR is the film itself
    check(np.array_equal(np.stack([f[c] for c in "RGB"], axis=-1), pfm.astype(np.float32).astype(np.float64)), "the EXR's R, G and B are the PFM's, bit for bit")

    # an OpenEXR environment map renders what the Radiance one it came from does
    maps = Path(workdir) / "maps"
    maps.mkdir(exist_ok=True)
    gradient = np.zeros((16, 32, 3), dtype=np.float32)
    for yy in range(16):
        for xx in range(32):
            gradient[yy, xx] = (0.2 + xx / 32.0, 0.1 + yy / 16.0, 0.5 + 3.0 * (xx % 8 == 0))
    vkscene.write_pfm(maps / "map.pfm", gradient)
    subprocess.run([str(IMAGE_CONVERT), str(maps / "map.pfm"), str(maps / "map.hdr")], check=True, capture_output=True)
    subprocess.run([str(IMAGE_CONVERT), str(maps / "map.hdr"), str(maps / "map.exr")], check=True, capture_output=True)
    renders = []
    for extension in ["hdr", "exr"]:
        shapes = [sphere("ball", (0, 0, 0), 1.0, LAMBERTIAN((0.7, 0.6, 0.5)))]
        scene = library_scene(shapes, camera((0, 0, 4), (0, 0, 0), 40.0), 32, 32, environment_map=maps / f"map.{extension}", samples=32)
        image, _ = render(scene, 32, workdir, f"env_{extension}", size=(32, 32), out="render.pfm")
        renders.append(image)
    check(np.array_equal(renders[0], renders[1]) and renders[0].max() > 0.2, "an EXR environment map renders what the Radiance map it was made from does, bit for bit")


def check_relight(spp, workdir):
    print("relight")
    shapes = [quad("floor", (0, -1, 0), (1, 0, 0), (0, 0, 1), 8.0, 8.0, LAMBERTIAN((0.7, 0.7, 0.7))),
              sphere("ball", (0, 0, 0), 0.8, LAMBERTIAN((0.6, 0.6, 0.6))),
              sphere("lamp", (-2, 1, 0), 0.4, EMISSIVE((1.0, 0.5, 0.2), 15.0))]
    lights = [{"type": "point", "name": "bulb", "position": [0, 3, 1.5], "color": [1, 1, 1], "intensity": 25.0, "lightGroup": 1}]
    scene = library_scene(shapes, camera((0, 1, 6), (0, 0, 0), 40.0), 64, 48, groups={"lamp": 0}, lights=lights,
                          background=(0.05, 0.05, 0.08), light_group_names=["Lamp", "Bulb"], samples=spp)
    exr, _ = render(scene, spp, workdir, "relight_exr", size=(64, 48), extra=["--light-groups"])
    channels, _ = read_exr(exr, workdir)
    picture = np.stack([channels[c] for c in "RGB"], axis=-1)
    bulb = np.stack([channels[f"LightGroup.Bulb.{c}"] for c in "RGB"], axis=-1)

    def png_of(name, scales):
        path, _ = render(scene, spp, workdir, name, size=(64, 48), extra=["--light-groups", "--relight", scales], out="render.png")
        return read_png(path)

    def display(linear):
        return np.round(np.sqrt(np.clip(linear, 0, 1)) * 255)

    flat = png_of("relight_one", "1,1")
    check(np.abs(flat - display(picture)).max() <= 2, f"with every group at scale 1 the relit picture is the picture (largest difference {np.abs(flat - display(picture)).max():.0f} of 255)")
    dimmed = png_of("relight_dim", "1,0")
    expected = display(picture - bulb)
    check(np.abs(dimmed - expected).max() <= 2, f"with the bulb's group at scale 0 it is the picture without the bulb (largest difference {np.abs(dimmed - expected).max():.0f} of 255)")
    boosted = png_of("relight_boost", "1,2")
    expected = display(picture + bulb)
    check(np.abs(boosted - expected).max() <= 2, f"with it at scale 2 it is the picture with the bulb twice (largest difference {np.abs(boosted - expected).max():.0f} of 255)")
    check(np.abs(dimmed.astype(float) - flat).max() > 20, "and the three are different pictures")


def check_baseline(baseline, spp, workdir):
    print("bit-identical when no output is kept")
    baseline = Path(baseline)
    scenes = ["cornell_reference.gltf", "all_materials.gltf", "caustic_glass.gltf", "sphere_scene.gltf"]
    for scene in scenes:
        old = Path(workdir) / f"base_{scene}.pfm"
        new = Path(workdir) / f"new_{scene}.pfm"
        for path in (old, new):
            if path.exists():
                path.unlink()
        env = dict(os.environ, VKGUIDE_WINDOW="background", VKGUIDE_VERIFY=f"{scene}:{spp}:{old}", VKGUIDE_VERIFY_SIZE="120x90")
        subprocess.run([str(baseline / "bin" / "vulkan_guide")], cwd=baseline / "bin", env=env, capture_output=True, timeout=900)
        env = dict(os.environ, VKGUIDE_WINDOW="background")
        subprocess.run([str(BIN / "vulkan_guide"), "--render", scene, "--spp", str(spp), "--size", "120x90", "--out", str(new)], cwd=BIN, env=env, capture_output=True, timeout=900)
        check(old.exists() and new.exists() and old.read_bytes() == new.read_bytes(), f"{scene}: {spp} samples at 120 x 90 are byte for byte what the build before made")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spp", type=int, default=64)
    parser.add_argument("--baseline")
    parser.add_argument("--only")
    args = parser.parse_args()
    workdir = tempfile.mkdtemp(prefix="vkaov_")

    sections = {
        "plane": lambda: check_plane(args.spp, workdir),
        "through": lambda: check_through(args.spp, workdir),
        "groups": lambda: check_groups(max(args.spp, 128), workdir),
        "ao": lambda: check_ao(workdir),
        "files": lambda: check_files(min(args.spp, 32), workdir),
        "relight": lambda: check_relight(max(args.spp, 64), workdir),
    }
    if args.baseline:
        sections["baseline"] = lambda: check_baseline(args.baseline, 32, workdir)
    for name, section in sections.items():
        if args.only is None or args.only == name:
            section()
    print("all checks passed" if not failures else f"{len(failures)} check(s) failed")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
