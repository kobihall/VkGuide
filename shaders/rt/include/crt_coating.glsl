// COATINGS ON THE GPU (docs/plans/completed/physoptics-optical-materials.md): what replaces an optical surface's bare Fresnel
// term, read from binding 56. Each table holds R and T for s and p over a wavelength grid (three columns, red, green and
// blue, in exact RGB) and 91 cosines, for a ray from the surface's plus side and one from its minus side
// (src/optic_coating.h CoatingTable). coatingLookup() is the line-for-line mirror of lookupCoatingTable() in
// src/optic_coating.cpp, and coatingExact() of transferMatrix(), which Exact coatings evaluates at every hit instead.
//
// A TABLE in binding 56, from its header's offset:
//   [0] uint columns, flags (CRT_COATING_*), layer count, kind (0 a table only, 1 a stack, 2 a metal)
//   [1] the first column's wavelength, the step between columns (nm), the design wavelength, 0
//   [2 ...] the data, [direction][column][sample], (Rs, Rp, Ts, Tp)
//   then, for Exact coatings, three vec4 per layer from the plus side (the thickness in m and n at 630, 532 and 465 nm;
//   k at the same; the index's and the absorption's SceneSpectra entries), and for a metal one more for the metal (its
//   eta and k at the three wavelengths, its eta's and k's entries)
//
// Requires crt_common.glsl, crt_spectrum.glsl and crt_fresnel.glsl.

#ifndef CRT_COATING_GLSL
#define CRT_COATING_GLSL

#define CRT_COATING_ANGLES 91u
// the header's flags
#define CRT_COATING_TRANSMITTED_FROM_PLUS 1u
#define CRT_COATING_TRANSMITTED_FROM_MINUS 2u
#define CRT_COATING_RGB 4u
#define CRT_COATING_LAYERS 8u
// nothing ever goes through it (a metal, an ideal mirror, an absorber): no refraction to disperse
#define CRT_COATING_OPAQUE 16u
#define CRT_COATING_KIND_METAL 2u

uvec4 coatingHeader(uint table)
{
	return floatBitsToUint(coatingData[table]);
}

// the table's fractional column for a wavelength, or a channel's column in an RGB table
float coatingColumn(uint table, float lambda)
{
	const vec4 grid = coatingData[table + 1u];
	return grid.y > 0.0 ? (lambda - grid.x) / grid.y : 0.0;
}

// (Rs, Rp, Ts, Tp) for a ray arriving from the plus side (direction 0) or the minus side (1) with the incident cosine
// `cosIncident`, eta = n_incident / n_other. The table's cosine is the one in the pair's lower-index medium: the
// incident one entering a denser medium, the transmitted one leaving it, past whose critical angle all is reflected
vec4 coatingLookup(uint table, uint direction, float column, float cosIncident, float eta)
{
	const uvec4 header = coatingHeader(table);
	const uint columns = header.x;
	float u = cosIncident;
	const uint transmittedAxis = direction == 0u ? (header.y & CRT_COATING_TRANSMITTED_FROM_PLUS) : (header.y & CRT_COATING_TRANSMITTED_FROM_MINUS);
	if (transmittedAxis != 0u) {
		const float sin2 = eta * eta * (1.0 - cosIncident * cosIncident);
		if (sin2 >= 1.0) {
			return vec4(1.0, 1.0, 0.0, 0.0);
		}
		u = sqrt(1.0 - sin2);
	}
	// the inverse of u = 1 - cos(pi j / 180)
	const float a = acos(1.0 - clamp(u, 0.0, 1.0)) * 0.6366197723675814 * float(CRT_COATING_ANGLES - 1u);
	const uint a0 = min(uint(a), CRT_COATING_ANGLES - 2u);
	const float fa = a - float(a0);
	const float c = clamp(column, 0.0, float(columns - 1u));
	const uint c0 = columns > 1u ? min(uint(c), columns - 2u) : 0u;
	const uint c1 = columns > 1u ? c0 + 1u : 0u;
	const float fc = c - float(c0);
	const uint base = table + 2u + direction * columns * CRT_COATING_ANGLES;
	const vec4 lo0 = coatingData[base + c0 * CRT_COATING_ANGLES + a0];
	const vec4 lo1 = coatingData[base + c0 * CRT_COATING_ANGLES + a0 + 1u];
	const vec4 hi0 = coatingData[base + c1 * CRT_COATING_ANGLES + a0];
	const vec4 hi1 = coatingData[base + c1 * CRT_COATING_ANGLES + a0 + 1u];
	const vec4 lo = lo0 + fa * (lo1 - lo0);
	const vec4 hi = hi0 + fa * (hi1 - hi0);
	return lo + fc * (hi - lo);
}

//---------------------------------------------------------------- the transfer matrix, at every hit

vec2 coatingCos(vec2 z)
{
	return vec2(cos(z.x) * cosh(z.y), -sin(z.x) * sinh(z.y));
}

vec2 coatingSin(vec2 z)
{
	return vec2(sin(z.x) * cosh(z.y), cos(z.x) * sinh(z.y));
}

