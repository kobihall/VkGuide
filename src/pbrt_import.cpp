#include <pbrt_import.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>

#include <fmt/format.h>
#include <glm/gtc/quaternion.hpp>

#include <image_io.h>
#include <light_sampling.h>
#include <ply_mesh.h>
#include <spectrum.h>

namespace {

constexpr double PI = std::numbers::pi;

// the plans that add what the import substitutes, as the report names them
const char* const PLAN_CAMERAS = "pbrt-cameras.md";
const char* const PLAN_DISPLAY = "pbrt-display-and-color.md";
const char* const PLAN_BSDFS = "pbrt-bsdf-library.md";
const char* const PLAN_LAYERED = "pbrt-layered-materials.md";
const char* const PLAN_MEASURED = "pbrt-measured-bsdfs.md";
const char* const PLAN_HAIR = "pbrt-curves-and-hair.md";
const char* const PLAN_SUBSURFACE = "pbrt-subsurface-scattering.md";
const char* const PLAN_MEDIA = "pbrt-participating-media.md";
const char* const PLAN_TEXTURES = "pbrt-textures.md";
const char* const PLAN_PROCEDURAL = "pbrt-procedural-textures.md";
const char* const PLAN_BUMP = "pbrt-bump-mapping.md";
const char* const PLAN_LIGHTS = "pbrt-light-types.md";
const char* const PLAN_ENVIRONMENT = "pbrt-environment-lighting.md";
const char* const PLAN_LIGHT_SAMPLER = "pbrt-bvh-light-sampler.md";
const char* const PLAN_SHAPES = "pbrt-analytic-shapes.md";
const char* const PLAN_SUBDIVISION = "pbrt-subdivision-and-displacement.md";
const char* const PLAN_MOTION = "pbrt-motion-blur.md";
const char* const PLAN_BDPT = "pbrt-bidirectional-path-tracing.md";
const char* const PLAN_PHOTONS = "pbrt-photon-mapping.md";
const char* const PLAN_MLT = "pbrt-metropolis-light-transport.md";
const char* const PLAN_REGULARIZATION = "pbrt-path-regularization.md";
const char* const PLAN_ACCELERATORS = "pbrt-alternative-accelerators.md";

// the segments a full circle is tessellated into, for disks, cylinders and spheres the renderer has no analytic
// shape for
constexpr int CIRCLE_SEGMENTS = 128;

glm::vec3 toVec3(const glm::dvec3& v)
{
	return glm::vec3(v);
}

glm::mat4 toMat4(const glm::dmat4& m)
{
	return glm::mat4(m);
}

double maxComponent(const glm::dvec3& v)
{
	return std::max(v.x, std::max(v.y, v.z));
}

// Fresnel reflectance at normal incidence of a conductor with complex index eta + i k
double conductorF0(double eta, double k)
{
	return ((eta - 1.0) * (eta - 1.0) + k * k) / ((eta + 1.0) * (eta + 1.0) + k * k);
}

// pbrt's TrowbridgeReitzDistribution::RoughnessToAlpha, then the `pbr` type's perceptual roughness, whose square
// is GGX's alpha
float perceptualRoughness(double roughness, bool remap)
{
	const double alpha = remap ? std::sqrt(std::max(roughness, 0.0)) : std::max(roughness, 0.0);
	return (float)std::clamp(std::sqrt(alpha), 0.0, 1.0);
}

// A value a material reads, after its textures are resolved: a color (or a float in x), and an image to multiply
// it by. What the renderer cannot texture is folded into the constant and reported
struct TexValue {
	glm::dvec3 factor { 1.0 };
	int image { -1 };
	// pbrt's uv mapping of the image: (su, sv, du, dv)
	glm::dvec4 mapping { 1.0, 1.0, 0.0, 0.0 };
};

enum class SpectrumUse {
	Albedo,
	Unbounded,
	Illuminant
};

// A material as the importer resolved it, before an area light may make it an emitter
struct Resolved {
	ImportedMaterial material;
	// the uv mapping of its base colour image, which the meshes it shades are given
	glm::dvec4 mapping { 1.0, 1.0, 0.0, 0.0 };
	// "interface" or none: the shape is only a medium's boundary and is not drawn
	bool invisible { false };
	// a material the renderer reflects nothing from would hide that pbrt's emitter also reflects
	bool reflects { false };
};

class Importer {
public:
	Importer(const PbrtScene& scene, ImportedModel& model)
		: s(scene)
		, m(model)
	{
	}

	void run()
	{
		chooseWorldMirror();
		camera();
		settings();
		for (const auto& [name, medium] : s.media) {
			report(fmt::format("participating medium \"{}\" ({}) is not rendered", name, medium.type), medium.loc, PLAN_MEDIA);
			medium.params.find("type");
		}
		for (const PbrtShape& shape : s.shapes) {
			placeShape(shape, glm::dmat4(1.0), shape.loc);
		}
		instances();
		lights();
		for (const auto& [type, skipped] : s.skippedShapes) {
			report(fmt::format("Shape \"{}\" is not supported; {} skipped", type, skipped.first), skipped.second, type == "curve" ? PLAN_HAIR : PLAN_SHAPES);
		}
		for (const PbrtMessage& warning : s.warnings) {
			report(warning.text, warning.loc);
		}
	}

private:
	const PbrtScene& s;
	ImportedModel& m;
	// pbrt world -> renderer world: a mirror or the identity, then the rotation that stands the scene up (chooseWorldMirror)
	glm::dmat4 W { 1.0 };

	std::map<std::string, int> m_images;
	std::map<std::string, std::optional<Resolved>> m_resolved;
	std::map<std::string, uint32_t> m_materialIndex;
	std::map<std::string, int> m_typeCounts;
	// the spectrum lightRgb() read last, which addLight() keeps
	SpectrumRef m_lightSpectrum { rgbSpectrum(glm::vec3(1.f)) };
	// per object, the meshes its shapes became (or -1), so each instance only adds nodes
	std::map<std::string, std::vector<int>> m_objectMeshes;

	void report(const std::string& text, const PbrtLoc& loc, const std::string& plan = {})
	{
		m.report.add(text, s.where(loc), plan);
	}

	void reportUnused(const PbrtParams& params, const std::string& directive)
	{
		for (const PbrtParam* p : params.unused()) {
			report(fmt::format("{}: parameter \"{} {}\" is ignored", directive, p->type, p->name), p->loc);
		}
	}

	//---------------------------------------------------------------- coordinates

	void chooseWorldMirror()
	{
		const glm::dmat4 cameraFromWorld = s.camera.has_value() ? s.camera->cameraFromWorld : glm::dmat4(1.0);
		const bool cameraMirrors = glm::determinant(glm::dmat3(cameraFromWorld)) < 0.0;
		const glm::dmat3 mirror = cameraMirrors ? glm::dmat3(1.0) : glm::dmat3(glm::scale(glm::dmat4(1.0), glm::dvec3(1.0, 1.0, -1.0)));

		//pbrt has no up axis; a scene's camera says which way is up, and the raster viewport's free camera (no roll)
		//needs it to be +y. The axis nearest the camera's up is the scene's up when it is pbrt's +y or +z, the two that
		//scenes use. Otherwise a camera within 25 degrees of looking along an axis is taken to look down it (a plan view
		//with a sideways up, as in watercolor and kroken), and failing that the camera's up is used as it is. A rotation
		//by quarter turns moves no value by more than a sign, so a render from the scene's camera does not change
		const glm::dmat3 pbrtFromCamera = glm::inverse(glm::dmat3(cameraFromWorld));
		const glm::dvec3 forward = glm::normalize(pbrtFromCamera * glm::dvec3(0.0, 0.0, 1.0));
		const auto nearestAxis = [](const glm::dvec3& v) {
			const glm::dvec3 a = glm::abs(v);
			const int axis = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
			glm::dvec3 unit(0.0);
			unit[axis] = v[axis] < 0.0 ? -1.0 : 1.0;
			return unit;
		};
		glm::dvec3 pbrtUp = nearestAxis(pbrtFromCamera * glm::dvec3(0.0, 1.0, 0.0));
		bool planView = false;
		if (pbrtUp != glm::dvec3(0.0, 1.0, 0.0) && pbrtUp != glm::dvec3(0.0, 0.0, 1.0)
			&& glm::length(nearestAxis(forward) - forward) < 2.0 * std::sin(glm::radians(25.0) / 2.0)) {
			pbrtUp = -nearestAxis(forward);
			planView = true;
		}
		const glm::dvec3 up = mirror * pbrtUp;
		const int axis = up.x != 0.0 ? 0 : (up.y != 0.0 ? 1 : 2);
		const bool negative = up[axis] < 0.0;

		//the quarter-turn rotation taking that axis to +y, by columns (where the renderer's x, y and z come from)
		glm::dmat3 stand(1.0);
		if (axis == 0) {
			stand = negative ? glm::dmat3(0, -1, 0, 1, 0, 0, 0, 0, 1) : glm::dmat3(0, 1, 0, -1, 0, 0, 0, 0, 1);
		} else if (axis == 1 && negative) {
			stand = glm::dmat3(1, 0, 0, 0, -1, 0, 0, 0, -1);
		} else if (axis == 2) {
			stand = negative ? glm::dmat3(1, 0, 0, 0, 0, 1, 0, -1, 0) : glm::dmat3(1, 0, 0, 0, 0, -1, 0, 1, 0);
		}
		W = glm::dmat4(stand * mirror);

		if (axis != 1 || negative) {
			const char* names[3] = { "x", "y", "z" };
			report(fmt::format("the scene's up is pbrt's {}{} axis, which becomes +y ({})", pbrtUp[axis] < 0.0 ? "-" : "+", names[axis],
					   planView ? "the camera looks down it" : "the camera's up"),
				s.camera.has_value() ? s.camera->loc : PbrtLoc {});
		}
	}

	glm::dvec3 worldPoint(const glm::dmat4& worldFromObject, const glm::dvec3& p) const
	{
		return glm::dvec3(W * worldFromObject * glm::dvec4(p, 1.0));
	}

	glm::dvec3 worldVector(const glm::dmat4& worldFromObject, const glm::dvec3& v) const
	{
		return glm::dvec3(W * worldFromObject * glm::dvec4(v, 0.0));
	}

	// the rotation taking local -z (where a spot or directional light shines, and a camera looks) to `direction`
	static glm::quat rotationTo(const glm::dvec3& direction)
	{
		const glm::vec3 d = glm::normalize(toVec3(direction));
		const glm::vec3 from(0.f, 0.f, -1.f);
		if (glm::dot(from, d) < -0.999999f) {
			return glm::angleAxis(glm::pi<float>(), glm::vec3(0.f, 1.f, 0.f));
		}
		return glm::normalize(glm::quat(from, d));
	}

	//---------------------------------------------------------------- spectra

	std::optional<DenseSpectrum> denseSpectrum(const PbrtParam& p)
	{
		if (p.type == "blackbody") {
			return p.numbers.empty() ? std::nullopt : std::optional(blackbodySpectrum(p.numbers[0]));
		}
		if (p.type != "spectrum") {
			return std::nullopt;
		}
		if (!p.strings.empty()) {
			if (std::optional<DenseSpectrum> named = namedSpectrum(p.strings[0])) {
				return named;
			}
			std::filesystem::path path = p.strings[0];
			if (path.is_relative()) {
				path = s.directory / path;
			}
			std::string error;
			if (std::optional<DenseSpectrum> file = readSpectrumFile(path, &error)) {
				return file;
			}
			report(fmt::format("spectrum \"{}\" is neither a named spectrum nor a readable file: {}", p.strings[0], error), p.loc);
			return std::nullopt;
		}
		if (p.numbers.size() >= 2 && p.numbers.size() % 2 == 0) {
			const std::vector<float> interleaved(p.numbers.begin(), p.numbers.end());
			return piecewiseLinearSpectrum(interleaved, false);
		}
		report(fmt::format("spectrum \"{}\" has an odd number of values", p.name), p.loc);
		return std::nullopt;
	}

