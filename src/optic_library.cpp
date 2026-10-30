#include <optic_library.h>

#include <algorithm>
#include <bit>
#include <cmath>

#include <fmt/format.h>

void OpticalLibrary::loadBundled(const std::filesystem::path& root, std::vector<std::string>& warnings)
{
	m_catalogs.clear();
	GlassCatalog builtin;
	builtin.name = CATALOG_BUILTIN;
	builtin.glasses = { airGlass(), vacuumGlass() };
	m_catalogs.push_back(std::move(builtin));

	std::string error;
	std::optional<GlassCatalog> bundled = readAgfFile(root / "assets/glass/bundled.agf", warnings, &error);
	if (bundled) {
		bundled->name = CATALOG_BUNDLED;
		for (OpticGlass& glass : bundled->glasses) {
			glass.catalog = CATALOG_BUNDLED;
		}
		m_catalogs.push_back(std::move(*bundled));
	} else {
		warnings.push_back(fmt::format("the bundled glass catalog did not load: {}", error));
		GlassCatalog empty;
		empty.name = CATALOG_BUNDLED;
		m_catalogs.push_back(std::move(empty));
	}
	GlassCatalog user;
	user.name = CATALOG_USER;
	m_catalogs.push_back(std::move(user));

	m_coatings = presetCoatings();
	m_revision++;
}

bool OpticalLibrary::loadCatalog(const std::filesystem::path& path, std::vector<std::string>& warnings, std::string* error)
{
	std::optional<GlassCatalog> catalog = readAgfFile(path, warnings, error);
	if (!catalog) {
		return false;
	}
	std::erase_if(m_catalogs, [&](const GlassCatalog& c) { return !c.path.empty() && c.path == path && c.name != CATALOG_BUNDLED; });
	for (const OpticGlass& glass : catalog->glasses) {
		const OpticGlass* existing = findGlass(glass.name);
		if (existing != nullptr) {
			warnings.push_back(fmt::format("{} is already in the {} catalog, which keeps it", glass.name, existing->catalog));
		}
	}
	m_catalogs.push_back(std::move(*catalog));
	m_revision++;
	return true;
}

std::vector<std::filesystem::path> OpticalLibrary::loadedCatalogPaths() const
{
	std::vector<std::filesystem::path> paths;
	for (const GlassCatalog& catalog : m_catalogs) {
		if (catalog.name != CATALOG_BUILTIN && catalog.name != CATALOG_BUNDLED && catalog.name != CATALOG_USER) {
			paths.push_back(catalog.path);
		}
	}
	return paths;
}

void OpticalLibrary::clearScene()
{
	std::erase_if(m_catalogs, [](const GlassCatalog& c) { return c.name != CATALOG_BUILTIN && c.name != CATALOG_BUNDLED && c.name != CATALOG_USER; });
	userCatalog().glasses.clear();
	std::erase_if(m_coatings, [](const OpticCoating& c) { return !c.builtin; });
	m_revision++;
}

const OpticGlass* OpticalLibrary::findGlass(std::string_view name) const
{
	for (const GlassCatalog& catalog : m_catalogs) {
		for (const OpticGlass& glass : catalog.glasses) {
			if (glass.name == name) {
				return &glass;
			}
		}
	}
	return nullptr;
}

const OpticCoating* OpticalLibrary::findCoating(std::string_view key) const
{
	for (const OpticCoating& coating : m_coatings) {
		if (coating.key == key) {
			return &coating;
		}
	}
	return nullptr;
}

OpticCoating* OpticalLibrary::findCoating(std::string_view key)
{
	return const_cast<OpticCoating*>(static_cast<const OpticalLibrary*>(this)->findCoating(key));
}

GlassCatalog& OpticalLibrary::userCatalog()
{
	for (GlassCatalog& catalog : m_catalogs) {
		if (catalog.name == CATALOG_USER) {
			return catalog;
		}
	}
	GlassCatalog user;
	user.name = CATALOG_USER;
	m_catalogs.push_back(std::move(user));
	return m_catalogs.back();
}

void OpticalLibrary::setUserGlass(OpticGlass glass)
{
	glass.catalog = CATALOG_USER;
	std::vector<OpticGlass>& glasses = userCatalog().glasses;
	const auto found = std::find_if(glasses.begin(), glasses.end(), [&](const OpticGlass& g) { return g.name == glass.name; });
	if (found != glasses.end()) {
		*found = std::move(glass);
	} else {
		glasses.push_back(std::move(glass));
	}
	m_revision++;
}

void OpticalLibrary::setUserCoating(OpticCoating coating)
{
	coating.builtin = false;
	const auto found = std::find_if(m_coatings.begin(), m_coatings.end(), [&](const OpticCoating& c) { return c.key == coating.key; });
	if (found != m_coatings.end()) {
		if (found->builtin) {
			return;
		}
		*found = std::move(coating);
	} else {
		m_coatings.push_back(std::move(coating));
	}
	m_revision++;
}

bool OpticalLibrary::removeUserGlass(std::string_view name)
{
	const size_t erased = std::erase_if(userCatalog().glasses, [&](const OpticGlass& g) { return g.name == name; });
	m_revision += erased;
	return erased > 0;
}

bool OpticalLibrary::removeUserCoating(std::string_view key)
{
	const size_t erased = std::erase_if(m_coatings, [&](const OpticCoating& c) { return !c.builtin && c.key == key; });
	m_revision += erased;
	return erased > 0;
}

std::vector<OpticGlass> OpticalLibrary::userGlasses() const
{
	for (const GlassCatalog& catalog : m_catalogs) {
		if (catalog.name == CATALOG_USER) {
			return catalog.glasses;
		}
	}
	return {};
}

