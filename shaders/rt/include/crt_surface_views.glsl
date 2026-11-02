// What a camera path's surface vertex adds beside its radiance: the ambient occlusion view's ray and the auxiliary outputs'
// visible surface. Kernel 06's (crt_surface.glsl) and the light vertex cache's stage 16
// (docs/plans/completed/pbrt-bidirectional-path-tracing.md); moved out of crt_surface.glsl unchanged.
// Requires crt_common.glsl, crt_spectrum.glsl, crt_interaction.glsl, crt_samples.glsl, crt_scatter.glsl, crt_aov.glsl,
// crt_spawn.glsl and SurfaceMaterial defined.

#ifndef CRT_SURFACE_VIEWS_GLSL
#define CRT_SURFACE_VIEWS_GLSL

// The ambient occlusion view (pbrt's AOIntegrator): one ray in a cosine-distributed direction about the shading
// normal, as long as the AO distance. Kernel 08 adds 1 to the path's radiance for the ray that reaches nothing, so
// the pixel is the fraction that do, and the path ends here
bool ambientOcclusionRay(SurfaceInteraction si, vec3 normal, uint pathIndex, PathState path, uint entry)
{
	uint rng = path.rngState;
	const vec3 direction = hemisphereFromSample(normal, 1.0, sampleDimension2D(pathIndex, rng, CRT_SAMPLE_DIRECT_POINT));
	ShadowRay ray;
	if (!spawnShadowRay(si, direction, pc.aoDistance, vec3(0.0), ray)) {
		return false;
	}
	ray.contribution = spectrumConst(1.0);
	ray.radianceSlot = path.radianceSlot;
	shadowRays[entry] = ray;
	return true;
}

// The camera path's visible surface, if this is the one (aovWantsSurface()): its position, normals, uv and ids go
// into PixelState, with the BSDF's directional albedo towards the camera when that output is on. The albedo is the
// mean of the weights scatterSurface() returns over pbrt's 16 fixed samples (crt_samples.glsl): a Lambertian
// surface gives its color, glass about 1, a mirror its tint, a fuzzy metal its tint times the share of samples
// that do not go below the surface. It draws none of the path's own numbers
void aovCapture(uint slot, HitRecord hit, SurfaceInteraction si, SurfaceMaterial material, vec3 inDir, vec3 normal, bool frontFace)
{
	if (!aovWantsSurface(slot, isSpecularMaterial(material))) {
		return;
	}
	vec3 albedo = vec3(0.0);
	if ((pc.aovOutputs & CRT_AOV_OUTPUT_ALBEDO) != 0u) {
		Spectrum sum = spectrumConst(0.0);
		for (int i = 0; i < CRT_ALBEDO_SAMPLES; i++) {
			g_albedoSample = i;
			uint rng = 0x9E3779B9u * uint(i + 1);
			Spectrum attenuation;
			vec3 outDir;
			if (scatterSurface(material, inDir, normal, frontFace, slot, rng, attenuation, outDir)) {
				sum = spectrumAdd(sum, attenuation);
			}
		}
		g_albedoSample = -1;
		// a spectral build reports the reflectance's color under the working space's white
#ifndef CRT_SPECTRAL
		albedo = sum;
		albedo *= 1.0 / float(CRT_ALBEDO_SAMPLES);
#else
		albedo = reflectanceToFilm(spectrumScale(sum, 1.0 / float(CRT_ALBEDO_SAMPLES)));
#endif
	}
	// the shading normal on the geometric normal's side, as pbrt's VisibleSurface has it
	const vec3 shadingNormal = dot(normal, si.n) < 0.0 ? -normal : normal;
	aovRecord(slot, si.p, si.n, shadingNormal, si.uv, albedo, hit.instance, si.material);
}

#endif
