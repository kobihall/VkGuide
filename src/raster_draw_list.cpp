#include <vk_engine.h>

#include <algorithm>

void MeshDrawList::rebuild(const std::vector<SceneMeshObject>& objects, const std::map<std::pair<std::string, uint32_t>, const MeshNode*>& nodes)
{
	//each model's nodes by index, so an object resolves with an array lookup: the map is ordered by (model, index)
	std::map<std::string_view, std::vector<const MeshNode*>> nodesByModel;
	std::vector<const MeshNode*>* modelNodes = nullptr;
	std::string_view modelKey;
	for (const auto& [key, node] : nodes) {
		if (modelNodes == nullptr || key.first != modelKey) {
			modelNodes = &nodesByModel[key.first];
			modelKey = key.first;
		}
		if (modelNodes->size() <= key.second) {
			modelNodes->resize((size_t)key.second + 1, nullptr);
		}
		(*modelNodes)[key.second] = node;
	}

	//the viewport draws the scene's mesh objects, not the loaded models: an object's transform is
	//the node's world transform, a deleted object draws nothing, and a hidden one is skipped. That
	//is what makes the gizmo and the outliner agree with what is on screen, and it is the same
	//list the raytracer traces
	DrawContext surfaces;
	surfaces.opaqueSurfaces.reserve(objects.size());
	const std::vector<const MeshNode*>* objectNodes = nullptr;
	std::string_view objectKey;
	for (const SceneMeshObject& object : objects) {
		if (!object.visible) {
			continue;
		}
		//a model's objects are usually together, so the model is looked up again only when it changes
		if (objectNodes == nullptr || object.modelKey != objectKey) {
			const auto found = nodesByModel.find(object.modelKey);
			objectNodes = found != nodesByModel.end() ? &found->second : nullptr;
			objectKey = object.modelKey;
		}
		if (objectNodes == nullptr || object.nodeIndex >= objectNodes->size() || (*objectNodes)[object.nodeIndex] == nullptr) {
			continue;
		}
		(*objectNodes)[object.nodeIndex]->drawSurfaces(object.transform, surfaces);
	}

	//in the order they are drawn, by material, then by mesh, then by surface, so a frame binds each as few times as it
	//can and drawGeometry() draws each run of one surface as one instanced draw. The keys are sorted rather than the
	//records, which are ten times their size
	struct DrawKey {
		const void* material;
		const void* indexBuffer;
		uint32_t firstIndex;
		uint32_t index;
	};
	std::vector<DrawKey> order;
	order.reserve(surfaces.opaqueSurfaces.size());
	for (uint32_t i = 0; i < (uint32_t)surfaces.opaqueSurfaces.size(); i++) {
		const RenderObject& object = surfaces.opaqueSurfaces[i];
		order.push_back({ object.material, object.indexBuffer, object.firstIndex, i });
	}
	std::sort(order.begin(), order.end(), [](const DrawKey& a, const DrawKey& b) {
		if (a.material != b.material) {
			return a.material < b.material;
		}
		if (a.indexBuffer != b.indexBuffer) {
			return a.indexBuffer < b.indexBuffer;
		}
		return a.firstIndex < b.firstIndex;
	});

	//each surface's object-space box carried to a box in world space: its center through the transform, its half
	//extents through the transform's absolute value
	opaque.clear();
	boxes.clear();
	opaque.reserve(order.size());
	boxes.reserve(order.size());
	for (const DrawKey& key : order) {
		const RenderObject& object = surfaces.opaqueSurfaces[key.index];
		const glm::mat4& m = object.transform;
		const glm::vec3& o = object.bounds.origin;
		const glm::vec3& e = object.bounds.extents;
		glm::vec3 lo;
		glm::vec3 hi;
		for (int row = 0; row < 3; row++) {
			const float center = m[0][row] * o.x + m[1][row] * o.y + m[2][row] * o.z + m[3][row];
			const float extent = std::abs(m[0][row]) * e.x + std::abs(m[1][row]) * e.y + std::abs(m[2][row]) * e.z;
			lo[row] = center - extent;
			hi[row] = center + extent;
		}
		opaque.push_back(object);
		boxes.emplace_back(lo, hi);
	}
	transparent = std::move(surfaces.transparentSurfaces);
	//the surfaces worth an occlusion query, and of those the ones of the most triangles when there are too many
	queried.assign(opaque.size(), 0);
	std::vector<uint32_t> candidates;
	for (uint32_t i = 0; i < (uint32_t)opaque.size(); i++) {
		if (opaque[i].indexCount / 3 >= OCCLUSION_MIN_TRIANGLES) {
			candidates.push_back(i);
		}
	}
	if (candidates.size() > OCCLUSION_MAX_QUERIES) {
		std::nth_element(candidates.begin(), candidates.begin() + OCCLUSION_MAX_QUERIES, candidates.end(), [this](uint32_t a, uint32_t b) {
			return opaque[a].indexCount > opaque[b].indexCount;
		});
		candidates.resize(OCCLUSION_MAX_QUERIES);
	}
	for (uint32_t i : candidates) {
		queried[i] = 1;
	}
	occluded.assign(opaque.size(), 0);
	generation++;
}

MeshDrawList::CullResult MeshDrawList::cull(const glm::mat4& viewproj, const glm::vec3& eye, bool skipOccluded, std::vector<const RenderObject*>& out, std::vector<uint32_t>& queries) const
{
	//the frustum's six planes from the rows of the view-projection (Gribb and Hartmann): left, right, bottom, top,
	//near and far. Vulkan keeps clip z from 0 to w, so the near plane is the third row alone
	const glm::mat4& m = viewproj;
	float rows[4][4];
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			rows[r][c] = m[c][r];
		}
	}
	float planes[6][4];
	for (int c = 0; c < 4; c++) {
		planes[0][c] = rows[3][c] + rows[0][c];
		planes[1][c] = rows[3][c] - rows[0][c];
		planes[2][c] = rows[3][c] + rows[1][c];
		planes[3][c] = rows[3][c] - rows[1][c];
		planes[4][c] = rows[2][c];
		planes[5][c] = rows[3][c] - rows[2][c];
	}
	//a box within this of the eye may be cut by the near plane (0.1, rasterProjection()), and its query would then miss
	//the parts in front of the camera: such a surface is drawn and not queried
	constexpr float EYE_MARGIN = 0.5f;
	CullResult result;
	out.reserve(out.size() + opaque.size());
	for (size_t i = 0; i < opaque.size(); i++) {
		const glm::vec3& lo = boxes[i].first;
		const glm::vec3& hi = boxes[i].second;
		bool inside = true;
		for (const float* plane : planes) {
			//the box's corner furthest along the plane's normal: the box is outside when even that is behind it
			const float x = plane[0] >= 0.f ? hi.x : lo.x;
			const float y = plane[1] >= 0.f ? hi.y : lo.y;
			const float z = plane[2] >= 0.f ? hi.z : lo.z;
			if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < 0.f) {
				inside = false;
				break;
			}
		}
		if (!inside) {
			continue;
		}
		const bool holdsEye = glm::all(glm::greaterThan(eye, lo - EYE_MARGIN)) && glm::all(glm::lessThan(eye, hi + EYE_MARGIN));
		if (queried[i] != 0 && !holdsEye) {
			queries.push_back((uint32_t)i);
			if (skipOccluded && occluded[i] != 0) {
				result.occluded++;
				result.occludedTriangles += opaque[i].indexCount / 3;
				continue;
			}
		}
		out.push_back(&opaque[i]);
	}
	return result;
}
