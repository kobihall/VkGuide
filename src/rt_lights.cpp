#include <rt_lights.h>

#include <algorithm>
#include <bit>
#include <cmath>

namespace {

float luminanceOf(const glm::vec3& c)
{
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

}

float lightUnitFactor(const SceneLight& light, LightUnit unit, const LightImage& image)
{
	if (unit == LightUnit::Radiometric) {
		return 1.f;
	}
	const glm::vec3 rgb = light.color.kind == SpectrumKind::Rgb ? light.color.rgb : exactRgb(light.color, SpectrumRole::Illuminant, {});
	const float photometric = luminanceOf(glm::max(rgb, glm::vec3(0.f))) / PHOTOMETRIC_TO_RADIOMETRIC;
	const bool imaged = (light.kind == LightKind::Projection || light.kind == LightKind::Goniometric) && image.valid;
	if (unit != LightUnit::Lumen) {
		//an image's intensity is the light's times its texel: a candela is its brightest direction's
		return imaged ? photometric * image.peak : photometric;
	}
	switch (light.kind) {
	case LightKind::Point:
		return photometric * 4.f * LIGHT_PI;
	case LightKind::Spot: {
		const float outerDegrees = std::clamp(light.outerConeDegrees, 0.f, 90.f);
		const float cosOuter = std::cos(glm::radians(outerDegrees));
		const float cosInner = std::cos(glm::radians(std::clamp(light.innerConeDegrees, 0.f, outerDegrees)));
		return photometric * spotSolidAngle(cosOuter, cosInner, light.falloff);
	}
	case LightKind::Projection:
	case LightKind::Goniometric:
		return photometric * (imaged ? image.solidAngle : 4.f * LIGHT_PI);
	case LightKind::Directional:
		break;
	}
	return photometric;
}

std::span<const LightUnit> lightUnits(LightKind kind)
{
	static constexpr LightUnit POINT_UNITS[] = { LightUnit::Radiometric, LightUnit::Candela, LightUnit::Lumen };
	static constexpr LightUnit DIRECTIONAL_UNITS[] = { LightUnit::Radiometric, LightUnit::Lux };
	if (kind == LightKind::Directional) {
		return DIRECTIONAL_UNITS;
	}
	return POINT_UNITS;
}

const char* lightUnitLabel(LightKind kind, LightUnit unit)
{
	switch (unit) {
	case LightUnit::Radiometric:
		return kind == LightKind::Directional ? "W/m^2" : "W/sr";
	case LightUnit::Candela:
		return "cd";
	case LightUnit::Lumen:
		return "lm";
	case LightUnit::Lux:
		return "lux";
	}
	return "";
}

float spotSolidAngle(float cosOuter, float cosInner, SpotFalloff falloff)
{
	return spotSolidAngle(cosOuter, cosInner, falloff == SpotFalloff::Smoothstep);
}

LightImage lightImageMeasure(bool projection, float fovDegrees, uint32_t width, uint32_t height, const float* luminance)
{
	LightImage image;
	if (width == 0 || height == 0 || luminance == nullptr) {
		return image;
	}
	image.valid = true;
	image.solidAngle = lightImageSolidAngle(projection, fovDegrees, width, height, luminance, &image.peak);
	image.aspect = (float)width / (float)height;
	return image;
}

void LightListBuilder::addPunctual(const SceneLight& light, const LightImage& image)
{
	//the delta lights must be lights[0, deltaCount), the range the BSDF strategy's table covers
	if (m_list.lights.size() != m_list.header.deltaCount) {
		return;
	}
	//an RGB color in an sRGB scene is the authored triple, so the record is what it always was
	const glm::vec3 color = light.color.kind == SpectrumKind::Rgb && m_context.space == ColorSpaceId::Srgb ? light.color.rgb : exactRgb(light.color, SpectrumRole::Illuminant, m_context);
	const glm::vec3 intensity = glm::max(color, glm::vec3(0.f)) * std::max(light.intensity, 0.f);
	float luminance = luminanceOf(intensity);
	//a spectrum with no luminance (an infrared line) is priced by its radiance: only a spectral build sees it
	if (!(luminance > 0.f) && light.color.kind != SpectrumKind::Rgb) {
		luminance = spectrumRadiometricPrice(light.color) * std::max(light.intensity, 0.f);
	}
	if (!(luminance > 0.f) || !std::isfinite(luminance)) {
		return;
	}

	GpuLight record;
	record.radiance = intensity;
	if (m_spectra != nullptr) {
		record.spectrum = m_spectra->add(light.color, SpectrumRole::Illuminant);
		record.spectrumScale = std::max(light.intensity, 0.f);
	}
	record.group = std::min(light.lightGroup, MAX_LIGHT_GROUPS - 1u);
	const glm::vec3 direction = glm::normalize(light.direction());
	float crossSection = 0.f;
	switch (light.kind) {
	case LightKind::Point:
		record.kind = (uint32_t)GpuLightKind::Point;
		record.a = glm::vec4(light.position, std::max(light.range, 0.f));
		//radiant intensity in every direction: 4 pi steradians of it
		record.power = 4.f * LIGHT_PI * luminance;
		break;
	case LightKind::Spot: {
		record.kind = (uint32_t)GpuLightKind::Spot;
		record.a = glm::vec4(light.position, std::max(light.range, 0.f));
		const float outerDegrees = std::clamp(light.outerConeDegrees, 0.f, 90.f);
		const float cosOuter = std::cos(glm::radians(outerDegrees));
		const float cosInner = std::cos(glm::radians(std::clamp(light.innerConeDegrees, 0.f, outerDegrees)));
		record.b = glm::vec4(direction, cosOuter);
		record.c = glm::vec4(cosInner, 0.f, 0.f, 0.f);
		record.power = luminance * spotSolidAngle(cosOuter, cosInner, light.falloff);
		if (light.falloff == SpotFalloff::Smoothstep) {
			record.kind |= GPU_LIGHT_FLAG_SMOOTHSTEP;
		}
		break;
	}
	case LightKind::Directional:
		record.kind = (uint32_t)GpuLightKind::Directional;
		record.a = glm::vec4(direction, 0.f);
		//irradiance over the scene's cross-section, priced once its size is known
		crossSection = luminance;
		break;
	case LightKind::Projection:
	case LightKind::Goniometric: {
		//without its image, a point light of its intensity: what the importer made of one before
		if (!image.valid) {
			record.kind = (uint32_t)GpuLightKind::Point;
			record.a = glm::vec4(light.position, 0.f);
			record.power = 4.f * LIGHT_PI * luminance;
			break;
		}
		const bool projection = light.kind == LightKind::Projection;
		record.kind = (uint32_t)(projection ? GpuLightKind::Projection : GpuLightKind::Goniometric) | (light.mirrored ? GPU_LIGHT_FLAG_MIRRORED : 0u);
		const uint32_t imageBits = image.descriptor | (image.sampler << 20);
		record.a = glm::vec4(light.position, std::bit_cast<float>(imageBits));
		const glm::vec3 xAxis = glm::normalize(light.orientation * glm::vec3(1.f, 0.f, 0.f));
		const glm::vec3 yAxis = glm::normalize(light.orientation * glm::vec3(0.f, 1.f, 0.f));
		glm::vec2 extents(0.f);
		if (projection && image.solidAngle > 0.f) {
			extents = projectionExtents(light.fovDegrees, image.aspect);
		}
		record.b = glm::vec4(xAxis, extents.x);
		record.c = glm::vec4(yAxis, extents.y);
		//the image's texels times the intensity, over the directions they cover (pbrt-v4's ProjectionLight::Phi and
		//GoniometricLight::Phi)
		record.power = luminance * image.solidAngle;
		if (image.luminance != nullptr && image.luminanceSize.x > 0 && image.luminanceSize.y > 0) {
			m_imageDistributions.emplace_back((uint32_t)m_list.lights.size(), buildLightImageDistribution(projection, extents, image.luminanceSize.x, image.luminanceSize.y, image.luminance));
		}
		break;
	}
	}
	m_list.lights.push_back(record);
	m_crossSectionPower.push_back(crossSection);
	m_list.header.deltaCount++;
	m_list.punctualLights++;
}

uint32_t LightListBuilder::addShape(ShapeKind kind, const glm::mat4& objectToWorld, const glm::vec3& radiance, uint32_t group, const LightSpectrum& spectrum, const ShapeEmitter& emitter)
{
	float luminance = luminanceOf(radiance);
	if (!(luminance > 0.f)) {
		luminance = spectrum.price;
	}
	const std::vector<AreaLightGeometry> surfaces = shapeLightGeometry(kind, objectToWorld);
	if (!(luminance > 0.f) || !std::isfinite(luminance) || surfaces.empty()) {
		return GPU_NO_LIGHT;
	}

	const uint32_t first = (uint32_t)m_list.lights.size();
	for (size_t face = 0; face < surfaces.size(); face++) {
		const AreaLightGeometry& surface = surfaces[face];
		GpuLight record;
		record.kind = (uint32_t)surface.kind | emitter.flags;
		//a rectangle's sampler gives cross(b, c) as its normal, which for a quad (+y outward) and the box's faces on the
		//negative side of their axis points in: its record says so, so a one-sided sample faces the way the hit does
		if (surface.kind == AreaLightKind::Rectangle && (emitter.flags & GPU_LIGHT_FLAG_ONE_SIDED) != 0) {
			glm::vec3 outward(0.f, 1.f, 0.f);
			if (kind == ShapeKind::Box) {
				outward = glm::vec3(0.f);
				outward[face / 2] = (face & 1u) != 0 ? 1.f : -1.f;
			}
			if (glm::dot(glm::cross(surface.b, surface.c), glm::vec3(objectToWorld * glm::vec4(outward, 0.f))) < 0.f) {
				record.kind ^= GPU_LIGHT_FLAG_REVERSED;
			}
		}
		record.radiance = radiance;
		record.group = group;
		record.spectrum = spectrum.index;
		record.spectrumScale = spectrum.scale;
		const float instance = (emitter.flags & GPU_LIGHT_FLAG_TEXTURED) != 0 ? std::bit_cast<float>(emitter.instance) : 0.f;
		switch (surface.kind) {
		case AreaLightKind::Sphere:
			record.a = glm::vec4(surface.a, surface.radius);
			record.b = glm::vec4(0.f, 0.f, 0.f, instance);
			break;
		case AreaLightKind::Rectangle:
			record.a = glm::vec4(surface.a, instance);
			record.b = glm::vec4(surface.b, 0.f);
			record.c = glm::vec4(surface.c, 0.f);
			break;
		case AreaLightKind::Cylinder:
			record.a = glm::vec4(surface.a, surface.radius);
			record.b = glm::vec4(surface.b, surface.height);
			record.c = glm::vec4(surface.c, instance);
			break;
		case AreaLightKind::Triangle:
			break;
		}
		//pi L per unit area leaves a surface into its outward hemisphere. A quad is seen from both
		//sides and emits twice that, unless it emits from one; a closed shape's inside is never seen. A degenerate box
		//face keeps its record, at zero power, so the faces stay in the order the intersect kernel names them
		const bool oneSided = (emitter.flags & GPU_LIGHT_FLAG_ONE_SIDED) != 0;
		const bool reversed = (emitter.flags & GPU_LIGHT_FLAG_REVERSED) != 0;
		float sides = kind == ShapeKind::Quad && !oneSided ? 2.f : 1.f;
		//a closed shape that emits only inward lights nothing outside it
		if (oneSided && reversed && kind != ShapeKind::Quad) {
			sides = 0.f;
		}
		const float power = sides * LIGHT_PI * surface.area * luminance * emitter.textureMean;
		record.power = std::isfinite(power) ? std::max(power, 0.f) : 0.f;
		m_list.lights.push_back(record);
		m_crossSectionPower.push_back(0.f);
	}
	m_list.shapeLights += (uint32_t)surfaces.size();
	return first;
}

uint32_t LightListBuilder::addTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& radiance, uint32_t attributeIndex, uint32_t material, int emissiveLayer, float textureMean, uint32_t group,
	const LightSpectrum& spectrum, uint32_t flags)
{
	const float area = 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
	//two-sided unless its material emits from one side, and as bright as its texture is on average over it
	const float luminance = luminanceOf(radiance) > 0.f ? luminanceOf(radiance) : spectrum.price;
	const float sides = (flags & GPU_LIGHT_FLAG_ONE_SIDED) != 0 ? 1.f : 2.f;
	const float power = sides * LIGHT_PI * area * luminance * textureMean;
	if (!(power > 0.f) || !std::isfinite(power)) {
		return GPU_NO_LIGHT;
	}
	GpuLight record;
	record.kind = (uint32_t)GpuLightKind::Triangle | flags;
	record.a = glm::vec4(v0, std::bit_cast<float>(attributeIndex));
	record.b = glm::vec4(v1, std::bit_cast<float>((int32_t)emissiveLayer));
	record.c = glm::vec4(v2, std::bit_cast<float>(material));
	record.radiance = radiance;
	record.power = power;
	record.group = group;
	record.spectrum = spectrum.index;
	record.spectrumScale = spectrum.scale;
	m_list.lights.push_back(record);
	m_crossSectionPower.push_back(0.f);
	m_list.triangleLightCount++;
	return (uint32_t)m_list.lights.size() - 1;
}

