// The BSDFs: given an incoming direction and a surface, pick an outgoing direction and the
// attenuation along it. The first five are a port of the retired src/rt_material.cpp, function for
// function; pbr is glTF 2.0's metallic-roughness model.
//
// Each material also answers the two questions light sampling asks of it: evalSurface(), the
// f(wo, wi) cos(theta_i) its scattering estimates, for a direction a light sample chose; and
// pdfSurface(), the density with which scatterSurface() would itself have picked that direction,
// which is the BSDF half of every MIS weight. Both are derived from the sampling routine rather
// than the other way round. For lambertian, phong and the fuzzy metal the attenuation
// scatterSurface() returns is exactly evalSurface() / pdfSurface() for the direction it picked. For
// pbr it is the weight of the lobe it chose, f_lobe cos / (chance pdf_lobe), which equals that ratio
// only in expectation: the expected weight of the samples in any region is the integral of
// evalSurface() over it, and that - with MIS weights that depend on the direction alone - is the
// condition for the strategies to agree (bin/bsdf_test checks it cell by cell). A dielectric, and a
// metal with no fuzz, are specular: a delta distribution no light sample can land in.
//
// Used by kernel 06 Sample Surface Scattering and by nothing else. Every 06 variant calls
// scatterSurface() for its BSDF lobe - it is the sampling STRATEGY that differs, not the material
// model - so this file is shared by every 06 variant rather than living inside one.
//
// The material is evaluated here too: its textures (resolveMaterial()) and its normal map
// (materialShadingNormal()), both from the SurfaceInteraction kernel 06 rebuilt.
//
// THE SPECTRUM. Every color here is a Spectrum (crt_spectrum.glsl): the vec3 it always was in the exact RGB build,
// whose expressions are the ones below operation for operation, and the path's lanes in a spectral build, where the
// material's color is its albedo reference at the path's wavelengths and a choice between lobes weighs a color by
// the mean of its lanes in place of its luminance.
//
// Requires crt_common.glsl, crt_spectrum.glsl, crt_interaction.glsl, crt_random.glsl and crt_samples.glsl.

#include "crt_material.glsl"

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
Spectrum fresnelSchlick(Spectrum f0, float cosine)
{
	return spectrumAdd(f0, spectrumScale(spectrumOneMinus(f0), pow(1.0 - clamp(cosine, 0.0, 1.0), 5.0)));
}

