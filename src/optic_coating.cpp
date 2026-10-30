#include <optic_coating.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include <fmt/format.h>

namespace {

constexpr double PI = 3.14159265358979323846;
using Complex = std::complex<double>;

// the tilted index q = N cos(theta) in a layer of index N (Macleod's N = n - ik) for an incident n0 sin(theta0) =
// alpha, the root in the fourth quadrant: a wave that decays into an absorbing or evanescent layer
Complex tiltedIndex(Complex n, double alpha)
{
	Complex q = std::sqrt(n * n - Complex(alpha * alpha, 0.0));
	if (q.real() < 0.0 || (q.real() == 0.0 && q.imag() > 0.0)) {
		q = -q;
	}
	if (q.imag() > 0.0 && std::abs(q.imag()) < 1e-300) {
		q = Complex(q.real(), 0.0);
	}
	return q;
}

// pbrt's n + ik as Macleod's n - ik
Complex macleod(Complex n)
{
	return std::conj(n);
}

// (r, t, R, T) for one polarization from the tilted admittances
void characteristic(Complex y0, const std::vector<Complex>& y, const std::vector<Complex>& delta, Complex ys, Complex& r, Complex& t, double& R, double& T)
{
	// [B; C] = prod_j [[cos d, i sin d / y], [i y sin d, cos d]] [1; ys]
	Complex B(1.0, 0.0);
	Complex C = ys;
	for (int j = (int)y.size() - 1; j >= 0; j--) {
		const Complex c = std::cos(delta[(size_t)j]);
		const Complex s = std::sin(delta[(size_t)j]);
		const Complex i(0.0, 1.0);
		const Complex nb = c * B + i * s / y[(size_t)j] * C;
		const Complex nc = i * y[(size_t)j] * s * B + c * C;
		B = nb;
		C = nc;
	}
	const Complex denominator = y0 * B + C;
	r = (y0 * B - C) / denominator;
	t = 2.0 * y0 / denominator;
	R = std::norm(r);
	T = 4.0 * y0.real() * ys.real() / std::norm(denominator);
	R = std::clamp(R, 0.0, 1.0);
	T = std::clamp(T, 0.0, 1.0 - R);
}

Complex metalIndex(const std::string& symbol, double lambda)
{
	const std::optional<SpectrumRef> eta = namedRef("metal-" + symbol + "-eta");
	const std::optional<SpectrumRef> k = namedRef("metal-" + symbol + "-k");
	if (!eta || !k) {
		return Complex(1.0, 0.0);
	}
	return Complex(evalSpectrumRefRaw(*eta, lambda), evalSpectrumRefRaw(*k, lambda));
}

}

const char* coatingKindName(CoatingKind kind)
{
	switch (kind) {
	case CoatingKind::Uncoated:
		return "uncoated";
	case CoatingKind::Ideal:
		return "ideal";
	case CoatingKind::Tabulated:
		return "tabulated";
	case CoatingKind::Stack:
		return "stack";
	case CoatingKind::Metal:
		return "metal";
	}
	return "uncoated";
}

std::optional<CoatingKind> findCoatingKind(std::string_view name)
{
	for (const CoatingKind kind : { CoatingKind::Uncoated, CoatingKind::Ideal, CoatingKind::Tabulated, CoatingKind::Stack, CoatingKind::Metal }) {
		if (name == coatingKindName(kind)) {
			return kind;
		}
	}
	return std::nullopt;
}

CoatingResponse transferMatrix(Complex n0In, const std::vector<ThinFilm>& layers, Complex nsIn, double lambda, double theta)
{
	// the incident medium is lossless: its absorption is the body's (Beer-Lambert), applied along the ray
	const double n0 = n0In.real();
	const double alpha = n0 * std::sin(theta);
	const double cos0 = std::cos(theta);
	std::vector<Complex> ys, yp, delta;
	for (const ThinFilm& layer : layers) {
		const Complex n = macleod(layer.index);
		const Complex q = tiltedIndex(n, alpha);
		ys.push_back(q);
		yp.push_back(n * n / q);
		delta.push_back(2.0 * PI * q * layer.thickness / (lambda * 1e-9));
	}
	const Complex ns = macleod(nsIn);
	const Complex qs = tiltedIndex(ns, alpha);
	CoatingResponse out;
	characteristic(Complex(n0 * cos0, 0.0), ys, delta, qs, out.rs, out.ts, out.Rs, out.Ts);
	characteristic(Complex(n0 / std::max(cos0, 1e-300), 0.0), yp, delta, ns * ns / qs, out.rp, out.tp, out.Rp, out.Tp);
	return out;
}

