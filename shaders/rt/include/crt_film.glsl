// The film arithmetic kernel 09's compensated variants run per pixel: the clamp, the Neumaier sums of
// sum(w L) and sum(w), Welford's running variance of the luminance, and the resolve. The line-for-line
// mirror of src/film.cpp (src/film.h): change both together and run bin/film_test.
// docs/plans/completed/pbrt-film-and-reconstruction.md.
//
// A float sum loses the low bits of every term once the total has outgrown them. Neumaier's
// compensation keeps what each addition rounded away in a second float and adds it back when the sum is
// read, which holds roughly twice float's precision while the number of terms times float's epsilon is
// well below 1 (a pixel takes thousands of samples; the radeon here has no shaderFloat64, which is what
// pbrt's pixels use). The compensation must not be reassociated away, so every operation that
// matters is `precise`, and the sample's weight is +1 or -1 so that w * L is exact.
// Requires crt_common.glsl.

vec3 filmClampSample(vec3 rgb, float maxComponent)
{
	const float peak = max(rgb.x, max(rgb.y, rgb.z));
	if (maxComponent > 0.0 && peak > maxComponent) {
		return rgb * (maxComponent / peak);
	}
	return rgb;
}

float filmLuminance(vec3 rgb)
{
	return 0.2126 * rgb.x + 0.7152 * rgb.y + 0.0722 * rgb.z;
}

// Neumaier's add of x to (sum, compensation)
void compensatedAdd(inout float total, inout float compensation, float x)
{
	precise float t = total + x;
	if (abs(total) >= abs(x)) {
		precise float lost = (total - t) + x;
		compensation += lost;
	} else {
		precise float lost = (x - t) + total;
		compensation += lost;
	}
	total = t;
}

// one accepted sample into the pixel's record: the clamp, the weighted sums, the variance
void filmAddSample(inout FilmPixel pixel, vec3 rgb, float weight, float clampComponent)
{
	const vec3 clamped = filmClampSample(rgb, clampComponent);

	compensatedAdd(pixel.sum.x, pixel.compensation.x, weight * clamped.x);
	compensatedAdd(pixel.sum.y, pixel.compensation.y, weight * clamped.y);
	compensatedAdd(pixel.sum.z, pixel.compensation.z, weight * clamped.z);
	compensatedAdd(pixel.sum.w, pixel.compensation.w, weight);

	// Welford's update of the luminance's mean and sum of squared deviations
	const float luminance = filmLuminance(clamped);
	pixel.accepted++;
	const float delta = luminance - pixel.welfordMean;
	pixel.welfordMean += delta / float(pixel.accepted);
	const float delta2 = luminance - pixel.welfordMean;
	pixel.welfordM2 += delta * delta2;
}

// the pixel's value, sum(w L) / sum(w); black where the weights sum to zero
vec3 filmResolve(FilmPixel pixel)
{
	const float weightSum = pixel.sum.w + pixel.compensation.w;
	if (weightSum == 0.0) {
		return vec3(0.0);
	}
	return vec3(pixel.sum.x + pixel.compensation.x, pixel.sum.y + pixel.compensation.y, pixel.sum.z + pixel.compensation.z) / weightSum;
}

// the standard error of the mean over the mean: sqrt(M2 / (n - 1) / n) / max(|mean|, floor). 0 below two samples
float filmRelativeStandardError(FilmPixel pixel, float floorValue)
{
	if (pixel.accepted < 2u) {
		return 0.0;
	}
	const float n = float(pixel.accepted);
	const float standardError = sqrt(max(pixel.welfordM2, 0.0) / (n - 1.0) / n);
	return standardError / max(abs(pixel.welfordMean), floorValue);
}

