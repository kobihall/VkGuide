# Lens prescription import

Optical designs live in lens design files. A camera lens from a patent, a microscope objective from a vendor, a telescope from a textbook all arrive as a sequence of surfaces: radius, thickness, glass, semi-diameter, surface by surface. OpticStudio's `.zmx` format is the common one, and pbrt ships its own small format for the lenses of its realistic camera. This plan reads both into the renderer's own representation, interpreting a sequence of surfaces as a set of optical elements on an assembly, and refuses a file it cannot interpret faithfully rather than guessing. It is a one-way import: nothing is written back.

*Not implemented. Depends on [physoptics-optical-elements.md](physoptics-optical-elements.md), [physoptics-beam-axis.md](physoptics-beam-axis.md) (assemblies) and [physoptics-optical-materials.md](physoptics-optical-materials.md) (the AGF reader and glasses). [pbrt-cameras.md](pbrt-cameras.md)'s realistic camera uses what this plan imports. Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

- Import is one-way, and an import may fail rather than be complicated by every corner of the format (the request).
- The realistic camera of [pbrt-cameras.md](pbrt-cameras.md) traces through an assembly made by this importer, not through a separate lens table (the request, which asks that the cameras plan use the beam axis object and an interpreter for pbrt's lens files).

## What exists today

- `assets/models/pbrt-v4-scenes/lenses/` holds pbrt's prescriptions: `dgauss.50mm.dat` (a 50 mm f/2 double Gauss, from US patent 2,673,491 scaled from 100 mm), `wide.22mm.dat`, `fisheye.10mm.dat` and `telephoto.250mm.dat`, with unscaled versions. Each line is a radius, the thickness to the next surface, an index of refraction (0 for the aperture stop) and an aperture diameter, in millimeters, ordered from the scene to the film.
- Nothing reads `.zmx`. [physoptics-optical-materials.md](physoptics-optical-materials.md) reads AGF glass catalogs.

## Design

### The intermediate form

Both readers produce a `SequentialPrescription` (`src/optic_prescription.h/.cpp`, in `rt_optics`): an ordered list of surfaces, each with a type, a curvature, a conic and aspheric terms, a thickness to the next, a glass name or a mirror flag, a clear semi-diameter, an optional mechanical semi-diameter, a stop flag, and the decenters and tilts of coordinate breaks before it. Plus the system's units, wavelengths, aperture (entrance pupil diameter or f-number) and fields. A sequential prescription is not yet a set of elements; the interpreter below makes it one.

### Reading `.zmx`

`src/optic_zmx.cpp` reads the text format (UTF-16 or 8-bit, as OpticStudio writes either), one keyword per line:

| Keyword | Read as |
|---|---|
| `UNIT` | millimeters, centimeters, inches or meters |
| `MODE` | must be `SEQ`; a non-sequential file is refused |
| `ENPD`, `FNUM`, `OBNA` | the aperture |
| `WAVM`, `PWAV` | the wavelengths and the primary one, which becomes the assembly's design wavelength |
| `YFLN`, `XFLN`, `FTYP` | the fields |
| `GCAT` | the glass catalogs to search, by name: the bundled file and any loaded AGF files |
| `SURF n` and its block | one surface: `TYPE`, `CURV`, `CONI`, `DISZ`, `GLAS`, `DIAM`, `MEMA`, `STOP`, `PARM` |

Surface types it understands: `STANDARD`, `EVENASPH` (`PARM 1` to `8` as the even terms), `COORDBRK` (decenters, tilts and their order flag from `PARM 1` to `6`), and `PARAXIAL` only when the user allows ideal lenses (see "Open questions"). A `GLAS` of `MIRROR` makes the surface reflect. A `MODEL` glass (index, Abbe number and partial dispersion) becomes a glass from OpticStudio's model-glass formula. Infinite thicknesses before the first surface or after the last are the object and image distances.

Anything else refuses the import, with the line and the keyword: other surface types (toroidal until [physoptics-gratings-and-xuv.md](physoptics-gratings-and-xuv.md) adds the toroid, gratings, Zernike, grid sag), multi-configuration data (`MCE`, `MNUM` above 1), solves that the file does not resolve into numbers, pickups, glasses no loaded catalog contains, and gradient-index media.

### Reading pbrt's `.dat`

`src/optic_pbrt_lens.cpp` reads pbrt's four columns. A radius of 0 is the aperture stop (pbrt's convention), an index of 0 or 1 is air, and any other index is a glass of that constant index, since the format carries no dispersion: the importer names it `pbrt n=1.670` and the panel suggests the catalog glass with the nearest n_d, which the user may substitute. The last thickness is where pbrt places the film before focusing; the importer keeps it as the sensor's initial distance, and [pbrt-cameras.md](pbrt-cameras.md)'s focusing moves it.

### Interpreting surfaces as elements

The interpreter walks the sequence and groups surfaces into the elements of [physoptics-optical-elements.md](physoptics-optical-elements.md):

