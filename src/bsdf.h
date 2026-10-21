#pragma once

// The surface scattering of the GPU path tracer, as plain C++: the six original materials of
// shaders/rt/include/crt_scatter.glsl, mirrored line for line so that bin/bsdf_test can check them
// against the properties a sampling routine and its density must share without a GPU. A change to
// one side must be made to the other, and bin/vulkan_guide's GPU self-test (Windows menu) compares
// the two on the device.
//
// What is mirrored: scatterSurface() (the sample), evalSurface() (f cos for a direction someone
// else chose) and pdfSurface() (the density the sample was drawn with), with the helpers each uses
// and the random number routines of crt_random.glsl and crt_samples.glsl they draw from. What is
// not: a material's textures and normal map (resolveMaterial(), materialShadingNormal()), which
// read the GPU's texture array and the instance's triangles.
//
// A new BSDF (docs/plans/pbrt-bsdf-library.md) lands in both places, and its tests in
// tests/bsdf_test.cpp. The test's machinery is generic over the signature of the three functions
// below, so a type added later needs a row in the test's material list and nothing else.

#include <cstdint>

#include <glm/glm.hpp>

inline constexpr float BSDF_PI = 3.14159265358979323846f;

// the values are the GPU's tags (crt_common.glsl CRT_MATERIAL_*)
enum class BsdfType : uint32_t {
	Lambertian = 0,
	Metal = 1,
	Phong = 2,
	Dielectric = 3,
	Emissive = 4,
	Pbr = 5,
};

// The part of ClassicMaterial (crt_material.glsl) the scattering reads
struct BsdfMaterial {
	BsdfType type { BsdfType::Lambertian };
	// the base colour
	glm::vec3 albedo { 0.8f };
	// metal: fuzz. phong: smoothness. dielectric: index of refraction. Unused by the others
	float param { 0.f };
	float metallic { 0.f };
	float roughness { 0.5f };
};

// The random numbers a scatter draws, in the order the shader asks for them: the path's PCG state
// (PathState.rngState), or the sample record kernel 01 wrote (crt_samples.glsl). `record` is the
// shader's specialization constant CRT_SAMPLE_RECORD
struct BsdfDraws {
	uint32_t rng { 1 };
	bool record { false };
	// the eight fields of a bounce's direct and indirect dimensions: field 4 is the lobe choice
	// and fields 5 and 6 the direction
	float fields[8] {};
};

// crt_random.glsl randomFloat(): a draw in [0, 1). The 32-bit word converts to a float that rounds up
// to 2^32 for the top 128 words, so the result is clamped to the largest float below 1
float bsdfRandomFloat(uint32_t& state);
// the conversion alone, for the test of its top words
float bsdfWordToFloat(uint32_t word);

// crt_samples.glsl sampleDimension() and sampleDimension2D()
float bsdfSampleDimension(BsdfDraws& draws, uint32_t field);
glm::vec2 bsdfSampleDimension2D(BsdfDraws& draws, uint32_t field);

// the warps of a sample (crt_samples.glsl)
glm::vec3 bsdfUnitVectorFromSample(glm::vec2 u);
glm::vec3 bsdfHemisphereFromSample(glm::vec3 normal, float alpha, glm::vec2 u);
glm::vec3 bsdfBallFromSample(glm::vec2 u, float radius);
// crt_random.glsl
glm::mat3 bsdfTangentSpace(glm::vec3 normal);
glm::vec3 bsdfRandomInBall(uint32_t& state);

// crt_scatter.glsl. `inDir` is the unit incoming direction, `normal` the shading normal facing it
// (facingShadingNormal()), `frontFace` the side of the surface the ray arrived on. Returns false
// when the path is absorbed
bool scatterSurface(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, bool frontFace, BsdfDraws& draws, glm::vec3& attenuation, glm::vec3& outDir);

// true for a material whose scattering is a delta distribution
bool isSpecularMaterial(const BsdfMaterial& m);

// THE ALBEDO THE AUXILIARY OUTPUTS REPORT (crt_surface.glsl aovCapture(), docs/plans/completed/pbrt-aovs-and-image-output.md):
// the mean of the weights scatterSurface() returns over pbrt-v4's 16 fixed samples (surfscatter.cpp's ucRho and uRho, which
// crt_samples.glsl holds as CRT_ALBEDO_UC and CRT_ALBEDO_U), an absorbed sample counting 0. A Lambertian surface gives its
// colour, glass about 1, a mirror its tint, a fuzzy metal its tint times the share of samples that do not go below the
// surface. The fuzz radius draws from a stream seeded by the sample's index, as the shader's does
inline constexpr int BSDF_ALBEDO_SAMPLES = 16;
glm::vec3 bsdfAlbedoEstimate(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, bool frontFace);

// f(wo, wi) |cos theta_i| for a direction someone else chose. Zero for a specular material and for a
// direction below the surface
glm::vec3 evalSurface(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, glm::vec3 wi);

// the solid-angle density with which scatterSurface() picks wi, not renormalized over the
// directions it keeps
float pdfSurface(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, glm::vec3 wi);

// the pieces the tests look at on their own
float bsdfSchlick(float cosine, float refIdx);
glm::vec3 bsdfRefract(glm::vec3 uv, glm::vec3 n, float etaiOverEtat);
glm::vec3 bsdfFresnelSchlick(glm::vec3 f0, float cosine);
float bsdfSmithG1(float cosine, float alpha);
float bsdfGgxD(float cosine, float alpha);
float bsdfFuzzBallPdf(glm::vec3 r, float fuzz, glm::vec3 wi);
float bsdfPhongLobePdf(glm::vec3 reflected, float alpha, glm::vec3 wi);
glm::vec3 bsdfSampleGgxVisibleNormal(glm::vec3 wo, float alpha, glm::vec2 u);
// scatterPbr()'s choice of lobe: false when neither lobe reflects anything
bool bsdfPbrLobes(const BsdfMaterial& m, glm::vec3 inDir, glm::vec3 normal, glm::vec3& f0, glm::vec3& diffuse, glm::vec3& fresnelView, float& specularChance);

// a metal's fuzz below which its reflection is a perfect mirror (CRT_SPECULAR_FUZZ)
inline constexpr float BSDF_SPECULAR_FUZZ = 1e-4f;
