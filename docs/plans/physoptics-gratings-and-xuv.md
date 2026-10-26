# Gratings, toroidal mirrors and XUV optics

The request's spectral goals are prism dispersion, lens chromatic aberration, XUV optics with gratings and a toroidal mirror, and dichroic mirrors. Two of them come free once [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md) and the glass catalogs of [physoptics-optical-materials.md](physoptics-optical-materials.md) exist: a dispersing prism and a singlet's color error are glass with a wavelength-dependent index, traced per lane. Dichroic mirrors are thin-film coatings from the same plan. What is missing is the XUV bench: toroidal and cylindrical surfaces, diffraction gratings with their orders and efficiencies (including the variable line spacing XUV spectrometers use), and mirror reflectivities at grazing incidence from atomic scattering data, in vacuum, at wavelengths where every material's index is within a percent of 1.

*Not implemented. Depends on [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md), [physoptics-optical-materials.md](physoptics-optical-materials.md), [physoptics-optical-elements.md](physoptics-optical-elements.md) and [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md) (arbitrary wavelength ranges). Non-visible beams are drawn with [pbrt-display-and-color.md](pbrt-display-and-color.md)'s false color. Part of the [laser optics plan](physoptics-laser-optics.md).*

## What the renderer will have

- Exact surfaces of the conic and even-asphere kinds, intersected in `crt_optic.glsl` and `rt_optics` ([physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)).
- Spectral lanes over any range from 1 to 20,000 nm, with hero wavelengths uniform outside the visible range, spectral images (`S0.<nm>` EXR channels) for non-visible results, and a monochromatic representation whose display shows a wavelength outside the visible range in a chosen color or through a colormap ([pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md), [pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md)). RGB-authored data beyond 360 to 830 nm holds its end values until [spectral-out-of-range.md](spectral-out-of-range.md) decides otherwise.
- Glasses with dispersion formulas, a vacuum surrounding medium, thin-film stacks with complex indices, and a surface finish with an rms roughness ([physoptics-optical-materials.md](physoptics-optical-materials.md)).

## Design

### Toroidal and cylindrical surfaces

`SurfaceShape::Toroid`: a curve in the surface frame's yz plane with radius `R_y` and conic `k_y`, swept about an axis parallel to y at distance `R_x` from the vertex, OpticStudio's toroidal surface. `R_x = ∞` is a cylinder, and `R_x = R_y` with `k_y = 0` a sphere, which the tests use. The intersection starts from the tangent plane's hit and refines with Newton's method on `z − sag(x, y) = 0`, as the even asphere does; at grazing incidence the plane's hit is far from the surface's, so the start is the hit on the osculating sphere at the vertex instead, which brings the Newton iteration within its basin for grazing angles down to 0.5°. Principal curvatures and their directions come in closed form, for the solver.

A toroidal mirror at grazing angle θ (from the surface) images a point at distance p to a point at q without astigmatism when its radii are

```text
R_meridional = 2 p q / ((p + q) sin θ),    R_sagittal = 2 p q sin θ / (p + q)
```

which the toroidal mirror kind's builder offers to fill in from p, q and θ.

### Gratings

A grating is an attribute of a surface, not a surface kind: any plane, sphere or toroid can be ruled, which covers flat gratings, Rowland-circle concave gratings and toroidal gratings alike. Its parameters:

| Parameter | Meaning |
|---|---|
| Line density | lines per millimeter at the vertex |
| Line direction | in the surface frame |
| Variable line spacing | `d(w) = d₀ (1 + b₂ w + b₃ w² + b₄ w³)` along the ruling's normal, w from the vertex (VLS gratings for flat-field XUV spectrometers) |
| Mode | reflection or transmission |
| Orders | the range that is traced, −3 to +3 by default |
| Efficiency | tabulated per order (and per polarization) over wavelength; or the scalar blaze model; or uniform, for tests |