std::vector<ThinFilm> resolveLayers(const OpticCoating& coating, const IndexLookup& index, double lambda)
{
	std::vector<ThinFilm> films;
	for (const CoatingLayer& layer : coating.layers) {
		double thickness = layer.thickness;
		if (layer.quarterWaves > 0.0) {
			const double design = index(layer.material, coating.designWavelength).real();
			thickness = layer.quarterWaves * coating.designWavelength * 1e-9 / (4.0 * std::max(design, 1e-6));
		}
		films.push_back(ThinFilm { index(layer.material, lambda), thickness });
	}
	return films;
}

CoatingResponse evaluateCoating(const OpticCoating& coating, const IndexLookup& index, double lambda, double theta, Complex nPlus, Complex nMinus, bool fromPlus)
{
	const Complex nIncident = fromPlus ? nPlus : nMinus;
	const Complex nOther = fromPlus ? nMinus : nPlus;
	switch (coating.kind) {
	case CoatingKind::Ideal: {
		CoatingResponse out;
		out.Rs = coating.rs;
		out.Rp = coating.rp;
		out.Ts = coating.ts;
		out.Tp = coating.tp;
		out.rs = std::sqrt(coating.rs);
		out.rp = std::sqrt(coating.rp);
		out.ts = std::sqrt(coating.ts);
		out.tp = std::sqrt(coating.tp);
		return out;
	}
	case CoatingKind::Tabulated: {
		CoatingResponse out;
		if (coating.curves.empty()) {
			out.Ts = out.Tp = 1.0;
			return out;
		}
		// linear in wavelength within a curve, linear in angle between curves, the nearest curve outside them
		auto atCurve = [lambda](const CoatingCurve& curve, const std::vector<double>& values) {
			if (curve.lambda.empty() || values.empty()) {
				return 0.0;
			}
			const auto it = std::lower_bound(curve.lambda.begin(), curve.lambda.end(), lambda);
			if (it == curve.lambda.begin()) {
				return values.front();
			}
			if (it == curve.lambda.end()) {
				return values.back();
			}
			const size_t i = (size_t)(it - curve.lambda.begin());
			const double f = (lambda - curve.lambda[i - 1]) / (curve.lambda[i] - curve.lambda[i - 1]);
			return values[i - 1] + f * (values[i] - values[i - 1]);
		};
		size_t hi = 0;
		while (hi < coating.curves.size() && coating.curves[hi].angle < theta) {
			hi++;
		}
		const size_t lo = hi == 0 ? 0 : hi - 1;
		hi = std::min(hi, coating.curves.size() - 1);
		const CoatingCurve& a = coating.curves[lo];
		const CoatingCurve& b = coating.curves[hi];
		const double f = hi != lo && b.angle > a.angle ? std::clamp((theta - a.angle) / (b.angle - a.angle), 0.0, 1.0) : 0.0;
		auto blend = [&](const std::vector<double> CoatingCurve::*member) {
			return (1.0 - f) * atCurve(a, a.*member) + f * atCurve(b, b.*member);
		};
		out.Rs = std::clamp(blend(&CoatingCurve::rs), 0.0, 1.0);
		out.Rp = std::clamp(blend(&CoatingCurve::rp), 0.0, 1.0);
		out.Ts = std::clamp(blend(&CoatingCurve::ts), 0.0, 1.0 - out.Rs);
		out.Tp = std::clamp(blend(&CoatingCurve::tp), 0.0, 1.0 - out.Rp);
		out.rs = std::sqrt(out.Rs);
		out.rp = std::sqrt(out.Rp);
		out.ts = std::sqrt(out.Ts);
		out.tp = std::sqrt(out.Tp);
		return out;
	}
	case CoatingKind::Metal: {
		const Complex metal = metalIndex(coating.metal, lambda);
		if (!fromPlus) {
			// from behind, through the substrate straight onto the metal
			CoatingResponse out = transferMatrix(nIncident, {}, metal, lambda, theta);
			out.Ts = out.Tp = 0.0;
			out.ts = out.tp = 0.0;
			return out;
		}
		CoatingResponse out = transferMatrix(nIncident, resolveLayers(coating, index, lambda), metal, lambda, theta);
		out.Ts = out.Tp = 0.0;
		out.ts = out.tp = 0.0;
		return out;
	}
	case CoatingKind::Stack: {
		std::vector<ThinFilm> films = resolveLayers(coating, index, lambda);
		if (!fromPlus) {
			std::reverse(films.begin(), films.end());
		}
		return transferMatrix(nIncident, films, nOther, lambda, theta);
	}
	case CoatingKind::Uncoated:
		break;
	}
	return transferMatrix(nIncident, {}, nOther, lambda, theta);
}

