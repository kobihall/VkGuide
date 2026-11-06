#include <length_unit.h>

#include <algorithm>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

namespace {

LengthUnit s_unit = LengthUnit::Meters;

// a format with the unit's suffix and enough digits for it
const char* format()
{
	return s_unit == LengthUnit::Millimeters ? "%.3f mm" : "%.4f m";
}

}

const char* lengthUnitKey(LengthUnit unit)
{
	return unit == LengthUnit::Millimeters ? "mm" : "m";
}

std::optional<LengthUnit> findLengthUnit(std::string_view key)
{
	if (key == "mm") {
		return LengthUnit::Millimeters;
	}
	if (key == "m") {
		return LengthUnit::Meters;
	}
	return std::nullopt;
}

const char* lengthUnitLabel(LengthUnit unit)
{
	return unit == LengthUnit::Millimeters ? "Millimeters" : "Meters";
}

LengthUnit currentLengthUnit()
{
	return s_unit;
}

void setCurrentLengthUnit(LengthUnit unit)
{
	s_unit = unit;
}

double lengthScale()
{
	return s_unit == LengthUnit::Millimeters ? 1000.0 : 1.0;
}

const char* lengthSuffix()
{
	return lengthUnitKey(s_unit);
}

std::string formatLength(double meters)
{
	return s_unit == LengthUnit::Millimeters ? fmt::format("{:.3f} mm", meters * 1e3) : fmt::format("{:.5f} m", meters);
}

bool dragLength(const char* label, float& meters, float speedMeters, float min, float max)
{
	const float scale = (float)lengthScale();
	float shown = meters * scale;
	if (ImGui::DragFloat(label, &shown, speedMeters * scale, min * scale, max * scale, format())) {
		meters = shown / scale;
		return true;
	}
	return false;
}

bool dragLength3(const char* label, glm::vec3& meters, float speedMeters)
{
	const float scale = (float)lengthScale();
	glm::vec3 shown = meters * scale;
	if (ImGui::DragFloat3(label, &shown.x, speedMeters * scale, 0.f, 0.f, format())) {
		meters = shown / scale;
		return true;
	}
	return false;
}

bool dragLength(const char* label, double& meters, double speedMeters, double min, double max)
{
	const double scale = lengthScale();
	double shown = meters * scale;
	const double lo = min * scale;
	const double hi = max * scale;
	const bool bounded = min != 0.0 || max != 0.0;
	if (ImGui::DragScalar(label, ImGuiDataType_Double, &shown, (float)(speedMeters * scale), bounded ? &lo : nullptr, bounded ? &hi : nullptr,
			s_unit == LengthUnit::Millimeters ? "%.4f mm" : "%.6f m", bounded ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None)) {
		meters = shown / scale;
		return true;
	}
	return false;
}

bool dragLength2(const char* label, glm::dvec2& meters, double speedMeters)
{
	const double scale = lengthScale();
	glm::dvec2 shown = meters * scale;
	if (ImGui::DragScalarN(label, ImGuiDataType_Double, &shown.x, 2, (float)(speedMeters * scale), nullptr, nullptr, s_unit == LengthUnit::Millimeters ? "%.4f mm" : "%.6f m")) {
		meters = shown / scale;
		return true;
	}
	return false;
}

bool dragLength3(const char* label, glm::dvec3& meters, double speedMeters)
{
	const double scale = lengthScale();
	glm::dvec3 shown = meters * scale;
	const float speed = (float)(speedMeters * scale);
	if (ImGui::DragScalarN(label, ImGuiDataType_Double, &shown.x, 3, speed, nullptr, nullptr, s_unit == LengthUnit::Millimeters ? "%.4f mm" : "%.6f m")) {
		meters = shown / scale;
		return true;
	}
	return false;
}

bool sliderLength(const char* label, float& meters, float min, float max, bool logarithmic)
{
	const float scale = (float)lengthScale();
	float shown = meters * scale;
	if (ImGui::SliderFloat(label, &shown, min * scale, max * scale, format(), logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None)) {
		meters = shown / scale;
		return true;
	}
	return false;
}

bool inputLength3(const char* label, glm::dvec3& meters)
{
	const double scale = lengthScale();
	glm::dvec3 shown = meters * scale;
	if (ImGui::InputScalarN(label, ImGuiDataType_Double, &shown.x, 3, nullptr, nullptr, s_unit == LengthUnit::Millimeters ? "%.4f mm" : "%.6f m", ImGuiInputTextFlags_EnterReturnsTrue)) {
		meters = shown / scale;
		return true;
	}
	return false;
}

bool inputLength(const char* label, double& meters, double min, double max)
{
	const double scale = lengthScale();
	double shown = meters * scale;
	if (ImGui::InputDouble(label, &shown, 0.0, 0.0, s_unit == LengthUnit::Millimeters ? "%.4f mm" : "%.6f m", ImGuiInputTextFlags_EnterReturnsTrue)) {
		const double value = shown / scale;
		meters = max > min ? std::clamp(value, min, max) : value;
		return true;
	}
	return false;
}
