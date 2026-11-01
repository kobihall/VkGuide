#pragma once

// IES LM-63 photometric files (ANSI/IES LM-63-2002), the way luminaire makers publish how bright a lamp is in each
// direction, for the goniometric light (docs/plans/completed/pbrt-light-types.md). pbrt-v4 reads no IES files; its goniometric
// light takes an equal-area square image of the distribution, which iesEqualAreaImage() makes.
//
// Only type C photometry (photometric type 1), which nearly every file uses, is read: vertical angles from the nadir
// (0, straight down the light's axis) to the zenith (180), horizontal angles around the axis. The file's symmetry is
// expanded, as LM-63 defines it by the horizontal angles' range: a single angle (the same in every direction), 0 to 90
// (a quadrant mirrored into all four), 0 to 180 (mirrored across the 0-180 plane), 90 to 270 (across the 90-270
// plane), or the full circle.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct IesData {
	// degrees, ascending
	std::vector<float> vertical;
	std::vector<float> horizontal;
	// candela, multiplier and ballast factors applied: candela[h * vertical.size() + v]
	std::vector<float> candela;
	// the file's lumens per lamp, or -1 for absolute photometry
	float lumens { -1.f };
	std::vector<std::string> keywords;
};

// false (and why, in `error`) for a file that is not type C LM-63 data
bool parseIes(const std::string& text, IesData& out, std::string* error = nullptr);
bool loadIes(const std::filesystem::path& path, IesData& out, std::string* error = nullptr);

// the candela the file gives the direction at `verticalDegrees` from the nadir and `horizontalDegrees` around the
// axis, bilinear between the file's angles, its symmetry expanded. 0 beyond the vertical angles the file covers
float iesCandela(const IesData& ies, float verticalDegrees, float horizontalDegrees);

// The goniometric light's image of the distribution: `size` x `size` texels of pbrt's equal-area square
// (equalAreaSquareToSphere(), light_sampling.h) in the light's frame, where the light shines down -z (the nadir) and
// horizontal angle 0 lies along +x, 90 along +y. Each texel is its direction's candela divided by 683, the tracer's
// W/sr (PHOTOMETRIC_TO_RADIOMETRIC), averaged over 4 x 4 points of the texel, so a light of intensity 1 shines what
// the file says. Row 0 is v = 0
inline constexpr uint32_t IES_IMAGE_SIZE = 256;
std::vector<float> iesEqualAreaImage(const IesData& ies, uint32_t size);