namespace {

OpticCoating ideal(const char* key, const char* name, double rs, double rp, double ts, double tp, const char* source, bool polarizing = false)
{
	OpticCoating c;
	c.key = key;
	c.name = name;
	c.kind = CoatingKind::Ideal;
	c.builtin = true;
	c.rs = rs;
	c.rp = rp;
	c.ts = ts;
	c.tp = tp;
	c.source = source;
	c.polarizing = polarizing;
	return c;
}

OpticCoating stack(const char* key, const char* name, double design, std::vector<CoatingLayer> layers, const char* source)
{
	OpticCoating c;
	c.key = key;
	c.name = name;
	c.kind = CoatingKind::Stack;
	c.builtin = true;
	c.designWavelength = design;
	c.layers = std::move(layers);
	c.source = source;
	return c;
}

OpticCoating metal(const char* key, const char* name, const char* symbol, double design, std::vector<CoatingLayer> layers, const char* source)
{
	OpticCoating c = stack(key, name, design, std::move(layers), source);
	c.kind = CoatingKind::Metal;
	c.metal = symbol;
	return c;
}

CoatingLayer quarter(const char* material, double waves = 1.0)
{
	return CoatingLayer { material, 0.0, waves };
}

// a quarter-wave mirror of `pairs` (H L) pairs and a closing H, from the air side
std::vector<CoatingLayer> quarterWaveMirror(const char* high, const char* low, int pairs)
{
	std::vector<CoatingLayer> layers;
	for (int i = 0; i < pairs; i++) {
		layers.push_back(quarter(high));
		layers.push_back(quarter(low));
	}
	layers.push_back(quarter(high));
	return layers;
}

}

