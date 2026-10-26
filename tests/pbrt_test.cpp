// CPU verification of the pbrt-v4 scene import (src/pbrt_parser.h, src/pbrt_import.h). No GPU.
//
// What the import promises:
//  - the scanner splits text as pbrt's Tokenizer does: pbrt-v4's own tokenizer cases (parser_test.cpp)
//  - the parser keeps pbrt's graphics state: every transform directive, the attribute stack, named coordinate
//    systems, Import's scoping, object instancing and forward references to named materials, each transform to 1e-9
//  - a camera lands where pbrt puts it: a point pbrt sees at some place in its image is seen at the same place
//    here, with and without the "Scale -1 1 1" many pbrt scenes start with, and a fov on a portrait film is
//    horizontal
//  - shapes, materials and lights map as docs/plans/completed/pbrt-scene-import.md says: a disk keeps its area, a
//    sphere stays analytic, a blackbody light has luminance 1 times its scale, the uvs run down the image, and
//    what is substituted is in the report
//
// --scan DIR imports every scene file under DIR (a .pbrt with a WorldBegin) and prints what each became, and
// --report FILE prints one file's import report in full.
//
// Run:  ./bin/pbrt_test [--scan assets/models/pbrt-v4-scenes] [--report file.pbrt]

#include <pbrt_import.h>
#include <pbrt_parser.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <glm/gtc/matrix_transform.hpp>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
	if (!ok) {
		g_failures++;
		fmt::println("    FAIL: {}", what);
	}
}

bool nearMat(const glm::dmat4& a, const glm::dmat4& b, double tolerance = 1e-9)
{
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			if (std::abs(a[c][r] - b[c][r]) > tolerance) {
				return false;
			}
		}
	}
	return true;
}

