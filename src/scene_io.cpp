#include <scene_io.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <simdjson.h>


namespace {

//bump when the payload's shape changes, and branch on it in parseSceneExtras() for older files.
//1: spheres only. 2: adds "models" and "environmentMap". 3: adds "render" and "environmentIntensity".
//4: adds "cameras" and "renderCamera"; the lens and exposure move from "render" to each camera
//5: adds "meshObjects", the placed glTF nodes. A version-4 file has none, and loading it places
//   every node of every model at its authored transform - exactly what importing does
//6: "spheres" becomes "geometry", every analytic shape in one list, each record naming its "type"
//   and carrying a "position", an "orientation" when the kind has one, and its sizes by name. An
//   older file's "spheres" still reads, as spheres
//7: adds the "emissive" material ("color", "strength") and "background", a solid colour for missed
//   rays; absent means the environment map or the sky, as before
//8: adds "kernels", the wavefront kernel variant selected in each slot as {"NN":"<variant id>"},
//   and "accel", how the BVH those kernels read is built. Both absent in an older file, which
//   then opens with the default selection and the default builder: BVH traversal as before, and
//   MIS (power) direct lighting where the tracer of the time had only BSDF sampling. A following
//   slot's entry (kernel 08's) is written but never read
//9: adds the "pbr" material ("albedo", "metallic", "roughness", "emission", "strength"), glTF's
//   metallic-roughness model. An older file has none, so it reads unchanged
//10: adds "lights", the punctual lights: each a "type" (point, spot, directional), "name",
//   "position", "color", "intensity" in the tracer's radiometric units, and by type an
//   "orientation", a "range", an "innerCone" and "outerCone" in degrees, and the "model" it was
//   imported with. Absent in an older file, whose models' own lights their imports recreate
//11: adds "integrator", the light transport algorithm's key, and "traversal", the id of the
//   traversal every kernel that traces is built for. Kernels 02 and 08 become one variant each,
//   "closest" and "occlusion"; an older file's kernel 02 entry names the traversal instead, and its
//   kernel 08 entry is not read
//12: adds the material library. "materials" holds the user materials in full: a "name", a "type",
//   and the type's parameters under their MATERIAL_TRAITS names. "materialOverrides" holds the
//   edits to glTF materials, each a "model" index, a "material" index in that file and the changed
//   parameters. A shape's and an override mesh object's "material" is a reference: an index into
//   "materials", or {"model":m,"material":i} for a glTF material. An older file's inline material
//   becomes one user material per object, named after it; a mesh object in glTF mode drops the
//   override material older files wrote for it anyway
//13: adds "errorBoundedOffsets" and "watertightTriangles" to "render", the intersection precision
//   settings. Absent in an older file, which reads with both on: its render differs from the one it
//   was saved with by what the old 0.001 ray epsilon and Moller-Trumbore got wrong, and by nothing else
//14: adds "samplerRandomization" to "render" (none, permute_digits, fast_owen, owen), and kernel 01's
//   samplers. A file without a "01" entry in "kernels" (one older than version 8, or written by hand)
//   reads with "pcg_hash", the only variant there was, rather than a new scene's ZSobol, so it renders
//   as it always did
//15: adds the film to "render": "pixelFilter" (a "kind" box, triangle, gaussian, mitchell or lanczos, its
//   "radius", and the "sigma", "b" and "c" or "tau" its kind uses), "clampSamples", "clampValue" and
//   "outlierKappa", and kernel 00's "filter_importance" and kernel 09's "compensated_mean" and
//   "outlier_reweighted". A file without a "00" or "09"
//   entry in "kernels" reads with "thin_lens" and "running_mean", the only variants there were, and one
//   without "pixelFilter" with a box, so it renders as it always did rather than as a new scene does
//16: adds "aov" and "lightGroups" to "render", "lightGroupNames", "environmentGroup" and "export" at the top,
//   "lightGroup" on a light, a material and a material override, and "relight" on a camera
//17: a model may be a pbrt-v4 scene file (.pbrt or .pbrt.gz), re-imported on load like a glTF file. A shape
//   the model brought carries its "model" index, and "environmentModel" names the model whose environment
//   the scene shows. Both absent in an older file, whose shapes are its own
//18: adds "spectral" to "render" (docs/plans/completed/pbrt-spectral-rendering.md): the "representation" (rgb,
//   monochromatic, gaussians, bands, hero), "lanes", "range", "wavelength", "designWavelength", "bandBasis",
//   "gaussians", "sampling", "emittersShare", "colorSpace" and "matching"; "spectralDisplay" at the top; and a
//   spectrum's object form wherever a color was an [r, g, b] array (spectrumJson()). An older file reads in exact
//   RGB with sRGB colors, as it always rendered
//19: the material library gains pbrt-v4's `diffuse`, `thin_glass`, `diffuse_transmission` and `mix` types (docs/plans/completed/pbrt-bsdf-library.md).
//   A mix's "materialA" and "materialB" are material references, written as a shape's "material" is; its "amount" is
//   a number. The pbr type gains "f0". An older file has none of them, and every type it names reads as it did
//20: adds "capabilities", the keys of the scene capabilities it has on (docs/plans/completed/physoptics-scene-capabilities.md),
//   "template", the template it started from, and in a template file "templateInfo" ("name", "description"). Each is
//   written only when it has something to say, so a scene without them saves as before apart from the version. An
//   older file has no capabilities and no template
//21: adds "optics", the optical elements (docs/plans/completed/physoptics-optical-elements.md): each a "type", "name", "size" (a
//   preset's key or "custom"), the "params" that differ from the kind's defaults by name in SI units (an enum by its
//   key), a double-precision "position" and "orientation", and "materials", the slots whose material differs from the
//   default, by slot name, written as a user material is. "opticalLibrary" holds the loaded AGF "catalogs" by path and
//   the user's "glasses" and "coatings" in full. "render" gains "optics" ("surrounding", "exactCoatings"), and the top
//   level "lengthUnit" ("mm"; meters when absent). A material's "coating" is a coating key. Each is written only when
//   it has something to say, so a scene without optics saves as before apart from the version
//22: adds "viewport", the raster viewport's free camera: its "position", "orientation" and "pivot", and its "speed" in
//   meters per second, vertical "fov" in degrees, and "near" and "far" clip planes in meters. Absent in an older file,
//   which opens with a new scene's speed, fov and clip planes, and leaves the view where it was
//23: adds "bench" (docs/plans/completed/physoptics-bench-and-mounts.md): whether the breadboard exists ("table"), its "size" in x
//   and z, the "beamHeight" and "snap", written when any differs from a new scene's. An optic's "position" and
//   "orientation" move into its "mount": the mount's "kind" (a mountKindKey()), and either the stack, its "origin" (x
//   and z of the optic's origin, opticOrigin()), "yaw", "height", "fork" ("angle", "reach") and the "fine" adjustment
//   that is not zero ("x" and "y" adjuster turns, a "rotor" angle); an unmounted or "free" optic has its "position" and
//   "orientation", and a free one its stack besides, where it stood. An optic in an older file is unmounted where the file put it, so it renders as it did
//24: adds "environmentRotation", the map's turn about the up axis in degrees (absent for 0), and "backgroundMode", what a missed ray sees and the raster preview shows: "gradient" (the editor's, black in a
//   render), "solid", "sky" (Ray Tracing in One Weekend's) or "environment". "background" is always written, the color
//   "solid" shows. An older file's choice was implied: a "background" was the solid color whatever else the file had,
//   otherwise a map was the environment, otherwise the sky, which it reads as, so it renders as it did
//25: adds the texture library and the new light types (docs/plans/completed/pbrt-textures.md, pbrt-light-types.md). "textures" lists
//   the image files materials read, each a "path", "colorSpace" ("srgb", "linear"), "wrap" ([u, v], each "repeat",
//   "clamp", "mirror" or "black"), "nearest" ([mag, min]), and a pbrt imagemap's "encoding", "gamma" and "invert". A
//   material (and a glTF material's override) holds "textures", by parameter name (and "normal", "alpha"): a "texture"
//   index, the "channel", "mapping", "uvSet", "offset", "scale", "rotation", the mapping's "transform" (16 numbers,
//   column-major) and an alpha texture's "alphaMode". Every type takes "emission", "strength" and "twoSided". A shape
//   has "reverseOrientation"; "render" adds "textureStorage" ("legacy", "native"), "maxTextureSize" and
//   "textureFilter". A light adds the types "projection" and "goniometric" with an "image" and a projection's "fov",
//   a spot's "falloff" ("smoothstep"), and the "unit" the panel shows. An older file has no texture storage, and reads
//   as the legacy array, so it renders as it did
//26: a camera gains its projection (docs/plans/completed/pbrt-cameras.md): its "kind" ("perspective", "orthographic",
//   "spherical"), an orthographic camera's "viewHeight", a spherical one's "mapping" ("equirect", "equalarea"), the
//   "lensMapping" ("polar", "concentric"), the "aperture" shape's "apertureShape" ("circle", "polygon", "image"),
//   "apertureBlades", "apertureRotation" and "apertureImage", the plane of focus's "tilt" and the film's "shift", each
//   [x, y]. An older camera is a perspective one with a circular aperture and the polar lens mapping, which renders as
//   it did
//27: the integrators "light", "lvc" and "bdpt" (docs/plans/completed/pbrt-bidirectional-path-tracing.md), whose stages 11 to 18
//   appear in "kernels", and "render" gains "bidirectional" when it differs from the defaults: the light vertex cache's
//   "connections" and "cacheBudget" (MB), and bdpt's "visualizeStrategies" and "visualizeWeights". An older file renders
//   with the path tracer it names, as it did
//28: the optic type "laser" (docs/plans/completed/physoptics-laser-sources.md), whose "params" are its "housing",
//   "wavelength" and "linewidth" (meters), "spectrum", "power" (watts), "waistRadius", "waistPosition", "m2", "astigmatic",
//   the y axis's "waistRadiusY", "waistPositionY", "m2Y" and "ellipseAngle", and the "pointingX", "pointingY", "offsetX"
//   and "offsetY", its "size" a preset's key ("HNL050LB"). Its slot "aperture" holds a "laser_aperture" material. A
//   laser is placed freely, as an unmounted optic is. "spectral" gains "designFollowsLaser", written only when false.
//   A user glass gains "absolute", written when its index is not relative to air (a catalog formula is). An older
//   file has no laser, and its glasses are read relative to air, as catalog glasses now are
//29: the detectors (docs/plans/completed/physoptics-detectors.md), each an optic placed freely as a laser is: the types
//   "power_meter", "beam_dump" (a "diameter"), "spectrometer" (a "diameter", "lambdaMin" and "lambdaMax" in meters and
//   "bins"), each one pixel, "camera_sensor", "beam_profiler", "alignment_card", "screen" and
//   "probe_plane" ("width", "height", "pixelsX", "pixelsY"), a camera sensor and a profiler also "physicalSensor" with
//   its "nativeX", "nativeY" and "pitch". Their "size" is "custom": no kind has presets. The sensor's slot is "sensor", a
//   "detector" material, or a card's and a screen's "face", a "diffuse" one; a probe plane has none. Films are not saved.
//   "renderDetector", the optic index of the render target when it is a detector rather than "renderCamera", and
//   "render" gains "lightPathsPerSample" when it differs from 512 x 512. An older file has no detector
//30: adds "sensorStacks" (src/optic_detectors.h): each a "name", its "sensor" (a detector's "type", "params" and
//   "materials", as an optic has them), the "start" and "end" of its axis and its "count" of sensors, the "orientation"
//   every sensor has and "alignToAxis". Its members are not in "optics": "renderDetector" counts them after the optics,
//   each stack's in order. A mounted optic's "mount" may give the "position" and "orientation" of its body, with no
//   "origin": the optic then stands in its mount on the bench where they put it, which a script writing a scene can do
//   without the mounts' geometry. "render" gains "renderAllSensors", written when true: light tracing records every
//   detector, where by default it records the render target and its stack's members. An older file has none of them,
//   and records its target alone
//31: a spherical lens is placed by its origin, wherever it stands: a free or unmounted lens's "position" is its origin's,
//   and not its body frame's, and its "params" always carry "originAt" ("front_vertex", "first_principal_plane",
//   "optical_center", "second_principal_plane" or "back_vertex"), which says where along its axis that origin is. A lens
//   without "originAt" is from an older file: its position was the body frame's, and it opens with its origin, which was
//   the middle between its vertices, kept where it was and now its optical center. Every other kind is as before
constexpr int64_t SCENE_FILE_FORMAT_VERSION = 31;

//---------------------------------------------------------------- writing

//always a valid json number that reads back as a double: no nan/inf, and always a fraction or
//exponent so simdjson does not hand back an integer. Formatted as the float it is, so the text is
//the shortest that round-trips to the same float (0.7f prints as 0.7, not 0.699999988)
std::string jsonNumber(float value)
{
	if (!std::isfinite(value)) {
		value = 0.f;
	}
	std::string text = fmt::format("{}", value);
	if (text.find_first_of(".eE") == std::string::npos) {
		text += ".0";
	}
	return text;
}

std::string jsonString(std::string_view text)
{
	std::string out = "\"";
	for (const char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if ((unsigned char)c < 0x20) {
				out += fmt::format("\\u{:04x}", (unsigned int)(unsigned char)c);
			} else {
				out += c;
			}
		}
	}
	out += '"';
	return out;
}

std::string jsonVec3(const glm::vec3& v)
{
	return fmt::format("[{},{},{}]", jsonNumber(v.x), jsonNumber(v.y), jsonNumber(v.z));
}

//x, y, z, w - named explicitly rather than trusting glm's memory order
// A SpectrumRef: an RGB value is the [r, g, b] array every older file has, so a scene that uses no spectrum reads and
// writes exactly as before; any other kind is an object that names it (docs/plans/completed/pbrt-spectral-rendering.md):
//   {"constant": 1.5}   {"blackbody": 2700, "scale": 1}   {"named": "metal-Au-eta", "scale": 1}
//   {"tabulated": [l0, v0, l1, v1, ...], "scale": 1}   {"spd": "path/file.spd", "scale": 1}
//   {"dispersion": 2, "coefficients": [...], "name": "N-BK7", "range": [300, 2500]}
//   {"line": 632.8, "fwhm": 0.002, "profile": "gaussian", "scale": 1}
std::string spectrumJson(const SpectrumRef& ref)
{
	switch (ref.kind) {
	case SpectrumKind::Rgb:
		return jsonVec3(ref.rgb);
	case SpectrumKind::Constant:
		return fmt::format(R"({{"constant":{}}})", jsonNumber(ref.value));
	case SpectrumKind::Blackbody:
		return fmt::format(R"({{"blackbody":{},"scale":{}}})", jsonNumber(ref.value), jsonNumber(ref.scale));
	case SpectrumKind::Named:
		return fmt::format(R"({{"named":{},"scale":{}}})", jsonString(ref.name), jsonNumber(ref.scale));
	case SpectrumKind::Tabulated: {
		std::string samples;
		for (size_t i = 0; i < ref.samples.size(); i++) {
			samples += (i == 0 ? "" : ",") + jsonNumber(ref.samples[i]);
		}
		return fmt::format(R"({{"tabulated":[{}],"scale":{}}})", samples, jsonNumber(ref.scale));
	}
	case SpectrumKind::File:
		return fmt::format(R"({{"spd":{},"scale":{}}})", jsonString(ref.name), jsonNumber(ref.scale));
	case SpectrumKind::Dispersion: {
		std::string coefficients;
		for (int i = 0; i < dispersionCoefficientCount(ref.formula); i++) {
			coefficients += fmt::format("{}{:.17g}", i == 0 ? "" : ",", ref.coefficients[(size_t)i]);
		}
		return fmt::format(R"({{"dispersion":{},"coefficients":[{}],"name":{},"range":[{},{}]}})", (int)ref.formula, coefficients, jsonString(ref.name),
			jsonNumber(ref.validRange.x), jsonNumber(ref.validRange.y));
	}
	case SpectrumKind::Line:
		return fmt::format(R"({{"line":{},"fwhm":{},"profile":"{}","scale":{}}})", jsonNumber(ref.value), jsonNumber(ref.width),
			ref.profile == LineProfile::Lorentzian ? "lorentzian" : "gaussian", jsonNumber(ref.scale));
	}
	return jsonVec3(ref.rgb);
}

std::string jsonQuat(const glm::quat& q)
{
	return fmt::format("[{},{},{},{}]", jsonNumber(q.x), jsonNumber(q.y), jsonNumber(q.z), jsonNumber(q.w));
}

std::string materialRefJson(const SceneMaterialRef& ref);