std::vector<OpticCoating> presetCoatings()
{
	std::vector<OpticCoating> out;
	out.push_back(ideal("uncoated", "Uncoated", 0, 0, 1, 1, "Exact Fresnel between the surface's two media"));
	out.back().kind = CoatingKind::Uncoated;

	// Thorlabs -E02, 400 to 750 nm: two quarter-wave TiO2/SiO2 mirrors stacked, centered at 470 and 650 nm, which covers
	// the band the published curve does at normal incidence (Thorlabs' real design is a chirped stack)
	{
		std::vector<CoatingLayer> layers = quarterWaveMirror("TIO2", "SIO2", 10);
		// the second mirror's layers at 650 / 470 of the first's optical thickness
		for (const CoatingLayer& layer : quarterWaveMirror("TIO2", "SIO2", 10)) {
			layers.push_back(CoatingLayer { layer.material, 0.0, 650.0 / 470.0 });
		}
		out.push_back(stack("thorlabs_E02", "Broadband dielectric -E02 (400-750 nm)", 470.0, std::move(layers),
			"Approximates Thorlabs' -E02 curve: two quarter-wave TiO2/SiO2 mirrors at 470 and 650 nm"));
	}
	out.push_back(metal("thorlabs_P01", "Protected silver -P01", "Ag", 550.0, { quarter("SIO2", 2.0) },
		"Approximates Thorlabs' -P01: silver under a half-wave SiO2 layer at 550 nm, absentee at its design wavelength"));
	out.push_back(metal("thorlabs_G01", "Protected gold -G01", "Au", 1000.0, { quarter("SIO2", 2.0) },
		"Approximates Thorlabs' -G01: gold under a half-wave SiO2 layer at 1000 nm"));
	out.push_back(metal("thorlabs_F01", "Protected aluminum -F01", "Al", 550.0, { quarter("SIO2", 2.0) },
		"Approximates Thorlabs' -F01: aluminum under a half-wave SiO2 layer at 550 nm"));
	// the -A, -B and -C broadband AR coatings: the classic quarter-half-quarter design (MgF2 / HfO2 / Al2O3 from the air
	// side), centered on each band's middle
	out.push_back(stack("thorlabs_A", "Broadband AR -A (350-700 nm)", 500.0, { quarter("MGF2"), quarter("HFO2", 2.0), quarter("AL2O3") },
		"Approximates Thorlabs' -A: quarter-half-quarter MgF2/HfO2/Al2O3 at 500 nm"));
	out.push_back(stack("thorlabs_B", "Broadband AR -B (650-1050 nm)", 830.0, { quarter("MGF2"), quarter("HFO2", 2.0), quarter("AL2O3") },
		"Approximates Thorlabs' -B: quarter-half-quarter MgF2/HfO2/Al2O3 at 830 nm"));
	out.push_back(stack("thorlabs_C", "Broadband AR -C (1050-1700 nm)", 1350.0, { quarter("MGF2"), quarter("HFO2", 2.0), quarter("AL2O3") },
		"Approximates Thorlabs' -C: quarter-half-quarter MgF2/HfO2/Al2O3 at 1350 nm"));
	out.push_back(stack("mgf2_single", "Single-layer MgF2 AR (550 nm)", 550.0, { quarter("MGF2") }, "One quarter-wave MgF2 layer at 550 nm"));
	// the BSW and BSN plate splitters: three quarter-wave layers (Ta2O5 / SiO2 / Ta2O5) reflect about half at their
	// design wavelength; designed for 45 degrees by centering a little long of the band
	out.push_back(stack("thorlabs_BSW", "Plate beamsplitter BSW (400-700 nm)", 600.0, quarterWaveMirror("TA2O5", "SIO2", 1),
		"Approximates Thorlabs' BSW 50:50 at 45 degrees: a quarter-wave HLH Ta2O5/SiO2 stack at 600 nm"));
	out.push_back(stack("thorlabs_BSN", "Plate beamsplitter BSN (700-1100 nm)", 950.0, quarterWaveMirror("TA2O5", "SIO2", 1),
		"Approximates Thorlabs' BSN 50:50 at 45 degrees: a quarter-wave HLH Ta2O5/SiO2 stack at 950 nm"));
	// a long-pass dichroic: a quarter-wave mirror centered below the edge reflects the short wavelengths and passes
	// the long ones
	{
		std::vector<CoatingLayer> layers;
		layers.push_back(quarter("SIO2", 0.5));
		for (int i = 0; i < 12; i++) {
			layers.push_back(quarter("TIO2"));
			layers.push_back(quarter("SIO2"));
		}
		layers.back().quarterWaves = 0.5;
		out.push_back(stack("dichroic_lp_567", "Long-pass dichroic (567 nm edge)", 480.0, std::move(layers),
			"Approximates Thorlabs' DMLP567 at 45 degrees: an (0.5L H 0.5L)^12 edge filter at 480 nm"));
	}
	out.push_back(ideal("ideal_50_50", "Ideal 50:50 splitter", 0.5, 0.5, 0.5, 0.5, "R = T = 0.5 at every wavelength, angle and polarization"));
	out.push_back(ideal("ideal_pbs", "Ideal polarizing splitter", 1.0, 0.0, 0.0, 1.0, "Reflects s, transmits p, at every wavelength and angle", true));
	out.push_back(ideal("ideal_mirror", "Ideal mirror", 1.0, 1.0, 0.0, 0.0, "R = 1"));
	out.push_back(ideal("ideal_ar", "Ideal AR", 0.0, 0.0, 1.0, 1.0, "R = 0, T = 1"));
	out.push_back(ideal("absorber", "Absorber", 0.0, 0.0, 0.0, 0.0, "R = T = 0: a beam dump"));
	return out;
}

std::optional<OpticCoating> readCoatingCsv(const std::string& text, bool transmission, double angle, std::string* error)
{
	std::vector<std::vector<double>> rows;
	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line)) {
		for (char& c : line) {
			if (c == ',' || c == ';' || c == '\t') {
				c = ' ';
			}
		}
		std::istringstream fields(line);
		std::vector<double> row;
		double value;
		while (fields >> value) {
			row.push_back(value);
		}
		// a header or a note line reads as no numbers
		if (row.size() >= 2 && fields.eof()) {
			rows.push_back(row);
		}
	}
	if (rows.size() < 2) {
		if (error != nullptr) {
			*error = "fewer than two rows of numbers";
		}
		return std::nullopt;
	}
	std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
	const size_t columns = rows.front().size();
	bool percent = false;
	for (const auto& row : rows) {
		for (size_t i = 1; i < row.size(); i++) {
			percent |= row[i] > 1.5;
		}
	}
	CoatingCurve curve;
	curve.angle = angle;
	for (const auto& row : rows) {
		if (row.size() < columns) {
			continue;
		}
		const double scale = percent ? 0.01 : 1.0;
		const double s = std::clamp(row[1] * scale, 0.0, 1.0);
		const double p = columns >= 3 ? std::clamp(row[2] * scale, 0.0, 1.0) : s;
		curve.lambda.push_back(row[0]);
		curve.rs.push_back(transmission ? 1.0 - s : s);
		curve.rp.push_back(transmission ? 1.0 - p : p);
		curve.ts.push_back(transmission ? s : 1.0 - s);
		curve.tp.push_back(transmission ? p : 1.0 - p);
	}
	OpticCoating coating;
	coating.kind = CoatingKind::Tabulated;
	coating.curves.push_back(std::move(curve));
	return coating;
}

