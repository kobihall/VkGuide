# Optical data export and comparison

A simulator is believed when it agrees with one people already trust. The user can run Zemax OpticStudio, open-source Python ray tracers (Optiland, RayOptics) and open-source wave optics codes (LightPipes, POPPY). This plan writes what the renderer computes in formats those programs read, and adds the scripts that run the same setup in both and report the difference: detector images and beam fields as ZBF beam files and arrays, ray sets as ZRD ray databases and arrays, and the solver's beam as tables.

*Not implemented. Depends on [physoptics-detectors.md](physoptics-detectors.md) and [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md) for what is exported, [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md) for the readback and file plumbing and its batch mode, and [physoptics-prescription-import.md](physoptics-prescription-import.md) for the shared `.zmx` setups. Part of the [laser optics plan](physoptics-laser-optics.md).*

## What exists today

- [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md) plans EXR, PFM, HDR and PNG export of the camera image, a readback through a host-visible buffer with `vmaInvalidateAllocation()`, and a batch mode (`--render`, `--spp`, `--out`).
- Detector films live in binding 60 ([physoptics-detectors.md](physoptics-detectors.md)).

## Design

### ZBF: beam files

OpticStudio's physical optics propagation (POP) reads and writes the ZBF beam file: a complex field sampled on a regular grid. Its binary layout, as the OpticStudio manual and Zemax's own example code give it: a header of 4-byte integers (format version, nx, ny, the "is polarized" flag, the units: 0 mm, 1 cm, 2 in, 3 m, and reserved words), then 8-byte doubles (the grid spacing dx and dy, the pilot beam's waist position, Rayleigh range and waist for each axis, the wavelength, the medium's index, and reserved values; the implementing session takes the exact order from the manual), then the field, Ex and for a polarized file Ey, as interleaved real and imaginary doubles in row-major order. `|Ex|² + |Ey|²` summed over the grid is power in watts.

The renderer writes two kinds of ZBF:

- The solver's beam at a plane. The Gaussian beam's complex field is known in closed form from its `Q`, its power and its Jones vector, with the wavefront curvature and the Gouy phase. Exporting it at a detector's or a probe plane's position gives a ZBF that POP can propagate onward, or compare against its own propagation of the same input through the same optics. It has no diffraction from clipping, as the solver has none.
- A detector's film. A film is power, not field, so its ZBF has amplitude `√(power per sample area)` and zero phase, with the polarized flag clear. It compares against POP's irradiance, not its phase, and the file says so in its comment field. Once the wave backend exists, a coherent film can be written with its phase.

"ZBF2" was named in the request; the OpticStudio documentation found while writing this plan describes only the ZBF format above (version 1, binary or text). Whether a newer format exists and is needed is an open question for the user.

### ZRD: ray databases

OpticStudio's non-sequential mode records rays in ZRD files, its ray database, which its ray database viewer and filter strings read and which can be replayed as a source. Its uncompressed layout is documented in the OpticStudio manual: per ray segment, the segment's level, its hit object and face, its status bits (reflected, transmitted, scattered, absorbed), the position, direction, normal, intensity and phase, path length and wavelength.

