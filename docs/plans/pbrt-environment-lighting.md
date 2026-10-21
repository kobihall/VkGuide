# Environment lighting

The environment is an equirectangular Radiance `.hdr` map, fixed in world space, sampled by a piecewise-constant distribution with pbrt's MIS compensation. That covers pbrt-v4's `ImageInfiniteLight` except for its map parameterization. pbrt adds a portal light for interiors lit through a window, and its `imgtool makesky` generates a physically based sky. This plan adds those, map rotation, the equal-area parameterization pbrt uses for every spherical image, and a sun with a finite disk.

*Not implemented. Depends on [pbrt-light-types.md](pbrt-light-types.md) (photometric units for the environment) and, for the sky's spectral form, [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md). OpenEXR maps load through [pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- `VulkanEngine::loadEnvironmentMap()` reads a `.hdr` file with `stbi_loadf()` and uploads it as rgba16f, divided by a power of two when its peak exceeds half float's range (`m_environmentMapScale`).
- `buildEnvironmentDistribution()` (`src/light_sampling.h`) tabulates luminance × sin θ at up to 2048 cells wide, plus the compensated table with the mean subtracted.
- The map's axes are the world's: u turns from −z toward +x, v runs from +y down (`equirectUvOf()`).
- A solid color can replace the map (`m_solidBackground`). BSDF samples alone find it, as in pbrt.

## Design

### Rotation

The environment gains an orientation, edited with the rotate-only gizmo from the Scene panel. Kernel 03 and `crt_light.glsl` rotate a world direction into map space before every lookup and rotate a sampled direction back. The tables stay in map space, so turning the map costs one 3 × 3 multiply per lookup and no rebuild. An identity rotation multiplies exactly, so today's scenes stay bit-identical.

### Equal-area maps

pbrt-v4 stores every spherical image, environment maps included, in Clarberg's equal-area octahedral square (PBR 4ed §3.8.3, spherical parameterizations). Every texel covers the same solid angle, so the sampling table has no sin θ factor and spends no resolution on the poles.

