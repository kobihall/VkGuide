#pragma once

// THE MATERIAL MODEL: what a material is, as data, and the one table that knows each type.
//
// A material is a MaterialDef: a type, and its parameters stored BY NAME in a ParamSet, every value
// ever set, so switching a material from one type to another and back loses nothing, and two types
// that share a parameter name (albedo, strength) share its value. The scene's materials are a
// library of these (RaytraceSceneEditor::materials()), which shapes and mesh objects reference by
// id; a glTF model's import adds one entry per glTF material, whose edits are saved as overrides.
//
// MATERIAL_TRAITS (materialTraits()), indexed by MaterialType, plays the role SHAPE_TRAITS plays for
// shapes: the only per-type knowledge outside the shaders. Each row names the type, lists its
// parameters (what the editor draws and the scene file saves), flags it basic or complex, and packs
// a material into the GPU record its shader case reads (crt_material.glsl). ADDING A TYPE is a row
// here, its parameters, its pack function, and its case in the shaders.
//
// A color parameter is a SpectrumRef (src/spectrum_ref.h): an RGB triple by default, or any spectrum, which the
// parameter's SpectrumRole says how to read (docs/plans/completed/pbrt-spectral-rendering.md). Until
// docs/plans/pbrt-textures.md lands a TexRef is a layer of the texture array: exactly what the tracer stored before
// parameters were data.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

#include <spectrum_ref.h>

//---------------------------------------------------------------- types

// the values are the GPU's tags (crt_common.glsl CRT_MATERIAL_*): append only
enum class MaterialType : uint8_t {
	Lambertian,
	Metal,
	Phong,
	Dielectric,
	// a light: every path that reaches it ends there, carrying albedo x strength home. From either
	// side, as RTNW's diffuse_light
	Emissive,
	// glTF 2.0's metallic-roughness model: a GGX specular lobe over a Lambertian base, blended by
	// metallic, plus an emission that does not end the path. What every glTF material becomes
	Pbr,
	// pbrt-v4's dielectric: smooth or rough glass with exact Fresnel, its index any spectrum, dispersive when the index
	// varies with wavelength (docs/plans/completed/pbrt-bsdf-library.md)
	Glass,
	// pbrt-v4's conductor: smooth or rough metal with complex Fresnel from its eta and k spectra, or from a reflectance
	Conductor,
	// pbrt-v4's diffuse: Lambertian reflection, or Oren-Nayar's rough diffuse surface when sigma is above 0
	Diffuse,
	// pbrt-v4's thin dielectric: a pane that transmits straight through, with the Fresnel term of two parallel interfaces
	ThinGlass,
	// pbrt-v4's diffuse transmission: Lambertian reflection and transmission
	DiffuseTransmission,
	// pbrt-v4's mix: each hit takes one of two other library materials, the second with the chance `amount`
	Mix
};

inline constexpr MaterialType MATERIAL_TYPES[] = {
	MaterialType::Lambertian,
	MaterialType::Metal,
	MaterialType::Phong,
	MaterialType::Dielectric,
	MaterialType::Emissive,
	MaterialType::Pbr,
	MaterialType::Glass,
	MaterialType::Conductor,
	MaterialType::Diffuse,
	MaterialType::ThinGlass,
	MaterialType::DiffuseTransmission,
	MaterialType::Mix,
};

inline constexpr size_t MATERIAL_TYPE_COUNT = sizeof(MATERIAL_TYPES) / sizeof(MATERIAL_TYPES[0]);

//---------------------------------------------------------------- parameters

// a texture a parameter reads, or none (pbrt-textures.md): a layer of RaytraceTextureArray until
// then. Only a glTF material's import sets one, and only for this session: the layers are
// re-resolved whenever the array is rebuilt, and a texture reference is never saved
struct TexRef {
	int layer { -1 };
	bool operator==(const TexRef&) const = default;
};

