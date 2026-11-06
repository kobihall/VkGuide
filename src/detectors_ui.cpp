#include <detectors_ui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>

#include <fmt/format.h>
#include <imgui.h>
#include <implot.h>

#include <length_unit.h>
#include <optic_detectors.h>
#include <vk_engine.h>

namespace {

constexpr double PI = 3.14159265358979323846;

DetectorFilms& films(VulkanEngine* engine)
{
	return engine->m_raytracer.gpu().detectors();
}

bool monochromatic(VulkanEngine* engine)
{
	return films(engine).spectral().representation == SpectralRepresentation::Monochromatic;
}

std::string formatIrradiance(double wattsPerSquareMeter)
{
	return fmt::format("{:.4g} W/m^2", wattsPerSquareMeter);
}

void drawMoments(const DetectorFilms::Entry& entry)
{
	if (!entry.reading.moments || !(entry.reading.moments->power > 0.0)) {
		return;
	}
	const BeamMoments& m = *entry.reading.moments;
	ImGui::SeparatorText("Centroid and widths (ISO 11146)");
	ImGui::Text("centroid  x %s, y %s", formatLength(m.centroidX).c_str(), formatLength(m.centroidY).c_str());
	ImGui::Text("D4 sigma  x %s, y %s", formatLength(m.diameterX).c_str(), formatLength(m.diameterY).c_str());
	ImGui::Text("principal %s and %s, at %.2f deg", formatLength(m.diameterMajor).c_str(), formatLength(m.diameterMinor).c_str(), m.angle * 180.0 / PI);
	ImGui::TextDisabled("radii (half the diameters) %s and %s; integration area %s after %u iterations", formatLength(0.5 * m.diameterMajor).c_str(),
		formatLength(0.5 * m.diameterMinor).c_str(), m.converged ? "settled" : "not settled", m.iterations);
	ImGui::Text("peak irradiance %s", formatIrradiance(entry.reading.peak).c_str());
}

void drawCurves(const DetectorFilms::Entry& entry)
{
	if (!entry.reading.moments || !(entry.reading.moments->power > 0.0)) {
		return;
	}
	const BeamMoments& m = *entry.reading.moments;
	const double reach = std::max(1.5 * m.diameterMajor, 2.0 * std::max(entry.image.pixelWidth, entry.image.pixelHeight));
	if (ImGui::CollapsingHeader("Encircled power")) {
		const EncircledPower curve = encircledPower(entry.image, m.centroidX, m.centroidY, reach, 96);
		std::vector<double> radius(curve.radius.size());
		std::vector<double> fraction(curve.power.size());
		const double total = std::max(entry.reading.power, 1e-300);
		for (size_t i = 0; i < radius.size(); i++) {
			radius[i] = curve.radius[i] * lengthScale();
			fraction[i] = curve.power[i] / total;
		}
		if (ImPlot::BeginPlot("##encircled", ImVec2(-1.f, ImGui::GetFontSize() * 12.f), ImPlotFlags_NoMenus | ImPlotFlags_NoLegend)) {
			ImPlot::SetupAxes(fmt::format("radius ({})", lengthSuffix()).c_str(), "fraction of the power");
			ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.05, ImGuiCond_Always);
			ImPlot::PlotLine("encircled", radius.data(), fraction.data(), (int)radius.size());
			ImPlot::EndPlot();
		}
	}
	if (ImGui::CollapsingHeader("Profiles")) {
		const Profile major = lineProfile(entry.image, m.centroidX, m.centroidY, m.angle, 256);
		const Profile minor = lineProfile(entry.image, m.centroidX, m.centroidY, m.angle + 0.5 * PI, 256);
		if (ImPlot::BeginPlot("##profiles", ImVec2(-1.f, ImGui::GetFontSize() * 12.f), ImPlotFlags_NoMenus)) {
			ImPlot::SetupAxes(fmt::format("from the centroid ({})", lengthSuffix()).c_str(), "W/m^2");
			for (const auto& [profile, label] : { std::pair { &major, "major axis" }, std::pair { &minor, "minor axis" } }) {
				std::vector<double> position(profile->position.size());
				for (size_t i = 0; i < position.size(); i++) {
					position[i] = profile->position[i] * lengthScale();
				}
				ImPlot::PlotLine(label, position.data(), profile->irradiance.data(), (int)position.size());
			}
			ImPlot::EndPlot();
		}
	}
}

