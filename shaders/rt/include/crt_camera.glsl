// THE CAMERA MODELS (docs/plans/completed/pbrt-cameras.md): how the render's camera turns a point of its film and a point
// of its lens into a ray. Mirrored line for line by src/camera_model.cpp, which bin/camera_test checks and the camera
// self-test compares with this file. Change both together.
//
// Every kind is a thin lens: perspective (PBR 4ed 5.2.2, 5.2.3), orthographic (5.2.1) and spherical (5.3, no lens). The
// aperture is a circle, a regular polygon or an image, a perspective or orthographic film can be shifted, and its plane
// of focus tilted (Kensler 2021). Every ray's weight is 1. The film rectangle (pc.lowerLeft, horizontal, vertical) lies
// on the plane of focus before the tilt; a camera ray leaves its point of the lens towards where the chief ray, from the
// lens's center through the film's point, meets the plane of focus. With the polar lens mapping, a circular aperture and
// no tilt that is Ray Tracing in One Weekend's camera, in its own arithmetic.
//
// Requires crt_common.glsl, crt_random.glsl and crt_samples.glsl (diskFromSample()); the including kernel declares
// binding 65 (ImageDistributions).

#ifndef CRT_CAMERA_GLSL
#define CRT_CAMERA_GLSL

#include "equirect.glsl"
#include "crt_warp.glsl"
#include "crt_image_table.glsl"

// src/camera_model.h CameraKind, CAMERA_FLAG_*, ApertureShape
#define CRT_CAMERA_PERSPECTIVE 0u
#define CRT_CAMERA_ORTHOGRAPHIC 1u
#define CRT_CAMERA_SPHERICAL 2u
#define CRT_CAMERA_FLAG_CONCENTRIC 1u
#define CRT_CAMERA_FLAG_TILTED 2u
#define CRT_CAMERA_FLAG_EQUAL_AREA 4u
#define CRT_CAMERA_APERTURE_SHIFT 8u
#define CRT_APERTURE_CIRCLE 0u
#define CRT_APERTURE_POLYGON 1u
#define CRT_APERTURE_IMAGE 2u

uint cameraApertureShape()
{
	return (pc.cameraFlags >> CRT_CAMERA_APERTURE_SHIFT) & 3u;
}

float cameraLensRadius()
{
	return pc.lensU.w;
}

// a point of the unit aperture for the lens sample `u`: the disk by the lens mapping, a polygon by one triangle of its
// fan, an image by its table
vec2 sampleAperture(vec2 u)
{
	const uint shape = cameraApertureShape();
	if (shape == CRT_APERTURE_POLYGON) {
		const uint blades = pc.apertureBlades;
		const float scaled = u.x * float(blades);
		const uint k = min(uint(scaled), blades - 1u);
		const vec3 barycentrics = sampleUniformTriangle(vec2(scaled - float(k), u.y));
		const float step = 2.0 * CRT_PI / float(blades);
		const vec2 c0 = vec2(cos(float(k) * step), sin(float(k) * step));
		const vec2 c1 = vec2(cos(float(k + 1u) * step), sin(float(k + 1u) * step));
		const vec2 p = barycentrics.y * c0 + barycentrics.z * c1;
		return vec2(pc.apertureCos * p.x - pc.apertureSin * p.y, pc.apertureSin * p.x + pc.apertureCos * p.y);
	}
	if (shape == CRT_APERTURE_IMAGE) {
		// row 0 of the image is the aperture's top
		float pdf;
		const vec2 uv = sampleImageTable(pc.apertureTable, u, pdf);
		return vec2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y);
	}
	return (pc.cameraFlags & CRT_CAMERA_FLAG_CONCENTRIC) != 0u ? sampleUniformDiskConcentric(u) : diskFromSample(u);
}

