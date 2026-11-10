//the beam preview's push constants and the Gaussian profile its fragment shader reads (shaders/optics/beam_preview.*)

layout(push_constant) uniform BeamPreview {
	//the eye, and in w the dashes' period in meters
	vec4 eye;
	//x the radius the rings are drawn at in units of w, y the least radius in pixels a ring is widened to
	vec4 beam;
	//x the viewport's height in pixels, y the projection's [1][1], z the distance the blend's depth weights are 1 at
	vec4 screen;
} preview;

//an offset from the beam's axis in units of w on each of its principal axes; a and b are the ring's semi-axes, drawn at
//beam.x w, made perpendicular to the beam's direction d (an end ring lies on a tilted surface)
vec2 beamWhitened(vec3 q, vec3 a, vec3 b, vec3 d)
{
	a -= d * dot(a, d);
	b -= d * dot(b, d);
	return preview.beam.x * vec2(dot(q, a) / dot(a, a), dot(q, b) / dot(b, b));
}

//erf(x), Abramowitz and Stegun 7.1.27
float beamErf(float x)
{
	const float a = abs(x);
	const float q = 1.0 / (1.0 + a * (0.278393 + a * (0.230389 + a * (0.000972 + a * 0.078108))));
	return sign(x) * (1.0 - q * q * q * q);
}

//the beam's Gaussian irradiance integrated along the view ray through `position`, over an infinite cylinder of the local
//cross-section and as far as `rayEnd` from the eye, relative to the whole integral seen side on through the axis: exp(-2 b^2)
//for the ray's least distance b from the axis in units of w, times how much longer the ray's path through the beam is than
//side on, at most 4 (looking down it), times the share of the Gaussian along the ray that lies before rayEnd
float beamGlow(vec3 position, vec3 center, vec3 a, vec3 b, vec3 d, float rayEnd)
{
	const vec3 view = normalize(position - preview.eye.xyz);
	const vec2 u0 = beamWhitened(preview.eye.xyz - center, a, b, d);
	const vec2 m = beamWhitened(view, a, b, d);
	const float mm = max(dot(m, m), 1e-30);
	//|m|^2 seen side on: 1 / (wa wb)
	const vec3 ap = a - d * dot(a, d);
	const vec3 bp = b - d * dot(b, d);
	const float side = preview.beam.x * preview.beam.x / (length(ap) * length(bp));
	const float t = dot(u0, m);
	const float distance2 = max(dot(u0, u0) - t * t / mm, 0.0);
	const float kept = 0.5 + 0.5 * beamErf(1.41421356 * sqrt(mm) * (rayEnd + t / mm));
	return exp(-2.0 * distance2) * min(sqrt(side / mm), 4.0) * kept;
}

//whether the eye is inside the section: within its length and its drawn radius
bool beamEyeInside(vec4 start, vec3 a, vec3 b, vec3 d)
{
	const float along = dot(preview.eye.xyz - start.xyz, d);
	if (along < 0.0 || along > start.w) {
		return false;
	}
	const vec2 u = beamWhitened(preview.eye.xyz - start.xyz, a, b, d);
	return dot(u, u) <= preview.beam.x * preview.beam.x;
}
