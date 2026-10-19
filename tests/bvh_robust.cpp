#include "bvh_robust.h"

#include <bvh_layout.h>
#include <bvh_scene.h>
#include <robust.h>
#include <shape.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <random>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>

namespace {

struct Random {
	std::mt19937_64 engine;

	explicit Random(uint64_t seed)
		: engine(seed)
	{
	}
	float unit() { return std::uniform_real_distribution<float>(0.f, 1.f)(engine); }
	float range(float low, float high) { return low + (high - low) * unit(); }
	// log-uniform in [low, high]
	float logRange(float low, float high) { return std::exp(range(std::log(low), std::log(high))); }
	uint32_t below(uint32_t n) { return (uint32_t)(engine() % n); }
	glm::vec3 direction()
	{
		for (;;) {
			const glm::vec3 d(range(-1.f, 1.f), range(-1.f, 1.f), range(-1.f, 1.f));
			const float l2 = glm::dot(d, d);
			if (l2 > 1e-4f && l2 <= 1.f) {
				return d / std::sqrt(l2);
			}
		}
	}
};

uint32_t bitsOf(float v)
{
	return std::bit_cast<uint32_t>(v);
}

//---------------------------------------------------------------- float steps

size_t checkFloatSteps()
{
	Random random(1);
	size_t checked = 0;
	size_t bad = 0;
	auto check = [&](float v) {
		checked++;
		const float inf = std::numeric_limits<float>::infinity();
		if (bitsOf(nextFloatUp(v)) != bitsOf(std::nextafter(v, inf)) || bitsOf(nextFloatDown(v)) != bitsOf(std::nextafter(v, -inf))) {
			if (bad < 3) {
				fmt::println("    nextFloat of {:e} (bits {:08x}): up {:08x} vs {:08x}, down {:08x} vs {:08x}", v, bitsOf(v), bitsOf(nextFloatUp(v)), bitsOf(std::nextafter(v, inf)),
					bitsOf(nextFloatDown(v)), bitsOf(std::nextafter(v, -inf)));
			}
			bad++;
		}
	};
	const float specials[] = { 0.f, -0.f, std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(),
		-std::numeric_limits<float>::min(), 1.f, -1.f, std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), 0.5f, 2.f };
	for (const float v : specials) {
		check(v);
	}
	for (int i = 0; i < 1000000; i++) {
		const uint32_t bits = (uint32_t)random.engine();
		if (((bits >> 23) & 0xFFu) == 0xFFu) {
			continue;
		}
		check(std::bit_cast<float>(bits));
	}
	fmt::println("  nextFloatUp/Down vs std::nextafter: {} values, {} wrong", checked, bad);
	return bad;
}

//---------------------------------------------------------------- meshes

// a UV sphere of radius 1: every vertex is one float triple shared by every triangle around it,
// the way a mesh's vertex buffer shares it. Three floats per vertex, three vertices per triangle
std::vector<glm::vec3> uvSphere(int rings, int segments)
{
	std::vector<glm::vec3> grid;
	auto vertex = [&](int r, int s) {
		const double theta = 3.14159265358979323846 * r / rings;
		const double phi = 2.0 * 3.14159265358979323846 * (s % segments) / segments;
		return glm::vec3((float)(std::sin(theta) * std::cos(phi)), (float)std::cos(theta), (float)(std::sin(theta) * std::sin(phi)));
	};
	std::vector<glm::vec3> triangles;
	for (int r = 0; r < rings; r++) {
		for (int s = 0; s < segments; s++) {
			const glm::vec3 a = vertex(r, s);
			const glm::vec3 b = vertex(r, s + 1);
			const glm::vec3 c = vertex(r + 1, s + 1);
			const glm::vec3 d = vertex(r + 1, s);
			if (r != 0) {
				triangles.insert(triangles.end(), { a, b, c });
			}
			if (r != rings - 1) {
				triangles.insert(triangles.end(), { a, c, d });
			}
		}
	}
	return triangles;
}

