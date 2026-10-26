# Light types

The tracer has pbrt's point, spot and distant lights, diffuse area emission from shapes and triangles, and an environment map. pbrt-v4 has nine light types; the missing ones are the projection light and the goniometric light. Its area lights can be one-sided and can carry an image, and its lights can be specified by the power or illuminance they deliver rather than by a raw scale. This plan adds those, and gives every light the emission-sampling functions that light tracing, BDPT and photon mapping need.

*Not implemented. Depends on [pbrt-textures.md](pbrt-textures.md) (images for projection, goniometric and textured emission), [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) (shape uvs) and [pbrt-material-system.md](completed/pbrt-material-system.md) (emission parameters). Color temperature comes from [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- `SceneLight` (`src/rt_scene_types.h`) is a point, spot or directional light with a linear RGB color and an intensity in W/sr (point, spot) or W/m² (directional). A glTF import divides candela and lux by 683.
- The spot falloff is glTF's: `t²` with `t` linear in the cosine between the outer and inner cones (`spotFalloff()` in `crt_light.glsl`). pbrt uses a smoothstep over the same range.
- Every emitting surface emits from both sides (`rt_lights.cpp`, and the comment at the top of `crt_shape.glsl`).
- Emissive glTF triangles read their emissive texture, and cut-outs emit nothing (`triangleLightRadiance()`). Shapes have no uvs, so shape emission is uniform.
- The environment joins the light list only when it is a map. A solid background is reached by BSDF samples alone, which is what pbrt-v4 does too: `UniformInfiniteLight::SampleLi()` returns no sample when the integrator allows an incomplete pdf, because BSDF sampling already covers a constant background well. This plan keeps that.
- Nothing can sample an emitted ray (a point on a light and a direction leaving it), which every method that starts paths at lights needs.

## Design

### Projection light

A point light that projects an image through a frustum, like a slide projector (PBR 4ed §12.2.2): the intensity in a direction is the image's value where that direction pierces the image plane, times a scale. `SceneLight` gains the kind with an image (a `TexRef`, [pbrt-textures.md](pbrt-textures.md)) and a field of view. It is a delta light, so next-event estimation needs no sampling, only the image lookup. Its power, which the light selection needs, is the image integrated over the frustum's solid angle, computed on the CPU when the light list is built.

### Goniometric light

A point light whose intensity varies with direction according to a measured distribution (PBR 4ed §12.2.3), the way real luminaires are described. pbrt-v4 stores the distribution as an equal-area square image in the light's space ([pbrt-environment-lighting.md](pbrt-environment-lighting.md) adds the same mapping for environment maps).

Luminaire manufacturers publish their distributions as IES LM-63 files, which pbrt does not read. A loader in `src/ies_loader.cpp` parses the vertical and horizontal angle tables and the candela values, expands the file's symmetry (a single horizontal angle, a quarter, a half or the full circle), and resamples it into a 256² equal-area image. The candela values are divided by 683, as imported glTF lights are.

### Spot falloff

A **Falloff** choice on spot lights: glTF (today's, the default and the only one glTF files use) or pbrt's smoothstep. The two differ in the transition's shape only, not its extent.

### Area emission

Emission becomes part of the material ([pbrt-material-system.md](completed/pbrt-material-system.md)) with these parameters:

- **Emission** as a `SpectrumRef`, and a **Strength**.
- **Emission texture** as a `TexRef`. A shape's emission reads it through the shape's uvs. The light list prices a textured emitter by its texture's mean, as it does for triangles today.
- **Two-sided**. pbrt's area lights emit on the side their normal faces unless `twosided` is set. Existing scenes stay two-sided. A one-sided emitter emits nothing to a point behind it, whether that point's light sample or a BSDF sample finds it, and its power halves. A shape's **Reverse orientation** flips which side that is.

glTF has no emission sidedness: a material's `doubleSided` flag only says whether back faces are culled. The importer keeps glTF emission two-sided, as today, and **Two-sided** is the override.

### Photometric specification

Each light can be specified in the units a lighting designer uses. The stored value stays radiometric, so rendering is unchanged, and the UI converts:

| Light | Specified by | pbrt parameter |
|---|---|---|
| Point, spot, projection, goniometric | luminous intensity (cd) or luminous power (lm) | `power` |
| Directional | illuminance at a surface facing it (lux) | `illuminance` |
| Area | luminous power (lm): the radiance follows from the area, the sidedness and the texture's mean | `power` |
| Environment | illuminance on an upward-facing surface (lux) | `illuminance` |

For a spot light, pbrt's power-to-intensity conversion uses the cone's solid angle adjusted for the falloff (`SpotLight::Phi()`): each falloff choice has its own.

A **Color temperature** option replaces a light's color with a normalized blackbody (PBR 4ed §4.4.1). In exact RGB it is the blackbody's RGB at luminance 1; in spectral modes it is the `Blackbody` `SpectrumRef` itself. [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md) built this on 2026-10-23, as the **Spectral** menu beside every light's and emitter's color, with the other spectrum kinds, so the new light types only need to take a `SpectrumRef` color.

### Emission sampling

`crt_light.glsl` gains, for every kind, the two functions pbrt calls `SampleLe()` and `PDF_Le()`: sample a ray leaving the light (a point on it and a direction from it), and return the positional and directional densities of a given one (PBR 3ed §16.1.2; the 4th edition moved this material online):

| Kind | Position | Direction |
|---|---|---|
| Point, goniometric | the point | uniform sphere; goniometric by its image's distribution |
| Spot | the point | uniform in the outer cone |
| Projection | the point | by the image's distribution over the frustum |
| Directional | uniform on a disk the size of the scene's bounding sphere, facing the light | the light's direction |
| Sphere, rectangle, cylinder, triangle | uniform by area | cosine-weighted about the normal; for two-sided emission, a side picked with probability ½ |
| Environment | uniform on a disk the size of the scene's bounding sphere | by the map's distribution, as `SampleLi()` already does |
| Laser ([physoptics-laser-sources.md](physoptics-laser-sources.md)) | Gaussian over the beam's waist, carried to the output aperture | Gaussian about the axis with the far-field divergence, the beam's Wigner distribution |

The path tracer never calls them, and next-event estimation never samples a laser, whose emission is confined to a cone of milliradians: lasers stay out of the alias tables, and only light paths start at them. [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md), [pbrt-photon-mapping.md](pbrt-photon-mapping.md) and [pbrt-metropolis-light-transport.md](pbrt-metropolis-light-transport.md) do, and this plan gives them one tested implementation instead of three.

The C++ mirrors go into `light_sampling.cpp`, and `bin/light_test` gains a check per kind: the emitted-ray densities integrate to 1, and the sampled power matches the light's `power`.

## Registry, bindings and data

- `LightKind` and `GpuLightKind` gain `Projection` and `Goniometric`. `GpuLight`'s three padding words carry the image's texture index and a flags word (one-sided, falloff choice).
- Light images live in the texture array, binding 27 ([pbrt-textures.md](pbrt-textures.md)). No new binding.
- Scene file: the new kinds and their images, IES paths, each light's specification units, falloff, color temperature and sidedness.

## Importing pbrt scenes

[completed/pbrt-scene-import.md](completed/pbrt-scene-import.md) imports pbrt-v4 scenes today and substitutes for this plan's features. Each substitution is a line in the import report, so `./bin/pbrt_test --report <file.pbrt>` lists the scenes that need this plan. When this plan lands, change the importer as follows, then run `tools/verify/pbrt_compare.py` on the scenes named below.

- **One-sided area lights.** pbrt's `diffuse` area light emits from the front only unless `"bool twosided" true`; 2405 of the 2688 in `pbrt-v4-scenes` are one-sided. The importer makes every emitter a two-sided `Emissive` material and reports it (`areaLightRadiance()`). In `ganesha` two lights face the statue from either side, and their backs light the room behind them, which pbrt leaves dark. To restore pbrt's behavior: add a one-sided flag to the emissive material (a `CRT_MATERIAL_FLAG_*` bit packed by `packClassic()`), make kernel 04 return no emission when the ray arrives at the back (`frontFace` false), make `sampleLight()` give zero for a point whose normal faces away from the shading point, and halve the light list's power for a one-sided emitter (`LightListBuilder`). Then set the flag in `shapeMaterial()` from `twosided`, flip it with `PbrtShape::reverseOrientation`, and remove the report. With the flag off, existing scenes must stay bit-identical.
- **An emitter's own reflection.** pbrt's area light is a property of a shape whose material still reflects; the `Emissive` type absorbs. `shapeMaterial()` reports "an emitter's own reflectance is dropped" when the shape's material is not black (`crown`, `book`, `ganesha`, `pavilion-night`). An emission parameter on `lambertian` and `pbr` (pbr has one, which does not end the path) would remove the substitution: have `shapeMaterial()` keep the resolved material and set its emission instead of switching to `Emissive`.
- **Spot falloff.** pbrt's spot light falls off as `smoothstep(cosTotalWidth, cosFalloffStart, cos)`; `spotFalloff()` in `crt_light.glsl` squares a linear ramp. `lights()` reports it (four spots in `villa-lights-on`). Add pbrt's falloff as a per-light choice and select it on import.
- **Lights the importer approximates.** `goniometric` and `projection` lights become point lights with their `I` and `scale` and lose their image. Area lights with a `filename` lose their image and keep `L`. Map them to this plan's goniometric, projection and textured area lights. No scene in `pbrt-v4-scenes` uses them, so a comparison needs a hand-written one.
- **Photometric units.** `power` on point, spot and area lights and `illuminance` on distant lights are converted with pbrt's formulas (`lights()`, `areaLightRadiance()`). Keep them when this plan's photometric units arrive; they are the comparison's check that both agree.

## Verification

- With no new kind or option in use, every scene renders bit-identical.
- `bin/light_test` passes for every kind, including the emitted-ray densities.
- A goniometric light built from an isotropic IES file (all candela values equal) renders the same image as a point light of the same intensity.
- A projection light with a white image and a 90° field of view lights a wall with the same irradiance as a point light of equal intensity inside the frustum, and nothing outside it.
- A one-sided emissive quad facing away from the camera is black from behind, and lights nothing behind it under every direct-lighting strategy.
- A 1000 lm point light and a 1000 lm spot light deliver the same total power to an enclosing sphere of diffuse white, measured as its mean radiance.

## Order of work

1. Two-sided flag, reverse orientation and the photometric UI. They need no images.
2. The smoothstep falloff and color temperature.
3. Emission textures on shapes, once shapes have uvs.
4. Projection and goniometric lights, and the IES loader.
5. Emission sampling, when the first bidirectional integrator starts.

## Sources

PBR 4ed §4.4 (blackbody emitters, standard illuminants), §12.1 (the light interface, photometric specification), §12.2 (point, spot, projection and goniometric lights), §12.3 (distant lights), §12.4 (area lights: `twosided`, images, alpha), §12.5.1 (the uniform infinite light and incomplete pdfs); PBR 3ed §16.1.2 (`Sample_Le` and `Pdf_Le` per light); pbrt-v4 `src/pbrt/lights.h` and `lights.cpp`; ANSI/IES LM-63-2002, *Standard file format for the electronic transfer of photometric data*; Khronos `KHR_lights_punctual` (spot falloff).
