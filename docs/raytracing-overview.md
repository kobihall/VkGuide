# The raytracer, end to end

*A readable tour of how this renderer traces a ray. For the file-by-file detail an editor needs, see `docs/codebase-map.md` §8.*

---

## 1. The shape of it

The GPU path tracer is a **wavefront** tracer, in the sense Laine, Karras & Aila gave the word in 2013 and that *Physically Based Rendering* 4ed chapter 15 builds on: instead of one big kernel that follows a single ray from the camera until it dies, the work is cut into small kernels, each doing one job for *every* live ray at once, with queues carrying work between them.

The alternative is the megakernel, one thread per pixel running the whole algorithm. It is the obvious design, and the project's first raytracer, on the CPU, worked that way. It falls apart on a GPU because threads execute in lockstep groups. After one bounce, the threads in a group have diverged completely: some rays left the scene, some hit glass, some hit a textured floor. Every branch any of them takes costs *all* of them, so a group of 64 threads can end up doing 64 times the necessary work.

The wavefront fixes this by sorting. Kernel 02 finds each ray's hit and then pushes it onto one of three queues by what it found. Kernel 06 then runs over *only* the rays that hit a scattering surface — so it starts out converged, with every lane genuinely having a BSDF to sample. That is the whole trade: you pay bandwidth (each kernel reads its inputs from memory and writes its outputs back, where a megakernel would keep them in registers) and you buy execution coherence.

```
one frame = 00 → [ 01 → 02 → {03, 04, 06} → 08 ] × rayDepth → 09 → tonemap
```

The frame is progressive: each one adds samples to a running per-pixel mean, so the image converges while you watch it and a camera nudge starts it over.

---

## 2. The kernels

![The wavefront kernels 00 to 09 and the queues between them. Kernel 02 sorts rays onto the escaped, emissive and surface queues for kernels 03, 04 and 06. Kernel 06 fills the shadow queue that kernel 08 drains and the ray queue for the next bounce. Kernels 03, 04 and 08 add radiance that kernel 09 averages.](images/wavefront-kernels.svg)

The numbering is figure 15.2 of PBR 4ed, and **the directory listing is the diagram**:

```
shaders/rt/
  0001_generate_camera_rays.comp        00
  0101_generate_samples.comp            01
  0201_intersect_closest_bvh.comp       02  ┐
  0202_intersect_closest_cwbvh.comp     02  ├ interchangeable
  0203_intersect_closest_linear.comp    02  ┘
  0301_handle_escaped.comp              03
  0401_handle_emissive.comp             04
  0501_sample_medium_interaction.comp   05  (stub)
  0601_surface_scatter_bsdf.comp        06  ┐
  0602_surface_scatter_light.comp       06  │ the direct-lighting
  0603_surface_scatter_mis_balance.comp 06  │ strategies
  0604_surface_scatter_mis_power.comp   06  ┘
  0701_sample_medium_scattering.comp    07  (stub)
  0801_trace_shadow_rays_bvh.comp       08  ┐
  0802_trace_shadow_rays_cwbvh.comp     08  │ one per kernel 02 traversal
  0803_trace_shadow_rays_linear.comp    08  ┘
  0901_update_film.comp                 09
  include/                              the shared pieces
```

`NNVV` is kernel `NN`, variant `VV`. Slots 05 and 07 have no implementation, since there is no participating media, but they keep their numbers, carry a stub file that documents what would go there, and have a plan (`docs/plans/participating-media.md`).

| # | Kernel | Runs over | What it does |
|---|---|---|---|
| 00 | Generate camera rays | the whole path pool, once per frame | One primary ray per (pixel, sample) slot, jittered in the pixel and across the aperture |
| 01 | Generate samples | the live ray queue, per bounce | Prepares each path's randomness for this bounce. Where a Sobol sampler would go |
| 02 | Intersect closest | the live ray queue, per bounce | Finds the nearest hit, applies alpha cutouts and normal maps, and **sorts** the paths into three queues |
| 03 | Handle escaped | the escaped queue | A ray that left the scene collects the background, weighted by the path's MIS record, and ends |
| 04 | Handle emissive geometry | the emissive queue | A ray that hit a light collects its emission, weighted by the path's MIS record, and ends. A ray that hit a glowing `pbr` surface collects its emission and carries on through 06 |
| 06 | Sample surface scattering | the surface queue | Samples a light and pushes a shadow ray towards it, then scatters the path off the surface, or ends it |
| 08 | Trace shadow rays | the shadow queue | Traces each shadow ray with kernel 02's traversal and adds its light where nothing is in the way |
| 09 | Update film | the pixels, once per frame | Folds the frame's samples into the running mean |

Then a shared tonemap pass turns the accumulated linear HDR into the rgba8 image the "Raytraced Output" window shows.

### Why the queues hold what they do

