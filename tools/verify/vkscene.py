"""Shared helpers for the verification scripts: build scene files, run the renderer headless on
them (the batch mode, `vulkan_guide --render`, src/command_line.h), and read what it writes.

A scene is the version-7 payload (inline materials), which the current build still reads; the
scripts never depend on the material library.
"""

import json
import math
import os
import subprocess
import struct
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "bin"


# ---------------------------------------------------------------- vectors and quaternions

def normalize(v):
    v = np.asarray(v, dtype=np.float64)
    return v / np.linalg.norm(v)


def quat_from_matrix(m):
    """The unit quaternion (x, y, z, w) of a rotation matrix whose columns are the rotated axes."""
    m = np.asarray(m, dtype=np.float64)
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    return list(q / np.linalg.norm(q))


def quat_rotate(q, v):
    x, y, z, w = q
    u = np.array([x, y, z])
    v = np.asarray(v, dtype=np.float64)
    return v + 2 * np.cross(u, np.cross(u, v) + w * v)


# ---------------------------------------------------------------- scene building

LAMBERTIAN = lambda albedo: {"type": "lambertian", "albedo": list(albedo)}
EMISSIVE = lambda color, strength: {"type": "emissive", "color": list(color), "strength": strength}


def quad(name, center, u_axis, v_axis, width, length, material):
    """A quad centered at `center`, `width` long along u_axis and `length` along v_axis (the
    unit quad's x and z). Its normal is v x u, which is a matter of taste: shapes are two-sided."""
    u = normalize(u_axis)
    v = normalize(v_axis)
    n = np.cross(v, u)
    orientation = quat_from_matrix(np.column_stack([u, n, v]))
    return {"type": "quad", "name": name, "position": [float(c) for c in center], "orientation": orientation,
            "width": width, "length": length, "material": material}


def box(name, center, size, yaw_degrees, material):
    half = math.radians(yaw_degrees) / 2
    return {"type": "box", "name": name, "position": [float(c) for c in center],
            "orientation": [0.0, math.sin(half), 0.0, math.cos(half)],
            "width": size[0], "height": size[1], "depth": size[2], "material": material}


def sphere(name, center, radius, material):
    return {"type": "sphere", "name": name, "position": [float(c) for c in center], "radius": radius, "material": material}


def camera(position, look_at, fov=40.0):
    """A camera at `position` looking at `look_at` with +y up (the identity orientation looks down -z)."""
    forward = normalize(np.asarray(look_at, dtype=np.float64) - np.asarray(position, dtype=np.float64))
    right = normalize(np.cross(forward, [0.0, 1.0, 0.0]))
    up = np.cross(right, forward)
    # columns: right, up, back (the camera looks down its -z)
    orientation = quat_from_matrix(np.column_stack([right, up, -forward]))
    return {"name": "render_camera", "position": [float(c) for c in position], "orientation": orientation,
            "fov": fov, "aperture": 0.0, "focusDistance": 10.0, "exposure": 1.0}


def make_scene(shapes, cam, width, height, *, background=None, environment_map=None, environment_intensity=1.0,
               samples=64, ray_depth=16, kernels=None, traversal=None, render_extra=None, samples_per_frame=32):
    render = {"width": width, "height": height, "matchViewport": False, "antialiasing": True, "maxSamples": samples,
              "unlimitedSamples": False, "rayDepth": ray_depth, "useFixedSeed": True, "seed": 1,
              "samplesPerFrame": samples_per_frame, "russianRoulette": False, "minBouncesBeforeRoulette": 4, "restartOnChange": False}
    render.update(render_extra or {})
    extras = {"version": 7, "models": [], "environmentIntensity": environment_intensity, "render": render,
              "renderCamera": 0, "cameras": [cam], "geometry": shapes, "meshObjects": []}
    if background is not None:
        extras["background"] = list(background)
    if environment_map is not None:
        extras["environmentMap"] = str(environment_map)
    if kernels is not None:
        extras["kernels"] = kernels
    if traversal is not None:
        extras["traversal"] = traversal
    return {"asset": {"generator": "vulkan_guide", "version": "2.0"}, "nodes": [{"name": "vulkan_guide_scene"}],
            "scene": 0, "scenes": [{"nodes": [0], "extras": extras, "name": "vulkan_guide scene"}], "extensions": {}}