void drawSpectrum(VulkanEngine* engine, const DetectorFilms::Entry& entry)
{
	ImGui::SeparatorText("Spectrum");
	if (films(engine).spectral().representation == SpectralRepresentation::ExactRgb) {
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextDisabled("Exact RGB carries no wavelengths: render in a spectral mode (Render Settings -> Spectral) for the spectrum. The power above is the whole beam's.");
		ImGui::PopTextWrapPos();
		return;
	}
	if (entry.reading.spectrum.empty()) {
		ImGui::TextDisabled("no spectrum read yet");
		return;
	}
	size_t peak = 0;
	for (size_t i = 1; i < entry.reading.spectrum.size(); i++) {
		if (entry.reading.spectrum[i] > entry.reading.spectrum[peak]) {
			peak = i;
		}
	}
	ImGui::Text("peak %.3f nm, %s", entry.reading.spectrumWavelength[peak], formatPower(entry.reading.spectrum[peak], true).c_str());
	if (ImPlot::BeginPlot("##spectrum", ImVec2(-1.f, ImGui::GetFontSize() * 12.f), ImPlotFlags_NoMenus | ImPlotFlags_NoLegend)) {
		ImPlot::SetupAxes("wavelength (nm)", "W/nm");
		ImPlot::PlotLine("spectrum", entry.reading.spectrumWavelength.data(), entry.reading.spectrum.data(), (int)entry.reading.spectrum.size());
		ImPlot::EndPlot();
	}
}

// What the stack plot shows along the axis, each where its kind has it
enum class StackQuantity : int {
	// the D4 sigma radii along the principal axes, and their caustic fit
	PrincipalRadii,
	RadiiXY,
	Centroid,
	Peak,
	Power,
	// a spectrometer's brightest bin
	PeakWavelength
};
constexpr const char* STACK_QUANTITY_LABELS[] = { "radii (major, minor)", "radii (x, y)", "centroid (x, y)", "peak irradiance", "power", "peak wavelength" };

// the quantity each stack shows, by stack id: an imaging sensor's radii and anything else's power until the user picks
std::map<uint64_t, StackQuantity> g_stackQuantity;

// A member's place along the stack and its reading. The place is the one the render recorded the member at (its
// detector record's world frame): editing the stack moves nothing in a plot until a render starts with the new places
struct StackPoint {
	uint64_t opticId;
	glm::dvec3 origin;
	double along;
	const DetectorFilms::Entry* entry;
};

