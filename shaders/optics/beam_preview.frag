#version 450

#extension GL_GOOGLE_include_directive : require

//a beam tube (docs/plans/completed/physoptics-beam-preview.md) into weighted blended order-independent transparency (McGuire
//and Bavoil 2013), which shaders/optics/beam_preview_composite.comp lays over the draw image. Each tube's near wall only, or
//its far wall while the eye is inside it, as translucent as the beam's Gaussian profile integrated along the view ray as far
//as a surface that ends the beam. A disk where the beam starts or lands is the Gaussian on it. Each fragment is weighted more
//the nearer it is. A segment cut off by the solver's caps is dashed

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inDistance;
layout(location = 4) flat in uint inFlags;
layout(location = 5) in vec3 inCenter;
layout(location = 6) in vec3 inAxisA;
layout(location = 7) in vec3 inAxisB;
layout(location = 8) in float inDim;
layout(location = 9) flat in vec4 inStart;
layout(location = 10) flat in vec3 inDirection;
layout(location = 11) in float inViewDepth;
layout(location = 12) in vec2 inDisk;
layout(location = 13) flat in vec4 inStartPlane;
layout(location = 14) flat in vec4 inEndPlane;

#include "beam_common.glsl"

//the weighted premultiplied color and alpha, summed; and alpha, which the blend multiplies the revealage by (1 - alpha)
layout(location = 0) out vec4 outAccum;
layout(location = 1) out vec4 outReveal;

const uint BEAM_SECTION_DASHED = 1u;
const uint BEAM_SECTION_SPOT = 2u;

//how far from the eye the view ray `ray` crosses this plane heading out of the tube, no nearer than `nearest`. Never, for a
//ray heading back into the tube or a plane of zeros
float rayLeavesThrough(vec4 plane, vec3 ray, float nearest)
{
	const float facing = dot(plane.xyz, ray);
	return facing > 1e-9 ? max((plane.w - dot(plane.xyz, preview.eye.xyz)) / facing, nearest) : 1e30;
}

void main()
{
	if ((inFlags & BEAM_SECTION_DASHED) != 0u && fract(inDistance / preview.eye.w) > 0.5) {
		discard;
	}
	const vec3 toEye = preview.eye.xyz - inPosition;
	const vec3 view = normalize(toEye);
	const vec3 normal = normalize(inNormal);
	float alpha;
	vec3 color = inColor.rgb;
	if ((inFlags & BEAM_SECTION_SPOT) != 0u) {
		//the beam's Gaussian on the surface, taken down to zero at the rim drawn so the rim shows no edge
		const float k2 = preview.beam.x * preview.beam.x;
		const float rim = exp(-2.0 * k2);
		alpha = clamp(0.8 * inColor.a * (exp(-2.0 * k2 * dot(inDisk, inDisk)) - rim) / (1.0 - rim), 0.0, 1.0);
	} else {
		if (dot(inNormal, toEye) < 0.0 && !beamEyeInside(inStart, inAxisA, inAxisB, inDirection)) {
			discard;
		}
		//from a near wall the ray stays in the beam until it leaves through the far wall or a surface that ends the beam
		float rayEnd = 1e30;
		if (dot(inNormal, toEye) >= 0.0) {
			const float here = length(toEye);
			rayEnd = min(rayLeavesThrough(inStartPlane, -view, here), rayLeavesThrough(inEndPlane, -view, here));
		}
		const float glow = beamGlow(inPosition, inCenter, inAxisA, inAxisB, inDirection, rayEnd);
		alpha = clamp(0.8 * inColor.a * glow * inDim, 0.0, 0.95);
		//color *= 0.3 + 0.7 * abs(dot(normal, view));
	}
	const float weight = alpha * clamp(pow(preview.screen.z / max(inViewDepth, 1e-6), 8.0), 1e-2, 3e3);
	outAccum = vec4(color * alpha, alpha) * weight;
	outReveal = vec4(alpha);
}