// q = N cos(theta) for Macleod's N = n - ik and the incident invariant alpha = n0 sin(theta0), in the fourth quadrant
vec2 coatingTiltedIndex(vec2 n, float alpha)
{
	vec2 q = complexSqrt(complexMul(n, n) - vec2(alpha * alpha, 0.0));
	if (q.x < 0.0 || (q.x == 0.0 && q.y > 0.0)) {
		q = -q;
	}
	return q;
}

// a layer's index at a wavelength as Macleod's n - ik: in exact RGB the channel's stored values, in a spectral build the
// layer's spectra at the path's lane
vec2 coatingLayerIndex(uint layer, uint channel, uint lane, float lambda, bool metal)
{
#ifndef CRT_SPECTRAL
	const vec4 n = coatingData[layer];
	const vec4 k = coatingData[layer + 1u];
	return vec2(n[channel + 1u], -k[channel]);
#else
	const uvec4 refs = floatBitsToUint(coatingData[layer + 2u]);
	const float n = scalarFromRef(sceneSpectrumRef(refs.x), lane);
	const float second = scalarFromRef(sceneSpectrumRef(refs.y), lane);
	// a metal's second spectrum is k; a film's is its absorption coefficient, alpha = 4 pi k / lambda
	const float k = metal ? second : second * lambda * 1e-9 / (4.0 * 3.14159265358979);
	return vec2(n, -k);
#endif
}

// one polarization's (R, T) from the tilted admittances (src/optic_coating.cpp characteristic())
vec2 coatingCharacteristic(uint table, uint direction, uint layers, uint channel, uint lane, float lambda, float alpha, vec2 y0, vec2 ys, bool p)
{
	const uvec4 header = coatingHeader(table);
	const uint data = table + 2u + 2u * header.x * CRT_COATING_ANGLES;
	vec2 B = vec2(1.0, 0.0);
	vec2 C = ys;
	// from the substrate up: the layers from the incident side's far end
	for (uint step = 0u; step < layers; step++) {
		const uint j = direction == 0u ? layers - 1u - step : step;
		const uint layer = data + 3u * j;
		const vec2 n = coatingLayerIndex(layer, channel, lane, lambda, false);
		const vec2 q = coatingTiltedIndex(n, alpha);
		const vec2 y = p ? complexDiv(complexMul(n, n), q) : q;
		const vec2 delta = (2.0 * 3.14159265358979 * coatingData[layer].x / (lambda * 1e-9)) * q;
		const vec2 c = coatingCos(delta);
		const vec2 s = coatingSin(delta);
		const vec2 i = vec2(0.0, 1.0);
		const vec2 nb = complexMul(c, B) + complexMul(complexDiv(complexMul(i, s), y), C);
		const vec2 nc = complexMul(complexMul(complexMul(i, y), s), B) + complexMul(c, C);
		B = nb;
		C = nc;
	}
	const vec2 denominator = complexMul(y0, B) + C;
	const vec2 r = complexDiv(complexMul(y0, B) - C, denominator);
	const float R = clamp(complexNorm(r), 0.0, 1.0);
	const float T = clamp(4.0 * y0.x * ys.x / complexNorm(denominator), 0.0, 1.0 - R);
	return vec2(R, T);
}

// (Rs, Rp, Ts, Tp) of a stack or metal at the path's lane, evaluated now: what Exact coatings uses in place of the table.
// `nIncident` and `nOther` are the surface's media, n + ik, at the wavelength
vec4 coatingExact(uint table, uint direction, uint channel, uint lane, float lambda, float cosIncident, vec2 nIncident, vec2 nOther)
{
	const uvec4 header = coatingHeader(table);
	const bool metal = header.w == CRT_COATING_KIND_METAL;
	const float n0 = nIncident.x;
	const float cos0 = max(cosIncident, 1e-6);
	const float alpha = n0 * sqrt(max(0.0, 1.0 - cosIncident * cosIncident));
	// the substrate: the other medium, or the metal (opaque, and from behind the only thing met)
	vec2 ns = vec2(nOther.x, -nOther.y);
	if (metal) {
		const uint metalRecord = table + 2u + 2u * header.x * CRT_COATING_ANGLES + 3u * header.z;
		ns = coatingLayerIndex(metalRecord, channel, lane, lambda, true);
	}
	const vec2 qs = coatingTiltedIndex(ns, alpha);
	// from behind a mirror the glass meets the metal directly: no layers between
	const uint layers = metal && direction == 1u ? 0u : header.z;
	const vec2 s = coatingCharacteristic(table, direction, layers, channel, lane, lambda, alpha, vec2(n0 * cos0, 0.0), qs, false);
	const vec2 p = coatingCharacteristic(table, direction, layers, channel, lane, lambda, alpha, vec2(n0 / cos0, 0.0), complexDiv(complexMul(ns, ns), qs), true);
	vec4 out4 = vec4(s.x, p.x, s.y, p.y);
	if (metal) {
		out4.zw = vec2(0.0);
	}
	return out4;
}

#endif
