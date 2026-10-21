#pragma once

// A finished render as files, with no Vulkan: the picture, and beside it everything the render kept, as the
// named channels of one OpenEXR file (docs/plans/completed/pbrt-aovs-and-image-output.md). The channel names follow
// pbrt-v4's GBufferFilm, so its imgtool reads the files: R G B, Albedo.R, P.X, N.X, Ns.X, u v, Variance.R,
// RelativeVariance.R; with Z for the depth, n for the sample count, id.instance and id.material for the ids, and
// LightGroup.<name>.R for each light group's share of the picture. The header carries pbrt's ImageMetadata where
// it applies: renderTimeSeconds, samplesPerPixel, cameraFromWorld, NDCFromWorld, MSE and chromaticities; and the
// scene, the kernel selection and the render settings as strings.

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <aov.h>
#include <image_io.h>

struct RenderExportInfo {
	// the scene file's path, empty for a scene never saved
	std::string scenePath;
	// the kernel selection as text, and the render settings as JSON
	std::string kernels;
	std::string settings;
	float renderSeconds { 0.f };
	// the camera, as pbrt keeps it: the view matrix (world to camera) and the matrix from world to normalized
	// device coordinates, x right and y down over [0, 1]. glm's column-major form: the file gets them row-major
	glm::mat4 cameraFromWorld { 1.f };
	glm::mat4 ndcFromWorld { 1.f };
	// the mean squared error against the loaded reference, when there is one
	bool hasMse { false };
	float mse { 0.f };
	// the names of the light groups, by index; the render's groups are the first lightGroups of them
	std::vector<std::string> lightGroupNames;
};

// the channels of the file: `beauty` as R, G and B, then each output the render kept, resolved as pbrt's GBufferFilm
// resolves its sums, then one RGB set per light group (its sums over the weight sum), with the header attributes
ChannelImage buildExportImage(const Image& beauty, const RenderOutputsData& outputs, const RenderExportInfo& info);

// a name that is safe as an EXR layer: letters, digits and underscores, never empty
std::string exportChannelName(const std::string& name);
