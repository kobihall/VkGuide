#pragma once

// THE CAMERA MODELS (docs/plans/completed/pbrt-cameras.md): how a camera turns a point of its film and a point of its
// lens into a ray, as plain C++ with no Vulkan. The line-for-line mirror of shaders/rt/include/crt_camera.glsl, checked
// on its own by tests/camera_test.cpp and against the GPU by the camera self-test (Windows -> GPU self-tests). Change
// both sides together.
//
// Every kind is a thin lens: perspective (PBR 4ed 5.2.2 and 5.2.3, Ray Tracing in One Weekend's camera), orthographic
// (5.2.1) and spherical (5.3, without a lens). The aperture is a circle, a regular polygon or an image, and the film of
// a perspective or orthographic camera can be shifted, and its plane of focus tilted (Kensler 2021). A ray's weight is
// always 1: the thin lens is normalized for exposure, so the aperture's size and shape change the depth of field and the
// bokeh and never the brightness, as pbrt's thin lens does.
//
// The film rectangle of a perspective or orthographic camera lies on the plane of focus before any tilt: a camera ray
// leaves the lens and passes through the point the ray through the lens's center (the chief ray) meets the plane of
// focus. Without a tilt that point is the film's own, which is Ray Tracing in One Weekend's construction and the reason
// its arithmetic is kept: a camera with the polar lens mapping, a circular aperture and no tilt renders bit-identical
// to every image made before the camera kinds existed.

#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

struct PiecewiseConstant2D;

// The ids are saved in the scene file; never rename
enum class CameraKind : uint32_t {
	Perspective,
	Orthographic,
	Spherical,
	Count
};

// How a spherical camera lays the sphere of directions out on its image
enum class SphericalMapping : uint32_t {
	// the environment map's own layout (shaders/equirect.glsl), so a render can be loaded back as the map
	Equirectangular,
	// pbrt-v4's equal-area square (Clarberg 2008), the layout its image infinite lights read
	EqualArea,
	Count
};

enum class ApertureShape : uint32_t {
	Circle,
	// a regular polygon of `apertureBlades` sides, its corners on the aperture's circle
	Polygon,
	// an image over the square the aperture's circle fits in, sampled by its luminance: a star, a heart, a ring
	Image,
	Count
};

// How a lens sample becomes a point of a circular aperture: Ray Tracing in One Weekend's polar map, which every scene
// file before version 26 rendered with and still does, or pbrt's concentric map (PBR 4ed A.5.1), which keeps a
// stratified sampler's strata stratified and is what a new camera uses
enum class LensMapping : uint32_t {
	Polar,
	Concentric,
	Count
};

const char* cameraKindId(CameraKind kind);
const char* cameraKindName(CameraKind kind);
// Count for an id this build does not have
CameraKind cameraKindFromId(std::string_view id);
const char* sphericalMappingId(SphericalMapping mapping);
const char* sphericalMappingName(SphericalMapping mapping);
SphericalMapping sphericalMappingFromId(std::string_view id);
const char* apertureShapeId(ApertureShape shape);
const char* apertureShapeName(ApertureShape shape);
ApertureShape apertureShapeFromId(std::string_view id);
const char* lensMappingId(LensMapping mapping);
LensMapping lensMappingFromId(std::string_view id);

// Everything that differs between the camera kinds outside the ray generation itself, as data: which of a camera's
// fields a kind uses, for the editor, the scene file and the viewport's outline. A new kind is a row here and its branch
// in generateCameraRay() and crt_camera.glsl
struct CameraTraits {
	CameraKind kind;
	// the vertical field of view (perspective); the view volume's height (orthographic); the spherical mapping
	bool fieldOfView;
	bool viewHeight;
	bool sphericalMapping;
	// a lens, with its aperture, focus distance and aperture shape, and a film that shifts and a plane of focus that tilts
	bool lens;
	// what light tracing and the bidirectional integrators can connect to (pbrt's PerspectiveCamera alone)
	bool importance;
};

inline constexpr CameraTraits CAMERA_TRAITS[] = {
	{ CameraKind::Perspective, true, false, false, true, true },
	{ CameraKind::Orthographic, false, true, false, true, false },
	{ CameraKind::Spherical, false, false, true, false, false },
};
static_assert(std::size(CAMERA_TRAITS) == (size_t)CameraKind::Count);