// The ray through the point `film` of the image, u right and v up over [0, 1]^2, and the lens sample `lens`, which a
// pinhole and the spherical camera never read. The direction is not normalized: a perspective ray reaches its point of
// focus at t = 1
void generateCameraRay(vec2 film, vec2 lens, out vec3 rayOrigin, out vec3 rayDirection)
{
	if (!CRT_CAMERA_MODEL) {
		// Ray Tracing in One Weekend's camera, in the arithmetic every render before the camera kinds was made with: a
		// change of context moved Metal's fast math by an ulp here
		vec3 origin = pc.origin.xyz;
		vec3 target = pc.lowerLeft.xyz + film.x * pc.horizontal.xyz + film.y * pc.vertical.xyz;
		const float lensRadius = pc.lensU.w;
		if (lensRadius > 0.0) {
			vec2 rd = lensRadius * diskFromSample(lens);
			origin += rd.x * pc.lensU.xyz + rd.y * pc.lensV.xyz;
		}
		rayOrigin = origin;
		rayDirection = target - origin;
		return;
	}
	if (pc.cameraKind == CRT_CAMERA_SPHERICAL) {
		// the image's v runs up, the mappings' rows down. pbrt's equal-area camera has its pole on camera y and looks down
		// its +z, which is this camera's -z
		const vec2 uv = vec2(film.x, 1.0 - film.y);
		vec3 d;
		if ((pc.cameraFlags & CRT_CAMERA_FLAG_EQUAL_AREA) != 0u) {
			const vec3 s = equalAreaSquareToSphere(uv);
			d = vec3(s.x, s.z, -s.y);
		} else {
			d = equirectDirectionOf(uv);
		}
		rayOrigin = pc.origin.xyz;
		rayDirection = d.x * pc.lensU.xyz + d.y * pc.lensV.xyz + d.z * pc.cameraBackward.xyz;
		return;
	}

	// the point of the film, on the untilted plane of focus; the chief ray reaches it from the lens's center
	vec3 origin = pc.origin.xyz;
	const vec3 target = pc.lowerLeft.xyz + film.x * pc.horizontal.xyz + film.y * pc.vertical.xyz;
	if (pc.cameraKind == CRT_CAMERA_ORTHOGRAPHIC) {
		origin = target + pc.cameraBackward.w * pc.cameraBackward.xyz;
	}
	const vec3 chief = origin;
	const float lensRadius = cameraLensRadius();
	if (lensRadius > 0.0) {
		vec2 rd = lensRadius * sampleAperture(lens);
		origin += rd.x * pc.lensU.xyz + rd.y * pc.lensV.xyz;
	}
	vec3 focus = target;
	if ((pc.cameraFlags & CRT_CAMERA_FLAG_TILTED) != 0u) {
		// where the chief ray meets the tilted plane; one that never does is focused at infinity, so every ray from the
		// lens runs parallel to it
		const vec3 d = target - chief;
		const float along = (pc.focusPlane.w - dot(pc.focusPlane.xyz, chief)) / dot(pc.focusPlane.xyz, d);
		if (!(along > 0.0) || isinf(along)) {
			rayOrigin = origin;
			rayDirection = d;
			return;
		}
		focus = chief + along * d;
	}
	rayOrigin = origin;
	rayDirection = focus - origin;
}

//---------------------------------------------------------------- camera importance

// What light tracing and the bidirectional integrators need of the camera (PBR 3ed 16.1.1; pbrt-v4's
// PerspectiveCamera::We(), PDF_We() and SampleWi()), mirrored by src/camera_model.cpp. Only a perspective camera without a
// tilt has them; the schedules refuse the others. A pinhole's lens density is 1, pbrt's convention

// the density apertureDensity() of src/camera_model.cpp gives a point of the unit aperture, per unit of its area
float apertureDensity(vec2 p)
{
	const uint shape = cameraApertureShape();
	if (shape == CRT_APERTURE_POLYGON) {
		const vec2 q = vec2(pc.apertureCos * p.x + pc.apertureSin * p.y, -pc.apertureSin * p.x + pc.apertureCos * p.y);
		const float step = 2.0 * CRT_PI / float(pc.apertureBlades);
		const float apothem = cos(step / 2.0);
		for (uint k = 0u; k < pc.apertureBlades; k++) {
			const float middle = (float(k) + 0.5) * step;
			if (q.x * cos(middle) + q.y * sin(middle) > apothem) {
				return 0.0;
			}
		}
		return 1.0 / (0.5 * float(pc.apertureBlades) * sin(step));
	}
	if (shape == CRT_APERTURE_IMAGE) {
		if (abs(p.x) > 1.0 || abs(p.y) > 1.0) {
			return 0.0;
		}
		return imageTablePdf(pc.apertureTable, vec2(0.5 * (p.x + 1.0), 0.5 * (1.0 - p.y))) / 4.0;
	}
	return dot(p, p) <= 1.0 ? 1.0 / CRT_PI : 0.0;
}