// Every member the render recorded, along the stack's axis, and a quantity of their readings plotted against it, with the
// member that shows marked. The radii get ISO 11146-1's caustic fit: the waist, where it is and M^2 at the design
// wavelength
void drawStackPlot(VulkanEngine* engine, const DetectorFilms::Entry& entry)
{
	const RaytraceSceneEditor& editor = engine->m_raytraceScene;
	const SceneOptic* shown = editor.findOptic(entry.opticId);
	const SensorStack* stack = shown != nullptr && shown->stack != 0 ? editor.findSensorStack(shown->stack) : nullptr;
	if (stack == nullptr || !ImGui::CollapsingHeader("Along the stack", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}
	std::vector<StackPoint> points;
	for (const SceneOptic& optic : editor.optics()) {
		const DetectorFilms::Entry* member = optic.stack == stack->id ? films(engine).find(optic.id) : nullptr;
		if (member != nullptr && member->reading.frames > 0) {
			points.push_back({ optic.id, glm::dvec3(member->record.origin), 0.0, member });
		}
	}
	if (points.size() < 2) {
		ImGui::TextDisabled("%zu sensors read so far", points.size());
		return;
	}
	// along the line from the first recorded member to the last, from the first
	const glm::dvec3 axis = glm::normalize(points.back().origin - points.front().origin);
	for (StackPoint& p : points) {
		p.along = glm::dot(p.origin - points.front().origin, axis);
	}

	// the quantities this kind has
	const bool imaging = entry.geometry.imaging;
	const bool spectrum = entry.geometry.spectrometer && films(engine).spectral().representation != SpectralRepresentation::ExactRgb;
	auto offered = [&](StackQuantity q) {
		switch (q) {
		case StackQuantity::PrincipalRadii:
		case StackQuantity::RadiiXY:
		case StackQuantity::Centroid:
		case StackQuantity::Peak:
			return imaging;
		case StackQuantity::Power:
			return true;
		case StackQuantity::PeakWavelength:
			return spectrum;
		}
		return false;
	};
	auto found = g_stackQuantity.find(stack->id);
	if (found == g_stackQuantity.end() || !offered(found->second)) {
		found = g_stackQuantity.insert_or_assign(stack->id, imaging ? StackQuantity::PrincipalRadii : StackQuantity::Power).first;
	}
	StackQuantity& quantity = found->second;
	if (ImGui::BeginCombo("quantity", STACK_QUANTITY_LABELS[(int)quantity])) {
		for (int q = 0; q < (int)std::size(STACK_QUANTITY_LABELS); q++) {
			if (offered((StackQuantity)q) && ImGui::Selectable(STACK_QUANTITY_LABELS[q], (int)quantity == q)) {
				quantity = (StackQuantity)q;
			}
		}
		ImGui::EndCombo();
	}

	// the curves, in the panels' units: lengths in the scene's unit, power in the unit of the largest reading
	const bool perNm = monochromatic(engine);
	double largest = 0.0;
	for (const StackPoint& p : points) {
		largest = std::max(largest, std::abs(p.entry->reading.power));
	}
	const double powerScale = largest >= 1.0 ? 1.0 : largest >= 1e-3 ? 1e3 : largest >= 1e-6 ? 1e6 : 1e9;
	const char* powerUnit = largest >= 1.0 ? "W" : largest >= 1e-3 ? "mW" : largest >= 1e-6 ? "uW" : "nW";
	struct Curve {
		std::string label;
		std::vector<double> x;
		std::vector<double> y;
		std::vector<double> error;
		// the colormap's entry: a fit takes its readings'
		int color { 0 };
	};
	std::vector<Curve> curves;
	std::string yLabel;
	auto curve = [&](const char* label) -> Curve& {
		curves.push_back(Curve { label, {}, {}, {}, (int)curves.size() });
		return curves.back();
	};
	const double toUnit = lengthScale();
	switch (quantity) {
	case StackQuantity::PrincipalRadii:
	case StackQuantity::RadiiXY:
	case StackQuantity::Centroid: {
		const bool principal = quantity == StackQuantity::PrincipalRadii;
		const bool centroid = quantity == StackQuantity::Centroid;
		curve(principal ? "major" : "x");
		curve(principal ? "minor" : "y");
		yLabel = fmt::format("{} ({})", centroid ? "centroid" : "D4 sigma radius", lengthSuffix());
		for (const StackPoint& p : points) {
			if (!p.entry->reading.moments || !(p.entry->reading.moments->power > 0.0)) {
				continue;
			}
			const BeamMoments& m = *p.entry->reading.moments;
			curves[0].x.push_back(p.along * toUnit);
			curves[1].x.push_back(p.along * toUnit);
			curves[0].y.push_back(toUnit * (centroid ? m.centroidX : 0.5 * (principal ? m.diameterMajor : m.diameterX)));
			curves[1].y.push_back(toUnit * (centroid ? m.centroidY : 0.5 * (principal ? m.diameterMinor : m.diameterY)));
		}
		break;
	}
	case StackQuantity::Peak: {
		Curve& c = curve("peak");
		yLabel = "W/m^2";
		for (const StackPoint& p : points) {
			c.x.push_back(p.along * toUnit);
			c.y.push_back(p.entry->reading.peak);
		}
		break;
	}
	case StackQuantity::Power: {
		Curve& c = curve("power");
		yLabel = fmt::format("{}{}", powerUnit, perNm ? "/nm" : "");
		for (const StackPoint& p : points) {
			c.x.push_back(p.along * toUnit);
			c.y.push_back(p.entry->reading.power * powerScale);
			c.error.push_back(p.entry->reading.powerError * powerScale);
		}
		break;
	}
	case StackQuantity::PeakWavelength: {
		Curve& c = curve("peak");
		yLabel = "nm";
		for (const StackPoint& p : points) {
			const std::vector<double>& s = p.entry->reading.spectrum;
			if (s.empty()) {
				continue;
			}
			const size_t peak = (size_t)(std::max_element(s.begin(), s.end()) - s.begin());
			c.x.push_back(p.along * toUnit);
			c.y.push_back(p.entry->reading.spectrumWavelength[peak]);
		}
		break;
	}
	}

	// the radii's caustics, each axis fitted on its own, drawn over the stack's length
	std::vector<Curve> fits;
	const bool radii = quantity == StackQuantity::PrincipalRadii || quantity == StackQuantity::RadiiXY;
	if (radii) {
		const double lambda = engine->opticBuildContext().designWavelength * 1e-9;
		for (const Curve& c : curves) {
			std::vector<double> z(c.x.size());
			std::vector<double> w(c.y.size());
			for (size_t i = 0; i < z.size(); i++) {
				z[i] = c.x[i] / toUnit;
				w[i] = c.y[i] / toUnit;
			}
			const std::optional<CausticFit> fit = fitCaustic(z, w);
			if (!fit) {
				ImGui::TextDisabled("%s: no caustic to fit (3 or more radii that widen away from a waist)", c.label.c_str());
				continue;
			}
			ImGui::Text("%s: waist radius %s, %s from the start, M^2 %.4f", c.label.c_str(), formatLength(fit->waist).c_str(), formatLength(fit->position).c_str(), fit->m2(lambda));
			ImGui::SetItemTooltip("ISO 11146-1's fit of w^2 = a + b z + c z^2 through the D4 sigma radii; M^2 = pi w0 theta / lambda at the design wavelength, %.2f nm.\n"
				"ISO 11146 asks for 10 planes or more, half within one Rayleigh range of the waist and half beyond two", lambda * 1e9);
			Curve line { fmt::format("{} fit", c.label), {}, {}, {}, c.color };
			const double from = std::min(0.0, fit->position);
			const double to = std::max(points.back().along, fit->position);
			for (int i = 0; i <= 128; i++) {
				const double zi = from + (to - from) * i / 128.0;
				line.x.push_back(zi * toUnit);
				line.y.push_back(fit->radius(zi) * toUnit);
			}
			fits.push_back(std::move(line));
		}
	}

	if (ImPlot::BeginPlot("##stack", ImVec2(-1.f, ImGui::GetFontSize() * 14.f), ImPlotFlags_NoMenus)) {
		ImPlot::SetupAxes(fmt::format("from the stack's start ({})", lengthSuffix()).c_str(), yLabel.c_str());
		// each quantity's readings as markers on a line, its error bars and its fit in the same color, the fit thinner
		for (size_t i = 0; i < curves.size(); i++) {
			const Curve& c = curves[i];
			const ImVec4 color = ImPlot::GetColormapColor(c.color);
			ImPlot::PlotLine(c.label.c_str(), c.x.data(), c.y.data(), (int)c.x.size(),
				ImPlotSpec(ImPlotProp_LineColor, ImGui::ColorConvertFloat4ToU32(color), ImPlotProp_LineWeight, 1.5f, ImPlotProp_Marker, ImPlotMarker_Circle, ImPlotProp_MarkerSize, 3.f));
			if (!c.error.empty()) {
				ImPlot::PlotErrorBars(c.label.c_str(), c.x.data(), c.y.data(), c.error.data(), (int)c.x.size(), ImPlotSpec(ImPlotProp_LineColor, ImGui::ColorConvertFloat4ToU32(color)));
			}
		}
		for (size_t i = 0; i < fits.size(); i++) {
			const Curve& f = fits[i];
			const ImVec4 color = ImPlot::GetColormapColor(f.color);
			ImPlot::PlotLine(f.label.c_str(), f.x.data(), f.y.data(), (int)f.x.size(), ImPlotSpec(ImPlotProp_LineColor, ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.6f)), ImPlotProp_LineWeight, 1.f));
		}
		// the member that shows
		for (const StackPoint& p : points) {
			if (p.opticId == entry.opticId) {
				const double x = p.along * toUnit;
				ImPlot::PlotInfLines("shown", &x, 1);
			}
		}
		ImPlot::EndPlot();
	}
}