std::vector<OpticCoating> OpticalLibrary::userCoatings() const
{
	std::vector<OpticCoating> out;
	for (const OpticCoating& coating : m_coatings) {
		if (!coating.builtin) {
			out.push_back(coating);
		}
	}
	return out;
}

std::string OpticalLibrary::uniqueCoatingKey(std::string_view base) const
{
	std::string key(base);
	for (int n = 2; findCoating(key) != nullptr; n++) {
		key = fmt::format("{}_{}", base, n);
	}
	return key;
}

std::string OpticalLibrary::uniqueGlassName(std::string_view base) const
{
	std::string name(base);
	for (int n = 2; findGlass(name) != nullptr; n++) {
		name = fmt::format("{}_{}", base, n);
	}
	return name;
}

std::complex<double> OpticalLibrary::complexIndex(std::string_view name, double lambda) const
{
	const OpticGlass* glass = findGlass(name);
	if (glass == nullptr) {
		return { 1.0, 0.0 };
	}
	// alpha = 4 pi k / lambda
	const double k = glassAbsorption(*glass, lambda) * lambda * 1e-9 / (4.0 * 3.14159265358979323846);
	return { glassIndex(*glass, lambda), k };
}

IndexLookup OpticalLibrary::indexLookup() const
{
	return [this](std::string_view name, double lambda) { return complexIndex(name, lambda); };
}

namespace {

float floatOf(uint32_t bits)
{
	return std::bit_cast<float>(bits);
}

// crt_coating.glsl CRT_COATING_*
constexpr uint32_t COATING_TRANSMITTED_FROM_PLUS = 1u;
constexpr uint32_t COATING_TRANSMITTED_FROM_MINUS = 2u;
constexpr uint32_t COATING_RGB = 4u;
constexpr uint32_t COATING_LAYERS = 8u;
constexpr uint32_t COATING_OPAQUE = 16u;
constexpr double RGB_LAMBDAS[3] = { 630.0, 532.0, 465.0 };

}

uint32_t OpticalLibrary::packCoatingRecord(const OpticCoating& coating, const CoatingTable& table, float lambdaMin, float lambdaStep, bool rgb,
	const std::function<uint32_t(const SpectrumRef&)>& addSpectrum, std::vector<glm::vec4>& out) const
{
	const uint32_t header = (uint32_t)out.size();
	const bool layered = coating.kind == CoatingKind::Stack || coating.kind == CoatingKind::Metal;
	const bool opaque = coating.kind == CoatingKind::Metal || (coating.kind == CoatingKind::Ideal && coating.ts == 0.0 && coating.tp == 0.0);
	const uint32_t flags = (table.transmittedAxis[0] != 0u ? COATING_TRANSMITTED_FROM_PLUS : 0u) | (table.transmittedAxis[1] != 0u ? COATING_TRANSMITTED_FROM_MINUS : 0u)
		| (rgb ? COATING_RGB : 0u) | (layered ? COATING_LAYERS : 0u) | (opaque ? COATING_OPAQUE : 0u);
	const uint32_t kind = coating.kind == CoatingKind::Metal ? 2u : (coating.kind == CoatingKind::Stack ? 1u : 0u);
	out.push_back(glm::vec4(floatOf(table.columns), floatOf(flags), floatOf(layered ? (uint32_t)coating.layers.size() : 0u), floatOf(kind)));
	out.push_back(glm::vec4(lambdaMin, lambdaStep, (float)coating.designWavelength, 0.f));
	out.insert(out.end(), table.data.begin(), table.data.end());
	if (!layered) {
		return header;
	}
	// for Exact coatings: each layer's thickness, n and k at red, green and blue, and its spectra; a metal last
	const IndexLookup index = indexLookup();
	const std::vector<ThinFilm> films = resolveLayers(coating, index, coating.designWavelength);
	const OpticGlass fallback = vacuumGlass();
	for (size_t i = 0; i < coating.layers.size(); i++) {
		const std::string& material = coating.layers[i].material;
		glm::vec3 n;
		glm::vec3 k;
		for (int c = 0; c < 3; c++) {
			const std::complex<double> value = index(material, RGB_LAMBDAS[c]);
			n[c] = (float)value.real();
			k[c] = (float)value.imag();
		}
		const OpticGlass* glass = findGlass(material);
		const OpticGlass& g = glass != nullptr ? *glass : fallback;
		out.push_back(glm::vec4((float)films[i].thickness, n));
		out.push_back(glm::vec4(k, 0.f));
		out.push_back(glm::vec4(floatOf(addSpectrum(g.index)), floatOf(addSpectrum(g.absorption)), 0.f, 0.f));
	}
	if (coating.kind == CoatingKind::Metal) {
		const SpectrumRef eta = namedRef("metal-" + coating.metal + "-eta").value_or(constantRef(1.f));
		const SpectrumRef kRef = namedRef("metal-" + coating.metal + "-k").value_or(constantRef(0.f));
		glm::vec3 n;
		glm::vec3 k;
		for (int c = 0; c < 3; c++) {
			n[c] = (float)evalSpectrumRefRaw(eta, RGB_LAMBDAS[c]);
			k[c] = (float)evalSpectrumRefRaw(kRef, RGB_LAMBDAS[c]);
		}
		out.push_back(glm::vec4(0.f, n));
		out.push_back(glm::vec4(k, 0.f));
		out.push_back(glm::vec4(floatOf(addSpectrum(eta)), floatOf(addSpectrum(kRef)), 0.f, 0.f));
	}
	return header;
}