	// A spectrum-valued parameter as a SpectrumRef, as the spectral modes carry it (docs/plans/completed/pbrt-spectral-rendering.md):
	// an RGB triple, a constant, a blackbody, one of pbrt's named spectra, an .spd file, or (wavelength, value) pairs.
	// nullopt if it is absent, a texture or unreadable
	std::optional<SpectrumRef> spectrumRefOf(const PbrtParam* p)
	{
		if (p == nullptr || p->type == "texture") {
			return std::nullopt;
		}
		if (p->type == "rgb") {
			return p->numbers.size() >= 3 ? std::optional(rgbSpectrum(glm::max(toVec3(glm::dvec3(p->numbers[0], p->numbers[1], p->numbers[2])), glm::vec3(0.f)))) : std::nullopt;
		}
		if (p->type == "float") {
			return p->numbers.empty() ? std::nullopt : std::optional(constantRef((float)p->numbers[0]));
		}
		if (p->type == "blackbody") {
			return p->numbers.empty() ? std::nullopt : std::optional(blackbodyRef((float)p->numbers[0]));
		}
		if (p->type != "spectrum") {
			return std::nullopt;
		}
		if (!p->strings.empty()) {
			if (std::optional<SpectrumRef> named = namedRef(p->strings[0])) {
				return named;
			}
			std::filesystem::path path = p->strings[0];
			if (path.is_relative()) {
				path = s.directory / path;
			}
			std::string error;
			return fileRef(path, &error);
		}
		if (p->numbers.size() >= 2 && p->numbers.size() % 2 == 0) {
			return tabulatedRef(std::vector<float>(p->numbers.begin(), p->numbers.end()));
		}
		return std::nullopt;
	}

	// AN EMITTER'S SPECTRUM: what it emits as a SpectrumRef, and as the linear RGB exact RGB sees, both before pbrt's
	// "scale". pbrt divides a light's spectrum by its luminance, which the Illuminant role does for any kind but RGB,
	// whose luminance is its own (pbrt divides it by its illuminant's alone)
	struct Emission {
		SpectrumRef color;
		glm::dvec3 rgb { 1.0 };
	};

	Emission emissionOf(const PbrtParam* p)
	{
		Emission e;
		e.color = rgbSpectrum(glm::vec3(1.f));
		if (p == nullptr) {
			return e;
		}
		e.rgb = spectrumRgb(p, SpectrumUse::Illuminant).value_or(glm::dvec3(1.0));
		if (p->type != "rgb") {
			if (std::optional<SpectrumRef> ref = spectrumRefOf(p)) {
				e.color = *ref;
				return e;
			}
		}
		e.color = rgbSpectrum(toVec3(e.rgb));
		return e;
	}

	// a spectrum-valued parameter as linear RGB, nullopt if it is absent or a texture
	std::optional<glm::dvec3> spectrumRgb(const PbrtParam* p, SpectrumUse use)
	{
		if (p == nullptr || p->type == "texture") {
			return std::nullopt;
		}
		if (p->type == "rgb") {
			if (p->numbers.size() < 3) {
				report(fmt::format("\"rgb {}\" needs three values", p->name), p->loc);
				return std::nullopt;
			}
			glm::dvec3 rgb(p->numbers[0], p->numbers[1], p->numbers[2]);
			if (use == SpectrumUse::Albedo && maxComponent(rgb) > 1.0) {
				report(fmt::format("\"rgb {}\" is a reflectance above 1; clamped", p->name), p->loc);
				rgb = glm::min(rgb, glm::dvec3(1.0));
			}
			return glm::max(rgb, glm::dvec3(0.0));
		}
		if (p->type == "float" && !p->numbers.empty()) {
			return glm::dvec3(p->numbers[0]);
		}
		std::optional<DenseSpectrum> dense = denseSpectrum(*p);
		if (!dense.has_value()) {
			return std::nullopt;
		}
		switch (use) {
		case SpectrumUse::Albedo:
			//kept as a spectrum where the material takes one (a diffuse reflectance); elsewhere this RGB is all there is
			if (p->name != "reflectance") {
				report(fmt::format("{} \"{}\" is its RGB under D65", p->type, p->name), p->loc, PLAN_BSDFS);
			}
			return glm::dvec3(reflectanceToRgb(*dense));
		case SpectrumUse::Illuminant: {
			//the spectrum itself is kept for the spectral modes (emissionOf()); exact RGB sees its projection
			const glm::dvec3 rgb(emissionToRgb(*dense));
			//a deep red blackbody lies outside sRGB: pbrt writes the negative blue, which exact RGB cannot emit
			if (std::min(rgb.x, std::min(rgb.y, rgb.z)) < 0.0) {
				report(fmt::format("{} emission outside the sRGB gamut is clamped to it in exact RGB; the spectral modes emit the spectrum", p->type), p->loc);
			}
			return glm::max(rgb, glm::dvec3(0.0));
		}
		case SpectrumUse::Unbounded:
			report(fmt::format("{} \"{}\" is its luminance-weighted mean", p->type, p->name), p->loc, PLAN_BSDFS);
			return glm::dvec3(luminanceWeightedMean(*dense));
		}
		return std::nullopt;
	}

	//---------------------------------------------------------------- textures

	int imageIndex(const std::filesystem::path& path, ImageEncoding encoding, float gamma, bool invert, bool data = false)
	{
		const std::string key = fmt::format("{}|{}|{}|{}|{}", path.string(), (int)encoding, gamma, invert, data);
		const auto found = m_images.find(key);
		if (found != m_images.end()) {
			return found->second;
		}
		const int index = (int)m.images.size();
		m.images.push_back({ path, encoding, gamma, invert, data });
		m_images[key] = index;
		return index;
	}

	// a named texture's value; `spectrum` picks pbrt's spectrum or float namespace
	TexValue texture(const std::string& name, bool spectrum, const PbrtLoc& loc, int depth = 0)
	{
		const std::map<std::string, PbrtTexture>& textures = spectrum ? s.spectrumTextures : s.floatTextures;
		const auto found = textures.find(name);
		if (found == textures.end() || depth > 16) {
			report(fmt::format("{} texture \"{}\" is not defined; white is used", spectrum ? "spectrum" : "float", name), loc);
			return {};
		}
		const PbrtTexture& t = found->second;
		TexValue value = textureValue(t, depth);
		reportUnused(t.params, fmt::format("Texture \"{}\"", t.className));
		return value;
	}

	// a parameter that is a texture or a value
	TexValue textureOrValue(const PbrtParams& params, std::string_view name, bool spectrum, const glm::dvec3& fallback, const PbrtLoc& loc, int depth)
	{
		const PbrtParam* p = params.find(name);
		if (p == nullptr) {
			return TexValue { fallback };
		}
		if (p->type == "texture" && !p->strings.empty()) {
			return texture(p->strings[0], spectrum, p->loc, depth + 1);
		}
		std::optional<glm::dvec3> rgb = spectrumRgb(p, SpectrumUse::Albedo);
		return TexValue { rgb.value_or(fallback) };
	}

	TexValue textureValue(const PbrtTexture& t, int depth)
	{
		const PbrtParams& p = t.params;
		if (t.className == "imagemap") {
			std::filesystem::path path = p.getString("filename", "");
			if (path.empty()) {
				report("imagemap texture without a filename", t.loc);
				return {};
			}
			if (path.is_relative()) {
				path = s.directory / path;
			}
			ImageEncoding encoding = ImageEncoding::Srgb;
			float gamma = 1.f;
			const std::string encodingName = p.getString("encoding", "sRGB");
			if (encodingName == "linear") {
				encoding = ImageEncoding::Linear;
			} else if (encodingName.rfind("gamma", 0) == 0) {
				encoding = ImageEncoding::Gamma;
				gamma = (float)std::atof(encodingName.c_str() + 5);
			}
			TexValue value;
			value.factor = glm::dvec3(p.getFloat("scale", 1.0));
			value.image = imageIndex(path, encoding, gamma, p.getBool("invert", false));
			value.mapping = { p.getFloat("uscale", 1.0), p.getFloat("vscale", 1.0), p.getFloat("udelta", 0.0), p.getFloat("vdelta", 0.0) };
			const std::string mapping = p.getString("mapping", "uv");
			if (mapping != "uv") {
				report(fmt::format("imagemap mapping \"{}\" is read as \"uv\"", mapping), t.loc, PLAN_TEXTURES);
				p.find("v1");
				p.find("v2");
			}
			const std::string wrap = p.getString("wrap", "repeat");
			if (wrap != "repeat") {
				report(fmt::format("imagemap wrap \"{}\" repeats instead", wrap), t.loc, PLAN_TEXTURES);
			}
			//the renderer filters its own way (the texture array has no mips yet)
			p.find("filter");
			p.find("maxanisotropy");
			return value;
		}
		if (t.className == "constant") {
			return textureOrValue(p, "value", t.spectrum, glm::dvec3(1.0), t.loc, depth);
		}
		if (t.className == "scale") {
			const TexValue a = textureOrValue(p, "tex", t.spectrum, glm::dvec3(1.0), t.loc, depth);
			const TexValue b = textureOrValue(p, "scale", false, glm::dvec3(1.0), t.loc, depth);
			TexValue value { a.factor * b.factor, a.image >= 0 ? a.image : b.image, a.image >= 0 ? a.mapping : b.mapping };
			if (a.image >= 0 && b.image >= 0) {
				report("scale texture of two images keeps only the first", t.loc, PLAN_TEXTURES);
			}
			return value;
		}
		if (t.className == "mix" || t.className == "directionmix") {
			const TexValue a = textureOrValue(p, "tex1", t.spectrum, glm::dvec3(0.0), t.loc, depth);
			const TexValue b = textureOrValue(p, "tex2", t.spectrum, glm::dvec3(1.0), t.loc, depth);
			double amount = 0.5;
			if (t.className == "mix") {
				const TexValue amountValue = textureOrValue(p, "amount", false, glm::dvec3(0.5), t.loc, depth);
				amount = amountValue.factor.x;
				if (amountValue.image >= 0) {
					report("mix texture's amount texture is read as its constant", t.loc, PLAN_PROCEDURAL);
				}
			} else {
				p.find("dir");
			}
			if (a.image < 0 && b.image < 0) {
				return TexValue { glm::mix(a.factor, b.factor, amount) };
			}
			report(fmt::format("{} texture of images keeps the larger share", t.className), t.loc, PLAN_PROCEDURAL);
			return amount < 0.5 ? a : b;
		}
		if (t.className == "checkerboard") {
			const TexValue a = textureOrValue(p, "tex1", t.spectrum, glm::dvec3(1.0), t.loc, depth);
			const TexValue b = textureOrValue(p, "tex2", t.spectrum, glm::dvec3(0.0), t.loc, depth);
			report("checkerboard texture is its mean", t.loc, PLAN_PROCEDURAL);
			return TexValue { (a.factor + b.factor) * 0.5 };
		}
		if (t.className == "bilerp") {
			const glm::dvec3 v = (textureOrValue(p, "v00", t.spectrum, glm::dvec3(0.0), t.loc, depth).factor
				+ textureOrValue(p, "v01", t.spectrum, glm::dvec3(1.0), t.loc, depth).factor
				+ textureOrValue(p, "v10", t.spectrum, glm::dvec3(0.0), t.loc, depth).factor
				+ textureOrValue(p, "v11", t.spectrum, glm::dvec3(1.0), t.loc, depth).factor) * 0.25;
			report("bilerp texture is its mean", t.loc, PLAN_PROCEDURAL);
			return TexValue { v };
		}
		report(fmt::format("{} texture is read as 0.5", t.className), t.loc, PLAN_PROCEDURAL);
		for (const PbrtParam& param : p.list) {
			param.used = true;
		}
		return TexValue { glm::dvec3(0.5) };
	}

