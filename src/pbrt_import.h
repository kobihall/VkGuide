#pragma once

// pbrt-v4 SCENES AS MODELS (static library rt_pbrt, no Vulkan): a parsed .pbrt file (pbrt_parser.h) mapped onto
// the renderer's scene model as an ImportedModel, with an import report of everything substituted or left out.
//
// The design is docs/plans/completed/pbrt-scene-import.md. In short:
//   - Coordinates. pbrt's world is left-handed and its camera looks down +z; the renderer's world is right-handed
//     and its camera looks down -z. A camera point maps by M = diag(1, 1, -1). A world point maps by M too, unless
//     the camera's own transform already mirrors (the "Scale -1 1 1" many pbrt-v4 scenes start with), when it maps
//     unchanged: either way the camera stays a rotation and the picture comes out the way pbrt draws it.
//   - Each pbrt feature maps to what the renderer has, and to the plan that adds the rest. A substitute is chosen
//     and reported: coated diffuse becomes `pbr`, a disk becomes triangles, a blackbody becomes its RGB.
//   - Spectra become RGB through rt_spectrum (spectrum.h), with pbrt's normalization of every light to a luminance
//     of 1 before its "scale".

#include <filesystem>
#include <string>

#include <imported_model.h>
#include <pbrt_parser.h>

// Reads `path` and maps it into `model`. False, with `error`, for a file pbrt itself would refuse
bool importPbrtFile(const std::filesystem::path& path, ImportedModel& model, std::string* error);
// the mapping alone, for a scene already parsed. Image files are not opened (the engine decodes them), except an
// environment map, which is converted here
void importPbrtScene(const PbrtScene& scene, ImportedModel& model);

// pbrt-v4's equal-area octahedral mapping between the unit square and the sphere (Clarberg), which its image
// infinite lights are stored in (src/pbrt/util/math.cpp)
glm::vec3 equalAreaSquareToSphere(glm::vec2 p);
glm::vec2 equalAreaSphereToSquare(glm::vec3 d);