namespace {

// whether a direction samples the transmitted cosine: the ray leaves the denser medium, and the coating can transmit
bool samplesTransmitted(const OpticCoating& coating, std::complex<double> nIncident, std::complex<double> nOther)
{
	return coating.kind != CoatingKind::Metal && nIncident.real() > nOther.real();
}

// the table's cosine at sample j: u = 1 - cos(pi j / 180), dense near u = 0, where the reflectance bends most (the
// critical angle seen from the denser side, grazing incidence from the other)
double sampleCosine(uint32_t j)
{
	return 1.0 - std::cos(0.5 * PI * (double)j / (double)(COATING_TABLE_ANGLES - 1));
}

// the incident angle of table sample j
double sampleAngle(uint32_t j, bool transmitted, double nIncident, double nOther)
{
	const double u = sampleCosine(j);
	if (!transmitted) {
		// a hair short of grazing, where the p admittance is singular
		return std::acos(std::max(u, 1e-5));
	}
	const double sinOther = std::sqrt(std::max(0.0, 1.0 - u * u));
	return std::asin(std::min(sinOther * nOther / nIncident, 1.0));
}

void fillDirection(size_t direction, bool transmitted, const OpticCoating& coating, const IndexLookup& index, const std::string& plusMedium,
	const std::string& minusMedium, const std::vector<double>& lambdas, std::vector<glm::dvec4>& out)
{
	out.assign(lambdas.size() * COATING_TABLE_ANGLES, glm::dvec4(0.0));
	for (size_t column = 0; column < lambdas.size(); column++) {
		const double lambda = lambdas[column];
		const std::complex<double> nPlus = index(plusMedium, lambda);
		const std::complex<double> nMinus = index(minusMedium, lambda);
		const std::complex<double> nIncident = direction == 0 ? nPlus : nMinus;
		const std::complex<double> nOther = direction == 0 ? nMinus : nPlus;
		for (uint32_t j = 0; j < COATING_TABLE_ANGLES; j++) {
			const double theta = sampleAngle(j, transmitted, nIncident.real(), nOther.real());
			const CoatingResponse r = evaluateCoating(coating, index, lambda, theta, nPlus, nMinus, direction == 0);
			out[column * COATING_TABLE_ANGLES + j] = glm::dvec4(r.Rs, r.Rp, r.Ts, r.Tp);
		}
	}
}

}

CoatingTable coatingTable(const OpticCoating& coating, const IndexLookup& index, const std::string& plusMedium, const std::string& minusMedium,
	const std::vector<float>& wavelengths)
{
	CoatingTable table;
	table.columns = (uint32_t)wavelengths.size();
	table.wavelengths = wavelengths;
	const std::vector<double> lambdas(wavelengths.begin(), wavelengths.end());
	const double middle = lambdas.empty() ? 550.0 : lambdas[lambdas.size() / 2];
	for (size_t direction = 0; direction < 2; direction++) {
		const std::complex<double> nIncident = index(direction == 0 ? plusMedium : minusMedium, middle);
		const std::complex<double> nOther = index(direction == 0 ? minusMedium : plusMedium, middle);
		const bool transmitted = samplesTransmitted(coating, nIncident, nOther);
		table.transmittedAxis[direction] = transmitted ? 1u : 0u;
		std::vector<glm::dvec4> values;
		fillDirection(direction, transmitted, coating, index, plusMedium, minusMedium, lambdas, values);
		for (const glm::dvec4& v : values) {
			table.data.push_back(glm::vec4(v));
		}
	}
	return table;
}

