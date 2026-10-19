// THE SURFACE INTERACTION: everything a kernel needs to know about a surface hit, rebuilt from the
// thin HitRecord kernel 02 wrote and the ray that made it (PBR 4ed 3.11, and 15.3.6 for building it
// after the fact). Kernels 04 and 06 call surfaceInteraction(); kernel 03 has a miss and needs only
// the direction, so it does not. Later stages (media, auxiliary outputs) call it the same way.
//
// Everything here is computed for every hit, derivatives included: a kernel that never reads a
// field leaves its arithmetic to the compiler, which drops it.
//
// NORMALS POINT OUTWARD AS AUTHORED, as in pbrt (PBR 4ed 9.1.1), never towards the ray. `n` is the
// true geometric normal, a triangle's from its winding and turned to the side of its vertex normals
// (pbrt's orientationIsAuthoritative), a shape's outward. `ns` is the shading normal, the triangle's
// interpolated vertex normals or the shape's own, before any normal map: a normal map belongs to the
// material, and crt_scatter.glsl applies it. The existing materials want a normal that faces the
// incoming ray, and facingShadingNormal() turns `ns` the way kernel 02 used to.
//
// `p` is rayOrigin + t * rayDirection, the point the tracer has always used, written as an explicit
// fma(). Kernel 02 is compiled to fuse that expression and kernel 04 is not, so without the fma the
// two kernels rebuild points an ulp apart and kernel 04's MIS weight for a cylinder light, whose pdf
// depends on the exact point, drifts. Every kernel that forms a hit point from t spells the fusion
// out. Under CRT_FLAG_ERROR_BOUNDS the point is instead rebuilt from the hit itself, a triangle's from
// its barycentrics and a shape's from the object-space ray, and `pError` bounds it (PBR 4ed 6.8.5):
// the spawn point of every ray leaving the surface is offset by it (offsetRayOrigin()).
//
// Requires crt_common.glsl.

#include "crt_shape.glsl"

struct SurfaceInteraction {
	// world space
	vec3 p;
	// the absolute error bound of p, per coordinate: what a ray spawned here is offset past. Zero
	// unless CRT_FLAG_ERROR_BOUNDS is set
	vec3 pError;
	// the geometric normal, unit, pointing out of the surface as authored
	vec3 n;
	// the shading normal, unit, on the same side as n, before the material's normal map
	vec3 ns;
	// the position's derivatives with respect to uv, world space. A mesh without usable uvs gets an
	// arbitrary frame around n, as in pbrt. shadingFrame() builds the material's tangent from dpdu
	vec3 dpdu;
	vec3 dpdv;
	// the derivatives of the shading normal (unnormalised) with respect to uv, world space: a
	// triangle's from its vertex normals, a shape's from its curvature
	vec3 dndu;
	vec3 dndv;
	vec2 uv;
	// into materials[]
	uint material;
	// the light list record of the surface hit, or CRT_NO_LIGHT: what kernel 04 prices the MIS weight
	// of an emitter found by a BSDF sample against
	uint lightIndex;
	// the ray arrived against n, on the outside of the surface as authored
	bool frontFace;
};

// crt_common.glsl GpuTriangleAttributes.tangents: one octahedral-encoded unit vector.
// src/rt_accel.cpp packUnitVector
vec3 unpackTangent(uint word)
{
	const vec2 e = unpackSnorm2x16(word);
	vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	if (v.z < 0.0) {
		v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
	}
	return normalize(v);
}

// the interpolated vertex normal in object space, unnormalised, which is what makes a low-poly mesh
// shade smoothly. A mesh with no NORMAL attribute loaded as (1,0,0) everywhere, so a degenerate
// result falls back to the geometric normal rather than producing NaN
vec3 triangleObjectNormal(GpuTriangleAttributes attributes, BvhTriangle tri, vec2 bary)
{
	const float w = 1.0 - bary.x - bary.y;
	vec3 normal = w * attributes.n0.xyz + bary.x * attributes.n1.xyz + bary.y * attributes.n2.xyz;
	if (dot(normal, normal) < 1e-12) {
		normal = cross(tri.v1.xyz - tri.v0.xyz, tri.v2.xyz - tri.v0.xyz);
	}
	return normal;
}