bool near(double a, double b, double tolerance)
{
	return std::abs(a - b) <= tolerance * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

bool reportHas(const ImportedModel& model, const std::string& fragment)
{
	for (const ImportMessage& message : model.report.messages) {
		if (message.text.find(fragment) != std::string::npos) {
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------- the scanner

void expectTokens(const std::string& text, const std::vector<std::string>& expected)
{
	std::vector<std::string> tokens;
	std::string error;
	check(scanPbrtTokens(text, tokens, &error), "scans: " + error);
	check(tokens == expected, fmt::format("tokens of {:?}: got [{}]", text, fmt::join(tokens, "|")));
}

void expectScanError(const std::string& text, const std::string& message)
{
	std::vector<std::string> tokens;
	std::string error;
	check(!scanPbrtTokens(text, tokens, &error) && error == message, fmt::format("{:?} fails with \"{}\", got \"{}\"", text, message, error));
}

void testScanner()
{
	fmt::println("scanner");
	//pbrt-v4's TokenizerBasics
	expectTokens("Shape \"sphere\" \"float radius\" [1]", { "Shape", "\"sphere\"", "\"float radius\"", "[", "1", "]" });
	expectTokens("Shape \"sphere\"\n\"float radius\" [1]", { "Shape", "\"sphere\"", "\"float radius\"", "[", "1", "]" });
	expectTokens("\nShape\"sphere\" # foo bar [\n\"float radius\\\"\" 1", { "Shape", "\"sphere\"", "# foo bar [", "\"float radius\"\"", "1" });
	//TokenizerErrors
	expectScanError("Shape\"sphere\"\t\t # foo bar\n\"float radius", "premature EOF");
	expectScanError("Shape\"sphere\"\t\t # foo bar\n\"float radius\\", "premature EOF");
	expectScanError("Shape\"sphere\"\t\t # foo bar\n\"float radius\n\" 5", "unterminated string");
	//TokenizeFile
	expectTokens("\nWorldBegin # hello\nIntegrator \"deep\" \"float density\" [ 2 2.66612 -5e-51]\n",
		{ "WorldBegin", "# hello", "Integrator", "\"deep\"", "\"float density\"", "[", "2", "2.66612", "-5e-51", "]" });
}

//---------------------------------------------------------------- the parser

std::filesystem::path writeFile(const std::filesystem::path& path, const std::string& text)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path) << text;
	return path;
}

void testParser()
{
	fmt::println("parser");
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "pbrt_test";
	std::filesystem::remove_all(dir);
	//an Include resolves against the first file's directory, even from a subdirectory
	writeFile(dir / "geometry" / "included.pbrt", "Shape \"sphere\" \"float radius\" 2\n");
	writeFile(dir / "imported.pbrt", "Translate 100 0 0\nMakeNamedMaterial \"fromImport\" \"string type\" \"diffuse\"\nShape \"sphere\"\n");
	const std::string text = R"(
LookAt 1 2 3  4 5 6  0 1 0
Camera "perspective" "float fov" 45
Film "rgb" "integer xresolution" 64 "integer yresolution" 32
WorldBegin
AttributeBegin
  Translate 1 2 3
  Scale 2 2 2
  Rotate 90 0 0 1
  CoordinateSystem "mine"
  NamedMaterial "later"
  Shape "sphere"
AttributeEnd
Shape "sphere"
AttributeBegin
  Transform [1 0 0 0  0 1 0 0  0 0 1 0  5 6 7 1]
  ConcatTransform [2 0 0 0  0 2 0 0  0 0 2 0  0 0 0 1]
  ReverseOrientation
  Shape "sphere"
  CoordSysTransform "mine"
  Shape "sphere"
  CoordSysTransform "camera"
  Shape "sphere"
AttributeEnd
MakeNamedMaterial "later" "string type" "conductor"
Import "imported.pbrt"
Shape "sphere"
ObjectBegin "thing"
  Translate 0 1 0
  Shape "sphere"
ObjectEnd
AttributeBegin
  Translate 0 0 10
  ObjectInstance "thing"
AttributeEnd
ActiveTransform EndTime
Translate 0 0 1
ActiveTransform All
Shape "sphere"
Include "geometry/included.pbrt"
)";
	PbrtScene scene;
	std::string error;
	const bool ok = parsePbrtString(text, scene, &error, dir);
	check(ok, "the scene parses: " + error);
	if (!ok) {
		return;
	}
	const glm::dmat4 lookAt = glm::inverse(glm::dmat4(glm::dvec4(glm::normalize(glm::cross(glm::dvec3(0, 1, 0), glm::normalize(glm::dvec3(3, 3, 3)))), 0.0),
		glm::dvec4(glm::cross(glm::normalize(glm::dvec3(3, 3, 3)), glm::normalize(glm::cross(glm::dvec3(0, 1, 0), glm::normalize(glm::dvec3(3, 3, 3))))), 0.0),
		glm::dvec4(glm::normalize(glm::dvec3(3, 3, 3)), 0.0), glm::dvec4(1, 2, 3, 1)));
	check(scene.camera.has_value() && nearMat(scene.camera->cameraFromWorld, lookAt), "the camera's transform is pbrt's LookAt");
	check(scene.shapes.size() == 9, fmt::format("nine shapes outside the object, got {}", scene.shapes.size()));
	if (scene.shapes.size() != 9) {
		return;
	}
	const glm::dmat4 first = glm::rotate(glm::scale(glm::translate(glm::dmat4(1.0), { 1, 2, 3 }), glm::dvec3(2.0)), glm::radians(90.0), { 0, 0, 1 });
	check(nearMat(scene.shapes[0].worldFromObject, first), "Translate, Scale and Rotate compose as pbrt's post-multiplication");
	check(scene.shapes[0].material.name == "later", "a NamedMaterial may come before its MakeNamedMaterial");
	check(nearMat(scene.shapes[1].worldFromObject, glm::dmat4(1.0)), "AttributeEnd restores the transform");
	const glm::dmat4 fourth = glm::scale(glm::translate(glm::dmat4(1.0), { 5, 6, 7 }), glm::dvec3(2.0));
	check(nearMat(scene.shapes[2].worldFromObject, fourth), "Transform replaces and ConcatTransform multiplies");
	check(scene.shapes[2].reverseOrientation, "ReverseOrientation is kept with the shape");
	check(nearMat(scene.shapes[3].worldFromObject, first), "CoordSysTransform restores a CoordinateSystem");
	check(nearMat(scene.shapes[4].worldFromObject, glm::inverse(lookAt)), "the \"camera\" coordinate system is the camera's world transform");
	check(nearMat(scene.shapes[5].worldFromObject, glm::translate(glm::dmat4(1.0), { 100, 0, 0 })), "an imported file's shape has its transform");
	check(nearMat(scene.shapes[6].worldFromObject, glm::dmat4(1.0)), "an Import's transform does not reach the importing file");
	check(scene.namedMaterials.count("fromImport") == 1, "an Import's named material does reach the scene");
	check(scene.shapes[7].animated && nearMat(scene.shapes[7].worldFromObject, glm::dmat4(1.0)), "ActiveTransform EndTime moves only the shutter-close transform");
	check(scene.shapes[8].params.getFloat("radius", 1.0) == 2.0, "an Include in a subdirectory resolves against the first file's directory");
	check(scene.objects.count("thing") == 1 && scene.objects["thing"].shapes.size() == 1, "an object holds its shapes");
	check(scene.instances.size() == 1 && nearMat(scene.instances[0].worldFromInstance, glm::translate(glm::dmat4(1.0), { 0, 0, 10 })), "an instance keeps its transform");

	PbrtScene broken;
	check(!parsePbrtString("WorldBegin\nShape \"sphere\" \"float radius\" [ 1 \n", broken, &error) && error.find("premature end of file") != std::string::npos,
		"an unclosed bracket is an error: " + error);
	check(!parsePbrtString("WorldBegin\nFoo 1\n", broken, &error) && error.find("unknown directive: Foo") != std::string::npos, "an unknown directive is an error: " + error);
	check(!parsePbrtString("Shape \"sphere\"\n", broken, &error) && error.find("only allowed after WorldBegin") != std::string::npos, "a shape before WorldBegin is an error: " + error);
	check(parsePbrtString("WorldBegin\nShape \"sphere\" \"bool flag\" \"true\"\n", broken, &error) && broken.shapes[0].params.getBool("flag", false),
		"pbrt-v3's quoted bools read as bools");
	std::filesystem::remove_all(dir);
}

//---------------------------------------------------------------- the camera

// where pbrt's perspective camera puts a world point, as (x, y) on its screen window: x right, y up, the shorter
// axis from -1 to 1
glm::dvec2 pbrtScreen(const glm::dmat4& cameraFromWorld, const glm::dvec3& p, double fovDegrees, double aspect)
{
	const glm::dvec3 c(cameraFromWorld * glm::dvec4(p, 1.0));
	const double scale = 1.0 / std::tan(glm::radians(fovDegrees) / 2.0);
	glm::dvec2 s(c.x / c.z * scale, c.y / c.z * scale);
	return s;
}

// where the renderer's camera puts a point of its own world, in the same screen units
glm::dvec2 ourScreen(const SceneCamera& camera, const glm::dvec3& p, double aspect)
{
	const glm::dvec3 c(glm::inverse(glm::dmat4(camera.transform())) * glm::dvec4(p, 1.0));
	const double tanHalf = std::tan(glm::radians((double)camera.vfovDegrees) / 2.0);
	//the vertical axis spans -1 to 1 over tan(vfov / 2); pbrt's shorter axis does
	const double shorter = aspect >= 1.0 ? tanHalf : tanHalf * aspect;
	return glm::dvec2(c.x / -c.z / shorter, c.y / -c.z / shorter);
}

void testCamera(const std::string& preamble, const char* label, int width, int height)
{
	const std::string text = preamble + fmt::format(R"(LookAt 3 4 -10  0.5 1 0  0 1 0
Camera "perspective" "float fov" 40 "float lensradius" 0.1 "float focaldistance" 7
Film "rgb" "integer xresolution" {} "integer yresolution" {} "float iso" 200
WorldBegin
Shape "sphere"
)",
		width, height);
	PbrtScene scene;
	std::string error;
	check(parsePbrtString(text, scene, &error), "parses: " + error);
	ImportedModel model;
	importPbrtScene(scene, model);
	check(model.cameras.size() == 1, "one camera");
	if (model.cameras.empty() || !scene.camera.has_value()) {
		return;
	}
	const SceneCamera& camera = model.cameras[0];
	const double aspect = (double)width / height;
	//the world map the import chose, recovered from the sphere it placed at the pbrt origin + (1, 2, 3)
	const bool mirrors = preamble.empty();
	for (const glm::dvec3& p : { glm::dvec3(0.5, 1, 0), glm::dvec3(2, 0, 3), glm::dvec3(-1, 3, 1), glm::dvec3(0, -2, 5) }) {
		const glm::dvec3 ours = mirrors ? glm::dvec3(p.x, p.y, -p.z) : p;
		const glm::dvec2 a = pbrtScreen(scene.camera->cameraFromWorld, p, 40.0, aspect);
		const glm::dvec2 b = ourScreen(camera, ours, aspect);
		check(glm::length(a - b) < 1e-5, fmt::format("{}: ({}, {}, {}) is at ({:.6f}, {:.6f}) for pbrt and ({:.6f}, {:.6f}) here", label, p.x, p.y, p.z, a.x, a.y, b.x, b.y));
	}
	check(near(camera.aperture, 0.2, 1e-6) && near(camera.focusDistance, 7.0, 1e-6), fmt::format("{}: lens radius 0.1 is aperture 0.2, focus 7", label));
	check(near(camera.exposure, 2.0, 1e-6), fmt::format("{}: ISO 200 is exposure 2", label));
	check(model.settings.has_value() && model.settings->render.width == width && model.settings->render.height == height, fmt::format("{}: the film's resolution", label));
}