const char* const TEXTURE_CHANNEL_NAMES[] = { "rgb", "r", "g", "b", "a", "average" };
const char* const TEXTURE_MAPPING_NAMES[] = { "uv", "spherical", "cylindrical", "planar" };
const char* const TEXTURE_WRAP_NAMES[] = { "repeat", "clamp", "mirror", "black" };
const char* const TEXTURE_FILTER_NAMES[] = { "point", "bilinear", "trilinear", "anisotropic" };

//version 25: the textures the user gave a material's parameters, by name, each naming its image by index into the
//file's "textures" (TextureBinding::fileTexture, set by VulkanEngine::saveScene()). A model's own are never written
std::string materialTexturesJson(const std::map<std::string, TextureBinding, std::less<>>& textures)
{
	std::string json;
	for (const auto& [name, binding] : textures) {
		if (binding.derived || binding.fileTexture < 0) {
			continue;
		}
		std::string transform;
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				transform += fmt::format("{}{}", transform.empty() ? "" : ",", jsonNumber(binding.mappingTransform[c][r]));
			}
		}
		json += fmt::format(R"({}{}:{{"texture":{},"channel":"{}","mapping":"{}","uvSet":{},"offset":[{},{}],"scale":[{},{}],"rotation":{},"transform":[{}],"worldSpace":{},"alphaMode":"{}"}})",
			json.empty() ? "" : ",", jsonString(name), binding.fileTexture, TEXTURE_CHANNEL_NAMES[(size_t)binding.channel], TEXTURE_MAPPING_NAMES[(size_t)binding.mapping], binding.uvSet,
			jsonNumber(binding.offset.x), jsonNumber(binding.offset.y), jsonNumber(binding.scale.x), jsonNumber(binding.scale.y), jsonNumber(binding.rotation), transform,
			binding.worldSpace, binding.alphaMode == AlphaMode::Stochastic ? "stochastic" : "cutoff");
	}
	return json.empty() ? std::string() : fmt::format(R"(,"textures":{{{}}})", json);
}

//a material's parameters as json fields, by the names MATERIAL_TRAITS gives them: those of its
//type that are in `names` (every one, for a user material), never a read-only one, which is derived
//rather than authored
std::string materialParamsJson(const MaterialDef& material, const std::set<std::string, std::less<>>* names)
{
	std::string json;
	for (const MaterialParam& param : materialTraits(material.type).params) {
		if ((param.flags & PARAM_READ_ONLY) != 0 || (names != nullptr && names->count(param.name) == 0)) {
			continue;
		}
		const ParamValue value = materialValue(material, param.name);
		if (const float* f = std::get_if<float>(&value)) {
			json += fmt::format(R"(,"{}":{})", param.name, jsonNumber(*f));
		} else if (const SpectrumRef* spectrum = std::get_if<SpectrumRef>(&value)) {
			json += fmt::format(R"(,"{}":{})", param.name, spectrumJson(*spectrum));
		} else if (const int* i = std::get_if<int>(&value)) {
			json += fmt::format(R"(,"{}":{})", param.name, *i);
		} else if (const bool* b = std::get_if<bool>(&value)) {
			json += fmt::format(R"(,"{}":{})", param.name, *b);
		} else if (const CoatingRef* coating = std::get_if<CoatingRef>(&value)) {
			json += fmt::format(R"(,"{}":{})", param.name, jsonString(coating->key));
		} else if (const MaterialLink* link = std::get_if<MaterialLink>(&value)) {
			//version 19: written as a shape's material reference is, once saveScene() has turned the id into a file reference
			if (link->fileModel >= 0 || link->fileUser >= 0) {
				json += fmt::format(R"(,"{}":{})", param.name, materialRefJson(SceneMaterialRef { link->fileUser, link->fileModel, link->fileModelMaterial }));
			}
		}
	}
	return json + materialTexturesJson(material.textures);
}

std::string materialRefJson(const SceneMaterialRef& ref)
{
	if (ref.model >= 0) {
		return fmt::format(R"({{"model":{},"material":{}}})", ref.model, ref.modelMaterial);
	}
	return fmt::format("{}", ref.user);
}

//relative to the scene file when the target sits inside the scene's folder, its parent or its
//grandparent (the project's own assets/scenes -> assets layout, and a scene in a folder of
//assets/scenes such as optics/), so an assets folder moves as a unit; absolute otherwise, since
//on posix everything shares a root and a "relative" path to another volume would be a long,
//fragile ../../.. chain. Forward slashes either way
std::string portablePath(const std::filesystem::path& absolutePath, const std::filesystem::path& sceneDirectory)
{
	std::filesystem::path relative = absolutePath.lexically_relative(sceneDirectory);
	int climbs = 0;
	for (const std::filesystem::path& part : relative) {
		if (part != "..") {
			break;
		}
		climbs++;
	}
	if (relative.empty() || climbs > 2) {
		relative = absolutePath;
	}
	return relative.generic_string();
}

std::string jsonDouble(double value)
{
	return std::isfinite(value) ? fmt::format("{:.17g}", value) : std::string("0.0");
}

std::string jsonDoubles(const std::vector<double>& values)
{
	std::string out = "[";
	for (size_t i = 0; i < values.size(); i++) {
		out += (i > 0 ? "," : "") + jsonDouble(values[i]);
	}
	return out + "]";
}

std::string glassJson(const OpticGlass& glass)
{
	std::string lines;
	for (size_t i = 0; i < glass.transmittance.size(); i++) {
		const glm::dvec3& line = glass.transmittance[i];
		lines += fmt::format("{}[{},{},{}]", i > 0 ? "," : "", jsonDouble(line.x), jsonDouble(line.y), jsonDouble(line.z));
	}
	//version 28: "absolute" for a glass whose index is not relative to air, as a catalog's is
	return fmt::format(R"({{"name":{},"index":{},"range":[{},{}],"nd":{},"vd":{},"transmittance":[{}],"thermal":{}{}}})", jsonString(glass.name), spectrumJson(glass.index),
		jsonDouble(glass.range.x), jsonDouble(glass.range.y), jsonDouble(glass.catalogNd), jsonDouble(glass.catalogVd), lines, jsonString(glass.thermal),
		glass.relativeToAir ? "" : R"(,"absolute":true)");
}

std::string coatingJson(const OpticCoating& c)
{
	std::string layers;
	for (size_t i = 0; i < c.layers.size(); i++) {
		layers += fmt::format(R"({}{{"material":{},"thickness":{},"quarterWaves":{}}})", i > 0 ? "," : "", jsonString(c.layers[i].material), jsonDouble(c.layers[i].thickness),
			jsonDouble(c.layers[i].quarterWaves));
	}
	std::string curves;
	for (size_t i = 0; i < c.curves.size(); i++) {
		const CoatingCurve& curve = c.curves[i];
		curves += fmt::format(R"({}{{"angle":{},"lambda":{},"rs":{},"rp":{},"ts":{},"tp":{}}})", i > 0 ? "," : "", jsonDouble(curve.angle), jsonDoubles(curve.lambda), jsonDoubles(curve.rs),
			jsonDoubles(curve.rp), jsonDoubles(curve.ts), jsonDoubles(curve.tp));
	}
	return fmt::format(R"({{"key":{},"name":{},"kind":"{}","source":{},"polarizing":{},"rs":{},"rp":{},"ts":{},"tp":{},"designWavelength":{},"metal":{},"layers":[{}],"curves":[{}]}})",
		jsonString(c.key), jsonString(c.name), coatingKindName(c.kind), jsonString(c.source), c.polarizing, jsonDouble(c.rs), jsonDouble(c.rp), jsonDouble(c.ts), jsonDouble(c.tp),
		jsonDouble(c.designWavelength), jsonString(c.metal), layers, curves);
}

std::string opticalLibraryJson(const SceneDescription& scene, const std::filesystem::path& sceneDirectory)
{
	if (scene.glassCatalogs.empty() && scene.userGlasses.empty() && scene.userCoatings.empty()) {
		return {};
	}
	std::string json = R"(,"opticalLibrary":{"catalogs":[)";
	for (size_t i = 0; i < scene.glassCatalogs.size(); i++) {
		json += (i > 0 ? "," : "") + jsonString(portablePath(scene.glassCatalogs[i], sceneDirectory));
	}
	json += R"(],"glasses":[)";
	for (size_t i = 0; i < scene.userGlasses.size(); i++) {
		json += (i > 0 ? "," : "") + glassJson(scene.userGlasses[i]);
	}
	json += R"(],"coatings":[)";
	for (size_t i = 0; i < scene.userCoatings.size(); i++) {
		json += (i > 0 ? "," : "") + coatingJson(scene.userCoatings[i]);
	}
	return json + "]}";
}

// a parameter's value as the file holds it: a number in SI units, an enum by its key, a bool, a glass's name
std::string opticValueJson(const OpticParam& param, const OpticValue& value)
{
	if (const double* d = std::get_if<double>(&value)) {
		return jsonDouble(*d);
	}
	if (const int* i = std::get_if<int>(&value)) {
		if (*i >= 0 && *i < (int)param.keys.size()) {
			return jsonString(param.keys[(size_t)*i]);
		}
		return fmt::format("{}", *i);
	}
	if (const bool* b = std::get_if<bool>(&value)) {
		return *b ? "true" : "false";
	}
	return jsonString(std::get<std::string>(value));
}

// an optic's mount: the stack it stands on, or where it is when it is free or unmounted
std::string mountJson(const MountPose& mount)
{
	std::string json = fmt::format(R"({{"kind":"{}")", mountKindKey(mount.kind));
	if (mount.isFree()) {
		const glm::dvec3& p = mount.freePosition;
		const glm::dquat& q = mount.freeOrientation;
		json += fmt::format(R"({},"position":[{},{},{}],"orientation":[{},{},{},{}])", mount.kind != MountKind::None ? R"(,"free":true)" : "", jsonDouble(p.x),
			jsonDouble(p.y), jsonDouble(p.z), jsonDouble(q.x), jsonDouble(q.y), jsonDouble(q.z), jsonDouble(q.w));
		//an unmounted optic has no stack; a free one keeps where its stack stood
		if (mount.kind == MountKind::None) {
			return json + "}";
		}
	}
	json += fmt::format(R"(,"origin":[{},{}],"yaw":{},"height":{},"fork":{{"angle":{},"reach":{}}})", jsonDouble(mount.origin.x), jsonDouble(mount.origin.y), jsonDouble(mount.yaw),
		jsonDouble(mount.height), jsonDouble(mount.forkAngle), jsonDouble(mount.forkReach));
	std::string fine;
	if (mount.fine.adjusterX != 0.0 || mount.fine.adjusterY != 0.0) {
		fine = fmt::format(R"("x":{},"y":{})", jsonDouble(mount.fine.adjusterX), jsonDouble(mount.fine.adjusterY));
	}
	if (mount.fine.rotor != 0.0) {
		fine += fmt::format(R"({}"rotor":{})", fine.empty() ? "" : ",", jsonDouble(mount.fine.rotor));
	}
	if (!fine.empty()) {
		json += fmt::format(R"(,"fine":{{{}}})", fine);
	}
	return json + "}";
}

std::string benchJson(const SceneDescription& scene)
{
	const BenchSettings& bench = scene.bench;
	if (bench == BenchSettings {}) {
		return {};
	}
	return fmt::format(R"(,"bench":{{"table":{},"size":[{},{}],"beamHeight":{},"snap":{}}})", bench.table, jsonDouble(bench.size.x), jsonDouble(bench.size.y),
		jsonDouble(bench.beamHeight), bench.snap);
}

// an optic's "params" that differ from its kind's defaults and the "materials" of the slots that differ from theirs,
// each the inside of its object
std::string opticParamsJson(const SceneOptic& optic)
{
	std::string params;
	for (const OpticParam& param : opticTraits(optic.kind).params) {
		const auto found = optic.params.find(param.name);
		// a lens's origin choice is always written: its absence marks a file from before the setting (readOptic())
		const bool always = optic.kind == OpticKind::SphericalLens && std::string_view(param.name) == "originAt";
		if (!always && (found == optic.params.end() || found->second == param.defaultValue)) {
			continue;
		}
		const OpticValue& value = found != optic.params.end() ? found->second : param.defaultValue;
		params += fmt::format(R"({}"{}":{})", params.empty() ? "" : ",", param.name, opticValueJson(param, value));
	}
	return params;
}

std::string opticMaterialsJson(OpticKind kind, const std::array<std::optional<MaterialDef>, MAX_OPTIC_MATERIAL_SLOTS>& slots)
{
	const OpticTraits& traits = opticTraits(kind);
	std::string materials;
	for (size_t slot = 0; slot < traits.slots.size(); slot++) {
		const std::optional<MaterialDef>& material = slots[slot];
		if (!material) {
			continue;
		}
		materials += fmt::format(R"({}"{}":{{"type":"{}"{}}})", materials.empty() ? "" : ",", traits.slots[slot].name, materialTypeName(material->type), materialParamsJson(*material, nullptr));
	}
	return materials;
}

std::string opticsJson(const SceneDescription& scene)
{
	if (scene.optics.empty()) {
		return {};
	}
	std::string json = R"(,"optics":[)";
	for (size_t i = 0; i < scene.optics.size(); i++) {
		const SceneOptic& optic = scene.optics[i].optic;
		const OpticTraits& traits = opticTraits(optic.kind);
		const std::string size = optic.sizePreset >= 0 && optic.sizePreset < (int)traits.sizes.size() ? traits.sizes[(size_t)optic.sizePreset].key : "custom";
		json += fmt::format(R"({}{{"type":"{}","name":{},"size":"{}","params":{{{}}},"mount":{},"materials":{{{}}}}})", i > 0 ? "," : "", traits.name,
			jsonString(optic.name), size, opticParamsJson(optic), mountJson(optic.mount), opticMaterialsJson(optic.kind, scene.optics[i].slots));
	}
	return json + "]";
}

//version 30: the sensor stacks
std::string sensorStacksJson(const SceneDescription& scene)
{
	if (scene.sensorStacks.empty()) {
		return {};
	}
	std::string json = R"(,"sensorStacks":[)";
	for (size_t i = 0; i < scene.sensorStacks.size(); i++) {
		const SensorStack& stack = scene.sensorStacks[i].stack;
		const glm::dquat& q = stack.orientation;
		json += fmt::format(R"({}{{"name":{},"sensor":{{"type":"{}","params":{{{}}},"materials":{{{}}}}},"start":[{},{},{}],"end":[{},{},{}],"count":{},"orientation":[{},{},{},{}],"alignToAxis":{}}})",
			i > 0 ? "," : "", jsonString(stack.name), opticTraits(stack.sensor.kind).name, opticParamsJson(stack.sensor),
			opticMaterialsJson(stack.sensor.kind, scene.sensorStacks[i].slots), jsonDouble(stack.start.x), jsonDouble(stack.start.y), jsonDouble(stack.start.z),
			jsonDouble(stack.end.x), jsonDouble(stack.end.y), jsonDouble(stack.end.z), stack.count, jsonDouble(q.x), jsonDouble(q.y), jsonDouble(q.z),
			jsonDouble(q.w), stack.alignToAxis);
	}
	return json + "]";
}