// A reference to another library material (a mix's children), by the id the library gives it. The id is never saved: the
// scene file writes the reference as it writes a shape's, an index into its user materials or a glTF material by model
// and index (src/scene_io.h SceneMaterialRef). The `file` fields carry that reference on the copies a SceneDescription
// holds, and are -1 everywhere else
struct MaterialLink {
	uint64_t id { 0 };
	int fileUser { -1 };
	int fileModel { -1 };
	uint32_t fileModelMaterial { 0 };
	// An import's mix names its children by their index in the model's own materials, before the library has given
	// them ids. VulkanEngine::addModel() turns it into an id; -1 everywhere else
	int modelIndex { -1 };
	bool operator==(const MaterialLink&) const = default;
};

enum class ParamKind : uint8_t {
	Float,
	Spectrum,
	Texture,
	Enum,
	Bool,
	Material
};

// one alternative per ParamKind, in ParamKind's order
using ParamValue = std::variant<float, SpectrumRef, TexRef, int, bool, MaterialLink>;
using ParamSet = std::map<std::string, ParamValue, std::less<>>;

// MaterialParam::flags
inline constexpr uint8_t PARAM_LOGARITHMIC = 1u << 0;
// a Spectrum shown as a color picker
inline constexpr uint8_t PARAM_COLOR = 1u << 1;
// a value that can later be read from a texture (pbrt-textures.md)
inline constexpr uint8_t PARAM_TEXTURABLE = 1u << 2;
// shown, never edited, never saved: derived from where the material came from (a glTF material's
// textures and alpha cutoff)
inline constexpr uint8_t PARAM_READ_ONLY = 1u << 3;

struct MaterialParam {
	// saved in the scene file; never rename
	const char* name;
	const char* label;
	ParamKind kind;
	ParamValue defaultValue;
	// the slider's range
	float min;
	float max;
	uint8_t flags;
	// shown when the widget is hovered; null for none
	const char* tooltip;
	// a Spectrum parameter: what its value means, which decides how an RGB value becomes a spectrum and how a
	// spectrum becomes exact RGB's color (src/color_space.h)
	SpectrumRole role { SpectrumRole::Albedo };
};

//---------------------------------------------------------------- materials

// a glTF material: its model's key in VulkanEngine::m_models and its index in the file's materials.
// The physoptics plans add an optical element's slot as a second kind of origin
struct ModelMaterialOrigin {
	std::string modelKey;
	uint32_t index { 0 };
	bool operator==(const ModelMaterialOrigin&) const = default;
};

struct MaterialDef {
	// stable, shares the scene editor's id space; never saved
	uint64_t id { 0 };
	std::string name;
	MaterialType type { MaterialType::Lambertian };
	// by parameter name, every value ever set. A name missing here reads as its default
	ParamSet params;
	// empty for a user material, else the glTF material it was imported from
	std::optional<ModelMaterialOrigin> origin;
	// for a glTF material: the parameters the user changed from the imported values, which are all
	// the scene file saves of it
	std::set<std::string, std::less<>> edited;
	// the light group (0 to 7) what the material emits is kept in, beside the picture
	// (docs/plans/completed/pbrt-aovs-and-image-output.md). A material that does not emit has none to use
	uint32_t lightGroup { 0 };

	bool operator==(const MaterialDef&) const = default;
};

// a new user material of `type` with every parameter at its default, and the albedo given
MaterialDef makeMaterial(MaterialType type, const glm::vec3& albedo = glm::vec3(0.7f));

// a parameter's value: the material's own, else its type's default for that name, else the default
// any type gives it. Asking for the wrong kind returns the kind's zero
ParamValue materialValue(const MaterialDef& material, std::string_view name);
float materialFloat(const MaterialDef& material, std::string_view name);
// a Spectrum parameter as exact RGB reads it: an RGB value in the working space as linear sRGB (the authored triple in
// an sRGB scene), any other spectrum projected by the parameter's role
glm::vec3 materialColor(const MaterialDef& material, std::string_view name, const SpectrumContext& context = {});
// a Spectrum parameter as it is, and the role its type gives it (Albedo for a name the type does not have)
SpectrumRef materialSpectrum(const MaterialDef& material, std::string_view name);
SpectrumRole materialSpectrumRole(MaterialType type, std::string_view name);
int materialTextureLayer(const MaterialDef& material, std::string_view name);

//---------------------------------------------------------------- the GPU record