- A **Parameterization** field on the map: equirectangular (today's) or equal-area. A square map in an EXR file written by pbrt is equal-area.
- The GPU sampler cannot wrap across the octahedron's folded edges, so equal-area lookups filter by hand: four `texelFetch` calls with pbrt's octahedral wrap (`WrapMode::OctahedralSphere`).
- **Convert to equal-area** resamples an equirectangular map on load, as `imgtool makeequiarea` does offline.

The goniometric light ([pbrt-light-types.md](pbrt-light-types.md)), the spherical camera's equal-area output ([pbrt-cameras.md](pbrt-cameras.md)) and the sky below use the same mapping, through one `crt_equal_area.glsl` mirrored in `light_sampling.cpp`.

### Portals

In a room lit through one window, most environment samples point at walls and are wasted. pbrt-v4's `PortalImageInfiniteLight` (Bitterli et al. 2015, PBR 4ed §12.5.3) takes a quadrilateral portal and reparameterizes the environment in the portal's rectified frame, where the portal's projection seen from any point is an axis-aligned rectangle. It samples only inside that rectangle, with a summed-area table, which pbrt calls `WindowedPiecewiseConstant2D`. PBR 4ed Figure 12.19 shows the error falling by a factor of 2.82 at 16 spp.

- The portal is a quad shape marked **Portal** in the Scene panel. It is not rendered or hit.
- The summed-area table, 512² by default, goes into binding 36, `EnvironmentExtras`.
- Radiance lookups keep reading the original map, so the portal changes the sampling only. The pdf evaluation uses the same windowed table, which keeps MIS consistent.
- As in pbrt, the portal light covers only the hemisphere in front of the portal's plane, and a point behind the plane receives nothing from it. The panel says so when a portal is set.

A portal is an **Environment sampling** choice (whole map, compensated, through portal), beside the existing compensation.

### Sky and sun

pbrt-v4 ships the Hosek–Wilkie sky model (`src/ext/skymodel`, Hosek and Wilkie 2012) and uses it in `imgtool makesky` to bake a sky into an equal-area image. This plan builds the same code into the renderer:

- An **Environment** kind: map (today's) or sky.
- Sky parameters: sun elevation and azimuth, turbidity (1.7 to 10, the model's valid range), and ground albedo. A change bakes a new equal-area map on the CPU, rebuilds the tables and restarts the render.
- The RGB dataset serves exact RGB. The spectral dataset (11 bands, 320 to 720 nm) is used by spectral modes directly instead of uplifting the baked RGB.

The sun is a new light kind, `Sun`: a distant light with a finite angular radius (0.27° for the real sun), and radiance from the model's solar radiance function at the sun's elevation and turbidity. A directional light's shadows are hard; the sun's penumbrae have the real width.

- Light samples use the existing cone sampler that spheres use (`crt_light.glsl`).
- BSDF samples can hit it, unlike a delta light: kernel 03 tests an escaped ray against the sun's cone and weighs the sun's radiance with MIS.
- A sun can also be added without the sky, over any map.

The newer Prague sky model (Wilkie et al. 2021) covers more of the sky's range, including twilight. It is a later option behind the same parameters.

### Product sampling

Environment × BSDF product sampling (Conty Estevez and Lecocq 2018) samples a direction in proportion to both at once, which helps glossy surfaces under a detailed map. It needs a hierarchical representation of the map per BSDF lobe, and is research here. [pbrt-resampled-direct-lighting.md](pbrt-resampled-direct-lighting.md)'s RIS approximates it with the existing tables.

## Registry, bindings and data

- Binding 36 `EnvironmentExtras`: the portal's summed-area table and the sky's baked parameters.
- `GpuLightKind::Sun`. The environment record's padding carries the parameterization and the portal flag.
- The environment's rotation in the push constants, three `vec4`s.
- Scene file: an `"environment"` object with kind, map path, parameterization, rotation, sampling choice, portal shape id, sky parameters and sun. The loose `m_environmentIntensity`, `m_solidBackground` and `m_backgroundColor` fields move into it, and older files still read.

## Verification

- With identity rotation, an equirectangular map and whole-map sampling, every scene renders bit-identical.
- Rotating the map and the camera together by the same angle gives the same image, up to float rounding.
- One map in both parameterizations converges to the same image (**Compare to reference**, within noise).
- `bin/light_test`: the equal-area table's pdf integrates to 1 over the sphere; the portal's windowed pdf integrates to 1 over the portal's solid angle; the samples of both pass a χ² test against their pdfs.
- A room lit through one window: equal-time MSE lower with the portal than without, with pbrt's factor of 2.82 as the point of comparison.
- The baked sky equals `imgtool makesky`'s output for the same parameters to half-float precision, since both run the same code.
- The penumbra of a pole under the sun has the width its angular radius predicts.

## Order of work

1. Rotation.
2. Equal-area maps, conversion and `crt_equal_area.glsl`.
3. The sun light.
4. The sky model.
5. Portals.
6. Product sampling, as research.

## Sources

PBR 4ed §3.8.3 (equal-area mapping), §12.5 (uniform, image and portal image infinite lights), §A.5.5 and §A.5.6 (piecewise-constant and windowed 2D distributions, summed-area tables), chapter 12 "Further Reading" (environment maps, sky models); Clarberg 2008, *Fast equal-area mapping of the (hemi)sphere using SIMD*; Bitterli et al. 2015, *Portal-masked environment map sampling*; Hosek and Wilkie 2012, *An analytic model for full spectral sky-dome radiance*; Wilkie et al. 2021, *A fitted radiance and attenuation model for realistic atmospheres*; Conty Estevez and Lecocq 2018, *Fast product importance sampling of environment maps*; pbrt-v4 `src/pbrt/lights.cpp`, `util/sampling.h`, `cmd/imgtool.cpp` (`makesky`, `makeequiarea`).
