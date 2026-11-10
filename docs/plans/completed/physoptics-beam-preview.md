# Beams in the raster preview

A laser beam in a real lab is invisible. In the viewport it should be a solid you can see: a tube along each segment whose radius follows the beam's 1/e² radius, narrowing to a waist after a lens and widening again, dim for a ghost and red where it clips. This plan draws the beam solver's segments that way, with the geometry the request specifies: at most three sections per segment, a single truncated cone when the segment holds no waist, and a cone, a cylinder of the waist's radius as long as the Rayleigh range, and a cone when it does.

*Implemented 2026-11-07, apart from what "Not built" lists; "As built" says where the code differs from the design below. Depends on [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md). Colors use `wavelengthDisplayColor()` (`src/spectral.h`), which the laser's overlay arrow already uses. Part of the [laser optics plan](../physoptics-laser-optics.md).*

## Decisions already made

- The preview's geometry follows the request: per segment, a truncated cone from `w(z₁)` to `w(z₂)` if the segment holds no waist; otherwise a cone from `w(z₁)` to `w₀`, a cylinder of radius `w₀` and length `z_R` centered on the waist, and a cone from `w₀` to `w(z₂)`.
- The beam is traced, not guessed: one chief ray per segment through the optics' real surfaces, by the solver ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)), which sees the optics alone (the user, 2026-11-07).
- On 2026-11-07 the user asked for this plan to be implemented whole with the solver, its settings' layout in the Beam window included.

## What the renderer does today

- `drawGeometry()` draws opaque surfaces, then transparent ones, with `MaterialPass::Transparent` through `GLTFMetallic_Roughness`'s transparent pipeline. Vertices come through the pipeline's vertex input and each instance's world matrix through a per-instance binding (`shaders/mesh.vert`); each run of one surface with one material is one instanced draw.
- Camera frustums and light markers are ImGui overlays in the Viewport window's draw list (`drawCameraOverlays()`, `drawLightOverlays()` in `src/vk_engine.cpp`), drawn through `ViewportView` (`src/viewport_view.h`), since there is no line pipeline. A capability's overlays come through its `viewportOverlays` hook, as Optical bench's lasers, detectors and beam axes do (`RaytraceSceneEditor::drawBeamAxisOverlays()`).
- The raster pass is reverse-Z since 2026-10-30: the depth buffer clears to 0 and every pipeline tests `VK_COMPARE_OP_GREATER_OR_EQUAL` (CLAUDE.md, "Depth testing").
- `draw()` skips the raster pass and keeps the last picture while `VulkanEngine::rasterKey()` holds still (**View** → **Pause Raster Preview**). Whatever changes the raster picture goes into `RasterKey`.
- The preview shapes rewrite a persistent buffer per frame in flight when the scene revision changes (`drawRaytraceShapes()`), with the flush MoltenVK requires.

## Design

### Sections

For each segment and each principal axis of its beam, the solver gives the waist position and radius and the Rayleigh range. For one axis, the profile along the segment from `z₁` to `z₂` is:

- **No waist inside** (the waist's position, minus and plus `z_R / 2`, lies outside `[z₁, z₂]`): one cone, `w(z₁)` to `w(z₂)`.
- **A waist inside**: a cone from `w(z₁)` at `z₁` to `w₀` at `max(z₁, z_w − z_R / 2)`, the cylinder at `w₀` up to `min(z₂, z_w + z_R / 2)`, and a cone from `w₀` to `w(z₂)` at `z₂`. Where the cylinder reaches an end of the segment, the cone on that side has zero length and is dropped.

Within the cylinder the true radius rises to `√(1 + 1/4) w₀ ≈ 1.118 w₀` at its ends, so the preview's radius is never more than 11.8 % below the beam's. **Waist region** in the panel offers the confocal parameter (the cylinder from `−z_R` to `+z_R`, the region where `w ≤ √2 w₀`) as an alternative definition, the request's length `z_R` being the default.

A stigmatic beam therefore gets at most three sections. An astigmatic beam has two profiles, one per principal axis, with their own waists. Its rings sit at the union of both profiles' break points, each ring an ellipse whose two radii are the two profiles at that position, so each axis still follows the three-section rule exactly and the segment has at most five sections. Each ring's ellipse is oriented along the beam's principal axes at that position, so a twisted beam's tube twists.

### The radius shown

The radius is 1.5 w by default since 2026-11-09. It was the 1/e² intensity radius `w`, the definition femtoPro draws (it is also the 1/e field radius). 1.5 w encloses 98.9 % of the power, for judging clearance against apertures, and **Radius** still offers w.

### Drawing

A new pipeline, `shaders/optics/beam_preview.vert` and `.frag`, draws every section of every segment in one instanced draw, in weighted blended order-independent transparency:

- No mesh: the vertex shader makes the tube from `gl_VertexIndex`, 48 steps around, two triangles a step between the two rings, 288 vertices a section.
- A storage buffer of section records (`BeamSection`, `src/beam_preview.h`) in descriptor set 1, indexed by `gl_InstanceIndex`, every section of every beam one instanced draw: each ring's center and the two semi-axis vectors of its ellipse, which hold the radii and the orientation, and a color and an opacity. 112 bytes a section. Not a buffer device address: MoltenVK makes every device-address buffer resident again on each draw of a pipeline that declares the capability (CLAUDE.md, "No buffer device addresses in raster shaders").
- The vertex shader places each ring vertex on its ellipse, widened to at least 1.5 pixels in radius on screen so a distant beam stays a continuous line, and lowers the opacity by as much as it widened the ring.
- The fragment shader keeps each tube's near wall, or its far wall while the eye is inside the tube, so a tube covers a pixel once. Its opacity is the beam's Gaussian irradiance integrated along the view ray through the wall (`beamGlow()`, `shaders/optics/beam_common.glsl`): highest along the axis, falling to `exp(-2)` of that at the 1/e² radius, and up to four times higher looking down the beam. It lights the wall from the eye, so a nearer beam's shading covers a farther one's.
- Weighted blended order-independent transparency (McGuire and Bavoil 2013): each fragment adds its premultiplied color and its alpha into one target, weighted by `(d / z)^8` clamped to 0.01 to 3000, where `z` is its distance in front of the eye and `d` the orbit pivot's, and multiplies the other target, the revealage, by `1 - alpha`. A nearer fragment weighs more, so where beams cross the nearer one shows on top, and the order they are drawn in does not matter.
- Depth test `GREATER_OR_EQUAL` against the raster pass (reverse-Z) and no depth write, so optics hide the beam behind them. The beams draw in a rendering of their own after `drawGeometry()`'s (`VulkanEngine::drawBeamPreview()`), into two rgba16f targets the draw image's size (`m_beamPreviewAccum`, `m_beamPreviewReveal`), and `shaders/optics/beam_preview_composite.comp` lays their weighted average color over the draw image by `1 -` the revealage, before the draw image is sRGB-encoded. The two targets blend differently, which needs the device's `independentBlend` feature (`PipelineBuilder::setColorAttachments()`).
- The sections' revision (`VulkanEngine::m_beamSectionsRevision`, which moves when the solver's revision or the preview's settings do) is in `RasterKey`, so a paused preview redraws once with the new beam.

The section buffer is one persistent slot per frame in flight (`FrameData::beamSections`), rewritten only when the sections changed since that slot last wrote them, and flushed after the write as every mapped write must be on MoltenVK. A thousand segments of three sections are about 340 KB and one draw.

### Color and brightness

- The color is the beam's wavelength as a display color: `wavelengthDisplayColor()` (`src/spectral.h`), which `LaserBeam::rgb` already carries and the laser's overlay arrow draws in, and which shows a line outside the visible range in the display's false colors. [pbrt-display-and-color.md](../pbrt-display-and-color.md) may later make the out-of-range colors configurable (an XUV beam's).
- The opacity follows the segment's power on a logarithmic scale: a **Brightness range** setting, four decades by default, maps the laser's power to full opacity and a ten-thousandth of it to nothing, so a 1 % ghost is visible and a 1e-6 ghost is not.
- Segments downstream of clipping are tinted toward red by the square root of the fraction lost, so a percent shows as a tenth of the way, and a segment whose branch the solver's caps cut off is dashed by the fragment shader, 4 mm on and 4 mm off.

### Annotations

Overlays in the Viewport through Optical bench's `viewportOverlays` hook, beside the beam axes' arrows (`RaytraceSceneEditor::drawBeamAxisOverlays()`), toggled in **Optics** → **Beam Preview**, on the beams' main branches:

- A label at each waist with `w₀` and `z_R`, one for a round beam and one per axis for an astigmatic one (**waist labels**).
- The propagation distance from the laser to each optic, as femtoPro shows it (**distances**). A spherical lens's label sits at the surface or plane its **origin at** setting chooses.
- Markers where the authored beam axis is not followed: a red cross where the beam crosses a missed visit's face plane, the distance, and a line toward the aperture's center (**misses**, on by default).

### Spots

**Display** offers **Full beam** (default), **Spots only** and **Off**. **Spots only** draws, instead of tubes, the beam's footprint ellipse on every surface it meets, at the obliquity of the hit, which is what an alignment card shows in a real lab. Spots are flat disks drawn by the same pipeline with zero length.

## Registry, bindings and data

- `shaders/optics/beam_preview.vert` and `.frag`, with `beam_common.glsl` (the push constants and the Gaussian profile) and `beam_preview_composite.comp`; the pipeline (`VulkanEngine::initBeamPreviewPipeline()`, `m_beamPreviewPipeline`) beside the occlusion box pipeline; `beamRings()` and `buildBeamSections()` in `src/beam_preview.h/.cpp`, part of `rt_optics` so that `bin/beam_test` can check them.
- Settings (`BeamPreviewSettings`) in **Optics** → **Beam Preview** (in the **Beam** window until 2026-11-08): **display**, **radius**, **waist region**, **brightness range**, and the annotations. The editor holds them (`RaytraceSceneEditor::beamPreview()`), and the scene file saves them in `"viewport"` `"beamPreview"`, each when it is not the default: payload version 33.
- No binding: the preview is not part of the path tracer.

