// The BSDFs: given an incoming direction and a surface, pick an outgoing direction and the
// attenuation along it. The first five are a port of the retired src/rt_material.cpp, function for
// function; pbr is glTF 2.0's metallic-roughness model.
//
// Used by kernel 06 Sample Surface Scattering and by nothing else. A variant of 06 that
// importance-samples lights rather than the BSDF still calls scatterSurface() for its BSDF
// lobe - it is the sampling STRATEGY that differs, not the material model - so this file is
// shared by every 06 variant rather than living inside one.
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