	// a float a material reads as a constant: its texture's constant, reported when an image is dropped
	double floatParam(const PbrtParams& params, std::string_view name, double fallback, const PbrtLoc& loc, const char* plan)
	{
		const PbrtParam* p = params.find(name);
		if (p == nullptr) {
			return fallback;
		}
		if (p->type == "texture") {
			const TexValue value = texture(p->strings.empty() ? std::string() : p->strings[0], false, p->loc);
			if (value.image >= 0) {
				report(fmt::format("float texture \"{}\" for \"{}\" is read as {}", p->strings[0], name, value.factor.x), loc, plan);
			}
			return value.factor.x;
		}
		return p->numbers.empty() ? fallback : p->numbers[0];
	}

	//---------------------------------------------------------------- materials

	// pbrt's roughness parameters ("roughness", or "uroughness" and "vroughness") as the `pbr` type's
	float roughnessOf(const PbrtParams& params, const std::string& prefix, const PbrtLoc& loc, const char* plan)
	{
		const bool remap = params.getBool("remaproughness", true);
		double u = floatParam(params, prefix + "roughness", 0.0, loc, plan);
		double v = u;
		if (params.find(prefix + "uroughness") != nullptr || params.find(prefix + "vroughness") != nullptr) {
			u = floatParam(params, prefix + "uroughness", u, loc, plan);
			v = floatParam(params, prefix + "vroughness", v, loc, plan);
			if (u != v) {
				report("anisotropic roughness is the mean of its two", loc, PLAN_BSDFS);
			}
		}
		return perceptualRoughness(0.5 * (u + v), remap);
	}

	// a conductor's color at normal incidence, from its eta and k spectra (pbrt's default copper)
	glm::dvec3 conductorColor(const PbrtParams& params, const std::string& prefix, const PbrtLoc& loc, bool reportSubstitution = true)
	{
		const PbrtParam* eta = params.find(prefix + "eta");
		const PbrtParam* k = params.find(prefix + "k");
		std::optional<DenseSpectrum> etaDense = eta != nullptr ? denseSpectrum(*eta) : namedSpectrum("metal-Cu-eta");
		std::optional<DenseSpectrum> kDense = k != nullptr ? denseSpectrum(*k) : namedSpectrum("metal-Cu-k");
		if (eta != nullptr && (eta->type == "rgb" || eta->type == "float") && (k == nullptr || k->type == "rgb" || k->type == "float")) {
			const glm::dvec3 e = spectrumRgb(eta, SpectrumUse::Unbounded).value_or(glm::dvec3(1.0));
			const glm::dvec3 kk = k != nullptr ? spectrumRgb(k, SpectrumUse::Unbounded).value_or(glm::dvec3(0.0)) : glm::dvec3(0.0);
			return { conductorF0(e.x, kk.x), conductorF0(e.y, kk.y), conductorF0(e.z, kk.z) };
		}
		if (!etaDense.has_value() || !kDense.has_value()) {
			report("conductor's eta or k is unreadable; copper is used", loc);
			etaDense = namedSpectrum("metal-Cu-eta");
			kDense = namedSpectrum("metal-Cu-k");
		}
		DenseSpectrum f0;
		for (int i = 0; i < spectrum_tables::CIE_SAMPLES; i++) {
			f0.values[i] = conductorF0(etaDense->values[i], kDense->values[i]);
		}
		if (reportSubstitution && (eta == nullptr || eta->type != "rgb")) {
			report("conductor color is its spectral Fresnel reflectance at normal incidence under D65", loc, PLAN_BSDFS);
		}
		return glm::dvec3(reflectanceToRgb(f0));
	}

	void setPbr(Resolved& r, const TexValue& base, float metallic, float roughness)
	{
		r.material.definition.type = MaterialType::Pbr;
		r.material.definition.params["albedo"] = rgbSpectrum(toVec3(base.factor));
		r.material.definition.params["metallic"] = metallic;
		r.material.definition.params["roughness"] = roughness;
		r.material.baseColorImage = base.image;
		r.mapping = base.mapping;
		r.material.previewColor = glm::vec4(toVec3(base.factor), 1.f);
		r.material.previewMetallic = metallic;
		r.material.previewRoughness = roughness;
		r.reflects = maxComponent(base.factor) > 0.0;
	}

	void setLambertian(Resolved& r, const TexValue& base)
	{
		r.material.definition.type = MaterialType::Lambertian;
		r.material.definition.params["albedo"] = rgbSpectrum(toVec3(base.factor));
		r.material.baseColorImage = base.image;
		r.mapping = base.mapping;
		r.material.previewColor = glm::vec4(toVec3(base.factor), 1.f);
		r.reflects = maxComponent(base.factor) > 0.0;
	}

	// pbrt's roughness parameters as the glass and conductor types take them: u and v, and whether to remap them
	void setRoughness(Resolved& r, const PbrtParams& params, const std::string& prefix, const PbrtLoc& loc)
	{
		double u = floatParam(params, prefix + "roughness", 0.0, loc, PLAN_TEXTURES);
		double v = u;
		if (params.find(prefix + "uroughness") != nullptr || params.find(prefix + "vroughness") != nullptr) {
			u = floatParam(params, prefix + "uroughness", u, loc, PLAN_TEXTURES);
			v = floatParam(params, prefix + "vroughness", v, loc, PLAN_TEXTURES);
		}
		r.material.definition.params["uRoughness"] = (float)u;
		r.material.definition.params["vRoughness"] = (float)v;
		r.material.definition.params["remapRoughness"] = params.getBool("remaproughness", true);
	}

	// pbrt's dielectric as the glass type: exact Fresnel, rough or smooth, and its eta as the spectrum it is, which
	// the spectral modes disperse and exact RGB reads at the design wavelength
	void setDielectric(Resolved& r, const PbrtParams& params, const PbrtLoc& loc)
	{
		SpectrumRef eta = constantRef(1.5f);
		if (const PbrtParam* p = params.find("eta")) {
			if (std::optional<SpectrumRef> ref = spectrumRefOf(p); ref.has_value() && ref->kind != SpectrumKind::Rgb) {
				eta = *ref;
			} else if (std::optional<glm::dvec3> mean = spectrumRgb(p, SpectrumUse::Unbounded)) {
				eta = constantRef((float)mean->x);
			}
		}
		r.material.definition.type = MaterialType::Glass;
		r.material.definition.params["eta"] = eta;
		setRoughness(r, params, "", loc);
		r.material.previewColor = glm::vec4(0.75f, 0.85f, 1.f, 1.f);
		r.reflects = true;
	}

	// pbrt's conductor as the conductor type: its eta and k spectra (pbrt's default copper), or its reflectance
	bool setConductor(Resolved& r, const PbrtParams& params, const PbrtLoc& loc)
	{
		r.material.definition.type = MaterialType::Conductor;
		if (const PbrtParam* reflectance = params.find("reflectance")) {
			const std::optional<glm::dvec3> rgb = spectrumRgb(reflectance, SpectrumUse::Albedo);
			if (!rgb.has_value()) {
				//a textured reflectance needs pbrt-textures.md on this type: the caller keeps pbr
				return false;
			}
			r.material.definition.params["useReflectance"] = true;
			r.material.definition.params["albedo"] = reflectance->type == "rgb" ? rgbSpectrum(toVec3(*rgb)) : spectrumRefOf(reflectance).value_or(rgbSpectrum(toVec3(*rgb)));
		} else {
			const std::optional<SpectrumRef> eta = params.find("eta") != nullptr ? spectrumRefOf(params.find("eta")) : namedRef("metal-Cu-eta");
			const std::optional<SpectrumRef> k = params.find("k") != nullptr ? spectrumRefOf(params.find("k")) : namedRef("metal-Cu-k");
			if (!eta.has_value() || !k.has_value()) {
				report("conductor's eta or k is unreadable; copper is used", loc);
			}
			r.material.definition.params["eta"] = eta.value_or(*namedRef("metal-Cu-eta"));
			r.material.definition.params["k"] = k.value_or(*namedRef("metal-Cu-k"));
		}
		setRoughness(r, params, "", loc);
		r.material.previewColor = glm::vec4(toVec3(conductorColor(params, "", loc, false)), 1.f);
		r.material.previewMetallic = 1.f;
		r.reflects = true;
		return true;
	}

