#pragma once

// PLY meshes as pbrt-v4 reads them (TriQuadMesh::ReadPLY in src/pbrt/util/mesh.cpp), through rply
// (../CPPLibraries/rply, pbrt's copy, which reads .ply.gz through zlib). Part of rt_pbrt, no Vulkan.

#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct PlyMesh {
	std::vector<glm::vec3> p;
	// empty when the file has none
	std::vector<glm::vec3> n;
	std::vector<glm::vec2> uv;
	// three per triangle
	std::vector<int> triIndices;
	// four per quad, in pbrt's bilinear patch order: the corners 0, 1, 3, 2 of the face as written
	std::vector<int> quadIndices;
	// per face, when the file has "face_indices"
	std::vector<int> faceIndices;

	// each quad as two triangles, as pbrt's ConvertToOnlyTriangles(): the face's corners (0, 1, 2) and (0, 2, 3)
	void convertToOnlyTriangles();
};

// false, with `error` set, for a file that cannot be read or has no vertices or faces. Faces that are neither
// triangles nor quads are skipped and counted in `skippedFaces`
bool readPlyMesh(const std::filesystem::path& path, PlyMesh& mesh, std::string* error, size_t* skippedFaces = nullptr);
