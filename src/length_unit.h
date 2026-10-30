#pragma once

// THE SCENE'S LENGTH UNIT (docs/plans/completed/physoptics-optical-elements.md): what every panel shows lengths in, meters or
// millimeters, saved with the scene. Display only: the scene model and the file store meters everywhere, so changing it
// changes no number the renderer reads. Optical bench recommends millimeters. The engine sets it when a scene opens, and
// every widget that edits a length goes through these, so a panel never formats a length itself.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

enum class LengthUnit : uint8_t {
	Meters,
	Millimeters
};

// the scene file's names
const char* lengthUnitKey(LengthUnit unit);
std::optional<LengthUnit> findLengthUnit(std::string_view key);
const char* lengthUnitLabel(LengthUnit unit);

LengthUnit currentLengthUnit();
void setCurrentLengthUnit(LengthUnit unit);
// meters -> the unit, and its suffix
double lengthScale();
const char* lengthSuffix();
// "12.70 mm" or "0.0127 m"
std::string formatLength(double meters);

// the widgets: values in meters, shown in the unit. `min` and `max` are meters; both 0 for unbounded
bool dragLength(const char* label, float& meters, float speedMeters = 0.01f, float min = 0.f, float max = 0.f);
bool dragLength3(const char* label, glm::vec3& meters, float speedMeters = 0.01f);
bool dragLength3(const char* label, glm::dvec3& meters, double speedMeters = 0.01);
bool sliderLength(const char* label, float& meters, float min, float max, bool logarithmic = false);
bool inputLength(const char* label, double& meters, double min, double max);
