#include <camera_model.h>

#include <algorithm>
#include <cmath>

#include <light_sampling.h>

namespace {

constexpr float CAMERA_PI = 3.14159265358979323846f;

struct NamedId {
	const char* id;
	const char* name;
};

constexpr NamedId CAMERA_KINDS[] = {
	{ "perspective", "Perspective" },
	{ "orthographic", "Orthographic" },
	{ "spherical", "Spherical" },
};
constexpr NamedId SPHERICAL_MAPPINGS[] = {
	{ "equirect", "Equirectangular" },
	{ "equalarea", "Equal-area" },
};
constexpr NamedId APERTURE_SHAPES[] = {
	{ "circle", "Circle" },
	{ "polygon", "Polygon" },
	{ "image", "Image" },
};
constexpr NamedId LENS_MAPPINGS[] = {
	{ "polar", "Polar" },
	{ "concentric", "Concentric" },
};

template <typename Enum, size_t N>
Enum fromId(const NamedId (&table)[N], std::string_view id)
{
	for (size_t i = 0; i < N; i++) {
		if (id == table[i].id) {
			return (Enum)i;
		}
	}
	return Enum::Count;
}

// the area of the unit aperture of this shape: the density sampleAperture() gives a circle or a polygon is its inverse
float unitApertureArea(ApertureShape shape, uint32_t blades)
{
	switch (shape) {
	case ApertureShape::Polygon:
		return 0.5f * (float)blades * std::sin(2.f * CAMERA_PI / (float)blades);
	case ApertureShape::Image:
		return 4.f;
	default:
		return CAMERA_PI;
	}
}

}

const char* cameraKindId(CameraKind kind) { return kind < CameraKind::Count ? CAMERA_KINDS[(size_t)kind].id : ""; }
const char* cameraKindName(CameraKind kind) { return kind < CameraKind::Count ? CAMERA_KINDS[(size_t)kind].name : ""; }
CameraKind cameraKindFromId(std::string_view id) { return fromId<CameraKind>(CAMERA_KINDS, id); }
const char* sphericalMappingId(SphericalMapping mapping) { return mapping < SphericalMapping::Count ? SPHERICAL_MAPPINGS[(size_t)mapping].id : ""; }
const char* sphericalMappingName(SphericalMapping mapping) { return mapping < SphericalMapping::Count ? SPHERICAL_MAPPINGS[(size_t)mapping].name : ""; }
SphericalMapping sphericalMappingFromId(std::string_view id) { return fromId<SphericalMapping>(SPHERICAL_MAPPINGS, id); }
const char* apertureShapeId(ApertureShape shape) { return shape < ApertureShape::Count ? APERTURE_SHAPES[(size_t)shape].id : ""; }
const char* apertureShapeName(ApertureShape shape) { return shape < ApertureShape::Count ? APERTURE_SHAPES[(size_t)shape].name : ""; }
ApertureShape apertureShapeFromId(std::string_view id) { return fromId<ApertureShape>(APERTURE_SHAPES, id); }
const char* lensMappingId(LensMapping mapping) { return mapping < LensMapping::Count ? LENS_MAPPINGS[(size_t)mapping].id : ""; }
LensMapping lensMappingFromId(std::string_view id) { return fromId<LensMapping>(LENS_MAPPINGS, id); }