Two indexing conventions coexist, and this is the one thing worth holding in your head:

- **`paths[]` and `radiance[]` are indexed by pool slot** — stable for a path's whole life.
- **`hits[]` and `shadowRays[]` are indexed by ray-queue position** — compacted, and different every bounce.

The two ray queues (ping-ponging between bounces) hold **path indices**. The three classification queues hold **ray-queue positions**, because that single number is *also* the index of the path's hit record. So kernel 06 pulls one number off its queue and reaches both the hit and the path from it, with no extra indirection. The shadow queue holds ray-queue positions too: kernel 06 writes a path's shadow ray at the position of its hit, and kernel 08 starts the ray from that hit's point. `PathState.radianceSlot` is the bridge back to the pixel.

### How a kernel knows how many threads to launch

It doesn't, and neither does the CPU. Each queue's header is laid out as a `VkDispatchIndirectCommand` with the count appended:

```glsl
struct QueueHeader { uint groupCountX, groupCountY, groupCountZ, rayCount; };
```

The workgroup-aggregated allocator that appends to a queue keeps `groupCountX` equal to `ceil(rayCount / 64)` as it goes — each workgroup adds the number of 64-boundaries its contiguous slice crosses. Every per-bounce kernel is then a `vkCmdDispatchIndirect` reading that header. Nothing is ever read back to the CPU to size a launch, so the compaction is genuinely free. (PBR takes the other route — it over-launches for the maximum and lets surplus threads return immediately — because CUDA launches came from the host there.)

---

## 3. Swapping strategies

This is the part built for experimenting. **Which shader runs in each slot is chosen at runtime**, from the *Windows → Raytracer Shaders* panel, and saved with the scene. Kernel 06's direct-lighting strategy also has a combo of its own, **Raytrace Render** → **Direct lighting** → **Strategy**.

### What exists today

| Slot | Variants |
|---|---|
| 02 Intersect closest | **BVH (binary)** · BVH (compressed 8-wide) · Linear scan (brute force) |
| 06 Sample surface scattering | BSDF sampling · Light sampling · MIS (balance heuristic) · **MIS (power heuristic)** |
| 08 Trace shadow rays | one partner per 02 variant, selected with it |
| everything else | one each |

The defaults are in bold. Section 5 covers the kernel 06 strategies.

The three traversal variants are the demonstration. They are three files of *six lines each*:

```glsl
#version 460
// 02 Intersect Closest - brute force, no acceleration structure.
#extension GL_GOOGLE_include_directive : require
#include "include/crt_common.glsl"
#include "include/crt_linear.glsl"      // <-- the only line that differs
#include "include/crt_intersect.glsl"
```

Because everything that is *not* the search — classifying the hit, interpolating normals and uvs, writing the record, pushing the queues — lives in `crt_intersect.glsl`, shared unchanged. A strategy file supplies exactly two functions, the closest-hit search kernel 02 runs and the any-hit test kernel 08 runs for shadow rays:

```glsl
bool traceScene(vec3 origin, vec3 direction, float tMin, float tMax, out TraceHit hit);
bool occluded(vec3 origin, vec3 direction, float tMin, float tMax);
```

That is the contract, and it is written down in `include/crt_traverse.glsl` along with the pieces every strategy needs regardless: the hit record, the work counters, the triangle test with its alpha cutouts, the object-space transform. Each strategy's kernel 08 variant is the same six lines with `crt_shadow.glsl` in place of `crt_intersect.glsl`.

**Verified:** with a fixed seed, all three strategies produce *bit-identical* images, shadow rays included. Swapping the traversal changes the cost and nothing else.

### Adding a fourth traversal

1. Write `include/crt_yourthing.glsl` defining `traceScene()` and `occluded()`.
2. Write `shaders/rt/0204_intersect_closest_yourthing.comp`, the six lines above with your include, and `shaders/rt/0804_trace_shadow_rays_yourthing.comp`, the same with `crt_shadow.glsl` last.
3. Add two entries to the table in `src/rt_kernels.cpp`: the kernel 02 variant, and a kernel 08 variant that follows it.

Step 3 is where you say which buffers each reads:

```cpp
v.push_back(KernelVariant {
    .slot = KernelSlot::IntersectClosest,
    .id = "yourthing",                 // saved in the scene file; never rename it
    .name = "Your thing",              // what the panel shows
    .description = "...",
    .shader = "rt/0204_intersect_closest_yourthing.comp",
    .domain = KernelDomain::CurrentRayQueue,
    .bindings = intersectBindings(true),
    .settings = KernelSettings::AccelerationStructure,
    .cost = TraversalCost::Acceleration,
    .requiresLayout = true,
    .layout = BvhLayout::Binary,       // the node layout it reads
});
v.push_back(KernelVariant {
    .slot = KernelSlot::TraceShadowRays,
    .id = "yourthing",
    .name = "Your thing",
    .description = "...",
    .shader = "rt/0804_trace_shadow_rays_yourthing.comp",
    .domain = KernelDomain::FixedQueue,
    .queue = QUEUE_SHADOW,
    .bindings = shadowBindings(true),
    .followsSlot = KernelSlot::IntersectClosest,
    .followsVariant = "yourthing",    // the kernel 02 variant it goes with
});
```

