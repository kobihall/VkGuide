#version 450

#extension GL_GOOGLE_include_directive : require

//the solved laser beams as tubes (docs/plans/completed/physoptics-beam-preview.md, VulkanEngine::drawBeamPreview()): one instance
//per section of src/beam_preview.h's BeamSection, BEAM_RING_VERTICES around, two triangles a step between its two rings,
//and no vertex buffer. A spot is a section whose second ring is its first's center: a flat disk

layout(set = 0, binding = 0) uniform SceneData {
	mat4 view;
	mat4 proj;
	mat4 viewproj;
	vec4 ambientColor;
	vec4 sunlightDirection;
	vec4 sunlightColor;
} sceneData;

#include "beam_common.glsl"

struct BeamSection {
	vec4 center0;
	vec4 axisA0;
	vec4 axisB0;
	vec4 center1;
	vec4 axisA1;
	vec4 axisB1;
	vec4 color;
};

layout(std430, set = 1, binding = 0) readonly buffer Sections {
	BeamSection sections[];
};

const int BEAM_RING_VERTICES = 48;
const uint BEAM_SECTION_SPOT = 2u;
const uint BEAM_SECTION_STOP_AT_START = 4u;
const uint BEAM_SECTION_STOP_AT_END = 8u;

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outColor;
//the distance from the source along the beam, and the section's flags
layout(location = 3) out float outDistance;
layout(location = 4) flat out uint outFlags;
//the beam's axis and cross-section here (the ring's center and semi-axes, interpolated between the two rings), and how much
//a widened ring's glow is dimmed
layout(location = 5) out vec3 outCenter;
layout(location = 6) out vec3 outAxisA;
layout(location = 7) out vec3 outAxisB;
layout(location = 8) out float outDim;
//the first ring's center and the section's length, and the beam's direction
layout(location = 9) flat out vec4 outStart;
layout(location = 10) flat out vec3 outDirection;
//the distance in front of the eye, which the blend weighs fragments by
layout(location = 11) out float outViewDepth;
//where on a spot's disk: 0 at its center, unit length on its rim
layout(location = 12) out vec2 outDisk;
//the surfaces the glow stops at: each a unit normal facing out of the tube and its distance from the origin, zeros for none
layout(location = 13) flat out vec4 outStartPlane;
layout(location = 14) flat out vec4 outEndPlane;

//each step's two triangles: (ring, step) of each corner
const ivec2 CORNERS[6] = ivec2[](ivec2(0, 0), ivec2(1, 0), ivec2(1, 1), ivec2(0, 0), ivec2(1, 1), ivec2(0, 1));

//how much a ring's semi-axis is widened so the beam is at least beam.y pixels in radius on screen
float widening(vec3 center, vec3 axis, vec3 d)
{
	const float depth = max(-(sceneData.view * vec4(center, 1.0)).z, 1e-6);
	const float pixels = length(axis - d * dot(axis, d)) * preview.screen.y * 0.5 * preview.screen.x / depth;
	return max(1.0, preview.beam.y / max(pixels, 1e-9));
}

//the plane of a ring laid on a surface, facing `outward`
vec4 ringPlane(vec3 center, vec3 a, vec3 b, vec3 outward)
{
	vec3 n = normalize(cross(a, b));
	if (dot(n, outward) < 0.0) {
		n = -n;
	}
	return vec4(n, dot(n, center));
}

void main()
{
	const BeamSection s = sections[gl_InstanceIndex];
	const int step = gl_VertexIndex / 6;
	const ivec2 corner = CORNERS[gl_VertexIndex % 6];
	const float angle = 6.28318530718 * float(step + corner.y) / float(BEAM_RING_VERTICES);
	const float c = cos(angle);
	const float sn = sin(angle);
	const bool second = corner.x == 1;
	outFlags = floatBitsToUint(s.axisA0.w);
	const bool spot = (outFlags & BEAM_SECTION_SPOT) != 0u;

	const vec3 span = s.center1.xyz - s.center0.xyz;
	const float length0 = length(span);
	const vec3 d = spot || length0 <= 0.0 ? normalize(cross(s.axisA0.xyz, s.axisB0.xyz)) : span / length0;
	//both rings, widened where they are under beam.y pixels; a widened ring's glow is dimmed in proportion
	vec3 a0 = s.axisA0.xyz;
	vec3 b0 = s.axisB0.xyz;
	vec3 a1 = s.axisA1.xyz;
	vec3 b1 = s.axisB1.xyz;
	float dim0 = 1.0;
	float dim1 = 1.0;
	if (!spot) {
		const float fa0 = widening(s.center0.xyz, a0, d);
		const float fb0 = widening(s.center0.xyz, b0, d);
		const float fa1 = widening(s.center1.xyz, a1, d);
		const float fb1 = widening(s.center1.xyz, b1, d);
		a0 *= fa0;
		b0 *= fb0;
		a1 *= fa1;
		b1 *= fb1;
		dim0 = inversesqrt(fa0 * fb0);
		dim1 = inversesqrt(fa1 * fb1);
	}
	const vec3 center = second ? s.center1.xyz : s.center0.xyz;
	const vec3 a = second ? a1 : a0;
	const vec3 b = second ? b1 : b0;
	vec3 p = center + c * a + sn * b;
	//a disk lies on the face it lands on, and the rounding of the two meshes' float positions is more than the depth bias
	//up close: toward the eye by 1 um and 1e-6 of the distance
	if (spot) {
		const float eyeDistance = length(preview.eye.xyz - p);
		p += (preview.eye.xyz - p) / max(eyeDistance, 1e-9) * (1e-6 + 1e-6 * eyeDistance);
	}

	//a spot's normal is its plane's; a tube's, the cross of the way around its own ring and the way along the tube at this
	//angle, which holds for a cone and for an end ring laid onto a tilted surface alike, turned outward
	if (spot) {
		outNormal = d;
	} else {
		const vec3 around = -sn * a + c * b;
		const vec3 along = (s.center1.xyz + c * a1 + sn * b1) - (s.center0.xyz + c * a0 + sn * b0);
		vec3 n = cross(around, along);
		const float lengthN = length(n);
		n = lengthN > 1e-20 ? n / lengthN : normalize(c * a + sn * b);
		outNormal = dot(n, c * a + sn * b) < 0.0 ? -n : n;
	}
	outPosition = p;
	outColor = s.color;
	outDistance = second ? s.center1.w : s.center0.w;
	outCenter = center;
	outAxisA = a;
	outAxisB = b;
	outDim = second ? dim1 : dim0;
	outStart = vec4(s.center0.xyz, length0);
	outDirection = d;
	outViewDepth = -(sceneData.view * vec4(p, 1.0)).z;
	outDisk = second ? vec2(0.0) : vec2(c, sn);
	outStartPlane = (outFlags & BEAM_SECTION_STOP_AT_START) != 0u ? ringPlane(s.center0.xyz, s.axisA0.xyz, s.axisB0.xyz, -d) : vec4(0.0);
	outEndPlane = (outFlags & BEAM_SECTION_STOP_AT_END) != 0u ? ringPlane(s.center1.xyz, s.axisA1.xyz, s.axisB1.xyz, d) : vec4(0.0);
	gl_Position = sceneData.viewproj * vec4(p, 1.0);
}