// two unit vectors completing a frame around the unit vector v (Duff et al. 2017, as pbrt's
// CoordinateSystem)
void interactionFrame(vec3 v, out vec3 a, out vec3 b)
{
	const float sign = v.z >= 0.0 ? 1.0 : -1.0;
	const float s = -1.0 / (sign + v.z);
	const float t = v.x * v.y * s;
	a = vec3(1.0 + sign * v.x * v.x * s, sign * t, -sign * v.x);
	b = vec3(t, sign + v.y * v.y * s, -v.y);
}

// A triangle's uv derivatives in object space, pbrt-v4's Triangle::InteractionFromIntersection():
// the position's from the uv differences across its edges, the normal's from its vertex normals the
// same way. Without uvs, or with degenerate ones, the position's become an arbitrary frame around the
// geometric normal and the normal's a frame scaled by how the vertex normals spread
void triangleDerivatives(GpuTriangleAttributes attributes, BvhTriangle tri, out vec3 dpdu, out vec3 dpdv, out vec3 dndu, out vec3 dndv)
{
	const vec2 uv0 = vec2(attributes.n0.w, attributes.vAndSurface.x);
	const vec2 uv1 = vec2(attributes.n1.w, attributes.vAndSurface.y);
	const vec2 uv2 = vec2(attributes.n2.w, attributes.vAndSurface.z);
	const vec2 duv02 = uv0 - uv2;
	const vec2 duv12 = uv1 - uv2;
	// p0 - p2 and p1 - p2 from the stored edges p1 - p0 and p2 - p0
	const vec3 e1 = tri.v1.xyz - tri.v0.xyz;
	const vec3 e2 = tri.v2.xyz - tri.v0.xyz;
	const vec3 dp02 = -e2;
	const vec3 dp12 = e1 - e2;
	const float determinant = duv02.x * duv12.y - duv02.y * duv12.x;

	bool degenerate = abs(determinant) < 1e-9;
	if (!degenerate) {
		const float invDet = 1.0 / determinant;
		dpdu = (duv12.y * dp02 - duv02.y * dp12) * invDet;
		dpdv = (duv02.x * dp12 - duv12.x * dp02) * invDet;
		const vec3 c = cross(dpdu, dpdv);
		degenerate = !(dot(c, c) > 0.0);
	}
	if (degenerate) {
		interactionFrame(normalize(cross(e2, e1)), dpdu, dpdv);
	}

	const vec3 dn1 = attributes.n0.xyz - attributes.n2.xyz;
	const vec3 dn2 = attributes.n1.xyz - attributes.n2.xyz;
	if (abs(determinant) < 1e-32) {
		const vec3 dn = cross(attributes.n2.xyz - attributes.n0.xyz, attributes.n1.xyz - attributes.n0.xyz);
		if (dot(dn, dn) == 0.0) {
			dndu = vec3(0.0);
			dndv = vec3(0.0);
		} else {
			interactionFrame(normalize(dn), dndu, dndv);
			dndu *= length(dn);
			dndv *= length(dn);
		}
	} else {
		const float invDet = 1.0 / determinant;
		dndu = (duv12.y * dn1 - duv02.y * dn2) * invDet;
		dndv = (duv02.x * dn2 - duv12.x * dn1) * invDet;
	}
}

