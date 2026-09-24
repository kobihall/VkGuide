// CPU-only verification and comparison of the BVH builders and layouts on a real glTF file.
//
// Loads a model's meshes and node placements with fastgltf (no Vulkan, no GPU), builds the
// two-level structure the path tracer uses with every builder x layout, and for each one:
//  - validates every BLAS (buildBlas refuses a malformed tree) and reports its quality: nodes,
//    duplication, depth, SAH cost, build time;
//  - traces the same random rays through traceScene() - the line-for-line mirror of the GPU
//    traversal - and compares every result with brute force over the world-space triangles.
//
// Run from anywhere:  ./bin/bvh_bench assets/structure.glb [--rays N] [--perturb] [--builders mbws] [--layouts bc]
// --perturb additionally rotates and non-uniformly scales every instance, to exercise transforms.

#include <bvh.h>
#include <bvh_layout.h>
#include <bvh_scene.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <random>
#include <thread>

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>

namespace {

struct LoadedMesh {
	// three per triangle, object space
	std::vector<glm::vec3> vertices;
};

struct LoadedScene {
	std::vector<LoadedMesh> meshes;
	std::vector<std::pair<uint32_t, glm::mat4>> nodes;
};

bool loadScene(const std::filesystem::path& path, LoadedScene& scene)
{
	auto data = fastgltf::GltfDataBuffer::FromPath(path);
	if (data.error() != fastgltf::Error::None) {
		fmt::println("cannot read {}", path.string());
		return false;
	}
	fastgltf::Parser parser;
	auto asset = parser.loadGltf(data.get(), path.parent_path(), fastgltf::Options::LoadExternalBuffers | fastgltf::Options::DontRequireValidAssetMember);
	if (asset.error() != fastgltf::Error::None) {
		fmt::println("cannot parse {}: {}", path.string(), fastgltf::getErrorMessage(asset.error()));
		return false;
	}

	for (fastgltf::Mesh& mesh : asset->meshes) {
		LoadedMesh loaded;
		for (fastgltf::Primitive& primitive : mesh.primitives) {
			const auto* position = primitive.findAttribute("POSITION");
			if (position == primitive.attributes.end()) {
				continue;
			}
			std::vector<glm::vec3> positions(asset->accessors[position->accessorIndex].count);
			fastgltf::iterateAccessorWithIndex<glm::vec3>(asset.get(), asset->accessors[position->accessorIndex], [&](glm::vec3 v, size_t i) { positions[i] = v; });
			if (primitive.indicesAccessor.has_value()) {
				fastgltf::iterateAccessor<uint32_t>(asset.get(), asset->accessors[*primitive.indicesAccessor], [&](uint32_t index) { loaded.vertices.push_back(positions[index]); });
			} else {
				loaded.vertices.insert(loaded.vertices.end(), positions.begin(), positions.end());
			}
			loaded.vertices.resize(loaded.vertices.size() / 3 * 3);
		}
		scene.meshes.push_back(std::move(loaded));
	}

	//every node without a parent, as the app's loader places them (LoadedGLTF::topNodes), rather
	//than only the default scene's roots
	std::vector<bool> hasParent(asset->nodes.size(), false);
	for (const fastgltf::Node& node : asset->nodes) {
		for (size_t child : node.children) {
			hasParent[child] = true;
		}
	}
	std::function<void(size_t, const fastgltf::math::fmat4x4&)> visit = [&](size_t index, const fastgltf::math::fmat4x4& parent) {
		const fastgltf::Node& node = asset->nodes[index];
		const fastgltf::math::fmat4x4 matrix = fastgltf::getTransformMatrix(node, parent);
		if (node.meshIndex.has_value()) {
			glm::mat4 transform;
			std::memcpy(&transform, matrix.data(), sizeof(transform));
			scene.nodes.push_back({ (uint32_t)*node.meshIndex, transform });
		}
		for (size_t child : node.children) {
			visit(child, matrix);
		}
	};
	for (size_t i = 0; i < asset->nodes.size(); i++) {
		if (!hasParent[i]) {
			visit(i, fastgltf::math::fmat4x4());
		}
	}
	return true;
}

// runs body(i) for i in [0, count) across the machine's cores
template<typename Body>
void parallelFor(size_t count, Body&& body)
{
	const size_t threads = std::max(std::thread::hardware_concurrency(), 1u);
	std::vector<std::future<void>> tasks;
	for (size_t t = 0; t < threads; t++) {
		tasks.push_back(std::async(std::launch::async, [&, t]() {
			for (size_t i = t; i < count; i += threads) {
				body(i);
			}
		}));
	}
	for (auto& task : tasks) {
		task.get();
	}
}

struct Reference {
	bool hit { false };
	float t { 1e30f };
};

}