CoatingTable coatingTableRgb(const OpticCoating& coating, const IndexLookup& index, const std::string& plusMedium, const std::string& minusMedium,
	const SpectrumContext& context, double designWavelength)
{
	std::vector<double> lambdas;
	for (int lambda = 360; lambda <= 830; lambda += 5) {
		lambdas.push_back((double)lambda);
	}
	CoatingTable table;
	table.columns = 3;
	table.data.assign(2 * 3 * COATING_TABLE_ANGLES, glm::vec4(0.f));
	for (size_t direction = 0; direction < 2; direction++) {
		const std::complex<double> nIncident = index(direction == 0 ? plusMedium : minusMedium, designWavelength);
		const std::complex<double> nOther = index(direction == 0 ? minusMedium : plusMedium, designWavelength);
		const bool transmitted = samplesTransmitted(coating, nIncident, nOther);
		table.transmittedAxis[direction] = transmitted ? 1u : 0u;
		std::vector<glm::dvec4> values;
		// every wavelength at the design wavelength's angle for each sample, as exact RGB refracts at that index
		values.assign(lambdas.size() * COATING_TABLE_ANGLES, glm::dvec4(0.0));
		for (size_t column = 0; column < lambdas.size(); column++) {
			const std::complex<double> nPlus = index(plusMedium, lambdas[column]);
			const std::complex<double> nMinus = index(minusMedium, lambdas[column]);
			for (uint32_t j = 0; j < COATING_TABLE_ANGLES; j++) {
				const double theta = sampleAngle(j, transmitted, nIncident.real(), nOther.real());
				const CoatingResponse r = evaluateCoating(coating, index, lambdas[column], theta, nPlus, nMinus, direction == 0);
				values[column * COATING_TABLE_ANGLES + j] = glm::dvec4(r.Rs, r.Rp, r.Ts, r.Tp);
			}
		}
		for (uint32_t a = 0; a < COATING_TABLE_ANGLES; a++) {
			// each of the four powers as a reflectance spectrum, projected as exact RGB projects an albedo
			glm::vec3 rgb[4];
			for (int component = 0; component < 4; component++) {
				std::vector<float> samples;
				for (size_t column = 0; column < lambdas.size(); column++) {
					samples.push_back((float)lambdas[column]);
					samples.push_back((float)values[column * COATING_TABLE_ANGLES + a][component]);
				}
				rgb[component] = glm::clamp(exactRgb(tabulatedRef(std::move(samples)), SpectrumRole::Albedo, context), glm::vec3(0.f), glm::vec3(1.f));
			}
			for (int channel = 0; channel < 3; channel++) {
				table.data[(direction * 3 + (size_t)channel) * COATING_TABLE_ANGLES + a] = glm::vec4(rgb[0][channel], rgb[1][channel], rgb[2][channel], rgb[3][channel]);
			}
		}
	}
	return table;
}

glm::vec4 lookupCoatingTable(const CoatingTable& table, uint32_t direction, float column, float cosIncident, float eta)
{
	float u = cosIncident;
	if (table.transmittedAxis[direction] != 0u) {
		const float sin2 = eta * eta * (1.f - cosIncident * cosIncident);
		if (sin2 >= 1.f) {
			// past the critical angle: everything reflects
			return glm::vec4(1.f, 1.f, 0.f, 0.f);
		}
		u = std::sqrt(1.f - sin2);
	}
	// the inverse of sampleCosine()
	const float a = std::acos(1.f - std::clamp(u, 0.f, 1.f)) * (float)(2.0 / 3.14159265358979323846) * (float)(COATING_TABLE_ANGLES - 1);
	const uint32_t a0 = std::min((uint32_t)a, COATING_TABLE_ANGLES - 2);
	const float fa = a - (float)a0;
	const float c = std::clamp(column, 0.f, (float)(table.columns - 1));
	const uint32_t c0 = table.columns > 1 ? std::min((uint32_t)c, table.columns - 2) : 0u;
	const uint32_t c1 = table.columns > 1 ? c0 + 1u : 0u;
	const float fc = c - (float)c0;
	auto at = [&](uint32_t col, uint32_t sample) { return table.data[((size_t)direction * table.columns + col) * COATING_TABLE_ANGLES + sample]; };
	const glm::vec4 lo = at(c0, a0) + fa * (at(c0, a0 + 1u) - at(c0, a0));
	const glm::vec4 hi = at(c1, a0) + fa * (at(c1, a0 + 1u) - at(c1, a0));
	return lo + fc * (hi - lo);
}
