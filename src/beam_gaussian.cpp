#include <beam_gaussian.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

//---------------------------------------------------------------- 2 x 2 complex matrices

CMat2 CMat2::identity()
{
	return diagonal(1.0, 1.0);
}

CMat2 CMat2::diagonal(cdouble a, cdouble b)
{
	CMat2 out;
	out(0, 0) = a;
	out(1, 1) = b;
	return out;
}

CMat2 CMat2::fromReal(const glm::dmat2& real)
{
	CMat2 out;
	for (int c = 0; c < 2; c++) {
		for (int r = 0; r < 2; r++) {
			out(r, c) = real[c][r];
		}
	}
	return out;
}

glm::dmat2 CMat2::real() const
{
	glm::dmat2 out;
	for (int c = 0; c < 2; c++) {
		for (int r = 0; r < 2; r++) {
			out[c][r] = (*this)(r, c).real();
		}
	}
	return out;
}

glm::dmat2 CMat2::imag() const
{
	glm::dmat2 out;
	for (int c = 0; c < 2; c++) {
		for (int r = 0; r < 2; r++) {
			out[c][r] = (*this)(r, c).imag();
		}
	}
	return out;
}

cdouble CMat2::determinant() const
{
	return (*this)(0, 0) * (*this)(1, 1) - (*this)(0, 1) * (*this)(1, 0);
}

CMat2 CMat2::inverse() const
{
	const cdouble inv = 1.0 / determinant();
	CMat2 out;
	out(0, 0) = (*this)(1, 1) * inv;
	out(1, 1) = (*this)(0, 0) * inv;
	out(0, 1) = -(*this)(0, 1) * inv;
	out(1, 0) = -(*this)(1, 0) * inv;
	return out;
}

CMat2 CMat2::operator+(const CMat2& other) const
{
	CMat2 out;
	for (int c = 0; c < 2; c++) {
		for (int r = 0; r < 2; r++) {
			out(r, c) = (*this)(r, c) + other(r, c);
		}
	}
	return out;
}

CMat2 CMat2::operator*(const CMat2& other) const
{
	CMat2 out;
	for (int c = 0; c < 2; c++) {
		for (int r = 0; r < 2; r++) {
			out(r, c) = (*this)(r, 0) * other(0, c) + (*this)(r, 1) * other(1, c);
		}
	}
	return out;
}

CMat2 CMat2::operator*(cdouble scale) const
{
	CMat2 out;
	for (int c = 0; c < 2; c++) {
		for (int r = 0; r < 2; r++) {
			out(r, c) = (*this)(r, c) * scale;
		}
	}
	return out;
}

CMat2 operator*(const glm::dmat2& real, const CMat2& complex)
{
	return CMat2::fromReal(real) * complex;
}

//---------------------------------------------------------------- ray-transfer matrices

RayTransfer RayTransfer::operator*(const RayTransfer& first) const
{
	RayTransfer out;
	out.A = A * first.A + B * first.C;
	out.B = A * first.B + B * first.D;
	out.C = C * first.A + D * first.C;
	out.D = C * first.B + D * first.D;
	return out;
}

double RayTransfer::symplecticError() const
{
	// S^T J S = J with J = [0 I; -I 0] is, in blocks: A^T C and B^T D symmetric, and A^T D - C^T B = I
	const glm::dmat2 ac = glm::transpose(A) * C;
	const glm::dmat2 bd = glm::transpose(B) * D;
	const glm::dmat2 id = glm::transpose(A) * D - glm::transpose(C) * B;
	double error = std::max(std::abs(ac[1][0] - ac[0][1]), std::abs(bd[1][0] - bd[0][1]));
	for (int c = 0; c < 2; c++) {
		for (int r = 0; r < 2; r++) {
			error = std::max(error, std::abs(id[c][r] - (r == c ? 1.0 : 0.0)));
		}
	}
	return error;
}

RayTransfer driftTransfer(double distance, double index)
{
	RayTransfer out;
	out.B = glm::dmat2(distance / index);
	return out;
}

glm::dvec3 tangentPoint(const glm::dvec3& normal, const glm::dvec3& d, const glm::dvec3& e1, const glm::dvec3& e2, const glm::dvec2& x)
{
	const glm::dvec3 offset = x.x * e1 + x.y * e2;
	return offset - d * (glm::dot(normal, offset) / glm::dot(normal, d));
}

