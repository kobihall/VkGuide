#pragma once

// OPTICAL ELEMENTS (docs/plans/completed/physoptics-optical-elements.md), part of rt_optics: each kind of optic Thorlabs sells,
// as a scene object (SceneOptic) and one row of a traits table, the way SHAPE_TRAITS describes the shapes. A row
// holds the kind's sizes, its parameters, its material slots, its output ports, and a builder that turns the
// parameters into the surface prescription of src/optic_surface.h, with a readout of what follows from them.
// OPTIC_TRAITS (opticTraits()) is the only place outside the builders that knows anything about a kind; later plans
// append rows (lasers, detectors, wave plates, gratings).
//
// Every value is stored by name in SI units (meters, radians), every value ever set, so switching a lens from
// bi-convex to plano-convex and back keeps both radii. The panels show lengths in the scene's length unit and angles
// in degrees. Positions are double: the beam solver places beams to a fraction of a micron over a meter of table, and
// the GPU instance rounds them to float when the scene is built.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optic_mount.h>
#include <optic_surface.h>
#include <rt_material.h>

class OpticalLibrary;

// the values are saved by name (OpticTraits::name): append only
enum class OpticKind : uint8_t {
	PlanarMirror,
	ConcaveMirror,
	OffAxisParabola,
	CircularFilter,
	SquareFilter,
	Iris,
	PlateBeamsplitter,
	CubeBeamsplitter,
	SphericalLens,
	RightAnglePrism,
	EquilateralPrism,
	PlateWedge,
	RoundWedge
};
inline constexpr size_t OPTIC_KIND_COUNT = 13;

// the most material slots a kind has
inline constexpr size_t MAX_OPTIC_MATERIAL_SLOTS = 4;

using OpticValue = std::variant<double, int, bool, std::string>;
using OpticParams = std::map<std::string, OpticValue, std::less<>>;

enum class OpticParamKind : uint8_t {
	// meters, shown in the scene's length unit
	Length,
	// radians, shown in degrees
	Angle,
	// a plain number
	Number,
	// one of `labels`, stored as its index
	Enum,
	Bool,
	// a glass of the optical library, by name
	Glass
};

struct OpticParam {
	// saved in the scene file; never rename
	const char* name;
	const char* label;
	OpticParamKind kind;
	OpticValue defaultValue;
	// the widget's range, SI
	double min { 0.0 };
	double max { 0.0 };
	// Enum: the labels, and the names the scene file saves them by
	std::span<const char* const> labels {};
	std::span<const char* const> keys {};
	const char* tooltip { nullptr };
	// shown only while this returns true (a lens's second radius is hidden on a plano form); null for always
	bool (*visible)(const OpticParams&) { nullptr };
};

// a size preset: its saved name, its label, and the parameters it sets (the main dimension and what Thorlabs makes at
// that size: a blank's thickness, a lens's center thickness)
struct OpticSizeValue {
	const char* param;
	double value;
};
struct OpticSize {
	const char* key;
	const char* label;
	std::span<const OpticSizeValue> values;
};

// what a material slot is made of when the element is created
enum class SlotDefault : uint8_t {
	// a polished optical interface with the slot's coating
	Polished,
	// a ground interface (rough glass between the same media)
	Ground,
	// black anodized metal: diffuse, reflectance 0.04 (an iris's blades)
	Black
};
struct MaterialSlot {
	const char* name;
	SlotDefault kind;
	// the coating key a polished slot starts with ("uncoated" for a bare face)
	const char* coating;
};

// One step of the reflections and refractions that make a port inside the body
struct PortEvent {
	uint32_t surface;
	bool reflect;
};
// a named output beam (physoptics-beam-axis.md), defined by the sequence that produces it for a ray entering along
// the kind's design axis
struct OpticPort {
	const char* name;
	std::span<const PortEvent> events;
};

struct SceneOptic {
	// shares the editor's id space with shapes, cameras, lights, mesh objects and materials; never saved
	uint64_t id { 0 };
	std::string name;
	OpticKind kind { OpticKind::PlanarMirror };
	// into the kind's sizes, or -1 for a custom size
	int sizePreset { 0 };
	OpticParams params;
	// what it stands in and where (src/optic_mount.h): the saved placement
	MountPose mount;
	// the body frame -> world, derived from the mount by opticWorldTransform() (src/optic_bench.h) each time the editor
	// resolves the bench; never saved. Rounded to float for the GPU
	glm::dmat4 world { 1.0 };
	// the library entries the element owns, by slot; 0 for a slot the kind does not have
	std::array<uint64_t, MAX_OPTIC_MATERIAL_SLOTS> materials {};