std::string sceneJson(const SceneDescription& scene, const std::filesystem::path& sceneDirectory)
{
	std::string json = fmt::format(R"({{"version":{},"models":[)", SCENE_FILE_FORMAT_VERSION);

	for (size_t i = 0; i < scene.modelPaths.size(); i++) {
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"path":{}}})", jsonString(portablePath(scene.modelPaths[i], sceneDirectory)));
	}
	json += ']';

	//version 20: the capabilities, the template, and a template file's own name
	if (!scene.capabilities.empty()) {
		json += R"(,"capabilities":[)";
		for (size_t i = 0; i < scene.capabilities.size(); i++) {
			json += fmt::format("{}{}", i > 0 ? "," : "", jsonString(scene.capabilities[i]));
		}
		json += ']';
	}
	if (!scene.templateKey.empty()) {
		json += fmt::format(R"(,"template":{})", jsonString(scene.templateKey));
	}
	if (scene.templateInfo.has_value()) {
		json += fmt::format(R"(,"templateInfo":{{"name":{},"description":{}}})", jsonString(scene.templateInfo->name), jsonString(scene.templateInfo->description));
	}

	if (!scene.environmentMapPath.empty()) {
		json += fmt::format(R"(,"environmentMap":{})", jsonString(portablePath(scene.environmentMapPath, sceneDirectory)));
	}
	if (scene.environmentModel >= 0) {
		json += fmt::format(R"(,"environmentModel":{})", scene.environmentModel);
	}

	json += fmt::format(R"(,"environmentIntensity":{})", jsonNumber(scene.environmentIntensity));
	if (scene.environmentRotation != 0.f) {
		json += fmt::format(R"(,"environmentRotation":{})", jsonNumber(scene.environmentRotation));
	}
	json += fmt::format(R"(,"backgroundMode":{},"background":{})", jsonString(backgroundModeName(scene.backgroundMode)), jsonVec3(scene.backgroundColor));

	//every setting, defaults on read, so a scene shared between machines loses nothing and an
	//older build ignores the block
	const RenderSettings& r = scene.render;
	json += fmt::format(R"(,"render":{{"width":{},"height":{},"matchViewport":{},"antialiasing":{},"maxSamples":{},"unlimitedSamples":{},"rayDepth":{},"useFixedSeed":{},"seed":{},"samplesPerFrame":{},"russianRoulette":{},"minBouncesBeforeRoulette":{},"restartOnChange":{},"errorBoundedOffsets":{},"watertightTriangles":{},"samplerRandomization":{})",
		r.width, r.height, r.matchViewport, r.antialiasing, r.maxSamples, r.unlimitedSamples, r.rayDepth, r.useFixedSeed, r.seed, r.samplesPerFrame, r.russianRoulette, r.minBouncesBeforeRoulette, r.restartOnChange,
		r.errorBoundedOffsets, r.watertightTriangles, jsonString(samplerRandomizationId(r.samplerRandomization)));
	//version 25: the textures
	json += fmt::format(R"(,"textureStorage":"{}","maxTextureSize":{},"textureFilter":"{}")", r.textureStorage == TextureStorage::Legacy ? "legacy" : "native", r.maxTextureSize,
		TEXTURE_FILTER_NAMES[(size_t)r.textureFilter]);
	//version 15: the film
	json += fmt::format(R"(,"pixelFilter":{{"kind":{},"radius":{},"sigma":{},"b":{},"c":{},"tau":{}}},"clampSamples":{},"clampValue":{},"outlierKappa":{}}})",
		jsonString(pixelFilterId(r.pixelFilter.kind)), jsonNumber(r.pixelFilter.radius), jsonNumber(r.pixelFilter.sigma), jsonNumber(r.pixelFilter.b), jsonNumber(r.pixelFilter.c),
		jsonNumber(r.pixelFilter.tau), r.clampSamples, jsonNumber(r.clampValue), jsonNumber(r.outlierKappa));
	//version 16: the auxiliary outputs (by id) and the light groups, still inside "render"
	json.pop_back();
	json += R"(,"aov":{"outputs":[)";
	bool firstOutput = true;
	for (uint32_t i = 0; i < (uint32_t)AovOutput::Count; i++) {
		if (r.aov.has((AovOutput)i)) {
			json += fmt::format("{}{}", firstOutput ? "" : ",", jsonString(aovOutputId((AovOutput)i)));
			firstOutput = false;
		}
	}
	json += fmt::format(R"(],"captureThroughSpecular":{}}},"lightGroups":{}}})", r.aov.captureThroughSpecular, r.lightGroups);
	//version 29: light tracing's paths toward a detector render target, when not the default
	if (r.lightPathsPerSample != RenderSettings {}.lightPathsPerSample) {
		json.pop_back();
		json += fmt::format(R"(,"lightPathsPerSample":{}}})", r.lightPathsPerSample);
	}
	//version 30: whether light tracing records every detector, when it does
	if (r.renderAllSensors) {
		json.pop_back();
		json += R"(,"renderAllSensors":true})";
	}
	//version 18: the spectral settings, still inside "render"
	{
		const SpectralSettings& s = r.spectral;
		json.pop_back();
		std::string gaussians;
		for (size_t i = 0; i < s.gaussians.size(); i++) {
			gaussians += fmt::format("{}[{},{}]", i == 0 ? "" : ",", jsonNumber(s.gaussians[i].center), jsonNumber(s.gaussians[i].fwhm));
		}
		json += fmt::format(R"(,"spectral":{{"representation":{},"lanes":{},"range":[{},{}],"wavelength":{},"designWavelength":{},"bandBasis":{},"gaussians":[{}],"sampling":{},"emittersShare":{},"colorSpace":{},"images":{},"matching":"cie1931"}}}})",
			jsonString(spectralRepresentationKey(s.representation)), s.lanes, jsonNumber(s.lambdaMin), jsonNumber(s.lambdaMax), jsonNumber(s.monochromaticWavelength), jsonNumber(s.designWavelength),
			jsonString(bandBasisKey(s.bandBasis)), gaussians, jsonString(wavelengthSamplingKey(s.sampling)), jsonNumber(s.lightSamplingShare), jsonString(colorSpace(s.colorSpace).key), s.imageBuckets);
		//version 28: only when the design wavelength does not follow the scene's laser
		if (!s.designFollowsLaser) {
			json.resize(json.size() - 2);
			json += R"(,"designFollowsLaser":false}})";
		}
	}
	//version 21: the optics settings, still inside "render", only when they differ from the defaults
	if (r.optics != OpticsSettings {}) {
		json.pop_back();
		json += fmt::format(R"(,"optics":{{"surrounding":"{}","exactCoatings":{}}}}})", r.optics.surrounding == SurroundingMedium::Vacuum ? "vacuum" : "air", r.optics.exactCoatings);
	}
	//version 27: the bidirectional integrators' settings, still inside "render", only when they differ from the defaults
	if (r.bidirectional != BidirectionalSettings {}) {
		json.pop_back();
		json += fmt::format(R"(,"bidirectional":{{"connections":{},"cacheBudget":{},"visualizeStrategies":{},"visualizeWeights":{}}}}})", r.bidirectional.connections,
			r.bidirectional.cacheBudgetMB, r.bidirectional.visualizeStrategies, r.bidirectional.visualizeWeights);
	}
	//version 18: how a monochromatic render is shown
	{
		const SpectralDisplay& d = scene.spectralDisplay;
		json += fmt::format(R"(,"spectralDisplay":{{"mode":{},"colormap":{},"logarithmic":{},"decades":{},"infrared":{},"ultraviolet":{},"xuv":{}}})", jsonString(monochromeDisplayKey(d.mode)),
			jsonString(colormapKey(d.colormap)), d.logarithmic, jsonNumber(d.decades), jsonVec3(d.infraredColor), jsonVec3(d.ultravioletColor), jsonVec3(d.xuvColor));
	}

	//version 16: the light groups' names and the background's group, and what exporting a render does
	json += R"(,"lightGroupNames":[)";
	for (size_t i = 0; i < scene.lightGroupNames.size(); i++) {
		json += fmt::format("{}{}", i > 0 ? "," : "", jsonString(scene.lightGroupNames[i]));
	}
	json += fmt::format(R"(],"environmentGroup":{},"export":{{"half":{},"saveWhenFinished":{},"path":{}}})", scene.environmentGroup, scene.exportSettings.halfFloat, scene.exportSettings.saveWhenFinished,
		jsonString(scene.exportSettings.saveWhenFinishedPath.empty() ? std::string() : portablePath(std::filesystem::path(scene.exportSettings.saveWhenFinishedPath), sceneDirectory)));

	//version 11: the integrator and the traversal; version 8: the variant of each stage, by slot
	//number and variant id. Ids rather than indices so that registering a new variant, or reordering
	//a table, never silently changes what an existing scene renders with
	const KernelSelectionIds kernelIds = kernelSelectionToIds(scene.kernels);
	json += fmt::format(R"(,"integrator":{},"traversal":{})", jsonString(kernelIds.integrator), jsonString(kernelIds.traversal));
	json += R"(,"kernels":{)";
	for (size_t i = 0; i < kernelIds.stages.size(); i++) {
		if (i > 0) {
			json += ',';
		}
		json += fmt::format("{}:{}", jsonString(kernelIds.stages[i].first), jsonString(kernelIds.stages[i].second));
	}
	json += '}';

	//version 8: how the BVH is built. The node layout is deliberately NOT here - it follows
	//whichever traversal is selected, so saving it would let a file contradict itself
	const BvhBuildOptions& a = scene.accel.blas;
	json += fmt::format(R"(,"accel":{{"builder":{},"maxLeafSize":{},"binCount":{},"traversalCost":{},"intersectionCost":{},"spatialAlpha":{},"spatialBudget":{},"maxDepth":{}}})",
		jsonString(bvhBuilderName(a.builder)), a.maxLeafSize, a.binCount,
		jsonNumber(a.traversalCost), jsonNumber(a.intersectionCost), jsonNumber(a.spatialAlpha), jsonNumber(a.spatialBudget), a.maxDepth);

	if (scene.renderDetector >= 0) {
		json += fmt::format(R"(,"renderDetector":{})", scene.renderDetector);
	}
	json += fmt::format(R"(,"renderCamera":{},"cameras":[)", scene.renderCamera);
	for (size_t i = 0; i < scene.cameras.size(); i++) {
		const SceneCamera& c = scene.cameras[i];
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"name":{},"position":{},"orientation":{},"fov":{},"aperture":{},"focusDistance":{},"exposure":{})",
			jsonString(c.name), jsonVec3(c.position), jsonQuat(c.orientation),
			jsonNumber(c.vfovDegrees), jsonNumber(c.aperture), jsonNumber(c.focusDistance), jsonNumber(c.exposure));
		//version 26: the projection, and the lens's mapping, shape, tilt and shift
		json += fmt::format(R"(,"kind":{},"lensMapping":{},"apertureShape":{},"apertureBlades":{},"apertureRotation":{},"tilt":[{},{}],"shift":[{},{}])",
			jsonString(cameraKindId(c.kind)), jsonString(lensMappingId(c.lensMapping)), jsonString(apertureShapeId(c.apertureShape)), c.apertureBlades,
			jsonNumber(c.apertureRotationDegrees), jsonNumber(c.tiltDegrees.x), jsonNumber(c.tiltDegrees.y), jsonNumber(c.shift.x), jsonNumber(c.shift.y));
		if (cameraTraits(c.kind).viewHeight) {
			json += fmt::format(R"(,"viewHeight":{})", jsonNumber(c.orthoHeight));
		}
		if (cameraTraits(c.kind).sphericalMapping) {
			json += fmt::format(R"(,"mapping":{})", jsonString(sphericalMappingId(c.mapping)));
		}
		if (!c.apertureImage.empty()) {
			json += fmt::format(R"(,"apertureImage":{})", jsonString(portablePath(std::filesystem::path(c.apertureImage), sceneDirectory)));
		}
		//version 16: what Relight multiplies each light group by, display only like the exposure; omitted at the default
		if (c.relight != SceneCamera {}.relight) {
			json += R"(,"relight":[)";
			for (size_t g = 0; g < c.relight.size(); g++) {
				json += fmt::format(R"({}{{"scale":{},"tint":{}}})", g > 0 ? "," : "", jsonNumber(c.relight[g].scale), jsonVec3(c.relight[g].tint));
			}
			json += ']';
		}
		json += '}';
	}
	json += ']';

	//version 10: the punctual lights, each with only what its kind uses
	json += R"(,"lights":[)";
	for (size_t i = 0; i < scene.lights.size(); i++) {
		const SceneLight& light = scene.lights[i].light;
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"type":"{}","name":{},"position":{})", lightKindName(light.kind), jsonString(light.name), jsonVec3(light.position));
		if (light.kind != LightKind::Point) {
			json += fmt::format(R"(,"orientation":{})", jsonQuat(light.orientation));
		}
		json += fmt::format(R"(,"color":{},"intensity":{})", spectrumJson(light.color), jsonNumber(light.intensity));
		if (light.kind != LightKind::Directional) {
			json += fmt::format(R"(,"range":{})", jsonNumber(light.range));
		}
		if (light.kind == LightKind::Spot) {
			json += fmt::format(R"(,"innerCone":{},"outerCone":{})", jsonNumber(light.innerConeDegrees), jsonNumber(light.outerConeDegrees));
			if (light.falloff == SpotFalloff::Smoothstep) {
				json += R"(,"falloff":"smoothstep")";
			}
		}
		if (light.kind == LightKind::Projection || light.kind == LightKind::Goniometric) {
			json += fmt::format(R"(,"image":{})", jsonString(light.image.empty() ? std::string() : portablePath(std::filesystem::path(light.image), sceneDirectory)));
			if (light.kind == LightKind::Projection) {
				json += fmt::format(R"(,"fov":{})", jsonNumber(light.fovDegrees));
			}
			if (light.mirrored) {
				json += R"(,"mirrored":true)";
			}
		}
		if (light.unit != LightUnit::Radiometric) {
			json += fmt::format(R"(,"unit":"{}")", lightUnitName(light.unit));
		}
		if (scene.lights[i].model >= 0) {
			json += fmt::format(R"(,"model":{})", scene.lights[i].model);
		}
		if (light.lightGroup != 0) {
			json += fmt::format(R"(,"lightGroup":{})", light.lightGroup);
		}
		json += '}';
	}
	json += ']';

	//version 25: the image files the materials' textures read
	json += R"(,"textures":[)";
	for (size_t i = 0; i < scene.textures.size(); i++) {
		const SceneTextureRecord& texture = scene.textures[i];
		json += fmt::format(R"({}{{"path":{},"colorSpace":"{}","wrap":["{}","{}"],"nearest":[{},{}],"encoding":"{}","gamma":{},"invert":{}}})", i > 0 ? "," : "",
			jsonString(portablePath(texture.path, sceneDirectory)), texture.colorSpace == TextureColorSpace::Linear ? "linear" : "srgb", TEXTURE_WRAP_NAMES[(size_t)texture.sampling.wrapU],
			TEXTURE_WRAP_NAMES[(size_t)texture.sampling.wrapV], texture.sampling.nearestMag, texture.sampling.nearestMin,
			texture.encoding == TextureEncoding::Linear ? "linear" : texture.encoding == TextureEncoding::Gamma ? "gamma" : "srgb", jsonNumber(texture.gamma), texture.invert);
	}
	json += ']';

	//version 12: the library. User materials in full, kept whether or not anything references them;
	//glTF materials only as what the user changed of them
	json += R"(,"materials":[)";
	for (size_t i = 0; i < scene.materials.size(); i++) {
		const MaterialDef& material = scene.materials[i];
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"name":{},"type":"{}"{}{}}})", jsonString(material.name), materialTypeName(material.type), materialParamsJson(material, nullptr),
			material.lightGroup != 0 ? fmt::format(R"(,"lightGroup":{})", material.lightGroup) : std::string());
	}
	json += R"(],"materialOverrides":[)";
	for (size_t i = 0; i < scene.materialOverrides.size(); i++) {
		const SceneMaterialOverride& edit = scene.materialOverrides[i];
		if (i > 0) {
			json += ',';
		}
		MaterialDef edited;
		edited.type = MaterialType::Pbr;
		edited.params = edit.params;
		edited.textures = edit.textures;
		std::set<std::string, std::less<>> names;
		for (const auto& [name, value] : edit.params) {
			names.insert(name);
		}
		json += fmt::format(R"({{"model":{},"material":{}{}{}}})", edit.model, edit.modelMaterial, materialParamsJson(edited, &names),
			edit.lightGroup != 0 ? fmt::format(R"(,"lightGroup":{})", edit.lightGroup) : std::string());
	}
	json += ']';

	//version 22: the viewport's free camera
	if (scene.viewport) {
		const SceneViewportRecord& viewport = *scene.viewport;
		json += fmt::format(R"(,"viewport":{{"position":{},"orientation":{},"pivot":{},"speed":{},"fov":{},"near":{},"far":{}}})", jsonVec3(viewport.position),
			jsonQuat(viewport.orientation), jsonVec3(viewport.pivot), jsonNumber(viewport.settings.speed), jsonNumber(viewport.settings.vfovDegrees),
			jsonNumber(viewport.settings.nearClip), jsonNumber(viewport.settings.farClip));
	}

	//version 21: the length unit, the optical library and the optical elements
	if (scene.lengthUnit != LengthUnit::Meters) {
		json += fmt::format(R"(,"lengthUnit":"{}")", lengthUnitKey(scene.lengthUnit));
	}
	json += opticalLibraryJson(scene, sceneDirectory);
	json += opticsJson(scene);
	json += sensorStacksJson(scene);
	//version 23: the bench
	json += benchJson(scene);

	//every kind through its traits: the sizes go out under the names the editor shows, so a
	//cylinder reads {"radius":0.5,"height":2} rather than as a raw scale vector
	json += R"(,"geometry":[)";
	for (size_t i = 0; i < scene.shapes.size(); i++) {
		const SceneShape& shape = scene.shapes[i].shape;
		const ShapeTraits& traits = shapeTraits(shape.kind);
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"type":"{}","name":{},"position":{})", traits.name, jsonString(shape.name), jsonVec3(shape.position));
		if (traits.rotatable) {
			json += fmt::format(R"(,"orientation":{})", jsonQuat(shape.orientation));
		}
		for (const ShapeParam& param : traits.params) {
			json += fmt::format(R"(,"{}":{})", param.name, jsonNumber(shapeParamValue(shape, param)));
		}
		if (scene.shapes[i].model >= 0) {
			json += fmt::format(R"(,"model":{})", scene.shapes[i].model);
		}
		if (shape.reverseOrientation) {
			json += R"(,"reverseOrientation":true)";
		}
		json += fmt::format(R"(,"material":{}}})", materialRefJson(scene.shapes[i].material));
	}

	json += ']';

	//version 5: the placed mesh objects. The transform goes out as the sixteen raw matrix
	//elements in glm's (column-major) order rather than as decomposed translation/rotation/scale,
	//so a save and reload is bit-exact and no euler ambiguity can creep in
	json += R"(,"meshObjects":[)";
	for (size_t i = 0; i < scene.meshObjects.size(); i++) {
		const SceneMeshObjectRecord& object = scene.meshObjects[i];
		if (i > 0) {
			json += ',';
		}
		std::string matrix;
		for (int element = 0; element < 16; element++) {
			if (element > 0) {
				matrix += ',';
			}
			matrix += jsonNumber((&object.transform[0][0])[element]);
		}
		json += fmt::format(R"({{"name":{},"model":{},"node":{},"transform":[{}],"visible":{},"shading":"{}")",
			jsonString(object.name), object.model, object.nodeIndex, matrix, object.visible,
			object.materialMode == MeshMaterialMode::Override ? "override" : "gltf");
		if (object.materialMode == MeshMaterialMode::Override) {
			json += fmt::format(R"(,"material":{})", materialRefJson(object.material));
		}
		json += '}';
	}

	json += "]}";
	return json;
}

