// The sRGB transfer function, for the raster preview: its draw image holds linear color, and the Viewport shows it
// through viewport_encode.comp. A background effect whose colors are display values decodes them first, so that it looks
// as it always has
vec3 srgbEncode(vec3 linear)
{
	const vec3 c = clamp(linear, 0.0, 1.0);
	return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 srgbDecode(vec3 encoded)
{
	const vec3 c = clamp(encoded, 0.0, 1.0);
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
