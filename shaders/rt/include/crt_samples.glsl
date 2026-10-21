// The random numbers kernels 00 and 06 consume, from the RaySamples record kernel 01 writes or from
// the path's PCG stream. docs/plans/completed/pbrt-sample-generation.md.
//
// A consumer asks for a FIELD of the record by name and hands over the path's PCG state:
//
//     const float u = sampleDimension(pathIndex, rng, CRT_SAMPLE_INDIRECT_LOBE);
//
// With the selected sampler writing no record (CRT_SAMPLE_RECORD off) the field is ignored
// and the answer is randomFloat(rng), so a consumer that asks in the order it always drew gets
// exactly the numbers it always got, and the render is bit-identical. With a record, the answer is
// the record's value, a pure function of (pixel, sample index, bounce) that a low-discrepancy
// sampler has stratified, and `rng` is not touched.
//
// A consumer that needs more numbers than the record holds (a metal's fuzz radius, a layered
// BSDF's random walk, extra light samples) draws the rest from `rng` with randomFloat() as pbrt-v4
// does: those are independent of the record in either mode. Whatever it asks for must be asked in
// the same order on every path of a given strategy, so that a field is always the same dimension of
// the sequence (PBR 4ed 8.3); the layout below guarantees it for everything the record covers.
//
// The field numbers are the dimension offsets within a bounce, src/sampler.h SAMPLE_*. Requires
// crt_common.glsl and crt_random.glsl.

// direct: next-event estimation
#define CRT_SAMPLE_DIRECT_LIGHT 0u
// two fields: the point on the light
#define CRT_SAMPLE_DIRECT_POINT 1u
// indirect: the continuation
#define CRT_SAMPLE_INDIRECT_LOBE 4u
// two fields: the direction
#define CRT_SAMPLE_INDIRECT_DIRECTION 5u
#define CRT_SAMPLE_INDIRECT_ROULETTE 7u
// the camera pass of kernel 01 writes dimensions 0 to 5 into fields 0 to 5
#define CRT_SAMPLE_CAMERA_PIXEL 0u
#define CRT_SAMPLE_CAMERA_LENS 2u
#define CRT_SAMPLE_CAMERA_TIME 4u
#define CRT_SAMPLE_CAMERA_WAVELENGTH 5u

// Mixed into a path's PCG seed when a record exists, so that the stream consumers spill into is
// not the one the independent sampler fills the camera's dimensions from
#define CRT_RECORD_STREAM_SALT 0x5bd1e995u

// Whether the sampler writes the record. A specialization constant, set when the pipeline is built:
// off, the branches below are gone before the compiler optimizes, and a kernel is the code it was
// before there was a record. A run-time flag was not that: Metal's compiler produced one ulp of
// difference around a branch it could not fold (docs/plans/completed/pbrt-sample-generation.md)
layout (constant_id = 0) const bool CRT_SAMPLE_RECORD = false;

float sampleRecordField(uint pathIndex, uint field)
{
	if (field < 4u) {
		return raySamples[pathIndex].direct[field];
	}
	if (field < 8u) {
		return raySamples[pathIndex].indirect[field - 4u];
	}
	return raySamples[pathIndex].medium[field - 8u];
}

vec2 sampleRecordField2D(uint pathIndex, uint field)
{
	return vec2(sampleRecordField(pathIndex, field), sampleRecordField(pathIndex, field + 1u));
}

// THE ALBEDO ESTIMATE'S SAMPLES. Under CRT_AOV, kernel 06 estimates the BSDF's directional albedo at the first
// surface a camera path meets by sampling the BSDF 16 times with the fixed numbers pbrt-v4's surfscatter.cpp uses
// (ucRho, uRho: the lobe choice and the direction), and averaging the weights. While g_albedoSample is the index of
// one of them, sampleDimension() returns them instead of the path's own numbers, so the estimate needs no sampling
// code of its own and never touches the path's stream or its record. -1 the rest of the time, and with CRT_AOV off
// the branch is gone
#define CRT_ALBEDO_SAMPLES 16
int g_albedoSample = -1;