void testCameras()
{
	fmt::println("cameras");
	testCamera("", "LookAt, landscape", 64, 32);
	testCamera("", "LookAt, portrait", 32, 64);
	testCamera("Scale -1 1 1\n", "Scale -1 1 1 then LookAt, landscape", 64, 32);
	testCamera("Scale -1 1 1\n", "Scale -1 1 1 then LookAt, portrait", 30, 50);
}

//the scene stands on the axis nearest its camera's up, which becomes +y: a point light at pbrt's (1, 2, 3) lands where
//that rotation takes it, projects to the same pixel as in pbrt, and the camera's up is near +y
void testUpAxis()
{
	fmt::println("up axis");
	struct Case {
		const char* label;
		const char* camera;
		glm::vec3 light;
	};
	const Case cases[] = {
		{ "y up", "LookAt 0 2 -10  0 0 0  0 1 0", glm::vec3(1.f, 2.f, -3.f) },
		{ "z up, mirrored in z", "LookAt 0 -10 2  0 0 0  0 0 1", glm::vec3(1.f, 3.f, 2.f) },
		{ "z up after Scale -1 1 1", "Scale -1 1 1\nLookAt 0 -10 2  0 0 0  0 0 1", glm::vec3(1.f, 3.f, -2.f) },
		{ "x up", "LookAt 0 -10 -10  0 0 0  1 0 0", glm::vec3(-2.f, 1.f, -3.f) },
		{ "z up looking along -y", "LookAt 0 10 0  0 0 0  0 0 1", glm::vec3(1.f, 3.f, 2.f) },
		{ "a plan view down -y, up -z", "LookAt 0 10 0  0 0 0  0 0 -1", glm::vec3(1.f, 2.f, -3.f) },
	};
	for (const Case& c : cases) {
		const std::string text = fmt::format("{}\nCamera \"perspective\" \"float fov\" 60\nFilm \"rgb\" \"integer xresolution\" 64 \"integer yresolution\" 32\n"
											 "WorldBegin\nLightSource \"point\" \"point3 from\" [1 2 3]\n",
			c.camera);
		PbrtScene scene;
		std::string error;
		check(parsePbrtString(text, scene, &error), std::string(c.label) + " parses: " + error);
		ImportedModel model;
		importPbrtScene(scene, model);
		if (model.cameras.size() != 1 || model.lights.size() != 1 || !scene.camera.has_value()) {
			check(false, fmt::format("{}: one camera and one light", c.label));
			continue;
		}
		const glm::vec3 at = model.lights[0].position;
		check(glm::length(at - c.light) < 1e-6f, fmt::format("{}: pbrt's (1, 2, 3) is ({}, {}, {}), expected ({}, {}, {})", c.label, at.x, at.y, at.z, c.light.x, c.light.y, c.light.z));
		const glm::dvec2 a = pbrtScreen(scene.camera->cameraFromWorld, glm::dvec3(1, 2, 3), 60.0, 2.0);
		const glm::dvec2 b = ourScreen(model.cameras[0], glm::dvec3(at), 2.0);
		check(glm::length(a - b) < 1e-5, fmt::format("{}: the light is at ({:.6f}, {:.6f}) for pbrt and ({:.6f}, {:.6f}) here", c.label, a.x, a.y, b.x, b.y));
		const glm::vec3 up = model.cameras[0].orientation * glm::vec3(0.f, 1.f, 0.f);
		const glm::vec3 view = model.cameras[0].orientation * glm::vec3(0.f, 0.f, -1.f);
		check(up.y > 0.9f || view.y < -0.9f, fmt::format("{}: the camera's up is ({:.3f}, {:.3f}, {:.3f})", c.label, up.x, up.y, up.z));
	}
}