void LightListBuilder::addEnvironment(float luminanceIntegral, float intensity, uint32_t group)
{
	const float crossSection = luminanceIntegral * std::max(intensity, 0.f);
	if (!(crossSection > 0.f) || !std::isfinite(crossSection)) {
		return;
	}
	GpuLight record;
	record.kind = (uint32_t)GpuLightKind::Environment;
	record.group = group;
	m_list.header.environment = (uint32_t)m_list.lights.size();
	m_list.lights.push_back(record);
	m_crossSectionPower.push_back(crossSection);
	m_list.environment = true;
}

uint32_t LightListBuilder::addLaser(const LaserLight& laser)
{
	//priced as every light is, by the luminance of what it sends out, here its power times its color's; a line no eye
	//sees still carries its false color, whose luminance keeps it in the table
	const float power = std::max(laser.power, 0.f);
	const float price = power * std::max(luminanceOf(laser.rgb), 1e-3f);
	if (!(price > 0.f) || !std::isfinite(price)) {
		return GPU_NO_LIGHT;
	}
	const LaserEmitter& e = laser.emitter;
	GpuLight first;
	first.kind = (uint32_t)GpuLightKind::Laser | GPU_LIGHT_FLAG_ONE_SIDED;
	first.a = glm::vec4(e.center, laser.apertureRadius);
	first.b = glm::vec4(e.axis, 0.f);
	first.c = glm::vec4(e.xAxis, 0.f);
	first.radiance = laser.rgb * power;
	first.power = price;
	first.spectrum = laser.spectrum.index;
	first.spectrumScale = power;
	GpuLight beam;
	beam.kind = (uint32_t)GpuLightKind::LaserBeam;
	beam.a = glm::vec4(e.waist, e.waistDistance);
	beam.b = glm::vec4(e.halfAngle, 0.f, 0.f);
	m_lasers.push_back(first);
	m_lasers.push_back(beam);
	return (uint32_t)(m_lasers.size() / 2 - 1);
}

