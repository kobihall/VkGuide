#pragma once

// THE GAUSSIAN BEAM'S MATH (docs/plans/completed/physoptics-gaussian-beam-solver.md), part of rt_optics: a general astigmatic
// Gaussian beam carried by complex rays through first-order ray-transfer matrices, in double. No scene, no tracing: the
// solver (src/beam_solver.h) traces the chief ray and calls these at each surface.
//
// THE COORDINATES. A beam lives in a transverse frame, two unit vectors perpendicular to its chief ray, which the solver
// carries from surface to surface (reflected at a mirror, turned with the ray at a refraction). A neighbouring ray is a
// position x in that frame and a REDUCED transverse direction u, the medium's index n times the direction's transverse
// components, so a flat interface leaves u unchanged and every ray-transfer matrix is symplectic (S^T J S = J).
//
// THE BEAM (Arnaud 1985, Arnaud and Kogelnik 1969). The field is exp(-i k0 r^T Q^-1 r / 2) with k0 the vacuum wave
// number and Q the reduced complex curvature matrix: q / n for a stigmatic beam, so that 1/Q = n / R - i lambda0 / (pi w^2)
// with lambda0 in vacuum, in any medium. It is carried as two complex rays, the columns of [X; U], with Q = X U^-1, and a
// ray-transfer matrix S = [A B; C D] maps them by [X'; U'] = S [X; U], the generalized ABCD law with no inversion along the
// way. Over a distance s in a medium of index n, Q(s) = Q + (s / n) I.
//
// M^2 (Siegman's embedded Gaussian): the complex rays carry the embedded beam, and the real beam's w^2 tensor is the
// embedded one's scaled by diag(M^2_x, M^2_y) in the transported frame, sqrt on each side. Exact while M^2_x = M^2_y or the
// beam's axes stay on the frame's; an approximation that BeamProfile::twisted flags once they turn.

#include <array>
#include <complex>
#include <functional>

#include <glm/glm.hpp>

using cdouble = std::complex<double>;

// a 2 x 2 complex matrix, column-major as glm's: m[column][row]
struct CMat2 {
	std::array<std::array<cdouble, 2>, 2> m {};
	cdouble& operator()(int row, int column) { return m[column][row]; }
	const cdouble& operator()(int row, int column) const { return m[column][row]; }
	static CMat2 identity();
	static CMat2 diagonal(cdouble a, cdouble b);
	static CMat2 fromReal(const glm::dmat2& real);
	glm::dmat2 real() const;
	glm::dmat2 imag() const;
	cdouble determinant() const;
	CMat2 inverse() const;
	CMat2 operator+(const CMat2& other) const;
	CMat2 operator*(const CMat2& other) const;
	CMat2 operator*(cdouble scale) const;
	bool operator==(const CMat2&) const = default;
};
CMat2 operator*(const glm::dmat2& real, const CMat2& complex);

// a first-order ray-transfer matrix in 2 x 2 blocks: x' = A x + B u, u' = C x + D u
struct RayTransfer {
	glm::dmat2 A { 1.0 };
	glm::dmat2 B { 0.0 };
	glm::dmat2 C { 0.0 };
	glm::dmat2 D { 1.0 };
	// this after `first`
	RayTransfer operator*(const RayTransfer& first) const;
	// how far S^T J S is from J, the largest entry of the difference
	double symplecticError() const;
	bool operator==(const RayTransfer&) const = default;
};
// free space over a physical distance in a medium of index n
RayTransfer driftTransfer(double distance, double index);

// The ray-transfer matrix of one surface at the chief ray's hit, between the incoming frame (e1, e2) of the direction d
// and the outgoing frame (f1, f2) of d', all world vectors. `normal` is the unit surface normal at the hit, either side;
// `k1`, `k2` and `dir1`, `dir2` the principal curvatures and their world directions, the curvatures positive where the
// surface bends toward `normal`. `n`, `nOut` the indices before and after (equal for a reflection). The neighbouring ray
// meets the surface's tangent plane, and its direction leaves by Snell's law at the perturbed normal: the generalized
// Coddington equations in matrix form (Stavroudis 1972; Herloski, Marshall and Antos 1983)
struct SurfaceGeometry {
	glm::dvec3 normal { 0.0, 0.0, 1.0 };
	double k1 { 0.0 };
	double k2 { 0.0 };
	glm::dvec3 dir1 { 1.0, 0.0, 0.0 };
	glm::dvec3 dir2 { 0.0, 1.0, 0.0 };
};
RayTransfer surfaceTransfer(const SurfaceGeometry& surface, const glm::dvec3& d, const glm::dvec3& e1, const glm::dvec3& e2, double n, const glm::dvec3& dOut,
	const glm::dvec3& f1, const glm::dvec3& f2, double nOut, bool reflect);
