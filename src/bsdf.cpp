#include <bsdf.h>

#include <algorithm>
#include <cmath>

#include <sampler.h>

// Each function below is the GLSL function of the same name in shaders/rt/include/crt_scatter.glsl,
// crt_samples.glsl or crt_random.glsl, expression for expression. glm's mix(), reflect(), dot() and
// cross() are GLSL's; the one place a GLSL built-in has no glm twin is spelled out where it appears

float bsdfWordToFloat(uint32_t word)
{
	return std::min(float(word) * (1.0f / 4294967296.0f), 0.99999994f);
}

float bsdfRandomFloat(uint32_t& state)
{
	state = state * 747796405u + 2891336453u;
	uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	word = (word >> 22u) ^ word;
	return bsdfWordToFloat(word);
}

namespace {

// pbrt-v4 src/pbrt/wavefront/surfscatter.cpp
constexpr float ALBEDO_UC[BSDF_ALBEDO_SAMPLES] = {
	0.75741637f, 0.37870818f, 0.7083487f, 0.18935409f, 0.9149363f, 0.35417435f, 0.5990858f, 0.09467703f,
	0.8578725f, 0.45746812f, 0.686759f, 0.17708716f, 0.9674518f, 0.2995429f, 0.5083201f, 0.047338516f };
constexpr glm::vec2 ALBEDO_U[BSDF_ALBEDO_SAMPLES] = {
	{ 0.855985f, 0.570367f }, { 0.381823f, 0.851844f }, { 0.285328f, 0.764262f }, { 0.733380f, 0.114073f },
	{ 0.542663f, 0.344465f }, { 0.127274f, 0.414848f }, { 0.964700f, 0.947162f }, { 0.594089f, 0.643463f },
	{ 0.095109f, 0.170369f }, { 0.825444f, 0.263359f }, { 0.429467f, 0.454469f }, { 0.244460f, 0.816459f },
	{ 0.756135f, 0.731258f }, { 0.516165f, 0.152852f }, { 0.180888f, 0.214174f }, { 0.898579f, 0.503897f } };

}

glm::vec3 bsdfAlbedoEstimate(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, bool frontFace)
{
	glm::vec3 sum(0.f);
	for (int i = 0; i < BSDF_ALBEDO_SAMPLES; i++) {
		//the shader's sampleDimension() answers from the table while the estimate runs, whatever the sampler: the
		//draws are a record whose fields are the table's, and the stream is the fuzz radius's alone
		BsdfDraws draws;
		draws.record = true;
		draws.rng = 0x9E3779B9u * (uint32_t)(i + 1);
		draws.fields[4] = ALBEDO_UC[i];
		draws.fields[5] = ALBEDO_U[i].x;
		draws.fields[6] = ALBEDO_U[i].y;
		glm::vec3 attenuation(0.f);
		glm::vec3 outDir(0.f);
		if (scatterSurface(m, inDir, normal, frontFace, draws, attenuation, outDir)) {
			sum += attenuation;
		}
	}
	return sum * (1.0f / (float)BSDF_ALBEDO_SAMPLES);
}

float bsdfSampleDimension(BsdfDraws& draws, uint32_t field)
{
	if (!draws.record) {
		return bsdfRandomFloat(draws.rng);
	}
	return draws.fields[field];
}

glm::vec2 bsdfSampleDimension2D(BsdfDraws& draws, uint32_t field)
{
	const float x = bsdfSampleDimension(draws, field);
	const float y = bsdfSampleDimension(draws, field + 1u);
	return glm::vec2(x, y);
}

glm::vec3 bsdfUnitVectorFromSample(glm::vec2 u)
{
	const float cosTheta = -1.0f + (1.0f - -1.0f) * u.x;
	const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
	const float phi = 2.0f * BSDF_PI * u.y;
	return glm::vec3(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);
}

