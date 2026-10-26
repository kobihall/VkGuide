#pragma once

// AUXILIARY OUTPUTS (AOVs), as plain C++ with no Vulkan: which outputs a render keeps, how they sit in the
// AovImages array, what one pixel accumulates and how it resolves into the channels of an image file. The
// line-for-line mirror of shaders/rt/include/crt_aov.glsl: change both together and run bin/aov_test and the
// GPU self-tests. Design: docs/plans/completed/pbrt-aovs-and-image-output.md.
//
// A camera path records the first visible surface it meets (pbrt's VisibleSurface) into its slot of
// PixelState. Kernel 09 folds the slots of a pixel into eight float4 layers with the sample's filter weight
// (the sign of its radiance slot's w), the estimator pbrt's GBufferFilm applies:
//
//   position, normals, uv, depth   sum(w x) over the samples that met a surface, over sum(w) of those
//   albedo                         sum(w albedo) over the sum of w of every sample (a miss has albedo 0)
//   ids                            the first surface sample's, never averaged
//   variance                       the samples' own variance per channel, Welford's, as pbrt's rgbVariance

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

enum class AovOutput : uint32_t {
	Position,
	Depth,
	Normal,
	ShadingNormal,
	Uv,
	Albedo,
	Ids,
	Variance,
	// the samples taken of each pixel: no accumulation of its own, read from the film's count
	SampleCount,
	Count
};

// the name in the panel, the id the scene file saves it under (never rename), and what the output is
const char* aovOutputName(AovOutput output);
const char* aovOutputId(AovOutput output);
// the output with this id, AovOutput::Count for none
AovOutput aovOutputFromId(const std::string& id);
const char* aovOutputDescription(AovOutput output);

// What a render keeps beside its picture. Plain data, saved with the scene; turning an output on restarts a
// render, since it has to accumulate from the first sample
struct AovSettings {
	// one bit per AovOutput
	uint32_t outputs { 0 };
	// record the first NON-SPECULAR surface a path meets instead of its first: a mirror or glass shows what it
	// reflects, which denoisers expect. pbrt records the first
	bool captureThroughSpecular { false };

	bool has(AovOutput output) const { return (outputs >> (uint32_t)output) & 1u; }
	void set(AovOutput output, bool on)
	{
		outputs = on ? (outputs | (1u << (uint32_t)output)) : (outputs & ~(1u << (uint32_t)output));
	}
	// some output needs PixelState and the AOV layers: everything but the sample count
	bool accumulates() const { return (outputs & ~(1u << (uint32_t)AovOutput::SampleCount)) != 0; }

	bool operator==(const AovSettings&) const = default;
};

// the layers of the AovImages array, one float4 each. `Weights` is always there when anything accumulates
enum class AovLayer : uint32_t {
	// x: sum of w over the samples that met a surface (pbrt's gBufferWeightSum), y: sum of w over every sample
	// (weightSum), z and w: the instance and the material of the first surface sample, plus 1 (0 for none)
	Weights,
	// xyz: sum(w P), w: sum(w depth)
	PositionDepth,
	// xyz: sum(w N)
	Normal,
	// xyz: sum(w Ns)
	ShadingNormal,
	// xy: sum(w uv)
	Uv,
	// xyz: sum(w albedo)
	Albedo,
	// xyz: the Welford mean of the samples' rgb, w: how many
	VarianceMean,
	// xyz: Welford's M2
	VarianceM2,
	Count
};

inline constexpr uint32_t AOV_LAYER_COUNT = (uint32_t)AovLayer::Count;
// a layer's index in CrtParams::aovLayers when the render keeps no such layer
inline constexpr uint32_t AOV_LAYER_NONE = 0xFu;

// Which layers the enabled outputs need, and where each sits in the array: the layers a render keeps are
// numbered 0, 1, 2, ... in AovLayer order, so a render with only normals keeps two
struct AovLayout {
	std::array<uint32_t, AOV_LAYER_COUNT> layer {};
	// how many layers the array holds, 0 when no output accumulates
	uint32_t count { 0 };

	bool has(AovLayer which) const { return layer[(size_t)which] != AOV_LAYER_NONE; }
	// the layer indices as the GPU reads them: one nibble per AovLayer, 0xF for none
	uint32_t pack() const;
};