**Record rays** in the light integrator's settings stores up to a set number of light paths' segments into binding 61, `RayRecords`, during one frame: each segment as a 64-byte record (position, direction, power, wavelength, the optic and surface it ended on, the branch it took, its on-graph state), written by the stages that end a segment (02's classification, 13, 26). The CPU reads the buffer back after the frame and writes ZRD (uncompressed mode) or a NumPy array. Objects are numbered in the file by their index in the scene's optics list, and a sidecar JSON maps numbers to names. It is meant for thousands to millions of rays, a diagnostic, not a render.

### Arrays and tables

- Detector films as NumPy `.npy` (float32 or float64, with a JSON sidecar of the detector's frame, pixel size, channels and units) and as EXR through [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md).
- The solver's segments and readouts as CSV (the Beam window's **Copy as CSV**, written to a file).
- A detector's measurements (power, centroid, widths, encircled power) as JSON, for scripts.

### Comparison scripts

`tools/compare/` holds Python scripts, each run on a scene saved by the renderer and on the same setup in the reference program, reporting the differences with their tolerances. Each starts from a shared description: either a `.zmx` the renderer imports ([physoptics-prescription-import.md](physoptics-prescription-import.md)) or a scene the script converts.

| Script | Reference | Compares |
|---|---|---|
| `paraxial_vs_opticstudio.py` | OpticStudio, through its ZOS-API from Python | effective and back focal lengths, pupils, and real ray intercepts per field |
| `pop_vs_opticstudio.py` | OpticStudio POP | the solver's ZBF at the image against POP's from the same input ZBF: beam radii, waist positions, peak irradiance |
| `nsc_vs_opticstudio.py` | OpticStudio non-sequential, detectors and ZRD | detector power and irradiance images, ghost powers from ZRD filter strings |
| `rays_vs_optiland.py` | Optiland (or RayOptics) | spot diagrams and RMS spot radii, from the renderer's ZRD or `.npy` rays against Optiland's real ray trace of the same `.zmx` |
| `gaussian_vs_lightpipes.py` | LightPipes (or POPPY) | the solver's Gaussian beam radii along a train of thin optics against a Fresnel propagation |

OpticStudio runs on Windows. The scripts that need it write their inputs and read its outputs through files, so they can run on a Windows machine with the files copied across; the open-source ones run beside the renderer on this Mac. Each script records its tolerance and its last result in `docs/plans/physoptics-verification-scenes.md`'s results tables.

### The Export menu

**File** → **Export Optical Data...**, registered under the **Optical bench** capability ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)), opens a dialog over what the scene has: each detector (ZBF, NPY, EXR, JSON), each probe plane's solver beam (ZBF), the last recorded rays (ZRD, NPY), and the solver's tables (CSV). The batch mode gains `--export-detectors <dir>`, which writes every detector after the render's target sample count, for the scripts.

## Registry, bindings and data

- Binding 61 `RayRecords`, allocated only while **Record rays** is on.
- `src/optic_export.h/.cpp`: ZBF writing and reading (reading lets a ZBF from POP be loaded as a reference image on a detector), ZRD writing, NPY writing.
- `tools/compare/*.py`.
- No scene-file change: export settings are per session.

## Verification

- With nothing exported and **Record rays** off, every scene renders bit-identical and nothing is allocated.
- A ZBF written by the renderer and read back by its own reader is identical; one written for a known Gaussian beam reads in OpticStudio's POP with the same power, beam radius and waist position (the POP beam file viewer's summary), and a ZBF written by POP reads in the renderer with the power and grid it reports.
- A ZRD written from a frame of a known source opens in OpticStudio's ray database viewer with the same ray count and segment hits, and its filter strings select the ghost rays the renderer's on-graph state marked off-graph.
- Each comparison script runs to completion on its test scene and reports its numbers into the verification tables.

## Order of work

1. NPY, CSV and JSON exports, which the open-source scripts need first.
2. `rays_vs_optiland.py` and `gaussian_vs_lightpipes.py`.
3. ZBF from the solver, and its reader; `pop_vs_opticstudio.py`.
4. ZBF from detector films.
5. **Record rays** and ZRD; `nsc_vs_opticstudio.py`.
6. `paraxial_vs_opticstudio.py`.

## Waiting for the wave backend

When this plan is implemented, move this section to `docs/plans/waiting/physoptics-data-export.md`.

- ZBF files of coherent detector films, with their phase, polarized when the backend carries Jones vectors.
- Comparisons of diffraction patterns (an iris's Airy rings, a double slit's fringes) against POP and LightPipes, which need coherent films.

## Open questions

- ZBF2. The request names ZBF or ZBF2. Only ZBF (version 1) appears in the OpticStudio documentation consulted. The user should say whether ZBF2 is a format their OpticStudio version writes, and if so where its specification is.
- Whether a standard ray file for sources, such as IES TM-25 (the ANSI/IES ray file format that LightTools, OpticStudio and Speos exchange), is worth reading so that a measured source can drive the renderer. Lasers are described well by their Gaussian parameters, so the request does not need it.

## Sources

The Zemax OpticStudio manual: "ZBF file format" (POP beam files), "ZRD file format" (the uncompressed ray database), physical optics propagation, non-sequential detectors and filter strings, the ZOS-API; Zemax Community, "Python reading and writing binary files: ZRD, ZBF, DAT, SDF" (example code for both layouts); the Optiland documentation (optiland.readthedocs.io); Hayford, RayOptics documentation; Vdovin and van Goor, LightPipes documentation; Perrin et al., POPPY documentation; IES TM-25-13, *Ray file format for the description of the emission property of light sources*; the NumPy `.npy` format specification.
