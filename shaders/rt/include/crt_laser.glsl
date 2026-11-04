// LASERS (docs/plans/completed/physoptics-laser-sources.md): a laser's beam as its two light records carry it, the rays
// light tracing starts at it, the radiance a camera ray finds leaving its aperture, and how light tracing picks a light
// when the scene has lasers. The beam mirrors src/light_sampling.cpp (sampleLaserLe(), laserPdfLe(), laserRadiance())
// line for line, which bin/light_test checks; the comments there say what the densities are.
//
// Lasers are in neither of next-event estimation's tables: their records follow every other light's, from
// lightHeader.laserBase, with a table of their own in their first records' alias fields. Light tracing picks among
// both by power (sampleEmitterIndex()); the light vertex cache and pbrt 3's BDPT never start a path at a laser.
// Requires crt_common.glsl, crt_spectrum.glsl and crt_light.glsl. Every call is behind CRT_LASERS.

#ifndef CRT_LASER_GLSL
#define CRT_LASER_GLSL

struct LaserEmitter {
	vec3 center;
	vec3 axis;
	vec3 xAxis;
	vec2 waist;
	vec2 waistDistance;
	vec2 halfAngle;
};

// the beam of the laser whose first record is `index`
LaserEmitter laserEmitter(uint index)
{
	const GpuLight first = lights[index];
	const GpuLight beam = lights[index + 1u];
	LaserEmitter laser;
	laser.center = first.a.xyz;
	laser.axis = first.b.xyz;
	laser.xAxis = first.c.xyz;
	laser.waist = beam.a.xy;
	laser.waistDistance = beam.a.zw;
	laser.halfAngle = beam.b.xy;
	return laser;
}

vec2 sampleStandardNormal2(vec2 u)
{
	const float r = sqrt(-2.0 * log(max(1.0 - u.x, 1e-30)));
	const float phi = 2.0 * CRT_PI * u.y;
	return vec2(r * cos(phi), r * sin(phi));
}

float gaussian2(vec2 v, vec2 sigma)
{
	const vec2 t = v / sigma;
	return exp(-0.5 * dot(t, t)) / (2.0 * CRT_PI * sigma.x * sigma.y);
}

void laserDensities(LaserEmitter laser, vec2 x, vec2 slope, out float pdfPos, out float pdfDir)
{
	const vec2 sigmaW = 0.5 * laser.waist;
	const vec2 sigmaS = 0.5 * laser.halfAngle;
	const vec2 d = laser.waistDistance;
	const vec2 varianceX = sigmaW * sigmaW + d * d * sigmaS * sigmaS;
	pdfPos = gaussian2(x, sqrt(varianceX));
	const vec2 mean = d * sigmaS * sigmaS * x / varianceX;
	const vec2 sigmaGiven = sigmaW * sigmaS / sqrt(varianceX);
	const float cosTheta = 1.0 / sqrt(1.0 + dot(slope, slope));
	pdfDir = gaussian2(slope - mean, sigmaGiven) / (cosTheta * cosTheta * cosTheta);
}

// a ray leaving the beam's plane and its densities; the plane's normal is the axis
bool sampleLaserRay(LaserEmitter laser, vec2 uPos, vec2 uDir, out vec3 origin, out vec3 direction, out float pdfPos, out float pdfDir)
{
	const vec3 yAxis = cross(laser.axis, laser.xAxis);
	const vec2 slope = sampleStandardNormal2(uDir) * (0.5 * laser.halfAngle);
	const vec2 x = sampleStandardNormal2(uPos) * (0.5 * laser.waist) + laser.waistDistance * slope;
	origin = laser.center + x.x * laser.xAxis + x.y * yAxis;
	direction = normalize(slope.x * laser.xAxis + slope.y * yAxis + laser.axis);
	laserDensities(laser, x, slope, pdfPos, pdfDir);
	return pdfPos > 0.0 && pdfDir > 0.0;
}

void laserPdfLe(LaserEmitter laser, vec3 origin, vec3 direction, out float pdfPos, out float pdfDir)
{
	pdfPos = 0.0;
	pdfDir = 0.0;
	const float cosTheta = dot(direction, laser.axis);
	if (!(cosTheta > 0.0)) {
		return;
	}
	const vec3 yAxis = cross(laser.axis, laser.xAxis);
	const vec3 offset = origin - laser.center;
	const vec2 x = vec2(dot(offset, laser.xAxis), dot(offset, yAxis));
	const vec2 slope = vec2(dot(direction, laser.xAxis), dot(direction, yAxis)) / cosTheta;
	laserDensities(laser, x, slope, pdfPos, pdfDir);
}

// the beam's radiance per watt along the unit `direction` through `point`
float laserRadiancePerWatt(LaserEmitter laser, vec3 point, vec3 direction)
{
	const float cosTheta = dot(direction, laser.axis);
	if (!(cosTheta > 0.0)) {
		return 0.0;
	}
	const vec3 yAxis = cross(laser.axis, laser.xAxis);
	const vec3 offset = point - laser.center;
	const vec3 crossing = offset - (dot(offset, laser.axis) / cosTheta) * direction;
	const vec2 x = vec2(dot(crossing, laser.xAxis), dot(crossing, yAxis));
	const vec2 slope = vec2(dot(direction, laser.xAxis), dot(direction, yAxis)) / cosTheta;
	const float joint = gaussian2(x - laser.waistDistance * slope, 0.5 * laser.waist) * gaussian2(slope, 0.5 * laser.halfAngle);
	const float cos2 = cosTheta * cosTheta;
	return joint / (cos2 * cos2);
}

// What leaves laser `index` (its first record) along the unit `direction` through `point`: its power's color or spectrum
// times the radiance per watt. Nothing for an index that is not a laser's: a laser aperture material on anything else
Spectrum laserRadiance(uint index, vec3 point, vec3 direction)
{
	if (index == CRT_NO_LIGHT || lightKindOf(lights[index]) != CRT_LIGHT_LASER) {
		return spectrumConst(0.0);
	}
	return spectrumScale(lightSpectrum(lights[index]), laserRadiancePerWatt(laserEmitter(index), point, direction));
}

// LIGHT TRACING'S PICK when the scene has lasers: the full table with the chance of its power over every light's, the
// lasers' table otherwise, so every light is picked in proportion to its power over the total
uint sampleEmitterIndex(float u)
{
	const float share = lightHeader.count > 0u ? lightHeader.invEmitPower / lightHeader.invTotalPower : 0.0;
	if (u < share) {
		return sampleLightIndex(min(u / share, CRT_ONE_MINUS_EPSILON));
	}
	const uint count = lightHeader.laserCount;
	const float v = min((u - share) / (1.0 - share), CRT_ONE_MINUS_EPSILON);
	const uint offset = min(uint(v * float(count)), count - 1u);
	const float up = min(v * float(count) - float(offset), CRT_ONE_MINUS_EPSILON);
	const GpuLight first = lights[lightHeader.laserBase + 2u * offset];
	return lightHeader.laserBase + 2u * (up < first.aliasQ ? offset : first.aliasIndex);
}

// the chance sampleEmitterIndex() picks light `index`
float emitterPmf(uint index)
{
	return lights[index].power * lightHeader.invEmitPower;
}

#endif