// the unit cube [-0.5, 0.5]^3, twelve triangles
std::vector<glm::vec3> unitBox()
{
	std::vector<glm::vec3> triangles;
	auto corner = [](int i) { return glm::vec3((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f); };
	const int faces[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
	for (const auto& face : faces) {
		triangles.insert(triangles.end(), { corner(face[0]), corner(face[1]), corner(face[2]), corner(face[0]), corner(face[2]), corner(face[3]) });
	}
	return triangles;
}

//---------------------------------------------------------------- the crack test

size_t checkCracks(size_t rayCount)
{
	const std::vector<glm::vec3> vertices = uvSphere(48, 96);
	const size_t triangleCount = vertices.size() / 3;
	size_t problems = 0;

	fmt::println("  {:<26} {:<12} {:>9} {:>9}", "layout", "test", "rays", "leaks");
	for (const BvhLayout layout : { BvhLayout::Binary, BvhLayout::Cwbvh8 }) {
		BvhBuildOptions options;
		options.builder = BvhBuilder::SweepSah;
		options.maxLeafSize = layout == BvhLayout::Cwbvh8 ? BVH_CWBVH_MAX_LEAF_SIZE : 4;
		const Blas blas = buildBlas(vertices, options, layout);
		if (!blas.usable()) {
			fmt::println("  cannot build the sphere: {}", blas.error);
			return 1;
		}

		for (const bool watertight : { false, true }) {
			Random random(7);
			size_t leaks = 0;
			for (size_t i = 0; i < rayCount; i++) {
				const uint32_t triangle = random.below((uint32_t)triangleCount);
				const uint32_t edge = random.below(3);
				//a tenth of the rays go for a vertex, and a tenth for an edge's midpoint
				const float choice = random.unit();
				const float s = choice < 0.1f ? 0.f : (choice < 0.2f ? 0.5f : random.unit());
				const glm::dvec3 a(vertices[triangle * 3 + edge]);
				const glm::dvec3 b(vertices[triangle * 3 + (edge + 1) % 3]);
				const glm::vec3 target = glm::vec3(a + (double)s * (b - a));
				//from outside the sphere, within thirty degrees of the normal there
				const glm::vec3 normal = glm::normalize(target);
				glm::vec3 tilt = random.direction();
				tilt -= normal * glm::dot(tilt, normal);
				const float distance = random.range(0.25f, 3.f);
				const glm::vec3 origin = target + normal * distance + tilt * distance * 0.5f;
				BvhRay ray;
				ray.origin = origin;
				ray.direction = glm::normalize(target - origin);
				ray.tMin = 0.f;
				ray.watertight = watertight;
				const float expected = glm::length(target - origin);
				const BvhHit hit = traceBvh(blas.bvh, blas.triangles, ray);
				if (!hit.hit || hit.t > expected * 1.001f + 1e-5f) {
					leaks++;
				}
			}
			fmt::println("  {:<26} {:<12} {:>9} {:>9}", bvhLayoutName(layout), watertight ? "watertight" : "Moller-Trumbore", rayCount, leaks);
			//Moller-Trumbore is allowed to leak; the watertight test is not
			if (watertight && leaks > 0) {
				problems++;
			}
		}
	}
	return problems;
}

//---------------------------------------------------------------- the vertex grid

// The renderer's own pinhole test, on the CPU: a flat grid of 64 x 64 quads whose vertices are
// exactly where a camera one unit per pixel at z = 0 aims the rays of its pixel centres, so every
// ray passes through a vertex that six triangles share, in a regular pattern, thousands of times
// over. Moller-Trumbore loses a share of them; the watertight test loses none (a ray through the
// mesh's own outer boundary may pass either side of it, so those are not counted)
size_t checkVertexGrid()
{
	constexpr int N = 64;
	auto vertex = [](int i, int j) { return glm::vec3((float)i - N / 2 + 0.5f, (float)j - N / 2 + 0.5f, 0.f); };
	std::vector<glm::vec3> vertices;
	for (int j = 0; j < N; j++) {
		for (int i = 0; i < N; i++) {
			const glm::vec3 a = vertex(i, j);
			const glm::vec3 b = vertex(i + 1, j);
			const glm::vec3 c = vertex(i + 1, j + 1);
			const glm::vec3 d = vertex(i, j + 1);
			if ((i + j) % 2 == 0) {
				vertices.insert(vertices.end(), { a, b, c, a, c, d });
			} else {
				vertices.insert(vertices.end(), { a, b, d, b, c, d });
			}
		}
	}
	size_t problems = 0;
	fmt::println("  {:<26} {:<16} {:>9} {:>9}", "layout", "test", "rays", "leaks");
	for (const BvhLayout layout : { BvhLayout::Binary, BvhLayout::Cwbvh8 }) {
		BvhBuildOptions options;
		options.builder = BvhBuilder::SweepSah;
		options.maxLeafSize = layout == BvhLayout::Cwbvh8 ? BVH_CWBVH_MAX_LEAF_SIZE : 4;
		const Blas blas = buildBlas(vertices, options, layout);
		for (const bool watertight : { false, true }) {
			size_t rays = 0;
			size_t leaks = 0;
			for (int j = 1; j < N; j++) {
				for (int i = 1; i < N; i++) {
					BvhRay ray;
					ray.origin = glm::vec3(0.f, 0.f, 100.f);
					ray.direction = glm::normalize(vertex(i, j) - ray.origin);
					ray.tMin = 0.f;
					ray.watertight = watertight;
					rays++;
					if (!traceBvh(blas.bvh, blas.triangles, ray).hit) {
						leaks++;
					}
				}
			}
			fmt::println("  {:<26} {:<16} {:>9} {:>9}", bvhLayoutName(layout), watertight ? "watertight" : "Moller-Trumbore", rays, leaks);
			if (watertight && leaks > 0) {
				problems++;
			}
		}
	}
	return problems;
}

//---------------------------------------------------------------- hit points and spawn offsets

struct Regime {
	const char* name;
	// where things are placed: a cube of this half-extent, and the sizes of what is placed in it
	float spread;
	float sizeLow;
	float sizeHigh;
};

struct KindStats {
	size_t hits { 0 };
	// how far the hit point was from the exact surface, as a fraction of its error bound; above one
	// is a bound that failed
	double worstRatio { 0.0 };
	size_t boundViolations { 0 };
	size_t spawns { 0 };
	size_t selfHits { 0 };
	// the same spawns from the unoffset hit point, which shows the test can fail
	size_t unoffsetSelfHits { 0 };
};

constexpr int KIND_COUNT = (int)SHAPE_KIND_COUNT + 2;
const char* kindName(int kind)
{
	static const char* names[] = { "sphere", "plane", "quad", "box", "cylinder", "mesh sphere", "mesh box" };
	return names[kind];
}

// the signed distance, in world units, of the world point `p` from the exact surface of the instance
// (in double precision, from the float matrix's own entries). `part` is the box face or cylinder
// part the hit is on
double exactSurfaceDistance(const SceneInstance& instance, const std::vector<std::array<glm::dvec3, 3>>* meshTriangle, uint32_t part, const glm::dvec3& p)
{
	glm::dmat4 m;
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			m[c][r] = instance.objectToWorld[c][r];
		}
	}
	if (meshTriangle != nullptr) {
		const auto& tri = (*meshTriangle)[0];
		const glm::dvec3 v0 = glm::dvec3(m * glm::dvec4(tri[0], 1.0));
		const glm::dvec3 v1 = glm::dvec3(m * glm::dvec4(tri[1], 1.0));
		const glm::dvec3 v2 = glm::dvec3(m * glm::dvec4(tri[2], 1.0));
		return glm::dot(p - v0, glm::normalize(glm::cross(v1 - v0, v2 - v0)));
	}
	const glm::dmat4 inverse = glm::inverse(m);
	const glm::dvec3 q = glm::dvec3(inverse * glm::dvec4(p, 1.0));
	double f = 0.0;
	glm::dvec3 gradient(0.0, 1.0, 0.0);
	switch (instance.shape) {
	case ShapeKind::Sphere:
		f = glm::length(q) - 1.0;
		gradient = glm::normalize(q);
		break;
	case ShapeKind::Plane:
	case ShapeKind::Quad:
		f = q.y;
		break;
	case ShapeKind::Box: {
		const int axis = (int)(part >> 1);
		const double sign = (part & 1u) != 0 ? 1.0 : -1.0;
		f = sign * q[axis] - 0.5;
		gradient = glm::dvec3(0.0);
		gradient[axis] = sign;
		break;
	}
	case ShapeKind::Cylinder:
		if (part == SHAPE_CYLINDER_SIDE) {
			const double radial = std::sqrt(q.x * q.x + q.z * q.z);
			f = radial - 1.0;
			gradient = glm::dvec3(q.x / radial, 0.0, q.z / radial);
		} else {
			const double sign = part == SHAPE_CYLINDER_TOP ? 1.0 : -1.0;
			f = sign * q.y - 0.5;
			gradient = glm::dvec3(0.0, sign, 0.0);
		}
		break;
	}
	//the gradient of f with respect to the world point is the inverse transpose applied to its gradient
	//with respect to the object point
	const glm::dvec3 worldGradient = glm::transpose(glm::dmat3(inverse)) * gradient;
	return f / glm::length(worldGradient);
}