const float CRT_ALBEDO_UC[CRT_ALBEDO_SAMPLES] = float[CRT_ALBEDO_SAMPLES](
	0.75741637, 0.37870818, 0.7083487, 0.18935409, 0.9149363, 0.35417435, 0.5990858, 0.09467703,
	0.8578725, 0.45746812, 0.686759, 0.17708716, 0.9674518, 0.2995429, 0.5083201, 0.047338516);
const vec2 CRT_ALBEDO_U[CRT_ALBEDO_SAMPLES] = vec2[CRT_ALBEDO_SAMPLES](
	vec2(0.855985, 0.570367), vec2(0.381823, 0.851844), vec2(0.285328, 0.764262), vec2(0.733380, 0.114073),
	vec2(0.542663, 0.344465), vec2(0.127274, 0.414848), vec2(0.964700, 0.947162), vec2(0.594089, 0.643463),
	vec2(0.095109, 0.170369), vec2(0.825444, 0.263359), vec2(0.429467, 0.454469), vec2(0.244460, 0.816459),
	vec2(0.756135, 0.731258), vec2(0.516165, 0.152852), vec2(0.180888, 0.214174), vec2(0.898579, 0.503897));

// the albedo estimate's value for a field of the record: the lobe, then the two direction numbers; the fields the
// BSDF samples do not read answer the middle
float albedoSampleField(int sampleIndex, uint field)
{
	if (field == CRT_SAMPLE_INDIRECT_LOBE) {
		return CRT_ALBEDO_UC[sampleIndex];
	}
	if (field == CRT_SAMPLE_INDIRECT_DIRECTION) {
		return CRT_ALBEDO_U[sampleIndex].x;
	}
	if (field == CRT_SAMPLE_INDIRECT_DIRECTION + 1u) {
		return CRT_ALBEDO_U[sampleIndex].y;
	}
	return 0.5;
}

float sampleDimension(uint pathIndex, inout uint rng, uint field)
{
	if (CRT_AOV && g_albedoSample >= 0) {
		return albedoSampleField(g_albedoSample, field);
	}
	if (!CRT_SAMPLE_RECORD) {
		return randomFloat(rng);
	}
	return sampleRecordField(pathIndex, field);
}

// two consecutive fields, the first the x
vec2 sampleDimension2D(uint pathIndex, inout uint rng, uint field)
{
	const float x = sampleDimension(pathIndex, rng, field);
	const float y = sampleDimension(pathIndex, rng, field + 1u);
	return vec2(x, y);
}

//---------------------------------------------------------------- warps of a sample

// A point on the unit sphere from two uniform numbers, the one randomUnitVector() draws and so as
// uniform on the sphere as the numbers are
vec3 unitVectorFromSample(vec2 u)
{
	float cosTheta = -1.0 + (1.0 - -1.0) * u.x;
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	float phi = 2.0 * CRT_PI * u.y;
	return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// the point of the unit disk randomInDisk() draws
vec2 diskFromSample(vec2 u)
{
	float r = sqrt(u.x);
	float phi = 2.0 * CRT_PI * u.y;
	return r * vec2(cos(phi), sin(phi));
}

// a point inside the unit ball, the one randomInBall() draws: a direction scaled by the cube root
// of a third number, which the record has no dimension for and so comes from the PCG stream
vec3 ballFromSample(vec2 u, float radius)
{
	return unitVectorFromSample(u) * pow(radius, 1.0 / 3.0);
}

// a direction around `normal` with a cos^alpha lobe, the one randomInHemisphere() draws
vec3 hemisphereFromSample(vec3 normal, float alpha, vec2 u)
{
	float cosTheta = pow(u.x, 1.0 / (alpha + 1.0));
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	float phi = 2.0 * CRT_PI * u.y;
	vec3 tangentSpaceDir = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
	return getTangentSpace(normal) * tangentSpaceDir;
}