	glm::dmat4 bodyToWorld() const { return world; }
	bool operator==(const SceneOptic&) const = default;
};

// what a builder needs from outside the element
struct OpticBuildContext {
	const OpticalLibrary* library { nullptr };
	// nm: where exact RGB and the readouts read a glass's index, and where a wedge's angle is solved
	double designWavelength { 587.6 };
	// the surrounding medium's glass (AIR or VACUUM)
	std::string surrounding { "AIR" };
	// n of a glass relative to the surrounding medium at the design wavelength; 1.5 for an unknown name
	double relativeIndex(const std::string& glass) const;
};

// the derived values the panel shows read-only, and why a parameter was refused
struct OpticReadout {
	std::vector<std::pair<std::string, std::string>> lines;
	// non-empty when the parameters do not make a body (an edge thickness below zero): the builder leaves the
	// prescription empty and the panel shows this beside the parameters
	std::string error;
	// lengths the panel formats in the scene's unit: (label, meters)
	std::vector<std::pair<std::string, double>> lengths;
	std::vector<std::pair<std::string, double>> angles;
};

struct OpticTraits {
	// the scene file's type name, and the prefix of a new object's name; never rename
	const char* name;
	const char* label;
	// the Optics folder's sub-folder
	const char* plural;
	std::span<const OpticSize> sizes;
	std::span<const OpticParam> params;
	std::span<const MaterialSlot> slots;
	// the mounts the kind may sit in, the first its default (MountKind::None is always allowed besides)
	std::span<const MountKind> mounts;
	std::span<const OpticPort> ports;
	// true for the kinds whose preview is a mount library model (planar mirrors: the PF blanks)
	bool previewFromModel;
	void (*build)(const SceneOptic&, const OpticBuildContext&, OpticPrescription&, OpticReadout&);
	// whether the body closes on itself (an iris is one sheet, which does not)
	bool closed;
};

std::span<const OpticTraits> opticTraitsTable();
const OpticTraits& opticTraits(OpticKind kind);
std::optional<OpticKind> findOpticKind(std::string_view name);
const OpticParam* findOpticParam(OpticKind kind, std::string_view name);

// a parameter's value: the element's own, else the size's, else the kind's default
double opticNumber(const SceneOptic& optic, std::string_view name);
int opticEnum(const SceneOptic& optic, std::string_view name);
bool opticBool(const SceneOptic& optic, std::string_view name);
std::string opticString(const SceneOptic& optic, std::string_view name);
OpticValue opticValue(const SceneOptic& optic, std::string_view name);
// sets a value, clamped to the parameter's range; returns whether it changed
bool setOpticValue(SceneOptic& optic, std::string_view name, OpticValue value);
// switches to a size preset (and its values) or to custom (-1, keeping the values)
void setOpticSize(SceneOptic& optic, int preset);

// a new element of `kind` at its first size, with no materials yet (the editor makes them)
SceneOptic makeOptic(OpticKind kind);

// what decides an element's body: its kind, size and parameters, as a string two elements of one body share (the build
// context aside). The scene's acceleration structure packs each body once by it, and the preview meshes each once
std::string opticBodyKey(const SceneOptic& optic);

// the prescription and readout of an element
OpticPrescription buildOptic(const SceneOptic& optic, const OpticBuildContext& context, OpticReadout& readout);

// the material a slot starts with: an optical interface with the slot's coating, a ground interface, or black
MaterialDef defaultSlotMaterial(const SceneOptic& optic, size_t slot);
// a polarizing splitter's diagonal and a non-polarizing one's start with different coatings
std::string defaultSlotCoating(const SceneOptic& optic, size_t slot);

//---------------------------------------------------------------- tracing rays through a body, in double

// the indices of a body's media at one wavelength, relative to nothing (absolute), [0] the surrounding medium
std::vector<double> mediumIndices(const OpticPrescription& body, const OpticBuildContext& context, double lambda);

struct OpticRay {
	glm::dvec3 origin { 0.0 };
	glm::dvec3 direction { 0.0, 0.0, 1.0 };
};

// Follows `events` through the body, in its frame: at each, the nearest hit must be the event's surface, where the ray
// reflects or refracts (by the media's indices and the side it arrived from). False when a hit is missing, is another
// surface, or a refraction is past the critical angle. `ray` ends as the ray leaving the last event
bool traceEvents(const OpticPrescription& body, const std::vector<double>& indices, std::span<const PortEvent> events, OpticRay& ray);