bool isPlanar(const SceneInstance& instance, uint32_t part)
{
	if (instance.kind == SceneInstanceKind::Mesh) {
		return true;
	}
	switch (instance.shape) {
	case ShapeKind::Sphere:
		return false;
	case ShapeKind::Cylinder:
		return part != SHAPE_CYLINDER_SIDE;
	default:
		return true;
	}
}

float instanceSize(const SceneInstance& instance)
{
	float size = 0.f;
	for (int c = 0; c < 3; c++) {
		size = std::max(size, glm::length(glm::vec3(instance.objectToWorld[c])));
	}
	return size;
}

// returns the number of problems: bounds that failed and offsets that re-hit their surface, under the
// watertight test
size_t checkSpawnOffsets(size_t rayCount, bool watertight, Random& random, const Regime& regime, std::array<KindStats, KIND_COUNT>& stats)
{
	const std::vector<glm::vec3> sphereVertices = uvSphere(16, 32);
	const std::vector<glm::vec3> boxVertices = unitBox();
	BvhBuildOptions options;
	options.builder = BvhBuilder::SweepSah;
	const Blas blasStorage[2] = { buildBlas(sphereVertices, options, BvhLayout::Binary), buildBlas(boxVertices, options, BvhLayout::Binary) };
	const Blas* blases[2] = { &blasStorage[0], &blasStorage[1] };
	const std::vector<glm::vec3>* meshVertices[2] = { &sphereVertices, &boxVertices };

	//the kinds are spread evenly so every one is exercised: one instance of each, then more random ones
	std::vector<SceneInstance> instances;
	std::vector<int> instanceKind;
	for (int i = 0; i < 28; i++) {
		const int kind = i % KIND_COUNT;
		SceneInstance instance;
		if (kind < (int)SHAPE_KIND_COUNT) {
			instance.kind = SceneInstanceKind::Shape;
			instance.shape = (ShapeKind)kind;
		} else {
			instance.kind = SceneInstanceKind::Mesh;
			instance.blas = (uint32_t)(kind - (int)SHAPE_KIND_COUNT);
		}
		const glm::vec3 position(random.range(-regime.spread, regime.spread), random.range(-regime.spread, regime.spread), random.range(-regime.spread, regime.spread));
		const glm::vec3 size(random.logRange(regime.sizeLow, regime.sizeHigh), random.logRange(regime.sizeLow, regime.sizeHigh), random.logRange(regime.sizeLow, regime.sizeHigh));
		instance.objectToWorld = glm::translate(glm::mat4(1.f), position) * glm::rotate(glm::mat4(1.f), random.range(0.f, 6.28f), random.direction()) * glm::scale(glm::mat4(1.f), size);
		//a plane at an axis-aligned, exactly-representable placement is the case with no error along
		//its normal, so some are placed that way
		if (kind == (int)ShapeKind::Plane && i % 2 == 0) {
			instance.objectToWorld = glm::translate(glm::mat4(1.f), glm::vec3(0.f, std::round(position.y * 4.f) / 4.f, 0.f));
		}
		instances.push_back(instance);
		instanceKind.push_back(kind);
	}
	const Tlas tlas = buildTlas(blases, instances);
	if (!tlas.bvh.error.empty()) {
		fmt::println("  TLAS: {}", tlas.bvh.error);
		return 1;
	}

	size_t problems = 0;
	for (size_t i = 0; i < rayCount; i++) {
		const uint32_t target = random.below((uint32_t)instances.size());
		const glm::vec3 center = glm::vec3(instances[target].objectToWorld[3]);
		const float size = instanceSize(instances[target]);
		BvhRay ray;
		ray.origin = glm::vec3(random.range(-1.5f, 1.5f) * regime.spread, random.range(-1.5f, 1.5f) * regime.spread, random.range(-1.5f, 1.5f) * regime.spread);
		ray.direction = glm::normalize(center + random.direction() * size * random.unit() - ray.origin);
		ray.tMin = TRACE_T_MIN_BOUNDED;
		ray.watertight = watertight;
		const SceneHit hit = traceScene(tlas, blases, instances, ray);
		if (!hit.hit) {
			continue;
		}
		const SceneInstance& instance = instances[hit.instance];
		const int kind = instanceKind[hit.instance];
		KindStats& s = stats[kind];
		s.hits++;
		const SceneHitPoint point = sceneHitPoint(tlas, blases, instances, hit, ray);

		//THE ERROR BOUND: how far the point is from the exact surface, against the bound along the normal
		std::vector<std::array<glm::dvec3, 3>> exactTriangle;
		if (instance.kind == SceneInstanceKind::Mesh) {
			const BvhTriangle& tri = blasStorage[instance.blas].triangles[hit.slot];
			exactTriangle.push_back({ glm::dvec3(tri.v0), glm::dvec3(tri.v1), glm::dvec3(tri.v2) });
		}
		const double distance = exactSurfaceDistance(instance, exactTriangle.empty() ? nullptr : &exactTriangle, point.part, glm::dvec3(point.p));
		const double bound = glm::dot(glm::abs(point.n), point.pError);
		const double ratio = bound > 0.0 ? std::abs(distance) / bound : (distance == 0.0 ? 0.0 : std::numeric_limits<double>::infinity());
		s.worstRatio = std::max(s.worstRatio, ratio);
		if (ratio > 1.0) {
			if (s.boundViolations < 2) {
				fmt::println("    bound failed on a {} at {}: distance {:e} vs bound {:e} (pError {:e}, {:e}, {:e})", kindName(kind), regime.name, distance, bound, point.pError.x, point.pError.y, point.pError.z);
			}
			s.boundViolations++;
			problems++;
		}

		//THE SPAWN: leave through either side, and the ray must not find the surface it left
		for (int k = 0; k < 4; k++) {
			const glm::vec3 out = random.direction();
			for (const bool offset : { true, false }) {
				BvhRay next;
				next.origin = offset ? offsetRayOrigin(point.p, point.pError, point.n, out) : point.p;
				next.direction = out;
				next.tMin = TRACE_T_MIN_BOUNDED;
				next.watertight = watertight;
				const SceneHit again = traceScene(tlas, blases, instances, next);
				bool selfHit = false;
				if (again.hit && again.instance == hit.instance) {
					uint32_t partAgain = 0;
					bool same = true;
					if (instance.kind == SceneInstanceKind::Mesh) {
						const auto original = [&](uint32_t slot) { return bitsOf(blasStorage[instance.blas].triangles[slot].v0.w) & BVH_TRIANGLE_INDEX_MASK; };
						same = original(again.slot) == original(hit.slot);
					} else {
						partAgain = sceneHitPoint(tlas, blases, instances, again, next).part;
						same = partAgain == point.part;
					}
					selfHit = same && (isPlanar(instance, point.part) || again.t < 1e-4f * instanceSize(instance));
				}
				if (offset) {
					s.spawns++;
					if (selfHit) {
						if (s.selfHits < 2) {
							fmt::println("    {} at {}: spawn re-hit its own surface at t = {:e}", kindName(kind), regime.name, again.t);
						}
						s.selfHits++;
						problems++;
					}
				} else if (selfHit) {
					s.unoffsetSelfHits++;
				}
			}
		}
	}
	return problems;
}

}

