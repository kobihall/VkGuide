// The BSDFs: given an incoming direction and a surface, pick an outgoing direction and the
// attenuation along it. The first five are a port of the retired src/rt_material.cpp, function for
// function; pbr is glTF 2.0's metallic-roughness model.
//
// Each material also answers the two questions light sampling asks of it: evalSurface(), the
// f(wo, wi) cos(theta_i) its scattering estimates, for a direction a light sample chose; and
// pdfSurface(), the density with which scatterSurface() would itself have picked that direction,
// which is the BSDF half of every MIS weight. Both are derived from the sampling routine rather
// than the other way round, so that the attenuation scatterSurface() returns is exactly
// evalSurface() / pdfSurface() for the direction it picked - the condition for the strategies to
// agree. A dielectric, and a metal with no fuzz, are specular: a delta distribution no light sample
// can land in.
//
// Used by kernel 06 Sample Surface Scattering and by nothing else. Every 06 variant calls
// scatterSurface() for its BSDF lobe - it is the sampling STRATEGY that differs, not the material
// model - so this file is shared by every 06 variant rather than living inside one.
//
// Requires crt_common.glsl and crt_random.glsl.

float schlick(float cosine, float refIdx)
{
	float r0 = (1.0 - refIdx) / (1.0 + refIdx);
	r0 = r0 * r0;
	return r0 + (1.0 - r0) * pow(1.0 - cosine, 5.0);
}

vec3 refractDir(vec3 uv, vec3 n, float etaiOverEtat)
{
	float cosTheta = min(dot(-uv, n), 1.0);
	vec3 rOutPerp = etaiOverEtat * (uv + cosTheta * n);
	vec3 rOutParallel = -sqrt(abs(1.0 - dot(rOutPerp, rOutPerp))) * n;
	return rOutPerp + rOutParallel;
}

