#pragma once

// A MODEL AS PLAIN DATA, before anything of it is on the GPU: what an importer other than the glTF loader produces
// (today pbrt_import.h), and what loadImportedModel() (vk_loader.h) turns into a LoadedModel, the same in-engine
// model a glTF file becomes. No Vulkan, so bin/pbrt_test can build and inspect one.
//
// Everything is in the renderer's world: right-handed, the camera looking down -z, uvs with v running down the
// image as glTF's do. An importer converts into that, not the engine.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rt_material.h>
#include <rt_scene_types.h>

// what an import changed, left out or could not read, with where in the file it first happened and how often
struct ImportMessage {
	std::string text;
	// "file:line" of the first time
	std::string where;
	size_t count { 1 };
	// the plan in docs/plans/ that adds what is missing, empty for none
	std::string plan;
};

// The import report: every substitution and every directive or parameter the import did not use, one line per
// distinct message, so a difference between a render and pbrt's can be traced to what the import dropped
struct ImportReport {
	std::vector<ImportMessage> messages;

	// adds a message, or counts it again if the same text is already there
	void add(const std::string& text, const std::string& where, const std::string& plan = {});
};

// How an 8-bit image file's values are encoded, which the texture array's sRGB decode needs to know
enum class ImageEncoding : uint8_t {
	Srgb,
	Linear,
	// value^gamma is linear
	Gamma
};

// an image file a material reads, decoded by the engine when it builds the model
struct ImportedImage {
	// the file, or with `rgba8` only its name, for messages
	std::filesystem::path path;
	ImageEncoding encoding { ImageEncoding::Srgb };
	float gamma { 1.f };
	// pbrt's "invert": 1 - value
	bool invert { false };
	// values the shaders read as they are (a normal map), not a color they decode from sRGB
	bool data { false };
	// the image already decoded instead of a file (a texture a .glb embeds): rgba8 rows of width x height, in the
	// encoding `encoding` says
	std::vector<uint8_t> rgba8;
	uint32_t width { 0 };
	uint32_t height { 0 };
};

struct ImportedMaterial {
	// what the material library entry starts as: its type and parameters. Texture parameters are filled in by the
	// engine once the images have layers
	MaterialDef definition;
	// images (into ImportedModel::images) read as the base colour and the normal map, -1 for none
	int baseColorImage { -1 };
	int normalImage { -1 };
	// what the raster preview shows, which has no path tracer to resolve the real appearance with
	glm::vec4 previewColor { 1.f };
	float previewMetallic { 0.f };
	float previewRoughness { 1.f };
};

// part of a mesh with one material
struct ImportedSurface {
	uint32_t firstIndex { 0 };
	uint32_t indexCount { 0 };
	// into ImportedModel::materials
	uint32_t material { 0 };
};

struct ImportedMesh {
	std::string name;
	std::vector<glm::vec3> positions;
	// per position, or empty: the triangles' own normals then shade them
	std::vector<glm::vec3> normals;
	// per position, or empty
	std::vector<glm::vec2> uvs;
	// per position, or empty: xyz the tangent, w the sign the shader's bitangent cross(normal, tangent) takes. An
	// importer supplies them in its own format's convention for normal maps; empty ones are generated from the uvs
	std::vector<glm::vec4> tangents;
	std::vector<uint32_t> indices;
	std::vector<ImportedSurface> surfaces;
};

// one placement of a mesh: what becomes a mesh object
struct ImportedNode {
	std::string name;
	// into ImportedModel::meshes
	uint32_t mesh { 0 };
	glm::mat4 transform { 1.f };
};

// an analytic shape the model brings, and its material (into ImportedModel::materials)
struct ImportedShape {
	SceneShape shape;
	uint32_t material { 0 };
};

// an environment map as linear rgba floats, row 0 at the top, in the renderer's equirectangular layout
struct ImportedEnvironment {
	uint32_t width { 0 };
	uint32_t height { 0 };
	std::vector<float> rgba;
	// the environment intensity it is meant to be shown at
	float intensity { 1.f };
	// what the environment map window names it by
	std::string label;
};

// what the file asks a render to be, which "Use pbrt settings" applies
struct ImportedSettings {
	RenderSettings render;
	// kernel 01's variant id ("halton", "zsobol", ...), empty to keep the scene's
	std::string sampler;
	// pbrt's "ambientocclusion" integrator, which is the ambient occlusion debug view at this distance
	std::optional<float> ambientOcclusionDistance;
	// pbrt's imaging ratio, shutter x ISO / 100, which pbrt multiplies into every pixel of the image it writes and
	// the renderer applies as the camera's exposure (display only: an EXR export leaves it out)
	float imagingRatio { 1.f };
};

struct ImportedModel {
	std::vector<ImportedImage> images;
	std::vector<ImportedMaterial> materials;
	std::vector<ImportedMesh> meshes;
	std::vector<ImportedNode> nodes;
	std::vector<ImportedShape> shapes;
	std::vector<SceneLight> lights;
	std::vector<SceneCamera> cameras;
	std::optional<ImportedEnvironment> environment;
	std::optional<ImportedSettings> settings;
	ImportReport report;
};
