// Finds the triangles of an optomechanical model that no view from outside can reach, for the converter
// of docs/plans/completed/physoptics-optomechanical-models.md (tools/optical_models/convert.py), and compares a
// cleaned model's silhouettes with the original's.
//
//   mesh_visibility <in.omv> <out.mask>
//       For every triangle, casts rays from points on it (a grid 0.2 mm apart along its longest edge, from 4
//       to 64 points) along 2562 directions in front of it, an icosphere's vertices, and along the 26 face,
//       edge and corner directions of a cube, where points 0.2 mm apart just inside its edges are tried too. A triangle is visible when one of those rays leaves the model
//       without hitting anything, in some configuration: every part at rest, or one part at one of its
//       other poses with the rest at rest (these with 642 directions and the cube's), or, in an "OMV2"
//       file, the configurations it lists. Writes one byte per triangle, 1 for visible.
//   mesh_visibility --compare <original.omv> <cleaned.omv> [--size N] [--depth-tolerance M] [--min P] [--images DIR]
//                   [--farther-only]
//       Ray-casts orthographic depth images of both models at rest from the 26 face, edge and corner
//       directions of a cube. A pixel agrees when both miss, or both hit within the depth tolerance
//       (meters). Prints each direction's agreement as a fraction of the pixels the original covers,
//       and exits 1 if any falls below --min (0.995). --images writes each direction's disagreement as
//       DIR/directionN.pgm: white where the two differ, gray where they agree on a hit. With --farther-only
//       a pixel differs only where the cleaned model misses or hits farther: a hole, not a cover (a spring's
//       cylinder in front of what its coils let through).
//   mesh_visibility --trace <scene.omv> <rays.omr> <hits.omh>
//       Traces each ray of the .omr file against every part at rest, nearest hit first, for the converter's
//       texture bakes.
//
// The .omv file, little-endian, written by the converter:
//   "OMV1", u32 partCount, then per part: u32 poseCount, poseCount x 12 floats (a 3x4 row-major
//   transform; pose 0 is the rest pose), u32 triangleCount, triangleCount x 9 floats (three vertices).
//   "OMV2" is the same followed by u32 configurationCount and, per configuration, partCount u32 pose indices:
//   the parts that move together (an iris's blades and lever) are tried only in the poses they share.
// The .mask file: "OMM1", u32 partCount, then per part u32 triangleCount and that many bytes.
// The .omr file: "OMR1", u32 rayCount, then per ray 7 floats: origin, direction, tMax.
// The .omh file: "OMH1", u32 rayCount, then per ray float t (negative for a miss), u32 part, u32 triangle, and
//   float b1, b2, the hit's barycentric weights of the triangle's second and third vertices.

#include <bvh_layout.h>
#include <bvh_scene.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