RayTransfer surfaceTransfer(const SurfaceGeometry& surface, const glm::dvec3& d, const glm::dvec3& e1, const glm::dvec3& e2, double n, const glm::dvec3& dOut,
	const glm::dvec3& f1, const glm::dvec3& f2, double nOut, bool reflect)
{
	// the normal turned along the incoming ray, and the curvatures' sign with it
	const double side = glm::dot(d, surface.normal) >= 0.0 ? 1.0 : -1.0;
	const glm::dvec3 N = side * surface.normal;
	const double cosIn = glm::dot(d, N);
	const double cosOut = glm::dot(dOut, N);
	// the normal's change along the surface, dN = W t: minus the shape operator, k positive bending toward the normal
	const auto shape = [&](const glm::dvec3& t) {
		return -side * (surface.k1 * surface.dir1 * glm::dot(surface.dir1, t) + surface.k2 * surface.dir2 * glm::dot(surface.dir2, t));
	};
	// Snell's law n' d' = n d + gamma N, perturbed: n' dd' = Q' (n dd + gamma dN), Q' the projection along N onto the
	// plane square to d'
	const double gamma = reflect ? -2.0 * n * cosIn : nOut * cosOut - n * cosIn;
	const auto project = [&](const glm::dvec3& v) { return v - N * (glm::dot(dOut, v) / cosOut); };

	const std::array<glm::dvec3, 2> in { e1, e2 };
	const std::array<glm::dvec3, 2> out { f1, f2 };
	RayTransfer s;
	s.B = glm::dmat2(0.0);
	for (int j = 0; j < 2; j++) {
		// the neighbour's offset on the tangent plane, and its reduced direction's change
		const glm::dvec3 t = in[j] - d * (glm::dot(N, in[j]) / cosIn);
		const glm::dvec3 bend = project(gamma * shape(t));
		const glm::dvec3 turn = project(in[j]);
		for (int i = 0; i < 2; i++) {
			s.A[j][i] = glm::dot(out[i], t);
			s.C[j][i] = glm::dot(out[i], bend);
			s.D[j][i] = glm::dot(out[i], turn);
		}
	}
	return s;
}

glm::dvec3 perpendicularTo(const glm::dvec3& d, const glm::dvec3& hint)
{
	glm::dvec3 h = hint - d * glm::dot(d, hint);
	if (glm::dot(h, h) < 1e-12) {
		const glm::dvec3 other = std::abs(d.x) < 0.9 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 1.0, 0.0);
		h = other - d * glm::dot(d, other);
	}
	return glm::normalize(h);
}

void transportFrame(const glm::dvec3& d, const glm::dvec3& dOut, const glm::dvec3& normal, bool reflect, const glm::dvec3& e1, const glm::dvec3& e2,
	glm::dvec3& f1, glm::dvec3& f2)
{
	if (reflect) {
		f1 = e1 - 2.0 * glm::dot(e1, normal) * normal;
		f2 = e2 - 2.0 * glm::dot(e2, normal) * normal;
	} else {
		// Rodrigues' rotation about d x d'
		const glm::dvec3 axis = glm::cross(d, dOut);
		const double sine = glm::length(axis);
		const double cosine = glm::dot(d, dOut);
		if (sine < 1e-15) {
			f1 = e1;
			f2 = e2;
		} else {
			const glm::dvec3 k = axis / sine;
			const auto rotate = [&](const glm::dvec3& v) { return v * cosine + glm::cross(k, v) * sine + k * glm::dot(k, v) * (1.0 - cosine); };
			f1 = rotate(e1);
			f2 = rotate(e2);
		}
	}
	// against rounding over many surfaces: square to d' and to each other
	f1 = glm::normalize(f1 - dOut * glm::dot(dOut, f1));
	f2 = f2 - dOut * glm::dot(dOut, f2);
	f2 = glm::normalize(f2 - f1 * glm::dot(f1, f2));
}

//---------------------------------------------------------------- the beam

CMat2 ComplexRays::q() const
{
	return X * U.inverse();
}

ComplexRays operator*(const RayTransfer& s, const ComplexRays& rays)
{
	ComplexRays out;
	out.X = (s.A * rays.X) + (s.B * rays.U);
	out.U = (s.C * rays.X) + (s.D * rays.U);
	out.m2 = rays.m2;
	return out;
}

ComplexRays simpleAstigmaticBeam(const glm::dvec2& waistPosition, const glm::dvec2& rayleighRange, double index, const glm::dvec2& m2)
{
	ComplexRays rays;
	rays.X = CMat2::diagonal(cdouble(-waistPosition.x, rayleighRange.x) / index, cdouble(-waistPosition.y, rayleighRange.y) / index);
	rays.U = CMat2::identity();
	rays.m2 = m2;
	return rays;
}

double BeamAxisProfile::radius(double s) const
{
	const double x = (s - waistAt) / rayleighRange;
	return waist * std::sqrt(1.0 + x * x);
}

double BeamAxisProfile::gouy(double s) const
{
	return std::atan((s - waistAt) / rayleighRange);
}

