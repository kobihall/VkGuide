# Test scenes

The scripts in `tools/verify/` write the scenes in this folder and render them
in their checks. Don't edit these scenes by hand: a check that renders an
edited scene fails, or passes for the wrong reason. To change one, change its
generator and run it. To make a scene of your own from one of these, copy it
to a folder of `assets/scenes/` outside `tests/`.

## Suites

Each folder is one suite. You write it with the first script and check it with
the second:

*   `core/`: `make_scenes.py`, read by `aov_check.py`, `film_check.py`,
    `filter_check.py`, `spectral_check.py` and `bidirectional_scenes.py`. It
    also holds copies of four hand-made scenes those checks read:
    `cornell_box`, `all_materials`, `sphere_scene` and `veach_mis`.
*   `bidirectional/`: `bidirectional_scenes.py`, `bidirectional_check.py`.
*   `cameras/`: `camera_scenes.py`, `camera_check.py`. The check writes
    `cam_room_equirect.exr` here, which `cam_mirror_env.gltf` reads and git
    ignores.
*   `lights_and_textures/`: `light_texture_scenes.py`,
    `light_texture_check.py`.
*   `optics/`: `optics_scenes.py`, `optics_check.py`.
*   `lasers/`: `laser_scenes.py`, `laser_check.py`.
*   `detectors/`: `detector_scenes.py`, `detector_check.py`.
*   `optics_verification/`: `optics_verification.py --write`.

The laser, detector, and optics verification checks build their scenes in
memory. The files in those folders show you what the checks render, and each
`<name>.expected.json` holds the values its scene must show.

A scene here names an asset three folders up, for example
`../../../textures/checker.png`.