CameraFrame buildCameraFrame(const RTCameraSnapshot& camera, uint32_t width, uint32_t height, uint32_t apertureTable)
{
	CameraFrame frame;
	frame.width = std::max(width, 1u);
	frame.height = std::max(height, 1u);
	frame.kind = camera.kind < CameraKind::Count ? camera.kind : CameraKind::Perspective;
	frame.focusDistance = camera.focusDistance;

	//the RTIOW camera: origin, lower-left corner of the image plane, its two edges, and the lens basis. Written as it
	//always was, so the perspective camera's floats are the ones every earlier render was made from
	const float aspect = (float)frame.width / (float)frame.height;
	const float theta = glm::radians(camera.vfovDegrees);
	const float h = glm::tan(theta / 2.f);
	const float viewportHeight = 2.f * h;
	const float viewportWidth = aspect * viewportHeight;
	const glm::vec3 t = glm::normalize(camera.lookFrom - camera.lookAt);
	const glm::vec3 n = glm::normalize(glm::cross(camera.vUp, t));
	const glm::vec3 b = glm::cross(t, n);
	glm::vec3 horizontal = camera.focusDistance * viewportWidth * n;
	glm::vec3 vertical = camera.focusDistance * viewportHeight * b;
	if (frame.kind == CameraKind::Orthographic) {
		//the view volume's cross-section, the same size at every depth
		horizontal = camera.orthoHeight * aspect * n;
		vertical = camera.orthoHeight * b;
	}
	glm::vec3 lowerLeft = camera.lookFrom - horizontal / 2.f - vertical / 2.f - camera.focusDistance * t;
	//the shift slides the film across its plane, in units of its height on both axes
	if (camera.shift != glm::vec2(0.f)) {
		lowerLeft += camera.shift.x * glm::length(vertical) * n + camera.shift.y * vertical;
	}

	frame.origin = camera.lookFrom;
	frame.lowerLeft = lowerLeft;
	frame.horizontal = horizontal;
	frame.vertical = vertical;
	frame.lensU = n;
	frame.lensV = b;
	frame.backward = t;
	frame.filmArea = viewportWidth * viewportHeight;
	switch (frame.kind) {
	case CameraKind::Perspective:
		frame.pixelSpread = std::atan(2.f * h / (float)frame.height);
		break;
	case CameraKind::Orthographic:
		frame.pixelWidth = camera.orthoHeight / (float)frame.height;
		frame.filmArea = glm::length(horizontal) * glm::length(vertical);
		break;
	case CameraKind::Spherical:
		//a row of an equirectangular image spans pi of latitude; an equal-area pixel covers 4 pi / pixels
		frame.pixelSpread = camera.mapping == SphericalMapping::EqualArea ? std::sqrt(4.f * CAMERA_PI / ((float)frame.width * (float)frame.height))
		                                                                   : CAMERA_PI / (float)frame.height;
		frame.filmArea = 4.f * CAMERA_PI;
		break;
	default:
		break;
	}

	//the lens. A spherical camera has none
	const ApertureShape shape = camera.apertureShape < ApertureShape::Count ? camera.apertureShape : ApertureShape::Circle;
	frame.lensRadius = frame.kind == CameraKind::Spherical ? 0.f : camera.aperture / 2.f;
	frame.blades = std::clamp(camera.apertureBlades, MIN_APERTURE_BLADES, MAX_APERTURE_BLADES);
	frame.apertureTable = apertureTable;
	const float rotation = glm::radians(camera.apertureRotationDegrees);
	frame.apertureRotation = glm::vec2(std::cos(rotation), std::sin(rotation));
	frame.lensArea = frame.lensRadius > 0.f ? unitApertureArea(shape, frame.blades) * frame.lensRadius * frame.lensRadius : 1.f;
	frame.flags = ((uint32_t)shape << CAMERA_APERTURE_SHIFT) | (camera.lensMapping == LensMapping::Concentric ? CAMERA_FLAG_CONCENTRIC : 0u)
		| (camera.mapping == SphericalMapping::EqualArea ? CAMERA_FLAG_EQUAL_AREA : 0u);

	//the tilted plane of focus, through the point on the axis at the focus distance: its normal is the axis turned by
	//the tilt about camera x, then about camera y
	if (frame.kind != CameraKind::Spherical && (camera.tiltDegrees.x != 0.f || camera.tiltDegrees.y != 0.f)) {
		const float a = glm::radians(std::clamp(camera.tiltDegrees.x, -MAX_CAMERA_TILT, MAX_CAMERA_TILT));
		const float c = glm::radians(std::clamp(camera.tiltDegrees.y, -MAX_CAMERA_TILT, MAX_CAMERA_TILT));
		const glm::vec3 local(std::sin(c) * std::cos(a), std::sin(a), std::cos(a) * std::cos(c));
		const glm::vec3 normal = glm::normalize(local.x * n + local.y * b + local.z * t);
		const glm::vec3 pivot = camera.lookFrom - camera.focusDistance * t;
		frame.focusPlane = glm::vec4(normal, glm::dot(normal, pivot));
		frame.flags |= CAMERA_FLAG_TILTED;
	}
	return frame;
}

glm::vec2 polarDiskFromSample(const glm::vec2& u)
{
	const float r = std::sqrt(u.x);
	const float phi = 2.f * CAMERA_PI * u.y;
	return r * glm::vec2(std::cos(phi), std::sin(phi));
}

