// EMITTED RAYS: a ray leaving a light, which every light path starts with (PBR 3ed 16.1.2; pbrt-v4's SampleLe() and
// PDF_Le()), and the densities of a ray a camera path found leaving one. The geometry mirrors src/light_sampling.cpp
// (sampleAreaLe(), areaPdfLe(), samplePointLe(), sampleSpotLe(), infiniteLightOrigin()), which bin/light_test checks:
//
//   point, goniometric   from the position: every direction alike, or by the goniometric image's table
//   spot                 from the position, uniform in the outer cone, scaled by its falloff
//   projection           from the position, through the frustum by its image's table (binding 65)
//   directional          from a disk of the scene's bounding sphere square to it, behind the scene
//   sphere, rectangle,   a point uniform by area, a direction cosine-weighted about the side it leaves from; a two-sided
//   cylinder, triangle   light picks the side with probability 1/2
//   environment          from the disk, towards the scene, in a direction drawn by the map's full table
//   laser                from its beam's plane, a point and a slope per axis from its Gaussian beam (crt_laser.glsl)
//
// A point light's range window falls off with the distance a light path travels to its first vertex, which the light
// path kernels apply (crt_light_path.glsl). Kept out of crt_light.glsl, so the path tracer's kernels are the code they
// were. Requires crt_common.glsl, crt_spectrum.glsl, crt_random.glsl, equirect.glsl and crt_light.glsl; the including
// kernel declares binding 65 (ImageDistributions) besides the light list's.

#ifndef CRT_LIGHT_EMISSION_GLSL
#define CRT_LIGHT_EMISSION_GLSL

#include "crt_warp.glsl"
#include "crt_image_table.glsl"
#include "crt_laser.glsl"

struct LightLeSample {
	vec3 origin;
	// an area light's surface normal at the origin; zero for every other light
	vec3 normal;
	// unit
	vec3 direction;
	// what leaves along the ray: an area light's radiance, a point-like light's intensity, an infinite light's
	// irradiance or radiance (src/rt_lights.h GpuLight::radiance)
	Spectrum radiance;
	// per unit of area (1 for a light at a point) and of solid angle (1 for a directional light)
	float pdfPos;
	float pdfDir;
	// the light has no area (point, spot, projection, goniometric), or a single direction (directional)
	bool deltaPosition;
	bool deltaDirection;
};

vec3 sampleCosineHemisphere(vec2 u)
{
	const vec2 d = sampleUniformDiskConcentric(u);
	const float z = lightSafeSqrt(1.0 - d.x * d.x - d.y * d.y);
	return vec3(d.x, d.y, z);
}

vec3 sampleUniformSphere(vec2 u)
{
	const float z = 1.0 - 2.0 * u.x;
	const float r = lightSafeSqrt(1.0 - z * z);
	const float phi = 2.0 * CRT_PI * u.y;
	return vec3(r * cos(phi), r * sin(phi), z);
}