	// a material by its type and parameters, as the renderer's closest type
	Resolved mapMaterial(const std::string& type, const PbrtParams& params, const PbrtLoc& loc, int depth)
	{
		Resolved r;
		params.find("type");
		const std::string where = fmt::format("{} material", type.empty() ? "unnamed" : type);
		if (type == "diffuse") {
			setLambertian(r, textureOrValue(params, "reflectance", true, glm::dvec3(0.5), loc, 0));
			//a measured reflectance is kept as the spectrum it is, which exact RGB reads under D65
			if (const PbrtParam* reflectance = params.find("reflectance"); reflectance != nullptr && reflectance->type != "rgb" && reflectance->type != "texture") {
				if (std::optional<SpectrumRef> ref = spectrumRefOf(reflectance)) {
					r.material.definition.params["albedo"] = *ref;
				}
			}
		} else if (type == "coateddiffuse") {
			setPbr(r, textureOrValue(params, "reflectance", true, glm::dvec3(0.5), loc, 0), 0.f, roughnessOf(params, "", loc, PLAN_LAYERED));
			report("coateddiffuse is pbr: a GGX coat with F0 0.04 over Lambertian", loc, PLAN_LAYERED);
			const double eta = params.getFloat("eta", 1.5);
			if (std::abs(eta - 1.5) > 1e-3) {
				report(fmt::format("coateddiffuse's coat eta {} is pbr's 1.5", eta), loc, PLAN_LAYERED);
			}
			for (const char* name : { "thickness", "albedo", "g", "maxdepth", "nsamples" }) {
				if (params.find(name) != nullptr) {
					report(fmt::format("coateddiffuse's \"{}\" has no pbr equivalent", name), loc, PLAN_LAYERED);
				}
			}
		} else if (type == "conductor") {
			if (!setConductor(r, params, loc)) {
				setPbr(r, textureOrValue(params, "reflectance", true, glm::dvec3(1.0), loc, 0), 1.f, roughnessOf(params, "", loc, PLAN_BSDFS));
				report("a conductor with a textured reflectance is pbr with metallic 1 and Schlick's Fresnel", loc, PLAN_TEXTURES);
			}
		} else if (type == "coatedconductor") {
			TexValue base;
			if (params.find("reflectance") != nullptr) {
				base = textureOrValue(params, "reflectance", true, glm::dvec3(1.0), loc, 0);
			} else {
				base.factor = conductorColor(params, "conductor.", loc);
			}
			const float roughness = roughnessOf(params, "conductor.", loc, PLAN_LAYERED);
			roughnessOf(params, "interface.", loc, PLAN_LAYERED);
			setPbr(r, base, 1.f, roughness);
			report("coatedconductor is pbr with metallic 1; the dielectric coat is dropped", loc, PLAN_LAYERED);
		} else if (type == "dielectric") {
			setDielectric(r, params, loc);
		} else if (type == "thindielectric") {
			setDielectric(r, params, loc);
			report("thindielectric is solid glass", loc, PLAN_BSDFS);
		} else if (type == "diffusetransmission") {
			TexValue base = textureOrValue(params, "reflectance", true, glm::dvec3(0.25), loc, 0);
			base.factor *= params.getFloat("scale", 1.0);
			setLambertian(r, base);
			textureOrValue(params, "transmittance", true, glm::dvec3(0.25), loc, 0);
			report("diffusetransmission is Lambertian; its transmission is dropped", loc, PLAN_BSDFS);
		} else if (type == "mix") {
			const PbrtParam* names = params.find("materials");
			const double amount = floatParam(params, "amount", 0.5, loc, PLAN_BSDFS);
			if (names == nullptr || names->strings.size() != 2) {
				report("mix material without two \"materials\"; gray Lambertian is used", loc);
				setLambertian(r, TexValue { glm::dvec3(0.5) });
			} else {
				//pbrt picks materials[1] with probability `amount`
				const std::string& chosen = names->strings[amount >= 0.5 ? 1 : 0];
				report("mix material is whichever of its two it picks more often", loc, PLAN_BSDFS);
				if (std::optional<Resolved> named = namedMaterial(chosen, loc, depth + 1)) {
					r = *named;
				} else {
					setLambertian(r, TexValue { glm::dvec3(0.5) });
				}
			}
		} else if (type == "measured") {
			setLambertian(r, TexValue { glm::dvec3(0.5) });
			params.find("filename");
			report("measured material is gray Lambertian", loc, PLAN_MEASURED);
		} else if (type == "hair") {
			setLambertian(r, TexValue { spectrumRgb(params.find("color"), SpectrumUse::Albedo).value_or(glm::dvec3(0.3)) });
			report("hair material is Lambertian", loc, PLAN_HAIR);
			for (const PbrtParam& p : params.list) {
				p.used = true;
			}
		} else if (type == "subsurface") {
			setLambertian(r, textureOrValue(params, "reflectance", true, glm::dvec3(0.5), loc, 0));
			report("subsurface material is Lambertian", loc, PLAN_SUBSURFACE);
			for (const PbrtParam& p : params.list) {
				p.used = true;
			}
		} else if (type == "interface" || type == "" || type == "none") {
			r.invisible = true;
			report("a shape with the interface material only bounds a medium and is not drawn", loc, PLAN_MEDIA);
		} else {
			report(fmt::format("unknown material \"{}\" is gray Lambertian", type), loc);
			setLambertian(r, TexValue { glm::dvec3(0.5) });
		}

		if (params.find("displacement") != nullptr) {
			report("displacement (bump mapping) is ignored", loc, PLAN_BUMP);
		}
		const std::string normalMap = params.getString("normalmap", "");
		if (!normalMap.empty()) {
			std::filesystem::path path = normalMap;
			if (path.is_relative()) {
				path = s.directory / path;
			}
			if (r.material.definition.type == MaterialType::Pbr || r.material.definition.type == MaterialType::Lambertian) {
				r.material.normalImage = imageIndex(path, ImageEncoding::Linear, 1.f, false, true);
			} else {
				report(fmt::format("normal map on a {} material is ignored", materialTypeName(r.material.definition.type)), loc, PLAN_BUMP);
			}
		}
		reportUnused(params, where);
		return r;
	}

	std::optional<Resolved> namedMaterial(const std::string& name, const PbrtLoc& loc, int depth = 0)
	{
		const std::string key = "n:" + name;
		if (const auto found = m_resolved.find(key); found != m_resolved.end()) {
			return found->second;
		}
		const auto material = s.namedMaterials.find(name);
		if (material == s.namedMaterials.end() || depth > 8) {
			report(fmt::format("named material \"{}\" is not defined; pbrt's default diffuse is used", name), loc);
			m_resolved[key] = std::nullopt;
			return std::nullopt;
		}
		Resolved r = mapMaterial(material->second.type, material->second.params, material->second.loc, depth);
		r.material.definition.name = name;
		m_resolved[key] = r;
		return r;
	}

	Resolved resolveMaterial(const PbrtMaterialRef& ref, const PbrtLoc& loc, std::string& key)
	{
		if (!ref.name.empty()) {
			key = "n:" + ref.name;
			if (std::optional<Resolved> r = namedMaterial(ref.name, loc)) {
				return *r;
			}
			key = "i:0";
		} else {
			key = fmt::format("i:{}", std::max(ref.index, 0));
		}
		if (const auto found = m_resolved.find(key); found != m_resolved.end() && found->second.has_value()) {
			return *found->second;
		}
		const PbrtEntity& material = s.materials[(size_t)std::clamp(ref.index, 0, (int)s.materials.size() - 1)];
		Resolved r = mapMaterial(material.type, material.params, material.loc, 0);
		const int n = ++m_typeCounts[material.type];
		r.material.definition.name = fmt::format("{} {}", material.type, n);
		m_resolved[key] = r;
		return r;
	}

	// an area light's emission (its spectrum and its linear RGB at pbrt's normalization) and its "scale" (and "power",
	// given the area): the radiance is the emission times the returned scale
	double areaLightRadiance(const PbrtEntity& light, double area, Emission& emission)
	{
		const PbrtParams& p = light.params;
		emission = emissionOf(p.find("L"));
		const glm::dvec3 L = emission.rgb;
		double scale = p.getFloat("scale", 1.0);
		const bool twoSided = p.getBool("twosided", false);
		const double power = p.getFloat("power", -1.0);
		if (power > 0.0 && area > 0.0) {
			//k_e: the luminance times the emitting area and pi; the RGB is already at luminance 1 for a spectrum
			const double luminance = 0.2126 * L.x + 0.7152 * L.y + 0.0722 * L.z;
			const double k = (twoSided ? 2.0 : 1.0) * area * PI * std::max(luminance, 1e-12);
			scale *= power / k;
		}
		if (!p.getString("filename", "").empty()) {
			report("area light image is ignored; its L is used", light.loc, PLAN_LIGHTS);
		}
		if (!twoSided) {
			report("one-sided area light emits from both sides", light.loc, PLAN_LIGHTS);
		}
		reportUnused(p, fmt::format("AreaLightSource \"{}\"", light.type));
		return scale;
	}

	// the library material a shape is shaded with, as an index into m.materials; nullopt for an invisible shape
	std::optional<uint32_t> shapeMaterial(const PbrtShape& shape, double area)
	{
		std::string key;
		const Resolved base = resolveMaterial(shape.material, shape.loc, key);
		if (base.invisible) {
			return std::nullopt;
		}
		if (shape.areaLight < 0) {
			return materialIndex(key, base.material);
		}
		const PbrtEntity& light = s.areaLights[(size_t)shape.areaLight];
		if (light.type != "diffuse") {
			report(fmt::format("area light \"{}\" is not supported; the shape does not emit", light.type), light.loc, PLAN_LIGHTS);
			return materialIndex(key, base.material);
		}
		Emission emission;
		const double scale = areaLightRadiance(light, area, emission);
		const glm::dvec3 radiance = emission.rgb * scale;
		if (base.reflects) {
			report("an emitter's own reflectance is dropped: emissive surfaces absorb", light.loc, PLAN_LIGHTS);
		}
		ImportedMaterial emitter;
		emitter.definition.type = MaterialType::Emissive;
		const double peak = std::max(maxComponent(radiance), 1e-30);
		if (emission.color.kind == SpectrumKind::Rgb) {
			emitter.definition.params["albedo"] = rgbSpectrum(toVec3(radiance / peak));
			emitter.definition.params["strength"] = (float)peak;
		} else {
			//the spectrum, normalized to luminance 1 by its role, times the scale
			emitter.definition.params["albedo"] = emission.color;
			emitter.definition.params["strength"] = (float)scale;
		}
		emitter.previewColor = glm::vec4(toVec3(radiance / peak), 1.f);
		const std::string emitterKey = fmt::format("{}|L {:.9g} {:.9g} {:.9g} {}", key, radiance.x, radiance.y, radiance.z, spectrumKindName(emission.color.kind));
		if (const auto found = m_materialIndex.find(emitterKey); found != m_materialIndex.end()) {
			return found->second;
		}
		int lights = 0;
		for (const auto& [existing, index] : m_materialIndex) {
			lights += existing.rfind(key + "|", 0) == 0 ? 1 : 0;
		}
		emitter.definition.name = lights == 0 ? base.material.definition.name + " light" : fmt::format("{} light {}", base.material.definition.name, lights + 1);
		return materialIndex(emitterKey, emitter);
	}

	uint32_t materialIndex(const std::string& key, const ImportedMaterial& material)
	{
		const auto [it, inserted] = m_materialIndex.try_emplace(key, (uint32_t)m.materials.size());
		if (inserted) {
			m.materials.push_back(material);
		}
		return it->second;
	}

	glm::dvec4 mappingOf(const PbrtShape& shape)
	{
		std::string key;
		return resolveMaterial(shape.material, shape.loc, key).mapping;
	}

	//---------------------------------------------------------------- shapes

	struct MeshData {
		std::vector<glm::dvec3> p;
		std::vector<glm::dvec3> n;
		std::vector<glm::dvec2> uv;
		std::vector<int> indices;
	};

	// trianglemesh and loopsubdiv's base mesh: P, indices, N, uv
	bool inlineMesh(const PbrtShape& shape, MeshData& mesh)
	{
		const PbrtParams& params = shape.params;
		const std::vector<double> p = params.getNumbers("P");
		if (p.size() < 9 || p.size() % 3 != 0) {
			report(fmt::format("{} without a whole number of \"point3 P\" vertices is skipped", shape.type), shape.loc);
			return false;
		}
		for (size_t i = 0; i + 2 < p.size(); i += 3) {
			mesh.p.emplace_back(p[i], p[i + 1], p[i + 2]);
		}
		std::vector<double> indices = params.getNumbers("indices");
		if (indices.empty() && mesh.p.size() == 3) {
			indices = { 0, 1, 2 };
		}
		for (double index : indices) {
			mesh.indices.push_back((int)index);
		}
		if (const PbrtParam* n = params.find("N"); n != nullptr && n->numbers.size() == p.size()) {
			for (size_t i = 0; i + 2 < n->numbers.size(); i += 3) {
				mesh.n.emplace_back(n->numbers[i], n->numbers[i + 1], n->numbers[i + 2]);
			}
		}
		const PbrtParam* uv = params.find("uv");
		if (uv == nullptr) {
			uv = params.find("st");
		}
		if (uv != nullptr && uv->numbers.size() == mesh.p.size() * 2) {
			for (size_t i = 0; i + 1 < uv->numbers.size(); i += 2) {
				mesh.uv.emplace_back(uv->numbers[i], uv->numbers[i + 1]);
			}
		}
		params.find("S");
		params.find("faceIndices");
		return true;
	}

	bool plyMesh(const PbrtShape& shape, MeshData& mesh)
	{
		std::filesystem::path path = shape.params.getString("filename", "");
		if (path.is_relative()) {
			path = s.directory / path;
		}
		PlyMesh ply;
		std::string error;
		size_t skippedFaces = 0;
		if (!readPlyMesh(path, ply, &error, &skippedFaces)) {
			report(fmt::format("plymesh is skipped: {}", error), shape.loc);
			return false;
		}
		if (skippedFaces > 0) {
			report("plymesh faces that are neither triangles nor quads are skipped", shape.loc);
		}
		if (!ply.quadIndices.empty()) {
			report("plymesh quads are two triangles each; pbrt intersects them as bilinear patches", shape.loc, PLAN_SHAPES);
			ply.convertToOnlyTriangles();
		}
		for (const glm::vec3& p : ply.p) {
			mesh.p.emplace_back(p);
		}
		for (const glm::vec3& n : ply.n) {
			mesh.n.emplace_back(n);
		}
		for (const glm::vec2& uv : ply.uv) {
			mesh.uv.emplace_back(uv);
		}
		mesh.indices = std::move(ply.triIndices);
		if (shape.params.find("displacement") != nullptr) {
			report("plymesh displacement is ignored", shape.loc, PLAN_SUBDIVISION);
			shape.params.find("edgelength");
		}
		return true;
	}