inline const CameraTraits& cameraTraits(CameraKind kind)
{
	return CAMERA_TRAITS[kind < CameraKind::Count ? (size_t)kind : 0];
}

inline constexpr uint32_t MIN_APERTURE_BLADES = 3;
inline constexpr uint32_t MAX_APERTURE_BLADES = 16;
// the largest tilt of the plane of focus, in degrees, either way about either axis
inline constexpr float MAX_CAMERA_TILT = 80.f;

// What a render takes of a SceneCamera: its pose resolved to a look-from, a look-at and an up vector, and everything
// about how it projects. Compared by value: a change restarts the render
struct RTCameraSnapshot {
	glm::vec3 lookFrom { 0.f, 0.f, 0.f };
	glm::vec3 lookAt { 0.f, 0.f, -1.f };
	glm::vec3 vUp { 0.f, 1.f, 0.f };
	CameraKind kind { CameraKind::Perspective };
	float vfovDegrees { 70.f };
	// orthographic: the height of the view volume, in scene units
	float orthoHeight { 2.f };
	SphericalMapping mapping { SphericalMapping::Equirectangular };
	// the aperture's diameter (its circle's, for a polygon or an image); 0 is a pinhole
	float aperture { 0.f };
	float focusDistance { 1.f };
	ApertureShape apertureShape { ApertureShape::Circle };
	uint32_t apertureBlades { 6 };
	float apertureRotationDegrees { 0.f };
	// ApertureShape::Image: the image file, absolute
	std::string apertureImage;
	LensMapping lensMapping { LensMapping::Concentric };
	// the plane of focus turned about the camera's x axis (x, the top receding for a positive angle) and its y axis
	// (y, the right side receding), in degrees, about the point on the axis at the focus distance
	glm::vec2 tiltDegrees { 0.f };
	// the film moved across the image plane, in units of the image's height, x right and y up
	glm::vec2 shift { 0.f };

	bool operator==(const RTCameraSnapshot&) const = default;
};

// THE GPU'S CAMERA: what kernel 00, and every kernel that connects a path to the camera, read from the push constants
// (src/rt_gpu_types.h CrtParams), computed once per render
inline constexpr uint32_t CAMERA_FLAG_CONCENTRIC = 1u << 0;
// the plane of focus is not the film's: a chief ray is carried to focusPlane
inline constexpr uint32_t CAMERA_FLAG_TILTED = 1u << 1;
inline constexpr uint32_t CAMERA_FLAG_EQUAL_AREA = 1u << 2;
// the ApertureShape, in bits 8 and 9
inline constexpr uint32_t CAMERA_APERTURE_SHIFT = 8u;

struct CameraFrame {
	glm::vec3 origin { 0.f };
	// the film rectangle on the untilted plane of focus: its lower-left corner and its two edges
	glm::vec3 lowerLeft { 0.f };
	glm::vec3 horizontal { 0.f };
	glm::vec3 vertical { 0.f };
	// the lens's axes, camera x and y, and camera z, which points back from the view
	glm::vec3 lensU { 1.f, 0.f, 0.f };
	glm::vec3 lensV { 0.f, 1.f, 0.f };
	glm::vec3 backward { 0.f, 0.f, 1.f };
	float lensRadius { 0.f };
	float focusDistance { 1.f };
	// orthographic: a pixel's height in scene units, which a ray cone starts as wide as
	float pixelWidth { 0.f };
	// a perspective or spherical camera's: the angle one pixel subtends, a ray cone's spread at the eye
	float pixelSpread { 0.f };
	// the tilted plane of focus: its unit normal and the dot of the normal with any point on it
	glm::vec4 focusPlane { 0.f, 0.f, 1.f, 0.f };
	CameraKind kind { CameraKind::Perspective };
	uint32_t flags { 0 };
	uint32_t blades { 6 };
	// ApertureShape::Image: where the aperture's table starts in binding 65 (ImageDistributions), in floats
	uint32_t apertureTable { 0 };
	// the aperture's rotation, its cosine and sine
	glm::vec2 apertureRotation { 1.f, 0.f };
	// the area the aperture's density is measured over: a circle's or a polygon's area, an image's square, 1 for a
	// pinhole (pbrt's convention); and the film's area on the plane at unit distance from the lens
	float lensArea { 1.f };
	float filmArea { 1.f };
	uint32_t width { 1 };
	uint32_t height { 1 };