//---------------------------------------------------------------- outlier reweighting
//
// The line-for-line mirror of the cascade section of src/film.cpp (cascadeAddSample(), cascadeCount(),
// outlierResolve()): Zirr, Hanika and Dachsbacher 2018, "Re-weighting firefly samples for improved
// finite-sample Monte Carlo estimates". A pixel keeps its samples in CRT_OUTLIER_BUFFERS buffers by
// brightness, a sample split between the two that bracket its luminance so that dividing a buffer's
// luminance sum by b^j counts the samples of that brightness (equation 19), and the resolve weighs each
// buffer by how well its brightness was sampled. See film.h for the estimator.

#define CRT_OUTLIER_BUFFERS 6u
// the buffers' brightness ratio, 8: b^j is exp2(3 j)
#define CRT_OUTLIER_MIN_COUNT 1.0

float cascadeLevelValue(uint level)
{
	return exp2(3.0 * float(level));
}

// one accepted sample (after the clamp) into the pixel's buffers
void cascadeAddSample(uint pixel, vec3 rgb, float weight)
{
	const float luminance = max(filmLuminance(rgb), 0.0);
	uint level = 0u;
	float alpha = 1.0;
	// dimmer than 1 goes whole to the first buffer, beyond the top whole to the last
	if (luminance >= 1.0) {
		const float exponent = floor(log2(luminance) / 3.0);
		if (exponent >= float(CRT_OUTLIER_BUFFERS - 1u)) {
			level = CRT_OUTLIER_BUFFERS - 1u;
		} else {
			level = uint(exponent);
			alpha = clamp((cascadeLevelValue(level) / luminance - 1.0 / 8.0) / (1.0 - 1.0 / 8.0), 0.0, 1.0);
		}
	}
	filmCascade[pixel * CRT_OUTLIER_BUFFERS + level] += vec4(alpha * weight * rgb, alpha * luminance);
	if (level + 1u < CRT_OUTLIER_BUFFERS) {
		filmCascade[pixel * CRT_OUTLIER_BUFFERS + level + 1u] += vec4((1.0 - alpha) * weight * rgb, (1.0 - alpha) * luminance);
	}
}

// the samples of brightness around b^level the pixel holds (equation 19), fractional
float cascadeCount(uint pixel, uint level)
{
	float count = 0.0;
	const uint last = min(level + 1u, CRT_OUTLIER_BUFFERS - 1u);
	for (uint k = (level > 0u ? level - 1u : 0u); k <= last; k++) {
		count += filmCascade[pixel * CRT_OUTLIER_BUFFERS + k].w / cascadeLevelValue(k);
	}
	return count;
}

// the pixel's reweighted value, its neighborhood being the existing pixels of the 3 x 3 block around `coord`
vec3 outlierResolve(uint pixel, ivec2 coord, uint accepted, float weightSum, float kappa)
{
	if (accepted == 0u || weightSum == 0.0) {
		return vec3(0.0);
	}
	const ivec2 size = ivec2(int(pc.width), int(pc.height));
	vec3 result = vec3(0.0);
	// the expected value of the buffers below, as the weighted composition so far
	float expected = 0.0;
	for (uint level = 0u; level < CRT_OUTLIER_BUFFERS; level++) {
		// the neighborhood's mean count of this brightness: a brightness nobody nearby has seen is rejected
		float neighborhood = 0.0;
		float neighbors = 0.0;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				const ivec2 p = coord + ivec2(dx, dy);
				if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) {
					continue;
				}
				neighborhood += cascadeCount(uint(p.y) * uint(size.x) + uint(p.x), level);
				neighbors += 1.0;
			}
		}
		neighborhood /= neighbors;

		float countWeight = 0.0;
		if (neighborhood >= CRT_OUTLIER_MIN_COUNT) {
			countWeight = min(1.0, cascadeCount(pixel, level) / kappa);
		}
		// a sample under N E / kappa cannot be a firefly, whatever its count
		const float valueWeight = min(1.0, float(accepted) * expected / (kappa * cascadeLevelValue(level)));
		const float weight = max(countWeight, valueWeight);
		result += weight * filmCascade[pixel * CRT_OUTLIER_BUFFERS + level].xyz / weightSum;
		expected = filmLuminance(result);
	}
	return result;
}
