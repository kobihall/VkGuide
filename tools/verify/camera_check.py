"""Renders the verification scenes of docs/plans/completed/pbrt-cameras.md (tools/verify/camera_scenes.py writes them) and
checks what the plan's Verification section asks:

  orthographic  the open box seen straight in: its side walls, parallel to the rays, show edge-on (no red or green),
                and the back wall is as wide on the film as the view volume makes it; a perspective camera sees the walls
  concentric    the concentric lens mapping converges to the polar one's image: they differ by no more than two
                renders of either with different seeds
  bokeh         the out-of-focus emitter takes the aperture's shape (a hexagon is 2 / sqrt(3) wider across its corners
                than across its flats, a circle is round), and the shape never changes the energy on the film
  tilt          the plane of focus tilted onto the ground keeps the near and the far ground as sharp as a pinhole's,
                where the same lens untilted blurs at least one of them
  equirect      an equirectangular render of the room from P, loaded as the environment map, lights a mirror sphere at P
                as the room itself does

Run:  python3 tools/verify/camera_check.py [--quick]
"""

import argparse
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402

SCENES = vkscene.TEST_SCENES / "cameras"
failures = []


def check(ok, message):
    print(("  ok   " if ok else "  FAIL ") + message)
    if not ok:
        failures.append(message)


def edited(name, edit):
    """The scene's payload after `edit` changes its extras, its relative paths made absolute"""
    scene = json.loads((SCENES / name).read_text())
    extras = scene["scenes"][0]["extras"]
    for texture in extras.get("textures", []):
        texture["path"] = str((SCENES / texture["path"]).resolve())
    for camera in extras.get("cameras", []):
        if camera.get("apertureImage"):
            camera["apertureImage"] = str((SCENES / camera["apertureImage"]).resolve())
    if extras.get("environmentMap"):
        extras["environmentMap"] = str((SCENES / extras["environmentMap"]).resolve())
    if edit is not None:
        edit(extras)
    return scene


def render(name, samples, *, seed=1, edit=None, out_name="render.pfm"):
    image, _ = vkscene.run_render(edited(name, edit), samples, seed=seed, out_name=out_name)
    return image


def camera_edit(**fields):
    def edit(extras):
        extras["cameras"][0].update(fields)
    return edit


def luminance(image):
    return image @ np.array([0.2126, 0.7152, 0.0722])


def box_blur(image, radius):
    kernel = 2 * radius + 1
    padded = np.pad(image, ((radius, radius), (radius, radius)) + ((0, 0),) * (image.ndim - 2), mode="edge")
    out = np.zeros_like(image)
    for dy in range(kernel):
        for dx in range(kernel):
            out += padded[dy:dy + image.shape[0], dx:dx + image.shape[1]]
    return out / (kernel * kernel)


