"""The beam axes' scene file round trip (docs/plans/physoptics-beam-axis.md), through the renderer's editor: each scene is
opened by the batch mode and saved back with --save-scene, which turns the file's indices into the editor's ids and back.

  round trip      every scene under assets/scenes/tests/ with beam axes saves the same axes (each node's optic, parent,
                  port and review flag, the source, the ghost threshold), naming the same optics; a second round trip
                  writes the same file again
  guide axis      the axis renders trace along and the trace mode (render settings) survive a round trip, and follow
                  their axis when the axes before it go
  repairs         a file with a visit of a missing optic, a port its parent's kind lacks, two visits on one place, a
                  cycle and a source that is no laser opens with each repaired as Remove would, and a warning for each
  shifted optics  a malformed optic the load skips: the axes still name the optics they named
  older file      a version 31 scene opens and saves without beam axes, as version 33

  python3 tools/verify/beam_axis_check.py
"""

import copy
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402

failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)


def extras(scene):
    return scene["scenes"][0]["extras"]


def resave(scene, workdir, name):
    """the scene (a dict or a path) opened and saved back by the renderer: the saved scene and the "render:" lines"""
    workdir.mkdir(parents=True, exist_ok=True)
    if isinstance(scene, dict):
        path = workdir / f"{name}.in.gltf"
        vkscene.write_scene(path, scene)
    else:
        path = Path(scene)
    out = workdir / f"{name}.out.gltf"
    if out.exists():
        out.unlink()
    env = dict(os.environ, VKGUIDE_WINDOW="background")
    result = subprocess.run([str(vkscene.BIN / "vulkan_guide"), "--render", str(path.resolve()), "--save-scene", str(out)],
                            cwd=vkscene.BIN, env=env, capture_output=True, text=True, timeout=300)
    lines = [line for line in (result.stdout + result.stderr).splitlines() if line.startswith("render:")]
    if result.returncode != 0 or not out.exists():
        raise RuntimeError(f"--save-scene failed (exit {result.returncode}):\n" + "\n".join(lines))
    return json.loads(out.read_text()), lines


def named_axes(scene):
    """the axes with every optic and camera named rather than indexed, as a round trip must keep them"""
    e = extras(scene)
    optics = [o["name"] for o in e.get("optics", [])]
    cameras = [c.get("name") for c in e.get("cameras", [])]
    axes = []
    for axis in e.get("beamAxes", []):
        named = copy.deepcopy(axis)
        # before version 34 every axis saved a threshold, and one at the default now follows the scene's
        if e["version"] < 34 and abs(named.get("ghostThreshold", 0.0) - 1e-4) < 1e-9:
            del named["ghostThreshold"]
        source = named.get("source", {})
        if "laser" in source:
            source["laser"] = optics[source["laser"]]
        if "camera" in source:
            source["camera"] = cameras[source["camera"]]
        for node in named["nodes"]:
            node["optic"] = optics[node["optic"]]
            node.setdefault("review", False)
        axes.append(named)
    return axes


def round_trips(workdir):
    print("round trip: every test scene with beam axes")
    scenes = sorted(p for p in vkscene.TEST_SCENES.rglob("*.gltf") if "beamAxes" in extras(json.loads(p.read_text())))
    check(len(scenes) >= 7, f"{len(scenes)} test scenes have beam axes")
    for path in scenes:
        original = json.loads(path.read_text())
        name = path.stem
        saved, lines = resave(path, workdir, name)
        warned = [line for line in lines if "beam axis" in line]
        check(not warned, f"{name}: opens without a beam axis warning" + (f" ({warned[0]})" if warned else ""))
        check(named_axes(saved) == named_axes(original), f"{name}: the axes save as they were")
        check(extras(saved).get("guideAxis") == extras(original).get("guideAxis"), f"{name}: the guide axis saves as it was")
        again, _ = resave(workdir / f"{name}.out.gltf", workdir, f"{name}.again")
        check(extras(again).get("beamAxes") == extras(saved).get("beamAxes"), f"{name}: a second round trip saves the same axes")