void drawPowerLines(VulkanEngine* engine, const DetectorFilms::Entry& entry)
{
	const bool perNm = monochromatic(engine);
	const DetectorReading& r = entry.reading;
	if (r.powerError > 0.0 && r.power != 0.0) {
		ImGui::Text("Power %s +- %s (%.3f %%)", formatPower(r.power, perNm).c_str(), formatPower(r.powerError, perNm).c_str(), 100.0 * r.powerError / std::abs(r.power));
	} else {
		ImGui::Text("Power %s", formatPower(r.power, perNm).c_str());
	}
	if (entry.cameraPaths) {
		ImGui::TextDisabled("Rendered by camera paths from its pixels: irradiance, pi times their mean radiance");
	} else {
		ImGui::TextDisabled("%.0f of %.0f light paths reached it, over %u frames", r.pathsReached, r.pathsEmitted, r.frames);
	}
	if ((entry.record.channels == 3 || entry.cameraPaths) && entry.channels.size() >= 3) {
		double rgb[3] = {};
		for (size_t i = 0; i < entry.channels.size(); i++) {
			rgb[i % 3] += entry.channels[i];
		}
		ImGui::TextDisabled("channels %s, %s, %s (exact RGB: the power is their mean)", formatPower(rgb[0]).c_str(), formatPower(rgb[1]).c_str(), formatPower(rgb[2]).c_str());
	}
}

}

