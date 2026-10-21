// The debug views. Each one replaces shading: it writes its value as the path's whole
// contribution and ends the path there, so a debug view accumulates - and averages under
// jitter - through exactly the same running mean as radiance does.
//
// Kernels 03, 04 and 06 all terminate paths, so all three consult debugTerminalValue() before
// doing their own work. A view that is meaningful in only one of them (the background heat in
// 03, say) still has to be handled in all three, or a path that ends elsewhere leaves the
// pre-written zero and the image gains black holes.
//
// Requires crt_common.glsl.

vec3 bounceHeat(uint bounce)
{
	return vec3(float(bounce) / float(pc.rayDepth));
}

// node visits + primitive tests of the ray that reached this hit, as a jet ramp: blue is cheap,
// red is CRT_TRAVERSAL_HEAT_SCALE or more. Where the acceleration structure struggles -
// overlapping instances, long thin triangles, grazing rays along walls - lights up
#define CRT_TRAVERSAL_HEAT_SCALE 256.0
vec3 traversalHeat(float cost)
{
	const float t = clamp(cost / CRT_TRAVERSAL_HEAT_SCALE, 0.0, 1.0);
	return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

// The value a terminating kernel should write for the active debug view, or false when the
// view is not one that terminates here (CRT_DEBUG_NONE, or the bounce heat, which only ends a
// path when the path itself ends). `inDir` is the unit incoming direction. For a hit, `normal` is
// the shading normal the materials use and `geometricNormal` the surface's own, both facing the
// ray; a miss passes zeros. `traversalCost` is what kernel 02 left in the hit record, hit or miss
bool debugTerminalValue(vec3 normal, vec3 geometricNormal, float traversalCost, vec3 inDir, bool isHit, out vec3 value)
{
	value = vec3(0.0);
	switch (pc.debugView) {
	case CRT_DEBUG_PRIMARY_DIRECTION:
		value = inDir * 0.5 + 0.5;
		return true;
	case CRT_DEBUG_HIT_MISS:
		value = isHit ? vec3(1.0) : vec3(0.0);
		return true;
	case CRT_DEBUG_NORMAL:
		value = isHit ? normal * 0.5 + 0.5 : vec3(0.0);
		return true;
	case CRT_DEBUG_TRAVERSAL_COST:
		value = traversalHeat(traversalCost);
		return true;
	case CRT_DEBUG_AMBIENT_OCCLUSION:
		// kernel 06 casts the rays and kernel 08 adds what reaches; a miss is black and a light seen straight is
		// unoccluded, so white. Neither has a ray of its own to cast (kernels 03 and 04 hold no shadow queue)
		value = isHit ? vec3(1.0) : vec3(0.0);
		return true;
	case CRT_DEBUG_NORMAL_DISAGREEMENT:
		if (isHit) {
			const float cosine = dot(normal, geometricNormal);
			value = cosine < 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(cosine);
		}
		return true;
	}
	return false;
}