//---------------------------------------------------------------- what maps where

void testMapping()
{
	fmt::println("mapping");
	const std::string text = R"(
Film "rgb" "integer xresolution" 16 "integer yresolution" 16 "float maxcomponentvalue" 10
Sampler "halton" "integer pixelsamples" 64
PixelFilter "mitchell"
Integrator "volpath" "integer maxdepth" 7 "bool regularize" true
WorldBegin
LightSource "infinite" "rgb L" [0.5 0.5 0.5] "float scale" 2
LightSource "point" "blackbody I" [3000] "float scale" 5 "point3 from" [1 2 3]
LightSource "distant" "point3 from" [0 1 0] "point3 to" [0 0 0] "rgb L" [3 3 3]
AttributeBegin
  AreaLightSource "diffuse" "blackbody L" [6500] "float scale" 4
  Material "diffuse" "rgb reflectance" [0 0 0]
  Translate 0 5 0
  Shape "disk" "float radius" 2
AttributeEnd
AttributeBegin
  Material "coateddiffuse" "rgb reflectance" [0.2 0.4 0.6] "float roughness" 0.04
  Translate 1 2 3
  Scale 0.5 0.5 0.5
  Shape "sphere" "float radius" 3
AttributeEnd
Material "dielectric" "spectrum eta" "glass-BK7"
Shape "trianglemesh" "point3 P" [0 0 0  1 0 0  1 1 0] "point2 uv" [0 0  1 0  1 1] "integer indices" [0 1 2]
Material "conductor" "spectrum eta" "metal-Au-eta" "spectrum k" "metal-Au-k" "float roughness" 0.01 "float mystery" 1
Shape "curve" "point3 P" [0 0 0 1 0 0 2 0 0 3 0 0]
Shape "trianglemesh" "point3 P" [0 0 0  1 0 0  1 1 0]
)";
	PbrtScene scene;
	std::string error;
	PbrtParseOptions options;
	options.skipShapes.insert("curve");
	check(parsePbrtString(text, scene, &error, {}, options), "parses: " + error);
	ImportedModel model;
	importPbrtScene(scene, model);

	check(model.settings.has_value(), "settings");
	if (model.settings.has_value()) {
		const RenderSettings& r = model.settings->render;
		check(r.maxSamples == 64 && model.settings->sampler == "halton", "Sampler halton with 64 samples");
		check(r.samplerRandomization == SamplerRandomization::PermuteDigits, "Halton's default randomization is pbrt's permuted digits");
		check(r.rayDepth == 8, fmt::format("pbrt's maxdepth 7 is a ray depth of 8, got {}", r.rayDepth));
		check(r.clampSamples && r.clampValue == 10.f, "maxcomponentvalue is the sample clamp");
		check(r.pixelFilter.kind == PixelFilterKind::Mitchell && r.pixelFilter.radius == 2.f, "the Mitchell filter at pbrt's radius 2");
	}
	check(reportHas(model, "path regularization"), "regularize is reported");

	check(model.environment.has_value() && model.environment->intensity == 2.f && model.environment->rgba[0] == 0.5f, "a uniform infinite light is a constant environment at its scale");
	check(model.lights.size() == 2, fmt::format("a point and a distant light, got {}", model.lights.size()));
	if (model.lights.size() == 2) {
		const SceneLight& point = model.lights[0];
		//the blackbody is kept as one, which its role normalizes to luminance 1 (docs/plans/completed/pbrt-spectral-rendering.md)
		check(point.color.kind == SpectrumKind::Blackbody, "a blackbody point light keeps its spectrum");
		const glm::dvec3 rgb = glm::dvec3(exactRgb(point.color, SpectrumRole::Illuminant, {})) * (double)point.intensity;
		const double luminance = 0.2126 * rgb.x + 0.7152 * rgb.y + 0.0722 * rgb.z;
		check(near(luminance, 5.0, 2e-3), fmt::format("a blackbody point light has luminance 1 times its scale 5, got {}", luminance));
		check(point.position == glm::vec3(1.f, 2.f, -3.f), "a point light at pbrt's (1, 2, 3) is at (1, 2, -3), mirrored in z");
		const SceneLight& sun = model.lights[1];
		check(glm::length(sun.direction() - glm::vec3(0.f, -1.f, 0.f)) < 1e-5f && sun.intensity == 3.f, "a distant light from +y travels down with its L");
	}

	//the disk is a mesh whose area is the disk's
	const ImportedMesh* disk = nullptr;
	for (const ImportedMesh& mesh : model.meshes) {
		disk = mesh.name.rfind("disk", 0) == 0 ? &mesh : disk;
	}
	check(disk != nullptr, "the disk is a mesh");
	if (disk != nullptr) {
		double area = 0.0;
		for (size_t i = 0; i < disk->indices.size(); i += 3) {
			const glm::dvec3 a(disk->positions[disk->indices[i]]);
			const glm::dvec3 b(disk->positions[disk->indices[i + 1]]);
			const glm::dvec3 c(disk->positions[disk->indices[i + 2]]);
			area += 0.5 * glm::length(glm::cross(b - a, c - a));
		}
		check(near(area, std::numbers::pi * 4.0, 1e-5), fmt::format("the disk's triangles cover pi r^2 = {}, got {}", std::numbers::pi * 4.0, area));
		const ImportedMaterial& emitter = model.materials[disk->surfaces[0].material];
		const glm::dvec3 L = glm::dvec3(materialColor(emitter.definition, "albedo")) * (double)materialFloat(emitter.definition, "strength");
		check(emitter.definition.type == MaterialType::Emissive && near(0.2126 * L.x + 0.7152 * L.y + 0.0722 * L.z, 4.0, 2e-3), "the disk emits luminance 4");
	}
	check(reportHas(model, "one-sided area light emits from both sides"), "a one-sided area light is reported");

	check(model.shapes.size() == 1, "the sphere stays analytic");
	if (!model.shapes.empty()) {
		const ImportedShape& sphere = model.shapes[0];
		check(sphere.shape.kind == ShapeKind::Sphere && sphere.shape.position == glm::vec3(1.f, 2.f, -3.f) && sphere.shape.size == glm::vec3(1.5f), "the sphere at (1, 2, -3) with radius 1.5");
		const MaterialDef& material = model.materials[sphere.material].definition;
		check(material.type == MaterialType::Pbr && materialFloat(material, "metallic") == 0.f, "coateddiffuse is pbr with metallic 0");
		check(near(materialFloat(material, "roughness"), std::sqrt(std::sqrt(0.04)), 1e-6), "roughness 0.04 remapped to alpha 0.2, perceptual sqrt(0.2)");
	}

	const ImportedMesh* glass = nullptr;
	for (const ImportedMesh& mesh : model.meshes) {
		if (!mesh.surfaces.empty() && model.materials[mesh.surfaces[0].material].definition.type == MaterialType::Glass) {
			glass = &mesh;
		}
	}
	check(glass != nullptr, "the dielectric triangle is glass");
	if (glass != nullptr) {
		const SpectrumRef eta = materialSpectrum(model.materials[glass->surfaces[0].material].definition, "eta");
		check(eta.kind == SpectrumKind::Named && eta.name == "glass-BK7", fmt::format("glass-BK7 keeps its dispersion, got {}", spectrumKindName(eta.kind)));
		const double nd = scalarAt(eta, 587.6);
		check(nd > 1.515 && nd < 1.52, fmt::format("glass-BK7 at the design wavelength is about 1.5168, got {}", nd));
		check(glass->uvs.size() == 3 && glass->uvs[0] == glm::vec2(0.f, 1.f) && glass->uvs[2] == glm::vec2(1.f, 0.f), "pbrt's uv v runs up the image; the renderer's runs down");
	}
	bool gold = false;
	for (const ImportedMaterial& material : model.materials) {
		if (material.definition.type == MaterialType::Conductor) {
			const SpectrumRef eta = materialSpectrum(material.definition, "eta");
			const glm::vec3 c = materialPreviewColor(material.definition);
			gold = eta.name == "metal-Au-eta" && c.r > 0.9f && c.g > 0.7f && c.g < 0.85f && c.b < 0.4f;
		}
	}
	check(gold, "a gold conductor is a conductor with gold's eta and k, and gold's color");
	check(reportHas(model, "parameter \"float mystery\" is ignored"), "an unknown parameter is reported");
	check(reportHas(model, "Shape \"curve\" is not supported; 1 skipped"), "a skipped shape type is counted");
}

