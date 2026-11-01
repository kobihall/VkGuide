#include <ies_loader.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <light_sampling.h>

namespace {

void fail(std::string* error, std::string message)
{
	if (error != nullptr) {
		*error = std::move(message);
	}
}

// linear interpolation in an ascending table: the index below `x` and the weight of the one above
bool bracket(const std::vector<float>& table, float x, size_t& index, float& t)
{
	if (table.size() == 1) {
		index = 0;
		t = 0.f;
		return x == table[0];
	}
	if (x < table.front() || x > table.back()) {
		return false;
	}
	const auto upper = std::upper_bound(table.begin(), table.end(), x);
	index = upper == table.end() ? table.size() - 2 : (size_t)std::max<std::ptrdiff_t>(upper - table.begin() - 1, 0);
	index = std::min(index, table.size() - 2);
	const float span = table[index + 1] - table[index];
	t = span > 0.f ? std::clamp((x - table[index]) / span, 0.f, 1.f) : 0.f;
	return true;
}

}

bool parseIes(const std::string& text, IesData& out, std::string* error)
{
	out = IesData {};
	std::istringstream lines(text);
	std::string line;
	bool tilt = false;
	std::string rest;
	while (std::getline(lines, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.rfind("TILT=", 0) == 0) {
			tilt = true;
			const std::string mode = line.substr(5);
			if (mode == "INCLUDE") {
				//the lamp-to-luminaire geometry, the tilt angles and their multipliers: read past and not applied, as
				//pbrt and most renderers do
				std::string tiltText;
				std::string tiltLine;
				while (std::getline(lines, tiltLine)) {
					tiltText += tiltLine + " ";
					std::istringstream count(tiltText);
					float geometry = 0.f;
					float pairs = 0.f;
					if (count >> geometry >> pairs) {
						std::vector<float> values;
						float v;
						while (count >> v) {
							values.push_back(v);
						}
						if (values.size() >= (size_t)(2.f * pairs)) {
							break;
						}
					}
				}
			}
			break;
		}
		if (!line.empty() && line[0] == '[') {
			out.keywords.push_back(line);
		}
	}
	if (!tilt) {
		fail(error, "no TILT= line: not an IES LM-63 file");
		return false;
	}
	std::stringstream numbers;
	numbers << lines.rdbuf();
	std::string all = numbers.str();
	std::replace(all.begin(), all.end(), ',', ' ');
	std::istringstream tokens(all);
	float lamps = 0.f;
	float lumens = 0.f;
	float multiplier = 1.f;
	float verticalCount = 0.f;
	float horizontalCount = 0.f;
	float photometricType = 0.f;
	float unitsType = 0.f;
	float width = 0.f;
	float length = 0.f;
	float height = 0.f;
	float ballast = 1.f;
	float ballastLamp = 1.f;
	float watts = 0.f;
	if (!(tokens >> lamps >> lumens >> multiplier >> verticalCount >> horizontalCount >> photometricType >> unitsType >> width >> length >> height >> ballast >> ballastLamp >> watts)) {
		fail(error, "the photometric header after TILT= is incomplete");
		return false;
	}
	if ((int)photometricType != 1) {
		fail(error, "only type C photometry (photometric type 1) is read");
		return false;
	}
	const size_t vertical = (size_t)verticalCount;
	const size_t horizontal = (size_t)horizontalCount;
	if (vertical == 0 || horizontal == 0 || vertical > 100000 || horizontal > 100000) {
		fail(error, "the file has no angles");
		return false;
	}
	out.vertical.resize(vertical);
	out.horizontal.resize(horizontal);
	out.candela.resize(vertical * horizontal);
	for (float& v : out.vertical) {
		if (!(tokens >> v)) {
			fail(error, "the file ends inside its vertical angles");
			return false;
		}
	}
	for (float& h : out.horizontal) {
		if (!(tokens >> h)) {
			fail(error, "the file ends inside its horizontal angles");
			return false;
		}
	}
	const float scale = multiplier * ballast * ballastLamp;
	for (float& c : out.candela) {
		if (!(tokens >> c)) {
			fail(error, "the file ends inside its candela values");
			return false;
		}
		c = std::max(c * scale, 0.f);
	}
	if (!std::is_sorted(out.vertical.begin(), out.vertical.end()) || !std::is_sorted(out.horizontal.begin(), out.horizontal.end())) {
		fail(error, "the angles are not in ascending order");
		return false;
	}
	out.lumens = lumens;
	return true;
}