//fastgltf asks for extras once per exported entity; only the single scene gets the payload
std::optional<std::string> writeExtras(std::size_t objectIndex, fastgltf::Category category, void* userPointer)
{
	if (category != fastgltf::Category::Scenes || objectIndex != 0) {
		return std::nullopt;
	}
	return *static_cast<const std::string*>(userPointer);
}

//---------------------------------------------------------------- reading

using JsonElement = simdjson::simdjson_result<simdjson::dom::element>;

bool readDouble(simdjson::dom::element element, double& out)
{
	if (element.get_double().get(out) == simdjson::SUCCESS) {
		return true;
	}
	//a hand-edited file may well say 100 rather than 100.0
	int64_t integer = 0;
	if (element.get_int64().get(integer) == simdjson::SUCCESS) {
		out = (double)integer;
		return true;
	}
	return false;
}

//the lookup form, for fields that may be missing entirely
bool readDouble(JsonElement result, double& out)
{
	simdjson::dom::element element;
	if (result.get(element) != simdjson::SUCCESS) {
		return false;
	}
	return readDouble(element, out);
}

bool readVec3(JsonElement element, glm::vec3& out)
{
	simdjson::dom::array array;
	if (element.get_array().get(array) != simdjson::SUCCESS || array.size() != 3) {
		return false;
	}
	size_t i = 0;
	for (simdjson::dom::element component : array) {
		double value = 0.0;
		if (!readDouble(component, value)) {
			return false;
		}
		out[(glm::length_t)i++] = (float)value;
	}
	return true;
}

// what spectrumJson() wrote; false, leaving `out` alone, for anything else. A file a .spd reference names that is
// gone keeps the reference with no samples, which evaluates to zero, and says so
bool readSpectrum(JsonElement element, SpectrumRef& out)
{
	glm::vec3 rgb(0.f);
	if (readVec3(element, rgb)) {
		out = rgbSpectrum(rgb);
		return true;
	}
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return false;
	}
	double scale = 1.0;
	readDouble(object["scale"], scale);
	double value = 0.0;
	std::string_view text;
	if (readDouble(object["constant"], value)) {
		out = constantRef((float)value);
		return true;
	}
	if (readDouble(object["blackbody"], value)) {
		out = blackbodyRef((float)value, (float)scale);
		return true;
	}
	if (object["named"].get_string().get(text) == simdjson::SUCCESS) {
		if (std::optional<SpectrumRef> named = namedRef(text, (float)scale)) {
			out = *named;
			return true;
		}
		fmt::println("scene: the named spectrum \"{}\" is not one pbrt-v4 has; the parameter keeps its value", text);
		return false;
	}
	if (object["spd"].get_string().get(text) == simdjson::SUCCESS) {
		std::string error;
		if (std::optional<SpectrumRef> file = fileRef(std::string(text), &error, (float)scale)) {
			out = *file;
		} else {
			fmt::println("scene: {}", error);
			out = tabulatedRef({}, (float)scale);
			out.kind = SpectrumKind::File;
			out.name = std::string(text);
		}
		return true;
	}
	simdjson::dom::array array;
	if (object["tabulated"].get_array().get(array) == simdjson::SUCCESS) {
		std::vector<float> samples;
		for (simdjson::dom::element v : array) {
			double number = 0.0;
			if (readDouble(v, number)) {
				samples.push_back((float)number);
			}
		}
		out = tabulatedRef(std::move(samples), (float)scale);
		return true;
	}
	int64_t formula = 0;
	if (object["dispersion"].get_int64().get(formula) == simdjson::SUCCESS) {
		std::vector<double> coefficients;
		if (object["coefficients"].get_array().get(array) == simdjson::SUCCESS) {
			for (simdjson::dom::element v : array) {
				double number = 0.0;
				if (readDouble(v, number)) {
					coefficients.push_back(number);
				}
			}
		}
		std::string_view name;
		object["name"].get_string().get(name);
		out = dispersionRef((DispersionFormula)formula, coefficients, std::string(name));
		glm::vec3 range(0.f);
		if (object["range"].get_array().get(array) == simdjson::SUCCESS && array.size() == 2) {
			double a = 0.0, b = 0.0;
			readDouble(array.at(0), a);
			readDouble(array.at(1), b);
			out.validRange = glm::vec2((float)a, (float)b);
		}
		return true;
	}
	if (readDouble(object["line"], value)) {
		double fwhm = 0.0;
		readDouble(object["fwhm"], fwhm);
		std::string_view profile;
		object["profile"].get_string().get(profile);
		out = lineRef((float)value, (float)fwhm, profile == "lorentzian" ? LineProfile::Lorentzian : LineProfile::Gaussian, (float)scale);
		return true;
	}
	return false;
}

//written x, y, z, w by jsonQuat()
bool readQuat(JsonElement element, glm::quat& out)
{
	simdjson::dom::array array;
	if (element.get_array().get(array) != simdjson::SUCCESS || array.size() != 4) {
		return false;
	}
	float q[4];
	size_t i = 0;
	for (simdjson::dom::element component : array) {
		double value = 0.0;
		if (!readDouble(component, value)) {
			return false;
		}
		q[i++] = (float)value;
	}
	//normalized only when it is not unit length already. Normalizing a quaternion that is unit to
	//float precision can move it by an ulp, and normalizing that result moves it back, so a file
	//saved and reopened alternated between two orientations on every save. jsonNumber() writes the
	//shortest text that reads back as the same float, so a value saved unit stays exactly as saved
	const glm::quat read(q[3], q[0], q[1], q[2]);
	const float lengthSquared = glm::dot(read, read);
	if (!(lengthSquared > 1e-12f)) {
		//no rotation can be recovered from a zero quaternion; it reads as none
		out = glm::quat(1.f, 0.f, 0.f, 0.f);
	} else if (std::abs(lengthSquared - 1.f) > 1e-5f) {
		out = glm::normalize(read);
	} else {
		out = read;
	}
	return true;
}

//an inline material of a file before version 12, as a user material: the selected type's fields, by
//the names the parameters still have
void readMaterialParams(simdjson::dom::object object, MaterialType type, ParamSet& params);

bool readLegacyMaterial(JsonElement element, MaterialDef& out, std::string& error)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		error = "\"material\" is missing or not an object";
		return false;
	}

	std::string_view typeName;
	if (object["type"].get_string().get(typeName) != simdjson::SUCCESS) {
		error = "material has no \"type\"";
		return false;
	}

	const std::optional<MaterialType> type = findMaterialType(typeName);
	if (!type.has_value()) {
		error = fmt::format("unknown material type \"{}\"", typeName);
		return false;
	}

	//the defaults stand for every field the file does not carry (the unselected types' parameters)
	out = MaterialDef {};
	out.type = *type;
	//the types newer than inline materials have no legacy form: a hand-written or generated file (tools/verify) gives
	//their parameters by name, as a version 12 material record does
	if (*type == MaterialType::Glass || *type == MaterialType::Conductor || *type == MaterialType::Diffuse || *type == MaterialType::ThinGlass
		|| *type == MaterialType::DiffuseTransmission || *type == MaterialType::Mix) {
		readMaterialParams(object, *type, out.params);
		return true;
	}

	//a light's colour is what it emits, not an albedo, and the file said so
	const char* colorKey = *type == MaterialType::Emissive ? "color" : "albedo";
	glm::vec3 albedo(0.f);
	if (*type != MaterialType::Dielectric) {
		if (!readVec3(object[colorKey], albedo)) {
			error = fmt::format("{} material has no valid \"{}\"", typeName, colorKey);
			return false;
		}
		out.params["albedo"] = rgbSpectrum(albedo);
	}

	double value = 0.0;
	switch (*type) {
	case MaterialType::Metal:
		if (!readDouble(object["fuzz"], value)) {
			error = "metal material has no valid \"fuzz\"";
			return false;
		}
		out.params["fuzz"] = std::clamp((float)value, 0.f, 1.f);
		break;
	case MaterialType::Phong:
		if (!readDouble(object["smoothness"], value)) {
			error = "phong material has no valid \"smoothness\"";
			return false;
		}
		out.params["smoothness"] = (float)value;
		break;
	case MaterialType::Dielectric:
		if (!readDouble(object["ir"], value)) {
			error = "dielectric material has no valid \"ir\"";
			return false;
		}
		out.params["ir"] = (float)value;
		break;
	case MaterialType::Emissive:
		if (!readDouble(object["strength"], value) || value < 0.0) {
			error = "emissive material has no valid \"strength\"";
			return false;
		}
		out.params["strength"] = (float)value;
		break;
	case MaterialType::Pbr:
		if (!readDouble(object["metallic"], value)) {
			error = "pbr material has no valid \"metallic\"";
			return false;
		}
		out.params["metallic"] = std::clamp((float)value, 0.f, 1.f);
		if (!readDouble(object["roughness"], value)) {
			error = "pbr material has no valid \"roughness\"";
			return false;
		}
		out.params["roughness"] = std::clamp((float)value, 0.f, 1.f);
		if (!readVec3(object["emission"], albedo)) {
			error = "pbr material has no valid \"emission\"";
			return false;
		}
		out.params["emission"] = rgbSpectrum(albedo);
		if (!readDouble(object["strength"], value) || value < 0.0) {
			error = "pbr material has no valid \"strength\"";
			return false;
		}
		out.params["strength"] = (float)value;
		break;
	default:
		//Lambertian, and the types newer than inline materials, which returned above
		break;
	}

	return true;
}

//optional fields: a missing or malformed one keeps the default, since older files have none
void readOptionalInt(JsonElement element, int& out)
{
	int64_t value = 0;
	if (element.get_int64().get(value) == simdjson::SUCCESS) {
		out = (int)value;
	}
}

void readOptionalUint(JsonElement element, uint32_t& out)
{
	uint64_t value = 0;
	if (element.get_uint64().get(value) == simdjson::SUCCESS) {
		out = (uint32_t)value;
	}
}

void readOptionalBool(JsonElement element, bool& out)
{
	bool value = false;
	if (element.get_bool().get(value) == simdjson::SUCCESS) {
		out = value;
	}
}

void readOptionalFloat(JsonElement element, float& out)
{
	double value = 0.0;
	if (readDouble(element, value)) {
		out = (float)value;
	}
}

//version 8: "kernels", {"00":"thin_lens","02":"closest",...}; version 11: "integrator" and
//"traversal" beside it. Before version 11 the traversal was kernel 02's variant ("02":"bvh_binary")
//and kernel 08 followed it, so an older file's "02" is read as the traversal and its "08" not at
//all. Unknown keys and ids are collected as warnings and left at their default rather than failing
//the load - a scene saved by a build with a strategy this one does not have must still open
void readKernelSelection(simdjson::dom::object& root, int64_t version, SceneDescription& scene)
{
	KernelSelectionIds ids;
	std::string_view text;
	if (root["integrator"].get_string().get(text) == simdjson::SUCCESS) {
		ids.integrator = std::string(text);
	}
	if (root["traversal"].get_string().get(text) == simdjson::SUCCESS) {
		ids.traversal = std::string(text);
	}
	simdjson::dom::object object;
	if (root["kernels"].get_object().get(object) == simdjson::SUCCESS) {
		for (auto field : object) {
			std::string_view value;
			if (field.value.get_string().get(value) != simdjson::SUCCESS) {
				continue;
			}
			if (version < 11 && field.key == "02") {
				ids.traversal = std::string(value);
				continue;
			}
			if (version < 11 && field.key == "08") {
				continue;
			}
			ids.stages.emplace_back(std::string(field.key), std::string(value));
		}
	}
	//version 14: kernel 01 had one variant until then, and a file that names none means it. Every file
	//from version 8 on names it, as "pcg_hash"; an older one has no "kernels" at all
	auto names = [&](KernelSlot slot) {
		return std::any_of(ids.stages.begin(), ids.stages.end(), [&](const auto& stage) { return stage.first == kernelSlotNumber(slot); });
	};
	if (!names(KernelSlot::GenerateSamples)) {
		ids.stages.emplace_back(kernelSlotNumber(KernelSlot::GenerateSamples), "pcg_hash");
	}
	//version 15: kernels 00 and 09 had one variant each until then, and a file that names none means it.
	//The same files as above: every one from version 8 on names them
	if (!names(KernelSlot::GenerateCameraRays)) {
		ids.stages.emplace_back(kernelSlotNumber(KernelSlot::GenerateCameraRays), "thin_lens");
	}
	if (!names(KernelSlot::UpdateFilm)) {
		ids.stages.emplace_back(kernelSlotNumber(KernelSlot::UpdateFilm), "running_mean");
	}
	scene.kernels = kernelSelectionFromIds(ids, &scene.kernelWarnings);
}

//version 8. The layout is not read: it follows the selected traversal, and
//RaytraceRenderer::setAccelSettings() puts it right
void readAccelSettings(JsonElement element, AccelSettings& out)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return;
	}
	BvhBuildOptions& blas = out.blas;
	std::string_view builder;
	if (object["builder"].get_string().get(builder) == simdjson::SUCCESS) {
		for (uint32_t i = 0; i < (uint32_t)BvhBuilder::Count; i++) {
			if (builder == bvhBuilderName((BvhBuilder)i)) {
				blas.builder = (BvhBuilder)i;
				break;
			}
		}
	}
	readOptionalUint(object["maxLeafSize"], blas.maxLeafSize);
	readOptionalUint(object["binCount"], blas.binCount);
	readOptionalUint(object["maxDepth"], blas.maxDepth);
	readOptionalFloat(object["traversalCost"], blas.traversalCost);
	readOptionalFloat(object["intersectionCost"], blas.intersectionCost);
	readOptionalFloat(object["spatialAlpha"], blas.spatialAlpha);
	readOptionalFloat(object["spatialBudget"], blas.spatialBudget);
}

void readBidirectionalSettings(JsonElement element, BidirectionalSettings& out)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return;
	}
	int64_t value = 0;
	if (object["connections"].get_int64().get(value) == simdjson::SUCCESS) {
		out.connections = (int)std::clamp<int64_t>(value, MIN_CACHE_CONNECTIONS, MAX_CACHE_CONNECTIONS);
	}
	if (object["cacheBudget"].get_int64().get(value) == simdjson::SUCCESS) {
		out.cacheBudgetMB = (int)std::clamp<int64_t>(value, MIN_CACHE_BUDGET_MB, MAX_CACHE_BUDGET_MB);
	}
	readOptionalBool(object["visualizeStrategies"], out.visualizeStrategies);
	readOptionalBool(object["visualizeWeights"], out.visualizeWeights);
}

void readOpticsSettings(JsonElement element, OpticsSettings& out)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return;
	}
	std::string_view surrounding;
	if (object["surrounding"].get_string().get(surrounding) == simdjson::SUCCESS) {
		out.surrounding = surrounding == "vacuum" ? SurroundingMedium::Vacuum : SurroundingMedium::Air;
	}
	readOptionalBool(object["exactCoatings"], out.exactCoatings);
}

