// THE DETECTORS (docs/plans/completed/physoptics-detectors.md): where a light path's power lands on a detector's film,
// and what it adds there. Stage 26 (2601_deposit_detector.comp) calls these for every light path segment that ends on
// a sensor's front or crosses a probe plane's. The records and the films are bindings 59 and 60 (crt_common.glsl); the
// CPU side is src/optic_detectors.h (the films' sizes and the readings) and src/rt_detectors.h (the buffers).
//
// A film is one float per pixel per channel: the three of the exact RGB build, in watts per channel as the laser's RGB
// color defines them (physoptics-laser-sources.md), or one in a spectral build, the lanes' power integrated over
// wavelength. A spectrometer in a spectral build also keeps its spectrum: each lane's power in the bin of its wavelength.
// A frame's sums are kept in copies, a path adding to the one its index picks (src/optic_detectors.h frameCopies()), which
// the fold adds together: a million paths on one pixel would round away a thousandth of their power in one float. Every
// value is a light path's power, its throughput, unnormalized: the CPU divides the sums by the paths emitted, which makes
// them watts.
// Requires crt_common.glsl and crt_spectrum.glsl, and GL_EXT_shader_atomic_float.

#ifndef CRT_DETECTOR_GLSL
#define CRT_DETECTOR_GLSL

// src/optic_detectors.h MAX_DETECTORS, SPECTRUM_COPIES; src/rt_accel.h GPU_DETECTOR_COPIES_SHIFT
#define CRT_MAX_DETECTORS 32u
#define CRT_SPECTRUM_COPIES 8u
#define CRT_DETECTOR_COPIES_SHIFT 8u
// DetectorRecord.flags: src/rt_accel.h GPU_DETECTOR_*
#define CRT_DETECTOR_DISK 1u
#define CRT_DETECTOR_PROBE 2u
#define CRT_DETECTOR_SPECTROMETER 4u
#define CRT_DETECTOR_NO_INSTANCE 0xFFFFFFFFu

// the sensor covers this point of its plane, in its frame
bool detectorCovers(DetectorRecord d, vec2 local)
{
	if ((d.flags & CRT_DETECTOR_DISK) != 0u) {
		return dot(local, local) <= d.origin.w * d.origin.w;
	}
	return abs(local.x) <= d.origin.w && abs(local.y) <= d.uAxis.w;
}

// the film pixel of a point of the sensor: column along its x, row along its y, row 0 at -half height
uint detectorPixel(DetectorRecord d, vec2 local)
{
	const vec2 halfSize = vec2(d.origin.w, d.uAxis.w);
	const vec2 unit = (local + halfSize) / (2.0 * halfSize);
	const uint x = uint(clamp(int(floor(unit.x * float(d.pixelsX))), 0, int(d.pixelsX) - 1));
	const uint y = uint(clamp(int(floor(unit.y * float(d.pixelsY))), 0, int(d.pixelsY) - 1));
	return y * d.pixelsX + x;
}

#ifdef CRT_SPECTRAL
// what a lane's value is worth in watts: a hero lane's Monte Carlo weight over wavelength, 1 / (pdf lanes); a band's
// width (its basis's integral), which binding 59 holds; a monochromatic render's one lane as it is, in W/nm
float detectorLaneWatts(uint lane)
{
	const uint representation = spectralRepresentation();
	if (representation == CRT_REPRESENTATION_MONOCHROMATIC) {
		return lane == 0u ? 1.0 : 0.0;
	}
	if (representation == CRT_REPRESENTATION_HERO) {
		return g_lambdaPdf[lane] > 0.0 ? 1.0 / (g_lambdaPdf[lane] * float(spectrumActiveLanes())) : 0.0;
	}
	return lane < 32u ? detectorBandWidths[lane >> 2u][lane & 3u] : 0.0;
}
#endif

// A light path's power `beta` reaching the sensor of record `d` at `local`: added to its pixel, and for a spectrometer
// in a spectral build, lane by lane to its spectrum's bins, in the copies path `pathIndex` picks
void detectorDeposit(DetectorRecord d, vec2 local, Spectrum beta, uint pathIndex)
{
	const uint pixel = detectorPixel(d, local);
	const uint copies = 1u << ((d.flags >> CRT_DETECTOR_COPIES_SHIFT) & 15u);
	const uint frame = d.imageOffset + (pathIndex & (copies - 1u)) * (d.pixelsX * d.pixelsY * d.channels);
#ifndef CRT_SPECTRAL
	const uint base = frame + pixel * 3u;
	if (beta.r != 0.0) {
		atomicAdd(detectorFilm[base], beta.r);
	}
	if (beta.g != 0.0) {
		atomicAdd(detectorFilm[base + 1u], beta.g);
	}
	if (beta.b != 0.0) {
		atomicAdd(detectorFilm[base + 2u], beta.b);
	}
#else
	float watts = 0.0;
	const bool spectrometer = (d.flags & CRT_DETECTOR_SPECTROMETER) != 0u && d.bins > 0u;
	const float lambdaMin = d.vAxis.w;
	const float lambdaSpan = d.normal.w - d.vAxis.w;
	for (uint i = 0u; i < CRT_LANE_END; i++) {
		const float lane = spectrumLane(beta, i) * detectorLaneWatts(i);
		if (lane == 0.0) {
			continue;
		}
		watts += lane;
		if (spectrometer) {
			const float u = (g_lambda[i] - lambdaMin) / lambdaSpan;
			if (u >= 0.0 && u < 1.0) {
				const uint bin = min(uint(u * float(d.bins)), d.bins - 1u);
				atomicAdd(detectorFilm[d.spectrumOffset + bin * CRT_SPECTRUM_COPIES + (pathIndex % CRT_SPECTRUM_COPIES)], lane);
			}
		}
	}
	if (watts != 0.0) {
		atomicAdd(detectorFilm[frame + pixel], watts);
	}
#endif
}

#endif