## Verification

Done on 2026-11-07:

- `bin/beam_test`, sections: a stigmatic segment whose waist is 0.1 m before it gives one section, its ends `w(z₁)` and `w(z₂)`; a waist inside gives cone, cylinder and cone, the cylinder at `w₀` from 0.08 to 0.12 m for a waist at 0.1 m and `z_R` = 0.04 m; a waist near the start gives a cylinder from the segment's start and one cone; an astigmatic segment with its waists at 0.1 and 0.2 m gives at most six rings, each axis's radius at every ring equal to its own three-section profile to 1e-12.
- A smoke run of `beam_axis_setup` in the app, with validation layers on: zero validation messages; the draw image shows the beam as a tube along every segment, hidden behind the splitter's mount, its ghosts faint; **Spots only** draws the footprints and **1.5 w** a wider tube, each redrawn while the preview was otherwise paused.
- The solve and the section build run in 0.13 to 0.2 ms a frame on that scene.

Still to check:

- With no laser, the raster image byte-identical: nothing is drawn when there are no sections, so it holds by construction, but no readback compared it.
- A lens turned 10 degrees showing its two waists apart, by eye.
- A thousand segments in under 0.5 ms of GPU time.

## Order of work

1. ~~`buildBeamSections()` for stigmatic beams, with its tests.~~
2. ~~The pipeline and the instanced draw.~~
3. ~~Astigmatic sections and their orientation.~~
4. ~~Color, brightness, clipping tint.~~
5. ~~Annotations and spots.~~

All done on 2026-11-07.

## Open questions

- Whether the waist cylinder's length should default to the request's `z_R` or the confocal parameter `2 z_R`. The plan follows the request and offers the other.
- Whether ghost branches should be hidden by default when an axis has more than some number of them. A tree of a thousand dim tubes is legible at four decades of brightness range; the setting may need a cap instead.

## As built

- Changed on 2026-11-08, after the user's test:
    - The settings are **Optics** → **Beam Preview** (`RaytraceSceneEditor::drawBeamPreviewMenu()`), not the **Beam** window, which keeps the results.
    - One picture of the beams at a time, in this order: the preview (**Full beam** or **Spots only**), else the beam axes' arrows (**Off**), else each laser's output arrow when the scene has no axis. A miss's marker shows with either.
    - **Ghosts** (`BeamPreviewSettings::ghosts`, off by default) adds the ghost branches; off, the preview draws each laser's main beam. A clipped ghost keeps its Gaussian size, which in `assets/scenes/optics/laser_beam_expander.gltf` reaches 15 cm across through 1 and 2 in lenses. A branch inside an optic is a ghost as soon as its events can no longer make a port (`opticPortPrefix()`), so a lens's internal reflections are not drawn as the main beam.
    - Added 2026-11-09: **Full beam** draws the footprint as a disk with a Gaussian falloff on each surface a beam lands on and at each laser's output, and a tube's glow stops at a surface no drawn part of the beam crosses. The integral along the view ray is truncated there with an `erf` approximation (Abramowitz and Stegun 7.1.27). Without the truncation the bright core ran to the far edge of the footprint, and the two legs at a mirror read as an X. Disks alone left the X in place, since a disk can only add coverage.
    - A segment's first and last rings lie on the surfaces it starts and ends on: the beam's ellipse carried along the beam onto the surface's tangent plane, so tubes meet a mirror at its face. Not at grazing incidence (`|n · d| < 0.05`), where the footprint runs away. The vertex shader takes each vertex's normal from the tube's geometry, the cross of the way around its ring and the way along the tube, and the pipeline is biased toward the camera by 2 steps and a slope of 0.5, a step beyond the optics'.

- No tube mesh is uploaded: the vertex shader makes the rings from `gl_VertexIndex`, which is the same geometry with nothing to bind.
- A section is 112 bytes, not 64: each ring is a center and two semi-axis vectors, which carry the radii, the orientation and a spot's footprint at its obliquity alike.
- The defaults the plan left open: the waist cylinder is `z_R` long (the request's), and ghosts are not capped; the brightness range hides those below its four decades.

## Not built

- Nothing of the plan. Out-of-range colors stay the display's defaults until [pbrt-display-and-color.md](../pbrt-display-and-color.md) makes them configurable.

## Sources

Siegman 1986, *Lasers*, §17.1 (the Gaussian beam's radius, waist and Rayleigh range); McGuire and Bavoil 2013, "Weighted Blended Order-Independent Transparency", *Journal of Computer Graphics Techniques* 2(2) (the blend and its depth weights); Brixner et al. 2023, §3.4, and Brixner et al. 2025 (femtoPro's beam meshes, the minimum vertex count for a given error, and its CIE coloring); CIE 015:2018, *Colorimetry*; the project's `drawCameraOverlays()` and `drawRaytraceShapes()` patterns.