glm::vec2 sampleAperture(const CameraFrame& frame, const glm::vec2& u, const PiecewiseConstant2D* aperture)
{
	switch (frame.apertureShape()) {
	case ApertureShape::Polygon: {
		//one triangle of the fan from the center, all of equal area, then a point uniform in it
		const float scaled = u.x * (float)frame.blades;
		const uint32_t k = std::min((uint32_t)scaled, frame.blades - 1u);
		const glm::vec3 barycentrics = sampleUniformTriangle(glm::vec2(scaled - (float)k, u.y));
		const float step = 2.f * CAMERA_PI / (float)frame.blades;
		const glm::vec2 c0(std::cos((float)k * step), std::sin((float)k * step));
		const glm::vec2 c1(std::cos((float)(k + 1u) * step), std::sin((float)(k + 1u) * step));
		const glm::vec2 p = barycentrics.y * c0 + barycentrics.z * c1;
		const glm::vec2 r = frame.apertureRotation;
		return glm::vec2(r.x * p.x - r.y * p.y, r.y * p.x + r.x * p.y);
	}
	case ApertureShape::Image: {
		//row 0 of the image is the aperture's top
		glm::vec2 uv = u;
		if (aperture != nullptr && aperture->integral > 0.f) {
			float pdf;
			uv = samplePiecewiseConstant2D(*aperture, u, pdf);
		}
		return glm::vec2(2.f * uv.x - 1.f, 1.f - 2.f * uv.y);
	}
	default:
		return (frame.flags & CAMERA_FLAG_CONCENTRIC) != 0u ? sampleUniformDiskConcentric(u) : polarDiskFromSample(u);
	}
}

float apertureDensity(const CameraFrame& frame, const glm::vec2& p, const PiecewiseConstant2D* aperture)
{
	switch (frame.apertureShape()) {
	case ApertureShape::Polygon: {
		//inside every edge of the polygon, turned back to its unrotated frame
		const glm::vec2 r = frame.apertureRotation;
		const glm::vec2 q(r.x * p.x + r.y * p.y, -r.y * p.x + r.x * p.y);
		const float step = 2.f * CAMERA_PI / (float)frame.blades;
		const float apothem = std::cos(step / 2.f);
		for (uint32_t k = 0; k < frame.blades; k++) {
			const float middle = ((float)k + 0.5f) * step;
			if (q.x * std::cos(middle) + q.y * std::sin(middle) > apothem) {
				return 0.f;
			}
		}
		return 1.f / unitApertureArea(ApertureShape::Polygon, frame.blades);
	}
	case ApertureShape::Image: {
		if (std::abs(p.x) > 1.f || std::abs(p.y) > 1.f) {
			return 0.f;
		}
		const glm::vec2 uv(0.5f * (p.x + 1.f), 0.5f * (1.f - p.y));
		const float pdf = (aperture != nullptr && aperture->integral > 0.f) ? piecewiseConstant2DPdf(*aperture, uv) : 1.f;
		return pdf / 4.f;
	}
	default:
		return glm::dot(p, p) <= 1.f ? 1.f / CAMERA_PI : 0.f;
	}
}

CameraRay generateCameraRay(const CameraFrame& frame, const glm::vec2& film, const glm::vec2& lens, const PiecewiseConstant2D* aperture)
{
	CameraRay ray;
	if (frame.kind == CameraKind::Spherical) {
		//the image's v runs up, the mappings' rows down. pbrt's equal-area camera has its pole on camera y and looks
		//down its +z, which is this camera's -z
		const glm::vec2 uv(film.x, 1.f - film.y);
		glm::vec3 d;
		if ((frame.flags & CAMERA_FLAG_EQUAL_AREA) != 0u) {
			const glm::vec3 s = equalAreaSquareToSphere(uv);
			d = glm::vec3(s.x, s.z, -s.y);
		} else {
			d = equirectDirectionOf(uv);
		}
		ray.origin = frame.origin;
		ray.direction = d.x * frame.lensU + d.y * frame.lensV + d.z * frame.backward;
		return ray;
	}

	//the point of the film, on the untilted plane of focus; the chief ray reaches it from the lens's center
	glm::vec3 origin = frame.origin;
	const glm::vec3 target = frame.lowerLeft + film.x * frame.horizontal + film.y * frame.vertical;
	if (frame.kind == CameraKind::Orthographic) {
		origin = target + frame.focusDistance * frame.backward;
	}
	const glm::vec3 chief = origin;
	if (frame.lensRadius > 0.f) {
		const glm::vec2 rd = frame.lensRadius * sampleAperture(frame, lens, aperture);
		origin += rd.x * frame.lensU + rd.y * frame.lensV;
	}
	glm::vec3 focus = target;
	if ((frame.flags & CAMERA_FLAG_TILTED) != 0u) {
		//where the chief ray meets the tilted plane; one that never does is focused at infinity, so every ray from the
		//lens runs parallel to it
		const glm::vec3 normal(frame.focusPlane);
		const glm::vec3 d = target - chief;
		const float along = (frame.focusPlane.w - glm::dot(normal, chief)) / glm::dot(normal, d);
		if (!(along > 0.f) || !std::isfinite(along)) {
			ray.origin = origin;
			ray.direction = d;
			return ray;
		}
		focus = chief + along * d;
	}
	ray.origin = origin;
	ray.direction = focus - origin;
	return ray;
}