bool loadIes(const std::filesystem::path& path, IesData& out, std::string* error)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		fail(error, "cannot open " + path.filename().string());
		return false;
	}
	std::stringstream text;
	text << file.rdbuf();
	return parseIes(text.str(), out, error);
}

float iesCandela(const IesData& ies, float verticalDegrees, float horizontalDegrees)
{
	if (ies.vertical.empty() || ies.horizontal.empty()) {
		return 0.f;
	}
	size_t vi = 0;
	float vt = 0.f;
	if (ies.vertical.size() == 1) {
		vi = 0;
	} else if (!bracket(ies.vertical, verticalDegrees, vi, vt)) {
		return 0.f;
	}
	//the symmetry the horizontal angles' range implies
	float c = horizontalDegrees - 360.f * std::floor(horizontalDegrees / 360.f);
	const float first = ies.horizontal.front();
	const float last = ies.horizontal.back();
	if (ies.horizontal.size() == 1) {
		c = first;
	} else if (first == 0.f && last == 90.f) {
		if (c > 180.f) {
			c = 360.f - c;
		}
		if (c > 90.f) {
			c = 180.f - c;
		}
	} else if (first == 0.f && last == 180.f) {
		if (c > 180.f) {
			c = 360.f - c;
		}
	} else if (first == 90.f && last == 270.f) {
		if (c < 90.f) {
			c = 180.f - c;
		} else if (c > 270.f) {
			c = 540.f - c;
		}
	}
	const size_t vertical = ies.vertical.size();
	auto at = [&](size_t h, size_t v) { return ies.candela[h * vertical + v]; };
	auto column = [&](size_t h) {
		if (vertical == 1) {
			return at(h, 0);
		}
		return at(h, vi) * (1.f - vt) + at(h, vi + 1) * vt;
	};
	if (ies.horizontal.size() == 1) {
		return column(0);
	}
	size_t hi = 0;
	float ht = 0.f;
	if (bracket(ies.horizontal, c, hi, ht)) {
		return column(hi) * (1.f - ht) + column(hi + 1) * ht;
	}
	//a full circle that stops short of 360: between the last angle and the first, wrapped
	const float span = first + 360.f - last;
	const float t = span > 0.f ? std::clamp((c < first ? c + 360.f - last : c - last) / span, 0.f, 1.f) : 0.f;
	return column(ies.horizontal.size() - 1) * (1.f - t) + column(0) * t;
}

std::vector<float> iesEqualAreaImage(const IesData& ies, uint32_t size)
{
	std::vector<float> image((size_t)size * size, 0.f);
	constexpr int SUB = 4;
	constexpr float TO_DEGREES = 180.f / LIGHT_PI;
	for (uint32_t y = 0; y < size; y++) {
		for (uint32_t x = 0; x < size; x++) {
			double sum = 0.0;
			for (int j = 0; j < SUB; j++) {
				for (int i = 0; i < SUB; i++) {
					const glm::vec2 uv(((float)x + ((float)i + 0.5f) / SUB) / (float)size, ((float)y + ((float)j + 0.5f) / SUB) / (float)size);
					const glm::vec3 d = equalAreaSquareToSphere(uv);
					//the nadir is -z; horizontal angle 0 along +x, 90 along +y
					const float verticalAngle = std::acos(std::clamp(-d.z, -1.f, 1.f)) * TO_DEGREES;
					const float horizontalAngle = std::atan2(d.y, d.x) * TO_DEGREES;
					sum += iesCandela(ies, verticalAngle, horizontalAngle);
				}
			}
			image[(size_t)y * size + x] = (float)(sum / (SUB * SUB)) / 683.f;
		}
	}
	return image;
}