int checkRobustIntersections(size_t rayCount)
{
	fmt::println("Robust intersections (src/robust.h, docs/plans/completed/pbrt-robust-intersections.md)");
	size_t problems = checkFloatSteps();

	fmt::println("\nCrack test: rays aimed at the edges and vertices of a 9k-triangle sphere");
	problems += checkCracks(rayCount);

	fmt::println("\nVertex grid: one ray through every interior vertex of a 64 x 64 quad grid");
	problems += checkVertexGrid();

	const Regime regimes[] = {
		{ "small", 0.05f, 0.005f, 0.04f },
		{ "medium", 100.f, 0.5f, 20.f },
		{ "far from the origin", 10000.f, 100.f, 800.f },
	};
	for (const bool watertight : { true, false }) {
		fmt::println("\nHit-point error bounds and spawn offsets, {} triangles. 'worst' is the largest distance from the exact surface as a fraction of the error bound (above 1 fails)", watertight ? "watertight" : "Moller-Trumbore");
		fmt::println("  {:<20} {:<12} {:>8} {:>9} {:>11} {:>8} {:>9} {:>14}", "regime", "kind", "hits", "worst", "bound fails", "spawns", "self-hits", "without offset");
		Random random(watertight ? 11 : 12);
		for (const Regime& regime : regimes) {
			std::array<KindStats, KIND_COUNT> stats {};
			problems += checkSpawnOffsets(rayCount / 3, watertight, random, regime, stats);
			for (int kind = 0; kind < KIND_COUNT; kind++) {
				const KindStats& s = stats[kind];
				fmt::println("  {:<20} {:<12} {:>8} {:>9.3f} {:>11} {:>8} {:>9} {:>14}", regime.name, kindName(kind), s.hits, s.worstRatio, s.boundViolations, s.spawns, s.selfHits, s.unoffsetSelfHits);
			}
		}
	}

	fmt::println("\nrobust intersections: {}", problems == 0 ? "PASS" : fmt::format("FAIL ({} problem(s))", problems));
	fmt::println("");
	return (int)problems;
}