glm::mat3 bsdfTangentSpace(glm::vec3 normal)
{
	glm::vec3 helper = glm::vec3(1.0f, 0.0f, 0.0f);
	if (std::abs(normal.x) > 0.99f) {
		helper = glm::vec3(0.0f, 0.0f, 1.0f);
	}
	const glm::vec3 tangent = glm::normalize(glm::cross(normal, helper));
	const glm::vec3 binormal = glm::normalize(glm::cross(normal, tangent));
	return glm::mat3(tangent, binormal, normal);
}

glm::vec3 bsdfHemisphereFromSample(glm::vec3 normal, float alpha, glm::vec2 u)
{
	const float cosTheta = std::pow(u.x, 1.0f / (alpha + 1.0f));
	const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
	const float phi = 2.0f * BSDF_PI * u.y;
	const glm::vec3 tangentSpaceDir = glm::vec3(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);
	return bsdfTangentSpace(normal) * tangentSpaceDir;
}

glm::vec3 bsdfBallFromSample(glm::vec2 u, float radius)
{
	return bsdfUnitVectorFromSample(u) * std::pow(radius, 1.0f / 3.0f);
}

// randomUnitVector() then the cube root of the next draw, the order the shader's expression evaluates
// them in: the unit vector's three draws first
glm::vec3 bsdfRandomInBall(uint32_t& state)
{
	// randomFloat(state, -1.0, 1.0)
	const float cosTheta = -1.0f + (1.0f - -1.0f) * bsdfRandomFloat(state);
	const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
	const float phi = 2.0f * BSDF_PI * bsdfRandomFloat(state);
	const glm::vec3 unit = glm::vec3(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);
	return unit * std::pow(bsdfRandomFloat(state), 1.0f / 3.0f);
}

//---------------------------------------------------------------- crt_scatter.glsl

float bsdfSchlick(float cosine, float refIdx)
{
	float r0 = (1.0f - refIdx) / (1.0f + refIdx);
	r0 = r0 * r0;
	return r0 + (1.0f - r0) * std::pow(1.0f - cosine, 5.0f);
}

glm::vec3 bsdfRefract(glm::vec3 uv, glm::vec3 n, float etaiOverEtat)
{
	const float cosTheta = std::min(glm::dot(-uv, n), 1.0f);
	const glm::vec3 rOutPerp = etaiOverEtat * (uv + cosTheta * n);
	const glm::vec3 rOutParallel = -std::sqrt(std::abs(1.0f - glm::dot(rOutPerp, rOutPerp))) * n;
	return rOutPerp + rOutParallel;
}

