"""The Gaussian beam solver on the verification scenes (docs/plans/completed/physoptics-gaussian-beam-solver.md), through the batch
mode's --solve-beams, against the closed forms tools/verify/optics_verification.py wrote beside each scene
(<name>.expected.json) and the light-traced meters it measured.

  every scene      each beam axis reads Hit at every visit, and the power budget adds up to the laser's to 1e-12
  gaussian_train   the waist after the lens and its distance from the back vertex against the thick lens's ABCD law
  telescopes       the radii on the probe plane and the beam profiler against ABCD
  oap_focus        the 90 degree off-axis parabola's focus: one waist, no astigmatism, where ABCD puts it
  tilted mirror    the 45 degree mirror's tangential and sagittal foci, R cos / 2 and R / (2 cos) as Gaussian waists
  beam_axis_setup  the meters against the render's (exact RGB reads the coatings over the visible spectrum, not at the
                   laser's line, so within 3 %); Follow beam from the source builds the axis with every visit Hit; with
                   the iris opened wide, M2's adjuster turned five revolutions marks the lens Missed by the distance the
                   geometry gives, to 1 um; the iris closed to 0.6 mm reads Clipped by a Gaussian's loss through the
                   opening it clamps to, the visits after it Hit
  double pass      both visits of the lens Hit, the focus and the return's offset against ABCD

  python3 tools/verify/beam_solver_check.py
"""

import copy
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vkscene  # noqa: E402

SCENES = vkscene.TEST_SCENES / "optics_verification"
failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)


def rel(a, b):
    return abs(a - b) / max(abs(b), 1e-300)


def solve(scene_path, workdir, follow=False):
    """the solver's solution for a scene file, as the batch mode writes it"""
    out = Path(workdir) / (Path(scene_path).stem + ".beams.json")
    env = dict(os.environ)
    env["VKGUIDE_WINDOW"] = "background"
    command = [str(vkscene.BIN / "vulkan_guide"), "--render", str(Path(scene_path).resolve()), "--solve-beams", str(out)]
    if follow:
        command.append("--follow-beams")
    result = subprocess.run(command, cwd=vkscene.BIN, env=env, capture_output=True, text=True, timeout=300)
    if result.returncode != 0 or not out.exists():
        raise RuntimeError(f"--solve-beams failed (exit {result.returncode}):\n{result.stdout[-2000:]}{result.stderr[-2000:]}")
    return json.loads(out.read_text())


def edited(name, workdir, edit):
    """a copy of a verification scene with `edit` applied to its payload"""
    scene = json.loads((SCENES / f"{name}.gltf").read_text())
    payload = scene["scenes"][0]["extras"]
    edit(payload)
    path = Path(workdir) / f"{name}_edited.gltf"
    path.write_text(json.dumps(scene))
    return path


def main_beam(solution, tree=0):
    """the segments of a tree that are not ghosts, by index"""
    return [(i, s) for i, s in enumerate(solution["trees"][tree]["segments"]) if not s["ghost"]]


def leaving(solution, optic, port, tree=0, nth=0):
    """the nth main segment that leaves `optic` by `port`"""
    found = [s for _, s in main_beam(solution, tree) if s["from"] == optic and s["port"] == port]
    return found[nth] if nth < len(found) else None


def every_visit_hit(name, solution):
    for axis in solution["axes"]:
        statuses = [(n["optic"], n["status"]) for n in axis["nodes"]]
        check(all(s == "Hit" for _, s in statuses), f"{name}: axis '{axis['name']}' reads {', '.join(f'{o} {s}' for o, s in statuses)}")
    for tree in solution["trees"]:
        total = tree["budget"]["total"]
        check(rel(total, tree["power"]) < 1e-12, f"{name}: the budget adds up to {total:.15g} W of {tree['power']:g} W ({len(tree['segments'])} segments, {solution['solveMs']:.3f} ms)")


def readings_by_optic(solution, tree=0):
    out = {}
    for r in solution["trees"][tree]["readings"]:
        out.setdefault(r["optic"], []).append(r)
    return out


