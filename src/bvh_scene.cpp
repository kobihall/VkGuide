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

	//an untraced instance gets an empty box, which the builder leaves out of the tree entirely - as
	//does an unbounded one, which is traced from its own list instead
	std::vector<Aabb> boxes(instances.size());
	for (size_t i = 0; i < instances.size(); i++) {
		const SceneInstance& instance = instances[i];
		const bool isMesh = instance.kind == SceneInstanceKind::Mesh;
		const Blas* blas = isMesh && instance.blas < blases.size() ? blases[instance.blas] : nullptr;
		//a zero scale collapses the instance to nothing and has no inverse to carry a ray with
		const float determinant = glm::determinant(glm::mat3(instance.objectToWorld));
		if ((isMesh && (blas == nullptr || !blas->usable())) || !(std::abs(determinant) > 1e-12f)) {
			continue;
		}
		tlas.worldToObject[i] = glm::inverse(instance.objectToWorld);
		tlas.traced[i] = 1;

		if (isMesh) {
			for (const Aabb& box : blas->frontier) {
				boxes[i].grow(transformBox(box, instance.objectToWorld));
			}
		} else if (shapeBounded(instance.shape)) {
			boxes[i] = transformBox(shapeObjectBounds(instance.shape), instance.objectToWorld);
		} else {
			tlas.unbounded.push_back((uint32_t)i);
		}
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
		//instance's own expected cost - one for a shape, one to enter a BLAS plus that BLAS's SAH
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
				const double cost = instance.kind == SceneInstanceKind::Shape ? 1.0 : 1.0 + blases[instance.blas]->stats.sahCost;
				leafCost += node.bounds.halfArea() * cost;
			}
		}
		const double rootArea = tlas.bounds.halfArea();
		tlas.sceneSahCost = rootArea > 0.0 ? (float)((interiorArea + leafCost) / rootArea) : (float)(bvh.nodes.size() + leafCost);
	}

	//every ray pays for every unbounded shape, whatever the tree holds
	tlas.sceneSahCost += (float)tlas.unbounded.size();

	tlas.bvh = packBvh(std::move(bvh), input, BvhLayout::Binary);
	tlas.buildMs = millisecondsSince(start);
	return tlas;
}

SceneHit traceScene(const Tlas& tlas, std::span<const Blas* const> blases, std::span<const SceneInstance> instances, const BvhRay& ray)
{
	SceneHit hit;
	float tMax = ray.tMax;

	auto testInstance = [&](uint32_t index, float& t) {
		//the ray in object space. Its direction is deliberately not renormalised: origin + t *
		//direction then names the same point in both spaces, so t, tMin and the running closest
		//hit carry across unchanged
		const glm::mat4& worldToObject = tlas.worldToObject[index];
		BvhRay local;
		local.origin = glm::vec3(worldToObject * glm::vec4(ray.origin, 1.f));
		local.direction = glm::mat3(worldToObject) * ray.direction;
		local.tMin = ray.tMin;
		local.tMax = t;
		local.anyHit = ray.anyHit;

		const SceneInstance& instance = instances[index];
		if (instance.kind == SceneInstanceKind::Shape) {
			hit.trianglesTested++;
			float shapeT;
			if (intersectShape(instance.shape, local.origin, local.direction, local.tMin, local.tMax, shapeT)) {
				t = shapeT;
				hit.hit = true;
				hit.t = shapeT;
				hit.instance = index;
			}
			return;
		}

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
	};

	//the unbounded shapes first: a near plane hit then prunes the TLAS walk
	for (const uint32_t index : tlas.unbounded) {
		testInstance(index, tMax);
		if (ray.anyHit && hit.hit) {
			return hit;
		}
	}

	auto testInstances = [&](uint32_t first, uint32_t count, float& t) {
		for (uint32_t slot = first; slot < first + count; slot++) {
			testInstance(tlas.bvh.primOrder[slot], t);
			if (ray.anyHit && hit.hit) {
				return true;
			}
		}
		return false;
	};

	traverseBinaryBvh(tlas.bvh, ray, tMax, testInstances, hit.nodesVisited, hit.stackOverflow);
	return hit;
}

bool occludedScene(const Tlas& tlas, std::span<const Blas* const> blases, std::span<const SceneInstance> instances, const BvhRay& ray)
{
	BvhRay shadow = ray;
	shadow.anyHit = true;
	return traceScene(tlas, blases, instances, shadow).hit;
}