AovLayout makeAovLayout(const AovSettings& settings);
// the layer index a packed word holds for `which`, AOV_LAYER_NONE for none
uint32_t unpackAovLayer(uint32_t packed, AovLayer which);

// What the camera path's first visible surface recorded, as kernels 03, 04 and 06 write it into PixelState
// (src/rt_gpu_types.h CrtPixelState)
struct AovSample {
	// the sample met a surface (false: it left the scene, or the surface was one a render capturing through
	// specular passed through, and the path ended in the background)
	bool hit { false };
	glm::vec3 position { 0.f };
	// along the camera's forward axis
	float depth { 0.f };
	glm::vec3 normal { 0.f };
	glm::vec3 shadingNormal { 0.f };
	glm::vec2 uv { 0.f };
	// the BSDF's directional albedo at the point, 0 for an emitter
	glm::vec3 albedo { 0.f };
	uint32_t instance { 0 };
	uint32_t material { 0 };
};

// One pixel's layers: what kernel 09 keeps per pixel in AovImages, and the arithmetic it does on them
struct AovPixel {
	std::array<glm::vec4, AOV_LAYER_COUNT> layers {};

	// One sample's contribution. `weight` is the filter's sign (+1 or -1), `radiance` the sample's rgb after the
	// film's clamp. Every layer is updated, wanted or not: the layout only decides which ones are kept and read
	void add(const AovSample& sample, float weight, const glm::vec3& radiance);
};

// What a pixel's layers resolve to, the channels of the file
struct AovResolved {
	glm::vec3 position { 0.f };
	float depth { 0.f };
	glm::vec3 normal { 0.f };
	glm::vec3 shadingNormal { 0.f };
	glm::vec2 uv { 0.f };
	glm::vec3 albedo { 0.f };
	// -1 for a pixel whose samples met no surface
	float instance { -1.f };
	float material { -1.f };
	// the variance of the samples per channel, and over the mean as pbrt writes it
	glm::vec3 variance { 0.f };
	glm::vec3 relativeVariance { 0.f };
	// the samples whose radiance the variance took
	float varianceSamples { 0.f };
};

// pbrt's GBufferFilm::GetImage on one pixel: sums over weights, normals normalized, the variance from Welford's
// M2 over n - 1 (0 for fewer than two samples)
AovResolved resolveAov(const AovPixel& pixel);

// A color that tells one id from the next, for the views: the same id gives the same color. The GLSL mirror is
// aovIdColor() in crt_aov.glsl
glm::vec3 aovIdColor(uint32_t id);

// the CrtParams::aovOutputs word: the enabled outputs in bits 0 to 8, and this bit for capturing through specular
inline constexpr uint32_t AOV_PARAM_CAPTURE_THROUGH_SPECULAR = 1u << 31;
// CrtPixelState::state: the path has recorded its surface. A path records at most one
inline constexpr uint32_t AOV_STATE_HIT = 1u;

// What a render kept beside its picture, on the host (GpuRenderer::readOutputs()): the AOV layers and the light
// groups' sums as the GPU holds them, and the samples taken of each pixel. src/render_export.h turns it into files
struct RenderOutputsData {
	uint32_t width { 0 };
	uint32_t height { 0 };
	// the outputs and layers the render ran with
	AovSettings aov;
	AovLayout layout;
	// layout.count layers of width x height x 4 floats, in layer order
	std::vector<float> aovLayers;
	// per pixel, groups x 3 floats of sum(w L), then sum(w), then the spectral images' buckets of sum(w L_bucket)
	// (ChannelSums as the GPU holds it)
	uint32_t lightGroups { 0 };
	std::vector<float> groupSums;
	// the spectral images (docs/plans/completed/pbrt-spectral-rendering.md): the bucket count and each bucket's center
	// in nm. A monochromatic render has none, and its one wavelength instead
	uint32_t spectralBuckets { 0 };
	std::vector<float> spectralWavelengths;
	float monochromaticWavelength { 0.f };
	// per pixel
	std::vector<uint32_t> samplesTaken;
	uint32_t samplesPerPixel { 0 };
};
