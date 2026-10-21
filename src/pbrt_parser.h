#pragma once

// READING A pbrt-v4 SCENE FILE (static library rt_pbrt, no Vulkan): the text of a .pbrt file into plain data, with
// every transform, material, texture, light and shape where pbrt-v4 itself would put it.
//
// It is pbrt-v4's own parser (src/pbrt/parser.cpp) and the part of its scene builder (scene.cpp,
// BasicSceneBuilder) that keeps the graphics state, in one pass and without pbrt's utility libraries:
//
//   - The scanner splits the text into tokens: a word, a number, a quoted string, '[' or ']'. A '#' starts a
//     comment that runs to the end of the line. It is pbrt's Tokenizer::Next(), character for character.
//   - The parser reads a directive name and then what that directive takes, as pbrt's parse() does, and keeps
//     the current transform, material, area light and the attribute stack as BasicSceneBuilder does.
//
// What a directive means for the renderer is not decided here: pbrt_import.h maps this data to the scene model
// and reports what it substitutes. Paths a file names (Include, Import, PLY files, textures) are resolved against
// the directory of the first file read, which is pbrt's rule (SetSearchDirectory), not the including file's.
//
// Coordinates are pbrt's: a left-handed world, the camera looking down +z. pbrt_import.h mirrors them.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

// where something was written: an index into PbrtScene::files, and a line from 1
struct PbrtLoc {
	uint32_t file { 0 };
	int line { 0 };
};

// One parameter of a directive, "type name" [values]: "float fov" [45], "rgb reflectance" [.5 .5 .5],
// "spectrum eta" "metal-Au-eta". Numbers keep their double value whatever the type says; a "bool" written as quoted
// strings (pbrt-v3's form) reads as bools
struct PbrtParam {
	// as pbrt-v4 spells them: "color", "point" and "vector" become "rgb", "point3" and "vector3"
	std::string type;
	std::string name;
	std::vector<double> numbers;
	std::vector<std::string> strings;
	std::vector<bool> bools;
	PbrtLoc loc;
	// set by every lookup, so the importer can report the parameters nothing read
	mutable bool used { false };
};

// a directive's parameter list with pbrt's ParameterDictionary lookups. A lookup that finds the name marks it used
struct PbrtParams {
	std::vector<PbrtParam> list;

	// the parameter called `name`, or null
	const PbrtParam* find(std::string_view name) const;
	double getFloat(std::string_view name, double fallback) const;
	int getInt(std::string_view name, int fallback) const;
	bool getBool(std::string_view name, bool fallback) const;
	std::string getString(std::string_view name, std::string_view fallback) const;
	// three numbers ("point3", "vector3", "normal", "rgb"), or nullopt
	std::optional<glm::dvec3> getVector3(std::string_view name) const;
	// every number, empty if there is no such parameter
	std::vector<double> getNumbers(std::string_view name) const;
	// the name a "texture" parameter refers to, or empty
	std::string getTexture(std::string_view name) const;
	// the parameters no lookup has asked for
	std::vector<const PbrtParam*> unused() const;
};

// A directive with a type and parameters: the camera, film, sampler, filter, integrator, accelerator, a material,
// an area light, a medium
struct PbrtEntity {
	// "perspective", "rgb", "halton", "diffuse": empty for one the file never gave
	std::string type;
	PbrtParams params;
	PbrtLoc loc;
};

struct PbrtCamera : PbrtEntity {
	// the transform current at the Camera directive, which pbrt calls cameraFromWorld
	glm::dmat4 cameraFromWorld { 1.0 };
};

// Texture "name" "float"|"spectrum" "class" params
struct PbrtTexture {
	std::string name;
	// "spectrum" textures color a material; "float" ones scale a number (roughness, bump, alpha)
	bool spectrum { true };
	// imagemap, scale, mix, checkerboard, ...
	std::string className;
	PbrtParams params;
	PbrtLoc loc;
	glm::dmat4 worldFromTexture { 1.0 };
};

// which material a shape uses: an index into PbrtScene::materials, or a name in namedMaterials, which pbrt resolves
// when the whole file is read, so a NamedMaterial may come before its MakeNamedMaterial
struct PbrtMaterialRef {
	int index { -1 };
	std::string name;
};