	bool bilinearMesh(const PbrtShape& shape, MeshData& mesh)
	{
		MeshData patches;
		const PbrtParams& params = shape.params;
		const std::vector<double> p = params.getNumbers("P");
		for (size_t i = 0; i + 2 < p.size(); i += 3) {
			mesh.p.emplace_back(p[i], p[i + 1], p[i + 2]);
		}
		std::vector<double> indices = params.getNumbers("indices");
		if (indices.empty() && mesh.p.size() == 4) {
			indices = { 0, 1, 2, 3 };
		}
		if (mesh.p.empty() || indices.size() % 4 != 0) {
			report("bilinearmesh without whole patches is skipped", shape.loc);
			return false;
		}
		//each patch's corners p00, p10, p01, p11 as the two triangles pbrt's ConvertToOnlyTriangles() makes
		for (size_t i = 0; i < indices.size(); i += 4) {
			const int q[4] = { (int)indices[i], (int)indices[i + 1], (int)indices[i + 2], (int)indices[i + 3] };
			mesh.indices.insert(mesh.indices.end(), { q[0], q[1], q[3], q[0], q[3], q[2] });
		}
		if (const PbrtParam* n = params.find("N"); n != nullptr && n->numbers.size() == p.size()) {
			for (size_t i = 0; i + 2 < n->numbers.size(); i += 3) {
				mesh.n.emplace_back(n->numbers[i], n->numbers[i + 1], n->numbers[i + 2]);
			}
		}
		if (const PbrtParam* uv = params.find("uv"); uv != nullptr && uv->numbers.size() == mesh.p.size() * 2) {
			for (size_t i = 0; i + 1 < uv->numbers.size(); i += 2) {
				mesh.uv.emplace_back(uv->numbers[i], uv->numbers[i + 1]);
			}
		}
		params.find("faceIndices");
		report("bilinearmesh patches are two triangles each", shape.loc, PLAN_SHAPES);
		return true;
	}

	// a disk or annulus at z = height facing +z, as a fan whose radii are scaled so its area is the disk's
	void diskMesh(const PbrtShape& shape, MeshData& mesh)
	{
		const PbrtParams& params = shape.params;
		const double height = params.getFloat("height", 0.0);
		const double radius = params.getFloat("radius", 1.0);
		const double inner = params.getFloat("innerradius", 0.0);
		const double phiMax = glm::radians(std::clamp(params.getFloat("phimax", 360.0), 0.0, 360.0));
		const int segments = std::max(3, (int)std::ceil(CIRCLE_SEGMENTS * phiMax / (2.0 * PI)));
		const double theta = phiMax / segments;
		const double areaScale = std::sqrt(theta / std::sin(theta));
		for (int i = 0; i <= segments; i++) {
			const double phi = theta * i;
			const glm::dvec2 dir(std::cos(phi), std::sin(phi));
			mesh.p.emplace_back(glm::dvec3(dir * radius * areaScale, height));
			mesh.p.emplace_back(glm::dvec3(dir * inner * areaScale, height));
			mesh.uv.emplace_back((double)i / segments, 0.0);
			mesh.uv.emplace_back((double)i / segments, 1.0);
		}
		for (int i = 0; i < segments; i++) {
			const int o0 = 2 * i;
			const int i0 = o0 + 1;
			const int o1 = o0 + 2;
			const int i1 = o0 + 3;
			mesh.indices.insert(mesh.indices.end(), { o0, o1, i1 });
			if (inner > 0.0) {
				mesh.indices.insert(mesh.indices.end(), { o0, i1, i0 });
			}
		}
		mesh.n.assign(mesh.p.size(), glm::dvec3(0.0, 0.0, 1.0));
		report("disk is triangles with its area kept", shape.loc, PLAN_SHAPES);
	}

	// a cylinder's side, open at both ends, with outward normals and the circumference kept
	void cylinderMesh(const PbrtShape& shape, MeshData& mesh)
	{
		const PbrtParams& params = shape.params;
		const double radius = params.getFloat("radius", 1.0);
		const double zMin = params.getFloat("zmin", -1.0);
		const double zMax = params.getFloat("zmax", 1.0);
		const double phiMax = glm::radians(std::clamp(params.getFloat("phimax", 360.0), 0.0, 360.0));
		const int segments = std::max(3, (int)std::ceil(CIRCLE_SEGMENTS * phiMax / (2.0 * PI)));
		const double theta = phiMax / segments;
		const double perimeterScale = (theta / 2.0) / std::sin(theta / 2.0);
		for (int i = 0; i <= segments; i++) {
			const double phi = theta * i;
			const glm::dvec3 dir(std::cos(phi), std::sin(phi), 0.0);
			for (const double z : { zMin, zMax }) {
				mesh.p.push_back(dir * radius * perimeterScale + glm::dvec3(0.0, 0.0, z));
				mesh.n.push_back(dir);
				mesh.uv.emplace_back((double)i / segments, (z - zMin) / std::max(zMax - zMin, 1e-30));
			}
		}
		for (int i = 0; i < segments; i++) {
			const int a = 2 * i;
			mesh.indices.insert(mesh.indices.end(), { a, a + 2, a + 3, a, a + 3, a + 1 });
		}
		report("cylinder is triangles with its area kept", shape.loc, PLAN_SHAPES);
	}

	// a sphere the analytic shape cannot be: partial, or under a transform that shears it
	void sphereMesh(double radius, MeshData& mesh)
	{
		const int rings = CIRCLE_SEGMENTS / 2;
		for (int j = 0; j <= rings; j++) {
			const double v = (double)j / rings;
			const double polar = v * PI;
			for (int i = 0; i <= CIRCLE_SEGMENTS; i++) {
				const double u = (double)i / CIRCLE_SEGMENTS;
				const double phi = u * 2.0 * PI;
				const glm::dvec3 n(std::sin(polar) * std::cos(phi), std::sin(polar) * std::sin(phi), -std::cos(polar));
				mesh.p.push_back(n * radius);
				mesh.n.push_back(n);
				mesh.uv.emplace_back(u, v);
			}
		}
		const int row = CIRCLE_SEGMENTS + 1;
		for (int j = 0; j < rings; j++) {
			for (int i = 0; i < CIRCLE_SEGMENTS; i++) {
				const int a = j * row + i;
				mesh.indices.insert(mesh.indices.end(), { a, a + 1, a + row + 1, a, a + row + 1, a + row });
			}
		}
	}

	// the shape's geometry in object space as the renderer's mesh: v flipped to run down the image, the material's
	// uv mapping applied, and the orientation reversed where pbrt reverses it
	std::optional<uint32_t> buildMesh(const PbrtShape& shape, MeshData&& data, uint32_t material)
	{
		if (data.indices.empty() || data.indices.size() % 3 != 0) {
			report(fmt::format("{} with no whole triangles is skipped", shape.type), shape.loc);
			return std::nullopt;
		}
		for (int index : data.indices) {
			if (index < 0 || (size_t)index >= data.p.size()) {
				report(fmt::format("{} has a vertex index out of range; skipped", shape.type), shape.loc);
				return std::nullopt;
			}
		}
		ImportedMesh mesh;
		mesh.name = fmt::format("{} {}", shape.type, m.meshes.size());
		mesh.positions.reserve(data.p.size());
		for (const glm::dvec3& p : data.p) {
			mesh.positions.push_back(toVec3(p));
		}
		//with vertex normals pbrt flips them for ReverseOrientation and the surface follows them; without, the
		//winding decides which side is out
		const bool flipNormals = shape.reverseOrientation && !data.n.empty();
		const bool flipWinding = shape.reverseOrientation && data.n.empty();
		for (const glm::dvec3& n : data.n) {
			mesh.normals.push_back(toVec3(flipNormals ? -n : n));
		}
		const glm::dvec4 mapping = mappingOf(shape);
		for (const glm::dvec2& uv : data.uv) {
			const glm::dvec2 st(mapping.x * uv.x + mapping.z, mapping.y * uv.y + mapping.w);
			//pbrt's image lookups flip t, so its v runs up the image; glTF's and the renderer's run down
			mesh.uvs.emplace_back((float)st.x, (float)(1.0 - st.y));
		}
		mesh.indices.reserve(data.indices.size());
		for (size_t i = 0; i < data.indices.size(); i += 3) {
			mesh.indices.push_back((uint32_t)data.indices[i]);
			mesh.indices.push_back((uint32_t)data.indices[flipWinding ? i + 2 : i + 1]);
			mesh.indices.push_back((uint32_t)data.indices[flipWinding ? i + 1 : i + 2]);
		}
		if (m.materials[material].normalImage >= 0 && !data.uv.empty()) {
			mesh.tangents = normalMapTangents(data, mesh.indices);
		}
		mesh.surfaces.push_back({ 0, (uint32_t)mesh.indices.size(), material });
		m.meshes.push_back(std::move(mesh));
		return (uint32_t)(m.meshes.size() - 1);
	}

	// pbrt's normal-map frame (NormalMap() in materials.h): x along dp/du, z the normal, y = z x x, whatever way the
	// uvs turn. The shader's bitangent is cross(normal, tangent) * w, so w is always +1. Per vertex, the triangles'
	// dp/du summed, where pbrt takes each triangle's own
	static std::vector<glm::vec4> normalMapTangents(const MeshData& data, const std::vector<uint32_t>& indices)
	{
		std::vector<glm::dvec3> sum(data.p.size(), glm::dvec3(0.0));
		for (size_t i = 0; i + 2 < indices.size(); i += 3) {
			const uint32_t a = indices[i];
			const uint32_t b = indices[i + 1];
			const uint32_t c = indices[i + 2];
			const glm::dvec2 d1 = data.uv[b] - data.uv[a];
			const glm::dvec2 d2 = data.uv[c] - data.uv[a];
			const double det = d1.x * d2.y - d2.x * d1.y;
			if (std::abs(det) < 1e-20) {
				continue;
			}
			const glm::dvec3 dpdu = ((data.p[b] - data.p[a]) * d2.y - (data.p[c] - data.p[a]) * d1.y) / det;
			for (const uint32_t v : { a, b, c }) {
				sum[v] += dpdu;
			}
		}
		std::vector<glm::vec4> tangents(data.p.size(), glm::vec4(0.f, 0.f, 0.f, 1.f));
		for (size_t v = 0; v < sum.size(); v++) {
			if (glm::dot(sum[v], sum[v]) > 0.0) {
				tangents[v] = glm::vec4(toVec3(glm::normalize(sum[v])), 1.f);
			}
		}
		return tangents;
	}

	// the area of a mesh in pbrt's world, which an area light's "power" divides by
	static double meshArea(const MeshData& mesh, const glm::dmat4& worldFromObject)
	{
		double area = 0.0;
		for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
			const glm::dvec3 a(worldFromObject * glm::dvec4(mesh.p[(size_t)mesh.indices[i]], 1.0));
			const glm::dvec3 b(worldFromObject * glm::dvec4(mesh.p[(size_t)mesh.indices[i + 1]], 1.0));
			const glm::dvec3 c(worldFromObject * glm::dvec4(mesh.p[(size_t)mesh.indices[i + 2]], 1.0));
			area += 0.5 * glm::length(glm::cross(b - a, c - a));
		}
		return area;
	}

