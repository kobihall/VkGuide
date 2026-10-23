# Beams in the raster preview

A laser beam in a real lab is invisible. In the viewport it should be a solid you can see: a tube along each segment whose radius follows the beam's 1/e² radius, narrowing to a waist after a lens and widening again, dim for a ghost and red where it clips. This plan draws the beam solver's segments that way, with the geometry the request specifies: at most three sections per segment, a single truncated cone when the segment holds no waist, and a cone, a cylinder of the waist's radius as long as the Rayleigh range, and a cone when it does.

*Not implemented. Depends on [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md). Colors use [pbrt-display-and-color.md](pbrt-display-and-color.md)'s wavelength display color when it exists. Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

- The preview's geometry follows the request: per segment, a truncated cone from `w(z₁)` to `w(z₂)` if the segment holds no waist; otherwise a cone from `w(z₁)` to `w₀`, a cylinder of radius `w₀` and length `z_R` centered on the waist, and a cone from `w₀` to `w(z₂)`.
- The beam is traced, not guessed: one chief ray per segment through the real scene, by the solver, which is the CPU mirror of the tracer's own traversal ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)).

## What the renderer does today

- `drawGeometry()` draws opaque surfaces, then transparent ones, with `MaterialPass::Transparent` through `GLTFMetallic_Roughness`'s transparent pipeline. Vertices come through the pipeline's vertex input and each instance's world matrix through a per-instance binding (`shaders/mesh.vert`); each run of one surface with one material is one instanced draw.
- Camera frustums and light markers are ImGui overlays in the foreground draw list (`drawCameraOverlays()`, `drawLightOverlays()` in `src/vk_engine.cpp`), since there is no line pipeline.
- The preview shapes rewrite a persistent buffer per frame in flight when the scene revision changes (`drawRaytraceShapes()`), with the flush MoltenVK requires.

## Design

### Sections

For each segment and each principal axis of its beam, the solver gives the waist position and radius and the Rayleigh range. For one axis, the profile along the segment from `z₁` to `z₂` is:

- **No waist inside** (the waist's position, minus and plus `z_R / 2`, lies outside `[z₁, z₂]`): one cone, `w(z₁)` to `w(z₂)`.
- **A waist inside**: a cone from `w(z₁)` at `z₁` to `w₀` at `max(z₁, z_w − z_R / 2)`, the cylinder at `w₀` up to `min(z₂, z_w + z_R / 2)`, and a cone from `w₀` to `w(z₂)` at `z₂`. Where the cylinder reaches an end of the segment, the cone on that side has zero length and is dropped.

Within the cylinder the true radius rises to `√(1 + 1/4) w₀ ≈ 1.118 w₀` at its ends, so the preview's radius is never more than 11.8 % below the beam's. **Waist region** in the panel offers the confocal parameter (the cylinder from `−z_R` to `+z_R`, the region where `w ≤ √2 w₀`) as an alternative definition, the request's length `z_R` being the default.

A stigmatic beam therefore gets at most three sections. An astigmatic beam has two profiles, one per principal axis, with their own waists. Its rings sit at the union of both profiles' break points, each ring an ellipse whose two radii are the two profiles at that position, so each axis still follows the three-section rule exactly and the segment has at most five sections. Each ring's ellipse is oriented along the beam's principal axes at that position, so a twisted beam's tube twists.

### The radius shown

The radius is the 1/e² intensity radius `w` by default, the definition femtoPro draws (it is also the 1/e field radius). **Radius** offers 1.5 w, which encloses 98.9 % of the power, for judging clearance against apertures.

### Drawing

A new pipeline, `shaders/optics/beam_preview.vert` and `.frag`, draws every section of every segment in one instanced draw:

- One unit tube mesh, two rings of 48 vertices, uploaded once.
- A storage buffer of section records in a descriptor set, indexed by `gl_InstanceIndex` with every section of a beam one instanced draw: the two ring centers, the two ring frames, the four radii, a color and an opacity. 64 bytes a section. Not a buffer device address: MoltenVK makes every device-address buffer resident again on each draw of a pipeline that declares the capability (CLAUDE.md, "No buffer device addresses in raster shaders").
- The vertex shader places each ring vertex on its ellipse. The fragment shader shades the tube as a glowing volume: its opacity scales with `1 / max(|n · v|, 0.2)`, so the tube reads as denser at its silhouette, as a lit haze does, and the color is additive.
- Additive blending, depth test `LESS_OR_EQUAL` against the opaque pass and no depth write, so optics hide the beam behind them and beams do not hide each other. It draws after the transparent pass.

The section buffer is one persistent slot per frame in flight, rewritten only when the solver's output changes, and flushed after the write as every mapped write must be on MoltenVK. A thousand segments, a large ghost tree, are about 200 KB and one draw.

### Color and brightness

- The color is the beam's wavelength as a display color: the CIE 1931 color of its spectrum, normalized and encoded to sRGB, from [pbrt-display-and-color.md](pbrt-display-and-color.md), or that plan's false color for a wavelength outside the visible range (an IR beam is drawn deep red, a UV beam violet, an XUV beam a configurable color). Until that plan lands, a small table maps 380 to 780 nm to sRGB.
- The opacity follows the segment's power on a logarithmic scale: a **Brightness range** setting, four decades by default, maps the laser's power to full opacity and a ten-thousandth of it to nothing, so a 1 % ghost is visible and a 1e-6 ghost is not.
- Segments downstream of clipping are tinted toward red by the fraction lost, and segments truncated by the solver's caps are dashed by the fragment shader.

### Annotations

Overlays in the foreground draw list, toggled in the **Beam** window:

- A label at each waist with `w₀` and `z_R` for each axis.
- The propagation distance from the laser at each interaction, as femtoPro shows it.
- Markers where the authored beam axis is not followed: a red cross where a node is missed, and the distance and direction of the miss.

### Spots

**Display** offers **Full beam** (default), **Spots only** and **Off**. **Spots only** draws, instead of tubes, the beam's footprint ellipse on every surface it meets, at the obliquity of the hit, which is what an alignment card shows in a real lab. Spots are flat disks drawn by the same pipeline with zero length.

## Registry, bindings and data

- `shaders/optics/beam_preview.vert` and `.frag`; the pipeline in `VulkanEngine` beside the mesh pipelines; `buildBeamSections()` in `src/beam_preview.h/.cpp`, the section generator, part of `rt_optics` so that `bin/beam_test` can check it.
- Settings in the **Beam** window: **Display**, **Radius**, **Waist region**, **Brightness range**, annotations. Saved with the scene as viewport settings.
- No binding: the preview is not part of the path tracer.

## Verification

- With no laser, nothing is drawn and the raster image is byte-identical (draw-image readback, the smoke-test recipe in the project memory).
- `bin/beam_test`, sections: a stigmatic segment without a waist gives one section with end radii `w(z₁)` and `w(z₂)`; with a waist, three sections whose cylinder has radius `w₀`, length `z_R` and center `z_w`, clamped at the segment's ends; an astigmatic segment gives at most five sections, and each axis's radii at every ring equal its own three-section profile.
- A lens focusing a beam shows the cone, cylinder and cone at the waist the Beam window reports; turning the lens 10° shows the two waists of the resulting astigmatism apart.
- Zero validation messages with the new pipeline.
- A thousand segments draw in under 0.5 ms of GPU time, measured at the frame level since per-dispatch timestamps read zero on this GPU.

## Order of work

1. `buildBeamSections()` for stigmatic beams, with its tests.
2. The pipeline and the instanced draw.
3. Astigmatic sections and their orientation.
4. Color, brightness, clipping tint.
5. Annotations and spots.

## Open questions

- Whether the waist cylinder's length should default to the request's `z_R` or the confocal parameter `2 z_R`. The plan follows the request and offers the other.
- Whether ghost branches should be hidden by default when an axis has more than some number of them. A tree of a thousand dim tubes is legible at four decades of brightness range; the setting may need a cap instead.

## Sources

Siegman 1986, *Lasers*, §17.1 (the Gaussian beam's radius, waist and Rayleigh range); Brixner et al. 2023, §3.4, and Brixner et al. 2025 (femtoPro's beam meshes, the minimum vertex count for a given error, and its CIE coloring); CIE 015:2018, *Colorimetry*; the project's `drawCameraOverlays()` and `drawRaytraceShapes()` patterns.