namespace {

// the spacing of the points tried on a triangle, in meters: a few per millimeter
constexpr float SAMPLE_SPACING = 2e-4f;
// how far inside its edge an edge point sits, in meters
constexpr float EDGE_INSET = 2e-6f;
// the cube's directions come first in each direction list
constexpr size_t CUBE_DIRECTIONS = 26;

struct Part {
	std::vector<glm::mat4> poses;
	std::vector<glm::vec3> vertices; // three per triangle, object space
};

bool readModel(const char* path, std::vector<Part>& parts, std::vector<std::vector<uint32_t>>* configurations = nullptr)
{
	FILE* f = std::fopen(path, "rb");
	if (!f) {
		std::fprintf(stderr, "cannot open %s\n", path);
		return false;
	}
	auto readU32 = [&](uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; };
	char magic[4];
	uint32_t partCount = 0;
	bool ok = std::fread(magic, 1, 4, f) == 4 && (std::memcmp(magic, "OMV1", 4) == 0 || std::memcmp(magic, "OMV2", 4) == 0) &&
		readU32(partCount);
	const bool listsConfigurations = ok && magic[3] == '2';
	for (uint32_t p = 0; ok && p < partCount; p++) {
		Part part;
		uint32_t poseCount = 0, triangleCount = 0;
		ok = readU32(poseCount);
		for (uint32_t i = 0; ok && i < poseCount; i++) {
			float m[12];
			ok = std::fread(m, 4, 12, f) == 12;
			glm::mat4 t(1.f);
			for (int r = 0; r < 3; r++) {
				for (int c = 0; c < 4; c++) {
					t[c][r] = m[r * 4 + c];
				}
			}
			part.poses.push_back(t);
		}
		ok = ok && readU32(triangleCount);
		if (ok) {
			std::vector<float> v(size_t(triangleCount) * 9);
			ok = std::fread(v.data(), 4, v.size(), f) == v.size();
			for (size_t i = 0; ok && i < v.size(); i += 3) {
				part.vertices.emplace_back(v[i], v[i + 1], v[i + 2]);
			}
		}
		if (part.poses.empty()) {
			part.poses.push_back(glm::mat4(1.f));
		}
		parts.push_back(std::move(part));
	}
	uint32_t configurationCount = 0;
	if (ok && listsConfigurations && readU32(configurationCount)) {
		for (uint32_t c = 0; ok && c < configurationCount; c++) {
			std::vector<uint32_t> pose(partCount);
			ok = std::fread(pose.data(), 4, partCount, f) == partCount;
			for (uint32_t p = 0; ok && p < partCount; p++) {
				ok = pose[p] < parts[p].poses.size();
			}
			if (ok && configurations) {
				configurations->push_back(std::move(pose));
			}
		}
	}
	std::fclose(f);
	if (!ok) {
		std::fprintf(stderr, "%s: not a valid .omv file\n", path);
	}
	return ok;
}

// The vertices of an icosahedron subdivided `levels` times, on the unit sphere: 10 * 4^levels + 2 of
// them, so 2562 at four levels. Built by edge midpoints with a map, so the order is fixed.
std::vector<glm::vec3> icosphere(int levels)
{
	const float t = (1.f + std::sqrt(5.f)) * 0.5f;
	std::vector<glm::vec3> v = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
		{ 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
	std::vector<glm::uvec3> faces = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 },
		{ 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
		{ 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
	for (auto& p : v) {
		p = glm::normalize(p);
	}
	for (int l = 0; l < levels; l++) {
		std::map<std::pair<uint32_t, uint32_t>, uint32_t> mid;
		auto midpoint = [&](uint32_t a, uint32_t b) {
			const auto key = std::minmax(a, b);
			auto it = mid.find(key);
			if (it != mid.end()) {
				return it->second;
			}
			v.push_back(glm::normalize(v[a] + v[b]));
			mid[key] = uint32_t(v.size() - 1);
			return uint32_t(v.size() - 1);
		};
		std::vector<glm::uvec3> next;
		for (const auto& f : faces) {
			const uint32_t a = midpoint(f.x, f.y), b = midpoint(f.y, f.z), c = midpoint(f.z, f.x);
			next.push_back({ f.x, a, c });
			next.push_back({ f.y, b, a });
			next.push_back({ f.z, c, b });
			next.push_back({ a, b, c });
		}
		faces = std::move(next);
	}
	return v;
}

// Every triangle of every part in one configuration, world space, with the (part, triangle) it came from.
struct Scene {
	Blas blas;
	std::vector<glm::vec3> vertices;
	float extent { 0.f };
};

Scene buildScene(const std::vector<Part>& parts, const std::vector<uint32_t>& pose)
{
	Scene scene;
	Aabb box;
	for (size_t p = 0; p < parts.size(); p++) {
		const glm::mat4& m = parts[p].poses[pose[p]];
		for (const glm::vec3& v : parts[p].vertices) {
			scene.vertices.push_back(glm::vec3(m * glm::vec4(v, 1.f)));
			box.grow(scene.vertices.back());
		}
	}
	BvhBuildOptions options;
	options.builder = BvhBuilder::BinnedSah;
	scene.blas = buildBlas(scene.vertices, options, BvhLayout::Binary);
	scene.extent = box.empty() ? 1.f : glm::length(box.extent());
	return scene;
}

bool escapes(const Scene& scene, const glm::vec3& origin, const glm::vec3& direction)
{
	BvhRay ray;
	ray.origin = origin;
	ray.direction = direction;
	ray.tMin = 0.f;
	ray.tMax = scene.extent * 4.f;
	ray.anyHit = true;
	return !traceBvh(scene.blas.bvh, scene.blas.triangles, ray).hit;
}

template <typename F>
void parallelFor(size_t count, F&& body)
{
	const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
	std::atomic<size_t> next { 0 };
	std::vector<std::thread> pool;
	for (unsigned t = 0; t < threads; t++) {
		pool.emplace_back([&] {
			for (size_t i; (i = next.fetch_add(64)) < count;) {
				for (size_t k = i; k < std::min(count, i + 64); k++) {
					body(k);
				}
			}
		});
	}
	for (auto& t : pool) {
		t.join();
	}
}

int visibility(const char* inPath, const char* outPath)
{
	std::vector<Part> parts;
	std::vector<std::vector<uint32_t>> listed;
	if (!readModel(inPath, parts, &listed)) {
		return 2;
	}
	// Rest gets 2562 directions; a moving part's other poses get 642, which is enough for the broad regions
	// a motion uncovers (an adjuster's shank as it backs out)
	std::vector<glm::vec3> fine = icosphere(4);
	std::vector<glm::vec3> coarse = icosphere(3);
	// and the 26 face, edge and corner directions of a cube, first: these models are built along their
	// axes, and the bottom of a narrow slit between two parts (a rotor in its housing) shows only straight down it
	std::vector<glm::vec3> cube;
	for (int x = -1; x <= 1; x++) {
		for (int y = -1; y <= 1; y++) {
			for (int z = -1; z <= 1; z++) {
				if (x != 0 || y != 0 || z != 0) {
					cube.push_back(glm::normalize(glm::vec3(x, y, z)));
				}
			}
		}
	}
	fine.insert(fine.begin(), cube.begin(), cube.end());
	coarse.insert(coarse.begin(), cube.begin(), cube.end());

	// The configurations: all at rest, then each part at each of its other poses, unless the file lists them.
	// The first is the one that gets the fine directions, so a listed set starts with the rest pose.
	std::vector<std::vector<uint32_t>> configurations(1, std::vector<uint32_t>(parts.size(), 0));
	if (!listed.empty()) {
		configurations = listed;
	} else {
		for (size_t p = 0; p < parts.size(); p++) {
			for (uint32_t i = 1; i < parts[p].poses.size(); i++) {
				configurations.push_back(std::vector<uint32_t>(parts.size(), 0));
				configurations.back()[p] = i;
			}
		}
	}

	std::vector<std::vector<uint8_t>> visible(parts.size());
	std::vector<std::pair<uint32_t, uint32_t>> owner; // world triangle -> (part, triangle)
	for (size_t p = 0; p < parts.size(); p++) {
		visible[p].assign(parts[p].vertices.size() / 3, 0);
		for (uint32_t t = 0; t < parts[p].vertices.size() / 3; t++) {
			owner.push_back({ uint32_t(p), t });
		}
	}

	for (size_t c = 0; c < configurations.size(); c++) {
		const std::vector<glm::vec3>& directions = c == 0 ? fine : coarse;
		const Scene scene = buildScene(parts, configurations[c]);
		if (!scene.blas.error.empty()) {
			std::fprintf(stderr, "BVH: %s\n", scene.blas.error.c_str());
			return 2;
		}
		const float eps = scene.extent * 2e-6f;
		parallelFor(owner.size(), [&](size_t w) {
			const auto [p, t] = owner[w];
			if (visible[p][t]) {
				return;
			}
			const glm::vec3 a = scene.vertices[w * 3], b = scene.vertices[w * 3 + 1], c = scene.vertices[w * 3 + 2];
			const glm::vec3 g = glm::cross(b - a, c - a);
			const float area = glm::length(g);
			if (!(area > 0.f)) {
				return; // degenerate: invisible, and harmless to drop
			}
			const glm::vec3 n = g / area;
			// Points on a barycentric grid of n x n small triangles' centers, n by the longest edge: a long triangle
			// may show only a sliver of itself, the bottom of a narrow slit between two parts
			const float longest = std::max({ glm::length(b - a), glm::length(c - b), glm::length(a - c) });
			const int grid = std::clamp(int(std::ceil(longest / SAMPLE_SPACING)), 2, 8);
			std::vector<glm::vec3> samples;
			samples.reserve(size_t(grid) * grid);
			for (int i = 0; i < grid; i++) {
				for (int j = 0; i + j < grid; j++) {
					for (int flip = 0; flip < 2 && (flip == 0 || i + j + 1 < grid); flip++) {
						const float u = (i + (flip ? 2.f : 1.f) / 3.f) / grid, v = (j + (flip ? 2.f : 1.f) / 3.f) / grid;
						samples.push_back(a + (b - a) * u + (c - a) * v);
					}
				}
			}
			// Along the axes, also points just inside each edge: the floor of a slit narrower than the grid shows
			// as a sliver along the edge where it meets the slit's wall
			std::vector<glm::vec3> edgeSamples;
			const glm::vec3 centroid = (a + b + c) / 3.f;
			for (const auto& [from, to] : { std::pair { a, b }, std::pair { b, c }, std::pair { c, a } }) {
				const int count = std::clamp(int(std::ceil(glm::length(to - from) / SAMPLE_SPACING)), 1, 32);
				for (int k = 0; k <= count; k++) {
					const glm::vec3 onEdge = from + (to - from) * (float(k) / count);
					const glm::vec3 inward = centroid - onEdge;
					edgeSamples.push_back(onEdge + inward * std::min(1.f, EDGE_INSET / std::max(glm::length(inward), 1e-12f)));
				}
			}
			for (size_t di = 0; di < directions.size(); di++) {
				const glm::vec3& d = directions[di];
				const float facing = glm::dot(d, n);
				if (facing <= 0.02f) {
					continue;
				}
				for (const glm::vec3& s : samples) {
					if (escapes(scene, s + n * eps + d * eps, d)) {
						visible[p][t] = 1;
						return;
					}
				}
				if (di < CUBE_DIRECTIONS) {
					for (const glm::vec3& s : edgeSamples) {
						if (escapes(scene, s + n * eps + d * eps, d)) {
							visible[p][t] = 1;
							return;
						}
					}
				}
			}
		});
	}

	FILE* f = std::fopen(outPath, "wb");
	if (!f) {
		std::fprintf(stderr, "cannot write %s\n", outPath);
		return 2;
	}
	std::fwrite("OMM1", 1, 4, f);
	const uint32_t partCount = uint32_t(parts.size());
	std::fwrite(&partCount, 4, 1, f);
	size_t kept = 0, total = 0;
	for (const auto& v : visible) {
		const uint32_t n = uint32_t(v.size());
		std::fwrite(&n, 4, 1, f);
		std::fwrite(v.data(), 1, v.size(), f);
		for (uint8_t b : v) {
			kept += b;
		}
		total += v.size();
	}
	std::fclose(f);
	std::printf("visible %zu of %zu triangles in %zu configurations\n", kept, total, configurations.size());
	return 0;
}

int trace(const char* scenePath, const char* raysPath, const char* hitsPath)
{
	std::vector<Part> parts;
	if (!readModel(scenePath, parts)) {
		return 2;
	}
	const Scene scene = buildScene(parts, std::vector<uint32_t>(parts.size(), 0));
	if (!scene.blas.error.empty()) {
		std::fprintf(stderr, "BVH: %s\n", scene.blas.error.c_str());
		return 2;
	}
	std::vector<uint32_t> partStart;
	for (uint32_t p = 0, n = 0; p < parts.size(); p++) {
		partStart.push_back(n);
		n += uint32_t(parts[p].vertices.size() / 3);
	}
	FILE* f = std::fopen(raysPath, "rb");
	char magic[4];
	uint32_t count = 0;
	std::vector<float> rays;
	bool ok = f && std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "OMR1", 4) == 0 && std::fread(&count, 4, 1, f) == 1;
	if (ok) {
		rays.resize(size_t(count) * 7);
		ok = std::fread(rays.data(), 4, rays.size(), f) == rays.size();
	}
	if (f) {
		std::fclose(f);
	}
	if (!ok) {
		std::fprintf(stderr, "%s: not a valid .omr file\n", raysPath);
		return 2;
	}
	struct Hit {
		float t;
		uint32_t part;
		uint32_t triangle;
		float b1;
		float b2;
	};
	static_assert(sizeof(Hit) == 20);
	std::vector<Hit> hits(count);
	parallelFor(count, [&](size_t i) {
		const float* r = &rays[i * 7];
		BvhRay ray;
		ray.origin = glm::vec3(r[0], r[1], r[2]);
		ray.direction = glm::vec3(r[3], r[4], r[5]);
		ray.tMin = 0.f;
		ray.tMax = r[6];
		const BvhHit hit = traceBvh(scene.blas.bvh, scene.blas.triangles, ray);
		Hit out { -1.f, 0, 0, 0.f, 0.f };
		if (hit.hit) {
			const uint32_t triangle = glm::floatBitsToUint(scene.blas.triangles[hit.slot].v0.w) & BVH_TRIANGLE_INDEX_MASK;
			const uint32_t part = uint32_t(std::upper_bound(partStart.begin(), partStart.end(), triangle) - partStart.begin() - 1);
			// the barycentrics against the vertices in the order the .omv file gave them
			const glm::vec3 a = scene.vertices[size_t(triangle) * 3], b = scene.vertices[size_t(triangle) * 3 + 1],
							c = scene.vertices[size_t(triangle) * 3 + 2];
			const glm::vec3 p = ray.origin + ray.direction * hit.t;
			const glm::vec3 e1 = b - a, e2 = c - a, d = p - a;
			const float d11 = glm::dot(e1, e1), d12 = glm::dot(e1, e2), d22 = glm::dot(e2, e2);
			const float denominator = d11 * d22 - d12 * d12;
			const float d1 = glm::dot(d, e1), d2 = glm::dot(d, e2);
			out = { hit.t, part, triangle - partStart[part], denominator != 0.f ? (d22 * d1 - d12 * d2) / denominator : 0.f,
				denominator != 0.f ? (d11 * d2 - d12 * d1) / denominator : 0.f };
		}
		hits[i] = out;
	});
	FILE* o = std::fopen(hitsPath, "wb");
	if (!o) {
		std::fprintf(stderr, "cannot write %s\n", hitsPath);
		return 2;
	}
	std::fwrite("OMH1", 1, 4, o);
	std::fwrite(&count, 4, 1, o);
	std::fwrite(hits.data(), sizeof(Hit), hits.size(), o);
	std::fclose(o);
	return 0;
}

int compare(const char* originalPath, const char* cleanedPath, int size, float depthTolerance, float minimum, const char* imageDir,
	bool fartherOnly)
{
	std::vector<Part> original, cleaned;
	if (!readModel(originalPath, original) || !readModel(cleanedPath, cleaned)) {
		return 2;
	}
	const Scene a = buildScene(original, std::vector<uint32_t>(original.size(), 0));
	// the part each of the original's triangles belongs to, to say where the disagreement is
	std::vector<uint32_t> partStart;
	for (uint32_t p = 0, n = 0; p < original.size(); p++) {
		partStart.push_back(n);
		n += uint32_t(original[p].vertices.size() / 3);
	}
	auto partOf = [&](const BvhHit& hit) {
		const uint32_t triangle = glm::floatBitsToUint(a.blas.triangles[hit.slot].v0.w) & BVH_TRIANGLE_INDEX_MASK;
		return uint32_t(std::upper_bound(partStart.begin(), partStart.end(), triangle) - partStart.begin() - 1);
	};
	const Scene b = buildScene(cleaned, std::vector<uint32_t>(cleaned.size(), 0));
	Aabb box;
	for (const glm::vec3& v : a.vertices) {
		box.grow(v);
	}
	const glm::vec3 center = box.center();
	const float radius = glm::length(box.extent()) * 0.5f;

	float worst = 1.f;
	int worstDirection = -1;
	int index = 0;
	for (int x = -1; x <= 1; x++) {
		for (int y = -1; y <= 1; y++) {
			for (int z = -1; z <= 1; z++) {
				if (x == 0 && y == 0 && z == 0) {
					continue;
				}
				const glm::vec3 d = glm::normalize(glm::vec3(x, y, z));
				const glm::vec3 up = std::abs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
				const glm::vec3 right = glm::normalize(glm::cross(up, d));
				const glm::vec3 upv = glm::cross(d, right);
				std::atomic<size_t> covered { 0 }, differ { 0 }, nearer { 0 };
				std::vector<std::atomic<size_t>> byPart(original.size());
				// gray where only the original is covered, white where they disagree, dark gray where both agree
				std::vector<uint8_t> image(imageDir ? size_t(size) * size : 0, 0);
				parallelFor(size_t(size) * size, [&](size_t k) {
					const float u = ((float(k % size) + 0.5f) / size * 2.f - 1.f) * radius;
					const float v = ((float(k / size) + 0.5f) / size * 2.f - 1.f) * radius;
					BvhRay ray;
					ray.origin = center + right * u + upv * v + d * (radius * 2.f);
					ray.direction = -d;
					ray.tMax = radius * 4.f;
					const BvhHit ha = traceBvh(a.blas.bvh, a.blas.triangles, ray);
					const BvhHit hb = traceBvh(b.blas.bvh, b.blas.triangles, ray);
					if (ha.hit) {
						covered++;
					}
					const bool differs = fartherOnly ? ha.hit && (!hb.hit || hb.t - ha.t > depthTolerance)
													 : ha.hit != hb.hit || (ha.hit && std::abs(ha.t - hb.t) > depthTolerance);
					if (differs) {
						differ++;
						if (ha.hit) {
							byPart[partOf(ha)]++;
						}
						// the cleaned model hits farther (something removed) or nearer (impossible unless it gained geometry)
						if (hb.hit && ha.hit && hb.t < ha.t) {
							nearer++;
						}
						if (std::getenv("MESH_VISIBILITY_DEBUG") && ha.hit && (k % 97) == 0) {
							const BvhTriangle& t = a.blas.triangles[ha.slot];
							const glm::vec3 n = glm::normalize(glm::cross(glm::vec3(t.v1) - glm::vec3(t.v0), glm::vec3(t.v2) - glm::vec3(t.v0)));
							const glm::vec3 p = ray.origin + ray.direction * ha.t;
							std::printf("  pixel %zu: original t %.6f at (%.5f, %.5f, %.5f) normal (%.3f, %.3f, %.3f); cleaned %s %.6f\n", k, ha.t,
								p.x, p.y, p.z, n.x, n.y, n.z, hb.hit ? "t" : "miss", hb.hit ? hb.t : 0.f);
						}
					}
					if (imageDir) {
						image[(size_t(size) - 1 - k / size) * size + k % size] = differs ? 255 : (ha.hit ? 70 : 0);
					}
				});
				if (imageDir) {
					const std::string path = std::string(imageDir) + "/direction" + std::to_string(index) + ".pgm";
					if (FILE* f = std::fopen(path.c_str(), "wb")) {
						std::fprintf(f, "P5\n%d %d\n255\n", size, size);
						std::fwrite(image.data(), 1, image.size(), f);
						std::fclose(f);
					}
				}
				const float agreement = covered ? 1.f - float(differ) / float(covered) : 1.f;
				std::string parts;
				for (size_t p = 0; p < byPart.size(); p++) {
					if (byPart[p]) {
						parts += " part" + std::to_string(p) + ":" + std::to_string(size_t(byPart[p]));
					}
				}
				std::printf("direction %2d (%2d,%2d,%2d): %zu covered, %zu differ (%zu nearer), agreement %.4f%s\n", index, x, y,
					z, size_t(covered), size_t(differ), size_t(nearer), agreement, parts.c_str());
				if (agreement < worst) {
					worst = agreement;
					worstDirection = index;
				}
				index++;
			}
		}
	}
	std::printf("worst agreement %.4f (direction %d), minimum %.4f: %s\n", worst, worstDirection, minimum,
		worst >= minimum ? "ok" : "FAIL");
	return worst >= minimum ? 0 : 1;
}

}

int main(int argc, char** argv)
{
	if (argc >= 4 && std::string(argv[1]) == "--compare") {
		int size = 512;
		float depthTolerance = 1e-4f, minimum = 0.995f;
		const char* imageDir = nullptr;
		bool fartherOnly = false;
		for (int i = 4; i < argc; i++) {
			const std::string a = argv[i];
			if (a == "--farther-only") {
				fartherOnly = true;
				continue;
			}
			if (i + 1 >= argc) {
				break;
			}
			if (a == "--size") {
				size = std::atoi(argv[i + 1]);
			} else if (a == "--depth-tolerance") {
				depthTolerance = float(std::atof(argv[i + 1]));
			} else if (a == "--min") {
				minimum = float(std::atof(argv[i + 1]));
			} else if (a == "--images") {
				imageDir = argv[i + 1];
			}
			i++;
		}
		return compare(argv[2], argv[3], size, depthTolerance, minimum, imageDir, fartherOnly);
	}
	if (argc == 5 && std::string(argv[1]) == "--trace") {
		return trace(argv[2], argv[3], argv[4]);
	}
	if (argc == 3) {
		return visibility(argv[1], argv[2]);
	}
	std::fprintf(stderr, "usage: mesh_visibility <in.omv> <out.mask>\n"
						 "       mesh_visibility --compare <original.omv> <cleaned.omv> [--size N] [--depth-tolerance M] [--min P] [--images DIR] [--farther-only]\n"
						 "       mesh_visibility --trace <scene.omv> <rays.omr> <hits.omh>\n");
	return 2;
}
