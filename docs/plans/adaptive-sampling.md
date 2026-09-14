# Adaptive Sampling for the Wavefront Path Tracer

A deliberately barebones plan — what the hook in `docs/plans/compute-pipeline-raytracing.md` §2.11 already provides, what a policy on top of it needs, and what else the same machinery could drive. Not a full spec; flesh it out when the path tracer exists and its noise behaviour has been seen.

## 0. Prerequisites

`docs/plans/compute-pipeline-raytracing.md`, built and verified. Everything below assumes its `sampleBudget[W*H]` buffer, `sampleCount` (`r32ui`) image, running-mean accumulation and `resolve` stage exist and that `generate` obeys the budget.

## 1. Goal

Spend the per-frame sample budget where the image is still noisy instead of uniformly: pixels that have converged stop receiving samples, the freed paths go to pixels that have not, and the whole frame converges to a target quality in fewer total samples. The frame's *total* work stays bounded (at most `W*H*K` paths), so frame time does not grow; what changes is which pixels the paths belong to.

## 2. What the hook already gives

- A per-pixel `budget` the generate stage reads: `0..K` samples this frame. Filling it is the whole job of the policy.
- Per-pixel `sampleCount`, so pixels can hold different numbers of samples and the mean stays correct.
- A generate stage that compacts, so a sparse budget makes the first extend dispatch smaller for free.
- Reset plumbing: every reset also has to reset the policy's state (§4).

## 3. What a policy needs

1. **A per-pixel variance estimate.** Add a second `rgba32f` image holding the running mean of the *squared* luminance (or of each channel squared); `resolve` updates it alongside the mean. Sample variance is then `E[x²] - E[x]²`, and the standard error of the mean is `sqrt(variance / n)`.
2. **A convergence test.** Relative standard error below a threshold (`stderr / max(mean, floor)` < `tolerance`), evaluated only once `n >= minSamples` — a pixel with 4 samples that all missed the geometry has zero variance and is not converged.
3. **A policy pass** (`crt_budget.comp`, over `W*H`): writes `budget[p] = converged ? 0 : K` (binary), or a graded value proportional to the error ranking. Binary is the right first version; grading needs a normalisation pass to keep the frame total bounded and is where most of the complexity would live.
4. **Stopping.** When no pixel wants samples the frame does nothing — detect it from a readback of the first queue's `rayCount` (with `FRAME_OVERLAP` latency) and show "converged" in the panel; the max-samples cap already stops runaway accumulation.
5. **UI**: enable toggle, tolerance, `minSamples`, and a `debugView` that shows `sampleCount` as a heat map (the raytracing doc already lists this view) and another showing the current budget mask.

## 4. Pitfalls known in advance

- **Fireflies** inflate the variance of their pixel forever; a huge-error pixel never converges and hogs budget. Clamp the per-sample contribution used for the *variance estimate* (not for the image) or use a robust statistic.
- **Early stopping is biased** whenever the first `n` samples missed a rare feature (a glint through a small gap). A generous `minSamples` (32–64) and a floor on the budget (never quite 0, e.g. 1 sample every 8 frames) are the usual defences.
- **Every reset must clear the variance image and the budget** back to "everything wants samples", in the same frame the mean is cleared.
- **Variance of a mean is not variance of the image**: neighbouring pixels can converge at very different rates on hard-edge geometry; the visual result is fine, but a heat map will look blotchy. Not a bug.
- **Budget semantics when `K` changes** (raytracing doc §8 item 7): store absolute counts and clamp to `K` in `generate`.

## 5. What else the same machinery can drive

- **Region of interest**: a mask painted through `DisplayRegistry`'s `onInteract` callback (the same hook `docs/plans/wave-simulation.md` uses for mouse injection) writes the budget directly — useful for inspecting one material without waiting for the whole frame to converge.
- **Foveated / progressive-refinement previews**: budget by distance from the cursor or by a mip-like coarse-to-fine schedule.
- **Time budgeting**: with the GPU timestamps from the raytracing doc, scale `K` per frame to hold a target frame time instead of a fixed count — useful because a render now runs alongside the live raster preview and should not make it stutter.
- **Per-material or per-light queues** are a different axis (coherence, not sample allocation) but reuse the same allocator; if they are ever built, the policy pass and the queue split are independent.

## 6. Sources to read when this is picked up

Standard references for pixel-level adaptive sampling in progressive renderers: the relative-error stopping criterion as used in Mitsuba's adaptive integrator, and Rousselle et al., *Adaptive Sampling and Reconstruction using Greedy Error Minimization* (2011) for the graded-budget version if binary proves insufficient.