def repairs(workdir):
    print("repairs: a file whose axes name what is not there")
    source = json.loads((vkscene.TEST_SCENES / "optics_verification" / "beam_axis_setup.gltf").read_text())
    names = [o["name"] for o in extras(source)["optics"]]
    index = {n: i for i, n in enumerate(names)}
    scene = copy.deepcopy(source)
    # the setup's chain M1 > M2 > I1 > L1 > BS1 > {PM_R, PM_T}, with five faults planted after it
    nodes = [{"optic": index["M1"]},
             {"optic": index["M2"], "parent": 0, "port": "Reflected"},
             {"optic": 999, "parent": 1, "port": "Reflected"},  # 2: a missing optic, between M2 and the iris
             {"optic": index["I1"], "parent": 2, "port": "Reflected"},
             {"optic": index["L1"], "parent": 3, "port": "Transmitted"},
             {"optic": index["BS1"], "parent": 4, "port": "Transmitted"},
             {"optic": index["PM_R"], "parent": 5, "port": "Reflected"},
             {"optic": index["PM_T"], "parent": 5, "port": "Nowhere"},  # 7: a port a splitter does not have
             {"optic": index["PM_T"], "parent": 5, "port": "Reflected"},  # 8: a place node 6 holds
             {"optic": index["M1"], "parent": 10, "port": "Reflected"},  # 9 and 10: a cycle
             {"optic": index["M2"], "parent": 9, "port": "Reflected"}]
    extras(scene)["beamAxes"] = [{"name": "faulty", "source": {"laser": index["M1"]}, "ghostThreshold": 1e-3, "nodes": nodes}]
    saved, lines = resave(scene, workdir, "repairs")
    warnings = [line for line in lines if "beam axis" in line]
    for what, label in (("is not a laser", "the source is no laser"), ("is gone", "a node's optic is gone"), ("does not have", "a node's port is not its parent's"),
                        ("where another node does", "two nodes share a place"), ("cycle", "nodes form a cycle")):
        check(any(what in w for w in warnings), f"a warning that {label}")
    axis = named_axes(saved)[0]
    check("source" not in axis, "a source that is no laser is dropped")
    check(abs(axis["ghostThreshold"] - 1e-3) < 1e-9, "the ghost threshold survives")
    by_optic = [(n["optic"], n.get("parent"), n.get("port"), n["review"]) for n in axis["nodes"]]
    # the missing optic's node goes, the iris moving up onto M2's port and marked; the bad port and the taken place
    # dangle, marked; the cycle dangles
    expected = [("M1", None, None, False), ("M2", 0, "Reflected", False), ("I1", 1, "Reflected", True), ("L1", 2, "Transmitted", False),
                ("BS1", 3, "Transmitted", False), ("PM_R", 4, "Reflected", False), ("PM_T", "dangling", None, True), ("PM_T", "dangling", None, True)]
    check(by_optic[:8] == expected, "the tree is repaired as Remove and Detach would: " + str(by_optic[:8]))
    cycle = by_optic[8:]
    check(len(cycle) == 2 and any(n[1] == "dangling" for n in cycle), f"the cycle is cut loose: {cycle}")


def guide_axis(workdir):
    print("guide axis: the axis renders trace along, and how")
    scene = json.loads((vkscene.TEST_SCENES / "optics_verification" / "beam_axis_setup.gltf").read_text())
    e = extras(scene)
    first = copy.deepcopy(e["beamAxes"][0])
    first["name"] = "first"
    e["beamAxes"] = [first, dict(copy.deepcopy(first), name="second")]
    e["guideAxis"] = 1
    e["render"].setdefault("optics", {}).update({"traceMode": "guided", "guideStrength": 0.5})
    saved, _ = resave(scene, workdir, "guide")
    s = extras(saved)
    check(s.get("guideAxis") == 1 and s["beamAxes"][1]["name"] == "second", "the guide axis saves as the axis it was")
    optics = s["render"].get("optics", {})
    check(optics.get("traceMode") == "guided" and abs(optics.get("guideStrength", 0.0) - 0.5) < 1e-6, f"the trace mode and its strength save: {optics}")
    # the first axis names an optic the file no longer has, and the guide stays on the second
    e["guideAxis"] = 7
    saved, _ = resave(scene, workdir, "guide_missing")
    check("guideAxis" not in extras(saved), "a guide axis the file does not have is none")
    e["guideAxis"] = 1
    e["render"]["optics"].pop("traceMode")
    saved, _ = resave(scene, workdir, "guide_full")
    check("traceMode" not in extras(saved)["render"].get("optics", {}), "a full trace mode is not written")


