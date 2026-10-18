# Material system

Materials are about to grow from six types with a handful of scalar fields to around fifteen types whose parameters can be spectra, textures or both. Today each shape and each overridden mesh object carries its own inline `SceneMaterial`, which stores every type's fields at once, and the GPU gets one fixed 64-byte record per material. This plan replaces that with a named material library, a traits table that describes every type's parameters as data, variable-length GPU records, and the two surface queues the user chose for kernel 06.

*Not implemented. Depends on [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md). [pbrt-bsdf-library.md](pbrt-bsdf-library.md), [pbrt-textures.md](pbrt-textures.md), [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) and every other appearance plan add types and parameter kinds through it. Part of the [roadmap](pbrt-roadmap.md).*

## Decisions already made

On 2026-09-26 the user chose:

- A named material library that objects reference by id, over inline per-object materials.
- glTF materials editable in place, with the edits saved as overrides keyed by the model and the glTF material index.
- Two surface queues, basic and complex, with kernel 06 compiled once per class.

## What the renderer does today

- `SceneMaterial` (`src/rt_scene_types.h`) holds every type's fields (albedo, fuzz, smoothness, ir, strength, metallic, roughness, emission), so switching an object's type and back keeps its values.
- `SceneShape::material` and `SceneMeshObject::material` are inline copies. A glTF surface's material comes from its `GLTFMaterial` through `gltfTriMaterial()` (`src/rt_accel.cpp`) into `RaytraceTriMaterial`, which adds texture layers, the alpha cutoff and the normal scale.
- `GpuMaterial` (`crt_common.glsl`, mirrored by `CrtMaterial` in `rt_gpu.h`) is 64 bytes: `param` means fuzz, smoothness, IOR or strength depending on the type.
- Kernel 06 is one shader with a `switch` over the type in `scatterSurface()`, `evalSurface()` and `pdfSurface()`.
- `drawMaterialParams()` in `rt_scene_editor.cpp` hand-writes the widgets for each type.

## Design

### The library

```cpp
struct MaterialDef {
    uint64_t id;                 // stable, shares the editor's id space
    std::string name;
    MaterialType type;
    ParamSet params;             // by parameter name, every value ever set
    // where it came from: empty for a user material, else the model and glTF material index
    std::optional<GltfMaterialOrigin> origin;
    // for a glTF material: the parameters the user changed from the imported values
    std::set<std::string> edited;
    bool operator==(const MaterialDef&) const = default;
};
```

`RaytraceSceneEditor` owns a `MaterialLibrary` beside the shape, camera and light lists, with the same stable ids and `revision()` counter. A shape holds a `materialId`. A mesh object's override mode holds a `materialId`; its glTF mode uses the library entries its model's import created.

Importing a glTF file creates one library entry per glTF material, named after it (or `material3` for an unnamed one, as `uniqueKey()` names them today). Removing the model removes its entries. If a user object still references one, the entry converts to a user material instead, with a load message naming it.

An optical element ([physoptics-optical-elements.md](physoptics-optical-elements.md)) owns one entry per material slot the same way: `origin` gains a second variant, the element's id and the slot, so editing an element's coating edits its own entry, the scene file saves only what differs from the kind's defaults, and deleting the element deletes its entries.

### Parameters as data

`MATERIAL_TRAITS`, indexed by `MaterialType`, plays the role `SHAPE_TRAITS` plays for shapes: the only per-type knowledge outside the shaders.

```cpp
enum class ParamKind : uint8_t { Float, Spectrum, Texture, Enum, Bool };

struct MaterialParam {
    const char* name;            // saved in the scene file; never rename
    const char* label;
    ParamKind kind;
    ParamValue defaultValue;
    float min, max;              // for sliders
    uint8_t flags;               // logarithmic, sRGB color, texturable
};

struct MaterialTraits {
    const char* name;            // "lambertian", "conductor", ...
    const char* label;
    std::span<const MaterialParam> params;
    uint8_t flags;               // Delta, Emissive, Scatters, Complex
    void (*pack)(const MaterialDef&, const PackContext&, MaterialRecordWriter&);
};
```

Because values are stored by name, switching a material from metal to conductor keeps `roughness` and loses nothing, which preserves today's behavior for every type.

`ParamValue` holds a float, a `SpectrumRef` ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)), a `TexRef` ([pbrt-textures.md](pbrt-textures.md)), an enum or a bool. Until those plans land, `SpectrumRef` is an RGB triple and `TexRef` is an optional texture layer, which is exactly what `RaytraceTriMaterial` holds today.

### GPU records

Binding 7 keeps its name and changes layout to a 16-byte header:

```glsl
struct GpuMaterial {      // binding 7, 16 bytes
    uint type;
    uint flags;           // CRT_MATERIAL_EMITS | SCATTERS | COMPLEX | CUTOUT | NORMAL_MAP ...
    uint paramOffset;     // into materialParams[], in vec4s
    uint paramCount;
};
layout (std430, set = 0, binding = 26) readonly buffer MaterialParamBuffer { vec4 materialParams[]; };
```

Each type's `pack` function writes its parameters in a fixed order, and `crt_material.glsl` reads them back in the same order into a per-type struct. The C++ writer and the GLSL reader are generated from the same traits entry by hand, side by side, with a `static_assert` on each type's record size.

The existing six types pack their current fields into the same floats they occupy today, so the shaders compute identical values.

Other plans add types through the same table, and none needs more than a traits row, its parameters and its shader case:

| Type | Plan | Class |
|---|---|---|
| `diffuse`, `conductor`, `glass`, `thin_glass`, `diffuse_transmission` | [pbrt-bsdf-library.md](pbrt-bsdf-library.md) | basic |
| `mix` | [pbrt-bsdf-library.md](pbrt-bsdf-library.md) | complex |
| `coated_diffuse`, `coated_conductor` | [pbrt-layered-materials.md](pbrt-layered-materials.md) | complex |
| `fresnel_blend` | [pbrt-layered-materials.md](pbrt-layered-materials.md) | basic |
| `measured`, later `fourier` | [pbrt-measured-bsdfs.md](pbrt-measured-bsdfs.md) | complex |
| `hair` | [pbrt-curves-and-hair.md](pbrt-curves-and-hair.md) | complex |
| `subsurface` | [pbrt-subsurface-scattering.md](pbrt-subsurface-scattering.md) | complex |
| `interface` | [pbrt-participating-media.md](pbrt-participating-media.md): no BSDF; kernels 02 and 05 continue the ray into the next medium instead of queueing it | neither |
| `linear_polarizer`, `retarder` | [pbrt-polarization.md](pbrt-polarization.md) | basic |
| `optical_interface` | [physoptics-optical-materials.md](physoptics-optical-materials.md): every optical surface of an optical body; its two indices come from the surface's media, its reflectance and transmittance from a coating | basic |
| `detector` | [physoptics-detectors.md](physoptics-detectors.md): a sensor surface, absorbing, diffuse or transparent by detector kind; kernel 02 sends light paths that hit it to the detector queue | basic |
| `laser_aperture` | [physoptics-laser-sources.md](physoptics-laser-sources.md): a laser's output aperture, emitting the beam's phase-space radiance toward a camera ray that hits it | basic |

### Two surface queues

Every material type is flagged basic or complex in its traits. Complex means a large or branchy evaluation: layered, measured, hair, subsurface, mix, and any material with a procedural texture. A material instance is complex if its type is, or if any parameter references a texture kind that only the complex build evaluates.

- Kernel 02 reads `GpuMaterial.flags` for the hit's material and appends the path to `CRT_QUEUE_SURFACE` (basic) or `CRT_QUEUE_SURFACE_COMPLEX` (queue 6).
- Every kernel 06 variant is compiled twice, with `#define CRT_MATERIAL_CLASS 0` or `1`. The basic build's type switch contains only basic types, so it allocates registers for them alone.
- The schedule dispatches kernel 06 once per queue, basic first. Both dispatches read the same path state and write disjoint entries, so the order changes no result.

The registry gains one field: `KernelVariant::perMaterialClass`, set on every kernel 06 variant, and later on kernel 07 variants that evaluate surface materials. CMake compiles such a kernel once per class through the same permutation step that builds a tracing kernel once per traversal ([integrator-layer.md](completed/integrator-layer.md)), `GpuRenderer`'s pipeline cache gains the class in its key, and `KernelDomain::SurfaceQueues` dispatches the variant over both queues.

### The Materials window

A **Materials** window under **Windows**:

- The library as a tree: **User** materials, then one folder per imported model. The laser optics plans add **Glasses** and **Coatings** folders, whose entries are not materials but are referenced by them ([physoptics-optical-materials.md](physoptics-optical-materials.md)), and a folder per optical element for the materials it owns.
- Selecting one shows a parameter editor generated from its traits. Color parameters get a color picker; texturable ones a texture picker once [pbrt-textures.md](pbrt-textures.md) exists.
- For a glTF material, edited parameters are marked, and **Revert** restores the imported value.
- **+ New** and **Duplicate**.

Each shape's and mesh object's panel shows a material combo with **Edit** (opens the Materials window on it) and **New from this**. `drawMaterialParams()` becomes the generic traits-driven editor.

### Scene file

The payload version this lands in writes:

```json
"materials": [
  {"name": "Red wall", "type": "lambertian", "albedo": [0.65, 0.05, 0.05]},
  ...
],
"materialOverrides": [
  {"model": 0, "material": 12, "roughness": 0.3}
],
"geometry": [{"type": "sphere", "material": 0, ...}]
```

User materials are saved in full, glTF materials only as the parameters in `edited`. An older file's inline material becomes one library entry per object, named after the object ("Sphere 3"). Identical inline materials are deliberately not merged, since merging would link objects the user edited separately.

### Light list and raster preview

`materialEmission()` and `materialPreviewColor()` read the traits instead of switching on the type. `LightListBuilder` sees no difference.

## Order of work

1. `MATERIAL_TRAITS`, `ParamSet` and the generic editor, with `SceneMaterial` still inline. The existing six types only. Bit-identical.
2. The 16-byte header and binding 26, with the six types packed into the new records. Bit-identical.
3. The library, references by id, the Materials window, and the scene-file format with the upgrade from inline materials.
4. glTF materials as library entries with overrides.
5. The two queues and the per-class builds of kernel 06. Bit-identical: the order of the two dispatches does not change any path.

## Verification

- After steps 2 and 5, the Cornell box, sphere scene, Veach scene and Sponza render bit-identical with a fixed seed under all four kernel 06 variants.
- Every file in `assets/scenes/` opens, shows the same materials, saves, and reopens with the same library.
- Editing a Sponza material, saving, and reopening re-applies the edit; **Revert** removes it and the next save drops it from `materialOverrides`.
- The per-queue path counts in the render panel add up to today's surface-queue count.

## Open questions

- Whether a library entry that no object references is kept, as a library would, or dropped on save. Keeping it is the expected behavior of a library; the Materials window then needs **Delete**.

## Sources

PBR 4ed §10.5 (the Material interface, texture evaluators and the basic/universal split), §15.3.9 (per-material kernels, and why materials are sorted by the complexity of their textures); pbrt-v4 `src/pbrt/materials.h`, `wavefront/surfscatter.cpp`.
