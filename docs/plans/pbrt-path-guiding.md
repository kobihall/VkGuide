# Path guiding

Kernel 06 chooses the next direction from the BSDF alone. That is a poor match wherever indirect light arrives from a narrow set of directions, such as a room lit through a doorway by an adjacent bright room: most BSDF samples hit dark walls. Path guiding learns the incident radiance as the render runs and samples from it. The user chose to plan Müller et al.'s practical path guiding (2017), the method pbrt's exercise 13.8 names and production renderers adopted, as research. It stores incident radiance in an SD-tree: a spatial binary tree whose leaves hold adaptive quadtrees over direction. This plan adds it as a kernel 06 variant with its training loop.

*Not implemented, research. Uses the per-pixel variance of [pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md) to combine iterations. Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- Every kernel 06 variant samples the continuation direction from the BSDF (`scatterSurface()` in `crt_scatter.glsl`) and records its pdf in `PathState.misPdf`.
- A path keeps no record of the vertices it has passed through, so radiance found at bounce 5 cannot be credited to bounce 2.
- A render restarts from zero whenever the camera or the scene changes.

## Design

### The SD-tree

Following Müller et al. 2017:

- The spatial tree is a binary tree over the scene's bounding box, splitting its longest axis in turn. A leaf splits once it has received more than `c · √(2^k)` records during iteration `k` (`c` = 12000).
- Each spatial leaf holds a directional quadtree over the square `(cos θ, φ)`, which maps area-preservingly to the sphere, so a quadtree cell's area is its solid angle over 4π. A quadtree node subdivides where it holds more than a fraction ρ (0.01) of its tree's energy.
- Two copies exist during training: the tree built by the previous iteration, which guides, and a tree with the same structure that collects this iteration's radiance.

Both trees go into binding 37, `GuidingTree`, as flat node arrays: about 20 bytes per quadtree node and a few MB for a typical scene. The build runs on the CPU in `rt_guiding`, a new Vulkan-free library. After each iteration the collecting tree is read back, refined, and uploaded as the next guiding tree. Iterations double in length, so there are only about ten in a render.

### Sampling

A new variant, `0608_surface_scatter_guided`, is the MIS power body with a different continuation sampler. Next-event estimation is unchanged.

- At a non-specular vertex, choose the BSDF with probability α (the **BSDF fraction**, default 0.5) or the guiding distribution otherwise. Sampling the quadtree descends by the four children's energies.
- The continuation's pdf is the mixture `α p_bsdf + (1 − α) p_guide`. It goes into `PathState.misPdf`, so kernels 03 and 04 weigh emission correctly without changing.
- Specular vertices, and vertices whose spatial leaf has no energy yet, sample the BSDF alone.

The quadtree's pdf at a direction is its leaf's energy over the tree's total, divided by the leaf's solid angle.

### Training

Incident radiance at a vertex is only known once the rest of the path has been traced. The standard trick records, per vertex, the throughput `βᵥ` of the continuation ray leaving it and the radiance `Lᵥ` the path had gathered up to and including the vertex's own light sample. When the path ends, the radiance that arrived at vertex `v` along its sampled direction is `(L_end − Lᵥ) / βᵥ`, component-wise.

- Kernel 06 writes a vertex record (position, octahedral direction, `βᵥ`, `Lᵥ`: 32 bytes) into a per-path ring of the last 4 vertices while training. Deeper vertices carry little weight in Müller's scheme.
- The vertex's own light sample arrives from the light's direction, not the sampled one, and its visibility is only known in kernel 08. So kernel 08, when its shadow ray is unoccluded, adds the contribution to the record's `Lᵥ` as well as to the radiance slot. It can also splat that sample into the tree in the light's direction, as Müller's implementation does.
- Kernel 09, which already visits every finished path, computes each recorded vertex's incident radiance and adds it into the collecting tree's spatial leaf and quadtree leaf with float atomics (`shaderBufferFloat32AtomicAdd`, which this GPU supports).
- The vertex records need their own buffer, 128 bytes per path: 512 MB at the 4M-path cap, so training halves the pool. Binding 48, `GuidingVertices`, is new; the roadmap's binding table gains it.

### Iterations and the image

Iteration `k` renders `2^k` samples per pixel. Müller et al. 2017 keep only the last iteration's image; Müller 2019 combines iterations weighted by their inverse variance, which wastes no samples. This plan does the second, using the per-pixel variance of `0902_compensated_mean`: each iteration accumulates into its own image, and the displayed image is the inverse-variance-weighted combination. Training stops after the **Training budget** (default 25 % of the target samples), and the rest of the render uses the final tree.

The tree describes the scene in world space, independent of the camera. A **Keep the tree when the camera moves** option (on by default) restarts the image but not the learning when only the camera changes. Any scene edit discards the tree.

### Consumers

[pbrt-russian-roulette.md](pbrt-russian-roulette.md)'s adjoint-driven rule reads the incident-radiance estimate `L̂ᵢ(x)` from the guiding tree.

### Later

- Product guiding with the BSDF (Herholz et al. 2016 for mixtures; Diolatzis et al. 2020 with linearly transformed cosines).
- Variance-aware guiding (Rath et al. 2020), which learns what reduces variance rather than radiance.
- Guiding in volumes (Herholz et al. 2019), after [pbrt-participating-media.md](pbrt-participating-media.md).
- Parametric mixtures (Vorba et al. 2014, Ruppert et al. 2020) as an alternative distribution, which would be a second guided variant with the same training interface.

## Registry, bindings and data

- `0608_surface_scatter_guided`.
- Binding 37 `GuidingTree`; binding 48 `GuidingVertices` (new).
- Settings: **BSDF fraction**, **Training budget**, **Spatial threshold** (c), **Directional threshold** (ρ), **Keep the tree when the camera moves**. The panel shows the iteration, the tree's node counts and the training time.
- `rt_guiding` and `bin/guiding_test`.

## Verification

- Existing variants render bit-identical; the guided variant is new.
- `bin/guiding_test`: the quadtree's pdf integrates to 1 over the sphere; samples match it (χ²); refining a tree conserves its total energy; the CPU refinement of a recorded tree matches the GPU's recorded sums.
- The guided variant converges to the MIS power reference on every NEE scene (**Compare to reference**, within noise). Unbiasedness holds because the BSDF keeps a share α of every direction.
- A room lit only through a doorway from a bright adjacent room, the scene type Müller et al. use: equal-time relative MSE of guided against MIS power after 1, 4 and 16 minutes. Also Sponza under the sun, where most light is indirect.

## Order of work

1. `rt_guiding`: the tree, refinement, sampling and pdf, with `bin/guiding_test`.
2. Sampling from a fixed tree trained on the CPU from a readback, to validate the variant alone.
3. The vertex records and the kernel 09 splats.
4. The iteration schedule and the inverse-variance combination.
5. Keeping the tree across camera moves.

## Sources

PBR 4ed chapter 13 "Further Reading" (path guiding) and exercise 13.8; Müller, Gross and Novák 2017, *Practical path guiding for efficient light-transport simulation*; Müller 2019, *"Practical path guiding" in production* (SIGGRAPH course); Vorba et al. 2014, *On-line learning of parametric mixture models for light transport simulation*; Herholz et al. 2016, *Product importance sampling for light transport path guiding*; Herholz et al. 2019, *Volume path guiding based on zero-variance random walk theory*; Rath et al. 2020, *Variance-aware path guiding*; Ruppert, Herholz and Lensch 2020, *Robust fitting of parallax-aware mixtures for path guiding*; Diolatzis et al. 2020.
