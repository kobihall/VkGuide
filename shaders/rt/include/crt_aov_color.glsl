// A color that tells one id from the next, for the views of the auxiliary outputs: the same id always gives the
// same color. Mirrors aovIdColor() in src/aov.cpp. No dependencies, so a shader that is not a path tracing kernel
// can include it.

vec3 aovIdColor(uint id)
{
	// PCG's output permutation, so neighboring ids get unrelated colors; kept off black so every id shows
	uint h = id * 747796405u + 2891336453u;
	h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
	h = (h >> 22u) ^ h;
	const vec3 bytes = vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u));
	return bytes * (0.75 / 255.0) + vec3(0.25);
}