def orthographic(samples):
    print("orthographic: an open box seen straight in")
    ortho = render("cam_ortho_box.gltf", samples)
    perspective = render("cam_ortho_box.gltf", samples, edit=camera_edit(kind="perspective"))

    def colored(image):
        r, g, b = image[..., 0], image[..., 1], image[..., 2]
        return float((((r > 2.5 * g) & (r > 2.5 * b)) | ((g > 2.5 * r) & (g > 2.5 * b))).mean())
    check(colored(ortho) < 0.002, f"orthographic: {colored(ortho) * 100:.3f}% of the pixels show a side wall")
    check(colored(perspective) > 0.05, f"perspective: {colored(perspective) * 100:.1f}% of the pixels show a side wall")
    # the 2 m back wall in a view volume 3 m tall and 4 m wide covers half the film's width
    row = luminance(ortho)[ortho.shape[0] // 2]
    lit = int((row > 0.02 * row.max()).sum())
    expected = ortho.shape[1] * 2.0 / 4.0
    check(abs(lit - expected) <= 2, f"the back wall is {lit} pixels wide, {expected:.0f} expected")


def concentric(samples):
    print("concentric: the concentric lens mapping against the polar one")
    a = render("cam_dof_spheres.gltf", samples, seed=1)
    b = render("cam_dof_spheres.gltf", samples, seed=2)
    polar = render("cam_dof_spheres.gltf", samples, seed=3, edit=camera_edit(lensMapping="polar"))
    noise = np.sqrt(np.mean((a - b) ** 2))
    difference = np.sqrt(np.mean((polar - 0.5 * (a + b)) ** 2))
    # the mean of two renders against a third: sqrt(3 / 2) of one render's noise, against sqrt(2) of it between two
    ratio = difference / noise
    check(ratio < 1.0, f"rmse polar against concentric {difference:.4g}, between two concentric seeds {noise:.4g} (ratio {ratio:.3f}, 0.87 for noise alone)")
    means = abs(polar.mean() / (0.5 * (a + b)).mean() - 1)
    check(means < 2e-3, f"means within {means * 100:.3f}%")


def bokeh(samples):
    print("bokeh: an out-of-focus emitter through a circle, a hexagon and a star")
    shapes = {}
    for shape in ("circle", "polygon", "image"):
        shapes[shape] = luminance(render("cam_bokeh.gltf", samples, edit=camera_edit(apertureShape=shape)))
    energies = {shape: image.sum() for shape, image in shapes.items()}
    # the energy's noise: about 1% at 1024 samples, measured over seeds
    tolerance = 0.02 * np.sqrt(1024 / samples)
    for shape in ("polygon", "image"):
        ratio = energies[shape] / energies["circle"]
        check(abs(ratio - 1) < tolerance, f"{shape}: {ratio:.4f} of the circle's energy on the film (limit {tolerance:.3f})")

    def extents(image):
        smooth = box_blur(image, 2)
        mask = smooth > 0.5 * np.percentile(smooth[smooth > 0], 90)
        ys, xs = np.nonzero(mask)
        return (xs.max() - xs.min() + 1), (ys.max() - ys.min() + 1)
    width, height = extents(shapes["polygon"])
    # the emitter's own size and the threshold shave the corners: the ratio lands between 1.09 and 2 / sqrt(3)
    check(1.09 < width / height < 1.2, f"the hexagon is {width} x {height} pixels, {width / height:.3f} wide per unit height against 1.155")
    width, height = extents(shapes["circle"])
    check(abs(width / height - 1) < 0.04, f"the circle is {width} x {height} pixels")


def tilt(samples):
    print("tilt: the plane of focus laid onto the ground")
    tilted = luminance(render("cam_tilt_ground.gltf", samples))
    untilted = luminance(render("cam_tilt_ground.gltf", samples, edit=camera_edit(tilt=[0.0, 0.0])))
    pinhole = luminance(render("cam_tilt_ground.gltf", samples, edit=camera_edit(aperture=0.0)))
    height = tilted.shape[0]

    def sharpness(image, top, bottom):
        band = image[int(top * height):int(bottom * height)]
        return float(np.mean(np.abs(np.diff(band, axis=1))) + np.mean(np.abs(np.diff(band, axis=0))))
    # rows of the film from the top: the far ground just below the horizon, the near ground at the bottom
    blurred = 1e9
    for label, top, bottom in (("far", 0.22, 0.32), ("near", 0.8, 1.0)):
        reference = sharpness(pinhole, top, bottom)
        t = sharpness(tilted, top, bottom) / reference
        u = sharpness(untilted, top, bottom) / reference
        check(t > 0.8, f"{label} ground: tilted keeps {t:.2f} of a pinhole's sharpness (untilted {u:.2f})")
        blurred = min(blurred, u)
    check(blurred < 0.5, f"the untilted lens blurs the ground to {blurred:.2f} of a pinhole's sharpness where it is least sharp")


def equirect(samples):
    print("equirect: the room from P as the environment map, in a mirror sphere at P")
    with tempfile.TemporaryDirectory() as directory:
        path, _ = vkscene.run_render(edited("cam_equirect_room.gltf", None), samples, workdir=directory, out_name="room.exr")
        target = SCENES / "cam_room_equirect.exr"
        target.write_bytes(Path(path).read_bytes())
    room = render("cam_mirror_room.gltf", samples * 2)
    environment = render("cam_mirror_env.gltf", 64)
    # the sphere's disc on the film, inside 90% of its radius, where neither parallax nor the rim's filtering matters
    size = room.shape[0]
    y, x = np.mgrid[0:size, 0:size]
    center = (size - 1) / 2
    disc = np.hypot(x - center, y - center) < 0.9 * size / 2 / 1.15
    mean_ratio = environment[disc].mean() / room[disc].mean()
    check(abs(mean_ratio - 1) < 0.02, f"the sphere's mean through the environment map is {mean_ratio:.4f} of the room's")
    # pixel by pixel through x / (1 + x): the ceiling light's reflection is 25 times as bright as the walls', and a
    # pixel's shift of its edge, from the map's texels, would outweigh every wall in a linear measure
    def tonemapped(image):
        blurred = box_blur(image, 1)
        return blurred / (1 + blurred)
    difference = np.abs(tonemapped(environment) - tonemapped(room))[disc]
    worst = np.percentile(difference, 99)
    mean_difference = difference.mean()
    check(mean_difference < 0.01 and worst < 0.06, f"pixel by pixel (tonemapped, 3 x 3 box) they differ by {mean_difference:.4f} on average and {worst:.3f} at the 99th percentile")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true", help="fewer samples")
    args = parser.parse_args()
    scale = 0.25 if args.quick else 1.0
    orthographic(int(64 * scale))
    concentric(int(512 * scale))
    bokeh(int(1024 * scale))
    tilt(int(256 * scale))
    equirect(int(512 * scale))
    if failures:
        print(f"camera_check: {len(failures)} check(s) FAILED")
        return 1
    print("camera_check: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