float luminance(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Schlick's Fresnel with a coloured F0, as glTF's metallic-roughness model defines it
vec3 fresnelSchlick(vec3 f0, float cosine)
{
	return f0 + (1.0 - f0) * pow(1.0 - clamp(cosine, 0.0, 1.0), 5.0);
}

// Smith's masking term for GGX, one direction
float smithG1(float cosine, float alpha)
{
	const float a2 = alpha * alpha;
	return 2.0 * cosine / (cosine + sqrt(a2 + (1.0 - a2) * cosine * cosine));
}

// A GGX microfacet normal from the distribution of normals VISIBLE from `wo` (Heitz 2018,
// "Sampling the GGX Distribution of Visible Normals"), in the frame where the surface normal is z.
// Sampling visible normals rather than all of them is what keeps the weight below bounded
vec3 sampleGgxVisibleNormal(vec3 wo, float alpha, vec2 u)
{
	const vec3 vh = normalize(vec3(alpha * wo.x, alpha * wo.y, wo.z));
	const float lengthSquared = vh.x * vh.x + vh.y * vh.y;
	const vec3 t1 = lengthSquared > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lengthSquared) : vec3(1.0, 0.0, 0.0);
	const vec3 t2 = cross(vh, t1);
	const float r = sqrt(u.x);
	const float phi = 2.0 * CRT_PI * u.y;
	const float p1 = r * cos(phi);
	const float s = 0.5 * (1.0 + vh.z);
	const float p2 = (1.0 - s) * sqrt(1.0 - p1 * p1) + s * r * sin(phi);
	const vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
	return normalize(vec3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

// glTF metallic-roughness: a GGX specular lobe with F0 = mix(0.04, base colour, metallic) over a
// Lambertian lobe of base colour x (1 - metallic). One lobe is sampled per bounce, chosen by how
// much each reflects towards `wo`, and its weight divided by that choice's probability.
// Specular weight under visible-normal sampling with separable Smith masking is F x G1(wi).
// The diffuse lobe takes what the Fresnel term at the viewing angle leaves, the usual cheap
// layering; it does not conserve energy exactly at grazing angles on rough surfaces
bool scatterPbr(GpuMaterial m, vec3 inDir, vec3 normal, inout uint rng, out vec3 attenuation, out vec3 outDir)
{
	const vec3 wo = -inDir;
	const float cosO = max(dot(normal, wo), 1e-4);
	const vec3 f0 = mix(vec3(0.04), m.albedo, m.metallic);
	const vec3 diffuse = m.albedo * (1.0 - m.metallic);
	const vec3 fresnelView = fresnelSchlick(f0, cosO);

	const float specularWeight = luminance(fresnelView);
	const float diffuseWeight = luminance(diffuse * (1.0 - fresnelView));
	if (specularWeight + diffuseWeight <= 0.0) {
		attenuation = vec3(0.0);
		outDir = normal;
		return false;
	}
	const float specularChance = specularWeight / (specularWeight + diffuseWeight);

	if (randomFloat(rng) < specularChance) {
		// perceptual roughness squared is GGX's alpha; a floor keeps a "mirror" out of the
		// singular limit the sampler divides by
		const float alpha = max(m.roughness * m.roughness, 1e-3);
		const mat3 frame = getTangentSpace(normal);
		const vec3 woLocal = transpose(frame) * wo;
		const vec3 h = sampleGgxVisibleNormal(woLocal, alpha, vec2(randomFloat(rng), randomFloat(rng)));
		const vec3 wiLocal = reflect(-woLocal, h);
		if (wiLocal.z <= 0.0) {
			attenuation = vec3(0.0);
			outDir = normal;
			return false;
		}
		attenuation = fresnelSchlick(f0, dot(woLocal, h)) * smithG1(wiLocal.z, alpha) / specularChance;
		outDir = frame * wiLocal;
		return true;
	}

	// normal + a point on the unit sphere is a cosine-weighted direction, as for lambertian
	vec3 d = normal + randomUnitVector(rng);
	if (dot(d, d) < 1e-8) {
		d = normal;
	}
	attenuation = diffuse * (1.0 - fresnelView) / (1.0 - specularChance);
	outDir = d;
	return true;
}

// the materials, by type. `inDir` is the unit incoming direction. Returns false when the path is
// absorbed
bool scatterSurface(GpuMaterial m, vec3 inDir, vec3 normal, bool frontFace, inout uint rng, out vec3 attenuation, out vec3 outDir)
{
	switch (m.type) {
	case CRT_MATERIAL_LAMBERTIAN: {
		// normal + a point on the unit sphere is a true cosine-weighted Lambertian distribution
		vec3 d = normal + randomUnitVector(rng);
		// the two can cancel almost exactly, and a zero direction turns into NaN once normalised
		if (dot(d, d) < 1e-8) {
			d = normal;
		}
		attenuation = m.albedo;
		outDir = d;
		return true;
	}
	case CRT_MATERIAL_METAL: {
		vec3 reflected = reflect(inDir, normal);
		outDir = reflected + m.param * randomInBall(rng);
		attenuation = m.albedo;
		return dot(outDir, normal) > 0.0;
	}
	case CRT_MATERIAL_PHONG: {
		vec3 reflected = reflect(inDir, normal);
		float alpha = pow(1000.0, m.param * m.param);
		outDir = randomInHemisphere(reflected, alpha, rng);
		attenuation = m.albedo;
		return dot(outDir, normal) > 0.0;
	}
	case CRT_MATERIAL_DIELECTRIC: {
		attenuation = vec3(1.0);
		float ratio = frontFace ? (1.0 / m.param) : m.param;
		float cosTheta = min(dot(-inDir, normal), 1.0);
		float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
		bool cannotRefract = ratio * sinTheta > 1.0;
		if (cannotRefract || schlick(cosTheta, ratio) > randomFloat(rng)) {
			outDir = reflect(inDir, normal);
		} else {
			outDir = refractDir(inDir, normal, ratio);
		}
		return true;
	}
	case CRT_MATERIAL_PBR:
		return scatterPbr(m, inDir, normal, rng, attenuation, outDir);
	}
	attenuation = vec3(0.0);
	outDir = normal;
	return false;
}

//---------------------------------------------------------------- evaluation, for light sampling

// a metal's fuzz below which its reflection is a perfect mirror: a delta distribution
#define CRT_SPECULAR_FUZZ 1e-4

// true for a material whose scattering is a delta distribution: no light sample can land in it, so
// a vertex on one samples no light and the next hit counts at full weight
bool isSpecularMaterial(GpuMaterial m)
{
	return m.type == CRT_MATERIAL_DIELECTRIC || (m.type == CRT_MATERIAL_METAL && m.param < CRT_SPECULAR_FUZZ);
}

// The density over directions of normalize(r + fuzz * X) for X uniform in the unit ball - the
// metal's scattering. It is the integral of t^2 along the ray from the origin through the ball of
// radius fuzz around the unit vector r, divided by the ball's volume: (t1^3 - t0^3) / (4 pi fuzz^3)
// with t0, t1 where the ray enters and leaves the ball (t0 clamped at 0 when the origin is inside it)
float fuzzBallPdf(vec3 r, float fuzz, vec3 wi)
{
	const float b = dot(wi, r);
	// b^2 - (1 - fuzz^2), with 1 - b^2 taken as |wi x r|^2 so a small fuzz keeps its precision
	const vec3 c = cross(wi, r);
	const float discriminant = fuzz * fuzz - dot(c, c);
	if (discriminant < 0.0) {
		return 0.0;
	}
	const float root = sqrt(discriminant);
	const float t1 = b + root;
	if (t1 <= 0.0) {
		return 0.0;
	}
	const float t0 = max(b - root, 0.0);
	// t1^3 - t0^3 factored, which keeps its precision when the two are close
	const float cubes = (t1 - t0) * (t1 * t1 + t1 * t0 + t0 * t0);
	return cubes / (4.0 * CRT_PI * fuzz * fuzz * fuzz);
}

// the cos^alpha lobe around the mirror direction that randomInHemisphere() samples
float phongLobePdf(vec3 reflected, float alpha, vec3 wi)
{
	const float cosine = dot(wi, reflected);
	return cosine > 0.0 ? (alpha + 1.0) / (2.0 * CRT_PI) * pow(cosine, alpha) : 0.0;
}

// GGX's distribution of normals (Trowbridge-Reitz), with `cosine` the angle to the surface normal
float ggxD(float cosine, float alpha)
{
	const float a2 = alpha * alpha;
	const float d = cosine * cosine * (a2 - 1.0) + 1.0;
	return a2 / (CRT_PI * d * d);
}

// scatterPbr()'s choice between its lobes, which depends on the viewing direction alone: the
// Fresnel term there, the diffuse albedo it leaves, and the chance of sampling the specular lobe.
// Must match scatterPbr() expression for expression. False when neither lobe reflects anything
bool pbrLobes(GpuMaterial m, vec3 inDir, vec3 normal, out vec3 f0, out vec3 diffuse, out vec3 fresnelView, out float specularChance)
{
	const vec3 wo = -inDir;
	const float cosO = max(dot(normal, wo), 1e-4);
	f0 = mix(vec3(0.04), m.albedo, m.metallic);
	diffuse = m.albedo * (1.0 - m.metallic);
	fresnelView = fresnelSchlick(f0, cosO);
	const float specularWeight = luminance(fresnelView);
	const float diffuseWeight = luminance(diffuse * (1.0 - fresnelView));
	specularChance = 0.0;
	if (specularWeight + diffuseWeight <= 0.0) {
		return false;
	}
	specularChance = specularWeight / (specularWeight + diffuseWeight);
	return true;
}

// f cos for pbr: the GGX lobe F D G1(wo) G1(wi) / (4 cos_o), which visible-normal sampling divided
// by its density leaves as the F G1(wi) scatterPbr() weighs by, plus the diffuse lobe
vec3 evalPbr(GpuMaterial m, vec3 inDir, vec3 normal, vec3 wi)
{
	vec3 f0;
	vec3 diffuse;
	vec3 fresnelView;
	float specularChance;
	if (!pbrLobes(m, inDir, normal, f0, diffuse, fresnelView, specularChance)) {
		return vec3(0.0);
	}
	const vec3 wo = -inDir;
	const float cosO = dot(normal, wo);
	const float cosI = dot(normal, wi);
	if (cosO <= 0.0 || cosI <= 0.0) {
		return vec3(0.0);
	}
	const float alpha = max(m.roughness * m.roughness, 1e-3);
	const vec3 h = normalize(wo + wi);
	const vec3 specular = fresnelSchlick(f0, dot(wo, h)) * (ggxD(dot(normal, h), alpha) * smithG1(cosO, alpha) * smithG1(cosI, alpha) / (4.0 * cosO));
	return specular + diffuse * (1.0 - fresnelView) * (cosI / CRT_PI);
}

// the density scatterPbr() picks wi with: its lobe choice times each lobe's own density - the
// visible-normal density G1(wo) D / (4 cos_o) for the specular lobe, cos / pi for the diffuse one
float pdfPbr(GpuMaterial m, vec3 inDir, vec3 normal, vec3 wi)
{
	vec3 f0;
	vec3 diffuse;
	vec3 fresnelView;
	float specularChance;
	if (!pbrLobes(m, inDir, normal, f0, diffuse, fresnelView, specularChance)) {
		return 0.0;
	}
	const vec3 wo = -inDir;
	const float cosO = dot(normal, wo);
	const float cosI = dot(normal, wi);
	if (cosO <= 0.0 || cosI <= 0.0) {
		return 0.0;
	}
	const float alpha = max(m.roughness * m.roughness, 1e-3);
	const vec3 h = normalize(wo + wi);
	const float specularPdf = smithG1(cosO, alpha) * ggxD(dot(normal, h), alpha) / (4.0 * cosO);
	return specularChance * specularPdf + (1.0 - specularChance) * (cosI / CRT_PI);
}

// f(wo, wi) |cos theta_i| of the material for a direction someone else chose. `inDir` is the unit
// incoming direction, `normal` the shading normal facing it, `wi` the unit direction towards the
// light. Zero for a specular material and for a direction below the surface
vec3 evalSurface(GpuMaterial m, vec3 inDir, vec3 normal, vec3 wi)
{
	if (dot(normal, wi) <= 0.0) {
		return vec3(0.0);
	}
	switch (m.type) {
	case CRT_MATERIAL_LAMBERTIAN:
		return m.albedo * (dot(normal, wi) / CRT_PI);
	case CRT_MATERIAL_METAL:
		return m.param < CRT_SPECULAR_FUZZ ? vec3(0.0) : m.albedo * fuzzBallPdf(reflect(inDir, normal), m.param, wi);
	case CRT_MATERIAL_PHONG:
		return m.albedo * phongLobePdf(reflect(inDir, normal), pow(1000.0, m.param * m.param), wi);
	case CRT_MATERIAL_PBR:
		return evalPbr(m, inDir, normal, wi);
	}
	return vec3(0.0);
}

// The solid-angle density with which scatterSurface() picks wi: the sampling routine's own density,
// not renormalised over the directions it keeps, since a lobe that absorbs the samples it sends below
// the surface still picks each direction above it with this density
float pdfSurface(GpuMaterial m, vec3 inDir, vec3 normal, vec3 wi)
{
	if (dot(normal, wi) <= 0.0) {
		return 0.0;
	}
	switch (m.type) {
	case CRT_MATERIAL_LAMBERTIAN:
		return dot(normal, wi) / CRT_PI;
	case CRT_MATERIAL_METAL:
		return m.param < CRT_SPECULAR_FUZZ ? 0.0 : fuzzBallPdf(reflect(inDir, normal), m.param, wi);
	case CRT_MATERIAL_PHONG:
		return phongLobePdf(reflect(inDir, normal), pow(1000.0, m.param * m.param), wi);
	case CRT_MATERIAL_PBR:
		return pdfPbr(m, inDir, normal, wi);
	}
	return 0.0;
}

// a textured material's factors modulated by its textures, exactly as glTF defines them: base
// colour by the sRGB base colour texel, roughness and metallic by the metal/rough texel's g and b.
// textureLod: compute has no derivatives to pick a level with, and the array has no mips
GpuMaterial resolveMaterial(uint materialIndex, vec2 uv)
{
	GpuMaterial material = materials[materialIndex];
	if (material.albedoLayer >= 0) {
		const vec3 texel = textureLod(materialTextures, vec3(uv, float(material.albedoLayer)), 0.0).rgb;
		material.albedo *= srgbToLinear(texel);
	}
	if (material.metalRoughLayer >= 0) {
		const vec3 texel = textureLod(materialTextures, vec3(uv, float(material.metalRoughLayer)), 0.0).rgb;
		material.roughness *= texel.g;
		material.metallic *= texel.b;
	}
	return material;
}