// what the pack functions are told about where the material is used. A shape has no textures
// until pbrt-textures.md adds them, so a material on a shape is packed without its texture layers
struct PackContext {
	bool textures { true };
	// the working space, which exact RGB converts a material's RGB values from
	SpectrumContext spectra;
	// what exact RGB reads a dispersive index at (src/spectral.h SpectralSettings::designWavelength)
	float designWavelength { 587.6f };
	// The index in the scene's material table of the library material `id`, packing it first if it is not there yet. A
	// mix's pack function calls it for its two children. Empty where nothing packs a scene (a preview), and the mix's
	// children are then index 0
	std::function<uint32_t(uint64_t id)> materialIndex;
};

// a material's spectra as the spectral builds read them, by slot (src/spectral.h SpectralSlot): the albedo, the
// emission, and two more for the new BxDFs' indices
inline constexpr size_t MATERIAL_SPECTRUM_SLOTS = 4;
using MaterialSpectrumSlot = std::optional<std::pair<SpectrumRef, SpectrumRole>>;

// A material as the GPU reads it: the header's type and flags (crt_common.glsl GpuMaterial), its
// parameter record in vec4s for binding 26, and what the light list needs to price it as an emitter
struct PackedMaterial {
	uint32_t type { 0 };
	// CRT_MATERIAL_FLAG_*, src/rt_gpu_types.h
	uint32_t flags { 0 };
	std::vector<glm::vec4> params;
	// the radiance it emits before any texture, black for a material that does not glow
	glm::vec3 emission { 0.f };
	// its emission texture's layer, -1 for none
	int emissiveLayer { -1 };
	// THE SPECTRAL SIDE (docs/plans/completed/pbrt-spectral-rendering.md): the spectra the spectral builds evaluate in
	// place of the record's RGB values, by slot, and the factor the emission is multiplied by (an emitter's strength)
	std::array<MaterialSpectrumSlot, MATERIAL_SPECTRUM_SLOTS> spectra {};
	float emissionScale { 0.f };
	// what the light list prices the material as an emitter by when its RGB emission has no luminance, as a line
	// outside the visible range has none: its radiance integrated over wavelength over CIE_Y_INTEGRAL, so it compares
	// with a luminance. 0 for an emitter with luminance, whose RGB prices it as before
	float emissionPrice { 0.f };

	bool operator==(const PackedMaterial&) const = default;
};

// where a pack function writes: the parameter record and the flags
struct MaterialRecordWriter {
	PackedMaterial& out;
	// appends `bytes` of plain data, padded to whole vec4s
	void write(const void* data, size_t bytes);
};

//---------------------------------------------------------------- the traits table

// MaterialTraits::flags
inline constexpr uint8_t MATERIAL_TYPE_EMISSIVE = 1u << 0;
inline constexpr uint8_t MATERIAL_TYPE_SCATTERS = 1u << 1;
// evaluated by the complex build of kernel 06: a large or branchy evaluation (pbrt's universal
// evaluator). None of the six original types is
inline constexpr uint8_t MATERIAL_TYPE_COMPLEX = 1u << 2;
// always a delta distribution, which no light sample can land in
inline constexpr uint8_t MATERIAL_TYPE_DELTA = 1u << 3;

struct MaterialTraits {
	// the scene file's type name
	const char* name;
	// what the type combo shows
	const char* label;
	std::span<const MaterialParam> params;
	uint8_t flags;
	void (*pack)(const MaterialDef&, const PackContext&, MaterialRecordWriter&);
	// the radiance it emits before any texture: what the light list prices an emitter by
	glm::vec3 (*emission)(const MaterialDef&);
	// the one flat color the raster preview shows it as, which has no path tracer to resolve a real
	// appearance with
	glm::vec3 (*previewColor)(const MaterialDef&);
};

const MaterialTraits& materialTraits(MaterialType type);
inline const char* materialTypeName(MaterialType type) { return materialTraits(type).name; }
std::optional<MaterialType> findMaterialType(std::string_view name);
// the type's parameter of that name, or null
const MaterialParam* findMaterialParam(MaterialType type, std::string_view name);

PackedMaterial packMaterial(const MaterialDef& material, const PackContext& context);
glm::vec3 materialEmission(const MaterialDef& material);
glm::vec3 materialPreviewColor(const MaterialDef& material);
