#pragma once

// PBRT-V4'S BXDFS (static library rt_bsdf, no Vulkan): the line-for-line C++ mirror of shaders/rt/include/crt_fresnel.glsl,
// crt_microfacet.glsl and crt_bxdf.glsl, which kernel 06's complex build runs for the `glass` and `conductor` material
// types (docs/plans/pbrt-bsdf-library.md; the parts docs/plans/completed/pbrt-spectral-rendering.md needs).
//
// Everything is in pbrt's local shading frame (PBR 4ed 9.1.1): z is the OUTWARD shading normal, directions point away
// from the surface, and wo.z < 0 means the ray arrived from inside. A spectrum is a glm::vec3, as the exact RGB build
// carries it; a spectral build's lanes are independent, so a lane is a channel here with the others equal.
//
//   frDielectric()    exact unpolarized Fresnel reflectance, total internal reflection included (PBR 4ed 9.3.2)
//   frComplex()       the same for a complex index eta + i k, a conductor's (9.3.6)
//   TrowbridgeReitz   the anisotropic GGX distribution: D, Lambda, G1, G, the visible-normal density and its sampling
//                     (9.6), effectivelySmooth() below an alpha of 1e-3
//   DielectricBxdf    smooth or rough glass (Walter et al. 2007, 9.5 and 9.7), radiance mode dividing by eta^2
//   ConductorBxdf     smooth or rough metal with complex Fresnel (9.4)

#include <cstdint>

#include <glm/glm.hpp>

inline constexpr uint32_t BXDF_REFLECTION = 1u << 0;
inline constexpr uint32_t BXDF_TRANSMISSION = 1u << 1;
inline constexpr uint32_t BXDF_GLOSSY = 1u << 3;
inline constexpr uint32_t BXDF_SPECULAR = 1u << 4;

// PBR 4ed 9.5.2: radiance flows along camera paths, importance along light paths (the light integrator's)
enum class TransportMode : uint32_t {
	Radiance = 0,
	Importance = 1
};

// pbrt's BSDFSample: the BxDF's value (not yet times |cos|), the direction, its density (proportional for a delta
// lobe, which is the lobe's chosen probability), what it is, and the relative index a transmission crossed
struct BxdfSample {
	glm::vec3 f { 0.f };
	glm::vec3 wi { 0.f };
	float pdf { 0.f };
	uint32_t flags { 0 };
	float eta { 1.f };
};

float frDielectric(float cosThetaI, float eta);
// a complex index's reflectance at one wavelength, and per channel
float frComplex(float cosThetaI, float eta, float k);
glm::vec3 frComplex(float cosThetaI, glm::vec3 eta, glm::vec3 k);
// Snell's law about n (the side wi is on decides which way eta reads); false for total internal reflection
bool refractDirection(glm::vec3 wi, glm::vec3 n, float eta, float& etap, glm::vec3& wt);

struct TrowbridgeReitz {
	float alphaX { 0.f };
	float alphaY { 0.f };

	// pbrt's constructor: a rough distribution keeps both alphas at least 1e-4
	TrowbridgeReitz(float ax, float ay);
	bool effectivelySmooth() const { return glm::max(alphaX, alphaY) < 1e-3f; }
	float D(glm::vec3 wm) const;
	float lambda(glm::vec3 w) const;
	float G1(glm::vec3 w) const { return 1.f / (1.f + lambda(w)); }
	float G(glm::vec3 wo, glm::vec3 wi) const { return 1.f / (1.f + lambda(wo) + lambda(wi)); }
	// the density of visible normals from w
	float D(glm::vec3 w, glm::vec3 wm) const;
	float pdf(glm::vec3 w, glm::vec3 wm) const { return D(w, wm); }
	glm::vec3 sampleWm(glm::vec3 w, glm::vec2 u) const;
	// pbrt's RoughnessToAlpha: sqrt(roughness)
	static float roughnessToAlpha(float roughness);
};

struct DielectricBxdf {
	float eta { 1.5f };
	TrowbridgeReitz distribution { 0.f, 0.f };

	uint32_t flags() const;
	// `uc` picks reflection or transmission, `u` the microfacet normal
	bool sample(glm::vec3 wo, float uc, glm::vec2 u, TransportMode mode, BxdfSample& out) const;
	glm::vec3 f(glm::vec3 wo, glm::vec3 wi, TransportMode mode) const;
	float pdf(glm::vec3 wo, glm::vec3 wi, TransportMode mode) const;
};

struct ConductorBxdf {
	TrowbridgeReitz distribution { 0.f, 0.f };
	glm::vec3 eta { 1.f };
	glm::vec3 k { 0.f };

	uint32_t flags() const;
	bool sample(glm::vec3 wo, float uc, glm::vec2 u, TransportMode mode, BxdfSample& out) const;
	glm::vec3 f(glm::vec3 wo, glm::vec3 wi, TransportMode mode) const;
	float pdf(glm::vec3 wo, glm::vec3 wi, TransportMode mode) const;
	// pbrt's reflectance mode: eta 1 and the k that gives this normal-incidence reflectance (clamped below 1)
	static ConductorBxdf fromReflectance(const TrowbridgeReitz& distribution, glm::vec3 reflectance);
};