At a hit, the outgoing direction of order m follows the vector grating equation: the outgoing wave vector's tangential component is the incoming one's plus `m λ / d` along the local grating vector (the ruling's normal in the surface's tangent plane, with `d` the local period from the VLS polynomial), and its normal component completes it to the medium's wavenumber. An order whose normal component would be imaginary is evanescent and not traced.

The scalar blaze model gives a ruled grating of blaze angle `θ_B` the efficiency `η_m = sinc²(m − m_B(λ))`, where `m_B(λ) = 2 d sin θ_B / λ` is the order the blaze favors at that wavelength in the Littrow configuration, scaled by the coating's reflectance. It holds for `d ≫ λ` and is the default only when no tabulated efficiency is loaded, since vendors publish measured curves for their gratings.

Sampling picks an order with probability proportional to its efficiency (the zeroth order included, as the specular reflection or transmission), and the weight is the total efficiency over the probability, as at any partial interface. The beam solver follows every order above the ghost threshold as a port, **Order m**, and its automatic differentiation through the grating equation gives each order's anamorphic magnification, `cos α / cos β`, with no special case.

The `grating` coating kind and the grating attribute live in [physoptics-optical-materials.md](physoptics-optical-materials.md)'s library, so a ruled surface can still carry a reflective coating's reflectance and phase.

### XUV mirror reflectivity

At XUV and soft X-ray wavelengths a material's index is `n = 1 − δ + iβ` with δ and β small, computed from its atomic scattering factors. The renderer bundles the Henke tables for the materials XUV optics are coated with (Au, Pt, Ni, Cr, C, Si, SiC, B₄C, Mo), 10 to 1000 eV, from the CXRO database, as tabulated complex-index spectra in `assets/spectra/xuv/` with their density, and the formula from scattering factors to index, `n = 1 − (r_e λ² / 2π) Σ nₐ (f₁ + i f₂)`, in `rt_optics`. A mirror coated with one of them reflects by the exact complex Fresnel equations at grazing incidence, and a Mo/Si multilayer for 13.5 nm is a thin-film stack of those indices. The surface finish's rms roughness σ attenuates the specular reflectance by the Névot–Croce factor `exp(−2 k₁ k₂ σ²)`, with `k₁` and `k₂` the normal components of the wave vectors on the two sides,, which at XUV wavelengths matters at the nanometer level, and adds the scattered power to the ABg lobe.

### The XUV bench

- **Surrounding medium** is vacuum ([physoptics-optical-materials.md](physoptics-optical-materials.md)); the panel warns when an XUV source is set in air, which absorbs it within millimeters.
- A laser element at an XUV wavelength is a fine model of a high-harmonic beam, which is close to Gaussian with a divergence of milliradians ([physoptics-laser-sources.md](physoptics-laser-sources.md)).
- The spectral range, the band set and the false color for the display follow the scene's sources: a setup whose lasers are all below 100 nm defaults to a hero-wavelength range around their lines, and to band images on detectors.
- XUV optics are not on posts in a lab; they sit in vacuum chambers. They use **Free placement** ([physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)), and a chamber model is not planned.

### Element kinds

New `OPTIC_TRAITS` rows: **Ruled grating** (plane, Thorlabs-style square or round), **Concave grating** (spherical or toroidal), **Toroidal mirror**, **Cylindrical mirror**, and **Cylindrical lens**, which the toroid makes possible and which a lab often needs.

## Registry, bindings and data

- `SurfaceShape::Toroid` in `crt_optic.glsl` and `src/optic_surface.*`.
- The grating attribute in the surface record's payload (line density, direction, VLS coefficients, mode, order range, efficiency reference); the `grating` coating kind; efficiency tables in binding 56 beside the coatings.
- `assets/spectra/xuv/`: Henke tables, with the CXRO source and terms in each file's header.
- Kinds `ruled_grating`, `concave_grating`, `toroidal_mirror`, `cylindrical_mirror`, `cylindrical_lens`; ports **Order m**.
- No new binding.

## Verification

