#!/usr/bin/env python3
"""Runs every verification check and prints one line per result:

  bin/bsdf_test, bin/light_test, bin/sampler_test, bin/film_test, bin/aov_test, bin/bvh_bench   the CPU tests
  VKGUIDE_SELFTEST=1 bin/vulkan_guide                              shaders against their C++ mirrors
  furnace_check.py, cornell_check.py, filter_check.py, film_check.py, aov_check.py   scene-level energy, reference, filter, film and output checks

Build first (cd build && cmake --build .). The renders need the GPU and take a few minutes; --cpu-only
skips them, --quick shrinks them.

Run:  python3 tools/verify/run_all.py [--cpu-only] [--quick] [--skip-pbrt]
"""

import argparse
import os
import subprocess
import sys
import time

from vkscene import BIN, ROOT

HERE = os.path.dirname(os.path.abspath(__file__))


def run(label, command, env=None, cwd=None):
    start = time.time()
    result = subprocess.run(command, capture_output=True, text=True, env=env, cwd=cwd)
    seconds = time.time() - start
    status = "ok" if result.returncode == 0 else "FAIL"
    print(f"{status:5s} {label} ({seconds:.0f} s)")
    if result.returncode != 0:
        tail = (result.stdout + result.stderr).strip().splitlines()[-12:]
        print("      " + "\n      ".join(tail))
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpu-only", action="store_true")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--skip-pbrt", action="store_true")
    args = parser.parse_args()

    ok = True
    quick = ["--quick"] if args.quick else []
    ok &= run("bsdf_test", [str(BIN / "bsdf_test")] + quick)
    ok &= run("light_test", [str(BIN / "light_test")])
    ok &= run("sampler_test", [str(BIN / "sampler_test")])
    ok &= run("film_test", [str(BIN / "film_test")])
    ok &= run("aov_test", [str(BIN / "aov_test")])
    ok &= run("bvh_bench", [str(BIN / "bvh_bench"), str(ROOT / "assets/models/structure.glb"), "--rays", "2000"])
    if not args.cpu_only:
        env = dict(os.environ, VKGUIDE_WINDOW="background", VKGUIDE_SELFTEST="1")
        ok &= run("GPU self-tests", [str(BIN / "vulkan_guide")], env=env, cwd=BIN)
        ok &= run("furnace_check", [sys.executable, os.path.join(HERE, "furnace_check.py")] + quick, cwd=HERE)
        cornell = [sys.executable, os.path.join(HERE, "cornell_check.py")]
        if args.quick:
            cornell += ["--spp", "256"]
        if args.skip_pbrt:
            cornell += ["--skip-pbrt"]
        ok &= run("cornell_check", cornell, cwd=HERE)
        ok &= run("filter_check", [sys.executable, os.path.join(HERE, "filter_check.py")] + (["--spp", "256"] if args.quick else []), cwd=HERE)
        ok &= run("film_check", [sys.executable, os.path.join(HERE, "film_check.py")], cwd=HERE)
        ok &= run("aov_check", [sys.executable, os.path.join(HERE, "aov_check.py")], cwd=HERE)
    print("all checks passed" if ok else "some checks failed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