def main():
    workdir = Path(tempfile.mkdtemp(prefix="beam_solver_check_"))
    expected = {p.stem.replace(".expected", ""): json.loads(p.read_text()) for p in SCENES.glob("*.expected.json")}

    print("gaussian_train")
    s = solve(SCENES / "gaussian_train.gltf", workdir)
    every_visit_hit("gaussian_train", s)
    e = expected["gaussian_train"]
    seg = leaving(s, "L1", "Transmitted")
    for k, label in ((0, "first"), (1, "second")):
        w = seg["waists"][k]
        check(rel(w["w0"], e["waist"]) < 1e-4, f"{label} axis: waist {w['w0'] * 1e6:.4f} um, ABCD {e['waist'] * 1e6:.4f} um")
        check(abs(w["at"] - e["waist_position_after_lens"]) < 1e-6, f"{label} axis: waist {w['at'] * 1e3:.4f} mm past the back vertex, ABCD {e['waist_position_after_lens'] * 1e3:.4f} mm")

    for name in ("telescope_kepler", "telescope_galilean"):
        print(name)
        s = solve(SCENES / f"{name}.gltf", workdir)
        every_visit_hit(name, s)
        e = expected[name]
        readings = readings_by_optic(s)
        for k, radius in enumerate(e["radius"]):
            r = readings.get(f"{name}_{k}", [None])[0]
            check(r is not None and max(rel(r["radii"][0], radius), rel(r["radii"][1], radius)) < 1e-4,
                  f"{e['planes_after_second_lens'][k]:g} m past the second lens: radii {r['radii'][0] * 1e3:.5f} x {r['radii'][1] * 1e3:.5f} mm, ABCD {radius * 1e3:.5f} mm" if r else "no reading")

    print("oap_focus")
    s = solve(SCENES / "oap_focus.gltf", workdir)
    every_visit_hit("oap_focus", s)
    e = expected["oap_focus"]
    seg = leaving(s, "OAP1", "Reflected")
    a, b = seg["waists"]
    check(rel(a["w0"], b["w0"]) < 1e-6 and abs(a["at"] - b["at"]) < 1e-9, f"no astigmatism: waists {a['w0'] * 1e6:.5f} and {b['w0'] * 1e6:.5f} um, {abs(a['at'] - b['at']) * 1e9:.3f} nm apart")
    check(rel(a["w0"], e["waist"]) < 1e-3, f"waist {a['w0'] * 1e6:.4f} um, ABCD {e['waist'] * 1e6:.4f} um")
    check(abs(a["at"] - e["focus_from_datum"]) < 1e-6, f"focus {a['at'] * 1e3:.4f} mm from the parabola, ABCD {e['focus_from_datum'] * 1e3:.4f} mm")

    print("tilted_spherical_mirror (45 degrees)")
    s = solve(SCENES / "tilted_spherical_mirror.gltf", workdir)
    every_visit_hit("tilted_spherical_mirror", s)
    e = next(entry for entry in expected["tilted_spherical_mirror"]["angles"] if entry["angle"] == 45.0)
    seg = leaving(s, "CM1", "Reflected")
    # the frame's first axis lies in the plane of incidence here: tangential, the second sagittal
    for k, label in ((0, "tangential"), (1, "sagittal")):
        w = seg["waists"][k]
        check(abs(w["at"] - e[label]["focus"]) < 1e-6 and rel(w["w0"], e[label]["waist"]) < 1e-4,
              f"{label}: focus {w['at'] * 1e3:.4f} mm, ABCD {e[label]['focus'] * 1e3:.4f} mm; waist {w['w0'] * 1e6:.4f} um against {e[label]['waist'] * 1e6:.4f} um")

    print("beam_axis_setup")
    s = solve(SCENES / "beam_axis_setup.gltf", workdir)
    every_visit_hit("beam_axis_setup", s)
    e = expected["beam_axis_setup"]
    readings = readings_by_optic(s)
    for meter, rendered in e["power"].items():
        solved = sum(r["power"] for r in readings.get(meter, []))
        check(rel(solved, rendered) < 0.03, f"{meter}: {solved * 1e3:.4f} mW, the render read {rendered * 1e3:.4f} mW ({(solved / rendered - 1) * 100:+.2f} %)")
    # Follow beam from the source, on the axis emptied
    path = edited("beam_axis_setup", workdir, lambda p: [axis.update(nodes=[]) for axis in p["beamAxes"]])
    followed = solve(path, workdir, follow=True)
    nodes = followed["axes"][0]["nodes"]
    check(sorted(n["optic"] for n in nodes) == sorted(["M1", "M2", "I1", "L1", "BS1", "PM_R", "PM_T"]) and all(n["status"] == "Hit" for n in nodes),
          "Follow beam builds the axis: " + ", ".join(f"{n['port'] or 'source'} > {n['optic']} {n['status']}" for n in nodes))
    # M2's adjuster one revolution: the lens Missed by the distance the turned beam passes its aperture's edge. The scene
    # gives M2's body pose, which a mount's adjusters would be absorbed into; saved back through the editor, it gives the
    # mount's own pose, whose adjuster turns the mirror
    canonical = Path(workdir) / "beam_axis_setup_canonical.gltf"
    env = dict(os.environ)
    env["VKGUIDE_WINDOW"] = "background"
    subprocess.run([str(vkscene.BIN / "vulkan_guide"), "--render", str((SCENES / "beam_axis_setup.gltf").resolve()), "--save-scene", str(canonical)],
                   cwd=vkscene.BIN, env=env, capture_output=True, text=True, timeout=300, check=True)
    # one revolution of a KM100's adjuster tilts M2 8 mrad, which moves the beam 3.2 mm at the lens, 12.7 mm in radius,
    # and the open iris's blades take it past 1.6 revolutions: with the iris opened wide, five revolutions miss the lens
    scene = json.loads(canonical.read_text())
    for o in scene["scenes"][0]["extras"]["optics"]:
        if o["name"] == "M2":
            o["mount"]["fine"] = {"x": 5.0, "y": 0.0}
        if o["name"] == "I1":
            o["params"]["opening"] = 1.0
    turned_path = Path(workdir) / "beam_axis_setup_turned.gltf"
    turned_path.write_text(json.dumps(scene))
    turned = solve(turned_path, workdir)
    statuses = {n["optic"]: n for n in turned["axes"][0]["nodes"]}
    lens = next(o for o in json.loads((SCENES / "beam_axis_setup.gltf").read_text())["scenes"][0]["extras"]["optics"] if o["name"] == "L1")
    lens_z = lens["mount"]["position"][2]
    lens_axis = lens["mount"]["position"][0], lens["mount"]["position"][1]
    beam = leaving(turned, "M2", "Reflected")
    t = (lens_z - beam["start"][2]) / beam["direction"][2]
    crossing = [beam["start"][i] + t * beam["direction"][i] for i in range(3)]
    off = math.hypot(crossing[0] - lens_axis[0], crossing[1] - lens_axis[1]) - 0.5 * lens["params"]["diameter"]
    tilt = math.acos(max(-1.0, min(1.0, beam["direction"][2]))) / 2.0
    node = statuses["L1"]
    check(node["status"] == "Missed", f"M2's adjuster turned five revolutions ({tilt * 1e3:.3f} mrad of tilt), the iris open: the lens is {node['status']}")
    check(abs(node["missDistance"] - off) < 1e-6, f"missed by {node['missDistance'] * 1e3:.4f} mm; the turned beam crosses the lens's plane {off * 1e3:.4f} mm past its edge")
    check(all(statuses[o]["status"] == "Unreached" for o in ("BS1", "PM_R", "PM_T")), "what follows the lens is Unreached")
    # the iris closed to 0.6 mm
    def close(p):
        for o in p["optics"]:
            if o["name"] == "I1":
                o["params"]["opening"] = 0.6e-3
    closed = solve(edited("beam_axis_setup", workdir, close), workdir)
    statuses = {n["optic"]: n for n in closed["axes"][0]["nodes"]}
    iris = statuses["I1"]
    at_iris = closed["trees"][0]["segments"][iris["segment"]]
    w = at_iris["radiiEnd"]
    # the iris clamps its opening to its size's smallest; the lost fraction is a Gaussian's outside the opening it has
    lost = iris["lostFraction"]
    opening = 2.0 * math.sqrt(-math.log(lost) * w[0] * w[1] / 2.0) if 0.0 < lost < 1.0 else float("nan")
    check(iris["status"] == "Clipped", f"iris closed: {iris['status']}, {lost * 100:.4f} % lost, which a centered Gaussian of {w[0] * 1e3:.4f} mm loses through a {opening * 1e3:.4f} mm opening")
    check(all(statuses[o]["status"] == "Hit" for o in ("L1", "BS1", "PM_R", "PM_T")), "the visits after the iris are Hit: " + ", ".join(f"{o} {statuses[o]['status']}" for o in ("L1", "BS1", "PM_R", "PM_T")))

    print("beam_axis_double_pass")
    s = solve(SCENES / "beam_axis_double_pass.gltf", workdir)
    every_visit_hit("beam_axis_double_pass", s)
    e = expected["beam_axis_double_pass"]
    seg = leaving(s, "L1", "Transmitted", nth=0)
    check(abs(seg["waists"][0]["at"] - e["focus_after_lens"]) < 1e-6, f"focus {seg['waists'][0]['at'] * 1e3:.4f} mm past the lens, ABCD {e['focus_after_lens'] * 1e3:.4f} mm")
    back = leaving(s, "L1", "Transmitted", nth=1)
    check(back is not None and abs(abs(back["start"][0]) - e["return_offset"]) < 2e-4,
          f"the return leaves the lens {abs(back['start'][0]) * 1e3:.3f} mm off its axis, 2 f tan(2 alpha) = {e['return_offset'] * 1e3:.3f} mm" if back else "no return through the lens")
    solved = sum(r["power"] for r in readings_by_optic(s).get("PM", []))
    check(rel(solved, e["power"]["PM"]) < 0.03, f"PM: {solved * 1e3:.4f} mW, the render read {e['power']['PM'] * 1e3:.4f} mW")

    print()
    if failures:
        print(f"beam_solver_check: {len(failures)} check(s) FAILED")
        return 1
    print("beam_solver_check: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