// the material's color: its RGB albedo in the exact RGB build, its albedo reference at the path's lanes in a spectral
// one (resolveMaterial())
Spectrum materialAlbedo(ClassicMaterial m)
{
#ifndef CRT_SPECTRAL
	return m.albedo;
#else
	return m.color;
#endif
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
bool scatterPbr(ClassicMaterial m, vec3 inDir, vec3 normal, uint pathIndex, inout uint rng, out Spectrum attenuation, out vec3 outDir)
{
	const vec3 wo = -inDir;
	const float cosO = max(dot(normal, wo), 1e-4);
	const Spectrum f0 = spectrumMix(spectrumConst(0.04), materialAlbedo(m), m.metallic);
	const Spectrum diffuse = spectrumScale(materialAlbedo(m), 1.0 - m.metallic);
	const Spectrum fresnelView = fresnelSchlick(f0, cosO);

	const float specularWeight = spectrumSelectionWeight(fresnelView);
	const float diffuseWeight = spectrumSelectionWeight(spectrumMul(diffuse, spectrumOneMinus(fresnelView)));
	if (specularWeight + diffuseWeight <= 0.0) {
		attenuation = spectrumConst(0.0);
		outDir = normal;
		return false;
	}
	const float specularChance = specularWeight / (specularWeight + diffuseWeight);

	// A material with no diffuse lobe (metallic 1) always takes the specular one: its diffuse branch
	// below divides 0 by 1 - specularChance = 0, and a lobe draw at or above specularChance would reach
	// it. The draw is made either way, so the path's stream does not change
	const float lobeDraw = sampleDimension(pathIndex, rng, CRT_SAMPLE_INDIRECT_LOBE);
	if (diffuseWeight <= 0.0 || lobeDraw < specularChance) {
		// perceptual roughness squared is GGX's alpha; a floor keeps a "mirror" out of the
		// singular limit the sampler divides by
		const float alpha = max(m.roughness * m.roughness, 1e-3);
		const mat3 frame = getTangentSpace(normal);
		const vec3 woLocal = transpose(frame) * wo;
		const vec3 h = sampleGgxVisibleNormal(woLocal, alpha, sampleDimension2D(pathIndex, rng, CRT_SAMPLE_INDIRECT_DIRECTION));
		const vec3 wiLocal = reflect(-woLocal, h);
		if (wiLocal.z <= 0.0) {
			attenuation = spectrumConst(0.0);
			outDir = normal;
			return false;
		}
		attenuation = spectrumDivide(spectrumScale(fresnelSchlick(f0, dot(woLocal, h)), smithG1(wiLocal.z, alpha)), specularChance);
		outDir = frame * wiLocal;
		return true;
	}

	// normal + a point on the unit sphere is a cosine-weighted direction, as for lambertian
	vec3 d = normal + unitVectorFromSample(sampleDimension2D(pathIndex, rng, CRT_SAMPLE_INDIRECT_DIRECTION));
	if (dot(d, d) < 1e-8) {
		d = normal;
	}
	attenuation = spectrumDivide(spectrumMul(diffuse, spectrumOneMinus(fresnelView)), 1.0 - specularChance);
	outDir = d;
	return true;
}

// the materials, by type. `inDir` is the unit incoming direction. Returns false when the path is
// absorbed
bool scatterSurface(ClassicMaterial m, vec3 inDir, vec3 normal, bool frontFace, uint pathIndex, inout uint rng, out Spectrum attenuation, out vec3 outDir)
{
	switch (m.type) {
	case CRT_MATERIAL_LAMBERTIAN: {
		// normal + a point on the unit sphere is a true cosine-weighted Lambertian distribution
		vec3 d = normal + unitVectorFromSample(sampleDimension2D(pathIndex, rng, CRT_SAMPLE_INDIRECT_DIRECTION));
		// the two can cancel almost exactly, and a zero direction turns into NaN once normalised
		if (dot(d, d) < 1e-8) {
			d = normal;
		}
		attenuation = materialAlbedo(m);
		outDir = d;
		return true;
	}
	case CRT_MATERIAL_METAL: {
		vec3 reflected = reflect(inDir, normal);
		// the albedo estimate of the auxiliary outputs draws pbrt's fixed numbers, never the path's (crt_samples.glsl)
		const bool albedoSamples = CRT_AOV && g_albedoSample >= 0;
		if (!CRT_SAMPLE_RECORD && !albedoSamples) {
			// without a record, the expression it always was: written through ballFromSample() it
			// rounds differently on Metal, by one ulp, and the render is no longer bit-identical
			outDir = reflected + m.param * randomInBall(rng);
		} else {
			// the direction takes the record's two numbers, the radius the stream's next
			const vec2 ballDirection = albedoSamples ? CRT_ALBEDO_U[max(g_albedoSample, 0)] : sampleRecordField2D(pathIndex, CRT_SAMPLE_INDIRECT_DIRECTION);
			const float ballRadius = randomFloat(rng);
			outDir = reflected + m.param * ballFromSample(ballDirection, ballRadius);
		}
		attenuation = materialAlbedo(m);
		return dot(outDir, normal) > 0.0;
	}
	case CRT_MATERIAL_PHONG: {
		vec3 reflected = reflect(inDir, normal);
		float alpha = pow(1000.0, m.param * m.param);
		outDir = hemisphereFromSample(reflected, alpha, sampleDimension2D(pathIndex, rng, CRT_SAMPLE_INDIRECT_DIRECTION));
		attenuation = materialAlbedo(m);
		return dot(outDir, normal) > 0.0;
	}
	case CRT_MATERIAL_DIELECTRIC: {
		attenuation = spectrumConst(1.0);
		float ratio = frontFace ? (1.0 / m.param) : m.param;
		float cosTheta = min(dot(-inDir, normal), 1.0);
		float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
		bool cannotRefract = ratio * sinTheta > 1.0;
		if (cannotRefract || schlick(cosTheta, ratio) > sampleDimension(pathIndex, rng, CRT_SAMPLE_INDIRECT_LOBE)) {
			outDir = reflect(inDir, normal);
		} else {
			outDir = refractDir(inDir, normal, ratio);
		}
		return true;
	}
	case CRT_MATERIAL_PBR:
		return scatterPbr(m, inDir, normal, pathIndex, rng, attenuation, outDir);
	}
	attenuation = spectrumConst(0.0);
	outDir = normal;
	return false;
}

//---------------------------------------------------------------- evaluation, for light sampling

// a metal's fuzz below which its reflection is a perfect mirror: a delta distribution
#define CRT_SPECULAR_FUZZ 1e-4

// true for a material whose scattering is a delta distribution: no light sample can land in it, so
// a vertex on one samples no light and the next hit counts at full weight
bool isSpecularMaterial(ClassicMaterial m)
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
bool pbrLobes(ClassicMaterial m, vec3 inDir, vec3 normal, out Spectrum f0, out Spectrum diffuse, out Spectrum fresnelView, out float specularChance)
{
	const vec3 wo = -inDir;
	const float cosO = max(dot(normal, wo), 1e-4);
	f0 = spectrumMix(spectrumConst(0.04), materialAlbedo(m), m.metallic);
	diffuse = spectrumScale(materialAlbedo(m), 1.0 - m.metallic);
	fresnelView = fresnelSchlick(f0, cosO);
	const float specularWeight = spectrumSelectionWeight(fresnelView);
	const float diffuseWeight = spectrumSelectionWeight(spectrumMul(diffuse, spectrumOneMinus(fresnelView)));
	specularChance = 0.0;
	if (specularWeight + diffuseWeight <= 0.0) {
		return false;
	}
	specularChance = specularWeight / (specularWeight + diffuseWeight);
	return true;
}

// f cos for pbr: the GGX lobe F D G1(wo) G1(wi) / (4 cos_o), which visible-normal sampling divided
// by its density leaves as the F G1(wi) scatterPbr() weighs by, plus the diffuse lobe
Spectrum evalPbr(ClassicMaterial m, vec3 inDir, vec3 normal, vec3 wi)
{
	Spectrum f0;
	Spectrum diffuse;
	Spectrum fresnelView;
	float specularChance;
	if (!pbrLobes(m, inDir, normal, f0, diffuse, fresnelView, specularChance)) {
		return spectrumConst(0.0);
	}
	const vec3 wo = -inDir;
	const float cosO = dot(normal, wo);
	const float cosI = dot(normal, wi);
	if (cosO <= 0.0 || cosI <= 0.0) {
		return spectrumConst(0.0);
	}
	const float alpha = max(m.roughness * m.roughness, 1e-3);
	const vec3 h = normalize(wo + wi);
	const Spectrum specular = spectrumScale(fresnelSchlick(f0, dot(wo, h)), ggxD(dot(normal, h), alpha) * smithG1(cosO, alpha) * smithG1(cosI, alpha) / (4.0 * cosO));
	return spectrumAdd(specular, spectrumScale(spectrumMul(diffuse, spectrumOneMinus(fresnelView)), cosI / CRT_PI));
}

// the density scatterPbr() picks wi with: its lobe choice times each lobe's own density - the
// visible-normal density G1(wo) D / (4 cos_o) for the specular lobe, cos / pi for the diffuse one
float pdfPbr(ClassicMaterial m, vec3 inDir, vec3 normal, vec3 wi)
{
	Spectrum f0;
	Spectrum diffuse;
	Spectrum fresnelView;
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
Spectrum evalSurface(ClassicMaterial m, vec3 inDir, vec3 normal, vec3 wi)
{
	if (dot(normal, wi) <= 0.0) {
		return spectrumConst(0.0);
	}
	switch (m.type) {
	case CRT_MATERIAL_LAMBERTIAN:
		return spectrumScale(materialAlbedo(m), dot(normal, wi) / CRT_PI);
	case CRT_MATERIAL_METAL:
		return m.param < CRT_SPECULAR_FUZZ ? spectrumConst(0.0) : spectrumScale(materialAlbedo(m), fuzzBallPdf(reflect(inDir, normal), m.param, wi));
	case CRT_MATERIAL_PHONG:
		return spectrumScale(materialAlbedo(m), phongLobePdf(reflect(inDir, normal), pow(1000.0, m.param * m.param), wi));
	case CRT_MATERIAL_PBR:
		return evalPbr(m, inDir, normal, wi);
	}
	return spectrumConst(0.0);
}

// The solid-angle density with which scatterSurface() picks wi: the sampling routine's own density,
// not renormalised over the directions it keeps, since a lobe that absorbs the samples it sends below
// the surface still picks each direction above it with this density
float pdfSurface(ClassicMaterial m, vec3 inDir, vec3 normal, vec3 wi)
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

// The object-space normal a material's normal map gives at this point of a triangle, or false where
// there is none to apply: no map, or a vertex without a tangent (a mesh with no uvs, or degenerate
// ones). `objectNormal` is the interpolated vertex normal (triangleObjectNormal())
bool normalMapped(ClassicMaterial material, GpuTriangleAttributes attributes, vec2 bary, vec2 uv, vec3 objectNormal, out vec3 mapped)
{
	mapped = objectNormal;
	if (material.normalLayer < 0 || (attributes.tangents.w & CRT_TANGENT_MISSING) != 0u) {
		return false;
	}
	const float w = 1.0 - bary.x - bary.y;
	const vec3 n = normalize(objectNormal);
	vec3 t = w * unpackTangent(attributes.tangents.x) + bary.x * unpackTangent(attributes.tangents.y) + bary.y * unpackTangent(attributes.tangents.z);
	t -= n * dot(n, t);
	if (dot(t, t) < 1e-12) {
		return false;
	}
	t = normalize(t);
	// glTF: bitangent = cross(normal, tangent.xyz) * tangent.w. The first vertex's sign stands for
	// the triangle's, since a mirrored uv seam never runs through a triangle's interior
	const vec3 b = cross(n, t) * ((attributes.tangents.w & 1u) != 0u ? -1.0 : 1.0);
	vec3 texel = textureLod(materialTextures, vec3(uv, float(material.normalLayer)), 0.0).xyz * 2.0 - 1.0;
	texel.xy *= material.normalScale;
	// built in object space and carried to world space like any normal. Under a non-uniform scale
	// that skews the tangent components slightly; every model in use is scaled uniformly
	mapped = t * texel.x + b * texel.y + n * texel.z;
	return dot(mapped, mapped) > 1e-12;
}

// The shading normal the materials below take: the interaction's ns turned to face the incoming ray
// (facingShadingNormal()), then perturbed by the material's normal map. A mapped normal is kept only
// while it still faces the ray: one tilted past the viewer would send the BSDF's continuation into
// the surface. `frontFace` is the side of the unmapped normal the ray arrived on, since a normal map
// changes how a surface shades, not which side it is. Only a glTF material, on a triangle, has a map
vec3 materialShadingNormal(ClassicMaterial material, HitRecord hit, SurfaceInteraction si, vec3 inDir, out bool frontFace)
{
	vec3 normal = facingShadingNormal(si, inDir, frontFace);
	const GpuInstance instance = instances[hit.instance];
	if (material.normalLayer < 0 || instance.nodeBase == CRT_INSTANCE_SHAPE) {
		return normal;
	}
	const BvhTriangle tri = blasTriangles[hit.primitive];
	const GpuTriangleAttributes attributes = triangleAttributes[instance.attributeBase + (floatBitsToUint(tri.v0.w) & CRT_TRIANGLE_INDEX_MASK)];
	vec3 mappedObjectNormal;
	if (normalMapped(material, attributes, hit.barycentrics, si.uv, triangleObjectNormal(attributes, tri, hit.barycentrics), mappedObjectNormal)) {
		vec3 mapped = worldNormal(instance, mappedObjectNormal);
		mapped = frontFace ? mapped : -mapped;
		if (dot(mapped, inDir) < 0.0) {
			normal = mapped;
		}
	}
	return normal;
}

// a textured material's factors modulated by its textures, exactly as glTF defines them: base
// colour by the sRGB base colour texel, roughness and metallic by the metal/rough texel's g and b.
// textureLod: compute has no derivatives to pick a level with, and the array has no mips
ClassicMaterial resolveMaterial(uint materialIndex, vec2 uv)
{
	ClassicMaterial material = classicMaterial(materialIndex);
	if (material.albedoLayer >= 0) {
		const vec3 texel = textureLod(materialTextures, vec3(uv, float(material.albedoLayer)), 0.0).rgb;
		material.albedo *= srgbToLinear(texel);
	}
#ifdef CRT_SPECTRAL
	// the albedo reference at the path's lanes, an RGB one uplifted with its texel multiplied in (pbrt's
	// RGBAlbedoSpectrum of the textured color)
	vec3 tint = vec3(1.0);
	if (material.albedoLayer >= 0) {
		tint = srgbToWorking(srgbToLinear(textureLod(materialTextures, vec3(uv, float(material.albedoLayer)), 0.0).rgb));
	}
	material.color = spectrumFromRef(materialSpectrumRef(materialIndex, CRT_SLOT_ALBEDO), tint);
#endif
	if (material.metalRoughLayer >= 0) {
		const vec3 texel = textureLod(materialTextures, vec3(uv, float(material.metalRoughLayer)), 0.0).rgb;
		material.roughness *= texel.g;
		material.metallic *= texel.b;
	}
	return material;
}

//---------------------------------------------------------------- the complex build's materials
//
// Kernel 06's complex build evaluates every type, pbrt-v4's BxDFs (crt_bxdf.glsl: glass, conductor) as well as the
// six original ones. Its material is a ComplexMaterial, and these overloads of scatterSurface(), evalSurface(),
// pdfSurface() and isSpecularMaterial() take one, so crt_surface.glsl's body is the same text in both builds
// (SurfaceMaterial). The basic build never sees any of this, which keeps it the code it was.

#if defined(CRT_MATERIAL_CLASS_EVALUATES_COMPLEX) && CRT_MATERIAL_CLASS_EVALUATES_COMPLEX

#include "crt_fresnel.glsl"
#include "crt_microfacet.glsl"
#include "crt_bxdf.glsl"

// src/rt_material_record.h CRT_BXDF_FLAG_*
#define CRT_BXDF_FLAG_REFLECTANCE 1u
#define CRT_BXDF_FLAG_DISPERSIVE 2u

struct ComplexMaterial {
	ClassicMaterial classic;
	BxdfMaterial bxdf;
	bool isBxdf;
	// the shading frame's tangent comes from dp/du (shadingFrame()); the BxDFs' u roughness runs along it
	vec3 dpdu;
	// CRT_BXDF_FLAG_*, and the material's index, for its spectra
	uint bxdfFlags;
	uint index;
	// the ray arrived on the outside of the surface as authored: which way a glass's index reads. Kernel 06 sets it
	// with the shading normal (materialShadingNormal())
	bool frontFace;
};

bool isBxdfMaterialType(uint type)
{
	return type == CRT_MATERIAL_GLASS || type == CRT_MATERIAL_CONDUCTOR;
}

// A glass's or a conductor's record (src/rt_material_record.h CrtBxdfMaterial) as the BxDF reads it. Exact RGB takes
// the RGB fields: the glass's index at the design wavelength, the conductor's eta and k at 630, 532 and 465 nm. A
// spectral build reads the conductor's spectra at the path's lanes, and the glass's index where
// applyDispersion() sets it
BxdfMaterial resolveBxdfMaterial(uint index, out uint flags)
{
	const uint base = materials[index].paramOffset;
	const vec4 a = materialParams[base];
	const vec4 b = materialParams[base + 1u];
	const vec4 c = materialParams[base + 2u];
	const vec4 d = materialParams[base + 3u];
	flags = floatBitsToUint(d.w);
	BxdfMaterial m;
	m.type = floatBitsToUint(b.x);
	m.alpha = trowbridgeReitz(b.w, c.w);
	m.eta = a.w;
	m.conductorEta = spectrumConst(1.0);
	m.conductorK = spectrumConst(0.0);
	if (m.type == CRT_MATERIAL_CONDUCTOR) {
		if ((flags & CRT_BXDF_FLAG_REFLECTANCE) != 0u) {
			// pbrt's reflectance mode: eta 1 and the k of the clamped reflectance
#ifndef CRT_SPECTRAL
			const Spectrum r = clamp(c.xyz, vec3(0.0), vec3(0.9999));
#else
			const Spectrum r = spectrumClamp(spectrumFromRef(materialSpectrumRef(index, CRT_SLOT_ALBEDO), vec3(1.0)), 0.0, 0.9999);
#endif
			for (uint i = 0u; i < CRT_LANE_END; i++) {
				const float ri = spectrumLane(r, i);
				spectrumSetLane(m.conductorK, i, 2.0 * sqrt(ri) / sqrt(max(1.0 - ri, 0.0)));
			}
		} else {
#ifndef CRT_SPECTRAL
			m.conductorEta = a.xyz;
			m.conductorK = c.xyz;
#else
			m.conductorEta = spectrumFromScalarRef(materialSpectrumRef(index, CRT_SLOT_AUX0));
			m.conductorK = spectrumFromScalarRef(materialSpectrumRef(index, CRT_SLOT_AUX1));
#endif
		}
	}
	return m;
}

ComplexMaterial resolveComplexMaterial(uint index, SurfaceInteraction si)
{
	ComplexMaterial m;
	m.classic = resolveMaterial(index, si.uv);
	m.isBxdf = isBxdfMaterialType(m.classic.type);
	m.dpdu = si.dpdu;
	m.index = index;
	m.bxdfFlags = 0u;
	m.frontFace = si.frontFace;
	if (m.isBxdf) {
		m.bxdf = resolveBxdfMaterial(index, m.bxdfFlags);
	} else {
		m.bxdf.type = 0u;
		m.bxdf.alpha = vec2(0.0);
		m.bxdf.eta = 1.0;
		m.bxdf.conductorEta = spectrumConst(1.0);
		m.bxdf.conductorK = spectrumConst(0.0);
	}
	return m;
}

// The local frame about the OUTWARD shading normal: `normal` faces the ray, and `frontFace` says which side it came
// from. Columns tangent, bitangent, normal
mat3 bxdfFrame(ComplexMaterial m, vec3 normal, bool frontFace)
{
	const vec3 n = frontFace ? normal : -normal;
	vec3 tangent = m.dpdu - n * dot(n, m.dpdu);
	if (dot(tangent, tangent) < 1e-20) {
		tangent = abs(n.x) > 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		tangent -= n * dot(n, tangent);
	}
	tangent = normalize(tangent);
	return mat3(tangent, cross(n, tangent), n);
}

bool scatterSurface(ComplexMaterial m, vec3 inDir, vec3 normal, bool frontFace, uint pathIndex, inout uint rng, out Spectrum attenuation, out vec3 outDir)
{
	if (!m.isBxdf) {
		return scatterSurface(m.classic, inDir, normal, frontFace, pathIndex, rng, attenuation, outDir);
	}
	const mat3 frame = bxdfFrame(m, normal, m.frontFace);
	const vec3 wo = transpose(frame) * -inDir;
	const float uc = sampleDimension(pathIndex, rng, CRT_SAMPLE_INDIRECT_LOBE);
	const vec2 u = sampleDimension2D(pathIndex, rng, CRT_SAMPLE_INDIRECT_DIRECTION);
	BxdfSample s;
	attenuation = spectrumConst(0.0);
	outDir = normal;
	if (!bxdfSample(m.bxdf, wo, uc, u, CRT_TRANSPORT_RADIANCE, s) || !(s.pdf > 0.0)) {
		return false;
	}
	attenuation = spectrumScale(s.f, abs(s.wi.z) / s.pdf);
	outDir = frame * s.wi;
	return true;
}

// f cos for a direction someone else chose: both hemispheres, since glass transmits
Spectrum evalSurface(ComplexMaterial m, vec3 inDir, vec3 normal, vec3 wi)
{
	if (!m.isBxdf) {
		return evalSurface(m.classic, inDir, normal, wi);
	}
	const mat3 frame = bxdfFrame(m, normal, m.frontFace);
	const vec3 woLocal = transpose(frame) * -inDir;
	const vec3 wiLocal = transpose(frame) * wi;
	return spectrumScale(bxdfF(m.bxdf, woLocal, wiLocal, CRT_TRANSPORT_RADIANCE), abs(wiLocal.z));
}

float pdfSurface(ComplexMaterial m, vec3 inDir, vec3 normal, vec3 wi)
{
	if (!m.isBxdf) {
		return pdfSurface(m.classic, inDir, normal, wi);
	}
	const mat3 frame = bxdfFrame(m, normal, m.frontFace);
	return bxdfPdf(m.bxdf, transpose(frame) * -inDir, transpose(frame) * wi, CRT_TRANSPORT_RADIANCE);
}

bool isSpecularMaterial(ComplexMaterial m)
{
	if (!m.isBxdf) {
		return isSpecularMaterial(m.classic);
	}
	return (bxdfFlags(m.bxdf) & CRT_BXDF_SPECULAR) != 0u;
}

#endif
