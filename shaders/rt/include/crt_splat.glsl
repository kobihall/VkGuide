// SPLATS (docs/plans/completed/pbrt-bidirectional-path-tracing.md; pbrt-v4's RGBFilm::AddSplat()): a contribution to an
// arbitrary point of the film, which a connection to the camera lands at, spread over the pixels within the pixel
// filter's radius by the filter's value there, the signed one of binding 30's table. Without the filtered camera rays
// (kernel 00's box variant, or antialiasing off) the filter is a box of half a pixel: the splat goes to the pixel it falls
// in. Binding 42 holds three floats a pixel, which kernel 09 folds into its resolve under CRT_SPLATS, divided by the
// pixel's samples and by the filter's integral (CrtParams::splatNormalization).
// Requires crt_common.glsl, with the including kernel declaring bindings 30 and 42.

#ifndef CRT_SPLAT_GLSL
#define CRT_SPLAT_GLSL

void splatPixel(uint x, uint y, vec3 rgb)
{
	const uint base = 3u * (y * pc.width + x);
	atomicAdd(splats[base], rgb.r);
	atomicAdd(splats[base + 1u], rgb.g);
	atomicAdd(splats[base + 2u], rgb.b);
}

// the signed filter at an offset from a pixel's center, in pixels, from its table: 0 outside its radius
float splatFilter(vec2 offset)
{
	const vec2 cell = (offset / filterRadius + 1.0) * 0.5 * vec2(float(filterWidth), float(filterHeight));
	if (cell.x < 0.0 || cell.y < 0.0 || cell.x >= float(filterWidth) || cell.y >= float(filterHeight)) {
		return 0.0;
	}
	const uint column = min(uint(cell.x), filterWidth - 1u);
	const uint row = min(uint(cell.y), filterHeight - 1u);
	// after the marginal cdf, the row cdfs and |f|: the signed f
	const uint signedBase = filterHeight + 1u + filterHeight * (filterWidth + 1u) + filterHeight * filterWidth;
	return filterData[signedBase + row * filterWidth + column];
}

// `film` is the point of the image, u right and v up over [0, 1)^2, as crt_camera.glsl's cameraWe() reports it
void splatAdd(vec2 film, vec3 rgb)
{
	if (!(film.x >= 0.0 && film.x < 1.0 && film.y >= 0.0 && film.y < 1.0)) {
		return;
	}
	// the continuous raster position, rows from the top
	const vec2 p = vec2(film.x * float(pc.width), (1.0 - film.y) * float(pc.height));
	if ((pc.bidirFlags & CRT_BIDIR_FLAG_FILTERED_SPLATS) == 0u) {
		splatPixel(min(uint(p.x), pc.width - 1u), min(uint(p.y), pc.height - 1u), rgb);
		return;
	}
	const ivec2 lo = max(ivec2(ceil(p - 0.5 - filterRadius)), ivec2(0));
	const ivec2 hi = min(ivec2(floor(p - 0.5 + filterRadius)), ivec2(int(pc.width) - 1, int(pc.height) - 1));
	for (int y = lo.y; y <= hi.y; y++) {
		for (int x = lo.x; x <= hi.x; x++) {
			const float weight = splatFilter(p - vec2(float(x) + 0.5, float(y) + 0.5));
			if (weight != 0.0) {
				splatPixel(uint(x), uint(y), weight * rgb);
			}
		}
	}
}

#endif