- A run of surfaces separated by glass (a surface whose `GLAS` is not air, up to the next surface that returns to air) is one optical body. Two surfaces make a singlet; three or more a cemented doublet or triplet, with the inner surfaces `Internal`. The body's edge is a cylinder at the larger of each surface's mechanical semi-diameter, or its clear one when none is given, between the faces' sags.
- A mirror surface becomes a mirror body: the surface as its front, and a substrate behind it whose thickness the file does not give. The importer uses the mechanical semi-diameter's sixth as the thickness, a common ratio for mirror blanks, and the report says so.
- The stop becomes an iris element at the stop's semi-diameter. A surface with a clear aperture but no glass change (a field stop, a baffle) becomes an iris too.
- Dummy surfaces (air on both sides, no aperture) only advance the position.
- Coordinate breaks tilt and decenter everything after them, in OpticStudio's order convention, so a folded or off-axis design keeps its geometry.
- The image surface becomes a camera sensor detector of the image's size, when the file has one.

The elements are of the generic kind `prescription`, whose parameters are the body's surfaces themselves rather than catalog parameters, so any lens the format describes fits. They are members of a new assembly ([physoptics-beam-axis.md](physoptics-beam-axis.md)), placed relative to its datum at the first surface's vertex, and the assembly's root is the file's aperture and fields. Its members are saved inline in the scene file, with the source file's path for reference: the scene does not depend on the `.zmx` afterwards.

### Where an import goes

**Optics** → **Import prescription...**, in the menu the **Optical bench** capability adds ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)), opens a file dialog for `.zmx` or `.dat` and creates the assembly, mounted at the bench's beam height on a single post by default, or free. **Use as camera lens** offers the assembly to a realistic camera ([pbrt-cameras.md](pbrt-cameras.md)). The import report lists what was read, every substitution (a pbrt constant-index glass, a mirror substrate thickness), and, when the import is refused, why.

### Checking an import

After an import the panel shows the system's paraxial data from `rt_optics`: effective focal length, back focal length, entrance and exit pupil positions and diameters, and the f-number at the design wavelength, computed by a paraxial trace through the imported bodies. They are the numbers OpticStudio's system data report shows for the same file, so a mismatch shows a wrong interpretation at once.

## Registry, bindings and data

- `SequentialPrescription`, the `.zmx` and `.dat` readers, the interpreter and the paraxial report (`src/optic_prescription.*`, `src/optic_zmx.cpp`, `src/optic_pbrt_lens.cpp`), in `rt_optics`.
- The `prescription` optic kind; **Import prescription...**; the import report window.
- Scene file: the assembly with its members' surfaces inline, and the source path.
- No binding or kernel change.

## Verification

- Scenes without an import render bit-identical.
- `bin/optics_test`:
    - `dgauss.50mm.dat` imports as four bodies (two singlets and two cemented doublets, by its index changes), an iris and a sensor, with an effective focal length of 50 mm within 1 %, and the same for pbrt's other lenses at their nominal focal lengths;
    - a `.zmx` of a Thorlabs AC254-100-A achromatic doublet (Thorlabs publishes `.zmx` files for its lenses) imports as one cemented body whose effective and back focal lengths match the Thorlabs page to 0.1 mm;
    - a `.zmx` with a coordinate-break fold mirror keeps every vertex at the position OpticStudio's prescription report lists, to 1 µm;
    - a non-sequential file, a toroidal surface and a missing glass each refuse with their reason and line.
- Against OpticStudio ([physoptics-data-export.md](physoptics-data-export.md)): for each test file, the paraxial report matches OpticStudio's system data to 1e-5 relative, and real ray intercepts at the image for the file's fields match its ray trace report to 1e-4 of the image height.

## Order of work

1. `SequentialPrescription` and the pbrt `.dat` reader, with the interpreter for singlets, doublets and stops.
2. The paraxial report and its tests.
3. The `.zmx` reader: standard and even-asphere surfaces, glasses through AGF, model glasses.
4. Coordinate breaks and mirrors.
5. The assembly, the import report, and **Use as camera lens**.

## Open questions

- Whether OpticStudio's `PARAXIAL` surface (an ideal thin lens, often used as a perfect tube lens or eyepiece in a design) should be importable as an "ideal lens" surface kind. It is not physical, so it would be flagged in the scene, but it makes many textbook designs importable. The plan refuses it unless the user enables it.
- Which other formats are worth reading. CODE V's `.seq` and the open Optiland JSON format are candidates; neither is needed for the request.
- The mirror substrate thickness rule (a sixth of the mechanical diameter) is a convention, not data. A user who knows the real blank should set it.

## Sources

The Zemax OpticStudio manual, "The ZMX file format" (keywords, surface types, coordinate breaks and their order flag, model glasses); PBR 3ed §6.4.1 (the realistic camera's lens description and the `.dat` format) and pbrt-v4's `RealisticCamera`; Smith, *Modern lens design*, p. 312, which the header of pbrt's `dgauss.dat` cites as its source; Thorlabs' downloadable `.zmx` files for its catalog lenses.