static float luminance(glm::vec3 c)
{
	return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

glm::vec3 bsdfFresnelSchlick(glm::vec3 f0, float cosine)
{
	return f0 + (1.0f - f0) * std::pow(1.0f - std::clamp(cosine, 0.0f, 1.0f), 5.0f);
}

float bsdfSmithG1(float cosine, float alpha)
{
	const float a2 = alpha * alpha;
	return 2.0f * cosine / (cosine + std::sqrt(a2 + (1.0f - a2) * cosine * cosine));
}

glm::vec3 bsdfSampleGgxVisibleNormal(glm::vec3 wo, float alpha, glm::vec2 u)
{
	const glm::vec3 vh = glm::normalize(glm::vec3(alpha * wo.x, alpha * wo.y, wo.z));
	const float lengthSquared = vh.x * vh.x + vh.y * vh.y;
	const glm::vec3 t1 = lengthSquared > 0.0f ? glm::vec3(-vh.y, vh.x, 0.0f) * glm::inversesqrt(lengthSquared) : glm::vec3(1.0f, 0.0f, 0.0f);
	const glm::vec3 t2 = glm::cross(vh, t1);
	const float r = std::sqrt(u.x);
	const float phi = 2.0f * BSDF_PI * u.y;
	const float p1 = r * std::cos(phi);
	const float s = 0.5f * (1.0f + vh.z);
	const float p2 = (1.0f - s) * std::sqrt(1.0f - p1 * p1) + s * r * std::sin(phi);
	const glm::vec3 nh = p1 * t1 + p2 * t2 + std::sqrt(std::max(0.0f, 1.0f - p1 * p1 - p2 * p2)) * vh;
	return glm::normalize(glm::vec3(alpha * nh.x, alpha * nh.y, std::max(0.0f, nh.z)));
}

static bool scatterPbr(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, BsdfDraws& draws, glm::vec3& attenuation, glm::vec3& outDir)
{
	const glm::vec3 wo = -inDir;
	const float cosO = std::max(glm::dot(normal, wo), 1e-4f);
	const glm::vec3 f0 = glm::mix(glm::vec3(m.param > 0.f ? m.param : 0.04f), m.albedo, m.metallic);
	const glm::vec3 diffuse = m.albedo * (1.0f - m.metallic);
	const glm::vec3 fresnelView = bsdfFresnelSchlick(f0, cosO);

	const float specularWeight = luminance(fresnelView);
	const float diffuseWeight = luminance(diffuse * (1.0f - fresnelView));
	if (specularWeight + diffuseWeight <= 0.0f) {
		attenuation = glm::vec3(0.0f);
		outDir = normal;
		return false;
	}
	const float specularChance = specularWeight / (specularWeight + diffuseWeight);

	const float lobeDraw = bsdfSampleDimension(draws, SAMPLE_INDIRECT_LOBE);
	if (diffuseWeight <= 0.0f || lobeDraw < specularChance) {
		const float alpha = std::max(m.roughness * m.roughness, 1e-3f);
		const glm::mat3 frame = bsdfTangentSpace(normal);
		const glm::vec3 woLocal = glm::transpose(frame) * wo;
		const glm::vec3 h = bsdfSampleGgxVisibleNormal(woLocal, alpha, bsdfSampleDimension2D(draws, SAMPLE_INDIRECT_DIRECTION));
		const glm::vec3 wiLocal = glm::reflect(-woLocal, h);
		if (wiLocal.z <= 0.0f) {
			attenuation = glm::vec3(0.0f);
			outDir = normal;
			return false;
		}
		attenuation = bsdfFresnelSchlick(f0, glm::dot(woLocal, h)) * bsdfSmithG1(wiLocal.z, alpha) / specularChance;
		outDir = frame * wiLocal;
		return true;
	}

	glm::vec3 d = normal + bsdfUnitVectorFromSample(bsdfSampleDimension2D(draws, SAMPLE_INDIRECT_DIRECTION));
	if (glm::dot(d, d) < 1e-8f) {
		d = normal;
	}
	attenuation = diffuse * (1.0f - fresnelView) / (1.0f - specularChance);
	outDir = d;
	return true;
}

bool scatterSurface(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, bool frontFace, BsdfDraws& draws, glm::vec3& attenuation, glm::vec3& outDir)
{
	switch (m.type) {
	case BsdfType::Lambertian: {
		glm::vec3 d = normal + bsdfUnitVectorFromSample(bsdfSampleDimension2D(draws, SAMPLE_INDIRECT_DIRECTION));
		if (glm::dot(d, d) < 1e-8f) {
			d = normal;
		}
		attenuation = m.albedo;
		outDir = d;
		return true;
	}
	case BsdfType::Metal: {
		const glm::vec3 reflected = glm::reflect(inDir, normal);
		if (!draws.record) {
			outDir = reflected + m.param * bsdfRandomInBall(draws.rng);
		} else {
			const glm::vec2 ballDirection = glm::vec2(draws.fields[SAMPLE_INDIRECT_DIRECTION], draws.fields[SAMPLE_INDIRECT_DIRECTION + 1]);
			const float ballRadius = bsdfRandomFloat(draws.rng);
			outDir = reflected + m.param * bsdfBallFromSample(ballDirection, ballRadius);
		}
		attenuation = m.albedo;
		return glm::dot(outDir, normal) > 0.0f;
	}
	case BsdfType::Phong: {
		const glm::vec3 reflected = glm::reflect(inDir, normal);
		const float alpha = std::pow(1000.0f, m.param * m.param);
		outDir = bsdfHemisphereFromSample(reflected, alpha, bsdfSampleDimension2D(draws, SAMPLE_INDIRECT_DIRECTION));
		attenuation = m.albedo;
		return glm::dot(outDir, normal) > 0.0f;
	}
	case BsdfType::Dielectric: {
		attenuation = glm::vec3(1.0f);
		const float ratio = frontFace ? (1.0f / m.param) : m.param;
		const float cosTheta = std::min(glm::dot(-inDir, normal), 1.0f);
		const float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
		const bool cannotRefract = ratio * sinTheta > 1.0f;
		if (cannotRefract || bsdfSchlick(cosTheta, ratio) > bsdfSampleDimension(draws, SAMPLE_INDIRECT_LOBE)) {
			outDir = glm::reflect(inDir, normal);
		} else {
			outDir = bsdfRefract(inDir, normal, ratio);
		}
		return true;
	}
	case BsdfType::Pbr:
		return scatterPbr(m, inDir, normal, draws, attenuation, outDir);
	case BsdfType::Emissive:
		break;
	}
	attenuation = glm::vec3(0.0f);
	outDir = normal;
	return false;
}

//---------------------------------------------------------------- evaluation, for light sampling

bool isSpecularMaterial(const BsdfMaterial& m)
{
	return m.type == BsdfType::Dielectric || (m.type == BsdfType::Metal && m.param < BSDF_SPECULAR_FUZZ);
}

float bsdfFuzzBallPdf(glm::vec3 r, float fuzz, glm::vec3 wi)
{
	const float b = glm::dot(wi, r);
	const glm::vec3 c = glm::cross(wi, r);
	const float discriminant = fuzz * fuzz - glm::dot(c, c);
	if (discriminant < 0.0f) {
		return 0.0f;
	}
	const float root = std::sqrt(discriminant);
	const float t1 = b + root;
	if (t1 <= 0.0f) {
		return 0.0f;
	}
	const float t0 = std::max(b - root, 0.0f);
	const float cubes = (t1 - t0) * (t1 * t1 + t1 * t0 + t0 * t0);
	return cubes / (4.0f * BSDF_PI * fuzz * fuzz * fuzz);
}

float bsdfPhongLobePdf(glm::vec3 reflected, float alpha, glm::vec3 wi)
{
	const float cosine = glm::dot(wi, reflected);
	return cosine > 0.0f ? (alpha + 1.0f) / (2.0f * BSDF_PI) * std::pow(cosine, alpha) : 0.0f;
}

float bsdfGgxD(float cosine, float alpha)
{
	const float a2 = alpha * alpha;
	const float d = cosine * cosine * (a2 - 1.0f) + 1.0f;
	return a2 / (BSDF_PI * d * d);
}

bool bsdfPbrLobes(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, glm::vec3& f0, glm::vec3& diffuse, glm::vec3& fresnelView, float& specularChance)
{
	const glm::vec3 wo = -inDir;
	const float cosO = std::max(glm::dot(normal, wo), 1e-4f);
	f0 = glm::mix(glm::vec3(m.param > 0.f ? m.param : 0.04f), m.albedo, m.metallic);
	diffuse = m.albedo * (1.0f - m.metallic);
	fresnelView = bsdfFresnelSchlick(f0, cosO);
	const float specularWeight = luminance(fresnelView);
	const float diffuseWeight = luminance(diffuse * (1.0f - fresnelView));
	specularChance = 0.0f;
	if (specularWeight + diffuseWeight <= 0.0f) {
		return false;
	}
	specularChance = specularWeight / (specularWeight + diffuseWeight);
	return true;
}

static glm::vec3 evalPbr(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, glm::vec3 wi)
{
	glm::vec3 f0;
	glm::vec3 diffuse;
	glm::vec3 fresnelView;
	float specularChance;
	if (!bsdfPbrLobes(m, inDir, normal, f0, diffuse, fresnelView, specularChance)) {
		return glm::vec3(0.0f);
	}
	const glm::vec3 wo = -inDir;
	const float cosO = glm::dot(normal, wo);
	const float cosI = glm::dot(normal, wi);
	if (cosO <= 0.0f || cosI <= 0.0f) {
		return glm::vec3(0.0f);
	}
	const float alpha = std::max(m.roughness * m.roughness, 1e-3f);
	const glm::vec3 h = glm::normalize(wo + wi);
	const glm::vec3 specular = bsdfFresnelSchlick(f0, glm::dot(wo, h)) * (bsdfGgxD(glm::dot(normal, h), alpha) * bsdfSmithG1(cosO, alpha) * bsdfSmithG1(cosI, alpha) / (4.0f * cosO));
	return specular + diffuse * (1.0f - fresnelView) * (cosI / BSDF_PI);
}

static float pdfPbr(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, glm::vec3 wi)
{
	glm::vec3 f0;
	glm::vec3 diffuse;
	glm::vec3 fresnelView;
	float specularChance;
	if (!bsdfPbrLobes(m, inDir, normal, f0, diffuse, fresnelView, specularChance)) {
		return 0.0f;
	}
	const glm::vec3 wo = -inDir;
	const float cosO = glm::dot(normal, wo);
	const float cosI = glm::dot(normal, wi);
	if (cosO <= 0.0f || cosI <= 0.0f) {
		return 0.0f;
	}
	const float alpha = std::max(m.roughness * m.roughness, 1e-3f);
	const glm::vec3 h = glm::normalize(wo + wi);
	const float specularPdf = bsdfSmithG1(cosO, alpha) * bsdfGgxD(glm::dot(normal, h), alpha) / (4.0f * cosO);
	return specularChance * specularPdf + (1.0f - specularChance) * (cosI / BSDF_PI);
}

glm::vec3 evalSurface(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, glm::vec3 wi)
{
	if (glm::dot(normal, wi) <= 0.0f) {
		return glm::vec3(0.0f);
	}
	switch (m.type) {
	case BsdfType::Lambertian:
		return m.albedo * (glm::dot(normal, wi) / BSDF_PI);
	case BsdfType::Metal:
		return m.param < BSDF_SPECULAR_FUZZ ? glm::vec3(0.0f) : m.albedo * bsdfFuzzBallPdf(glm::reflect(inDir, normal), m.param, wi);
	case BsdfType::Phong:
		return m.albedo * bsdfPhongLobePdf(glm::reflect(inDir, normal), std::pow(1000.0f, m.param * m.param), wi);
	case BsdfType::Pbr:
		return evalPbr(m, inDir, normal, wi);
	case BsdfType::Dielectric:
	case BsdfType::Emissive:
		break;
	}
	return glm::vec3(0.0f);
}

float pdfSurface(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, glm::vec3 wi)
{
	if (glm::dot(normal, wi) <= 0.0f) {
		return 0.0f;
	}
	switch (m.type) {
	case BsdfType::Lambertian:
		return glm::dot(normal, wi) / BSDF_PI;
	case BsdfType::Metal:
		return m.param < BSDF_SPECULAR_FUZZ ? 0.0f : bsdfFuzzBallPdf(glm::reflect(inDir, normal), m.param, wi);
	case BsdfType::Phong:
		return bsdfPhongLobePdf(glm::reflect(inDir, normal), std::pow(1000.0f, m.param * m.param), wi);
	case BsdfType::Pbr:
		return pdfPbr(m, inDir, normal, wi);
	case BsdfType::Dielectric:
	case BsdfType::Emissive:
		break;
	}
	return 0.0f;
}