There is no fourth step. The pipelines, their descriptor set layouts, descriptor writes and dispatches, the panel rows and the scene-file entries all come from those two entries, and selecting the new traversal selects its kernel 08 partner.

### How the "which quantities are read and written" problem is solved

The awkward part of swapping shaders is that different shaders want different resources bound. Three approaches were on the table:

- **One fat descriptor set for everything.** What the tracer did before: every stage bound all 18 bindings whether it used them or not. Simple, but every new resource is carried by every kernel forever, and the set becomes a junk drawer.
- **Everything behind buffer device addresses.** Maximally flexible — a new resource is a new pointer field, no descriptor layout anywhere. But it means rewriting every raytracing shader, which was explicitly not the goal.
- **Global binding numbers, per-variant subsets.** What was built.

The binding *numbers* are global and fixed, declared once in `include/crt_common.glsl` and mirrored by the `CrtBinding` enum. But a variant declares which **subset** it uses, and gets a descriptor set layout containing only those numbers. Vulkan is perfectly happy with a layout that has gaps, so:

- The BVH variants list `BlasNodes` and `TlasNodes`. The linear variant doesn't — it never binds them at all.
- Kernel 09 binds three things. A BVH kernel 02 binds fourteen.
- No existing shader's binding numbers changed, so `crt_common.glsl` stayed a single shared include.

Adding a resource is: a new number at the *end* of `crt_common.glsl`, a matching enumerator at the end of `CrtBinding`, a `case` in `GpuPathTracer::writeSet()`, and listing it in the variants that want it. Existing numbers never move.

### UI that follows the selection

A variant declares what settings it implies, and the rest of the app reacts without knowing which variants those are:

- The **"Acceleration structure" section** of the Raytrace Render panel only appears when the selected 02 variant declares `KernelSettings::AccelerationStructure`. Pick the linear scan and the whole section disappears, because none of it means anything any more.
- The **node layout is no longer a separate control**. It was a combo next to the builder; it is now implied by which 02 variant you picked, because a CWBVH kernel simply cannot read binary nodes. Picking the variant rebuilds the BLASes in the layout it needs.
- The **render guard** asks the variant what a ray costs. A BVH ray costs the scene's SAH — tens of steps. A brute-force ray costs *every primitive in the scene*. That is four orders of magnitude, and it is exactly the case the guard exists for: a linear scan over a million triangles at 1080p once hung this machine's GPU hard enough to take the window server down. Switch kernel 02 to linear on a large model and the Render button refuses until you lower the resolution or accept it explicitly. Kernel 06's variant adds whether each bounce also traces a shadow ray, which doubles the price whenever the scene has a light for it to aim at.

### When a slot follows another

Kernel 08 traces shadow rays through the same acceleration structure kernel 02 walks, and a CWBVH kernel cannot read binary nodes. So kernel 08 is never chosen on its own: each of its variants names the kernel 02 variant it goes with (`KernelVariant::followsSlot`), and selecting a traversal selects its partner. The **Raytracer Shaders** window shows the row as following kernel 02, and a scene file's entry for it is not read.

### What gets saved

Payload version 8 of the scene file adds the kernel selection and the BVH build settings. A file saved today with the linear scan selected holds:

```json
"kernels": {"00":"thin_lens","01":"pcg_hash","02":"linear","03":"background",
            "04":"direct","05":"none","06":"mis_power","07":"none","08":"linear","09":"running_mean"},
"accel":   {"builder":"spatial splits (SBVH)","maxLeafSize":4, ...}
```

Saved **by id, never by index**, so adding or reordering variants later can't silently change what an old scene renders with. An id this build doesn't recognise falls back to the slot's default and reports it as a load warning rather than failing. Every older scene file still opens. A file without `"kernels"` opens with today's defaults, so kernel 06 renders it with MIS (power heuristic) rather than the BSDF sampling the file was made with.

The node layout is deliberately *not* saved — it follows the kernel 02 selection, so storing it would let a file contradict itself.

Payload version 9 adds the `pbr` material type (`"albedo"`, `"metallic"`, `"roughness"`, `"emission"`, `"strength"`), which shapes and mesh overrides can use as well as glTF models.

Payload version 10 adds `"lights"`, the punctual lights, each with its `"type"` (point, spot, directional), `"position"`, `"color"` and `"intensity"`, and as its type needs an `"orientation"`, a `"range"`, an `"innerCone"` and an `"outerCone"` in degrees. A light imported with a glTF model also records the `"model"` it came from.