std::string formatPower(double watts, bool perNanometer)
{
	const double a = std::abs(watts);
	const char* suffix = perNanometer ? "/nm" : "";
	if (a == 0.0) {
		return fmt::format("0 W{}", suffix);
	}
	if (a >= 1.0) {
		return fmt::format("{:.5g} W{}", watts, suffix);
	}
	if (a >= 1e-3) {
		return fmt::format("{:.5g} mW{}", watts * 1e3, suffix);
	}
	if (a >= 1e-6) {
		return fmt::format("{:.5g} uW{}", watts * 1e6, suffix);
	}
	if (a >= 1e-9) {
		return fmt::format("{:.5g} nW{}", watts * 1e9, suffix);
	}
	return fmt::format("{:.4g} W{}", watts, suffix);
}

void drawDetectorPictureSettings(DetectorFilms::Entry& entry)
{
	ImGui::Checkbox("logarithmic", &entry.view.logarithmic);
	ImGui::SameLine();
	ImGui::Checkbox("false color", &entry.view.falseColor);
	if (entry.view.logarithmic) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.f);
		ImGui::SliderFloat("decades", &entry.view.decades, 1.f, 8.f, "%.1f");
	}
	//the picture's top, as a beam profiler's normalization: the film's peak each frame, or a value held while the beam
	//changes, which clips what rises above it
	//turning it off holds the value it was showing in gray, not the one held before it was turned on
	if (ImGui::Checkbox("auto-normalize", &entry.view.autoNormalize) && !entry.view.autoNormalize) {
		entry.view.normalization = (float)entry.reading.peak;
	}
	ImGui::SetItemTooltip("Scale the picture to the film's peak irradiance each frame");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.f);
	ImGui::BeginDisabled(entry.view.autoNormalize);
	float shown = entry.view.autoNormalize ? (float)entry.reading.peak : entry.view.normalization;
	const float step = std::max(std::abs(shown) * 0.005f, 1e-30f);
	if (ImGui::DragFloat("normalization", &shown, step, 0.f, FLT_MAX, "%.4g W/m^2", ImGuiSliderFlags_AlwaysClamp) && !entry.view.autoNormalize) {
		entry.view.normalization = shown;
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("The irradiance at the top of the picture: brighter pixels clip to it");
}

void drawDetectorReadout(VulkanEngine* engine, const DetectorFilms::Entry& entry, int hoverX, int hoverY)
{
	ImGui::TextDisabled("%s, x to the right and y up as its frame has them", opticTraits(entry.kind).label);
	if (hoverX >= 0 && hoverY >= 0 && (uint32_t)hoverX < entry.image.width && (uint32_t)hoverY < entry.image.height) {
		const double watts = entry.image.at((uint32_t)hoverX, (uint32_t)hoverY);
		const double area = entry.image.pixelWidth * entry.image.pixelHeight;
		ImGui::SetTooltip("x %s, y %s\n%s in the pixel\n%s", formatLength(entry.image.centerX((uint32_t)hoverX)).c_str(), formatLength(entry.image.centerY((uint32_t)hoverY)).c_str(),
			formatPower(watts).c_str(), formatIrradiance(area > 0.0 ? watts / area : 0.0).c_str());
	}
	drawPowerLines(engine, entry);
	if (entry.geometry.imaging) {
		drawMoments(entry);
		drawCurves(entry);
	}
	if (entry.geometry.spectrometer) {
		drawSpectrum(engine, entry);
	}
	drawStackPlot(engine, entry);
}

void drawDetectorTargetButton(VulkanEngine* engine, const SceneOptic& optic)
{
	ImGui::Spacing();
	if (engine->m_raytracer.renderDetectorId() == optic.id) {
		ImGui::TextDisabled("The render target: the Raytraced Output window shows its film and readings");
	} else if (ImGui::SmallButton("Render to this sensor")) {
		engine->m_raytracer.setRenderDetector(optic.id);
	}
}
