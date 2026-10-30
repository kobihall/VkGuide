#pragma once

// THE OPTICAL LIBRARY (docs/plans/completed/physoptics-optical-materials.md), part of rt_optics: the glasses an optical body's
// media are made of and the coatings its surfaces carry, beside the material library rather than in it, since a
// coating's layer stack does not fit a material's parameter set. Glasses come in catalogs: the built-in surrounding
// media (air and vacuum), the bundled catalog (assets/glass/bundled.agf), any AGF file the user loads, and the user's
// own glasses. Coatings are the bundled presets and the user's. Bundled entries are read-only and duplicable.
//
// A glass is named by its catalog name and a coating by its key, everywhere: an element's parameters, a material's
// coating, a coating's layers and the scene file. The first catalog to hold a name wins, so a loaded catalog cannot
// silently change what a bundled glass name means.

#include <complex>
#include <functional>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <optic_coating.h>
#include <optic_glass.h>

inline constexpr const char* CATALOG_BUILTIN = "builtin";
inline constexpr const char* CATALOG_BUNDLED = "bundled";
inline constexpr const char* CATALOG_USER = "user";

class OpticalLibrary {
public:
	// the built-in media, the bundled catalog from `root`/assets/glass/bundled.agf and the presets; what it could not
	// read goes to `warnings`
	void loadBundled(const std::filesystem::path& root, std::vector<std::string>& warnings);

	// another AGF catalog, added after the others (replacing one already loaded from the same path)
	bool loadCatalog(const std::filesystem::path& path, std::vector<std::string>& warnings, std::string* error);
	// the catalogs the user loaded (not builtin, bundled or user), and forgetting all of them
	std::vector<std::filesystem::path> loadedCatalogPaths() const;
	void clearScene();

	const std::vector<GlassCatalog>& catalogs() const { return m_catalogs; }
	const std::vector<OpticCoating>& coatings() const { return m_coatings; }

	const OpticGlass* findGlass(std::string_view name) const;
	const OpticCoating* findCoating(std::string_view key) const;
	OpticCoating* findCoating(std::string_view key);
	// the user's own entries: added, replaced by name or key, removed
	void setUserGlass(OpticGlass glass);
	void setUserCoating(OpticCoating coating);
	bool removeUserGlass(std::string_view name);
	bool removeUserCoating(std::string_view key);
	std::vector<OpticGlass> userGlasses() const;
	std::vector<OpticCoating> userCoatings() const;
	// a key or name no entry has, from `base`
	std::string uniqueCoatingKey(std::string_view base) const;
	std::string uniqueGlassName(std::string_view base) const;

	// the complex index n + ik of a glass at lambda nm (absorption alpha becomes k = alpha lambda / 4 pi); 1 for an
	// unknown name. What the transfer matrix reads its layers and media through
	std::complex<double> complexIndex(std::string_view glass, double lambda) const;
	IndexLookup indexLookup() const;

	// A coating's record in binding 56 (shaders/rt/include/crt_coating.glsl), appended to `out`: the header, the table,
	// and for a stack or a metal its layers for Exact coatings. `addSpectrum` interns a spectrum the spectral builds read
	// (a layer's index and absorption, a metal's eta and k) and returns its SceneSpectra index. Returns the header's offset
	uint32_t packCoatingRecord(const OpticCoating& coating, const CoatingTable& table, float lambdaMin, float lambdaStep, bool rgb,
		const std::function<uint32_t(const SpectrumRef&)>& addSpectrum, std::vector<glm::vec4>& out) const;

	// moves with every change, so the scene's acceleration structure knows to repack
	uint64_t revision() const { return m_revision; }

private:
	GlassCatalog& userCatalog();
	std::vector<GlassCatalog> m_catalogs;
	std::vector<OpticCoating> m_coatings;
	uint64_t m_revision { 1 };
};
