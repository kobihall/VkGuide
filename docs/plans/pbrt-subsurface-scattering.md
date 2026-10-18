# Subsurface scattering

Skin, marble, milk, wax and jade look soft because light enters them, scatters beneath the surface and leaves somewhere else. The tracer's materials all scatter at the point of entry. The sources describe two ways to model it: a BSSRDF, a tabulated profile of where light exits relative to where it entered (PBR 3ed §11.4 and §15.4 to §15.5; pbrt-v4's `SubsurfaceMaterial`), and a volumetric random walk through the object's interior, which production renderers adopted (Chiang et al. 2016, cited in PBR 4ed chapter 14's "Further Reading"). The user chose both, selected per material. This plan adds a `subsurface` material with the two methods, a subsurface kernel, and pbrt's table of measured scattering coefficients.

*Not implemented. Depends on [pbrt-participating-media.md](pbrt-participating-media.md) (homogeneous media, chromatic MIS, the named-media table), [pbrt-material-system.md](completed/pbrt-material-system.md) and [pbrt-bsdf-library.md](pbrt-bsdf-library.md) (the dielectric boundary). Part of the [roadmap](pbrt-roadmap.md).*

## Decision already made

On 2026-09-26 the user chose to plan both a volumetric random walk and a BSSRDF, chosen per material, over either alone.

## What the renderer does today

- Every material scatters at the hit point. The legacy `dielectric` refracts into an object and out again with nothing in between.
- There is no medium, no BSSRDF table, and no traversal query other than `traceScene()` and `occluded()` (`crt_traverse.glsl`).

## Design

### The material

A `subsurface` material ([pbrt-material-system.md](completed/pbrt-material-system.md)) with:

- **Method**, random walk or BSSRDF.
- The interior's scattering, specified one of three ways, as pbrt allows: σa and σs directly; a surface albedo (reflectance) and a mean free path per channel, inverted to σa and σs; or a named measured material.
- **Scale** for σa and σs, and an interior asymmetry `g`.
- The boundary: an index of refraction η and a roughness, evaluated by the dielectric BxDF of [pbrt-bsdf-library.md](pbrt-bsdf-library.md).
- For the random walk, a **Boundary** choice: dielectric (the physical boundary) or diffuse transmission. A diffuse boundary lets next-event estimation run where a path enters and exits, which a smooth dielectric cannot; Arnold's and Cycles' random walks offer the same choice.

The named measured materials are pbrt's table (`GetMediumScatteringProperties()` in `media.cpp`): 47 entries from Jensen et al. 2001 and Narasimhan et al. 2006, from apple, ketchup, marble and two skins to milks, coffees, wines, juices and Pacific Ocean surface water. [pbrt-participating-media.md](pbrt-participating-media.md) uses the same table for media presets.

### The subsurface kernel

Both methods are long, divergent loops that would waste the wavefront's bounce iterations if split into one dispatch per step. They get one kernel of their own:

- Kernel 06, when it samples transmission into a `subsurface` material, appends the path to queue 9 (`CRT_QUEUE_SUBSURFACE`, reserved by the roadmap) instead of the next ray queue.
- A new stage, 10 (`sample_subsurface`), drains queue 9. Its one variant, `1001_subsurface`, branches on the material's method, a branch that is uniform per material. It traces through `crt_trace.glsl`, so the build compiles it for every traversal ([integrator-layer.md](completed/integrator-layer.md)). It appends each result to the next ray queue and, where the exit point allows it, a shadow ray to the shadow queue.
- The schedule dispatches slot 10 after kernel 06 and before kernel 08, only when the scene has a `subsurface` material. pbrt-v4's wavefront runs its `SampleSubsurface()` at the same point.

Stage 10 extends the PBR Figure 15.2 numbering past 09, which the roadmap allows because it renames nothing. The path tracer's `IntegratorInfo` lists it between 06 and 08. Its random numbers come from the path's PCG stream, since the sample record's three vectors are taken.

### Random walk

After Chiang et al. 2016 and Wrenninge et al. 2017. The interior is a homogeneous medium derived from the material:

1. Sample a distance by the extinction of a lane chosen at random, with the chromatic MIS weights of [pbrt-participating-media.md](pbrt-participating-media.md).
2. Trace a ray against this object only. `crt_traverse.glsl` gains `traceInstance(instance, ray)`, which every traversal strategy implements with the instance walk it already has. Other objects inside the volume are ignored, as in Chiang's walk.
3. If the distance ends inside, scatter by Henyey–Greenstein and continue. If it reaches the boundary, evaluate the boundary from inside: a reflection continues the walk, and a transmission ends it, with the path leaving from the exit point.
4. Stop after 256 steps, or earlier by Russian roulette on the walk's throughput.

