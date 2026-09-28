# Procedural textures

**This plan is incomplete.** On 2026-09-26 the user said procedural textures are not a priority and left their representation undecided. This file records what the sources describe and the options on the table, so that the decision can be made later without re-reading the books. Do not implement it until the open question below is settled.

*Not implemented. Depends on [textures.md](textures.md), whose `TexRef.kind` is where procedural kinds plug in. Part of the [roadmap](roadmap.md).*

## What the sources describe

Textures that compute their value instead of reading an image:

| Texture | Source | What it is |
|---|---|---|
| Checkerboard, 2D | PBR 3ed §10.5.2, pbrt-v4 | two textures alternating in (s, t), with a closed-form box-filtered antialiased version |
| Checkerboard, 3D | PBR 3ed §10.5.3, RTNW "Solid Textures" | alternating in object or world space: an object looks carved from cubes |
| Bilinear | PBR 3ed §10.3.4, pbrt-v4 | four values interpolated across (s, t) |
| Dots | PBR 3ed §10.6.2, pbrt-v4 | randomly placed polka dots, one chance in two per cell |
| Perlin noise | PBR 3ed §10.6.1, RTNW "Perlin Noise" | band-limited gradient noise on a hashed lattice, with Hermite smoothing |
| fBm and turbulence | PBR 3ed §10.6.3, RTNW | sums of noise octaves; turbulence sums absolute values |
| Wrinkled, bumpy | PBR 3ed §10.6.4, pbrt-v4 | turbulence and fBm as bump-map displacements |
| Windy | PBR 3ed §10.6.5, pbrt-v4 | fBm waves scaled by a lower-frequency wind strength |
| Marble | PBR 3ed §10.6.6, RTNW "Adjusting the Phase" | noise-perturbed strata looked up in a colour spline |
| Scale, mix, direction mix | PBR 4ed §10.3.2 to §10.3.3 | combine other textures by a factor, a blend texture, or the normal's angle to a direction |
| Worley (cellular) noise | PBR 3ed and 4ed exercises | distance to the nearest feature point |

Antialiasing is part of each: a procedural texture should remove frequencies above what the footprint can represent (PBR 4ed §10.1.4), and the checkerboard and noise idioms show how, clamping octaves by the ray-cone width that [textures.md](textures.md) provides.

## The undecided representation

The user was offered three options:

- A texture node graph: every material parameter is a constant or a node in a node buffer (`TextureNodes`, binding 35 in the roadmap), and a bounded-stack interpreter in GLSL evaluates it. Most expressive, and it matches pbrt, where textures take other textures as inputs. Needs a node editor in the Materials window, and the complex material queue absorbs its divergence.
- Fixed slots: each parameter is a constant, an image or one parameterised procedural generator, with no composition. Simpler to edit and to evaluate, and it cannot express marble over a checkerboard.
- Baking to images on the CPU: render each procedural into an image texture at a chosen resolution. The render path needs nothing new, but resolution limits it and solid 3D textures are impossible.

## What does not depend on the decision

Whichever representation wins, these are shared and can be written in advance if another plan needs them:

- `crt_noise.glsl` with Perlin noise, fBm and turbulence, mirrored in `rt_texture` for testing, following PBR 3ed §10.6 and Kensler's permutation-free hashing so no permutation table has to be uploaded.
- The 3D texture mapping (object or world space) in `TexRef.mapping`, which [textures.md](textures.md) reserves.

## Sources

PBR 3ed §10.3.4, §10.5 and §10.6 (procedural and solid textures, noise, polka dots, spectral synthesis, bumpy and wrinkled textures, windy waves, marble); PBR 4ed §10.3 (texture interface, scale and mix textures), §15.3.9 (why complex textures are evaluated in a separate kernel); Shirley, *Ray Tracing: The Next Week*, "Solid Textures" and "Perlin Noise"; pbrt-v4 `src/pbrt/textures.h`.