void readRenderSettings(JsonElement element, RenderSettings& out)
{
	//a file before version 15 has no filter, and its renders placed a sample uniformly in its pixel
	out.pixelFilter = boxPixelFilter();
	//and one before 25 read its models' textures from the legacy array
	out.textureStorage = TextureStorage::Legacy;
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return;
	}
	std::string_view textureText;
	if (object["textureStorage"].get_string().get(textureText) == simdjson::SUCCESS) {
		out.textureStorage = textureText == "native" ? TextureStorage::Native : TextureStorage::Legacy;
	}
	readOptionalInt(object["maxTextureSize"], out.maxTextureSize);
	out.maxTextureSize = std::clamp(out.maxTextureSize, 16, 16384);
	if (object["textureFilter"].get_string().get(textureText) == simdjson::SUCCESS) {
		for (size_t i = 0; i < 4; i++) {
			if (textureText == TEXTURE_FILTER_NAMES[i]) {
				out.textureFilter = (TextureFilter)i;
			}
		}
	}
	readOptionalInt(object["width"], out.width);
	readOptionalInt(object["height"], out.height);
	readOptionalBool(object["matchViewport"], out.matchViewport);
	readOptionalBool(object["antialiasing"], out.antialiasing);
	readOptionalInt(object["maxSamples"], out.maxSamples);
	readOptionalBool(object["unlimitedSamples"], out.unlimitedSamples);
	readOptionalInt(object["rayDepth"], out.rayDepth);
	readOptionalBool(object["useFixedSeed"], out.useFixedSeed);
	readOptionalUint(object["seed"], out.seed);
	readOptionalInt(object["samplesPerFrame"], out.samplesPerFrame);
	readOptionalBool(object["russianRoulette"], out.russianRoulette);
	readOptionalInt(object["minBouncesBeforeRoulette"], out.minBouncesBeforeRoulette);
	readOptionalBool(object["restartOnChange"], out.restartOnChange);
	readOptionalBool(object["errorBoundedOffsets"], out.errorBoundedOffsets);
	readOptionalBool(object["watertightTriangles"], out.watertightTriangles);
	//version 14
	std::string_view randomization;
	if (object["samplerRandomization"].get_string().get(randomization) == simdjson::SUCCESS) {
		out.samplerRandomization = samplerRandomizationFromId(std::string(randomization));
	}
	//version 15
	simdjson::dom::object filter;
	if (object["pixelFilter"].get_object().get(filter) == simdjson::SUCCESS) {
		std::string_view kind;
		if (filter["kind"].get_string().get(kind) == simdjson::SUCCESS) {
			const PixelFilterKind parsed = pixelFilterFromId(std::string(kind).c_str());
			if (parsed != PixelFilterKind::Count) {
				out.pixelFilter = defaultPixelFilter(parsed);
			}
		}
		readOptionalFloat(filter["radius"], out.pixelFilter.radius);
		readOptionalFloat(filter["sigma"], out.pixelFilter.sigma);
		readOptionalFloat(filter["b"], out.pixelFilter.b);
		readOptionalFloat(filter["c"], out.pixelFilter.c);
		readOptionalFloat(filter["tau"], out.pixelFilter.tau);
	}
	readOptionalBool(object["clampSamples"], out.clampSamples);
	readOptionalFloat(object["clampValue"], out.clampValue);
	readOptionalFloat(object["outlierKappa"], out.outlierKappa);
	//version 16: the auxiliary outputs, by id (an id this build does not know is skipped), and the light groups
	simdjson::dom::object aov;
	if (object["aov"].get_object().get(aov) == simdjson::SUCCESS) {
		simdjson::dom::array outputs;
		if (aov["outputs"].get_array().get(outputs) == simdjson::SUCCESS) {
			for (simdjson::dom::element entry : outputs) {
				std::string_view id;
				if (entry.get_string().get(id) == simdjson::SUCCESS) {
					const AovOutput output = aovOutputFromId(std::string(id));
					if (output != AovOutput::Count) {
						out.aov.set(output, true);
					}
				}
			}
		}
		readOptionalBool(aov["captureThroughSpecular"], out.aov.captureThroughSpecular);
	}
	readOptionalBool(object["lightGroups"], out.lightGroups);
	{
		int64_t paths = 0;
		if (object["lightPathsPerSample"].get_int64().get(paths) == simdjson::SUCCESS && paths > 0) {
			out.lightPathsPerSample = (uint32_t)std::min<int64_t>(paths, 4096ll * 4096ll);
		}
	}
	if (object["renderAllSensors"].get_bool().get(out.renderAllSensors) != simdjson::SUCCESS) {
		out.renderAllSensors = false;
	}
	readOpticsSettings(object["optics"], out.optics);
	readBidirectionalSettings(object["bidirectional"], out.bidirectional);
	//version 18: the spectral settings; an older file renders in exact RGB with sRGB colors
	simdjson::dom::object spectral;
	if (object["spectral"].get_object().get(spectral) == simdjson::SUCCESS) {
		SpectralSettings& s = out.spectral;
		auto readKey = [&](const char* field, auto& value, auto count, auto keyOf) {
			std::string_view text;
			if (spectral[field].get_string().get(text) != simdjson::SUCCESS) {
				return;
			}
			for (uint32_t i = 0; i < (uint32_t)count; i++) {
				if (text == keyOf(i)) {
					value = (std::remove_reference_t<decltype(value)>)i;
				}
			}
		};
		readKey("representation", s.representation, SpectralRepresentation::Count, [](uint32_t i) { return spectralRepresentationKey((SpectralRepresentation)i); });
		readKey("bandBasis", s.bandBasis, BandBasis::Count, [](uint32_t i) { return bandBasisKey((BandBasis)i); });
		readKey("sampling", s.sampling, WavelengthSampling::Count, [](uint32_t i) { return wavelengthSamplingKey((WavelengthSampling)i); });
		readKey("colorSpace", s.colorSpace, ColorSpaceId::Count, [](uint32_t i) { return colorSpace((ColorSpaceId)i).key; });
		readOptionalUint(spectral["lanes"], s.lanes);
		readOptionalUint(spectral["images"], s.imageBuckets);
		simdjson::dom::array range;
		if (spectral["range"].get_array().get(range) == simdjson::SUCCESS && range.size() == 2) {
			double a = s.lambdaMin, b = s.lambdaMax;
			readDouble(range.at(0), a);
			readDouble(range.at(1), b);
			s.lambdaMin = (float)a;
			s.lambdaMax = (float)b;
		}
		readOptionalFloat(spectral["wavelength"], s.monochromaticWavelength);
		readOptionalFloat(spectral["designWavelength"], s.designWavelength);
		readOptionalBool(spectral["designFollowsLaser"], s.designFollowsLaser);
		readOptionalFloat(spectral["emittersShare"], s.lightSamplingShare);
		simdjson::dom::array gaussians;
		if (spectral["gaussians"].get_array().get(gaussians) == simdjson::SUCCESS) {
			size_t i = 0;
			for (simdjson::dom::element band : gaussians) {
				simdjson::dom::array pair;
				if (i < s.gaussians.size() && band.get_array().get(pair) == simdjson::SUCCESS && pair.size() == 2) {
					double center = s.gaussians[i].center, fwhm = s.gaussians[i].fwhm;
					readDouble(pair.at(0), center);
					readDouble(pair.at(1), fwhm);
					s.gaussians[i] = { (float)center, (float)fwhm };
				}
				i++;
			}
		}
		s = clampSpectralSettings(s);
	}

	//a hand-edited file cannot land on something the ui could not: the custom-resolution fields
	//hold to the same bounds
	out.width = std::clamp(out.width, MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION);
	out.height = std::clamp(out.height, MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION);
	out.maxSamples = std::max(out.maxSamples, 1);
	out.rayDepth = std::max(out.rayDepth, 1);
	out.samplesPerFrame = std::max(out.samplesPerFrame, 1);
	out.minBouncesBeforeRoulette = std::max(out.minBouncesBeforeRoulette, 0);
	//the film's parameters hold to the panel's bounds, which keep the filter's table small and finite
	out.pixelFilter.radius = std::clamp(out.pixelFilter.radius, MIN_FILTER_RADIUS, MAX_FILTER_RADIUS);
	out.pixelFilter.sigma = std::clamp(out.pixelFilter.sigma, 0.05f, 4.f);
	out.pixelFilter.b = std::clamp(out.pixelFilter.b, 0.f, 1.f);
	out.pixelFilter.c = std::clamp(out.pixelFilter.c, 0.f, 1.f);
	out.pixelFilter.tau = std::clamp(out.pixelFilter.tau, 0.5f, 8.f);
	out.clampValue = std::max(out.clampValue, 0.001f);
	out.outlierKappa = std::clamp(out.outlierKappa, 1.f, 1000.f);
}

//a version-3 file kept the lens and exposure in "render"; they now belong to every camera, so
//they seed the default camera the loader creates for such a file
void readLegacyCameraSettings(JsonElement element, SceneCamera& out)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return;
	}
	readOptionalFloat(object["aperture"], out.aperture);
	readOptionalFloat(object["focusDistance"], out.focusDistance);
	readOptionalFloat(object["exposure"], out.exposure);
}

std::filesystem::path resolvePath(std::string_view text, const std::filesystem::path& sceneDirectory);

// [x, y]; false, and `out` untouched, for anything else
bool readVec2(JsonElement element, glm::vec2& out)
{
	simdjson::dom::array array;
	if (element.get_array().get(array) != simdjson::SUCCESS || array.size() != 2) {
		return false;
	}
	glm::vec2 value(0.f);
	size_t i = 0;
	for (simdjson::dom::element component : array) {
		double number = 0.0;
		if (!readDouble(component, number)) {
			return false;
		}
		value[(glm::length_t)i++] = (float)number;
	}
	out = value;
	return true;
}

bool readCamera(simdjson::dom::object object, const std::filesystem::path& sceneDirectory, SceneCamera& out, std::string& error, std::vector<std::string>& warnings)
{
	//a camera from before version 26 is a perspective one whose lens maps its samples as Ray Tracing in One Weekend's
	//does, which is how it rendered
	out.lensMapping = LensMapping::Polar;
	std::string_view name;
	if (object["name"].get_string().get(name) == simdjson::SUCCESS) {
		out.name = std::string(name);
	}
	if (!readVec3(object["position"], out.position)) {
		error = "camera has no valid \"position\"";
		return false;
	}

	if (!readQuat(object["orientation"], out.orientation)) {
		error = "camera has no valid \"orientation\"";
		return false;
	}

	readOptionalFloat(object["fov"], out.vfovDegrees);
	readOptionalFloat(object["aperture"], out.aperture);
	readOptionalFloat(object["focusDistance"], out.focusDistance);
	readOptionalFloat(object["exposure"], out.exposure);
	//version 26: the projection and the rest of the lens
	std::string_view text;
	if (object["kind"].get_string().get(text) == simdjson::SUCCESS) {
		const CameraKind kind = cameraKindFromId(text);
		if (kind == CameraKind::Count || !cameraTraits(kind).sceneCamera) {
			warnings.push_back(fmt::format("camera \"{}\" has kind '{}', which this build does not know; it is a perspective camera", out.name, text));
		} else {
			out.kind = kind;
		}
	}
	readOptionalFloat(object["viewHeight"], out.orthoHeight);
	if (object["mapping"].get_string().get(text) == simdjson::SUCCESS && sphericalMappingFromId(text) != SphericalMapping::Count) {
		out.mapping = sphericalMappingFromId(text);
	}
	if (object["lensMapping"].get_string().get(text) == simdjson::SUCCESS && lensMappingFromId(text) != LensMapping::Count) {
		out.lensMapping = lensMappingFromId(text);
	}
	if (object["apertureShape"].get_string().get(text) == simdjson::SUCCESS) {
		const ApertureShape shape = apertureShapeFromId(text);
		if (shape == ApertureShape::Count) {
			warnings.push_back(fmt::format("camera \"{}\" has aperture shape '{}', which this build does not know; it is a circle", out.name, text));
		} else {
			out.apertureShape = shape;
		}
	}
	readOptionalUint(object["apertureBlades"], out.apertureBlades);
	readOptionalFloat(object["apertureRotation"], out.apertureRotationDegrees);
	if (object["apertureImage"].get_string().get(text) == simdjson::SUCCESS && !text.empty()) {
		out.apertureImage = resolvePath(text, sceneDirectory).string();
	}
	readVec2(object["tilt"], out.tiltDegrees);
	readVec2(object["shift"], out.shift);
	out.orthoHeight = std::max(out.orthoHeight, 1e-4f);
	out.apertureBlades = std::clamp(out.apertureBlades, MIN_APERTURE_BLADES, MAX_APERTURE_BLADES);
	out.tiltDegrees = glm::clamp(out.tiltDegrees, glm::vec2(-MAX_CAMERA_TILT), glm::vec2(MAX_CAMERA_TILT));
	//version 16: Relight's gain for each light group, display only
	simdjson::dom::array relight;
	if (object["relight"].get_array().get(relight) == simdjson::SUCCESS) {
		size_t group = 0;
		for (simdjson::dom::element entry : relight) {
			if (group >= out.relight.size()) {
				break;
			}
			readOptionalFloat(entry["scale"], out.relight[group].scale);
			readVec3(entry["tint"], out.relight[group].tint);
			out.relight[group].scale = std::max(out.relight[group].scale, 0.f);
			out.relight[group].tint = glm::max(out.relight[group].tint, glm::vec3(0.f));
			group++;
		}
	}
	out.vfovDegrees = std::clamp(out.vfovDegrees, 1.f, 179.f);
	out.aperture = std::max(out.aperture, 0.f);
	out.focusDistance = std::max(out.focusDistance, 0.01f);
	out.exposure = std::max(out.exposure, 0.f);
	return true;
}

//One "lights" record. The type, the position and a non-negative intensity must be there; the rest
//keeps SceneLight's defaults when missing
bool readLight(simdjson::dom::object object, size_t index, size_t modelCount, const std::filesystem::path& sceneDirectory, SceneLightRecord& out, std::string& error)
{
	std::string_view typeName;
	if (object["type"].get_string().get(typeName) != simdjson::SUCCESS) {
		error = fmt::format("light {} has no \"type\"", index);
		return false;
	}
	std::optional<LightKind> kind;
	for (const LightKind candidate : LIGHT_KINDS) {
		if (typeName == lightKindName(candidate)) {
			kind = candidate;
		}
	}
	if (!kind.has_value()) {
		error = fmt::format("light {} has unknown type \"{}\"", index, typeName);
		return false;
	}
	SceneLight& light = out.light;
	light.kind = *kind;

	std::string_view name;
	if (object["name"].get_string().get(name) == simdjson::SUCCESS) {
		light.name = std::string(name);
	} else {
		light.name = fmt::format("{}_{}", lightKindName(light.kind), index + 1);
	}
	if (!readVec3(object["position"], light.position)) {
		error = fmt::format("light \"{}\" has no valid \"position\"", light.name);
		return false;
	}
	readQuat(object["orientation"], light.orientation);
	readSpectrum(object["color"], light.color);
	if (light.color.kind == SpectrumKind::Rgb) {
		light.color.rgb = glm::max(light.color.rgb, glm::vec3(0.f));
	}
	double intensity = 0.0;
	if (!readDouble(object["intensity"], intensity) || intensity < 0.0) {
		error = fmt::format("light \"{}\" has no valid \"intensity\"", light.name);
		return false;
	}
	light.intensity = (float)intensity;
	readOptionalFloat(object["range"], light.range);
	readOptionalFloat(object["innerCone"], light.innerConeDegrees);
	readOptionalFloat(object["outerCone"], light.outerConeDegrees);
	readOptionalUint(object["lightGroup"], light.lightGroup);
	light.lightGroup = std::min(light.lightGroup, MAX_LIGHT_GROUPS - 1u);
	std::string_view text;
	if (object["falloff"].get_string().get(text) == simdjson::SUCCESS && text == "smoothstep") {
		light.falloff = SpotFalloff::Smoothstep;
	}
	if (object["unit"].get_string().get(text) == simdjson::SUCCESS) {
		for (const LightUnit unit : { LightUnit::Radiometric, LightUnit::Candela, LightUnit::Lumen, LightUnit::Lux }) {
			if (text == lightUnitName(unit)) {
				light.unit = unit;
			}
		}
	}
	if (object["image"].get_string().get(text) == simdjson::SUCCESS && !text.empty()) {
		light.image = resolvePath(text, sceneDirectory).string();
	}
	readOptionalFloat(object["fov"], light.fovDegrees);
	readOptionalBool(object["mirrored"], light.mirrored);
	light.fovDegrees = std::clamp(light.fovDegrees, 1.f, 170.f);
	light.range = std::max(light.range, 0.f);
	light.outerConeDegrees = std::clamp(light.outerConeDegrees, 0.f, 90.f);
	light.innerConeDegrees = std::clamp(light.innerConeDegrees, 0.f, light.outerConeDegrees);

	int64_t model = -1;
	if (object["model"].get_int64().get(model) == simdjson::SUCCESS && model >= 0 && model < (int64_t)modelCount) {
		out.model = (int)model;
	}
	return true;
}