int main(int argc, char* argv[])
{
	if (argc < 2) {
		fmt::println("usage: bvh_bench <file.glb> [--rays N] [--perturb] [--builders mbws] [--layouts bc]");
		return 1;
	}
	const std::filesystem::path path = argv[1];
	size_t rayCount = 20000;
	bool perturb = false;
	std::string builderLetters = "mbws";
	std::string layoutLetters = "bc";
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--rays") && i + 1 < argc) {
			rayCount = std::stoul(argv[++i]);
		} else if (!strcmp(argv[i], "--perturb")) {
			perturb = true;
		} else if (!strcmp(argv[i], "--builders") && i + 1 < argc) {
			builderLetters = argv[++i];
		} else if (!strcmp(argv[i], "--layouts") && i + 1 < argc) {
			layoutLetters = argv[++i];
		}
	}

	LoadedScene loaded;
	if (!loadScene(path, loaded)) {
		return 1;
	}

	//the scene: every mesh node as an instance, plus one of every analytic shape so each shape's path
	//- and the infinite plane's, outside the TLAS - is covered
	std::mt19937 rng(1234);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	std::vector<SceneInstance> instances;
	for (const auto& [mesh, transform] : loaded.nodes) {
		SceneInstance instance;
		instance.kind = SceneInstanceKind::Mesh;
		instance.blas = mesh;
		instance.objectToWorld = transform;
		if (perturb) {
			const glm::vec3 axis = glm::normalize(glm::vec3(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f) + 1e-3f);
			const glm::vec3 scale(0.5f + unit(rng), 0.5f + unit(rng), 0.5f + unit(rng));
			instance.objectToWorld = transform * glm::rotate(glm::mat4(1.f), unit(rng) * 6.28f, axis) * glm::scale(glm::mat4(1.f), scale);
		}
		instances.push_back(instance);
	}

	size_t uniqueTriangles = 0;
	for (const LoadedMesh& mesh : loaded.meshes) {
		uniqueTriangles += mesh.vertices.size() / 3;
	}

	//world-space triangles, for brute force
	std::vector<BvhTriangle> world;
	for (const SceneInstance& instance : instances) {
		const LoadedMesh& mesh = loaded.meshes[instance.blas];
		for (size_t i = 0; i < mesh.vertices.size(); i += 3) {
			auto toWorld = [&](const glm::vec3& p) { return glm::vec3(instance.objectToWorld * glm::vec4(p, 1.f)); };
			world.push_back(makeBvhTriangle(toWorld(mesh.vertices[i]), toWorld(mesh.vertices[i + 1]), toWorld(mesh.vertices[i + 2]), 0));
		}
	}
	Aabb sceneBounds;
	for (const BvhTriangle& triangle : world) {
		sceneBounds.grow(glm::vec3(triangle.v0));
		sceneBounds.grow(glm::vec3(triangle.v0 + triangle.e1));
		sceneBounds.grow(glm::vec3(triangle.v0 + triangle.e2));
	}
	const glm::vec3 center = sceneBounds.center();
	const float radius = glm::length(sceneBounds.extent()) * 0.5f;
	auto addShape = [&](ShapeKind shape, const glm::vec3& position, const glm::vec3& size) {
		const glm::vec3 axis = glm::normalize(glm::vec3(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f) + 1e-3f);
		const glm::mat4 rotation = glm::rotate(glm::mat4(1.f), unit(rng) * 6.28f, axis);
		instances.push_back({ SceneInstanceKind::Shape, 0, shape, glm::translate(glm::mat4(1.f), position) * rotation * glm::scale(glm::mat4(1.f), size) });
	};
	addShape(ShapeKind::Sphere, center, glm::vec3(radius * 0.05f));
	addShape(ShapeKind::Sphere, sceneBounds.min, glm::vec3(radius * 0.1f));
	addShape(ShapeKind::Quad, center + glm::vec3(radius * 0.2f, 0.f, 0.f), glm::vec3(radius * 0.3f, 1.f, radius * 0.2f));
	addShape(ShapeKind::Box, center - glm::vec3(radius * 0.2f, 0.f, 0.f), glm::vec3(radius * 0.1f, radius * 0.3f, radius * 0.05f));
	addShape(ShapeKind::Cylinder, center + glm::vec3(0.f, radius * 0.2f, 0.f), glm::vec3(radius * 0.08f, radius * 0.3f, radius * 0.08f));
	//tilted, so it cuts through the scene rather than lying under it
	addShape(ShapeKind::Plane, center - glm::vec3(0.f, radius * 0.3f, 0.f), glm::vec3(1.f));

	fmt::println("{}: {} meshes, {} instances, {} unique triangles, {} placed triangles{}", path.filename().string(), loaded.meshes.size(), loaded.nodes.size(), uniqueTriangles, world.size(), perturb ? " (perturbed)" : "");
	fmt::println("bounds ({:.2f}, {:.2f}, {:.2f}) - ({:.2f}, {:.2f}, {:.2f})", sceneBounds.min.x, sceneBounds.min.y, sceneBounds.min.z, sceneBounds.max.x, sceneBounds.max.y, sceneBounds.max.z);

	//rays: half from inside the scene in random directions (secondary bounces), half from a shell
	//outside it aimed at a random point inside (camera rays)
	std::vector<BvhRay> rays(rayCount);
	for (size_t i = 0; i < rayCount; i++) {
		auto inside = [&]() { return sceneBounds.min + sceneBounds.extent() * glm::vec3(unit(rng), unit(rng), unit(rng)); };
		BvhRay& ray = rays[i];
		ray.tMin = 1e-3f * radius;
		if (i % 2 == 0) {
			ray.origin = inside();
			glm::vec3 d;
			do {
				d = glm::vec3(unit(rng), unit(rng), unit(rng)) * 2.f - 1.f;
			} while (glm::dot(d, d) > 1.f || glm::dot(d, d) < 1e-4f);
			ray.direction = glm::normalize(d);
		} else {
			glm::vec3 d;
			do {
				d = glm::vec3(unit(rng), unit(rng), unit(rng)) * 2.f - 1.f;
			} while (glm::dot(d, d) > 1.f || glm::dot(d, d) < 1e-4f);
			ray.origin = center + glm::normalize(d) * radius * 1.5f;
			ray.direction = glm::normalize(inside() - ray.origin);
		}
	}

	//brute force: every ray against every placed triangle and every shape, no tree at either level
	const auto bruteStart = std::chrono::steady_clock::now();
	std::vector<Reference> reference(rayCount);
	parallelFor(rayCount, [&](size_t r) {
		const BvhRay& ray = rays[r];
		Reference& out = reference[r];
		float tMax = ray.tMax;
		for (const BvhTriangle& triangle : world) {
			float t;
			glm::vec2 barycentrics;
			if (intersectBvhTriangle(triangle, ray.origin, ray.direction, ray.tMin, tMax, t, barycentrics)) {
				tMax = t;
				out.hit = true;
				out.t = t;
			}
		}
		for (const SceneInstance& instance : instances) {
			if (instance.kind != SceneInstanceKind::Shape) {
				continue;
			}
			const glm::mat4 worldToObject = glm::inverse(instance.objectToWorld);
			const glm::vec3 origin = glm::vec3(worldToObject * glm::vec4(ray.origin, 1.f));
			const glm::vec3 direction = glm::mat3(worldToObject) * ray.direction;
			float t;
			if (intersectShape(instance.shape, origin, direction, ray.tMin, tMax, t)) {
				tMax = t;
				out.hit = true;
				out.t = t;
			}
		}
	});
	const double bruteMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bruteStart).count();
	size_t referenceHits = 0;
	for (const Reference& r : reference) {
		referenceHits += r.hit;
	}
	fmt::println("brute force: {} rays, {} hit, {:.0f} ms ({:.2f} Mrays/s)\n", rayCount, referenceHits, bruteMs, rayCount / bruteMs / 1e3);

	fmt::println("{:<24} {:<26} {:>9} {:>8} {:>6} {:>7} {:>8} {:>9} {:>9} {:>8} {:>8} {:>9} {:>9}",
		"builder", "layout", "build ms", "nodes", "dup %", "depth", "BLAS SAH", "scene SAH", "MB", "nodes/r", "tris/r", "Mrays/s", "mismatch");

	const std::pair<char, BvhBuilder> builderChoices[] = { { 'm', BvhBuilder::Midpoint }, { 'b', BvhBuilder::BinnedSah }, { 'w', BvhBuilder::SweepSah }, { 's', BvhBuilder::SpatialSah } };
	const std::pair<char, BvhLayout> layoutChoices[] = { { 'b', BvhLayout::Binary }, { 'c', BvhLayout::Cwbvh8 } };

	int failures = 0;
	for (const auto& [builderLetter, builder] : builderChoices) {
		if (builderLetters.find(builderLetter) == std::string::npos) {
			continue;
		}
		for (const auto& [layoutLetter, layout] : layoutChoices) {
			if (layoutLetters.find(layoutLetter) == std::string::npos) {
				continue;
			}

			BvhBuildOptions options;
			options.builder = builder;
			options.maxLeafSize = layout == BvhLayout::Cwbvh8 ? BVH_CWBVH_MAX_LEAF_SIZE : 4;

			const auto buildStart = std::chrono::steady_clock::now();
			std::vector<Blas> blasStorage(loaded.meshes.size());
			for (size_t m = 0; m < loaded.meshes.size(); m++) {
				blasStorage[m] = buildBlas(loaded.meshes[m].vertices, options, layout);
				if (!blasStorage[m].error.empty()) {
					fmt::println("  mesh {}: {}", m, blasStorage[m].error);
					failures++;
				}
			}
			std::vector<const Blas*> blases;
			for (const Blas& blas : blasStorage) {
				blases.push_back(&blas);
			}
			const Tlas tlas = buildTlas(blases, instances);
			const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
			if (!tlas.bvh.error.empty()) {
				fmt::println("  TLAS: {}", tlas.bvh.error);
				failures++;
			}

			size_t nodes = 0;
			size_t refs = 0;
			size_t bytes = tlas.bvh.nodes.size() * 16;
			uint32_t depth = 0;
			double sahWeighted = 0.0;
			for (const Blas& blas : blasStorage) {
				nodes += blas.bvh.nodeCount;
				refs += blas.triangles.size();
				bytes += blas.bvh.nodes.size() * 16 + blas.triangles.size() * sizeof(BvhTriangle);
				depth = std::max(depth, blas.bvh.depth);
				sahWeighted += (double)blas.stats.sahCost * blas.triangleCount;
			}

			std::vector<SceneHit> hits(rayCount);
			const auto traceStart = std::chrono::steady_clock::now();
			parallelFor(rayCount, [&](size_t r) { hits[r] = traceScene(tlas, blases, instances, rays[r]); });
			const double traceMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - traceStart).count();

			size_t mismatches = 0;
			double nodesVisited = 0.0;
			double trianglesTested = 0.0;
			for (size_t r = 0; r < rayCount; r++) {
				const SceneHit& hit = hits[r];
				nodesVisited += hit.nodesVisited;
				trianglesTested += hit.trianglesTested;
				//object-space and world-space arithmetic round differently, so t agrees to a tolerance;
				//a hit where brute force missed (or the reverse) is only allowed as a graze of an edge,
				//and is counted either way
				const bool agree = hit.hit == reference[r].hit && (!hit.hit || std::abs(hit.t - reference[r].t) <= 1e-3f * std::max(1.f, reference[r].t));
				if (!agree || hit.stackOverflow) {
					if (mismatches < 3) {
						fmt::println("    ray {}: bvh {} t={} vs brute {} t={}{}", r, hit.hit, hit.t, reference[r].hit, reference[r].t, hit.stackOverflow ? " (stack overflow)" : "");
					}
					mismatches++;
				}
			}

			fmt::println("{:<24} {:<26} {:>9.0f} {:>8} {:>6.1f} {:>7} {:>8.1f} {:>9.1f} {:>9.1f} {:>8.1f} {:>8.1f} {:>9.2f} {:>9}",
				bvhBuilderName(builder), bvhLayoutName(layout), buildMs, nodes, 100.0 * ((double)refs / uniqueTriangles - 1.0), depth,
				sahWeighted / uniqueTriangles, tlas.sceneSahCost, bytes / (1024.0 * 1024.0), nodesVisited / rayCount, trianglesTested / rayCount,
				rayCount / traceMs / 1e3, mismatches);
			//a handful of edge grazes in tens of thousands is rounding; more is a bug
			if (mismatches > rayCount / 1000) {
				failures++;
			}
		}
	}

	fmt::println("\n{}", failures == 0 ? "PASS" : fmt::format("FAIL ({} problem(s))", failures));
	return failures == 0 ? 0 : 2;
}