---

## 4. The maths, kernel by kernel

### 00 — Generate camera rays

A thin-lens camera, precomputed on the CPU as an origin, the lower-left corner of the image plane, its two edge vectors, and the lens basis:

$$P(u,v) = \text{lowerLeft} + u\,\vec{h} + v\,\vec{v}, \qquad \vec{d} = P - (O + r_x \vec{n} + r_y \vec{b})$$

with $(r_x, r_y)$ uniform in a disk of radius aperture/2. The RNG is pure-integer PCG, seeded only from `(pixel, sample index, render seed)` — so a fixed seed reproduces an image exactly regardless of how the queues happened to be ordered or the workgroups scheduled. That determinism is what made the three-way traversal comparison possible.

Every radiance slot is pre-written to zero, with `w` marking whether the slot was spawned at all. A path that later dies silently — absorbed, roulette-killed, out of depth — then contributes exactly zero without writing anything. Each path's MIS record (section 5) starts at full weight: a camera ray sampled no light, so whatever it hits first counts in full.

### 02 — Intersect closest

**Slab test** for boxes, returning the near distance or ∞:

$$t_{near} = \max_i \min\left(\tfrac{b^{min}_i - o_i}{d_i}, \tfrac{b^{max}_i - o_i}{d_i}\right), \quad t_{far} = \min_i \max(\cdot), \quad \text{hit} \iff t_{near} \le t_{far}$$

Reciprocals replace near-zero components with a signed $10^{-12}$, so no box test forms $0 \times \infty$ — a NaN compares false both ways and would admit or reject a box arbitrarily.

**Möller–Trumbore** for triangles, two-sided, never forming the plane:

$$\begin{bmatrix}t\\u\\v\end{bmatrix} = \frac{1}{(\vec d \times \vec e_2)\cdot \vec e_1}\begin{bmatrix}(\vec t \times \vec e_1)\cdot \vec e_2\\ (\vec d \times \vec e_2)\cdot \vec t\\ (\vec t \times \vec e_1)\cdot \vec d\end{bmatrix}, \quad \vec t = O - v_0$$

**Analytic shapes** (sphere, plane, quad, box, capped cylinder) as unit primitives in object space, with the numerically stable quadratic from *Ray Tracing Gems* ch. 7 — the discriminant taken from the closest-approach vector rather than $|o|^2 - r^2$, which cancels catastrophically for a large sphere seen from close by.

Two details in the two-level structure are worth knowing:

- The object-space ray direction is **deliberately not renormalised**. Since $M(O + t\vec d) = MO + t\,M\vec d$, the parameter $t$ is identical in both spaces, so `tMin`, `tMax` and the running closest hit carry across the transform untouched.
- Normals go back to world space as $\vec n' = n_x\,\text{row}_0 + n_y\,\text{row}_1 + n_z\,\text{row}_2$ using the stored world-to-object *rows* — which is exactly $(M^{-1})^T \vec n$, correct under non-uniform scale, with no matrix inverse computed anywhere.

Only the closest hit pays for its shading data: the traversal touches positions alone, and uvs and shading normals are fetched and barycentrically interpolated once, at the end. Two exceptions:

- **Alpha cutouts.** A triangle whose glTF material is `alphaMode` `MASK` carries a flag in the top bit of its index word. When the traversal hits one, it fetches the uv and the base colour texel's alpha, and rejects the hit if the alpha is below the material's cutoff. Unflagged triangles pay nothing.
- **Normal maps.** Once the closest hit is known, kernel 02 perturbs the interpolated normal by the material's normal map in the tangent frame $(T, B, N)$ with $B = (N \times T)\,w$, glTF's convention. The front face is still decided by the unmapped normal.

A hit on a `pbr` material with emission goes onto the emissive **and** the surface queue, as in pbrt-v4. A barrier between 04 and 06 keeps 04's read of the throughput ahead of 06's write.

### 03 — Handle escaped

$$L \mathrel{+}= \beta \cdot w_b \cdot L_{\text{bg}}(\vec d)$$

Background in priority order: a solid colour, the equirectangular environment map ($u = \tfrac12 + \tfrac{\operatorname{atan2}(d_x, -d_z)}{2\pi}$, $v = \tfrac12 - \tfrac{\arcsin d_y}{\pi}$), or the Ray Tracing in One Weekend sky gradient. The weight $w_b$ is 1 except where the environment map is the background and kernel 06 sampled it at the previous vertex; section 5 explains how the path's MIS record decides it.

### 04 — Handle emissive geometry

$$L \mathrel{+}= \beta \cdot w_b \cdot \text{albedo} \cdot \text{strength}$$

For a `pbr` surface, the emitted radiance is the emission colour times its strength, times the sRGB-decoded emissive texel if there is one. The path does not end there.

