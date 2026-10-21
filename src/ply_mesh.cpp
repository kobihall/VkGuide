#include <ply_mesh.h>

#include <cstring>

#include <fmt/format.h>
#include <rply/rply.h>

namespace {

struct FaceContext {
	int face[4] {};
	std::vector<int> triIndices;
	std::vector<int> quadIndices;
	size_t skipped { 0 };
};

struct ErrorContext {
	std::string message;
};

void messageCallback(p_ply ply, const char* message)
{
	void* user = nullptr;
	ply_get_ply_user_data(ply, &user, nullptr);
	if (user != nullptr && static_cast<ErrorContext*>(user)->message.empty()) {
		static_cast<ErrorContext*>(user)->message = message;
	}
}

// the flags hold the stride (bits 4-7) and the component (bits 0-3), as pbrt packs them
int vertexCallback(p_ply_argument argument)
{
	float* buffer = nullptr;
	long index = 0;
	long flags = 0;
	ply_get_argument_user_data(argument, (void**)&buffer, &flags);
	ply_get_argument_element(argument, nullptr, &index);
	const int stride = (int)((flags & 0x0F0) >> 4);
	const int offset = (int)(flags & 0x00F);
	buffer[index * stride + offset] = (float)ply_get_argument_value(argument);
	return 1;
}

int faceCallback(p_ply_argument argument)
{
	FaceContext* context = nullptr;
	long flags = 0;
	ply_get_argument_user_data(argument, (void**)&context, &flags);
	long length = 0;
	long valueIndex = 0;
	ply_get_argument_property(argument, nullptr, &length, &valueIndex);
	if (length != 3 && length != 4) {
		if (valueIndex == 0) {
			context->skipped++;
		}
		return 1;
	}
	if (valueIndex < 0) {
		return 1;
	}
	context->face[valueIndex] = (int)ply_get_argument_value(argument);
	if (valueIndex == length - 1) {
		if (length == 3) {
			for (int i = 0; i < 3; i++) {
				context->triIndices.push_back(context->face[i]);
			}
		} else {
			//the bilinear patch order pbrt stores quads in
			context->quadIndices.push_back(context->face[0]);
			context->quadIndices.push_back(context->face[1]);
			context->quadIndices.push_back(context->face[3]);
			context->quadIndices.push_back(context->face[2]);
		}
	}
	return 1;
}

int faceIndexCallback(p_ply_argument argument)
{
	std::vector<int>* faceIndices = nullptr;
	long flags = 0;
	ply_get_argument_user_data(argument, (void**)&faceIndices, &flags);
	faceIndices->push_back((int)ply_get_argument_value(argument));
	return 1;
}

}

void PlyMesh::convertToOnlyTriangles()
{
	triIndices.reserve(triIndices.size() + quadIndices.size() / 4 * 6);
	for (size_t i = 0; i + 3 < quadIndices.size(); i += 4) {
		triIndices.push_back(quadIndices[i]);
		triIndices.push_back(quadIndices[i + 1]);
		triIndices.push_back(quadIndices[i + 3]);
		triIndices.push_back(quadIndices[i]);
		triIndices.push_back(quadIndices[i + 3]);
		triIndices.push_back(quadIndices[i + 2]);
	}
	quadIndices.clear();
}

bool readPlyMesh(const std::filesystem::path& path, PlyMesh& mesh, std::string* error, size_t* skippedFaces)
{
	auto failed = [&](const std::string& message) {
		if (error != nullptr) {
			*error = fmt::format("{}: {}", path.filename().string(), message);
		}
		return false;
	};

	mesh = PlyMesh {};
	ErrorContext errors;
	p_ply ply = ply_open(path.string().c_str(), messageCallback, 0, &errors);
	if (ply == nullptr) {
		return failed(errors.message.empty() ? "cannot open PLY file" : errors.message);
	}
	if (ply_read_header(ply) == 0) {
		ply_close(ply);
		return failed(errors.message.empty() ? "unable to read the header of the PLY file" : errors.message);
	}

	size_t vertexCount = 0;
	size_t faceCount = 0;
	p_ply_element element = nullptr;
	while ((element = ply_get_next_element(ply, element)) != nullptr) {
		const char* name = nullptr;
		long instances = 0;
		ply_get_element_info(element, &name, &instances);
		if (std::strcmp(name, "vertex") == 0) {
			vertexCount = (size_t)instances;
		} else if (std::strcmp(name, "face") == 0) {
			faceCount = (size_t)instances;
		}
	}
	if (vertexCount == 0 || faceCount == 0) {
		ply_close(ply);
		return failed("PLY file is invalid: no face or vertex elements");
	}

	mesh.p.resize(vertexCount);
	if (ply_set_read_cb(ply, "vertex", "x", vertexCallback, mesh.p.data(), 0x30) == 0
		|| ply_set_read_cb(ply, "vertex", "y", vertexCallback, mesh.p.data(), 0x31) == 0
		|| ply_set_read_cb(ply, "vertex", "z", vertexCallback, mesh.p.data(), 0x32) == 0) {
		ply_close(ply);
		return failed("vertex coordinate property not found");
	}

	mesh.n.resize(vertexCount);
	if (ply_set_read_cb(ply, "vertex", "nx", vertexCallback, mesh.n.data(), 0x30) == 0
		|| ply_set_read_cb(ply, "vertex", "ny", vertexCallback, mesh.n.data(), 0x31) == 0
		|| ply_set_read_cb(ply, "vertex", "nz", vertexCallback, mesh.n.data(), 0x32) == 0) {
		mesh.n.clear();
	}

	//the conventions for uv names vary; pbrt tries these four in order
	mesh.uv.resize(vertexCount);
	auto uvNames = [&](const char* u, const char* v) {
		return ply_set_read_cb(ply, "vertex", u, vertexCallback, mesh.uv.data(), 0x20) != 0
			&& ply_set_read_cb(ply, "vertex", v, vertexCallback, mesh.uv.data(), 0x21) != 0;
	};
	if (!(uvNames("u", "v") || uvNames("s", "t") || uvNames("texture_u", "texture_v") || uvNames("texture_s", "texture_t"))) {
		mesh.uv.clear();
	}

	FaceContext context;
	context.triIndices.reserve(faceCount * 3);
	if (ply_set_read_cb(ply, "face", "vertex_indices", faceCallback, &context, 0) == 0) {
		ply_close(ply);
		return failed("vertex indices not found in PLY file");
	}
	if (ply_set_read_cb(ply, "face", "face_indices", faceIndexCallback, &mesh.faceIndices, 0) != 0) {
		mesh.faceIndices.reserve(faceCount);
	}
	if (ply_read(ply) == 0) {
		ply_close(ply);
		return failed(errors.message.empty() ? "unable to read the contents of the PLY file" : errors.message);
	}
	ply_close(ply);

	mesh.triIndices = std::move(context.triIndices);
	mesh.quadIndices = std::move(context.quadIndices);
	if (skippedFaces != nullptr) {
		*skippedFaces = context.skipped;
	}
	for (const std::vector<int>* indices : { &mesh.triIndices, &mesh.quadIndices }) {
		for (int index : *indices) {
			if (index < 0 || (size_t)index >= mesh.p.size()) {
				return failed(fmt::format("vertex index {} is out of bounds; the valid range is [0..{})", index, mesh.p.size()));
			}
		}
	}
	return true;
}