def shifted_optics(workdir):
    print("shifted optics: a malformed optic the load skips")
    scene = json.loads((vkscene.TEST_SCENES / "optics_verification" / "beam_axis_setup.gltf").read_text())
    before = named_axes(scene)
    e = extras(scene)
    e["optics"].insert(0, {"type": "no_such_kind", "name": "ghost"})
    for axis in e["beamAxes"]:
        axis["source"]["laser"] += 1
        for node in axis["nodes"]:
            node["optic"] += 1
    saved, _ = resave(scene, workdir, "shifted")
    check(named_axes(saved) == before, "the axes name the optics they named, the skipped one aside")


def solved_threshold(scene, workdir, name):
    """the threshold the batch mode's beam solver traced the scene's first tree to"""
    path = workdir / f"{name}.in.gltf"
    vkscene.write_scene(path, scene)
    out = workdir / f"{name}.beams.json"
    env = dict(os.environ, VKGUIDE_WINDOW="background")
    result = subprocess.run([str(vkscene.BIN / "vulkan_guide"), "--render", str(path.resolve()), "--solve-beams", str(out)],
                            cwd=vkscene.BIN, env=env, capture_output=True, text=True, timeout=300)
    if result.returncode != 0 or not out.exists():
        raise RuntimeError(f"--solve-beams failed (exit {result.returncode})")
    return json.loads(out.read_text())["trees"][0]["threshold"]


def ghost_thresholds(workdir):
    print("ghost thresholds: the scene's, and an axis's own")
    scene = json.loads((vkscene.TEST_SCENES / "optics_verification" / "beam_axis_setup.gltf").read_text())
    e = extras(scene)
    axis = e["beamAxes"][0]

    e["version"] = 33
    axis["ghostThreshold"] = 1e-4
    saved, _ = resave(scene, workdir, "threshold_v33_default")
    check("ghostThreshold" not in extras(saved)["beamAxes"][0], "version 33: an axis at the default follows the scene's")
    axis["ghostThreshold"] = 1e-3
    saved, _ = resave(scene, workdir, "threshold_v33_own")
    check(abs(extras(saved)["beamAxes"][0].get("ghostThreshold", 0.0) - 1e-3) < 1e-9, "version 33: any other value is the axis's own")

    e["version"] = 34
    axis["ghostThreshold"] = 1e-4
    saved, _ = resave(scene, workdir, "threshold_v34_own")
    check(abs(extras(saved)["beamAxes"][0].get("ghostThreshold", 0.0) - 1e-4) < 1e-9, "version 34: an axis's own 1e-4 stays its own")
    del axis["ghostThreshold"]
    e["viewport"]["beamPreview"] = {"ghostThreshold": 1e-7}
    saved, _ = resave(scene, workdir, "threshold_v34_follow")
    check("ghostThreshold" not in extras(saved)["beamAxes"][0], "version 34: no threshold stays none")
    check(abs(extras(saved)["viewport"].get("beamPreview", {}).get("ghostThreshold", 0.0) - 1e-7) < 1e-12, "the scene's threshold survives")

    traced = solved_threshold(scene, workdir, "threshold_solve_follow")
    check(abs(traced - 1e-7) < 1e-12, f"the solver traces a following axis to the scene's: {traced:.3g}")
    axis["ghostThreshold"] = 1e-3
    traced = solved_threshold(scene, workdir, "threshold_solve_own")
    check(abs(traced - 1e-3) < 1e-9, f"and an axis with its own to that: {traced:.3g}")


def older_file(workdir):
    print("older file: version 31")
    scene = json.loads((vkscene.TEST_SCENES / "optics_verification" / "beam_axis_setup.gltf").read_text())
    e = extras(scene)
    e["version"] = 31
    del e["beamAxes"]
    saved, lines = resave(scene, workdir, "older")
    check("beamAxes" not in extras(saved), "an older file saves with no beam axes")
    # the version this build writes: 34 since a beam axis's ghost threshold became optional
    check(extras(saved)["version"] == 34, "and saves as version 34")


def main():
    workdir = Path(tempfile.mkdtemp(prefix="beam_axis_check_"))
    round_trips(workdir)
    repairs(workdir)
    guide_axis(workdir)
    shifted_optics(workdir)
    ghost_thresholds(workdir)
    older_file(workdir)
    print()
    print("PASS" if not failures else f"FAIL ({len(failures)})")
    for f in failures:
        print("  " + f)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