uint32_t LightListBuilder::beginTriangleRun(uint32_t triangleCount)
{
	const uint32_t base = (uint32_t)m_list.triangleLights.size();
	m_list.triangleLights.resize(m_list.triangleLights.size() + triangleCount, GPU_NO_LIGHT);
	return base;
}

void LightListBuilder::setTriangleLight(uint32_t runBase, uint32_t triangle, uint32_t light)
{
	if ((size_t)runBase + triangle < m_list.triangleLights.size()) {
		m_list.triangleLights[runBase + triangle] = light;
	}
}

RaytraceLightList LightListBuilder::finish(float sceneRadius)
{
	RaytraceLightList list = std::move(m_list);

	//binding 65's start: a word per punctual light naming its table, then the tables
	list.imageTables.assign(list.header.deltaCount, std::bit_cast<float>(GPU_NO_LIGHT));
	for (const auto& [light, table] : m_imageDistributions) {
		if (table.integral > 0.f) {
			list.imageTables[light] = std::bit_cast<float>(packPiecewiseConstant2D(table, list.imageTables));
		}
	}

	//a directional light and the environment deliver their power over the scene's cross-section
	//(pbrt-v4's DistantLight::Phi, ImageInfiniteLight::Phi)
	const float crossSection = LIGHT_PI * sceneRadius * sceneRadius;
	for (size_t i = 0; i < list.lights.size(); i++) {
		if (m_crossSectionPower[i] > 0.f) {
			list.lights[i].power = m_crossSectionPower[i] * crossSection;
		}
	}

	std::vector<float> weights(list.lights.size());
	double total = 0.0;
	double deltaTotal = 0.0;
	for (size_t i = 0; i < list.lights.size(); i++) {
		weights[i] = list.lights[i].power;
		total += weights[i];
		if (i < list.header.deltaCount) {
			deltaTotal += weights[i];
		}
	}
	list.totalPower = (float)total;

	const std::vector<AliasEntry> table = buildAliasTable(weights);
	const std::vector<AliasEntry> deltaTable = buildAliasTable(std::span<const float>(weights.data(), list.header.deltaCount));
	for (size_t i = 0; i < table.size(); i++) {
		list.lights[i].aliasQ = table[i].q;
		list.lights[i].aliasIndex = table[i].alias;
	}
	for (size_t i = 0; i < deltaTable.size(); i++) {
		list.lights[i].deltaAliasQ = deltaTable[i].q;
		list.lights[i].deltaAliasIndex = deltaTable[i].alias;
	}

	//a list whose every light is black samples nothing; its records still exist, so that what the
	//intersect kernel names stays in range, and each prices at pmf 0 - full weight for a BSDF sample
	list.header.count = table.empty() ? 0u : (uint32_t)list.lights.size();
	list.header.invTotalPower = total > 0.0 ? (float)(1.0 / total) : 0.f;
	if (deltaTable.empty()) {
		list.header.deltaCount = 0;
	}
	list.header.invDeltaPower = deltaTotal > 0.0 ? (float)(1.0 / deltaTotal) : 0.f;

	//the lasers, after every light next-event estimation samples, with their table in their first records
	if (!m_lasers.empty()) {
		const uint32_t base = (uint32_t)list.lights.size();
		const uint32_t count = (uint32_t)(m_lasers.size() / 2);
		std::vector<float> laserWeights(count);
		double laserTotal = 0.0;
		for (uint32_t i = 0; i < count; i++) {
			laserWeights[i] = m_lasers[2 * i].power;
			laserTotal += laserWeights[i];
		}
		const std::vector<AliasEntry> laserTable = buildAliasTable(laserWeights);
		for (uint32_t i = 0; i < count && i < laserTable.size(); i++) {
			m_lasers[2 * i].aliasQ = laserTable[i].q;
			m_lasers[2 * i].aliasIndex = laserTable[i].alias;
		}
		list.lights.insert(list.lights.end(), m_lasers.begin(), m_lasers.end());
		list.header.laserBase = base;
		list.header.laserCount = count;
		//the first table counts only when it samples anything
		const double emitTotal = (list.header.count > 0 ? total : 0.0) + laserTotal;
		list.header.invEmitPower = emitTotal > 0.0 ? (float)(1.0 / emitTotal) : 0.f;
		list.laserPower = (float)laserTotal;
	}

	m_list = {};
	m_crossSectionPower.clear();
	m_lasers.clear();
	return list;
}