float lensDensity(vec3 lensPoint)
{
	const float lensRadius = cameraLensRadius();
	if (!(lensRadius > 0.0)) {
		return 1.0;
	}
	const vec3 offset = lensPoint - pc.origin.xyz;
	const vec2 unit = vec2(dot(offset, pc.lensU.xyz) / lensRadius, dot(offset, pc.lensV.xyz) / lensRadius);
	return apertureDensity(unit) / (lensRadius * lensRadius);
}

// the importance of a ray from `lensPoint` along the unit `direction`, and in `film` the point of the image it reaches
// (u right, v up); 0 for a ray that misses the film
float cameraWe(vec3 lensPoint, vec3 direction, out vec2 film)
{
	film = vec2(-1.0);
	const float cosTheta = -dot(direction, pc.cameraBackward.xyz);
	if (!(cosTheta > 0.0)) {
		return 0.0;
	}
	// where the ray meets the plane of focus, which the film's rectangle lies on
	const vec3 focus = lensPoint + (pc.cameraBackward.w / cosTheta) * direction;
	const vec3 local = focus - pc.lowerLeft.xyz;
	film = vec2(dot(local, pc.horizontal.xyz) / dot(pc.horizontal.xyz, pc.horizontal.xyz), dot(local, pc.vertical.xyz) / dot(pc.vertical.xyz, pc.vertical.xyz));
	if (film.x < 0.0 || film.x >= 1.0 || film.y < 0.0 || film.y >= 1.0) {
		return 0.0;
	}
	const float cos2Theta = cosTheta * cosTheta;
	return lensDensity(lensPoint) / (pc.filmArea * cos2Theta * cos2Theta);
}

void cameraPdfWe(vec3 lensPoint, vec3 direction, out float pdfPos, out float pdfDir)
{
	pdfPos = 0.0;
	pdfDir = 0.0;
	vec2 film;
	if (cameraWe(lensPoint, direction, film) == 0.0) {
		return;
	}
	const float cosTheta = -dot(direction, pc.cameraBackward.xyz);
	pdfPos = lensDensity(lensPoint);
	pdfDir = 1.0 / (pc.filmArea * cosTheta * cosTheta * cosTheta);
}

struct CameraWiSample {
	vec3 lensPoint;
	// unit, from the reference point towards the lens point
	vec3 wi;
	float distance;
	// per unit of solid angle at the reference point
	float pdf;
	float we;
	vec2 film;
};

// a point on the lens for the lens sample `u`, seen from `ref`: false when the ray from it to `ref` misses the film
bool cameraSampleWi(vec3 ref, vec2 u, out CameraWiSample s)
{
	s.lensPoint = pc.origin.xyz;
	s.wi = vec3(0.0, 0.0, 1.0);
	s.distance = 0.0;
	s.pdf = 0.0;
	s.we = 0.0;
	s.film = vec2(-1.0);
	const float lensRadius = cameraLensRadius();
	if (lensRadius > 0.0) {
		const vec2 rd = lensRadius * sampleAperture(u);
		s.lensPoint += rd.x * pc.lensU.xyz + rd.y * pc.lensV.xyz;
	}
	const vec3 toLens = s.lensPoint - ref;
	const float distanceSquared = dot(toLens, toLens);
	if (!(distanceSquared > 0.0)) {
		return false;
	}
	s.distance = sqrt(distanceSquared);
	s.wi = toLens / s.distance;
	s.we = cameraWe(s.lensPoint, -s.wi, s.film);
	const float cosLens = abs(dot(pc.cameraBackward.xyz, s.wi));
	if (!(s.we > 0.0) || !(cosLens > 0.0)) {
		return false;
	}
	s.pdf = lensDensity(s.lensPoint) * distanceSquared / cosLens;
	return s.pdf > 0.0;
}

#endif