//One "geometry" record, or - `legacySphere` - one record of an older file's "spheres" list, which
//had no "type" and called the position "center". Every size the kind has must be present and
//positive; a missing orientation is the identity
bool readShape(simdjson::dom::object object, bool legacySphere, size_t index, SceneShape& out, std::string& error)
{
	std::optional<ShapeKind> kind;
	if (legacySphere) {
		kind = ShapeKind::Sphere;
	} else {
		std::string_view typeName;
		if (object["type"].get_string().get(typeName) != simdjson::SUCCESS) {
			error = fmt::format("geometry {} has no \"type\"", index);
			return false;
		}
		for (const ShapeKind candidate : SHAPE_KINDS) {
			if (typeName == shapeTraits(candidate).name) {
				kind = candidate;
			}
		}
		if (!kind.has_value()) {
			error = fmt::format("geometry {} has unknown type \"{}\"", index, typeName);
			return false;
		}
	}

	const ShapeTraits& traits = shapeTraits(*kind);
	out = makeShape(*kind);

	std::string_view name;
	if (object["name"].get_string().get(name) == simdjson::SUCCESS) {
		out.name = std::string(name);
	} else {
		out.name = fmt::format("{}_{}", traits.name, index + 1);
	}

	const char* positionKey = legacySphere ? "center" : "position";
	if (!readVec3(object[positionKey], out.position)) {
		error = fmt::format("{} \"{}\" has no valid \"{}\"", traits.name, out.name, positionKey);
		return false;
	}

	if (traits.rotatable) {
		readQuat(object["orientation"], out.orientation);
	}
	{
		bool reversed = false;
		if (object["reverseOrientation"].get_bool().get(reversed) == simdjson::SUCCESS) {
			out.reverseOrientation = reversed;
		}
	}

	for (const ShapeParam& param : traits.params) {
		double value = 0.0;
		if (!readDouble(object[param.name], value) || !(value > 0.0)) {
			error = fmt::format("{} \"{}\" has no valid positive \"{}\"", traits.name, out.name, param.name);
			return false;
		}
		setShapeParam(out, param, (float)value);
	}

	return true;
}

//the parameters of `type` a version-12 record carries, by name, into `params`. One the record lacks
//keeps its default, and one of the wrong kind is skipped; a read-only one is never in a file
//version 25: a material's "textures", which name their images by index into the file's "textures"
void readMaterialTextures(simdjson::dom::object object, std::map<std::string, TextureBinding, std::less<>>& textures, size_t textureCount)
{
	simdjson::dom::object map;
	if (object["textures"].get_object().get(map) != simdjson::SUCCESS) {
		return;
	}
	for (auto [key, value] : map) {
		TextureBinding binding;
		int64_t index = -1;
		if (value["texture"].get_int64().get(index) != simdjson::SUCCESS || index < 0 || index >= (int64_t)textureCount) {
			continue;
		}
		binding.fileTexture = (int)index;
		std::string_view text;
		if (value["channel"].get_string().get(text) == simdjson::SUCCESS) {
			for (size_t i = 0; i < 6; i++) {
				if (text == TEXTURE_CHANNEL_NAMES[i]) {
					binding.channel = (TextureChannel)i;
				}
			}
		}
		if (value["mapping"].get_string().get(text) == simdjson::SUCCESS) {
			for (size_t i = 0; i < 4; i++) {
				if (text == TEXTURE_MAPPING_NAMES[i]) {
					binding.mapping = (TextureMapping)i;
				}
			}
		}
		readOptionalUint(value["uvSet"], binding.uvSet);
		binding.uvSet = std::min(binding.uvSet, 1u);
		simdjson::dom::array pair;
		if (value["offset"].get_array().get(pair) == simdjson::SUCCESS && pair.size() == 2) {
			binding.offset = glm::vec2((float)pair.at(0).get_double().value_unsafe(), (float)pair.at(1).get_double().value_unsafe());
		}
		if (value["scale"].get_array().get(pair) == simdjson::SUCCESS && pair.size() == 2) {
			binding.scale = glm::vec2((float)pair.at(0).get_double().value_unsafe(), (float)pair.at(1).get_double().value_unsafe());
		}
		readOptionalFloat(value["rotation"], binding.rotation);
		simdjson::dom::array transform;
		if (value["transform"].get_array().get(transform) == simdjson::SUCCESS && transform.size() == 16) {
			for (size_t i = 0; i < 16; i++) {
				double v = 0.0;
				if (transform.at(i).get_double().get(v) == simdjson::SUCCESS) {
					binding.mappingTransform[(int)(i / 4)][(int)(i % 4)] = (float)v;
				}
			}
		}
		readOptionalBool(value["worldSpace"], binding.worldSpace);
		if (value["alphaMode"].get_string().get(text) == simdjson::SUCCESS && text == "stochastic") {
			binding.alphaMode = AlphaMode::Stochastic;
		}
		textures[std::string(key)] = binding;
	}
}

void readMaterialParams(simdjson::dom::object object, MaterialType type, ParamSet& params)
{
	for (const MaterialParam& param : materialTraits(type).params) {
		if ((param.flags & PARAM_READ_ONLY) != 0) {
			continue;
		}
		simdjson::dom::element element;
		if (object[param.name].get(element) != simdjson::SUCCESS) {
			continue;
		}
		switch (param.kind) {
		case ParamKind::Float: {
			double value = 0.0;
			if (readDouble(element, value)) {
				params[param.name] = (float)value;
			}
			break;
		}
		case ParamKind::Spectrum: {
			SpectrumRef value;
			if (readSpectrum(object[param.name], value)) {
				params[param.name] = value;
			}
			break;
		}
		case ParamKind::Enum: {
			int64_t value = 0;
			if (element.get_int64().get(value) == simdjson::SUCCESS) {
				params[param.name] = (int)value;
			}
			break;
		}
		case ParamKind::Bool: {
			bool value = false;
			if (element.get_bool().get(value) == simdjson::SUCCESS) {
				params[param.name] = value;
			}
			break;
		}
		case ParamKind::Material: {
			//a reference as a shape's material is; the loader turns it into a library id once every material is in
			MaterialLink link;
			int64_t user = -1;
			int64_t model = -1;
			uint64_t index = 0;
			if (element.get_int64().get(user) == simdjson::SUCCESS) {
				link.fileUser = (int)user;
				params[param.name] = link;
			} else if (element["model"].get_int64().get(model) == simdjson::SUCCESS && element["material"].get_uint64().get(index) == simdjson::SUCCESS) {
				link.fileModel = (int)model;
				link.fileModelMaterial = (uint32_t)index;
				params[param.name] = link;
			}
			break;
		}
		case ParamKind::Coating: {
			std::string_view key;
			if (element.get_string().get(key) == simdjson::SUCCESS) {
				params[param.name] = CoatingRef { std::string(key) };
			}
			break;
		}
		case ParamKind::Texture:
			break;
		}
	}
}

bool readDoubles(simdjson::dom::element element, std::vector<double>& out)
{
	simdjson::dom::array array;
	if (element.get_array().get(array) != simdjson::SUCCESS) {
		return false;
	}
	out.clear();
	for (simdjson::dom::element value : array) {
		double d = 0.0;
		if (!readDouble(value, d)) {
			return false;
		}
		out.push_back(d);
	}
	return true;
}

bool readDoubles(JsonElement element, std::vector<double>& out)
{
	simdjson::dom::element value;
	return element.get(value) == simdjson::SUCCESS && readDoubles(value, out);
}

void readOptionalDouble(JsonElement element, double& out)
{
	simdjson::dom::element value;
	if (element.get(value) == simdjson::SUCCESS) {
		readDouble(value, out);
	}
}

std::string readOptionalString(JsonElement element)
{
	std::string_view text;
	return element.get_string().get(text) == simdjson::SUCCESS ? std::string(text) : std::string();
}

//one user glass of "opticalLibrary"
bool readGlass(simdjson::dom::object object, OpticGlass& out)
{
	out.name = readOptionalString(object["name"]);
	if (out.name.empty() || !readSpectrum(object["index"], out.index)) {
		return false;
	}
	std::vector<double> range;
	if (readDoubles(object["range"], range) && range.size() == 2) {
		out.range = glm::dvec2(range[0], range[1]);
	}
	readOptionalDouble(object["nd"], out.catalogNd);
	readOptionalDouble(object["vd"], out.catalogVd);
	simdjson::dom::array lines;
	if (object["transmittance"].get_array().get(lines) == simdjson::SUCCESS) {
		for (simdjson::dom::element line : lines) {
			std::vector<double> values;
			if (readDoubles(line, values) && values.size() == 3) {
				out.transmittance.emplace_back(values[0], values[1], values[2]);
			}
		}
	}
	out.absorption = absorptionFromTransmittance(out.transmittance);
	out.thermal = readOptionalString(object["thermal"]);
	bool absolute = false;
	readOptionalBool(object["absolute"], absolute);
	out.relativeToAir = !absolute;
	return true;
}

//one user coating of "opticalLibrary"
bool readCoating(simdjson::dom::object object, OpticCoating& out)
{
	out.key = readOptionalString(object["key"]);
	const std::optional<CoatingKind> kind = findCoatingKind(readOptionalString(object["kind"]));
	if (out.key.empty() || !kind) {
		return false;
	}
	out.kind = *kind;
	out.name = readOptionalString(object["name"]);
	out.source = readOptionalString(object["source"]);
	readOptionalBool(object["polarizing"], out.polarizing);
	readOptionalDouble(object["rs"], out.rs);
	readOptionalDouble(object["rp"], out.rp);
	readOptionalDouble(object["ts"], out.ts);
	readOptionalDouble(object["tp"], out.tp);
	readOptionalDouble(object["designWavelength"], out.designWavelength);
	out.metal = readOptionalString(object["metal"]);
	simdjson::dom::array layers;
	if (object["layers"].get_array().get(layers) == simdjson::SUCCESS) {
		for (simdjson::dom::element entry : layers) {
			simdjson::dom::object layerObject;
			if (entry.get_object().get(layerObject) != simdjson::SUCCESS) {
				continue;
			}
			CoatingLayer layer;
			layer.material = readOptionalString(layerObject["material"]);
			readOptionalDouble(layerObject["thickness"], layer.thickness);
			readOptionalDouble(layerObject["quarterWaves"], layer.quarterWaves);
			out.layers.push_back(std::move(layer));
		}
	}
	simdjson::dom::array curves;
	if (object["curves"].get_array().get(curves) == simdjson::SUCCESS) {
		for (simdjson::dom::element entry : curves) {
			simdjson::dom::object curveObject;
			if (entry.get_object().get(curveObject) != simdjson::SUCCESS) {
				continue;
			}
			CoatingCurve curve;
			readOptionalDouble(curveObject["angle"], curve.angle);
			readDoubles(curveObject["lambda"], curve.lambda);
			readDoubles(curveObject["rs"], curve.rs);
			readDoubles(curveObject["rp"], curve.rp);
			readDoubles(curveObject["ts"], curve.ts);
			readDoubles(curveObject["tp"], curve.tp);
			out.curves.push_back(std::move(curve));
		}
	}
	return true;
}

bool readMaterialDef(simdjson::dom::object object, size_t index, MaterialDef& out, std::string& error, size_t textureCount);

//a free optic's "position" and "orientation", each the default's where absent
void readFreePlacement(simdjson::dom::object object, MountPose& mount)
{
	std::vector<double> values;
	if (readDoubles(object["position"], values) && values.size() == 3) {
		mount.freePosition = glm::dvec3(values[0], values[1], values[2]);
	}
	values.clear();
	if (readDoubles(object["orientation"], values) && values.size() == 4) {
		const glm::dquat q(values[3], values[0], values[1], values[2]);
		const double length = glm::length(q);
		mount.freeOrientation = length > 0.0 ? (std::abs(length - 1.0) > 1e-12 ? q / length : q) : glm::dquat(1.0, 0.0, 0.0, 0.0);
	}
}

//an optic's "mount" (version 23): a kind this build does not know is unmounted, with a warning
void readMount(simdjson::dom::object object, SceneOptic& optic, std::vector<std::string>& warnings)
{
	MountPose& mount = optic.mount;
	mount = MountPose {};
	const std::string key = readOptionalString(object["kind"]);
	const std::optional<MountKind> kind = findMountKind(key);
	if (!kind) {
		warnings.push_back(fmt::format("optic \"{}\" has mount kind '{}', which this build does not know; it is unmounted", optic.name, key));
	}
	mount.kind = kind.value_or(MountKind::None);
	bool free = false;
	if (object["free"].get_bool().get(free) != simdjson::SUCCESS) {
		free = false;
	}
	mount.freePlacement = free && mount.kind != MountKind::None;
	readFreePlacement(object, mount);
	if (mount.kind == MountKind::None) {
		return;
	}
	std::vector<double> origin;
	if (readDoubles(object["origin"], origin) && origin.size() == 2) {
		mount.origin = glm::dvec2(origin[0], origin[1]);
	}
	readDouble(object["yaw"], mount.yaw);
	readDouble(object["height"], mount.height);
	simdjson::dom::object fork;
	if (object["fork"].get_object().get(fork) == simdjson::SUCCESS) {
		readDouble(fork["angle"], mount.forkAngle);
		readDouble(fork["reach"], mount.forkReach);
	}
	simdjson::dom::object fine;
	if (object["fine"].get_object().get(fine) == simdjson::SUCCESS) {
		readDouble(fine["x"], mount.fine.adjusterX);
		readDouble(fine["y"], mount.fine.adjusterY);
		readDouble(fine["rotor"], mount.fine.rotor);
	}
}

//one "optics" record. An unknown type is skipped with a warning, as an unknown kernel id is
bool readOptic(simdjson::dom::object object, size_t index, SceneOpticRecord& out, std::vector<std::string>& warnings)
{
	const std::string typeName = readOptionalString(object["type"]);
	const std::optional<OpticKind> kind = findOpticKind(typeName);
	if (!kind) {
		warnings.push_back(fmt::format("optic {} has type '{}', which this build does not know; it is skipped", index, typeName));
		return false;
	}
	SceneOptic& optic = out.optic;
	optic.kind = *kind;
	const OpticTraits& traits = opticTraits(optic.kind);
	optic.name = readOptionalString(object["name"]);
	if (optic.name.empty()) {
		optic.name = fmt::format("{}_{}", traits.name, index + 1);
	}
	const std::string size = readOptionalString(object["size"]);
	optic.sizePreset = -1;
	for (int i = 0; i < (int)traits.sizes.size(); i++) {
		if (size == traits.sizes[(size_t)i].key) {
			optic.sizePreset = i;
		}
	}
	simdjson::dom::object params;
	if (object["params"].get_object().get(params) == simdjson::SUCCESS) {
		for (const OpticParam& param : traits.params) {
			simdjson::dom::element element;
			if (params[param.name].get(element) != simdjson::SUCCESS) {
				continue;
			}
			double d = 0.0;
			bool b = false;
			std::string_view text;
			switch (param.kind) {
			case OpticParamKind::Length:
			case OpticParamKind::Angle:
			case OpticParamKind::Number:
			case OpticParamKind::Wavelength:
				if (readDouble(element, d)) {
					optic.params[param.name] = d;
				}
				break;
			case OpticParamKind::Enum:
				if (element.get_string().get(text) == simdjson::SUCCESS) {
					for (size_t key = 0; key < param.keys.size(); key++) {
						if (text == param.keys[key]) {
							optic.params[param.name] = (int)key;
						}
					}
				} else if (readDouble(element, d)) {
					optic.params[param.name] = (int)d;
				}
				break;
			case OpticParamKind::Bool:
				if (element.get_bool().get(b) == simdjson::SUCCESS) {
					optic.params[param.name] = b;
				}
				break;
			case OpticParamKind::Glass:
				if (element.get_string().get(text) == simdjson::SUCCESS) {
					optic.params[param.name] = std::string(text);
				}
				break;
			}
		}
	}
	// a lens saved before its origin was a setting (version 30 and earlier) was placed by the middle between its vertices;
	// it keeps that point, now as its optical center, once the mount below is read
	const bool legacyLens = optic.kind == OpticKind::SphericalLens && optic.params.find("originAt") == optic.params.end();
	if (legacyLens) {
		optic.params["originAt"] = 2;
	}
	//version 23: the mount; an older file's optic is unmounted where the file put it
	simdjson::dom::object mount;
	if (object["mount"].get_object().get(mount) == simdjson::SUCCESS) {
		readMount(mount, optic, warnings);
		//version 30: a mounted optic placed by its body rather than its stack
		simdjson::dom::element element;
		out.standOnBench = !optic.mount.isFree() && mount["origin"].get(element) != simdjson::SUCCESS && mount["position"].get(element) == simdjson::SUCCESS;
	} else {
		optic.mount = MountPose {};
		readFreePlacement(object, optic.mount);
	}
	// a free legacy lens's position was its body frame's: the origin the file meant is the middle of its thickness along the
	// axis, which is where the back vertex is halved
	if (legacyLens && (optic.mount.isFree() || out.standOnBench)) {
		optic.mount.freePosition += optic.mount.freeOrientation * glm::dvec3(0.0, 0.0, 0.5 * opticNumber(optic, "centerThickness"));
	}
	optic.world = optic.mount.freeTransform();
	simdjson::dom::object materials;
	if (object["materials"].get_object().get(materials) == simdjson::SUCCESS) {
		for (size_t slot = 0; slot < traits.slots.size(); slot++) {
			simdjson::dom::object materialObject;
			if (materials[traits.slots[slot].name].get_object().get(materialObject) != simdjson::SUCCESS) {
				continue;
			}
			MaterialDef material;
			std::string error;
			if (readMaterialDef(materialObject, slot, material, error, 0)) {
				out.slots[slot] = std::move(material);
			} else {
				warnings.push_back(fmt::format("optic \"{}\" slot {}: {}", optic.name, traits.slots[slot].name, error));
			}
		}
	}
	return true;
}

