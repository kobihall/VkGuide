// The BSDFs: given an incoming direction and a surface, pick an outgoing direction and the
// attenuation along it. A port of src/rt_material.cpp, function for function.
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

// the four materials, by type. `inDir` is the unit incoming direction. Returns false when the
// path is absorbed
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
	}
	attenuation = vec3(0.0);
	outDir = normal;
	return false;
}

// a textured material's albedo is its factor modulated by the texture, exactly as glTF defines
// base colour. textureLod: compute has no derivatives to pick a level with, and the array has
// no mips
GpuMaterial resolveMaterial(uint materialIndex, vec2 uv)
{
	GpuMaterial material = materials[materialIndex];
	if (material.albedoLayer >= 0) {
		const vec3 texel = textureLod(albedoTextures, vec3(uv, float(material.albedoLayer)), 0.0).rgb;
		material.albedo *= srgbToLinear(texel);
	}
	return material;
}
