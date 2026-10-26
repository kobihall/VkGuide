# Appearance extras

These are appearance models from PBR's "Further Reading" and exercises that pbrt itself does not implement: energy compensation for microfacet models, a sheen lobe for cloth, thin-film iridescence, and glints. Each is independent of the others and lands as a material parameter or lobe through the material system.

*Research extension, planned at the user's request on 2026-09-26. Not implemented. Depends on [pbrt-bsdf-library.md](pbrt-bsdf-library.md); iridescence is best with [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md); glints need the ray footprint from [pbrt-textures.md](pbrt-textures.md). Part of the [roadmap](pbrt-roadmap.md).*

## Microfacet energy compensation

A single-scattering microfacet model loses the light that bounces between microfacets more than once, and loses more the rougher the surface: a white rough conductor in the white furnace renders visibly darker than 1 (PBR 4ed §9.6.5 names this as the model's main flaw). The glTF pbr material in `crt_scatter.glsl` also says of itself that its layering "does not conserve energy exactly at grazing angles on rough surfaces".

Two established fixes:

- Kulla and Conty (2017) precompute the directional albedo E(μ, α) of the single-scattering lobe and its hemispherical average E_avg(α), and add a multiple-scattering lobe `f_ms = (1 − E(μo)) (1 − E(μi)) / (π (1 − E_avg))`, tinted by the average Fresnel term for conductors. It has its own sampling (cosine-weighted) and density. For dielectrics it needs E for reflection and transmission, and for inside and outside.
- Turquin (2019) scales the single-scattering lobe by `1 + F0 (1 / E(μo) − 1)`. It is cheaper and needs one table, and it is exact in the furnace for conductors and approximate for others.

Both need small tables: E(μ, α) at 32 × 32, E_avg(α) at 32, and the dielectric variants over η. `rt_bsdf` integrates them at start-up (a second or two on the CPU) and uploads them to a `BsdfTables` buffer (binding 47). The roadmap's binding table gains this row.

Each microfacet material (`conductor`, `glass`, `pbr`, the coated materials' interfaces) gains a **Multiple scattering** parameter: off (default, so every existing scene stays bit-identical), Kulla–Conty, Turquin.

## Sheen

Velvet, dusty surfaces and peach fuzz reflect most at grazing angles, which no microfacet lobe with a Beckmann or Trowbridge–Reitz distribution does. Conty Estevez and Kulla (2017) introduce the "Charlie" distribution, `D(θ) ∝ (2 + 1/α) sin^(1/α) θ / 2π`, with an approximate visibility term. glTF's `KHR_materials_sheen` specifies that model, with a sheen color and roughness layered over the base, energy-conserving through a precomputed albedo scaling of the base.

It becomes a sheen color and roughness on `diffuse`, `coated_diffuse` and `pbr`, sampled uniformly over the hemisphere or cosine-weighted (the distribution has no closed-form sampling), and imported from `KHR_materials_sheen`. Its albedo table joins `BsdfTables`.

## Thin-film iridescence

Soap bubbles, oil on water and heat-tinted metal are colored by interference in a film a few hundred nanometers thick over a surface. The reflectance at one wavelength is the Airy summation over the film's internal reflections, which depends on the film's thickness, its IOR and the base's complex IOR (Belcour and Barla 2017):

- In spectral modes it is evaluated exactly per lane at the lane's wavelength, since each lane is one wavelength. This is where spectral rendering makes a feature simpler.
- In exact RGB it uses Belcour and Barla's closed-form integration of the interference terms against the color-matching functions, which avoids the aliasing of evaluating at three wavelengths.

The single film is the one-layer case of the transfer-matrix method that [physoptics-optical-materials.md](physoptics-optical-materials.md) implements for coatings (`crt_tmm.glsl`), and the spectral evaluation reuses it rather than a second implementation.

It becomes a thin-film thickness and IOR on `conductor`, `glass` and `pbr`, replacing the Fresnel term of the top interface, and imports from `KHR_materials_iridescence` (thickness range from a texture).

## Glints

Glittery paint, bead-blasted plastic and scratched metal under a sun show sparkles: the microfacets within one pixel's footprint are too few to be described by a smooth distribution. Every glint model evaluates the BSDF over a footprint rather than at a point:

- Jakob et al. 2014: a stochastic count of mirror facets per footprint and direction cone, evaluated on the fly.
- Atanasov, Wilkie and Koylazov 2021: a multi-level structure over a discretized normal distribution, cheaper than Jakob's and with a visible-normal sampling scheme (Chermain et al. 2021).

It needs the ray-cone footprint at the hit ([pbrt-textures.md](pbrt-textures.md)) to know how many facets a pixel covers. It becomes a `glint` density and roughness on `conductor` and `pbr`, in the complex queue. This is the most speculative item here; do it last and only if the others are done.

## Verification

- White furnace: Kulla–Conty and Turquin bring white rough conductors to 1 at every roughness; the `pbr` material with compensation stays at or below 1 at grazing angles.
- `bin/bsdf_test` on every new lobe, including the sheen lobe's density.
- A soap bubble (thin glass, film 350 nm, IOR 1.33) rendered in hero-wavelength mode shows the interference bands; the RGB approximation matches it to within visible tolerance.
- Every scene that does not use a new parameter renders bit-identical.

## Order of work

1. The E tables and Turquin's scaling, then Kulla–Conty.
2. Sheen.
3. Thin film, spectral first, then the RGB integration.
4. Glints.

## Sources

PBR 4ed chapter 9 "Further Reading" (multiple scattering among microfacets, glints and microstructure, wave optics) and exercises 9.4 and 9.8; Kulla and Conty Estevez 2017, *Revisiting physically based shading at Imageworks*; Turquin 2019, *Practical multiple scattering compensation for microfacet models*; Conty Estevez and Kulla 2017, *Production friendly microfacet sheen BRDF*; Belcour and Barla 2017, *A practical extension to microfacet theory for the modeling of varying iridescence*; Jakob et al. 2014; Atanasov, Wilkie and Koylazov 2021; the Khronos `KHR_materials_sheen` and `KHR_materials_iridescence` specifications.