The weight $w_b$ is what keeps a light from being counted twice. Under BSDF sampling it is 1, since a light found by chance is the only way the light arrives. When kernel 06 also sent a shadow ray towards the lights from the previous vertex, the same light could have arrived that way too, and $w_b$ is the MIS weight of the BSDF sample against that light sample (section 5).

### 06 — Sample surface scattering

$$\beta_{n+1} = \beta_n \cdot f(\omega_o, \omega_i)$$

Four scattering materials carried over from the project's first, CPU, raytracer, and glTF's metallic-roughness model:

- **Lambertian** — `normal + randomUnitVector()`. Adding a uniform-sphere point to the normal gives a *true* cosine-weighted distribution, so the sampled direction's weight is the albedo alone: the $\cos\theta/\pi$ in the BSDF and in the density cancel.
- **Metal** — `reflect(d, n) + fuzz · randomInBall()`, absorbed if it goes below the surface.
- **Phong** — a $\cos^\alpha$ lobe around the reflection direction with $\alpha = 1000^{s^2}$, sampled as $\cos\theta = \xi^{1/(\alpha+1)}$ in a tangent frame.
- **Dielectric** — Snell with Schlick's approximation $R(\theta) = R_0 + (1-R_0)(1-\cos\theta)^5$, total internal reflection when $\eta\sin\theta > 1$.
- **PBR** — glTF 2.0 metallic-roughness. A GGX specular lobe with $F_0 = \text{mix}(0.04, c, m)$ over a Lambertian lobe of $c\,(1-m)$, for base colour $c$ and metallic $m$. One lobe is sampled per bounce, with probability proportional to the luminance of what it reflects towards $\omega_o$. The specular lobe samples visible normals (Heitz 2018), so its weight is $F(\omega_o \cdot h)\,G_1(\omega_i)$. The metal/rough texture scales roughness by its green channel and metallic by its blue. Every glTF material renders as this type.

Then **Russian roulette**: past a few bounces, a path survives with probability $p = \text{clamp}(\max\beta, 0.05, 1)$ and its throughput is divided by $p$. Unbiased, and it is what makes late bounces cheap — by bounce six the queue is a fraction of its original size, and the indirect dispatch shrinks with it.

Before it continues, a path at a non-specular surface samples one light and pushes a shadow ray towards it, carrying the whole contribution

$$\beta_n \cdot f(\omega_o, \omega_l)\,|\cos\theta_l| \cdot L_e \cdot \frac{w_l}{p_l(\omega_l)}$$

for kernel 08 to add if nothing is in the way. Each material answers two questions for this beside its sampling routine: `evalSurface()`, the $f\,|\cos\theta|$ its sampling estimates, and `pdfSurface()`, the density its sampling would have picked that direction with. Both come from the sampling routine, so that the attenuation `scatterSurface()` returns is exactly their ratio: $\cos\theta/\pi$ for a lambertian surface, the density of a point uniform in the fuzz ball for the metal, $\tfrac{\alpha+1}{2\pi}\cos^\alpha$ around the mirror direction for phong, and for pbr the lobe choice's mix of the visible-normal density $G_1(\omega_o) D(h) / (4\cos\theta_o)$ and $\cos\theta/\pi$. A dielectric, and a metal with no fuzz, are specular: no light sample can land in them, so they sample none.

### 08 — Trace shadow rays

Each shadow ray starts at its path's hit point and stops just short of the light. Kernel 08 asks the traversal whether anything lies in between, which `occluded()` answers at the first hit it finds, and adds the contribution when nothing does. It shares kernel 02's traversal and alpha test, so a cut-out leaf casts a cut-out shadow.

### 09 — Update film

A running mean rather than a running sum, so that a future adaptive sample budget can leave pixels with different counts:

$$\mu_{n+m} = \frac{n\,\mu_n + \sum_{k} L_k}{n+m}$$

Unspawned slots and non-finite samples are skipped and not counted — one NaN folded into a mean poisons that pixel permanently.

A skipped sample still biases its pixel, since it stood for a real path's contribution. So kernel 09 counts the non-finite samples it drops, and **Raytrace Render** shows the total in orange whenever it is above zero. Any count there means a NaN or an infinity got into a path.

---

## 5. Light sampling and multiple importance sampling

![The Veach scene at 384 by 256 pixels. Top row, at 16 samples per pixel: BSDF sampling, light sampling, and MIS with the balance heuristic. Bottom row: MIS with the power heuristic at 16 samples, the MIS weights debug view at 256, and a 4096-sample reference.](images/veach-mis-strategies.jpg)

### Two ways to find a light

The light a surface reflects towards the camera straight from the lights is an integral over incoming directions:

$$L_d = \int f(\omega_o, \omega_i)\, L_e(\omega_i)\, |\cos\theta_i|\, d\omega_i$$