//one "materials" record: a user material
bool readMaterialDef(simdjson::dom::object object, size_t index, MaterialDef& out, std::string& error, size_t textureCount)
{
	std::string_view name;
	out.name = object["name"].get_string().get(name) == simdjson::SUCCESS ? std::string(name) : fmt::format("material_{}", index + 1);
	std::string_view typeName;
	if (object["type"].get_string().get(typeName) != simdjson::SUCCESS) {
		error = fmt::format("material \"{}\" has no \"type\"", out.name);
		return false;
	}
	const std::optional<MaterialType> type = findMaterialType(typeName);
	if (!type.has_value()) {
		error = fmt::format("material \"{}\" has unknown type \"{}\"", out.name, typeName);
		return false;
	}
	out.type = *type;
	readMaterialParams(object, out.type, out.params);
	readMaterialTextures(object, out.textures, textureCount);
	readOptionalUint(object["lightGroup"], out.lightGroup);
	out.lightGroup = std::min(out.lightGroup, MAX_LIGHT_GROUPS - 1u);
	return true;
}

//a version-12 material reference: an index into "materials", or {"model":m,"material":i}. One that
//names nothing the file has reads as unset, and the loader gives its object a default material
SceneMaterialRef readMaterialRef(JsonElement element, size_t materialCount, size_t modelCount)
{
	SceneMaterialRef ref;
	int64_t user = -1;
	if (element.get_int64().get(user) == simdjson::SUCCESS) {
		if (user >= 0 && user < (int64_t)materialCount) {
			ref.user = (int)user;
		}
		return ref;
	}
	int64_t model = -1;
	uint64_t index = 0;
	if (element["model"].get_int64().get(model) == simdjson::SUCCESS && element["material"].get_uint64().get(index) == simdjson::SUCCESS
		&& model >= 0 && model < (int64_t)modelCount) {
		ref.model = (int)model;
		ref.modelMaterial = (uint32_t)index;
	}
	return ref;
}

//An object's material in either format: a reference from version 12, or an older file's inline
//material, which becomes a user material named after the object
bool readObjectMaterial(JsonElement element, int64_t version, const std::string& ownerName, SceneDescription& scene, SceneMaterialRef& out, std::string& error)
{
	if (version >= 12) {
		out = readMaterialRef(element, scene.materials.size(), scene.modelPaths.size());
		return true;
	}
	MaterialDef material;
	if (!readLegacyMaterial(element, material, error)) {
		return false;
	}
	material.name = ownerName;
	out.user = (int)scene.materials.size();
	scene.materials.push_back(std::move(material));
	return true;
}

struct ParseContext {
	std::filesystem::path sceneDirectory;
	SceneDescription scene;
	bool found { false };
	std::string error;
};

std::filesystem::path resolvePath(std::string_view text, const std::filesystem::path& sceneDirectory)
{
	std::filesystem::path path { std::string(text) };
	if (path.is_relative()) {
		path = sceneDirectory / path;
	}
	return path.lexically_normal();
}

//fires from inside the parse, once per entity that has extras. The simdjson object is only valid
//for the duration of the call, so the shapes are built here rather than after loadGltf returns
void parseSceneExtras(simdjson::dom::object* extras, std::size_t objectIndex, fastgltf::Category category, void* userPointer)
{
	if (category != fastgltf::Category::Scenes || extras == nullptr) {
		return;
	}

	ParseContext& ctx = *static_cast<ParseContext*>(userPointer);
	if (ctx.found) {
		//the first scene carrying scene data wins
		return;
	}

	simdjson::dom::object& root = *extras;

	//version 6 on keeps its shapes in "geometry"; every earlier version had "spheres"
	simdjson::dom::array geometry;
	const bool legacySpheres = root["geometry"].get_array().get(geometry) != simdjson::SUCCESS;
	if (legacySpheres && root["spheres"].get_array().get(geometry) != simdjson::SUCCESS) {
		//extras of some other kind - an ordinary glTF file's own application data
		return;
	}
	ctx.found = true;

	int64_t version = 0;
	if (root["version"].get_int64().get(version) != simdjson::SUCCESS) {
		ctx.error = "scene has no \"version\" field";
		return;
	}
	if (version > SCENE_FILE_FORMAT_VERSION) {
		ctx.error = fmt::format("scene file format version {} is newer than the version {} this build understands", version, SCENE_FILE_FORMAT_VERSION);
		return;
	}

	//version 20: the capabilities by key (the engine sorts out which it knows), the template, a template file's info
	simdjson::dom::array capabilityKeys;
	if (root["capabilities"].get_array().get(capabilityKeys) == simdjson::SUCCESS) {
		for (simdjson::dom::element entry : capabilityKeys) {
			std::string_view key;
			if (entry.get_string().get(key) == simdjson::SUCCESS && !key.empty()) {
				ctx.scene.capabilities.emplace_back(key);
			}
		}
	}
	std::string_view templateKey;
	if (root["template"].get_string().get(templateKey) == simdjson::SUCCESS) {
		ctx.scene.templateKey = std::string(templateKey);
	}
	simdjson::dom::object templateInfo;
	if (root["templateInfo"].get_object().get(templateInfo) == simdjson::SUCCESS) {
		SceneTemplateInfo info;
		std::string_view text;
		if (templateInfo["name"].get_string().get(text) == simdjson::SUCCESS) {
			info.name = std::string(text);
		}
		if (templateInfo["description"].get_string().get(text) == simdjson::SUCCESS) {
			info.description = std::string(text);
		}
		ctx.scene.templateInfo = std::move(info);
	}

	//version 1 had spheres only, so both of these are optional
	simdjson::dom::array models;
	if (root["models"].get_array().get(models) == simdjson::SUCCESS) {
		size_t modelIndex = 0;
		for (simdjson::dom::element entry : models) {
			std::string_view modelPath;
			if (entry["path"].get_string().get(modelPath) != simdjson::SUCCESS || modelPath.empty()) {
				ctx.error = fmt::format("model {} has no \"path\"", modelIndex);
				return;
			}
			ctx.scene.modelPaths.push_back(resolvePath(modelPath, ctx.sceneDirectory));
			modelIndex++;
		}
	}

	std::string_view environmentMap;
	if (root["environmentMap"].get_string().get(environmentMap) == simdjson::SUCCESS && !environmentMap.empty()) {
		ctx.scene.environmentMapPath = resolvePath(environmentMap, ctx.sceneDirectory);
	}

	int64_t environmentModel = -1;
	if (root["environmentModel"].get_int64().get(environmentModel) == simdjson::SUCCESS && environmentModel >= 0
		&& environmentModel < (int64_t)ctx.scene.modelPaths.size()) {
		ctx.scene.environmentModel = (int)environmentModel;
	}

	//version 3: both optional, defaults otherwise
	readOptionalFloat(root["environmentIntensity"], ctx.scene.environmentIntensity);
	ctx.scene.environmentIntensity = std::max(ctx.scene.environmentIntensity, 0.f);
	//version 24: absent for an unturned map
	readOptionalFloat(root["environmentRotation"], ctx.scene.environmentRotation);
	//into -180 to 180, the panel's range
	ctx.scene.environmentRotation = std::remainder(ctx.scene.environmentRotation, 360.f);
	readRenderSettings(root["render"], ctx.scene.render);

	//version 7: the solid color, optional. Version 24: the mode, which an older file implied (SCENE_FILE_FORMAT_VERSION)
	glm::vec3 background;
	const bool hasBackground = readVec3(root["background"], background);
	if (hasBackground) {
		ctx.scene.backgroundColor = glm::max(background, glm::vec3(0.f));
	}
	if (version >= 24) {
		std::string_view mode;
		if (root["backgroundMode"].get_string().get(mode) == simdjson::SUCCESS) {
			for (const BackgroundMode candidate : BACKGROUND_MODES) {
				if (mode == backgroundModeName(candidate)) {
					ctx.scene.backgroundMode = candidate;
				}
			}
		}
	} else if (hasBackground) {
		ctx.scene.backgroundMode = BackgroundMode::Solid;
	} else if (!ctx.scene.environmentMapPath.empty() || ctx.scene.environmentModel >= 0) {
		ctx.scene.backgroundMode = BackgroundMode::Environment;
	} else {
		ctx.scene.backgroundMode = BackgroundMode::Sky;
	}

	//version 8: the kernel selection and the BVH settings. Both optional; an older file simply
	//keeps the defaults SceneDescription was constructed with
	readKernelSelection(root, version, ctx.scene);
	readAccelSettings(root["accel"], ctx.scene.accel);

	//version 16: the light groups' names and the background's group, and the export settings. A file without them
	//has the one group, and exports as a new scene does
	simdjson::dom::array groupNames;
	if (root["lightGroupNames"].get_array().get(groupNames) == simdjson::SUCCESS) {
		std::vector<std::string> names;
		for (simdjson::dom::element entry : groupNames) {
			std::string_view groupName;
			if (names.size() < MAX_LIGHT_GROUPS && entry.get_string().get(groupName) == simdjson::SUCCESS) {
				names.push_back(std::string(groupName));
			}
		}
		if (!names.empty()) {
			ctx.scene.lightGroupNames = std::move(names);
		}
	}
	readOptionalUint(root["environmentGroup"], ctx.scene.environmentGroup);
	ctx.scene.environmentGroup = std::min(ctx.scene.environmentGroup, (uint32_t)ctx.scene.lightGroupNames.size() - 1u);
	simdjson::dom::object displayObject;
	if (root["spectralDisplay"].get_object().get(displayObject) == simdjson::SUCCESS) {
		SpectralDisplay& d = ctx.scene.spectralDisplay;
		std::string_view text;
		if (displayObject["mode"].get_string().get(text) == simdjson::SUCCESS) {
			for (uint32_t i = 0; i < (uint32_t)MonochromeDisplay::Count; i++) {
				if (text == monochromeDisplayKey((MonochromeDisplay)i)) {
					d.mode = (MonochromeDisplay)i;
				}
			}
		}
		if (displayObject["colormap"].get_string().get(text) == simdjson::SUCCESS) {
			for (uint32_t i = 0; i < (uint32_t)Colormap::Count; i++) {
				if (text == colormapKey((Colormap)i)) {
					d.colormap = (Colormap)i;
				}
			}
		}
		readOptionalBool(displayObject["logarithmic"], d.logarithmic);
		readOptionalFloat(displayObject["decades"], d.decades);
		readVec3(displayObject["infrared"], d.infraredColor);
		readVec3(displayObject["ultraviolet"], d.ultravioletColor);
		readVec3(displayObject["xuv"], d.xuvColor);
	}
	simdjson::dom::object exportObject;
	if (root["export"].get_object().get(exportObject) == simdjson::SUCCESS) {
		readOptionalBool(exportObject["half"], ctx.scene.exportSettings.halfFloat);
		readOptionalBool(exportObject["saveWhenFinished"], ctx.scene.exportSettings.saveWhenFinished);
		std::string_view exportPath;
		if (exportObject["path"].get_string().get(exportPath) == simdjson::SUCCESS && !exportPath.empty()) {
			ctx.scene.exportSettings.saveWhenFinishedPath = resolvePath(exportPath, ctx.sceneDirectory).string();
		}
	}

	//version 4: cameras. An older file gets one default camera, carrying the lens settings its
	//"render" block had
	simdjson::dom::array cameras;
	if (root["cameras"].get_array().get(cameras) == simdjson::SUCCESS) {
		size_t cameraIndex = 0;
		for (simdjson::dom::element entry : cameras) {
			simdjson::dom::object cameraObject;
			if (entry.get_object().get(cameraObject) != simdjson::SUCCESS) {
				ctx.error = fmt::format("camera {} is not an object", cameraIndex);
				return;
			}
			SceneCamera camera;
			std::string cameraError;
			if (!readCamera(cameraObject, ctx.sceneDirectory, camera, cameraError, ctx.scene.kernelWarnings)) {
				ctx.error = fmt::format("camera {}: {}", cameraIndex, cameraError);
				return;
			}
			ctx.scene.cameras.push_back(std::move(camera));
			cameraIndex++;
		}
		int64_t renderCamera = -1;
		if (root["renderCamera"].get_int64().get(renderCamera) == simdjson::SUCCESS && renderCamera >= 0 && renderCamera < (int64_t)ctx.scene.cameras.size()) {
			ctx.scene.renderCamera = (int)renderCamera;
		}
		int64_t renderDetector = -1;
		if (root["renderDetector"].get_int64().get(renderDetector) == simdjson::SUCCESS && renderDetector >= 0) {
			ctx.scene.renderDetector = (int)renderDetector;
		}
	} else {
		SceneCamera camera;
		camera.name = "render_camera";
		camera.lensMapping = LensMapping::Polar;
		readLegacyCameraSettings(root["render"], camera);
		ctx.scene.cameras.push_back(std::move(camera));
		ctx.scene.renderCamera = 0;
	}

	//version 10: the punctual lights. Absent in an older file, which then keeps the lights its
	//models' imports recreate
	simdjson::dom::array lights;
	if (root["lights"].get_array().get(lights) == simdjson::SUCCESS) {
		ctx.scene.hasLights = true;
		size_t lightIndex = 0;
		for (simdjson::dom::element entry : lights) {
			simdjson::dom::object lightObject;
			if (entry.get_object().get(lightObject) != simdjson::SUCCESS) {
				ctx.error = fmt::format("light {} is not an object", lightIndex);
				return;
			}
			SceneLightRecord record;
			if (!readLight(lightObject, lightIndex, ctx.scene.modelPaths.size(), ctx.sceneDirectory, record, ctx.error)) {
				return;
			}
			ctx.scene.lights.push_back(std::move(record));
			lightIndex++;
		}
	}

	//version 25: the image files the materials' textures read, before the materials
	simdjson::dom::array textureList;
	if (root["textures"].get_array().get(textureList) == simdjson::SUCCESS) {
		for (simdjson::dom::element entry : textureList) {
			SceneTextureRecord texture;
			std::string_view text;
			if (entry["path"].get_string().get(text) == simdjson::SUCCESS) {
				texture.path = resolvePath(text, ctx.sceneDirectory);
			}
			if (entry["colorSpace"].get_string().get(text) == simdjson::SUCCESS && text == "linear") {
				texture.colorSpace = TextureColorSpace::Linear;
			}
			simdjson::dom::array wrap;
			if (entry["wrap"].get_array().get(wrap) == simdjson::SUCCESS && wrap.size() == 2) {
				for (size_t axis = 0; axis < 2; axis++) {
					std::string_view mode;
					if (wrap.at(axis).get_string().get(mode) == simdjson::SUCCESS) {
						for (size_t i = 0; i < 4; i++) {
							if (mode == TEXTURE_WRAP_NAMES[i]) {
								(axis == 0 ? texture.sampling.wrapU : texture.sampling.wrapV) = (TextureWrap)i;
							}
						}
					}
				}
			}
			simdjson::dom::array nearest;
			if (entry["nearest"].get_array().get(nearest) == simdjson::SUCCESS && nearest.size() == 2) {
				bool mag = false;
				bool min = false;
				(void)nearest.at(0).get_bool().get(mag);
				(void)nearest.at(1).get_bool().get(min);
				texture.sampling.nearestMag = mag;
				texture.sampling.nearestMin = min;
			}
			if (entry["encoding"].get_string().get(text) == simdjson::SUCCESS) {
				texture.encoding = text == "linear" ? TextureEncoding::Linear : text == "gamma" ? TextureEncoding::Gamma : TextureEncoding::Srgb;
			}
			readOptionalFloat(entry["gamma"], texture.gamma);
			readOptionalBool(entry["invert"], texture.invert);
			ctx.scene.textures.push_back(std::move(texture));
		}
	}

	//version 12: the library, before the objects that reference it
	simdjson::dom::array materials;
	if (version >= 12 && root["materials"].get_array().get(materials) == simdjson::SUCCESS) {
		size_t materialIndex = 0;
		for (simdjson::dom::element entry : materials) {
			simdjson::dom::object materialObject;
			if (entry.get_object().get(materialObject) != simdjson::SUCCESS) {
				ctx.error = fmt::format("material {} is not an object", materialIndex);
				return;
			}
			MaterialDef material;
			if (!readMaterialDef(materialObject, materialIndex, material, ctx.error, ctx.scene.textures.size())) {
				return;
			}
			ctx.scene.materials.push_back(std::move(material));
			materialIndex++;
		}
	}
	simdjson::dom::array overrides;
	if (version >= 12 && root["materialOverrides"].get_array().get(overrides) == simdjson::SUCCESS) {
		for (simdjson::dom::element entry : overrides) {
			simdjson::dom::object overrideObject;
			int64_t model = -1;
			uint64_t modelMaterial = 0;
			if (entry.get_object().get(overrideObject) != simdjson::SUCCESS || overrideObject["model"].get_int64().get(model) != simdjson::SUCCESS
				|| overrideObject["material"].get_uint64().get(modelMaterial) != simdjson::SUCCESS || model < 0 || model >= (int64_t)ctx.scene.modelPaths.size()) {
				//an override of a model the file does not list has nothing to apply to
				continue;
			}
			SceneMaterialOverride edit;
			edit.model = (int)model;
			edit.modelMaterial = (uint32_t)modelMaterial;
			readMaterialParams(overrideObject, MaterialType::Pbr, edit.params);
			readMaterialTextures(overrideObject, edit.textures, ctx.scene.textures.size());
			readOptionalUint(overrideObject["lightGroup"], edit.lightGroup);
			edit.lightGroup = std::min(edit.lightGroup, MAX_LIGHT_GROUPS - 1u);
			ctx.scene.materialOverrides.push_back(std::move(edit));
		}
	}

	//version 22: the viewport's free camera, kept only with a valid pose
	simdjson::dom::object viewportObject;
	if (root["viewport"].get_object().get(viewportObject) == simdjson::SUCCESS) {
		SceneViewportRecord viewport;
		if (readVec3(viewportObject["position"], viewport.position) && readQuat(viewportObject["orientation"], viewport.orientation)) {
			readVec3(viewportObject["pivot"], viewport.pivot);
			ViewportCameraSettings& settings = viewport.settings;
			readOptionalFloat(viewportObject["speed"], settings.speed);
			readOptionalFloat(viewportObject["fov"], settings.vfovDegrees);
			readOptionalFloat(viewportObject["near"], settings.nearClip);
			readOptionalFloat(viewportObject["far"], settings.farClip);
			settings = settings.clamped();
			ctx.scene.viewport = viewport;
		}
	}

	//version 21: the length unit, the optical library and the optical elements
	if (const std::optional<LengthUnit> unit = findLengthUnit(readOptionalString(root["lengthUnit"]))) {
		ctx.scene.lengthUnit = *unit;
	}
	simdjson::dom::object library;
	if (root["opticalLibrary"].get_object().get(library) == simdjson::SUCCESS) {
		simdjson::dom::array catalogs;
		if (library["catalogs"].get_array().get(catalogs) == simdjson::SUCCESS) {
			for (simdjson::dom::element entry : catalogs) {
				std::string_view path;
				if (entry.get_string().get(path) == simdjson::SUCCESS) {
					ctx.scene.glassCatalogs.push_back(resolvePath(path, ctx.sceneDirectory));
				}
			}
		}
		simdjson::dom::array glasses;
		if (library["glasses"].get_array().get(glasses) == simdjson::SUCCESS) {
			for (simdjson::dom::element entry : glasses) {
				simdjson::dom::object glassObject;
				OpticGlass glass;
				if (entry.get_object().get(glassObject) == simdjson::SUCCESS && readGlass(glassObject, glass)) {
					ctx.scene.userGlasses.push_back(std::move(glass));
				}
			}
		}
		simdjson::dom::array coatings;
		if (library["coatings"].get_array().get(coatings) == simdjson::SUCCESS) {
			for (simdjson::dom::element entry : coatings) {
				simdjson::dom::object coatingObject;
				OpticCoating coating;
				if (entry.get_object().get(coatingObject) == simdjson::SUCCESS && readCoating(coatingObject, coating)) {
					ctx.scene.userCoatings.push_back(std::move(coating));
				}
			}
		}
	}
	//version 23: the bench
	simdjson::dom::object bench;
	if (root["bench"].get_object().get(bench) == simdjson::SUCCESS) {
		BenchSettings& out = ctx.scene.bench;
		if (bench["table"].get_bool().get(out.table) != simdjson::SUCCESS) {
			out.table = false;
		}
		std::vector<double> size;
		if (readDoubles(bench["size"], size) && size.size() == 2) {
			out.size = snapBenchSize(glm::dvec2(size[0], size[1]));
		}
		readDouble(bench["beamHeight"], out.beamHeight);
		if (bench["snap"].get_bool().get(out.snap) != simdjson::SUCCESS) {
			out.snap = true;
		}
	}
	simdjson::dom::array optics;
	if (root["optics"].get_array().get(optics) == simdjson::SUCCESS) {
		size_t opticIndex = 0;
		for (simdjson::dom::element entry : optics) {
			simdjson::dom::object opticObject;
			SceneOpticRecord record;
			if (entry.get_object().get(opticObject) == simdjson::SUCCESS && readOptic(opticObject, opticIndex, record, ctx.scene.kernelWarnings)) {
				ctx.scene.optics.push_back(std::move(record));
			}
			opticIndex++;
		}
	}
	//version 30: the sensor stacks, each sensor read as an optic is
	simdjson::dom::array stacks;
	if (root["sensorStacks"].get_array().get(stacks) == simdjson::SUCCESS) {
		size_t stackIndex = 0;
		for (simdjson::dom::element entry : stacks) {
			simdjson::dom::object stackObject;
			simdjson::dom::object sensorObject;
			SceneOpticRecord sensor;
			if (entry.get_object().get(stackObject) != simdjson::SUCCESS || stackObject["sensor"].get_object().get(sensorObject) != simdjson::SUCCESS
				|| !readOptic(sensorObject, stackIndex, sensor, ctx.scene.kernelWarnings)) {
				stackIndex++;
				continue;
			}
			SceneSensorStackRecord record;
			SensorStack& stack = record.stack;
			stack.name = readOptionalString(stackObject["name"]);
			if (!isDetectorKind(sensor.optic.kind)) {
				ctx.scene.kernelWarnings.push_back(fmt::format("sensor stack \"{}\" has a {}, which is no detector; it is skipped", stack.name, opticTraits(sensor.optic.kind).name));
				stackIndex++;
				continue;
			}
			stack.sensor = std::move(sensor.optic);
			stack.sensor.name.clear();
			stack.sensor.mount = MountPose {};
			record.slots = sensor.slots;
			std::vector<double> values;
			if (readDoubles(stackObject["start"], values) && values.size() == 3) {
				stack.start = glm::dvec3(values[0], values[1], values[2]);
			}
			values.clear();
			if (readDoubles(stackObject["end"], values) && values.size() == 3) {
				stack.end = glm::dvec3(values[0], values[1], values[2]);
			}
			values.clear();
			if (readDoubles(stackObject["orientation"], values) && values.size() == 4) {
				const glm::dquat q(values[3], values[0], values[1], values[2]);
				stack.orientation = glm::length(q) > 0.0 ? glm::normalize(q) : glm::dquat(1.0, 0.0, 0.0, 0.0);
			}
			double count = (double)stack.count;
			readDouble(stackObject["count"], count);
			stack.count = std::clamp((int)std::lround(count), 1, MAX_STACK_SENSORS);
			if (stackObject["alignToAxis"].get_bool().get(stack.alignToAxis) != simdjson::SUCCESS) {
				stack.alignToAxis = true;
			}
			ctx.scene.sensorStacks.push_back(std::move(record));
			stackIndex++;
		}
	}

	size_t index = 0;
	for (simdjson::dom::element entry : geometry) {
		simdjson::dom::object shapeObject;
		if (entry.get_object().get(shapeObject) != simdjson::SUCCESS) {
			ctx.error = fmt::format("geometry {} is not an object", index);
			return;
		}
		//ids are the editor's to assign; a loaded shape has none yet
		SceneShapeRecord record;
		if (!readShape(shapeObject, legacySpheres, index, record.shape, ctx.error)) {
			return;
		}
		std::string materialError;
		if (!readObjectMaterial(shapeObject["material"], version, record.shape.name, ctx.scene, record.material, materialError)) {
			ctx.error = fmt::format("{} \"{}\": {}", shapeTraits(record.shape.kind).name, record.shape.name, materialError);
			return;
		}
		int64_t model = -1;
		if (shapeObject["model"].get_int64().get(model) == simdjson::SUCCESS && model >= 0 && model < (int64_t)ctx.scene.modelPaths.size()) {
			record.model = (int)model;
		}
		ctx.scene.shapes.push_back(std::move(record));
		index++;
	}

	//version 5: the placed mesh objects. Absent in an older file, which the loader reads as "place
	//every node at its authored transform"
	simdjson::dom::array meshObjects;
	if (root["meshObjects"].get_array().get(meshObjects) == simdjson::SUCCESS) {
		ctx.scene.hasMeshObjects = true;

		size_t objectIndex = 0;
		for (simdjson::dom::element entry : meshObjects) {
			simdjson::dom::object object;
			if (entry.get_object().get(object) != simdjson::SUCCESS) {
				ctx.error = fmt::format("mesh object {} is not an object", objectIndex);
				return;
			}

			SceneMeshObjectRecord record;

			std::string_view name;
			if (object["name"].get_string().get(name) == simdjson::SUCCESS) {
				record.name = std::string(name);
			} else {
				record.name = fmt::format("mesh_{}", objectIndex + 1);
			}

			int64_t model = -1;
			if (object["model"].get_int64().get(model) != simdjson::SUCCESS || model < 0 || model >= (int64_t)ctx.scene.modelPaths.size()) {
				ctx.error = fmt::format("mesh object \"{}\" names model {}, which the scene does not have", record.name, model);
				return;
			}
			record.model = (int)model;
			readOptionalUint(object["node"], record.nodeIndex);

			simdjson::dom::array matrix;
			if (object["transform"].get_array().get(matrix) != simdjson::SUCCESS || matrix.size() != 16) {
				ctx.error = fmt::format("mesh object \"{}\" has no valid 16-element \"transform\"", record.name);
				return;
			}
			int element = 0;
			for (simdjson::dom::element component : matrix) {
				double value = 0.0;
				if (!readDouble(component, value)) {
					ctx.error = fmt::format("mesh object \"{}\" has a non-numeric \"transform\" element", record.name);
					return;
				}
				(&record.transform[0][0])[element++] = (float)value;
			}

			readOptionalBool(object["visible"], record.visible);

			std::string_view shading;
			if (object["shading"].get_string().get(shading) == simdjson::SUCCESS && shading == "override") {
				record.materialMode = MeshMaterialMode::Override;
			}
			//an override's material. Older files wrote one whichever mode was selected; only an object
			//in override mode keeps it, as a user material named after the object
			if (record.materialMode == MeshMaterialMode::Override) {
				std::string materialError;
				if (!readObjectMaterial(object["material"], version, record.name, ctx.scene, record.material, materialError)) {
					//a file without one gets a default, as it always did
					record.material = {};
				}
			}

			ctx.scene.meshObjects.push_back(std::move(record));
			objectIndex++;
		}
	}
}

}

