// SAMPLE WARPS that light sampling and the cameras share: maps from the unit square onto a triangle, a disk and the
// sphere. Mirrored line for line by src/light_sampling.cpp, which bin/light_test and bin/camera_test check.
// Requires crt_random.glsl (CRT_PI).

#ifndef CRT_WARP_GLSL
#define CRT_WARP_GLSL

// pbrt-v4's SampleUniformTriangle(): barycentrics uniform over a triangle's area
vec3 sampleUniformTriangle(vec2 u)
{
	float b0;
	float b1;
	if (u.x < u.y) {
		b0 = u.x / 2.0;
		b1 = u.y - b0;
	} else {
		b1 = u.y / 2.0;
		b0 = u.x - b1;
	}
	return vec3(b0, b1, 1.0 - b0 - b1);
}

// pbrt-v4's SampleUniformDiskConcentric(): Shirley and Chiu's low-distortion square-to-disk map, which keeps a
// stratified pattern stratified (PBR 4ed A.5.1)
vec2 sampleUniformDiskConcentric(vec2 u)
{
	const vec2 offset = 2.0 * u - 1.0;
	if (offset.x == 0.0 && offset.y == 0.0) {
		return vec2(0.0);
	}
	float r;
	float theta;
	if (abs(offset.x) > abs(offset.y)) {
		r = offset.x;
		theta = (CRT_PI / 4.0) * (offset.y / offset.x);
	} else {
		r = offset.y;
		theta = CRT_PI / 2.0 - (CRT_PI / 4.0) * (offset.x / offset.y);
	}
	return r * vec2(cos(theta), sin(theta));
}

// pbrt-v4's equal-area mapping between the unit square and the sphere (Clarberg 2008, PBR 4ed 3.8.3), which a
// goniometric light's image and an equal-area spherical camera's are laid out in (src/light_sampling.cpp)
vec3 equalAreaSquareToSphere(vec2 p)
{
	const float u = 2.0 * p.x - 1.0;
	const float v = 2.0 * p.y - 1.0;
	const float up = abs(u);
	const float vp = abs(v);
	// the radius is the signed distance from the diagonal
	const float signedDistance = 1.0 - (up + vp);
	const float d = abs(signedDistance);
	const float r = 1.0 - d;
	const float phi = (r == 0.0 ? 1.0 : (vp - up) / r + 1.0) * CRT_PI / 4.0;
	const float z = signedDistance < 0.0 ? -(1.0 - r * r) : 1.0 - r * r;
	const float cosPhi = u < 0.0 ? -abs(cos(phi)) : abs(cos(phi));
	const float sinPhi = v < 0.0 ? -abs(sin(phi)) : abs(sin(phi));
	const float s = r * sqrt(max(2.0 - r * r, 0.0));
	return vec3(cosPhi * s, sinPhi * s, z);
}

vec2 equalAreaSphereToSquare(vec3 d)
{
	const float x = abs(d.x);
	const float y = abs(d.y);
	const float z = abs(d.z);
	const float r = sqrt(max(1.0 - z, 0.0));
	const float a = max(x, y);
	float b = min(x, y);
	b = a == 0.0 ? 0.0 : b / a;
	float phi = 0.406758566246788489601959989e-5
		+ b * (0.636226545274016134946890922156
			+ b * (0.61572017898280213493197203466e-2
				+ b * (-0.247333733281268944196501420480 + b * (0.881770664775316294736387951347e-1 + b * (0.419038818029165735901852432784e-1 + b * -0.251390972343483509333252996350e-1)))));
	if (x < y) {
		phi = 1.0 - phi;
	}
	float v = phi * r;
	float u = r - v;
	if (d.z < 0.0) {
		const float t = u;
		u = 1.0 - v;
		v = 1.0 - t;
	}
	u = d.x < 0.0 ? -u : u;
	v = d.y < 0.0 ? -v : v;
	return vec2(0.5 * (u + 1.0), 0.5 * (v + 1.0));
}

#endif
