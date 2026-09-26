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

void LightListBuilder::addPunctual(const SceneLight& light)
{
	//the delta lights must be lights[0, deltaCount), the range the BSDF strategy's table covers
	if (m_list.lights.size() != m_list.header.deltaCount) {
		return;
	}
	const glm::vec3 intensity = glm::max(light.color, glm::vec3(0.f)) * std::max(light.intensity, 0.f);
	const float luminance = luminanceOf(intensity);
	if (!(luminance > 0.f) || !std::isfinite(luminance)) {
		return;
	}

	GpuLight record;
	record.radiance = intensity;
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
		//the inner cone at full intensity and the falloff ring at a third of it, the mean of the
		//squared ramp in cos(angle) glTF recommends (pbrt-v4's SpotLight::Phi, for that falloff)
		record.power = 2.f * LIGHT_PI * luminance * ((1.f - cosInner) + (cosInner - cosOuter) / 3.f);
		break;
	}
	case LightKind::Directional:
		record.kind = (uint32_t)GpuLightKind::Directional;
		record.a = glm::vec4(direction, 0.f);
		//irradiance over the scene's cross-section, priced once its size is known
		crossSection = luminance;
		break;
	}
	m_list.lights.push_back(record);
	m_crossSectionPower.push_back(crossSection);
	m_list.header.deltaCount++;
	m_list.punctualLights++;
}

uint32_t LightListBuilder::addShape(ShapeKind kind, const glm::mat4& objectToWorld, const glm::vec3& radiance)
{
	const float luminance = luminanceOf(radiance);
	const std::vector<AreaLightGeometry> surfaces = shapeLightGeometry(kind, objectToWorld);
	if (!(luminance > 0.f) || !std::isfinite(luminance) || surfaces.empty()) {
		return GPU_NO_LIGHT;
	}

	const uint32_t first = (uint32_t)m_list.lights.size();
	for (const AreaLightGeometry& surface : surfaces) {
		GpuLight record;
		record.kind = (uint32_t)surface.kind;
		record.radiance = radiance;
		switch (surface.kind) {
		case AreaLightKind::Sphere:
			record.a = glm::vec4(surface.a, surface.radius);
			break;
		case AreaLightKind::Rectangle:
			record.a = glm::vec4(surface.a, 0.f);
			record.b = glm::vec4(surface.b, 0.f);
			record.c = glm::vec4(surface.c, 0.f);
			break;
		case AreaLightKind::Cylinder:
			record.a = glm::vec4(surface.a, surface.radius);
			record.b = glm::vec4(surface.b, surface.height);
			record.c = glm::vec4(surface.c, 0.f);
			break;
		case AreaLightKind::Triangle:
			break;
		}
		//pi L per unit area leaves a surface into its outward hemisphere. A quad is seen from both
		//sides and emits twice that; a closed shape's inside is never seen. A degenerate box face keeps
		//its record, at zero power, so the faces stay in the order the intersect kernel names them
		const float sides = kind == ShapeKind::Quad ? 2.f : 1.f;
		const float power = sides * LIGHT_PI * surface.area * luminance;
		record.power = std::isfinite(power) ? std::max(power, 0.f) : 0.f;
		m_list.lights.push_back(record);
		m_crossSectionPower.push_back(0.f);
	}
	m_list.shapeLights += (uint32_t)surfaces.size();
	return first;
}

uint32_t LightListBuilder::addTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& radiance, uint32_t attributeIndex, uint32_t material, int emissiveLayer, float textureMean)
{
	const float area = 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
	//two-sided, as the tracer's triangles are, and as bright as its texture is on average over it
	const float power = 2.f * LIGHT_PI * area * luminanceOf(radiance) * textureMean;
	if (!(power > 0.f) || !std::isfinite(power)) {
		return GPU_NO_LIGHT;
	}
	GpuLight record;
	record.kind = (uint32_t)GpuLightKind::Triangle;
	record.a = glm::vec4(v0, std::bit_cast<float>(attributeIndex));
	record.b = glm::vec4(v1, std::bit_cast<float>((int32_t)emissiveLayer));
	record.c = glm::vec4(v2, std::bit_cast<float>(material));
	record.radiance = radiance;
	record.power = power;
	m_list.lights.push_back(record);
	m_crossSectionPower.push_back(0.f);
	m_list.triangleLightCount++;
	return (uint32_t)m_list.lights.size() - 1;
}

void LightListBuilder::addEnvironment(float luminanceIntegral, float intensity)
{
	const float crossSection = luminanceIntegral * std::max(intensity, 0.f);
	if (!(crossSection > 0.f) || !std::isfinite(crossSection)) {
		return;
	}
	GpuLight record;
	record.kind = (uint32_t)GpuLightKind::Environment;
	m_list.header.environment = (uint32_t)m_list.lights.size();
	m_list.lights.push_back(record);
	m_crossSectionPower.push_back(crossSection);
	m_list.environment = true;
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

	m_list = {};
	m_crossSectionPower.clear();
	return list;
}