	ApertureShape apertureShape() const { return (ApertureShape)((flags >> CAMERA_APERTURE_SHIFT) & 3u); }
	// more than Ray Tracing in One Weekend's camera: what the shaders' CRT_CAMERA_MODEL specialization is set from. A
	// pinhole's mapping and aperture shape never matter
	bool needsModel() const
	{
		const bool lens = lensRadius > 0.f;
		return kind != CameraKind::Perspective || (flags & CAMERA_FLAG_TILTED) != 0u
			|| (lens && ((flags & CAMERA_FLAG_CONCENTRIC) != 0u || apertureShape() != ApertureShape::Circle));
	}
};

// `apertureTable` is where the aperture image's table will be in binding 65, when the shape is an image
CameraFrame buildCameraFrame(const RTCameraSnapshot& camera, uint32_t width, uint32_t height, uint32_t apertureTable = 0);

// a point of the unit aperture (radius 1) for the lens sample `u`: the disk by the frame's lens mapping, a polygon by a
// triangle of its fan, an image by its table (null: the whole square)
glm::vec2 sampleAperture(const CameraFrame& frame, const glm::vec2& u, const PiecewiseConstant2D* aperture);
// the density sampleAperture() gives the point `p` of the unit aperture, per unit of its area; 0 outside it
float apertureDensity(const CameraFrame& frame, const glm::vec2& p, const PiecewiseConstant2D* aperture);
// Ray Tracing in One Weekend's polar map of the unit square onto the unit disk
glm::vec2 polarDiskFromSample(const glm::vec2& u);

struct CameraRay {
	glm::vec3 origin { 0.f };
	// not normalized: a perspective ray reaches its point of focus at t = 1, as it always did
	glm::vec3 direction { 0.f, 0.f, -1.f };
};

// The ray through the point `film` of the image, u right and v up over [0, 1]^2 (pixel x + offset over the width, and
// the row from the bottom likewise), and the lens sample `lens`
CameraRay generateCameraRay(const CameraFrame& frame, const glm::vec2& film, const glm::vec2& lens, const PiecewiseConstant2D* aperture);

//---------------------------------------------------------------- camera importance

// What light tracing and the bidirectional integrators need of the camera (PBR 3ed 16.1.1; pbrt-v4's
// PerspectiveCamera::We(), PDF_We() and SampleWi()): the importance a ray leaving the lens carries, its densities, and a
// point on the lens seen from a point of the scene. Only a perspective camera without a tilt has them, as pbrt's
// perspective camera alone does (CameraTraits::importance). A pinhole's lens density is 1, pbrt's convention
bool cameraConnects(const CameraFrame& frame);
// the density of the lens point `lensPoint` (in the lens's plane) per unit of area: 1 / lensArea for a circle or a
// polygon, the image's density for an image, 1 for a pinhole
float lensDensity(const CameraFrame& frame, const glm::vec3& lensPoint, const PiecewiseConstant2D* aperture);
// the importance of a ray from `lensPoint` along the unit `direction`, and in `film` the point of the image it reaches
// (u right, v up, as generateCameraRay() takes it); 0 for a ray that misses the film
float cameraWe(const CameraFrame& frame, const glm::vec3& lensPoint, const glm::vec3& direction, glm::vec2& film, const PiecewiseConstant2D* aperture);
// the densities of that ray: its lens point's per unit of area, and its direction's per unit of solid angle
void cameraPdfWe(const CameraFrame& frame, const glm::vec3& lensPoint, const glm::vec3& direction, float& pdfPos, float& pdfDir, const PiecewiseConstant2D* aperture);

struct CameraWiSample {
	glm::vec3 lensPoint { 0.f };
	// unit, from the reference point towards the lens point
	glm::vec3 wi { 0.f, 0.f, 1.f };
	float distance { 0.f };
	// per unit of solid angle at the reference point
	float pdf { 0.f };
	float we { 0.f };
	glm::vec2 film { 0.f };
};

// a point on the lens for the lens sample `u`, seen from `ref`: false when the ray from it to `ref` misses the film
bool cameraSampleWi(const CameraFrame& frame, const glm::vec3& ref, const glm::vec2& u, const PiecewiseConstant2D* aperture, CameraWiSample& out);