// a neighbour's point on the tangent plane for a transverse offset x of the incoming frame: the projection along d
glm::dvec3 tangentPoint(const glm::dvec3& normal, const glm::dvec3& d, const glm::dvec3& e1, const glm::dvec3& e2, const glm::dvec2& x);

// the outgoing frame after a reflection (each vector mirrored in the surface) or a refraction (each turned by the
// rotation that takes d to dOut): parallel transport, so the frame twists only when the geometry twists the beam
void transportFrame(const glm::dvec3& d, const glm::dvec3& dOut, const glm::dvec3& normal, bool reflect, const glm::dvec3& e1, const glm::dvec3& e2,
	glm::dvec3& f1, glm::dvec3& f2);
// a unit vector perpendicular to d, as close to `hint` as it can be
glm::dvec3 perpendicularTo(const glm::dvec3& d, const glm::dvec3& hint);

// the beam as two complex rays, and the embedded Gaussian's M^2 on each axis of the frame
struct ComplexRays {
	CMat2 X;
	CMat2 U;
	// the effective M^2 per frame axis: the laser's, times n0 lambda0 / lambda so a laser's own w(z) comes back exactly
	glm::dvec2 m2 { 1.0 };

	// Q = X U^-1, reduced
	CMat2 q() const;
	bool operator==(const ComplexRays&) const = default;
};
ComplexRays operator*(const RayTransfer& s, const ComplexRays& rays);
// a simple astigmatic beam on the frame's axes: each axis's reduced q = (z - zWaist + i zR) / n
ComplexRays simpleAstigmaticBeam(const glm::dvec2& waistPosition, const glm::dvec2& rayleighRange, double index, const glm::dvec2& m2);

// One principal axis of a beam over a segment: a stigmatic profile w(s)^2 = w0^2 (1 + ((s - waistAt) / zR)^2), s the
// physical distance from the segment's start
struct BeamAxisProfile {
	double waist { 0.0 };
	double waistAt { 0.0 };
	double rayleighRange { 0.0 };
	double radius(double s) const;
	// Gouy phase relative to the waist, atan((s - waistAt) / zR)
	double gouy(double s) const;
};

// what a beam is at one place: the real beam's 1/e^2 radii along its principal axes and the first axis's angle from the
// frame's first vector, the wavefront radii (infinite at a waist, positive diverging), and the w^2 tensor
struct BeamCrossSection {
	glm::dvec2 radii { 0.0 };
	double angle { 0.0 };
	glm::dvec2 curvatureRadii { 0.0 };
	glm::dmat2 w2 { 0.0 };
};
// `s` along a segment in a medium of index n from where `rays` are, at the vacuum wavelength lambda0 (m)
BeamCrossSection crossSection(const ComplexRays& rays, double s, double index, double lambda0);

// the beam's two principal profiles over a segment: from the axes of the intensity ellipse at its start, each axis's
// complex q read off the diagonal. `twisted` when the off-diagonal is more than 1e-6 of the diagonal, or when unequal
// M^2 meet axes that have turned from the frame's: then the profiles are approximate
struct BeamProfile {
	std::array<BeamAxisProfile, 2> axes;
	// the frame angle of the first profile's axis at the start
	double angle { 0.0 };
	bool twisted { false };
};
BeamProfile beamProfile(const ComplexRays& rays, double index, double lambda0);

// THE FRACTION OF A BEAM INSIDE AN APERTURE. The beam's intensity is a Gaussian of w^2 tensor `w2` about the origin of
// its transverse plane; `inside` says whether a transverse offset lands inside. In whitened coordinates over +-7 sigma: on
// each line of the first coordinate the runs inside are found on a 49-point grid, their ends refined by bisection, and
// each run integrated exactly with erf; across the first coordinate the places where the number of runs changes split
// the span, and each piece takes 24 Gauss-Legendre nodes under a = m + h sin(t), which smooths the square-root edge a run
// opens or closes with. `convex` lets a beam whose 6-sigma ellipse is inside at 32 points count as wholly inside (the
// rest is below 1e-8)
double fractionInside(const glm::dmat2& w2, const std::function<bool(const glm::dvec2&)>& inside, bool convex);