//---------------------------------------------------------------- scenes on disk

bool hasWorldBegin(const std::filesystem::path& path)
{
	std::ifstream file(path);
	std::string line;
	while (std::getline(file, line)) {
		if (line.find("WorldBegin") != std::string::npos) {
			return true;
		}
	}
	return false;
}

int scan(const std::filesystem::path& dir)
{
	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
		if (entry.is_regular_file() && entry.path().extension() == ".pbrt" && hasWorldBegin(entry.path())) {
			files.push_back(entry.path());
		}
	}
	std::sort(files.begin(), files.end());
	int failed = 0;
	for (const std::filesystem::path& path : files) {
		const auto start = std::chrono::steady_clock::now();
		ImportedModel model;
		std::string error;
		const bool ok = importPbrtFile(path, model, &error);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		size_t triangles = 0;
		for (const ImportedMesh& mesh : model.meshes) {
			triangles += mesh.indices.size() / 3;
		}
		const std::string name = std::filesystem::relative(path, dir).string();
		if (!ok) {
			failed++;
			fmt::println("FAILED {}: {}", name, error);
			continue;
		}
		fmt::println("{}: {:.1f} s, {} meshes ({} triangles), {} nodes, {} shapes, {} materials, {} images, {} lights{}, {} report lines", name, seconds,
			model.meshes.size(), triangles, model.nodes.size(), model.shapes.size(), model.materials.size(), model.images.size(), model.lights.size(),
			model.environment.has_value() ? " + environment" : "", model.report.messages.size());
	}
	fmt::println("pbrt_test --scan: {} of {} scene files imported", files.size() - failed, files.size());
	return failed == 0 ? 0 : 1;
}

int printReport(const std::filesystem::path& path)
{
	ImportedModel model;
	std::string error;
	if (!importPbrtFile(path, model, &error)) {
		fmt::println("FAILED: {}", error);
		return 1;
	}
	for (const ImportMessage& message : model.report.messages) {
		fmt::println("{}{} ({}{}){}", message.text, message.count > 1 ? fmt::format(" x{}", message.count) : "", message.where,
			message.count > 1 ? ", first" : "", message.plan.empty() ? "" : " -> docs/plans/" + message.plan);
	}
	return 0;
}

}

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--scan") == 0 && i + 1 < argc) {
			return scan(argv[i + 1]);
		}
		if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
			return printReport(argv[i + 1]);
		}
		fmt::println("usage: pbrt_test [--scan dir] [--report file.pbrt]");
		return 2;
	}
	testScanner();
	testParser();
	testCameras();
	testUpAxis();
	testMapping();
	if (g_failures > 0) {
		fmt::println("pbrt_test: {} check(s) FAILED", g_failures);
		return 1;
	}
	fmt::println("pbrt_test: all checks passed");
	return 0;
}