The walk takes no light samples inside, since light reaches the interior only through the boundary. With a diffuse boundary, the exit vertex takes a light sample through the diffuse transmission lobe.

The surface albedo and mean free path map to the medium's single-scattering albedo and extinction through Chiang et al. 2016's fitted inversion. The random walk is the same physics as a homogeneous medium inside a dielectric, which the general media kernels already render. It runs as one loop against one instance, so that each step does not cost a wavefront iteration.

### BSSRDF

pbrt's `TabulatedBSSRDF` (PBR 3ed §11.4.2 and §15.4 to §15.5):

- A profile table computed by photon beam diffusion (Habel et al. 2013) at startup: 100 single-scattering albedos by 64 optical radii, with the effective albedo of each row (pbrt's `BSSRDFTable` and `ComputeBeamDiffusionBSSRDF()`). It goes into binding 40, `BssrdfTables`, and the C++ side mirrors it in `rt_media`.
- A material specified by surface albedo is inverted through the table's effective albedos (pbrt's `SubsurfaceFromDiffuse()`).
- Sampling: pick a channel and one of three projection axes, sample a radius from the profile, and form a probe segment through the entry point. pbrt-v4's `IntersectOneRandom()` chooses one intersection with the same material along the probe by weighted reservoir sampling. Here it is a loop of `traceInstance()` calls with the reservoir of [pbrt-resampled-direct-lighting.md](pbrt-resampled-direct-lighting.md)'s `crt_reservoir.glsl`. Only this instance is searched, a simplification of pbrt, which searches every object with the material.
- At the chosen exit point, the BSSRDF's spatial term weighs the path, the exit lobe is pbrt's normalized Fresnel diffuse lobe, and the exit takes a light sample and continues.

A BSSRDF assumes a locally flat, semi-infinite, homogeneous slab. It is faster than a random walk in thick, highly scattering objects, and wrong in thin parts such as ears and fingers, where the random walk is right.

## Registry, bindings and data

- Slot 10 `sample_subsurface` with `1001_subsurface`; queue 9.
- `traceInstance()` joins the traversal contract in `crt_traverse.glsl`, one function per strategy file.
- Binding 40 `BssrdfTables`.
- The `subsurface` material type and its parameters; the named measured table, shared with media.
- The roadmap's slot rule records slot 10.

## Verification

- With no `subsurface` material in the scene, every scene renders bit-identical, and slot 10 is never dispatched.
- White furnace ([pbrt-verification-harness.md](pbrt-verification-harness.md)): a random-walk sphere with albedo 1 and an index-matched boundary (η = 1) under a uniform white environment is invisible.
- A random-walk sphere renders the same image as a sphere of the same homogeneous medium inside a dielectric rendered through the general media kernels. Both are the same physics.
- `bin/media_test`: the BSSRDF profile integrates to the table's effective albedo; albedo inversion round-trips; probe-radius samples match the profile's pdf (χ²).
- A thick marble slab: the BSSRDF and the random walk agree closely. A thin ear-like fin lit from behind: they differ, and the random walk matches the general media reference.

## Order of work

1. `traceInstance()` in each traversal strategy, with a `bvh_bench` check against `traceScene()` restricted to one instance.
2. The material, queue 9 and slot 10, with the random walk and a dielectric boundary.
3. The diffuse boundary and its light samples.
4. The BSSRDF table, inversion and probe sampling.
5. Named measured materials.

## Sources

PBR 3ed §11.4 (the BSSRDF, separable and tabulated BSSRDFs, subsurface materials), §15.4 (sampling subsurface reflection functions, subsurface scattering in the path tracer), §15.5 (photon beam diffusion, filling the table, setting scattering properties); PBR 4ed chapter 14 "Further Reading" (subsurface scattering); pbrt-v4 `src/pbrt/bssrdf.h`, `materials.h` (`SubsurfaceMaterial`), `media.cpp` (`GetMediumScatteringProperties()`), `wavefront/subsurface.cpp`; Chiang, Kutz and Burley 2016, *Practical and controllable subsurface scattering for production path tracing*; Wrenninge, Villemin and Hery 2017, *Path traced subsurface scattering using anisotropic phase functions and non-exponential free flights*; Habel, Christensen and Jarosz 2013, *Photon beam diffusion*; Jensen et al. 2001, *A practical model for subsurface light transport*; Narasimhan et al. 2006, *Acquiring scattering properties of participating media by dilution*.
