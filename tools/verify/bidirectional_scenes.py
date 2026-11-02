"""Writes the verification scenes of docs/plans/completed/pbrt-bidirectional-path-tracing.md into assets/scenes/
(committed, like the camera scenes). Each names the integrator it is meant for; tools/verify/bidirectional_check.py
renders every one with the other integrators too, by changing the integrator in its copy.

  assets/scenes/bd_cornell.gltf      the Cornell box of cornell.py at 160 x 160, rendered by bdpt
  assets/scenes/bd_veach.gltf        Veach's MIS scene (veach_mis.gltf) at 192 x 128, rendered by lvc
  assets/scenes/bd_caustic.gltf      caustic_scene.py's glass sphere on a diffuse floor under a small bright sphere light,
                                     at 128 x 128, rendered by bdpt: the floor's caustic is light the path tracer finds
                                     only by chance, and light tracing, lvc and bdpt connect to the camera
  assets/scenes/bd_strategies.gltf   the Cornell box at depth 5, rendered by bdpt with its strategy images, weighted
                                     (pbrt's visualizeweights), which an EXR export writes as bdpt_dDD_sSS_tTT

Run:  python3 tools/verify/bidirectional_scenes.py
"""

import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cornell  # noqa: E402
import vkscene  # noqa: E402
from caustic_scene import caustic_scene  # noqa: E402

SCENES = vkscene.ROOT / "assets" / "scenes"


def with_integrator(scene, integrator, width=None, height=None, samples=None, ray_depth=None, bidirectional=None):
    """A copy of `scene` rendered by `integrator` (an IntegratorInfo key: path, light, lvc, bdpt)."""
    scene = copy.deepcopy(scene)
    extras = scene["scenes"][0]["extras"]
    extras["integrator"] = integrator
    render = extras["render"]
    if width is not None:
        render["width"] = width
        render["height"] = height
    if samples is not None:
        render["maxSamples"] = samples
    if ray_depth is not None:
        render["rayDepth"] = ray_depth
    if bidirectional is not None:
        render["bidirectional"] = bidirectional
    return scene


def scenes():
    veach = json.loads((SCENES / "veach_mis.gltf").read_text())
    return {
        "bd_cornell": with_integrator(cornell.scene(True, 160, 160, samples=256), "bdpt"),
        "bd_veach": with_integrator(veach, "lvc", 192, 128, samples=256),
        "bd_caustic": with_integrator(caustic_scene(128, 128, samples=256), "bdpt"),
        "bd_strategies": with_integrator(cornell.scene(True, 160, 160, samples=256), "bdpt", ray_depth=6,
                                         bidirectional={"connections": 1, "cacheBudget": 256, "visualizeStrategies": True, "visualizeWeights": True}),
    }


def main():
    SCENES.mkdir(parents=True, exist_ok=True)
    for name, scene in scenes().items():
        vkscene.write_scene(SCENES / f"{name}.gltf", scene)
        print(f"wrote assets/scenes/{name}.gltf")


if __name__ == "__main__":
    main()