vec3 sampleUniformCone(vec2 u, float cosMax)
{
	const float cosTheta = (1.0 - u.x) + u.x * cosMax;
	const float sinTheta = lightSafeSqrt(1.0 - cosTheta * cosTheta);
	const float phi = u.y * 2.0 * CRT_PI;
	return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float spotPdfDir(vec3 axis, float cosOuter, vec3 direction)
{
	return dot(axis, direction) >= cosOuter && cosOuter < 1.0 ? 1.0 / (2.0 * CRT_PI * (1.0 - cosOuter)) : 0.0;
}

vec3 infiniteLightOrigin(vec3 towardLight, vec2 uPos)
{
	vec3 x;
	vec3 y;
	lightFrameFromZ(towardLight, x, y);
	const vec2 disk = sampleUniformDiskConcentric(uPos);
	return pc.sceneSphere.xyz + pc.sceneSphere.w * (disk.x * x + disk.y * y) + pc.sceneSphere.w * towardLight;
}

float infiniteLightPdfPos()
{
	return pc.sceneSphere.w > 0.0 ? 1.0 / (CRT_PI * pc.sceneSphere.w * pc.sceneSphere.w) : 0.0;
}

// an area light's emitting surface's area
float areaLightArea(GpuLight light)
{
	switch (lightKindOf(light)) {
	case CRT_LIGHT_SPHERE:
		return 4.0 * CRT_PI * light.a.w * light.a.w;
	case CRT_LIGHT_RECTANGLE:
		return length(cross(light.b.xyz, light.c.xyz));
	case CRT_LIGHT_CYLINDER:
		return 2.0 * CRT_PI * light.a.w * light.b.w + 2.0 * CRT_PI * light.a.w * light.a.w;
	case CRT_LIGHT_TRIANGLE:
		return 0.5 * length(cross(light.b.xyz - light.a.xyz, light.c.xyz - light.a.xyz));
	}
	return 0.0;
}

bool areaLightTwoSided(GpuLight light)
{
	return (light.kind & CRT_LIGHT_FLAG_ONE_SIDED) == 0u;
}

// the word binding 65 holds for punctual light `index`: where its image's emission table starts, or CRT_NO_LIGHT
uint lightImageTable(uint index)
{
	return floatBitsToUint(imageDistributions[index]);
}

// What a light emits: its record's radiance (exact RGB) or reference and scale (spectral), tinted by `tint` (a texel of
// an image or a texture, linear sRGB as the textures hold it) and scaled by `factor`
Spectrum lightEmission(GpuLight light, vec3 tint, float factor)
{
#ifndef CRT_SPECTRAL
	return light.radiance * tint * factor;
#else
	return spectrumScale(spectrumFromRefOrRgb(sceneSpectrumRef(light.spectrum), srgbToWorking(tint), CRT_ROLE_ILLUMINANT), uintBitsToFloat(light.spectrumScale) * factor);
#endif
}

// what an environment map's texel towards `towardLight` emits, times the scene's intensity
Spectrum environmentEmission(vec3 towardLight)
{
	const vec3 texel = textureLod(environmentMap, equirectUv(environmentToMap(towardLight, environmentRotation())), 0.0).rgb;
#ifndef CRT_SPECTRAL
	return texel * pc.environmentIntensity;
#else
	return spectrumScale(spectrumFromRefOrRgb(CRT_SPECTRAL_NONE, srgbToWorking(texel), CRT_ROLE_ILLUMINANT), pc.environmentIntensity);
#endif
}

// What an area light emits at a point of it, as its light sampler evaluates it: a triangle's by its texel (false where
// its alpha test cuts the surface away), a textured shape's by its emission texel
bool areaLightEmission(GpuLight light, vec3 point, vec3 barycentrics, out Spectrum radiance)
{
	radiance = spectrumConst(0.0);
	if (lightKindOf(light) != CRT_LIGHT_TRIANGLE) {
		radiance = lightEmission(light, shapeLightTexture(light, point), 1.0);
		return true;
	}
#ifndef CRT_SPECTRAL
	radiance = triangleLightRadiance(light, barycentrics);
	return max(radiance.r, max(radiance.g, radiance.b)) > 0.0;
#else
	vec3 tint;
	if (!triangleLightTint(light, barycentrics, tint)) {
		return false;
	}
	// the texel is in the working space already
	radiance = spectrumScale(spectrumFromRefOrRgb(sceneSpectrumRef(light.spectrum), tint, CRT_ROLE_ILLUMINANT), uintBitsToFloat(light.spectrumScale));
	return true;
#endif
}

// A ray leaving light `index`, from the position sample `uPos` and the direction sample `uDir`: false for no sample
bool sampleLe(uint index, vec2 uPos, vec2 uDir, out LightLeSample s)
{
	const GpuLight light = lights[index];
	const uint kind = lightKindOf(light);
	s.origin = vec3(0.0);
	s.normal = vec3(0.0);
	s.direction = vec3(0.0, 0.0, 1.0);
	s.radiance = spectrumConst(0.0);
	s.pdfPos = 1.0;
	s.pdfDir = 1.0;
	s.deltaPosition = isDeltaLight(light.kind) && kind != CRT_LIGHT_DIRECTIONAL;
	s.deltaDirection = kind == CRT_LIGHT_DIRECTIONAL;
	vec3 tint = vec3(1.0);
	float factor = 1.0;

	// a laser's beam, outside the switch so that a scene without one runs the code it always did
	if (CRT_LASERS && kind == CRT_LIGHT_LASER) {
		const LaserEmitter laser = laserEmitter(index);
		if (!sampleLaserRay(laser, uPos, uDir, s.origin, s.direction, s.pdfPos, s.pdfDir)) {
			return false;
		}
		s.normal = laser.axis;
		s.radiance = spectrumScale(lightSpectrum(light), laserRadiancePerWatt(laser, s.origin, s.direction));
		return true;
	}

	switch (kind) {
	case CRT_LIGHT_POINT:
		s.origin = light.a.xyz;
		s.direction = sampleUniformSphere(uDir);
		s.pdfDir = 1.0 / (4.0 * CRT_PI);
		break;
	case CRT_LIGHT_SPOT: {
		s.origin = light.a.xyz;
		const vec3 local = sampleUniformCone(uDir, light.b.w);
		vec3 x;
		vec3 y;
		lightFrameFromZ(light.b.xyz, x, y);
		s.direction = local.x * x + local.y * y + local.z * light.b.xyz;
		s.pdfDir = spotPdfDir(light.b.xyz, light.b.w, s.direction);
		factor = spotFalloffOf(light.kind, dot(light.b.xyz, s.direction), light.b.w, light.c.x);
		break;
	}
	case CRT_LIGHT_PROJECTION:
	case CRT_LIGHT_GONIOMETRIC: {
		s.origin = light.a.xyz;
		const vec3 xAxis = light.b.xyz;
		const vec3 yAxis = light.c.xyz;
		const vec3 zAxis = (light.kind & CRT_LIGHT_FLAG_MIRRORED) != 0u ? -cross(xAxis, yAxis) : cross(xAxis, yAxis);
		// a point of the image by its table, or uniformly without one
		const uint table = lightImageTable(index);
		vec2 uv = uDir;
		float uvPdf = 1.0;
		if (table != CRT_NO_LIGHT) {
			uv = sampleImageTable(table, uDir, uvPdf);
		}
		vec3 local;
		if (kind == CRT_LIGHT_PROJECTION) {
			// the image plane at unit distance down the light's -z, half b.w wide and c.w tall
			const vec2 screen = vec2((2.0 * uv.x - 1.0) * light.b.w, (1.0 - 2.0 * uv.y) * light.c.w);
			const float r2 = 1.0 + dot(screen, screen);
			local = vec3(screen, -1.0) / sqrt(r2);
			s.pdfDir = uvPdf * r2 * sqrt(r2) / (4.0 * light.b.w * light.c.w);
		} else {
			local = equalAreaSquareToSphere(uv);
			s.pdfDir = uvPdf / (4.0 * CRT_PI);
		}
		s.direction = local.x * xAxis + local.y * yAxis + local.z * zAxis;
		tint = imagedLightFactor(light, s.direction);
		break;
	}
	case CRT_LIGHT_DIRECTIONAL: {
		// a.xyz is the direction it travels
		s.direction = light.a.xyz;
		s.origin = infiniteLightOrigin(-light.a.xyz, uPos);
		s.pdfPos = infiniteLightPdfPos();
		break;
	}
	case CRT_LIGHT_ENVIRONMENT: {
		vec3 towardLight;
		float pdf;
		if (!sampleEnvironment(0u, uDir, environmentRotation(), towardLight, pdf)) {
			return false;
		}
		s.direction = -towardLight;
		s.origin = infiniteLightOrigin(towardLight, uPos);
		s.pdfPos = infiniteLightPdfPos();
		s.pdfDir = pdf;
		s.radiance = environmentEmission(towardLight);
		return s.pdfPos > 0.0 && s.pdfDir > 0.0;
	}
	default: {
		// an area light: a point uniform by area, as its light sampler takes one when it samples by area
		const float area = areaLightArea(light);
		if (!(area > 0.0)) {
			return false;
		}
		vec3 barycentrics = vec3(0.0);
		switch (kind) {
		case CRT_LIGHT_SPHERE:
			s.normal = sampleUniformSphere(uPos);
			s.origin = light.a.xyz + light.a.w * s.normal;
			break;
		case CRT_LIGHT_RECTANGLE:
			s.origin = light.a.xyz + uPos.x * light.b.xyz + uPos.y * light.c.xyz;
			s.normal = normalize(cross(light.b.xyz, light.c.xyz));
			break;
		case CRT_LIGHT_CYLINDER: {
			const vec3 axisX = light.b.xyz;
			const vec3 axisY = light.c.xyz;
			const vec3 axisZ = cross(axisX, axisY);
			const float radius = light.a.w;
			const float height = light.b.w;
			const float sideArea = 2.0 * CRT_PI * radius * height;
			const float capArea = CRT_PI * radius * radius;
			const float t = uPos.x * area;
			if (t < sideArea) {
				const float phi = 2.0 * CRT_PI * min(t / sideArea, CRT_ONE_MINUS_EPSILON);
				const vec3 radial = cos(phi) * axisX + sin(phi) * axisZ;
				s.origin = light.a.xyz + radius * radial + height * (uPos.y - 0.5) * axisY;
				s.normal = radial;
			} else {
				const float capU = (t - sideArea) / capArea;
				const bool top = capU < 1.0;
				const vec2 disk = sampleUniformDiskConcentric(vec2(min(top ? capU : capU - 1.0, CRT_ONE_MINUS_EPSILON), uPos.y));
				s.origin = light.a.xyz + radius * (disk.x * axisX + disk.y * axisZ) + height * (top ? 0.5 : -0.5) * axisY;
				s.normal = top ? axisY : -axisY;
			}
			break;
		}
		case CRT_LIGHT_TRIANGLE:
			barycentrics = sampleUniformTriangle(uPos);
			s.origin = barycentrics.x * light.a.xyz + barycentrics.y * light.b.xyz + barycentrics.z * light.c.xyz;
			s.normal = normalize(cross(light.b.xyz - light.a.xyz, light.c.xyz - light.a.xyz));
			break;
		}
		// the side it leaves from: a one-sided light's front (its normal, or the other side for a reversed record), a
		// two-sided light's picked by the first number, which is stretched back over [0, 1)
		vec2 u = uDir;
		float side = (light.kind & CRT_LIGHT_FLAG_REVERSED) != 0u ? -1.0 : 1.0;
		float sideChance = 1.0;
		if (areaLightTwoSided(light)) {
			sideChance = 0.5;
			side = 1.0;
			if (u.x < 0.5) {
				u.x = min(u.x * 2.0, CRT_ONE_MINUS_EPSILON);
			} else {
				u.x = min((u.x - 0.5) * 2.0, CRT_ONE_MINUS_EPSILON);
				side = -1.0;
			}
		}
		const vec3 local = sampleCosineHemisphere(u);
		const vec3 z = side * s.normal;
		vec3 x;
		vec3 y;
		lightFrameFromZ(z, x, y);
		s.direction = local.x * x + local.y * y + local.z * z;
		s.pdfPos = 1.0 / area;
		s.pdfDir = sideChance * local.z / CRT_PI;
		return areaLightEmission(light, s.origin, barycentrics, s.radiance) && s.pdfDir > 0.0;
	}
	}
	s.radiance = lightEmission(light, tint, factor);
	return s.pdfPos > 0.0 && s.pdfDir > 0.0;
}

// The densities sampleLe() gives a ray leaving area light `index` along the unit `direction` from a point of normal
// `normal` (either orientation: a one-sided light emits along only one, and a camera path that found it emitting
// came from that side)
void areaLightPdfLe(uint index, vec3 normal, vec3 direction, out float pdfPos, out float pdfDir)
{
	const GpuLight light = lights[index];
	const float area = areaLightArea(light);
	pdfPos = area > 0.0 ? 1.0 / area : 0.0;
	pdfDir = (areaLightTwoSided(light) ? 0.5 : 1.0) * abs(dot(normal, direction)) / CRT_PI;
}

// The densities of a ray leaving the environment towards the scene along the unit `direction`
void environmentPdfLe(vec3 direction, out float pdfPos, out float pdfDir)
{
	pdfPos = infiniteLightPdfPos();
	pdfDir = environmentPdf(0u, -direction, environmentRotation());
}

bool isInfiniteLight(uint kind)
{
	return kind == CRT_LIGHT_DIRECTIONAL || kind == CRT_LIGHT_ENVIRONMENT;
}

// The densities sampleLe() gives a ray leaving light `index` along the unit `direction`, from a point of normal `normal`
// on an area light (either orientation, as areaLightPdfLe() takes it): what the bidirectional integrators' MIS weighs a
// light sample and a camera path that found a light by (pbrt-v4's Light::PDF_Le())
void lightPdfLe(uint index, vec3 normal, vec3 direction, out float pdfPos, out float pdfDir)
{
	const GpuLight light = lights[index];
	const uint kind = lightKindOf(light);
	pdfPos = 1.0;
	pdfDir = 0.0;
	switch (kind) {
	case CRT_LIGHT_POINT:
		pdfDir = 1.0 / (4.0 * CRT_PI);
		return;
	case CRT_LIGHT_SPOT:
		pdfDir = spotPdfDir(light.b.xyz, light.b.w, direction);
		return;
	case CRT_LIGHT_PROJECTION:
	case CRT_LIGHT_GONIOMETRIC: {
		const vec3 xAxis = light.b.xyz;
		const vec3 yAxis = light.c.xyz;
		const vec3 zAxis = (light.kind & CRT_LIGHT_FLAG_MIRRORED) != 0u ? -cross(xAxis, yAxis) : cross(xAxis, yAxis);
		const vec3 local = vec3(dot(direction, xAxis), dot(direction, yAxis), dot(direction, zAxis));
		const uint table = lightImageTable(index);
		if (kind == CRT_LIGHT_PROJECTION) {
			if (!(local.z < 0.0)) {
				return;
			}
			const vec2 screen = local.xy / -local.z;
			if (abs(screen.x) > light.b.w || abs(screen.y) > light.c.w) {
				return;
			}
			const vec2 uv = vec2(0.5 * (screen.x / light.b.w + 1.0), 0.5 * (1.0 - screen.y / light.c.w));
			const float r2 = 1.0 + dot(screen, screen);
			pdfDir = (table != CRT_NO_LIGHT ? imageTablePdf(table, uv) : 1.0) * r2 * sqrt(r2) / (4.0 * light.b.w * light.c.w);
		} else {
			const vec2 uv = equalAreaSphereToSquare(local);
			pdfDir = (table != CRT_NO_LIGHT ? imageTablePdf(table, uv) : 1.0) / (4.0 * CRT_PI);
		}
		return;
	}
	case CRT_LIGHT_DIRECTIONAL:
		pdfPos = infiniteLightPdfPos();
		pdfDir = 1.0;
		return;
	case CRT_LIGHT_ENVIRONMENT:
		environmentPdfLe(direction, pdfPos, pdfDir);
		return;
	default:
		areaLightPdfLe(index, normal, direction, pdfPos, pdfDir);
		return;
	}
}

#endif