// the surface `hit` landed on, seen along the ray (rayOrigin, rayDirection) that found it.
// rayDirection must be the unit direction kernel 02 traced, so that p is the same point. Requires a
// hit: t >= 0
SurfaceInteraction surfaceInteraction(HitRecord hit, vec3 rayOrigin, vec3 rayDirection)
{
	SurfaceInteraction si;
	const GpuInstance instance = instances[hit.instance];
	si.p = fma(vec3(hit.t), rayDirection, rayOrigin);
	si.pError = vec3(0.0);
	si.lightIndex = CRT_NO_LIGHT;
	const bool robust = (pc.flags & CRT_FLAG_ERROR_BOUNDS) != 0u;

	vec3 objectN;
	vec3 objectNs;
	vec3 dpdu;
	vec3 dpdv;
	vec3 dndu;
	vec3 dndv;
	if (instance.nodeBase == CRT_INSTANCE_SHAPE) {
		const uint kind = instance.triangleBase;
		// the hit point back in the unit primitive's space, where the shape is defined (the same
		// point kernel 02 took the part and uv from)
		vec3 objectError;
		const vec3 objectPoint = shapeObjectHit(instance, rayOrigin, rayDirection, hit.t, objectError);
		if (robust) {
			si.p = toWorldPoint(instance, objectPoint, objectError, si.pError);
		}
		objectN = shapeNormal(kind, objectPoint);
		objectNs = objectN;
		shapeDerivatives(kind, hit.primitive, objectPoint, dpdu, dpdv, dndu, dndv);
		si.uv = hit.barycentrics;
		si.material = instance.materialBase;
		// an emitting shape's light record; a box has one per face, in boxFace() order
		if (instance.lightBase != CRT_NO_LIGHT) {
			si.lightIndex = instance.lightBase + (kind == CRT_SHAPE_BOX ? hit.primitive : 0u);
		}
	} else {
		const BvhTriangle tri = blasTriangles[hit.primitive];
		const uint triangle = floatBitsToUint(tri.v0.w) & CRT_TRIANGLE_INDEX_MASK;
		const GpuTriangleAttributes attributes = triangleAttributes[instance.attributeBase + triangle];
		if (robust) {
			// the point rebuilt from its barycentrics in object space, where it is as accurate as the
			// vertices: b0 p0 + b1 p1 + b2 p2 has an error of gamma(7) (|b0 p0| + |b1 p1| + |b2 p2|)
			// (PBR 4ed 6.8.5). b0 is 1 - b1 - b2, which is not itself exact: gamma(2) of the three
			// vertices' magnitude covers that rounding as well
			const float b1 = hit.barycentrics.x;
			const float b2 = hit.barycentrics.y;
			const float b0 = 1.0 - b1 - b2;
			const vec3 weighted0 = b0 * tri.v0.xyz;
			const vec3 weighted1 = b1 * tri.v1.xyz;
			const vec3 weighted2 = b2 * tri.v2.xyz;
			const vec3 objectPoint = fma(vec3(b2), tri.v2.xyz, fma(vec3(b1), tri.v1.xyz, weighted0));
			const vec3 objectError = CRT_GAMMA(7) * (abs(weighted0) + abs(weighted1) + abs(weighted2)) + CRT_GAMMA(2) * (abs(tri.v0.xyz) + abs(tri.v1.xyz) + abs(tri.v2.xyz));
			si.p = toWorldPoint(instance, objectPoint, objectError, si.pError);
		}
		objectNs = triangleObjectNormal(attributes, tri, hit.barycentrics);
		objectN = cross(tri.v1.xyz - tri.v0.xyz, tri.v2.xyz - tri.v0.xyz);
		triangleDerivatives(attributes, tri, dpdu, dpdv, dndu, dndv);
		si.uv = triangleUv(attributes, hit.barycentrics);
		si.material = instanceMaterials[instance.materialBase + floatBitsToUint(tri.v1.w)];
		// an emitting triangle's light record, found through its mesh-local index
		if (instance.lightBase != CRT_NO_LIGHT) {
			si.lightIndex = triangleLights[instance.lightBase + triangle];
		}
	}

	si.ns = worldNormal(instance, objectNs);
	si.n = worldNormal(instance, objectN);
	// the vertex normals say which side is out
	if (dot(si.n, si.ns) < 0.0) {
		si.n = -si.n;
	}
	si.dpdu = toWorldVector(instance, dpdu);
	si.dpdv = toWorldVector(instance, dpdv);
	si.dndu = toWorldNormal(instance, dndu);
	si.dndv = toWorldNormal(instance, dndv);
	si.frontFace = dot(rayDirection, si.n) < 0.0;
	return si;
}

// The shading normal the existing materials take: ns turned to face the incoming ray, and which side
// of the shading surface the ray arrived on, which a dielectric reads as entering or leaving. Kernel
// 02 decided both this way before the hit record was thin, and the arithmetic is the same.
// `inDir` is the unit incoming direction
vec3 facingShadingNormal(SurfaceInteraction si, vec3 inDir, out bool frontFace)
{
	frontFace = dot(inDir, si.ns) < 0.0;
	return frontFace ? si.ns : -si.ns;
}

// The shading frame the materials work in: the tangent is dpdu made perpendicular to `normal`
// (Gram-Schmidt), the bitangent completes it. Where dpdu is parallel to the normal, or zero (a
// sphere's pole), the frame is an arbitrary one around the normal. Columns tangent, bitangent, normal
mat3 shadingFrame(SurfaceInteraction si, vec3 normal)
{
	vec3 tangent = si.dpdu - normal * dot(normal, si.dpdu);
	vec3 bitangent;
	if (dot(tangent, tangent) < 1e-20) {
		interactionFrame(normal, tangent, bitangent);
	} else {
		tangent = normalize(tangent);
		bitangent = cross(normal, tangent);
	}
	return mat3(tangent, bitangent, normal);
}
