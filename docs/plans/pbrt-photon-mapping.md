# Stochastic progressive photon mapping

Some light paths no unbiased method samples well. A caustic seen in a mirror, a pool floor lit through water and viewed through the water, is a specular–diffuse–specular path: neither the camera side nor the light side can connect across the diffuse vertex, so path tracing and BDPT converge on it very slowly. Photon mapping estimates the light at diffuse points from photons deposited nearby, which trades a bias that vanishes over time for robustness on those paths. pbrt 3's `SPPMIntegrator` implements stochastic progressive photon mapping (Hachisuka and Jensen 2009; PBR 3ed §16.2): each iteration traces camera paths to visible points, traces photons from the lights, and shrinks each pixel's gathering radius so that the estimate is consistent. This plan adds it as an integrator.

*Not implemented. Depends on the light-tracing stages of [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- One integrator, the path tracer. Nothing stores points for density estimation, and nothing traces photons.
- Float buffer atomics are available (`shaderBufferFloat32AtomicAdd`); image float atomics are not.

## Design

### One iteration

pbrt's algorithm, with its three passes built from the stage library ([integrator-layer.md](completed/integrator-layer.md)): the path tracer's stages, `light`'s, and three of `sppm`'s own in the roadmap's range 19 to 21.

1. In the camera pass, camera paths run through the path tracer's 00 to 04 and 08. At each vertex they add emitted light and a light sample to the pixel's direct estimate `Ld`, as the path tracer does. Stage 19, store visible points, replaces kernel 06: it includes `crt_surface.glsl` for the light sample, continues specular vertices, and makes the first non-specular vertex the pixel's *visible point*: position, outgoing direction, the thin hit record from which the BSDF can be rebuilt, and the path throughput `β`. The path then ends.
2. The grid build inserts every visible point's bounding sphere, of the pixel's current radius, into a hash grid over the scene: a count pass with atomics, a prefix sum, and a fill pass, giving compact per-cell lists. pbrt builds lock-free linked lists instead; compact lists read better on the GPU. The cell size is the largest radius, so a photon needs to look in one cell only.
3. In the photon pass, photons leave the lights through `light`'s stage 11, choosing lights by power, and trace through the path tracer's 01 and 02. At each photon–surface hit after the first, which the camera pass's direct estimate already counts, stage 20, deposit photons, looks up the hit's cell. For each visible point there within its radius, it adds `β_photon · f(wo, wi)` to that pixel's flux `Φ` and increments its photon count `M`, both with atomics. Then the photon scatters by the adjoint BSDF in `light`'s stage 13 and continues, with Russian roulette.
4. Stage 21, update photon statistics, applies pbrt's rule per pixel, with γ = 2/3: `N' = N + γM`, `r' = r √(N' / (N + M))`, `τ' = (τ + β_visible Φ) r'² / r²`; then `Φ` and `M` reset.

The pixel's estimate after `k` iterations is `Ld / k + τ / (k N_p π r²)`, where `N_p` is the photons emitted per iteration. Stage 21 writes it into the accumulation image each iteration, in place of kernel 09, so the display, **Compare to reference** and export work unchanged.

### Settings and memory

- **Photons per iteration**, by default the pixel count, as pbrt's `photonsperiteration = -1` means.
- **Initial radius**, in world units, default 1 as in pbrt. Too small gives noise, too large blurs; the panel suggests one from the scene's size.
- Iterations are the render's sample count.
- Visible points and per-pixel state (`Ld`, `N`, `r`, `τ`, `Φ`, `M`): about 96 bytes per pixel, in binding 54, `VisiblePoints` (new; the roadmap's binding table gains it). The grid's cell table and lists go into binding 43, `PhotonGrid`. Both are sized by the pixel count, 250 to 300 MB at 1920 × 1080.

### Shared with VCM

The hash grid over points with radii is the same structure vertex merging needs. [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md)'s `lvc` integrator can reuse it to merge camera vertices with cached light vertices, which is VCM (Georgiev et al. 2012; Hachisuka et al. 2012).

### Later

From PBR 3ed's chapter 16 exercises:

- Final gathering (exercise 16.8): follow camera paths one bounce further past the first diffuse vertex before storing the visible point, which smooths the low-frequency error.
- Adapting photon emission toward directions that reach visible points (16.5), or Hachisuka and Jensen 2011's Markov chain photon tracing (16.11), for scenes where the camera sees a small part of the lit world.
- Volumetric photon mapping (16.10; Knaus and Zwicker 2011), after [pbrt-participating-media.md](pbrt-participating-media.md).

## Registry, bindings and data

- Integrator `sppm` ([integrator-layer.md](completed/integrator-layer.md)), running the path tracer's 00 to 04 and 08, `light`'s 11 and 13, and stages 19 to 21, whose shaders sit in `shaders/rt/` with the rest of the library.
- The grid build's count, prefix sum and fill are plain compute passes in `shaders/rt/sppm/`, since nobody swaps them.
- Bindings 43 `PhotonGrid` and 54 `VisiblePoints`.
- Settings: **Photons per iteration**, **Initial radius**.

## Verification

- The path tracer renders bit-identical.
- A C++ test of the radius and flux update against pbrt's code on a fixed sequence of photon counts.
- On the Cornell box, SPPM converges toward the path tracer's reference as iterations grow: the relative error keeps falling, and its bias at a fixed iteration count shrinks with the initial radius.
- A caustic seen in a mirror (a glass sphere lighting a floor, viewed in a mirror): at equal time, SPPM's error falls where `path`, `lvc` and `bdpt` barely improve. The scene goes into `assets/scenes/`.

## Order of work

1. The camera pass and visible points, with `Ld` alone as the image.
2. The grid build.
3. The photon pass and deposits.
4. The update and the estimate.
5. Final gathering and the grid's reuse for VCM, later.

## Sources

PBR 3ed §16.2 (the theory of particle tracing, photon mapping, progressive and stochastic progressive photon mapping, `SPPMIntegrator`, visible points, the grid, photon contributions) and exercises 16.5, 16.8, 16.10 and 16.11; PBR 4ed chapter 13 "Further Reading" (photon mapping, VCM and UPS); Jensen 1996, *Global illumination using photon maps*; Hachisuka, Ogaki and Jensen 2008, *Progressive photon mapping*; Hachisuka and Jensen 2009, *Stochastic progressive photon mapping*; Knaus and Zwicker 2011, *Progressive photon mapping: a probabilistic approach*; Hachisuka and Jensen 2011, *Robust adaptive photon tracing using photon path visibility*; Georgiev et al. 2012; Hachisuka, Pantaleoni and Jensen 2012, *A path space extension for robust light transport simulation*; pbrt-v4 `src/pbrt/cpu/integrators.cpp` (`SPPMIntegrator`).