def write_scene(path, scene):
    Path(path).write_text(json.dumps(scene, indent=None))


# ---------------------------------------------------------------- images

def read_pfm(path):
    """A PFM as a float array (height, width, 3), row 0 at the top."""
    with open(path, "rb") as f:
        magic = f.readline().strip()
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), dtype="<f4" if scale < 0 else ">f4")
    channels = 3 if magic == b"PF" else 1
    image = data.reshape(h, w, channels)[::-1]
    return np.ascontiguousarray(image.astype(np.float64))


def write_pfm(path, image):
    image = np.asarray(image, dtype="<f4")
    h, w, _ = image.shape
    with open(path, "wb") as f:
        f.write(b"PF\n%d %d\n-1.0\n" % (w, h))
        f.write(np.ascontiguousarray(image[::-1]).tobytes())


# ---------------------------------------------------------------- running the renderer

def run_render(scene, samples, *, size=None, seed=None, workdir=None, timeout=900, keep_scene=None, extra_args=(), out_name="render.pfm", log=None):
    """Renders `scene` (a dict, or a path) to `samples` per pixel and returns the linear image and the count of
    samples dropped. `extra_args` are more batch-mode options (--aov, --light-groups, ...), and `out_name` picks the
    format: render.pfm by default, or render.exr to read the channels through read_exr(). A list passed as `log`
    receives the renderer's "render:" lines."""
    workdir = Path(workdir or tempfile.mkdtemp(prefix="vkverify_"))
    workdir.mkdir(parents=True, exist_ok=True)
    if isinstance(scene, dict):
        scene_path = Path(keep_scene) if keep_scene else workdir / "scene.gltf"
        write_scene(scene_path, scene)
    else:
        scene_path = Path(scene)
    out = workdir / out_name
    if out.exists():
        out.unlink()
    env = dict(os.environ)
    env["VKGUIDE_WINDOW"] = "background"
    command = [str(BIN / "vulkan_guide"), "--render", str(scene_path.resolve()), "--spp", str(samples), "--out", str(out), "--timeout", str(timeout)]
    if size:
        command += ["--size", f"{size[0]}x{size[1]}"]
    if seed is not None:
        command += ["--seed", str(seed)]
    command += list(extra_args)
    result = subprocess.run(command, cwd=BIN, env=env, capture_output=True, text=True, timeout=timeout + 60)
    output = result.stdout + result.stderr
    if log is not None:
        log.extend(line for line in output.splitlines() if line.startswith("render:"))
    if result.returncode != 0 or not out.exists():
        raise RuntimeError(f"render failed (exit {result.returncode}):\n" + "\n".join(l for l in output.splitlines() if "render:" in l or "rror" in l)[-2000:])
    dropped = 0
    for line in output.splitlines():
        if "render:" in line and " samples, " in line and " dropped" in line:
            dropped = int(line.split(" samples, ")[1].split(" dropped")[0])
    if out.suffix == ".pfm":
        return read_pfm(out), dropped
    return out, dropped


def save_png(path, image, exposure=1.0):
    """A quick look at a linear image: sRGB-encoded 8 bit, through a PPM and sips (macOS)."""
    srgb = np.clip(image * exposure, 0, 1)
    srgb = np.where(srgb <= 0.0031308, 12.92 * srgb, 1.055 * np.power(srgb, 1 / 2.4) - 0.055)
    data = (np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8)
    ppm = Path(path).with_suffix(".ppm")
    with open(ppm, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (data.shape[1], data.shape[0]))
        f.write(data.tobytes())
    subprocess.run(["sips", "-s", "format", "png", str(ppm), "--out", str(path)], capture_output=True)
    ppm.unlink()