	// the geometry of a shape that becomes a mesh, nullopt for one that does not
	std::optional<MeshData> shapeGeometry(const PbrtShape& shape)
	{
		MeshData mesh;
		if (shape.type == "trianglemesh") {
			if (!inlineMesh(shape, mesh)) {
				return std::nullopt;
			}
		} else if (shape.type == "plymesh") {
			if (!plyMesh(shape, mesh)) {
				return std::nullopt;
			}
		} else if (shape.type == "loopsubdiv") {
			if (!inlineMesh(shape, mesh)) {
				return std::nullopt;
			}
			shape.params.find("levels");
			report("loopsubdiv is its control mesh, unsubdivided", shape.loc, PLAN_SUBDIVISION);
		} else if (shape.type == "bilinearmesh") {
			if (!bilinearMesh(shape, mesh)) {
				return std::nullopt;
			}
		} else if (shape.type == "disk") {
			diskMesh(shape, mesh);
		} else if (shape.type == "cylinder") {
			cylinderMesh(shape, mesh);
		} else {
			report(fmt::format("Shape \"{}\" is not supported; skipped", shape.type), shape.loc, PLAN_SHAPES);
			for (const PbrtParam& p : shape.params.list) {
				p.used = true;
			}
			return std::nullopt;
		}
		return mesh;
	}

	void shapeCommon(const PbrtShape& shape)
	{
		if (shape.animated) {
			report("a shape's transform is animated; the shutter-open pose is used", shape.loc, PLAN_MOTION);
		}
		if (const PbrtParam* alpha = shape.params.find("alpha")) {
			report(fmt::format("shape alpha ({}) is ignored", alpha->type), alpha->loc, PLAN_TEXTURES);
		}
		if (!shape.insideMedium.empty() || !shape.outsideMedium.empty()) {
			report("a shape's media are ignored", shape.loc, PLAN_MEDIA);
		}
		shape.params.find("emissionfilename");
	}

	// A sphere as the analytic shape, when it is whole and its transform scales it along the axes; else as triangles
	void placeSphere(const PbrtShape& shape, const glm::dmat4& worldFromObject, const std::string& name)
	{
		const PbrtParams& params = shape.params;
		const double radius = params.getFloat("radius", 1.0);
		const double zMin = params.getFloat("zmin", -radius);
		const double zMax = params.getFloat("zmax", radius);
		const double phiMax = params.getFloat("phimax", 360.0);
		const bool whole = zMin <= -radius && zMax >= radius && phiMax >= 360.0;
		if (!whole) {
			report("partial sphere is a whole sphere", shape.loc, PLAN_SHAPES);
		}
		const glm::dmat4 world = W * worldFromObject;
		const glm::dmat3 linear(world);
		const glm::dvec3 scale(glm::length(linear[0]), glm::length(linear[1]), glm::length(linear[2]));
		const bool uniform = std::abs(scale.x - scale.y) <= 1e-6 * scale.x && std::abs(scale.x - scale.z) <= 1e-6 * scale.x;
		bool axisAligned = true;
		for (int c = 0; c < 3; c++) {
			for (int r = 0; r < 3; r++) {
				axisAligned = axisAligned && (r == c || std::abs(linear[c][r]) <= 1e-9 * scale[c]);
			}
		}
		const double area = 4.0 * PI * radius * radius * scale.x * scale.x;
		const std::optional<uint32_t> material = shapeMaterial(shape, area);
		if (!material.has_value()) {
			return;
		}
		if (uniform || axisAligned) {
			ImportedShape imported;
			imported.shape = makeShape(ShapeKind::Sphere);
			imported.shape.name = name;
			imported.shape.position = toVec3(glm::dvec3(world[3]));
			imported.shape.size = toVec3(radius * scale);
			imported.material = *material;
			m.shapes.push_back(imported);
			return;
		}
		report("sphere under a sheared transform is triangles", shape.loc, PLAN_SHAPES);
		MeshData mesh;
		sphereMesh(radius, mesh);
		if (std::optional<uint32_t> index = buildMesh(shape, std::move(mesh), *material)) {
			m.nodes.push_back({ name, *index, toMat4(world) });
		}
	}

	// one shape, placed at worldFromInstance * its own transform (the identity outside an instance)
	void placeShape(const PbrtShape& shape, const glm::dmat4& worldFromInstance, const PbrtLoc& where)
	{
		shapeCommon(shape);
		const glm::dmat4 worldFromObject = worldFromInstance * shape.worldFromObject;
		const std::string name = fmt::format("{} {}", shape.type, m.nodes.size() + m.shapes.size());
		if (shape.type == "sphere") {
			placeSphere(shape, worldFromObject, name);
			reportUnused(shape.params, "Shape \"sphere\"");
			return;
		}
		std::optional<MeshData> data = shapeGeometry(shape);
		if (!data.has_value()) {
			return;
		}
		const double area = shape.areaLight >= 0 ? meshArea(*data, worldFromObject) : 0.0;
		const std::optional<uint32_t> material = shapeMaterial(shape, area);
		reportUnused(shape.params, fmt::format("Shape \"{}\"", shape.type));
		if (!material.has_value()) {
			return;
		}
		if (std::optional<uint32_t> mesh = buildMesh(shape, std::move(*data), *material)) {
			m.nodes.push_back({ name, *mesh, toMat4(W * worldFromObject) });
		}
		(void)where;
	}

	void instances()
	{
		for (const PbrtInstance& instance : s.instances) {
			const auto object = s.objects.find(instance.object);
			if (object == s.objects.end()) {
				report(fmt::format("ObjectInstance of undefined object \"{}\"", instance.object), instance.loc);
				continue;
			}
			if (instance.animated) {
				report("an instance's transform is animated; the shutter-open pose is used", instance.loc, PLAN_MOTION);
			}
			std::vector<int>& meshes = m_objectMeshes[instance.object];
			const bool first = meshes.empty();
			if (first) {
				meshes.assign(object->second.shapes.size(), -1);
			}
			for (size_t i = 0; i < object->second.shapes.size(); i++) {
				const PbrtShape& shape = object->second.shapes[i];
				const glm::dmat4 world = W * instance.worldFromInstance * shape.worldFromObject;
				const std::string name = fmt::format("{} {}", instance.object, m.nodes.size() + m.shapes.size());
				if (shape.type == "sphere") {
					shapeCommon(shape);
					placeSphere(shape, instance.worldFromInstance * shape.worldFromObject, name);
					continue;
				}
				if (first) {
					shapeCommon(shape);
					std::optional<MeshData> data = shapeGeometry(shape);
					const std::optional<uint32_t> material = data.has_value() ? shapeMaterial(shape, 0.0) : std::nullopt;
					reportUnused(shape.params, fmt::format("Shape \"{}\"", shape.type));
					if (data.has_value() && material.has_value()) {
						meshes[i] = buildMesh(shape, std::move(*data), *material).value_or(-1);
					}
				}
				if (meshes[i] >= 0) {
					m.nodes.push_back({ name, (uint32_t)meshes[i], toMat4(world) });
				}
			}
		}
		for (const auto& [name, object] : s.objects) {
			if (m_objectMeshes.count(name) == 0) {
				report(fmt::format("object \"{}\" is defined but never instanced", name), object.loc);
			}
		}
	}

	//---------------------------------------------------------------- lights

	// a light's spectrum parameter as RGB with pbrt's normalization, and "scale"; the spectrum itself, for addLight(),
	// in m_lightSpectrum
	glm::dvec3 lightRgb(const PbrtLight& light, const char* name, double& scale)
	{
		scale = light.params.getFloat("scale", 1.0);
		const Emission emission = emissionOf(light.params.find(name));
		m_lightSpectrum = emission.color;
		return emission.rgb;
	}

	void addLight(SceneLight light, const glm::dvec3& rgb, double scale)
	{
		const double peak = std::max(maxComponent(rgb), 1e-30);
		light.color = rgbSpectrum(toVec3(rgb / peak));
		light.intensity = (float)(peak * scale);
		//a spectrum is kept as it is, at luminance 1 by its role, times the scale (rgb is its projection times any
		//"power" factor, so the factor is what rgb's luminance adds to 1)
		if (m_lightSpectrum.kind != SpectrumKind::Rgb) {
			const glm::dvec3 unit = glm::max(glm::dvec3(exactRgb(m_lightSpectrum, SpectrumRole::Illuminant, {})), glm::dvec3(0.0));
			const double factor = maxComponent(unit) > 0.0 ? maxComponent(rgb) / maxComponent(unit) : 1.0;
			light.color = m_lightSpectrum;
			light.intensity = (float)(factor * scale);
		}
		m_lightSpectrum = rgbSpectrum(glm::vec3(1.f));
		light.name = fmt::format("{} {}", lightKindName(light.kind), m.lights.size() + 1);
		m.lights.push_back(light);
	}

	void lights()
	{
		bool haveEnvironment = false;
		for (const PbrtLight& light : s.lights) {
			const PbrtParams& p = light.params;
			if (light.type == "point") {
				double scale = 1.0;
				const glm::dvec3 I = lightRgb(light, "I", scale);
				const double power = p.getFloat("power", -1.0);
				if (power > 0.0) {
					scale *= power / (4.0 * PI * (0.2126 * I.x + 0.7152 * I.y + 0.0722 * I.z));
				}
				SceneLight out;
				out.kind = LightKind::Point;
				out.position = toVec3(worldPoint(light.worldFromLight, p.getVector3("from").value_or(glm::dvec3(0.0))));
				addLight(out, I, scale);
			} else if (light.type == "spot") {
				double scale = 1.0;
				const glm::dvec3 I = lightRgb(light, "I", scale);
				const glm::dvec3 from = p.getVector3("from").value_or(glm::dvec3(0.0));
				const glm::dvec3 to = p.getVector3("to").value_or(glm::dvec3(0.0, 0.0, 1.0));
				const double cone = p.getFloat("coneangle", 30.0);
				const double delta = p.getFloat("conedeltaangle", 5.0);
				const double power = p.getFloat("power", -1.0);
				const double cosEnd = std::cos(glm::radians(cone));
				const double cosStart = std::cos(glm::radians(cone - delta));
				if (power > 0.0) {
					const double k = 2.0 * PI * ((1.0 - cosStart) + (cosStart - cosEnd) / 2.0);
					scale *= power / (k * (0.2126 * I.x + 0.7152 * I.y + 0.0722 * I.z));
				}
				SceneLight out;
				out.kind = LightKind::Spot;
				out.position = toVec3(worldPoint(light.worldFromLight, from));
				out.orientation = rotationTo(worldVector(light.worldFromLight, to - from));
				out.outerConeDegrees = (float)cone;
				out.innerConeDegrees = (float)std::max(cone - delta, 0.0);
				addLight(out, I, scale);
				report("spot light falls off as the square of a linear ramp, pbrt's as a smoothstep", light.loc, PLAN_LIGHTS);
			} else if (light.type == "distant") {
				double scale = 1.0;
				const glm::dvec3 L = lightRgb(light, "L", scale);
				const glm::dvec3 from = p.getVector3("from").value_or(glm::dvec3(0.0));
				const glm::dvec3 to = p.getVector3("to").value_or(glm::dvec3(0.0, 0.0, 1.0));
				const double illuminance = p.getFloat("illuminance", -1.0);
				if (illuminance > 0.0) {
					scale *= illuminance;
				}
				SceneLight out;
				out.kind = LightKind::Directional;
				out.position = toVec3(worldPoint(light.worldFromLight, from));
				//the direction the light travels: from `from` towards `to`
				out.orientation = rotationTo(worldVector(light.worldFromLight, to - from));
				addLight(out, L, scale);
			} else if (light.type == "infinite") {
				if (haveEnvironment) {
					report("a second infinite light is ignored: the renderer has one environment", light.loc, PLAN_ENVIRONMENT);
					for (const PbrtParam& param : p.list) {
						param.used = true;
					}
					continue;
				}
				haveEnvironment = environment(light);
			} else if (light.type == "goniometric" || light.type == "projection") {
				double scale = 1.0;
				const glm::dvec3 I = lightRgb(light, "I", scale);
				SceneLight out;
				out.kind = LightKind::Point;
				out.position = toVec3(worldPoint(light.worldFromLight, glm::dvec3(0.0)));
				addLight(out, I, scale);
				p.find("filename");
				p.find("fov");
				p.find("power");
				report(fmt::format("{} light is a point light without its image", light.type), light.loc, PLAN_LIGHTS);
			} else {
				report(fmt::format("LightSource \"{}\" is not supported; skipped", light.type), light.loc, PLAN_LIGHTS);
				continue;
			}
			if (!light.medium.empty()) {
				report("a light's medium is ignored", light.loc, PLAN_MEDIA);
			}
			reportUnused(p, fmt::format("LightSource \"{}\"", light.type));
		}
	}