namespace {

// a symmetric 2 x 2 matrix's eigenvalues, the first along `angle` from the frame's first axis, chosen within 45 degrees
// of it so the axes keep their names through a beam that is nearly round
void symmetricEigen(const glm::dmat2& m, glm::dvec2& values, double& angle)
{
	const double a = m[0][0];
	const double b = 0.5 * (m[0][1] + m[1][0]);
	const double c = m[1][1];
	const double mean = 0.5 * (a + c);
	const double spread = std::sqrt(0.25 * (a - c) * (a - c) + b * b);
	angle = 0.5 * std::atan2(2.0 * b, a - c);
	values = glm::dvec2(mean + spread, mean - spread);
	if (angle > 0.25 * std::numbers::pi) {
		angle -= 0.5 * std::numbers::pi;
		std::swap(values.x, values.y);
	} else if (angle <= -0.25 * std::numbers::pi) {
		angle += 0.5 * std::numbers::pi;
		std::swap(values.x, values.y);
	}
}

glm::dmat2 rotation(double angle)
{
	const double c = std::cos(angle);
	const double s = std::sin(angle);
	// columns: the first axis, then the second
	return glm::dmat2(c, s, -s, c);
}

CMat2 qAt(const ComplexRays& rays, double s, double index)
{
	return rays.q() + CMat2::identity() * cdouble(s / index, 0.0);
}

} // namespace

BeamCrossSection crossSection(const ComplexRays& rays, double s, double index, double lambda0)
{
	const CMat2 qInverse = qAt(rays, s, index).inverse();
	const glm::dmat2 mu = -qInverse.imag();
	// the embedded beam's w^2 tensor, scaled by the M^2 on each side
	const glm::dmat2 embedded = (lambda0 / std::numbers::pi) * glm::inverse(mu);
	const glm::dmat2 scale(std::sqrt(rays.m2.x), 0.0, 0.0, std::sqrt(rays.m2.y));
	BeamCrossSection out;
	out.w2 = scale * embedded * scale;
	glm::dvec2 values;
	symmetricEigen(out.w2, values, out.angle);
	out.radii = glm::sqrt(glm::max(values, glm::dvec2(0.0)));
	const glm::dmat2 r = rotation(out.angle);
	const glm::dmat2 curvature = qInverse.real();
	for (int i = 0; i < 2; i++) {
		const glm::dvec2 v = r[i];
		const double c = glm::dot(v, curvature * v);
		out.curvatureRadii[i] = c == 0.0 ? INFINITY : index / c;
	}
	return out;
}

BeamProfile beamProfile(const ComplexRays& rays, double index, double lambda0)
{
	const CMat2 q = rays.q();
	glm::dvec2 values;
	BeamProfile out;
	symmetricEigen(-q.inverse().imag(), values, out.angle);
	const glm::dmat2 r = rotation(out.angle);
	const CMat2 local = CMat2::fromReal(glm::transpose(r)) * q * CMat2::fromReal(r);
	const double scale = std::max(std::abs(local(0, 0)), std::abs(local(1, 1)));
	out.twisted = std::abs(local(0, 1)) > 1e-6 * scale;
	const double turned = std::min(std::abs(std::sin(out.angle)), std::abs(std::cos(out.angle)));
	if (rays.m2.x != rays.m2.y && turned > 1e-6) {
		out.twisted = true;
	}
	for (int i = 0; i < 2; i++) {
		const glm::dvec2 axis = r[i];
		const double m2 = axis.x * axis.x * rays.m2.x + axis.y * axis.y * rays.m2.y;
		const cdouble qi = local(i, i);
		BeamAxisProfile& p = out.axes[i];
		p.waistAt = -index * qi.real();
		p.rayleighRange = index * qi.imag();
		p.waist = std::sqrt(std::max(m2 * lambda0 * qi.imag() / std::numbers::pi, 0.0));
	}
	return out;
}

//---------------------------------------------------------------- clipping

namespace {

struct LegendreRule {
	std::vector<double> nodes;
	std::vector<double> weights;
};

// the n-point Gauss-Legendre rule on [-1, 1], by Newton's method on P_n
LegendreRule legendreRule(int n)
{
	LegendreRule rule;
	rule.nodes.resize(n);
	rule.weights.resize(n);
	for (int i = 0; i < n; i++) {
		double x = std::cos(std::numbers::pi * (i + 0.75) / (n + 0.5));
		double derivative = 0.0;
		for (int iteration = 0; iteration < 100; iteration++) {
			double p0 = 1.0;
			double p1 = x;
			for (int k = 2; k <= n; k++) {
				const double p2 = ((2.0 * k - 1.0) * x * p1 - (k - 1.0) * p0) / k;
				p0 = p1;
				p1 = p2;
			}
			derivative = n * (x * p1 - p0) / (x * x - 1.0);
			const double step = p1 / derivative;
			x -= step;
			if (std::abs(step) < 1e-16) {
				break;
			}
		}
		rule.nodes[i] = x;
		rule.weights[i] = 2.0 / ((1.0 - x * x) * derivative * derivative);
	}
	return rule;
}

double normalCdf(double x)
{
	return 0.5 * std::erfc(-x / std::numbers::sqrt2);
}

} // namespace