A path tracer can estimate it two ways. It can sample the BSDF and hope the direction hits a light, or it can pick a point on a light and trace a shadow ray to see whether it is visible. Each fails where the other works. The image above reproduces the scene of figures 9.2 and 9.8 of Veach's thesis, which PBR 4ed redraws as figure 13.8, and it shows both failures at once. Four plates run from smooth at the top to rough at the bottom, under four spheres of equal power whose radii triple from left to right:

- BSDF sampling (top left) is clean where a smooth plate reflects the large light: the lobe is narrow, and most samples inside it hit the light. It fails for the small lights on the rough plates, where a wide lobe almost never finds a light that small, and the few samples that do are very bright.
- Light sampling (top middle) is clean for the small lights on rough plates, and fails for the large light on the smooth plate: points spread over a large light rarely fall inside a narrow lobe, and when one does, the BSDF there is huge.

Multiple importance sampling takes one sample of each and weighs each by how well its technique could produce it (Veach and Guibas 1995):

$$\hat L_d = w_l \frac{f\, L_e\, |\cos\theta|}{p_l}\Big|_{\omega_l} + w_b \frac{f\, L_e\, |\cos\theta|}{p_b}\Big|_{\omega_b}, \qquad w_l = \frac{p_l^\beta}{p_l^\beta + p_b^\beta}, \quad w_b = \frac{p_b^\beta}{p_b^\beta + p_l^\beta}$$

where $p_l$ is the density with which light sampling picks a direction and $p_b$ the density with which BSDF sampling does, both evaluated at the same direction. The weights sum to 1 wherever either technique can reach, so the estimate stays unbiased, and each sample counts for less where the other technique is more likely to produce it. With $\beta = 1$ this is the balance heuristic; with $\beta = 2$, the power heuristic, which trusts the better technique more and is usually a little less noisy. The top right and bottom left images use them.

### The strategies are kernel 06 variants

Kernel 06 is where a path is at a surface, so it is where the strategy lives. Its four variants share one body, `include/crt_surface.glsl`, and differ in a single define:

| Variant | Shadow ray to | Its weight | What 03 and 04 then add |
|---|---|---|---|
| BSDF sampling | delta lights only | 1 | everything the path hits, at full weight |
| Light sampling | one light, picked by power | 1 | nothing from a light in the light list |
| MIS (balance) | one light, picked by power | $w_l$, $\beta = 1$ | the light, weighted $w_b$ |
| MIS (power) | one light, picked by power | $w_l$, $\beta = 2$ | the light, weighted $w_b$ |

The last column is the part that crosses kernels. A light found by the BSDF sample is handled by kernel 03 or 04 one bounce later, which cannot know which strategy chose the direction. So kernel 06 leaves an MIS record in the path it continues: `PathState.misPdf` is -1 (`CRT_MIS_FULL`) for full weight, 0 for "light sampling already covered this", or the BSDF density $p_b$ of the direction it chose, with the heuristic's exponent beside it. Kernels 03 and 04 compute $p_l$ for what the ray hit, from the path's origin, which is still the previous vertex, and apply the weight the record implies. pbrt-v4's wavefront integrator carries its `r_u` and `r_l` in its work items for the same reason. Because the record, not a separate kernel, carries the strategy, choosing kernel 06's variant is the whole switch: there is no pairing of kernels to get wrong.

Three cases keep full weight whatever the strategy. A camera ray sampled no light. A ray leaving a specular surface (a dielectric, or a metal with no fuzz) could not have been matched by a light sample, since no light sample can land in a delta distribution. And an emitter the light list does not hold, the infinite plane, has no light technique to share with.

Point, spot and directional lights work the other way round: no ray can hit them, so only a shadow ray can reach them, at weight 1. Even BSDF sampling sends shadow rays to them, which is Veach's own setup, where the spotlight lights both of his images, and it keeps all four strategies converging to the same image.

### The light list