	// pbrt's infinite light as the renderer's equirectangular environment, rotated and mirrored into its world
	bool environment(const PbrtLight& light)
	{
		const PbrtParams& p = light.params;
		ImportedEnvironment env;
		double scale = p.getFloat("scale", 1.0);
		if (p.find("portal") != nullptr) {
			report("infinite light portal is ignored", light.loc, PLAN_ENVIRONMENT);
		}
		const std::string filename = p.getString("filename", "");
		if (filename.empty()) {
			double unused = 1.0;
			const glm::dvec3 L = lightRgb(light, "L", unused);
			env.width = 64;
			env.height = 32;
			env.rgba.assign((size_t)env.width * env.height * 4, 1.f);
			for (size_t i = 0; i < (size_t)env.width * env.height; i++) {
				env.rgba[i * 4 + 0] = (float)L.x;
				env.rgba[i * 4 + 1] = (float)L.y;
				env.rgba[i * 4 + 2] = (float)L.z;
			}
			env.label = "pbrt uniform infinite light";
		} else {
			std::filesystem::path path = filename;
			if (path.is_relative()) {
				path = s.directory / path;
			}
			Image image;
			const ImageIoStatus status = loadImage(path, image);
			if (!status.ok) {
				report(fmt::format("infinite light image is unreadable ({}); no environment", status.message), light.loc);
				return false;
			}
			if (image.width != image.height) {
				report("infinite light image is not square, so not pbrt's equal-area layout; read as equirectangular", light.loc);
				env.width = image.width;
				env.height = image.height;
				env.rgba.resize((size_t)image.width * image.height * 4, 1.f);
				for (size_t i = 0; i < image.pixelCount(); i++) {
					for (int c = 0; c < 3; c++) {
						env.rgba[i * 4 + c] = image.rgb[i * 3 + c];
					}
				}
			} else {
				const double illuminance = p.getFloat("illuminance", -1.0);
				if (illuminance > 0.0) {
					//pbrt measures it on an upward-facing patch in the light's own space (+z up)
					double measured = 0.0;
					for (uint32_t y = 0; y < image.height; y++) {
						for (uint32_t x = 0; x < image.width; x++) {
							const glm::vec3 w = equalAreaSquareToSphere({ (x + 0.5f) / image.width, (y + 0.5f) / image.height });
							if (w.z <= 0.f) {
								continue;
							}
							const float* t = &image.rgb[((size_t)y * image.width + x) * 3];
							measured += (0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2]) * w.z;
						}
					}
					measured *= 2.0 * PI / ((double)image.width * image.height / 2.0);
					if (measured > 0.0) {
						scale *= illuminance / measured;
					}
				}
				resampleEqualArea(image, light.worldFromLight, env);
				env.label = path.filename().string();
			}
		}
		env.intensity = (float)scale;
		m.environment = std::move(env);
		return true;
	}

	// each equirectangular texel takes 2 x 2 nearest lookups in pbrt's map, as pbrt looks up its texels. Nearest lookups
	// alone do not keep the map's energy: sanmiguel's sun is two texels holding half the sky's light, and how many lookups
	// land on them depends on how the two grids line up (its sky came out 15 percent bright stood on pbrt's y, 1.5 percent
	// dark on z). So a lookup is weighed by its source texel's solid angle, 4 pi / n^2 in the equal-area square, over the
	// solid angle of all the lookups that land on that texel, and every source texel gives exactly its own light
	void resampleEqualArea(const Image& image, const glm::dmat4& worldFromLight, ImportedEnvironment& env)
	{
		const uint32_t n = image.width;
		env.width = 2 * n;
		env.height = n;
		env.rgba.assign((size_t)env.width * env.height * 4, 1.f);
		const glm::dmat3 lightFromWorld = glm::inverse(glm::dmat3(worldFromLight));
		const glm::dmat3 pbrtFromRenderer = glm::inverse(glm::dmat3(W));

		//every lookup's source texel, and the solid angle that lands on each source texel
		std::vector<uint32_t> source((size_t)env.width * env.height * 4);
		std::vector<double> covered((size_t)n * n, 0.0);
		std::vector<double> lookupSolidAngle(env.height);
		for (uint32_t y = 0; y < env.height; y++) {
			//a row's texel spans latitudes (0.5 - y / height) pi to (0.5 - (y + 1) / height) pi (equirectDirectionOf())
			const double top = std::sin((0.5 - (double)y / env.height) * glm::pi<double>());
			const double bottom = std::sin((0.5 - (double)(y + 1) / env.height) * glm::pi<double>());
			lookupSolidAngle[y] = (top - bottom) * 2.0 * glm::pi<double>() / env.width / 4.0;
			for (uint32_t x = 0; x < env.width; x++) {
				for (int sy = 0; sy < 2; sy++) {
					for (int sx = 0; sx < 2; sx++) {
						const glm::vec2 uv((x + (sx + 0.5f) / 2.f) / env.width, (y + (sy + 0.5f) / 2.f) / env.height);
						const glm::dvec3 d = glm::dvec3(equirectDirectionOf(uv));
						const glm::vec3 local = glm::normalize(toVec3(lightFromWorld * (pbrtFromRenderer * d)));
						const glm::vec2 st = equalAreaSphereToSquare(local);
						const uint32_t px = std::min((uint32_t)(st.x * n), n - 1);
						const uint32_t py = std::min((uint32_t)(st.y * n), n - 1);
						const uint32_t texel = py * n + px;
						source[((size_t)y * env.width + x) * 4 + (size_t)(sy * 2 + sx)] = texel;
						covered[texel] += lookupSolidAngle[y];
					}
				}
			}
		}

		const double texelSolidAngle = 4.0 * glm::pi<double>() / ((double)n * n);
		for (uint32_t y = 0; y < env.height; y++) {
			for (uint32_t x = 0; x < env.width; x++) {
				glm::dvec3 sum(0.0);
				for (int k = 0; k < 4; k++) {
					const uint32_t texel = source[((size_t)y * env.width + x) * 4 + (size_t)k];
					const float* t = &image.rgb[(size_t)texel * 3];
					sum += glm::dvec3(t[0], t[1], t[2]) * (texelSolidAngle / covered[texel]);
				}
				float* out = &env.rgba[((size_t)y * env.width + x) * 4];
				out[0] = (float)std::max(sum.x / 4.0, 0.0);
				out[1] = (float)std::max(sum.y / 4.0, 0.0);
				out[2] = (float)std::max(sum.z / 4.0, 0.0);
			}
		}
	}

	//---------------------------------------------------------------- camera and settings

	// the film's resolution, which the camera's field of view depends on
	glm::ivec2 filmResolution() const
	{
		return { s.film.params.getInt("xresolution", 1280), s.film.params.getInt("yresolution", 720) };
	}

	void camera()
	{
		SceneCamera camera;
		camera.name = s.camera.has_value() && !s.files.empty() ? s.files[0].stem().string() : "pbrt camera";
		const glm::dmat4 cameraFromWorld = s.camera.has_value() ? s.camera->cameraFromWorld : glm::dmat4(1.0);
		const glm::dmat4 mirrorZ = glm::scale(glm::dmat4(1.0), glm::dvec3(1.0, 1.0, -1.0));
		const glm::dmat4 worldFromCamera = W * glm::inverse(cameraFromWorld) * mirrorZ;
		glm::dmat3 basis(worldFromCamera);
		for (int i = 0; i < 3; i++) {
			basis[i] = glm::normalize(basis[i]);
		}
		camera.position = toVec3(glm::dvec3(worldFromCamera[3]));
		camera.orientation = glm::normalize(glm::quat_cast(glm::mat3(basis)));

		const glm::ivec2 resolution = filmResolution();
		double aspect = (double)resolution.x / std::max(resolution.y, 1);
		if (!s.camera.has_value()) {
			report("no Camera directive; pbrt's default perspective camera at the origin is used", PbrtLoc {});
			camera.vfovDegrees = 90.f;
		} else {
			const PbrtParams& p = s.camera->params;
			aspect = p.getFloat("frameaspectratio", aspect);
			if (p.find("screenwindow") != nullptr) {
				report("camera screenwindow is ignored", s.camera->loc, PLAN_CAMERAS);
			}
			if (s.camera->type != "perspective") {
				report(fmt::format("{} camera is a perspective camera with the same pose", s.camera->type), s.camera->loc, PLAN_CAMERAS);
				for (const PbrtParam& param : p.list) {
					param.used = true;
				}
			}
			//pbrt's fov spans the shorter image axis; the renderer's is vertical
			const double fov = glm::radians(p.getFloat("fov", 90.0));
			const double vfov = aspect >= 1.0 ? fov : 2.0 * std::atan(std::tan(fov / 2.0) / aspect);
			camera.vfovDegrees = (float)glm::degrees(vfov);
			const double lensRadius = p.getFloat("lensradius", 0.0);
			const double focalDistance = p.getFloat("focaldistance", 1e6);
			if (lensRadius > 0.0) {
				camera.aperture = (float)(2.0 * lensRadius);
				camera.focusDistance = (float)focalDistance;
			}
			const double open = p.getFloat("shutteropen", 0.0);
			const double close = p.getFloat("shutterclose", 1.0);
			const double imagingRatio = (close - open) * s.film.params.getFloat("iso", 100.0) / 100.0;
			camera.exposure = (float)imagingRatio;
			if (std::abs(imagingRatio - 1.0) > 1e-6) {
				report(fmt::format("pbrt's imaging ratio {} (shutter x ISO / 100) is the camera's exposure, which an EXR export leaves out",
					imagingRatio), s.camera->loc, PLAN_DISPLAY);
			}
			reportUnused(p, fmt::format("Camera \"{}\"", s.camera->type));
		}
		m.cameras.push_back(camera);
	}

	void settings()
	{
		ImportedSettings out;
		RenderSettings& r = out.render;

		//the film
		const PbrtEntity& film = s.film;
		const glm::ivec2 resolution = filmResolution();
		r.width = std::clamp(resolution.x, MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION);
		r.height = std::clamp(resolution.y, MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION);
		const std::string filmType = film.type.empty() ? "rgb" : film.type;
		film.params.find("filename");
		film.params.find("savefp16");
		film.params.find("diagonal");
		const double maxComponentValue = film.params.getFloat("maxcomponentvalue", std::numeric_limits<double>::infinity());
		if (std::isfinite(maxComponentValue)) {
			r.clampSamples = true;
			r.clampValue = (float)maxComponentValue;
		}
		if (film.params.find("cropwindow") != nullptr || film.params.find("pixelbounds") != nullptr) {
			report("film cropwindow / pixelbounds is ignored: the whole image is rendered", film.loc);
		}
		if (film.params.find("whitebalance") != nullptr && film.params.getFloat("whitebalance", 0.0) != 0.0) {
			report("film white balance is ignored", film.loc, PLAN_DISPLAY);
		}
		if (const std::string sensor = film.params.getString("sensor", "cie1931"); sensor != "cie1931") {
			report(fmt::format("film sensor \"{}\" is the CIE 1931 observer", sensor), film.loc, PLAN_DISPLAY);
		}
		if (filmType == "gbuffer") {
			for (AovOutput output : { AovOutput::Position, AovOutput::Depth, AovOutput::Normal, AovOutput::ShadingNormal, AovOutput::Uv,
					 AovOutput::Albedo, AovOutput::Variance }) {
				r.aov.set(output, true);
			}
			if (film.params.getString("coordinatesystem", "camera") != "world") {
				report("gbuffer film positions and normals are in world space, not camera space", film.loc);
			}
		} else if (filmType == "spectral") {
			//pbrt's SpectralFilm: hero wavelengths, with its buckets kept as the spectral images
			r.spectral.representation = SpectralRepresentation::HeroWavelengths;
			r.spectral.imageBuckets = (uint32_t)std::clamp(film.params.getInt("nbuckets", 16), 1, 256);
			r.spectral.lambdaMin = (float)film.params.getFloat("lambdamin", 360.0);
			r.spectral.lambdaMax = (float)film.params.getFloat("lambdamax", 830.0);
			r.spectral = clampSpectralSettings(r.spectral);
		} else if (filmType != "rgb") {
			report(fmt::format("Film \"{}\" is an RGB film", filmType), film.loc);
		}
		if (s.colorSpace != "srgb") {
			report(fmt::format("color space \"{}\" is read as sRGB", s.colorSpace), PbrtLoc {}, PLAN_DISPLAY);
		}
		reportUnused(film.params, fmt::format("Film \"{}\"", filmType));

		//the sampler
		const PbrtEntity& sampler = s.sampler;
		const std::string samplerType = sampler.type.empty() ? "zsobol" : sampler.type;
		r.maxSamples = sampler.params.getInt("pixelsamples", 16);
		r.unlimitedSamples = false;
		r.useFixedSeed = true;
		r.seed = (uint32_t)std::max(sampler.params.getInt("seed", 0), 0) + 1;
		static const std::map<std::string, std::string> samplers {
			{ "halton", "halton" }, { "sobol", "sobol" }, { "zsobol", "zsobol" }, { "paddedsobol", "padded_sobol" },
			{ "pmj02bn", "pmj02bn" }, { "independent", "independent" }, { "stratified", "stratified" },
		};
		if (const auto found = samplers.find(samplerType); found != samplers.end()) {
			out.sampler = found->second;
		} else {
			report(fmt::format("Sampler \"{}\" is ZSobol", samplerType), sampler.loc);
			out.sampler = "zsobol";
		}
		if (samplerType == "stratified") {
			r.maxSamples = sampler.params.getInt("xsamples", 4) * sampler.params.getInt("ysamples", 4);
			sampler.params.find("jitter");
		}
		//pbrt's default scramble for each sampler
		const std::string randomization = sampler.params.getString("randomization", samplerType == "halton" ? "permutedigits" : "fastowen");
		static const std::map<std::string, SamplerRandomization> randomizations {
			{ "none", SamplerRandomization::None }, { "permutedigits", SamplerRandomization::PermuteDigits },
			{ "fastowen", SamplerRandomization::FastOwen }, { "owen", SamplerRandomization::Owen },
		};
		r.samplerRandomization = randomizations.count(randomization) != 0 ? randomizations.at(randomization) : SamplerRandomization::FastOwen;
		reportUnused(sampler.params, fmt::format("Sampler \"{}\"", samplerType));

		//the pixel filter
		const PbrtEntity& filter = s.filter;
		const std::string filterType = filter.type.empty() ? "gaussian" : filter.type;
		static const std::map<std::string, PixelFilterKind> filters {
			{ "box", PixelFilterKind::Box }, { "triangle", PixelFilterKind::Triangle }, { "gaussian", PixelFilterKind::Gaussian },
			{ "mitchell", PixelFilterKind::Mitchell }, { "sinc", PixelFilterKind::Lanczos }, { "lanczos", PixelFilterKind::Lanczos },
		};
		const auto kind = filters.find(filterType);
		if (kind == filters.end()) {
			report(fmt::format("PixelFilter \"{}\" is Gaussian", filterType), filter.loc);
		}
		PixelFilterSettings pixelFilter = defaultPixelFilter(kind != filters.end() ? kind->second : PixelFilterKind::Gaussian);
		const double xRadius = filter.params.getFloat("xradius", pixelFilter.radius);
		const double yRadius = filter.params.getFloat("yradius", pixelFilter.radius);
		if (std::abs(xRadius - yRadius) > 1e-6) {
			report("a filter with two radii has the larger on both axes", filter.loc);
		}
		pixelFilter.radius = (float)std::clamp(std::max(xRadius, yRadius), (double)MIN_FILTER_RADIUS, (double)MAX_FILTER_RADIUS);
		pixelFilter.sigma = (float)filter.params.getFloat("sigma", pixelFilter.sigma);
		pixelFilter.b = (float)filter.params.getFloat("B", pixelFilter.b);
		pixelFilter.c = (float)filter.params.getFloat("C", pixelFilter.c);
		pixelFilter.tau = (float)filter.params.getFloat("tau", pixelFilter.tau);
		r.pixelFilter = pixelFilter;
		reportUnused(filter.params, fmt::format("PixelFilter \"{}\"", filterType));

		//the integrator
		const PbrtEntity& integrator = s.integrator;
		const std::string integratorType = integrator.type.empty() ? "volpath" : integrator.type;
		r.rayDepth = integrator.params.getInt("maxdepth", 5) + 1;
		if (integratorType == "ambientocclusion") {
			out.ambientOcclusionDistance = (float)integrator.params.getFloat("maxdistance", std::numeric_limits<float>::infinity());
			integrator.params.find("cossample");
		} else if (integratorType == "bdpt" || integratorType == "lightpath") {
			report(fmt::format("Integrator \"{}\" is the path tracer", integratorType), integrator.loc, PLAN_BDPT);
		} else if (integratorType == "sppm") {
			report("Integrator \"sppm\" is the path tracer", integrator.loc, PLAN_PHOTONS);
		} else if (integratorType == "mlt") {
			report("Integrator \"mlt\" is the path tracer", integrator.loc, PLAN_MLT);
		} else if (integratorType != "path" && integratorType != "volpath") {
			report(fmt::format("Integrator \"{}\" is the path tracer", integratorType), integrator.loc);
		}
		if (integrator.params.getBool("regularize", false)) {
			report("path regularization is off", integrator.loc, PLAN_REGULARIZATION);
		}
		if (integratorType == "path" || integratorType == "volpath") {
			const std::string lightSampler = integrator.params.getString("lightsampler", "bvh");
			if (lightSampler != "power") {
				report(fmt::format("light sampler \"{}\" is the power sampler: the noise differs, not the converged image", lightSampler),
					integrator.loc, PLAN_LIGHT_SAMPLER);
			}
		}
		if (integratorType == "sppm" || integratorType == "bdpt" || integratorType == "mlt" || integratorType == "lightpath") {
			for (const PbrtParam& p : integrator.params.list) {
				p.used = true;
			}
		}
		reportUnused(integrator.params, fmt::format("Integrator \"{}\"", integratorType));

		if (!s.accelerator.type.empty()) {
			report(fmt::format("Accelerator \"{}\" is the scene's BVH", s.accelerator.type), s.accelerator.loc, PLAN_ACCELERATORS);
			for (const PbrtParam& p : s.accelerator.params.list) {
				p.used = true;
			}
		}
		for (const auto& [name, value] : s.options) {
			report(fmt::format("Option \"{}\" is ignored", name), PbrtLoc {});
		}

		out.imagingRatio = 1.f;
		if (s.camera.has_value()) {
			const double open = s.camera->params.getFloat("shutteropen", 0.0);
			const double close = s.camera->params.getFloat("shutterclose", 1.0);
			out.imagingRatio = (float)((close - open) * s.film.params.getFloat("iso", 100.0) / 100.0);
		}
		m.settings = out;
	}
};

}