double fractionInside(const glm::dmat2& w2, const std::function<bool(const glm::dvec2&)>& inside, bool convex)
{
	// whitened: r = L xi with L L^T = w2 / 4, since the intensity is exp(-2 r^T w2^-1 r) = exp(-|xi|^2 / 2)
	const double l11 = std::sqrt(std::max(w2[0][0] / 4.0, 0.0));
	if (!(l11 > 0.0)) {
		return inside(glm::dvec2(0.0)) ? 1.0 : 0.0;
	}
	const double l21 = 0.25 * 0.5 * (w2[0][1] + w2[1][0]) / l11;
	const double l22 = std::sqrt(std::max(w2[1][1] / 4.0 - l21 * l21, 0.0));
	const auto offset = [&](double a, double b) { return glm::dvec2(l11 * a, l21 * a + l22 * b); };

	if (convex) {
		bool all = inside(glm::dvec2(0.0));
		for (int k = 0; all && k < 32; k++) {
			const double angle = 2.0 * std::numbers::pi * k / 32.0;
			all = inside(offset(6.0 * std::cos(angle), 6.0 * std::sin(angle)));
		}
		if (all) {
			return 1.0;
		}
	}

	static const LegendreRule rule = legendreRule(24);
	constexpr double SPAN = 7.0;
	constexpr int GRID = 49;
	// along one line of the whitened plane: the runs inside, each integrated exactly in the other coordinate
	const auto line = [&](double a, int* runs) {
		const auto in = [&](double b) { return inside(offset(a, b)); };
		const auto edge = [&](double lo, double hi, bool loInside) {
			for (int i = 0; i < 40; i++) {
				const double mid = 0.5 * (lo + hi);
				(in(mid) == loInside ? lo : hi) = mid;
			}
			return 0.5 * (lo + hi);
		};
		double sum = 0.0;
		int count = 0;
		double previous = -SPAN;
		bool previousInside = in(previous);
		double runStart = -SPAN;
		for (int j = 1; j < GRID; j++) {
			const double b = -SPAN + 2.0 * SPAN * j / (GRID - 1);
			const bool now = in(b);
			if (now != previousInside) {
				const double crossing = runs != nullptr ? b : edge(previous, b, previousInside);
				if (now) {
					runStart = crossing;
				} else {
					sum += normalCdf(crossing) - normalCdf(runStart);
					count++;
				}
			}
			previous = b;
			previousInside = now;
		}
		if (previousInside) {
			sum += normalCdf(SPAN) - normalCdf(runStart);
			count++;
		}
		if (runs != nullptr) {
			*runs = count;
		}
		return sum;
	};
	// where the number of runs changes across the first coordinate: the line's integral has a square-root edge there, so
	// each piece between them is integrated on its own, with a = m + h sin(t), which smooths those edges
	constexpr int SCAN = 97;
	std::vector<double> breaks { -SPAN };
	int previousRuns = 0;
	line(-SPAN, &previousRuns);
	for (int i = 1; i < SCAN; i++) {
		const double a = -SPAN + 2.0 * SPAN * i / (SCAN - 1);
		int runs = 0;
		line(a, &runs);
		if (runs != previousRuns) {
			double lo = a - 2.0 * SPAN / (SCAN - 1);
			double hi = a;
			for (int k = 0; k < 40; k++) {
				const double mid = 0.5 * (lo + hi);
				int midRuns = 0;
				line(mid, &midRuns);
				(midRuns == previousRuns ? lo : hi) = mid;
			}
			breaks.push_back(0.5 * (lo + hi));
		}
		previousRuns = runs;
	}
	breaks.push_back(SPAN);
	double total = 0.0;
	for (size_t piece = 1; piece < breaks.size(); piece++) {
		const double middle = 0.5 * (breaks[piece] + breaks[piece - 1]);
		const double half = 0.5 * (breaks[piece] - breaks[piece - 1]);
		if (half <= 0.0) {
			continue;
		}
		for (size_t k = 0; k < rule.nodes.size(); k++) {
			const double t = 0.5 * std::numbers::pi * rule.nodes[k];
			const double a = middle + half * std::sin(t);
			const double density = std::exp(-0.5 * a * a) / std::sqrt(2.0 * std::numbers::pi);
			total += rule.weights[k] * 0.5 * std::numbers::pi * half * std::cos(t) * density * line(a, nullptr);
		}
	}
	return std::clamp(total, 0.0, 1.0);
}
