# RGB spectra outside the visible range

An RGB color has a spectrum only from 360 to 830 nm, where the uplift tables and the CIE functions are defined. A render can carry wavelengths from 1 to 20,000 nm, and what an RGB-authored reflectance, emitter or texture is at 1064 nm or 13.5 nm has no answer in the data. This plan records that question. It holds no design, since the user has not chosen an answer.

*Not designed. Follows [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

Every RGB value holds its 360 nm value below 360 nm and its 830 nm value above 830 nm: albedos, emitters, textures and the environment map alike. The decision is from 2026-10-23, as the interim behavior ([pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md)). Measured spectra (named, tabulated, `.spd`), blackbodies, dispersion formulas and lines are defined at every wavelength and are not part of this question.

## The question

The user has not decided between these, and may choose another:

- Physical extrapolation: a model of how real materials behave beyond the data.
- Zero outside the range: RGB data neither reflects nor emits where it says nothing.
- Something else.

The answer may differ by role (a reflectance, an emitter, a texture) and may be a per-scene or per-parameter setting.

## Who is affected

- Monochromatic renders and spectral bands outside the visible range, such as a 1064 nm laser scene with RGB-colored mounts and walls.
- The infrared and XUV work of [physoptics-gratings-and-xuv.md](physoptics-gratings-and-xuv.md) and [physoptics-optical-materials.md](physoptics-optical-materials.md), whose materials are measured and so are unaffected, but whose surroundings are often RGB.
- `bin/spectrum_test`, whose uplift checks assert today's hold.