struct PbrtShape {
	// trianglemesh, plymesh, sphere, disk, ...
	std::string type;
	PbrtParams params;
	PbrtLoc loc;
	glm::dmat4 worldFromObject { 1.0 };
	// the transform at the end of the shutter differs from the start (ActiveTransform EndTime): motion blur
	bool animated { false };
	bool reverseOrientation { false };
	PbrtMaterialRef material;
	// into PbrtScene::areaLights, -1 for a shape that does not emit
	int areaLight { -1 };
	std::string insideMedium;
	std::string outsideMedium;
};

struct PbrtLight : PbrtEntity {
	glm::dmat4 worldFromLight { 1.0 };
	std::string medium;
};

struct PbrtMedium : PbrtEntity {
	glm::dmat4 worldFromMedium { 1.0 };
};

// ObjectBegin ... ObjectEnd: shapes that ObjectInstance places, each in the transform current when it was defined
struct PbrtObject {
	std::string name;
	std::vector<PbrtShape> shapes;
	PbrtLoc loc;
};

struct PbrtInstance {
	std::string object;
	// the transform current at ObjectInstance; a shape of the object lands at worldFromInstance * its own transform
	glm::dmat4 worldFromInstance { 1.0 };
	bool animated { false };
	PbrtLoc loc;
};

struct PbrtMessage {
	PbrtLoc loc;
	std::string text;
};

struct PbrtScene {
	// every file read, PbrtLoc::file indexes it; [0] is the file the read started from
	std::vector<std::filesystem::path> files;
	// what relative paths resolve against: the first file's directory
	std::filesystem::path directory;

	std::optional<PbrtCamera> camera;
	PbrtEntity film;
	PbrtEntity sampler;
	PbrtEntity filter;
	PbrtEntity integrator;
	PbrtEntity accelerator;
	// the last ColorSpace directive; pbrt's default is "srgb"
	std::string colorSpace { "srgb" };
	// Option "name" value
	std::vector<std::pair<std::string, std::string>> options;
	// shutter times for motion blur (TransformTimes); pbrt's defaults are 0 and 1
	double transformStartTime { 0.0 };
	double transformEndTime { 1.0 };

	// the anonymous materials (Material directives); [0] is pbrt's default, a "diffuse" a shape gets before any
	// Material or NamedMaterial
	std::vector<PbrtEntity> materials;
	// MakeNamedMaterial: the type is the "string type" parameter
	std::map<std::string, PbrtEntity> namedMaterials;
	std::map<std::string, PbrtTexture> floatTextures;
	std::map<std::string, PbrtTexture> spectrumTextures;
	// AreaLightSource directives that some shape used
	std::vector<PbrtEntity> areaLights;
	std::vector<PbrtShape> shapes;
	std::vector<PbrtLight> lights;
	std::map<std::string, PbrtMedium> media;
	std::map<std::string, PbrtObject> objects;
	std::vector<PbrtInstance> instances;
	// shape types PbrtParseOptions::skipShapes left out: how many, and where the first was
	std::map<std::string, std::pair<size_t, PbrtLoc>> skippedShapes;
	// what the file does that pbrt would warn about, without failing the read
	std::vector<PbrtMessage> warnings;

	// "file:line" for a location
	std::string where(const PbrtLoc& loc) const;
};

struct PbrtParseOptions {
	// shape types to count instead of keeping: the hair scenes hold millions of "curve" shapes the renderer has no
	// use for, and keeping their parameters would take gigabytes
	std::set<std::string, std::less<>> skipShapes;
};

// Reads `path` and every file it includes into `out`. False, with `error` as "file:line: message", for a file that
// cannot be read or a syntax error, where pbrt would exit
bool parsePbrtFile(const std::filesystem::path& path, PbrtScene& out, std::string* error, const PbrtParseOptions& options = {});
// the same for text in memory, as if read from a file called <string> in `directory`
bool parsePbrtString(std::string text, PbrtScene& out, std::string* error, const std::filesystem::path& directory = {},
	const PbrtParseOptions& options = {});

// THE SCANNER, exposed for the tests: the tokens of `text` in order, each as written (a quoted string keeps its
// quotes, a comment its '#'), or false with `error` at the first malformed token (a string that runs past the end of
// its line or of the file)
bool scanPbrtTokens(std::string text, std::vector<std::string>& tokens, std::string* error);