std::filesystem::path absoluteDirectoryOf(const std::filesystem::path& file)
{
	std::error_code ec;
	std::filesystem::path absolute = std::filesystem::absolute(file, ec);
	return (ec ? file : absolute).lexically_normal().parent_path();
}

IoResult saveSceneFile(const SceneDescription& scene, const std::filesystem::path& fileIn)
{
	if (fileIn.empty()) {
		return IoResult::failure("No path given");
	}

	//fastgltf's exporter rejects a path with no directory component at all ("invalid glTF
	//directory"), so a bare "scene.gltf" would fail for no reason the caller could see. Resolving
	//against the working directory first gives it something to write next to
	std::error_code absoluteEc;
	const std::filesystem::path absolute = std::filesystem::absolute(fileIn, absoluteEc);
	const std::filesystem::path file = absoluteEc ? fileIn : absolute.lexically_normal();

	//FileExporter creates a single missing directory level; the default save location is nested
	if (!file.parent_path().empty()) {
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		if (ec) {
			return IoResult::failure(fmt::format("Could not create directory '{}': {}", file.parent_path().string(), ec.message()));
		}
	}

	//the smallest asset that is still a valid glTF 2.0 document. The schema requires a scene to
	//reference at least one node, so it carries one empty node
	fastgltf::Asset asset;

	fastgltf::AssetInfo info;
	info.gltfVersion = "2.0";
	info.generator = "vulkan_guide";
	asset.assetInfo = std::move(info);

	fastgltf::Node node;
	node.name = "vulkan_guide_scene";
	asset.nodes.push_back(std::move(node));

	fastgltf::Scene gltfScene;
	gltfScene.name = "vulkan_guide scene";
	gltfScene.nodeIndices.push_back(0);
	asset.scenes.push_back(std::move(gltfScene));
	asset.defaultScene = 0;

	std::string json = sceneJson(scene, absoluteDirectoryOf(file));

	fastgltf::FileExporter exporter;
	exporter.setUserPointer(&json);
	exporter.setExtrasWriteCallback(&writeExtras);

	const fastgltf::Error error = exporter.writeGltfJson(asset, file, fastgltf::ExportOptions::PrettyPrintJson);
	if (error != fastgltf::Error::None) {
		return IoResult::failure(fmt::format("Failed to write '{}': {}", file.string(), fastgltf::getErrorMessage(error)));
	}

	return IoResult::success(fmt::format("Saved '{}': {} model(s), {} shape(s), {} light(s), {} camera(s){}", file.filename().string(), scene.modelPaths.size(), scene.shapes.size(), scene.lights.size(), scene.cameras.size(), scene.environmentMapPath.empty() ? "" : ", environment map"));
}

IoResult loadSceneFile(const std::filesystem::path& file, SceneDescription& out)
{
	if (file.empty()) {
		return IoResult::failure("No path given");
	}

	//fastgltf's own message for a missing file blames the directory, so check first
	std::error_code ec;
	if (!std::filesystem::exists(file, ec)) {
		return IoResult::failure(fmt::format("No file at '{}'", file.string()));
	}

	auto data = fastgltf::GltfDataBuffer::FromPath(file);
	if (data.error() != fastgltf::Error::None) {
		return IoResult::failure(fmt::format("Could not read '{}': {}", file.string(), fastgltf::getErrorMessage(data.error())));
	}

	//the same parser setup loadGltf() uses, plus the extras callback and minus every category this
	//has no use for
	ParseContext ctx;
	ctx.sceneDirectory = absoluteDirectoryOf(file);
	fastgltf::Parser parser;
	parser.setUserPointer(&ctx);
	parser.setExtrasParseCallback(&parseSceneExtras);

	auto asset = parser.loadGltf(data.get(), file.parent_path(), fastgltf::Options::None, fastgltf::Category::Scenes | fastgltf::Category::Asset);
	if (asset.error() != fastgltf::Error::None) {
		return IoResult::failure(fmt::format("'{}' is not a readable glTF file: {}", file.string(), fastgltf::getErrorMessage(asset.error())));
	}

	if (!ctx.error.empty()) {
		return IoResult::failure(fmt::format("'{}': {}", file.string(), ctx.error));
	}
	if (!ctx.found) {
		return IoResult::failure(fmt::format("'{}' is a glTF file but not a saved scene (no scene extras with a \"geometry\" array). Use Import glTF Model for model files", file.string()));
	}

	out = std::move(ctx.scene);
	return IoResult::success(fmt::format("Read '{}': {} model(s), {} shape(s), {} camera(s)", file.filename().string(), out.modelPaths.size(), out.shapes.size(), out.cameras.size()));
}
