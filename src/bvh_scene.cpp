#include <bvh_scene.h>

#include <algorithm>
#include <chrono>

namespace {

double millisecondsSince(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// at most this many boxes bound an instance: enough to follow a rotated mesh's shape closely,
// few enough that transforming them (8 corners each) on every object edit costs nothing
constexpr size_t FRONTIER_SIZE = 16;

// a cut through the tree taken by repeatedly opening the largest interior node
std::vector<Aabb> frontierOf(const Bvh& bvh)
{
	std::vector<uint32_t> cut { 0u };
	while (cut.size() < FRONTIER_SIZE) {
		int largest = -1;
		float largestArea = -1.f;
		for (size_t i = 0; i < cut.size(); i++) {
			const BvhNode& node = bvh.nodes[cut[i]];
			if (!node.isLeaf() && node.bounds.halfArea() > largestArea) {
				largest = (int)i;
				largestArea = node.bounds.halfArea();
			}
		}
		if (largest < 0) {
			break;
		}
		const BvhNode& opened = bvh.nodes[cut[largest]];
		cut[largest] = opened.left;
		cut.push_back(opened.right);
	}

	std::vector<Aabb> boxes;
	for (uint32_t index : cut) {
		boxes.push_back(bvh.nodes[index].bounds);
	}
	return boxes;
}

Aabb transformBox(const Aabb& box, const glm::mat4& transform)
{
	Aabb out;
	for (int corner = 0; corner < 8; corner++) {
		const glm::vec3 p((corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y, (corner & 4) ? box.max.z : box.min.z);
		out.grow(glm::vec3(transform * glm::vec4(p, 1.f)));
	}
	return out;
}

}

Blas buildBlas(std::span<const glm::vec3> triangleVertices, const BvhBuildOptions& options, BvhLayout layout)
{
	Blas blas;
	blas.triangleCount = (uint32_t)(triangleVertices.size() / 3);

	std::vector<Aabb> boxes(blas.triangleCount);
	for (uint32_t i = 0; i < blas.triangleCount; i++) {
		boxes[i].grow(triangleVertices[i * 3]);
		boxes[i].grow(triangleVertices[i * 3 + 1]);
		boxes[i].grow(triangleVertices[i * 3 + 2]);
	}

	const BvhBuildInput input { boxes, triangleVertices };

	const auto buildStart = std::chrono::steady_clock::now();
	Bvh bvh = buildBvh(input, options);
	blas.buildMs = millisecondsSince(buildStart);

	if (bvh.empty()) {
		return blas;
	}
	blas.bounds = bvh.nodes[0].bounds;
	blas.frontier = frontierOf(bvh);
	blas.stats = computeBvhStats(bvh, options.traversalCost, options.intersectionCost);
	//a malformed tree does not render slowly on the GPU, it hangs it: nothing unvalidated is traced
	blas.error = validateBvh(bvh, input);
	if (!blas.error.empty()) {
		return blas;
	}

	const auto packStart = std::chrono::steady_clock::now();
	blas.bvh = packBvh(std::move(bvh), input, layout);
	blas.triangles.reserve(blas.bvh.primOrder.size());
	for (const uint32_t triangle : blas.bvh.primOrder) {
		blas.triangles.push_back(makeBvhTriangle(triangleVertices[triangle * 3], triangleVertices[triangle * 3 + 1], triangleVertices[triangle * 3 + 2], triangle));
	}
	blas.packMs = millisecondsSince(packStart);
	blas.error = blas.bvh.error;
	return blas;
}

Tlas buildTlas(std::span<const Blas* const> blases, std::span<const SceneInstance> instances)
{
	const auto start = std::chrono::steady_clock::now();

	Tlas tlas;
	tlas.worldToObject.resize(instances.size(), glm::mat4(1.f));
	tlas.traced.resize(instances.size(), 0);

	//an untraced instance gets an empty box, which the builder leaves out of the tree entirely
	std::vector<Aabb> boxes(instances.size());
	for (size_t i = 0; i < instances.size(); i++) {
		const SceneInstance& instance = instances[i];
		if (instance.kind == SceneInstanceKind::Sphere) {
			if (instance.sphere.w > 0.f) {
				boxes[i].grow(glm::vec3(instance.sphere) - instance.sphere.w);
				boxes[i].grow(glm::vec3(instance.sphere) + instance.sphere.w);
				tlas.traced[i] = 1;
			}
			continue;
		}

		const Blas* blas = instance.blas < blases.size() ? blases[instance.blas] : nullptr;
		//a zero scale collapses the mesh to nothing and has no inverse to carry a ray with
		const float determinant = glm::determinant(glm::mat3(instance.objectToWorld));
		if (blas == nullptr || !blas->usable() || !(std::abs(determinant) > 1e-12f)) {
			continue;
		}
		tlas.worldToObject[i] = glm::inverse(instance.objectToWorld);
		for (const Aabb& box : blas->frontier) {
			boxes[i].grow(transformBox(box, instance.objectToWorld));
		}
		tlas.traced[i] = 1;
	}

	//few, expensive primitives: every instance costs a transform and a whole BLAS walk, so the
	//best (exhaustive) SAH search and one instance per leaf
	BvhBuildOptions options;
	options.builder = BvhBuilder::SweepSah;
	options.maxLeafSize = 1;
	options.parallel = false;
	const BvhBuildInput input { boxes, {} };
	Bvh bvh = buildBvh(input, options);

	if (!bvh.empty()) {
		tlas.bounds = bvh.nodes[0].bounds;
		tlas.stats = computeBvhStats(bvh);

		//the scene's expected cost per ray: the SAH over the TLAS with each leaf's cost being its
		//instance's own expected cost - one for a sphere, one to enter a BLAS plus that BLAS's SAH
		//cost (itself already conditioned on the ray reaching its root box)
		double interiorArea = 0.0;
		double leafCost = 0.0;
		for (const BvhNode& node : bvh.nodes) {
			if (!node.isLeaf()) {
				interiorArea += node.bounds.halfArea();
				continue;
			}
			for (uint32_t i = node.first; i < node.first + node.count; i++) {
				const SceneInstance& instance = instances[bvh.primRefs[i]];
				const double cost = instance.kind == SceneInstanceKind::Sphere ? 1.0 : 1.0 + blases[instance.blas]->stats.sahCost;
				leafCost += node.bounds.halfArea() * cost;
			}
		}
		const double rootArea = tlas.bounds.halfArea();
		tlas.sceneSahCost = rootArea > 0.0 ? (float)((interiorArea + leafCost) / rootArea) : (float)(bvh.nodes.size() + leafCost);
	}

	tlas.bvh = packBvh(std::move(bvh), input, BvhLayout::Binary);
	tlas.buildMs = millisecondsSince(start);
	return tlas;
}

bool intersectSphere(const glm::vec4& sphere, const glm::vec3& origin, const glm::vec3& direction, float tMin, float tMax, float& t)
{
	const glm::vec3 f = origin - glm::vec3(sphere);
	const float radius = sphere.w;
	const float bPrime = -glm::dot(f, direction);
	const glm::vec3 closest = f + bPrime * direction;
	const float delta = radius * radius - glm::dot(closest, closest);
	if (delta < 0.f) {
		return false;
	}
	const float q = bPrime + (bPrime >= 0.f ? 1.f : -1.f) * std::sqrt(delta);
	if (std::abs(q) < 1e-20f) {
		return false;
	}
	const float c = glm::dot(f, f) - radius * radius;
	const float t0 = c / q;
	const float t1 = q;
	float hit = std::min(t0, t1);
	if (hit < tMin || hit > tMax) {
		hit = std::max(t0, t1);
		if (hit < tMin || hit > tMax) {
			return false;
		}
	}
	t = hit;
	return true;
}

SceneHit traceScene(const Tlas& tlas, std::span<const Blas* const> blases, std::span<const SceneInstance> instances, const BvhRay& ray)
{
	SceneHit hit;
	float tMax = ray.tMax;

	auto testInstances = [&](uint32_t first, uint32_t count, float& t) {
		for (uint32_t slot = first; slot < first + count; slot++) {
			const uint32_t index = tlas.bvh.primOrder[slot];
			const SceneInstance& instance = instances[index];
			if (instance.kind == SceneInstanceKind::Sphere) {
				hit.trianglesTested++;
				float sphereT;
				if (intersectSphere(instance.sphere, ray.origin, ray.direction, ray.tMin, t, sphereT)) {
					t = sphereT;
					hit.hit = true;
					hit.t = sphereT;
					hit.instance = index;
				}
				continue;
			}

			//the ray in object space. Its direction is deliberately not renormalised: origin + t *
			//direction then names the same point in both spaces, so t, tMin and the running closest
			//hit carry across unchanged
			const glm::mat4& worldToObject = tlas.worldToObject[index];
			BvhRay local;
			local.origin = glm::vec3(worldToObject * glm::vec4(ray.origin, 1.f));
			local.direction = glm::mat3(worldToObject) * ray.direction;
			local.tMin = ray.tMin;
			local.tMax = t;

			const Blas& blas = *blases[instance.blas];
			const BvhHit blasHit = traceBvh(blas.bvh, blas.triangles, local);
			hit.nodesVisited += blasHit.nodesVisited;
			hit.trianglesTested += blasHit.trianglesTested;
			hit.stackOverflow |= blasHit.stackOverflow;
			if (blasHit.hit) {
				t = blasHit.t;
				hit.hit = true;
				hit.t = blasHit.t;
				hit.instance = index;
				hit.slot = blasHit.slot;
				hit.barycentrics = blasHit.barycentrics;
			}
		}
	};

	traverseBinaryBvh(tlas.bvh, ray, tMax, testInstances, hit.nodesVisited, hit.stackOverflow);
	return hit;
}