//---------------------------------------------------------------- camera importance

bool cameraConnects(const CameraFrame& frame)
{
	return cameraTraits(frame.kind).importance && (frame.flags & CAMERA_FLAG_TILTED) == 0u;
}

float lensDensity(const CameraFrame& frame, const glm::vec3& lensPoint, const PiecewiseConstant2D* aperture)
{
	if (!(frame.lensRadius > 0.f)) {
		return 1.f;
	}
	const glm::vec3 offset = lensPoint - frame.origin;
	const glm::vec2 unit(glm::dot(offset, frame.lensU) / frame.lensRadius, glm::dot(offset, frame.lensV) / frame.lensRadius);
	return apertureDensity(frame, unit, aperture) / (frame.lensRadius * frame.lensRadius);
}

float cameraWe(const CameraFrame& frame, const glm::vec3& lensPoint, const glm::vec3& direction, glm::vec2& film, const PiecewiseConstant2D* aperture)
{
	film = glm::vec2(-1.f);
	const float cosTheta = -glm::dot(direction, frame.backward);
	if (!(cosTheta > 0.f)) {
		return 0.f;
	}
	//where the ray meets the plane of focus, which the film's rectangle lies on
	const glm::vec3 focus = lensPoint + (frame.focusDistance / cosTheta) * direction;
	const glm::vec3 local = focus - frame.lowerLeft;
	film = glm::vec2(glm::dot(local, frame.horizontal) / glm::dot(frame.horizontal, frame.horizontal), glm::dot(local, frame.vertical) / glm::dot(frame.vertical, frame.vertical));
	if (film.x < 0.f || film.x >= 1.f || film.y < 0.f || film.y >= 1.f) {
		return 0.f;
	}
	const float cos2Theta = cosTheta * cosTheta;
	return lensDensity(frame, lensPoint, aperture) / (frame.filmArea * cos2Theta * cos2Theta);
}

void cameraPdfWe(const CameraFrame& frame, const glm::vec3& lensPoint, const glm::vec3& direction, float& pdfPos, float& pdfDir, const PiecewiseConstant2D* aperture)
{
	pdfPos = 0.f;
	pdfDir = 0.f;
	glm::vec2 film;
	if (cameraWe(frame, lensPoint, direction, film, aperture) == 0.f) {
		return;
	}
	const float cosTheta = -glm::dot(direction, frame.backward);
	pdfPos = lensDensity(frame, lensPoint, aperture);
	pdfDir = 1.f / (frame.filmArea * cosTheta * cosTheta * cosTheta);
}

bool cameraSampleWi(const CameraFrame& frame, const glm::vec3& ref, const glm::vec2& u, const PiecewiseConstant2D* aperture, CameraWiSample& out)
{
	out = CameraWiSample {};
	glm::vec3 lensPoint = frame.origin;
	if (frame.lensRadius > 0.f) {
		const glm::vec2 rd = frame.lensRadius * sampleAperture(frame, u, aperture);
		lensPoint += rd.x * frame.lensU + rd.y * frame.lensV;
	}
	const glm::vec3 toLens = lensPoint - ref;
	const float distanceSquared = glm::dot(toLens, toLens);
	if (!(distanceSquared > 0.f)) {
		return false;
	}
	out.distance = std::sqrt(distanceSquared);
	out.wi = toLens / out.distance;
	out.lensPoint = lensPoint;
	out.we = cameraWe(frame, lensPoint, -out.wi, out.film, aperture);
	const float cosLens = std::abs(glm::dot(frame.backward, out.wi));
	if (!(out.we > 0.f) || !(cosLens > 0.f)) {
		return false;
	}
	out.pdf = lensDensity(frame, lensPoint, aperture) * distanceSquared / cosLens;
	return out.pdf > 0.f;
}