void ImportReport::add(const std::string& text, const std::string& where, const std::string& plan)
{
	for (ImportMessage& message : messages) {
		if (message.text == text) {
			message.count++;
			return;
		}
	}
	messages.push_back({ text, where, 1, plan });
}

glm::vec3 equalAreaSquareToSphere(glm::vec2 p)
{
	const float u = 2.f * p.x - 1.f;
	const float v = 2.f * p.y - 1.f;
	const float up = std::abs(u);
	const float vp = std::abs(v);
	const float signedDistance = 1.f - (up + vp);
	const float d = std::abs(signedDistance);
	const float r = 1.f - d;
	const float phi = (r == 0.f ? 1.f : (vp - up) / r + 1.f) * (float)PI / 4.f;
	const float z = std::copysign(1.f - r * r, signedDistance);
	const float cosPhi = std::copysign(std::cos(phi), u);
	const float sinPhi = std::copysign(std::sin(phi), v);
	const float s = r * std::sqrt(std::max(2.f - r * r, 0.f));
	return glm::vec3(cosPhi * s, sinPhi * s, z);
}

glm::vec2 equalAreaSphereToSquare(glm::vec3 d)
{
	const float x = std::abs(d.x);
	const float y = std::abs(d.y);
	const float z = std::abs(d.z);
	const float r = std::sqrt(std::max(1.f - z, 0.f));
	const float a = std::max(x, y);
	float b = std::min(x, y);
	b = a == 0.f ? 0.f : b / a;
	//pbrt's minimax polynomial for atan(b) * 2 / pi on [0, 1]
	const float t1 = 0.406758566246788489601959989e-5f;
	const float t2 = 0.636226545274016134946890922156f;
	const float t3 = 0.61572017898280213493197203466e-2f;
	const float t4 = -0.247333733281268944196501420480f;
	const float t5 = 0.881770664775316294736387951347e-1f;
	const float t6 = 0.419038818029165735901852432784e-1f;
	const float t7 = -0.251390972343483509333252996350e-1f;
	float phi = t1 + b * (t2 + b * (t3 + b * (t4 + b * (t5 + b * (t6 + b * t7)))));
	if (x < y) {
		phi = 1.f - phi;
	}
	float v = phi * r;
	float u = r - v;
	if (d.z < 0.f) {
		std::swap(u, v);
		u = 1.f - u;
		v = 1.f - v;
	}
	u = std::copysign(u, d.x);
	v = std::copysign(v, d.y);
	return glm::vec2(0.5f * (u + 1.f), 0.5f * (v + 1.f));
}

void importPbrtScene(const PbrtScene& scene, ImportedModel& model)
{
	model = ImportedModel {};
	Importer(scene, model).run();
}

bool importPbrtFile(const std::filesystem::path& path, ImportedModel& model, std::string* error)
{
	PbrtScene scene;
	PbrtParseOptions options;
	//millions of hair strands: counted and reported, never kept
	options.skipShapes.insert("curve");
	if (!parsePbrtFile(path, scene, error, options)) {
		return false;
	}
	importPbrtScene(scene, model);
	return true;
}