`src/rt_lights.cpp` gathers every light kernel 06 can sample, in world space, whenever the scene changes. Each gets a power, and an alias table picks one in proportion to it in constant time (pbrt-v4's `PowerLightSampler`, PBR 4ed 12.6.2). The delta lights come first and have a second table of their own, which BSDF sampling uses. Kernel 02 records which light a hit landed on (`HitRecord.lightIndex`) so kernel 04 can price it.

| Light | Sampled | Density $p$ | Power |
|---|---|---|---|
| Sphere | uniformly in the cone it subtends; by area from inside | $1 / 2\pi(1 - \cos\theta_{max})$ | $\pi A L$ |
| Quad, or one face of a box | uniformly in solid angle (Ureña et al. 2013) | $1 / \Omega$ | $2\pi A L$, $\pi A L$ per box face |
| glTF triangle | uniformly in solid angle (Arvo 1995) | $1 / \Omega$ | $2\pi A L$ times its emissive texture's mean over it |
| Cylinder | by area | $d^2 / A \lvert\cos\theta_l\rvert$ | $\pi A L$ |
| Environment map | by its table | $p(u, v) / 2\pi^2 \sin\theta$ | $\pi r^2 \int L\, d\omega$ |
| Point, spot, directional | their one direction | delta | $4\pi I$; the spot's cone; $\pi r^2 E$ |

A rectangle or triangle that subtends less than $10^{-3}$ sr, or nearly a hemisphere, is sampled by area instead, as pbrt does. The infinite plane has no finite area and stays out of the list; $r$ is the radius of a sphere around the scene's geometry. Every routine follows pbrt-v4's implementation, and `src/light_sampling.cpp` mirrors the GLSL line for line so `bin/light_test` can check each one on the CPU: a sample's density must equal the density function at the sampled point, and the solid angle a light covers, estimated through its own samples, must match one counted from directions spread uniformly around it.

### The environment map as a light

An HDRI concentrates its light: half the power in `assets/nowhere_road_4k.hdr` comes from 0.0004% of the sphere, the sun. BSDF sampling finds the sun only by chance, and in the table below that leaves its error five orders of magnitude above MIS's. Light sampling draws directions from a piecewise-constant 2D distribution over the map's texels, proportional to their luminance times $\sin\theta$, built when the map loads (pbrt-v4's `ImageInfiniteLight`, PBR 4ed 12.5.2). The table is at most 2048 cells wide, and each cell averages its texels and a one-texel border, so bilinear filtering never puts light where the table has none.

The tables come from the file's 32-bit floats, but the GPU copy of the map is half float, whose largest value is 65504. A brighter map is stored divided by the smallest power of two that fits, and the shaders multiply the factor back in with the environment intensity. The lamp in `assets/moon_lab_4k.hdr` peaks at 466944, so that map is stored at 1/8. Without the factor, those texels would be infinite on the GPU while the tables sent most light samples their way, and kernel 09 would drop every sample that met them.

Under MIS, light samples come from a second table with the mean subtracted and clamped at zero, the MIS compensation of Karlík et al. (2019) that pbrt-v4 uses: they concentrate on what is brighter than average, and BSDF samples, which can reach every direction, cover the rest. Light sampling on its own must use the full table, since nothing covers what the compensated one leaves out. Kernel 03 prices a direction from the same table kernel 06 drew from.

### What it costs

A shadow ray is a second walk through the scene, but an any-hit one that stops at the first thing in the way: in Sponza it visits 47 BVH nodes where a closest-hit ray visits 81. A frame costs about a quarter more: Sponza under `nowhere_road_4k.hdr`, at 320×180 with 4 samples per frame and 6 bounces, takes 56 ms per frame under BSDF sampling and 69 ms under MIS. The render guard counts two traversals per bounce whenever the selected variant has a light to send shadow rays to.

At equal samples, the relative mean squared error against a 16384-sample reference, at 4096 samples per pixel:

| Scene | BSDF | Light | MIS balance | MIS power |
|---|---|---|---|---|
| Cornell box | 4.9e-3 | 1.3e-4 | 1.2e-4 | 1.2e-4 |
| Veach scene, direct light only | 5.5e-2 | 1.8e-3 | 1.1e-4 | 1.1e-4 |
| Spheres under `nowhere_road_4k.hdr`, no glass | 4.4 | 1.2e-4 | 3.8e-5 | 3.4e-5 |
| 980 lights of every kind | 9.6e-4 | 2.0e-3 | 8.9e-4 | 7.7e-4 |

The last row shows what power-proportional selection cannot do: with hundreds of small emitters, most picks land on a light far away or out of sight, and light sampling falls behind even BSDF sampling. A light sampler that weighs lights by where they are from the shading point, pbrt-v4's BVH light sampler (PBR 4ed 12.6.3), is the next step there.

### Comparing strategies yourself

`assets/scenes/veach_mis.gltf` is the Veach scene, built from Mitsuba's `veach_mi` reconstruction of his figure: Veach's coloured lights at equal power, his overhead spotlight, and plates of glTF's metallic-roughness material with a rough-plastic finish. It traces direct light only, at a ray depth of 2 and 64 samples per pixel; raise the depth to add the light the plates and walls reflect onto each other. Switch strategies with **Raytrace Render** → **Direct lighting** → **Strategy**. The **MIS weights** debug view (section 6) shows which technique each part of the image leans on, and **Compare to reference** measures how far a render is from a converged one.

---

## 6. Debug views

The debug views (normals, hit/miss, bounce heat, traversal cost, …) replace shading: each writes its value as the path's whole contribution and ends the path there. Because they go through the radiance slots, they accumulate and average under jitter exactly like radiance does, rather than being a separate display mode.

They're also the reason kernels 03, 04 and 06 all call the same `debugTerminalValue()` — all three terminate paths, so a view handled in only one of them leaves black holes where paths ended elsewhere.

The **traversal cost** view is the one to reach for when comparing strategies. Kernel 02 records each ray's node visits and primitive tests, aggregated per workgroup and read back per frame. These are counted, not timed, which matters on this laptop: it thermally throttles enough that identical runs drift 70% within a minute, so wall-clock comparisons between builders are worthless and counted work is not.

The MIS weights view works differently: it lets a path run to its second vertex and shows the light that reaches the first surface directly, after its MIS weight, split by the technique that found it. Red is what BSDF samples found, added by kernels 03 and 04; green is what light samples found, carried by kernel 06's shadow rays. Where both techniques carry weight the two mix to yellow, as in Veach's figure 9.8(d). A light seen straight from the camera belongs to neither technique and stays black, and so do point, spot and directional lights, which only light sampling can reach. Under BSDF sampling alone the view is red, and under light sampling alone it is green, apart from what only the other technique reaches.

**Compare to reference** in the **Raytrace Render** panel measures a render against a converged one. Render one strategy to thousands of samples, press **Use this render as reference**, then render the others: each frame shows the RMSE and the relative MSE against the reference as the render accumulates. Relative MSE divides each pixel's squared error by its reference value squared plus 0.01, so dark regions count. `shaders/image_error.comp` reduces both on the GPU, 16×16 pixels per workgroup. The numbers mean something only for the scene and camera the reference was rendered from, and a render at another resolution is not compared.

---

## 7. Where things live

| What | Where |
|---|---|
| The kernels | `shaders/rt/NNVV_*.comp` |
| Shared shader code | `shaders/rt/include/` |
| The registry — **edit this to add a variant** | `src/rt_kernels.h/.cpp` |
| Pipelines, buffers, the schedule | `src/rt_gpu.h/.cpp` |
| The panels, the render guard, BVH and direct-lighting settings, the reference readout | `src/rt_renderer.h/.cpp` |
| BVH building and layouts (no Vulkan) | `src/bvh*.{h,cpp}`, `src/rt_accel.*` |
| Scene persistence | `src/scene_io.*` |
| Light sampling in GLSL | `shaders/rt/include/crt_light.glsl` |
| Kernel 06's and 08's bodies | `shaders/rt/include/crt_surface.glsl`, `crt_shadow.glsl` |
| The light list | `src/rt_lights.h/.cpp`, built by `buildSceneAccel()` in `src/rt_accel.cpp` |
| Light sampling on the CPU (no Vulkan) | `src/light_sampling.h/.cpp` |
| The reference error | `shaders/image_error.comp`, `GpuPathTracer::recordReference()` |
| CPU verification harnesses | `tests/bvh_bench.cpp` → `bin/bvh_bench`, `tests/light_test.cpp` → `bin/light_test` |

Several shader files are mirrored line for line by CPU code: `crt_bvh.glsl` ↔ `bvh_layout.h`, `crt_shape.glsl` ↔ `shape.cpp`, `crt_light.glsl` ↔ `light_sampling.cpp`, and the `crt_common.glsl` structs ↔ `rt_gpu.h`, `rt_accel.h` and `rt_lights.h`, with `static_assert`s on size. Each carries a "change both together" note in its header. `bvh_bench` checks the traversal side against brute force and `light_test` checks every light's sample densities, which transitively validates the GLSL.

---

## 8. Sources

Laine, Karras & Aila 2013, *Megakernels considered harmful* (the wavefront architecture); PBR 4ed ch. 15, especially §15.1.2 on why megakernels diverge and §15.2.4 on work queues; Aila & Laine 2009 (binary GPU BVH layout and ordered traversal); Ylitie, Karras & Laine 2017 (CWBVH); Möller & Trumbore 1997; Shirley, *Ray Tracing in One Weekend* / *The Next Week* (the camera, and the lambertian, metal, dielectric and emissive materials, by way of the project's original CPU raytracer); Bikker, *How to build a BVH*.

For light sampling and MIS: Veach 1997, *Robust Monte Carlo methods for light transport simulation*, ch. 9 (the estimator, both heuristics, and the plates scene); Veach & Guibas 1995, *Optimally combining sampling techniques for Monte Carlo rendering*; PBR 4ed §2.2.3 (MIS and MIS compensation), §12.5.2 (image infinite lights), §12.6 (light sampling), §13.4 (the path tracer kernel 06 follows), §15.3.8–15.3.10 (the wavefront's emission, surface scattering and shadow rays) and §A.1 (the alias method); Ureña, Fajardo & King 2013, *An area-preserving parametrization for spherical rectangles*; Arvo 1995, *Stratified sampling of spherical triangles*; Karlík et al. 2019, *MIS compensation*; Mitsuba's `veach_mi` scene, which `assets/scenes/veach_mis.gltf` reproduces.