- A scene without these kinds renders bit-identical.
- `bin/optics_test`:
    - a toroid with `R_x = R_y` and `k = 0` intersects identically to a sphere, to 1e-6 relative in `float`, and a cylinder's hits lie on it, including at 0.5° grazing incidence;
    - the grating equation: a 600 lines/mm grating at 633 nm in Littrow sends order −1 back along the incident ray to 1e-9 rad; every order's direction satisfies the scalar grating equation in its plane; evanescent orders are not produced;
    - a VLS grating's local period at the hit equals the polynomial's;
    - efficiencies over all propagating orders, plus absorption, add up to 1 for the uniform model;
    - gold's reflectance at 5° grazing incidence and 30 nm matches the CXRO X-ray interactions calculator to 1e-3, and a 40-period Mo/Si multilayer peaks at 13.5 nm with the reflectance CXRO's multilayer calculator gives.
- `bin/beam_test`: a grating's first-order beam has the anamorphic magnification `cos α / cos β`; a toroidal mirror at 3° grazing with the radii above images a point source with sagittal and meridional foci coincident within 1e-6 of q.
- Rendering, in a spectral mode: a grating spectrometer (slit, concave grating, camera sensor) puts two laser lines at the sensor positions the grating equation predicts, within one pixel; a dispersing prism and a singlet's chromatic focal shift match [physoptics-verification-scenes.md](physoptics-verification-scenes.md)'s analytic values.

## Order of work

1. The toroid, with its tests, and the toroidal and cylindrical mirror kinds.
2. Plane ruled gratings: the grating equation, orders, uniform efficiency, sampling and the solver's ports.
3. Tabulated and scalar-blaze efficiencies.
4. Concave and VLS gratings.
5. Henke tables, complex-index XUV reflectance, roughness, and the XUV bench defaults.

## Waiting for the wave backend

When this plan is implemented, move this section to `docs/plans/waiting/physoptics-gratings-and-xuv.md`.

- A spectral line's diffraction-limited width, `λ / (m N)` for N illuminated lines, and the grating's resolving power: the geometric trace gives a line's width from the beam's divergence and the slit alone.
- The phase of each order (kept per order in the coating tables) for coherent detection.

## Open questions

### Pending answers

- Which gratings. The lab's gratings: line densities, blazes, sizes, and whether the vendor's efficiency curves are available to load. Thorlabs' visible ruled gratings (GR-series, 25 mm square) and XUV vendors' VLS gratings (for example Shimadzu's or Hitachi's flat-field designs) have different parameter sets.
- The XUV range and coatings. Which wavelengths and which mirror coatings (gold, platinum, B₄C, multilayers) the XUV setups use, so the bundled Henke tables cover them.
- Mounts for gratings. Whether a square grating sits in a KM100 with an adapter, a prism mount or an FP01 plate holder on this bench.

### Design questions

- Rigorous grating efficiencies (rigorous coupled-wave analysis, or the integral method) are electromagnetic calculations of their own. They could be computed offline into tabulated efficiencies by an external tool, which this plan already reads; whether that belongs with the user's wave backend is the user's call.

## Sources

Palmer and Loewen 2014, *Diffraction grating handbook*, 7th ed. (the grating equation, blaze, efficiency, concave and VLS gratings, anamorphic magnification); Henke, Gullikson and Davis 1993, *X-ray interactions: photoabsorption, scattering, transmission, and reflection at E = 50–30,000 eV, Z = 1–92*, Atomic Data and Nuclear Data Tables 54(2), and the CXRO database (henke.lbl.gov); Attwood and Sakdinawat 2017, *X-rays and extreme ultraviolet radiation*, chapters 3 and 4 (refractive index at XUV wavelengths, grazing-incidence mirrors, multilayers); Névot and Croce 1980 (roughness and reflectance); Peatman 1997, *Gratings, mirrors and slits* (toroidal mirrors and their radii); Harada and Kita 1980 (flat-field VLS spectrographs); the Zemax OpticStudio manual, "Toroidal" surfaces and "Diffraction Grating" surfaces.
