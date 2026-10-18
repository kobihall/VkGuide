# Display transform and color

The shared tonemap pass (`shaders/tonemap.comp`) scales the linear image by the camera's exposure, clamps it to [0, 1] and takes the square root, the "gamma 2" of Ray Tracing in One Weekend. That is not the sRGB transfer curve, it clips highlights hard, and it offers no white balance and no way to express exposure the way a photographer would. pbrt's film and its `imgtool` add a sensor model with ISO and shutter time, von Kries white balance, output color spaces, and Reinhard and ACES filmic tone curves. This plan turns the tonemap pass into a display transform with those stages.

*Not implemented. Depends on [pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md). Color temperatures and sensor curves come from [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- `TonemapPass` (`src/vk_tonemap.h`) runs one compute shader over the accumulation image into the rgba8 display image: `sqrt(clamp(c × scale, 0, 1))`, with NaN set to zero. `scale` is the render camera's `exposure` (`RaytraceRenderer::record()`).
- Exposure is a plain multiplier on `SceneCamera`, applied every frame without restarting the render.
- The accumulation is linear RGB with Rec. 709 primaries.
- The swapchain is `B8G8R8A8_UNORM` with `SRGB_NONLINEAR` color space (`vk_engine.cpp`), so the display image's values reach the screen as written: the encoding is the shader's job.

## Design

The pass becomes a fixed chain of stages, each a function in `shaders/display/display_ops.glsl`, selected by a push-constant switch that is uniform over the image:

```text
linear working RGB → exposure → white balance → tone curve → gamut mapping → output encoding → rgba8
```

Every stage is a display setting, so none of them restarts a render, like exposure today. They live on `SceneCamera` beside `exposure`, since they describe how that camera's picture is developed.

### Exposure

The photographic modes apply to radiance × 683, undoing the import's division by 683 (`PHOTOMETRIC_TO_RADIOMETRIC`), so a light authored in candela or lux exposes as it would in front of a camera.

| Mode | Multiplier | Use |
|---|---|---|
| Scale | `exposure` | today's, the default |
| pbrt sensor | `shutter × ISO / 100` | pbrt-v4's imaging ratio (`film.cpp`, PBR 4ed §5.4.2), which ignores the f-number; for matching pbrt renders ([pbrt-scene-import.md](pbrt-scene-import.md)) |
| Camera | `1 / (1.2 × 2^EV100)`, `EV100 = log2(N² / t × 100 / ISO)` | the saturation-based sensitivity of ISO 12232 as Lagarde and de Rousiers 2014 use it: f-number, shutter and ISO behave as on a camera |

The thin lens's aperture radius, which sets depth of field, stays independent of the f-number in the **Camera** mode, as on a real camera whose focal length is not modeled. The realistic camera computes its aperture's effect on irradiance physically ([pbrt-cameras.md](pbrt-cameras.md)), so with it only the shutter and ISO terms apply.

### White balance

pbrt's von Kries transform (PBR 4ed §5.4.2): convert to XYZ, then to LMS cone space, divide by the source white's LMS and multiply by the target white's, and come back. The source white is a color temperature (a blackbody's chromaticity) or a named illuminant; the target is D65, the working space's white. A 3 × 3 matrix per frame, applied in every spectral mode including exact RGB.

### Tone curves

| Curve | Notes |
|---|---|
| Clamp, gamma 2 | today's, for bit-identical comparison |
| Clamp | linear values clipped at 1, then encoded properly |
| Reinhard | extended Reinhard on the channel average, `(1 + L / Lw²) / (1 + L)` with white point `Lw`, as `imgtool convert --tonemap --maxluminance` applies it |
| ACES filmic | Narkowicz's per-channel fit, as `imgtool convert --acesfilmic` applies it |
| Khronos PBR Neutral | Khronos's hue-preserving curve for product renders, from outside the books |
| False color | luminance mapped through viridis, `imgtool falsecolor`'s map, with a log scale |

The false-color curve also serves the error and variance views of [pbrt-verification-harness.md](pbrt-verification-harness.md) and [pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md).

### Gamut mapping

Spectral rendering can produce colors outside Rec. 709, carried as negative components ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)). The display maps them in:

- **Clip** each channel, the cheapest, and `imgtool`'s default.
- **Scale by the largest component** when it exceeds 1, `imgtool convert --preservecolors`, which keeps the channel ratios.
- **Desaturate toward the luminance** until the color fits, which keeps luminance and hue at the cost of saturation.

### Output encoding

The piecewise sRGB transfer function replaces the square root. The storage image stays `R8G8B8A8_UNORM`, since sRGB formats cannot be storage images on most drivers, and the swapchain stays `UNORM`, so the shader writes encoded values.

Wide-gamut output (Display P3 on this Mac's panel, through `VK_EXT_swapchain_colorspace` if MoltenVK exposes it) is a later option: the working space stays Rec. 709, and a P3 display matrix follows the gamut mapping. Saved images take the color spaces `imgtool convert --colorspace` offers (sRGB, DCI-P3, Rec. 2020, ACES2065-1), which [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md) writes into EXR metadata.

### Wavelength display colors

`wavelengthDisplayColor(λ)` turns one wavelength into an sRGB color: the CIE 1931 color of a line at λ, normalized and gamut-mapped, inside the visible range, and the false-color map's color for λ outside it (deep red for the infrared, violet for the ultraviolet, and a configurable color for XUV). The beam preview ([physoptics-beam-preview.md](physoptics-beam-preview.md)) colors laser beams with it, and the lasers of [physoptics-laser-sources.md](physoptics-laser-sources.md) take their exact-RGB color from it, as femtoPro colors its beams from their spectra.

### Glare

pbrt's `imgtool bloom` adds a thresholded, blurred copy of the bright pixels. It is a post effect, not a physical one, and is off by default. It needs a separable blur over the image before the tone curve: two extra dispatches in the pass.

## Verification

- With **Clamp, gamma 2** and a scale exposure, the display image is bit-identical to today's.
- A gray ramp from 0 to 1 displayed with **Clamp** and the sRGB curve reads 188 at 0.5 linear, where gamma 2 reads 180.
- A scene lit by a 2700 K blackbody light and white-balanced to 2700 K shows white surfaces as white.
- In **Camera** mode at ISO 100, 1/100 s and f/16 (the "sunny 16" rule), an 18 % gray card facing a 100,000 lux sun has luminance 5730 cd/m² and displays at a linear 0.187 before the tone curve.

## Order of work

1. The stage chain with today's behavior, and the sRGB curve.
2. Tone curves and gamut mapping.
3. White balance.
4. Photographic exposure.
5. False color for the debug views.
6. Glare and wide-gamut output, if wanted.

## Sources

PBR 4ed §4.6 (XYZ, RGB color spaces), §5.4.2 (sensor response, imaging ratio, chromatic adaptation and white balance), §B.5.6 (color encodings), chapter 4 "Further Reading" (tone reproduction, gamut mapping); pbrt-v4 `src/pbrt/film.cpp` (`PixelSensor`), `util/color.h` (`WhiteBalance`), `cmd/imgtool.cpp` (`convert`, `bloom`, `falsecolor`); Lagarde and de Rousiers 2014, *Moving Frostbite to physically based rendering* §4.2 (EV100 and the saturation-based exposure); Narkowicz 2015 (ACES filmic fit); Khronos PBR Neutral specification.
