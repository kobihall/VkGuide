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

	//in the order they are drawn, by material and then by mesh, so a frame binds each as few times as it can. The keys
	//are sorted rather than the records, which are ten times their size
	struct DrawKey {
		const void* material;
		const void* indexBuffer;
		uint32_t index;
	};
	std::vector<DrawKey> order;
	order.reserve(surfaces.opaqueSurfaces.size());
	for (uint32_t i = 0; i < (uint32_t)surfaces.opaqueSurfaces.size(); i++) {
		order.push_back({ surfaces.opaqueSurfaces[i].material, surfaces.opaqueSurfaces[i].indexBuffer, i });
	}
	std::sort(order.begin(), order.end(), [](const DrawKey& a, const DrawKey& b) {
		return a.material == b.material ? a.indexBuffer < b.indexBuffer : a.material < b.material;
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
}

void MeshDrawList::cull(const glm::mat4& viewproj, std::vector<const RenderObject*>& out) const
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
		if (inside) {
			out.push_back(&opaque[i]);
		}
	}
}
