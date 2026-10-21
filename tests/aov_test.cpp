// CPU verification of the auxiliary outputs (src/aov.h, the line-for-line mirror of
// shaders/rt/include/crt_aov.glsl). No Vulkan, no GPU.
//
// What the outputs promise:
//  - the layers a render keeps follow its outputs, numbered in order, and pack into the word the shaders read
//  - a pixel of identical hit samples resolves to the sample: position, depth and uv exact, normals unit length,
//    albedo exact
//  - position, normals, uv and depth average over the samples that met a surface, and albedo over every
//    sample, so a pixel half on an object has the object's position and half its albedo (pbrt's
//    gBufferWeightSum and weightSum)
//  - a filter weight of -1 subtracts, as it does in the film
//  - the id is the first surface sample's and is never averaged
//  - the variance is the samples' own, Welford's, over n - 1, and matches a two-pass computation in double
//  - an export names its channels as pbrt's GBufferFilm does, holds the resolved values and each light group's
//    share over its weight sum, carries the metadata, and reads back from an EXR file unchanged
// --selfcheck proves the tests can fail: it plants an albedo divided by the wrong weight and a variance over n,
// and fails unless both are caught.
//
// Run:  ./bin/aov_test [--selfcheck]

#include <aov.h>
#include <render_export.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>

#include <fmt/format.h>

namespace {

int g_failures = 0;
bool g_quiet = false;

void check(bool ok, const std::string& what)
{
	if (!ok) {
		g_failures++;
		if (!g_quiet) {
			fmt::println("    FAIL: {}", what);
		}
	}
}

bool near(float a, float b, float tolerance = 1e-5f)
{
	return std::abs(a - b) <= tolerance * std::max(1.f, std::max(std::abs(a), std::abs(b)));
}

bool near3(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-5f)
{
	return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

using Resolver = std::function<AovResolved(const AovPixel&)>;

AovSample surface(const glm::vec3& position, const glm::vec3& normal, float albedo)
{
	AovSample sample;
	sample.hit = true;
	sample.position = position;
	sample.depth = glm::length(position);
	sample.normal = normal;
	sample.shadingNormal = normal;
	sample.uv = glm::vec2(position.x * 0.5f, position.y * 0.25f);
	sample.albedo = glm::vec3(albedo);
	sample.instance = 7;
	sample.material = 3;
	return sample;
}

//---------------------------------------------------------------- the layout

void testLayout()
{
	fmt::println("layout");
	AovSettings none;
	check(makeAovLayout(none).count == 0, "no outputs keep no layers");
	none.set(AovOutput::SampleCount, true);
	check(makeAovLayout(none).count == 0, "the sample count keeps no layers of its own");
	check(!none.accumulates(), "the sample count alone accumulates nothing");

	AovSettings normals;
	normals.set(AovOutput::Normal, true);
	AovLayout layout = makeAovLayout(normals);
	check(layout.count == 2, "normals keep the weights and the normal");
	check(layout.layer[(size_t)AovLayer::Weights] == 0 && layout.layer[(size_t)AovLayer::Normal] == 1, "layers are numbered in AovLayer order");
	check(!layout.has(AovLayer::Albedo) && layout.layer[(size_t)AovLayer::Albedo] == AOV_LAYER_NONE, "a layer nobody wants is absent");

	AovSettings depthOnly;
	depthOnly.set(AovOutput::Depth, true);
	check(makeAovLayout(depthOnly).has(AovLayer::PositionDepth), "depth alone keeps the position layer, which holds it");
	AovSettings both = depthOnly;
	both.set(AovOutput::Position, true);
	check(makeAovLayout(both).count == 2, "position and depth share one layer");

	AovSettings all;
	for (uint32_t i = 0; i < (uint32_t)AovOutput::Count; i++) {
		all.set((AovOutput)i, true);
	}
	layout = makeAovLayout(all);
	check(layout.count == AOV_LAYER_COUNT, "every output keeps every layer");
	const uint32_t packed = layout.pack();
	bool roundTrip = true;
	for (uint32_t i = 0; i < AOV_LAYER_COUNT; i++) {
		roundTrip = roundTrip && unpackAovLayer(packed, (AovLayer)i) == layout.layer[i];
	}
	check(roundTrip, "the packed word gives every layer back");
	check(unpackAovLayer(makeAovLayout(normals).pack(), AovLayer::Albedo) == AOV_LAYER_NONE, "an absent layer unpacks as none");

	AovSettings variance;
	variance.set(AovOutput::Variance, true);
	check(makeAovLayout(variance).count == 3, "variance keeps the weights, its mean and its M2");
	check(all.has(AovOutput::Ids) && !normals.has(AovOutput::Ids), "set and has agree");
}

//---------------------------------------------------------------- the estimators

void testConstant(const Resolver& resolve)
{
	fmt::println("identical samples");
	AovPixel pixel;
	const AovSample sample = surface(glm::vec3(1.f, 2.f, 3.f), glm::vec3(0.f, 0.f, 2.f), 0.5f);
	for (int i = 0; i < 16; i++) {
		pixel.add(sample, 1.f, glm::vec3(0.25f, 0.5f, 0.75f));
	}
	const AovResolved r = resolve(pixel);
	check(near3(r.position, sample.position), "position is the sample's");
	check(near(r.depth, sample.depth), "depth is the sample's");
	check(near3(r.normal, glm::vec3(0.f, 0.f, 1.f)), "a normal sums, then normalizes: unit length");
	check(near(r.uv.x, sample.uv.x) && near(r.uv.y, sample.uv.y), "uv is the sample's");
	check(near3(r.albedo, glm::vec3(0.5f)), "albedo is the sample's");
	check(r.instance == 7.f && r.material == 3.f, "the ids are the sample's");
	check(near3(r.variance, glm::vec3(0.f), 1e-6f), "samples that agree have no variance");
	check(near3(r.relativeVariance, glm::vec3(0.f), 1e-6f), "and no relative variance");
}

void testMisses(const Resolver& resolve)
{
	fmt::println("samples that missed");
	AovPixel pixel;
	const AovSample hit = surface(glm::vec3(1.f, 2.f, 3.f), glm::vec3(0.f, 1.f, 0.f), 0.5f);
	AovSample miss;
	for (int i = 0; i < 3; i++) {
		pixel.add(hit, 1.f, glm::vec3(1.f));
	}
	pixel.add(miss, 1.f, glm::vec3(0.f));
	const AovResolved r = resolve(pixel);
	check(near3(r.position, hit.position), "position averages over the samples that met a surface only");
	check(near(r.depth, hit.depth), "so does depth");
	check(near3(r.albedo, glm::vec3(0.375f)), "albedo averages over every sample: three of four are 0.5");

	AovPixel empty;
	for (int i = 0; i < 4; i++) {
		empty.add(miss, 1.f, glm::vec3(0.f));
	}
	const AovResolved none = resolve(empty);
	check(near3(none.position, glm::vec3(0.f)) && near3(none.normal, glm::vec3(0.f)) && near3(none.albedo, glm::vec3(0.f)), "a pixel that never met a surface is zero");
	check(none.instance == -1.f && none.material == -1.f, "and has no id");
}

void testWeights(const Resolver& resolve)
{
	fmt::println("filter weights");
	//a filter with a negative lobe: samples weigh +1 and -1, and the estimate is sum(w x) / sum(w)
	std::mt19937 random(5);
	std::uniform_real_distribution<float> uniform(0.f, 1.f);
	AovPixel pixel;
	double weightSum = 0.0;
	double surfaceSum = 0.0;
	glm::dvec3 position(0.0);
	double albedo = 0.0;
	for (int i = 0; i < 400; i++) {
		const float weight = uniform(random) < 0.2f ? -1.f : 1.f;
		AovSample sample;
		if (uniform(random) < 0.7f) {
			sample = surface(glm::vec3(uniform(random), uniform(random), uniform(random)), glm::vec3(0.f, 1.f, 0.f), uniform(random));
		}
		pixel.add(sample, weight, glm::vec3(uniform(random)));
		weightSum += weight;
		albedo += weight * sample.albedo.x;
		if (sample.hit) {
			surfaceSum += weight;
			position += double(weight) * glm::dvec3(sample.position);
		}
	}
	const AovResolved r = resolve(pixel);
	const glm::dvec3 expected = position / surfaceSum;
	check(std::abs(r.position.x - expected.x) < 1e-4 && std::abs(r.position.y - expected.y) < 1e-4 && std::abs(r.position.z - expected.z) < 1e-4, "position is sum(w P) / sum(w) over the surface samples, negative weights included");
	check(std::abs(r.albedo.x - albedo / weightSum) < 1e-4, "albedo is sum(w a) / sum(w) over every sample");
}

void testIds(const Resolver& resolve)
{
	fmt::println("ids");
	AovPixel pixel;
	AovSample miss;
	pixel.add(miss, 1.f, glm::vec3(0.f));
	AovSample first = surface(glm::vec3(1.f), glm::vec3(0.f, 1.f, 0.f), 1.f);
	first.instance = 0;
	first.material = 0;
	AovSample second = first;
	second.instance = 41;
	second.material = 9;
	pixel.add(first, 1.f, glm::vec3(0.f));
	pixel.add(second, 1.f, glm::vec3(0.f));
	const AovResolved r = resolve(pixel);
	check(r.instance == 0.f && r.material == 0.f, "the first surface sample's ids stand, id 0 included, and a miss before it does not count");

	check(aovIdColor(5) == aovIdColor(5), "an id's color is deterministic");
	check(aovIdColor(5) != aovIdColor(6), "neighboring ids differ");
	bool inRange = true;
	for (uint32_t id = 0; id < 4096; id++) {
		const glm::vec3 color = aovIdColor(id);
		inRange = inRange && glm::all(glm::greaterThanEqual(color, glm::vec3(0.25f - 1e-6f))) && glm::all(glm::lessThanEqual(color, glm::vec3(1.f + 1e-6f)));
	}
	check(inRange, "an id's color stays between 0.25 and 1, so no object is black");
}

void testNormals(const Resolver& resolve)
{
	fmt::println("normals");
	AovPixel pixel;
	pixel.add(surface(glm::vec3(0.f), glm::vec3(1.f, 0.f, 0.f), 0.f), 1.f, glm::vec3(0.f));
	pixel.add(surface(glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f), 0.f), 1.f, glm::vec3(0.f));
	const AovResolved r = resolve(pixel);
	check(near3(r.normal, glm::vec3(0.70710678f, 0.70710678f, 0.f)), "two normals at a right angle average to the bisector, normalized");
	check(near3(r.shadingNormal, r.normal), "the shading normal does the same");
}

void testVariance(const Resolver& resolve)
{
	fmt::println("variance");
	std::mt19937 random(11);
	std::uniform_real_distribution<float> uniform(0.f, 4.f);
	AovPixel pixel;
	const int n = 2000;
	std::vector<glm::vec3> values;
	for (int i = 0; i < n; i++) {
		const glm::vec3 value(uniform(random), uniform(random) * 0.1f, uniform(random) + 10.f);
		values.push_back(value);
		pixel.add(AovSample {}, 1.f, value);
	}
	glm::dvec3 mean(0.0);
	for (const glm::vec3& v : values) {
		mean += glm::dvec3(v);
	}
	mean /= n;
	glm::dvec3 squared(0.0);
	for (const glm::vec3& v : values) {
		const glm::dvec3 d = glm::dvec3(v) - mean;
		squared += d * d;
	}
	const glm::dvec3 variance = squared / double(n - 1);
	const AovResolved r = resolve(pixel);
	for (int c = 0; c < 3; c++) {
		check(std::abs(r.variance[c] - variance[c]) < 2e-4 * variance[c], fmt::format("channel {}: Welford's variance matches the two-pass one ({} against {})", c, r.variance[c], variance[c]));
		check(std::abs(r.relativeVariance[c] - variance[c] / mean[c]) < 2e-4 * variance[c] / mean[c], fmt::format("channel {}: the relative variance is the variance over the mean", c));
	}
	check(r.varianceSamples == float(n), "the sample count is kept");

	AovPixel single;
	single.add(AovSample {}, 1.f, glm::vec3(3.f));
	check(near3(resolve(single).variance, glm::vec3(0.f)), "one sample has no variance");
	check(near3(resolve(AovPixel {}).variance, glm::vec3(0.f)), "no samples have none either");
}

//---------------------------------------------------------------- the export

void testExport()
{
	fmt::println("export");
	constexpr uint32_t W = 4;
	constexpr uint32_t H = 3;
	constexpr uint32_t PIXELS = W * H;
	AovSettings settings;
	for (AovOutput output : { AovOutput::Normal, AovOutput::Albedo, AovOutput::Depth, AovOutput::Ids, AovOutput::SampleCount }) {
		settings.set(output, true);
	}
	RenderOutputsData data;
	data.width = W;
	data.height = H;
	data.aov = settings;
	data.layout = makeAovLayout(settings);
	data.samplesPerPixel = 5;
	data.aovLayers.assign((size_t)data.layout.count * PIXELS * 4, 0.f);
	data.samplesTaken.assign(PIXELS, 5);
	std::vector<AovPixel> pixels(PIXELS);
	for (uint32_t p = 0; p < PIXELS; p++) {
		for (int s = 0; s < 5; s++) {
			pixels[p].add(surface(glm::vec3((float)p, 1.f, 2.f), glm::vec3(0.f, 1.f, 0.f), 0.25f + 0.05f * (float)p), 1.f, glm::vec3(1.f));
		}
		for (uint32_t layer = 0; layer < AOV_LAYER_COUNT; layer++) {
			const uint32_t index = data.layout.layer[layer];
			if (index == AOV_LAYER_NONE) {
				continue;
			}
			std::memcpy(&data.aovLayers[((size_t)index * PIXELS + p) * 4], &pixels[p].layers[layer], sizeof(glm::vec4));
		}
	}
	//two light groups: weight sum 5 in every pixel, sums 2.5 and 5 for the first, 5 and 0 for the second
	data.lightGroups = 2;
	data.groupSums.assign((size_t)PIXELS * 7, 0.f);
	for (uint32_t p = 0; p < PIXELS; p++) {
		float* sums = &data.groupSums[(size_t)p * 7];
		sums[0] = 2.5f; sums[1] = 5.f; sums[2] = 0.f;
		sums[3] = 5.f; sums[4] = 0.f; sums[5] = 5.f;
		sums[6] = 5.f;
	}
	Image beauty;
	beauty.width = W;
	beauty.height = H;
	beauty.rgb.assign((size_t)PIXELS * 3, 0.5f);

	RenderExportInfo info;
	info.scenePath = "scenes/test.gltf";
	info.kernels = "integrator=path";
	info.settings = "{}";
	info.hasMse = true;
	info.mse = 0.125f;
	info.renderSeconds = 2.5f;
	info.lightGroupNames = { "Sky light", "key/lamp" };
	const ChannelImage exported = buildExportImage(beauty, data, info);

	for (const char* name : { "R", "G", "B", "Albedo.R", "Albedo.G", "Albedo.B", "Z", "N.X", "N.Y", "N.Z", "id.instance", "id.material", "n", "LightGroup.Sky_light.R", "LightGroup.key_lamp.B" }) {
		check(exported.find(name) != nullptr, fmt::format("the export has channel {}", name));
	}
	for (const char* name : { "P.X", "Ns.X", "u", "Variance.R" }) {
		check(exported.find(name) == nullptr, fmt::format("and not {}, an output the render did not keep", name));
	}
	const ImageChannel* albedo = exported.find("Albedo.R");
	const ImageChannel* group = exported.find("LightGroup.Sky_light.G");
	const ImageChannel* other = exported.find("LightGroup.key_lamp.R");
	if (albedo != nullptr && group != nullptr && other != nullptr) {
		check(near(albedo->values[3], 0.25f + 0.15f), "Albedo.R is the resolved albedo of its pixel");
		check(near(group->values[2], 1.f) && near(other->values[2], 1.f), "a light group is its sums over the weight sum: 5 / 5");
		check(near(exported.find("LightGroup.Sky_light.R")->values[0], 0.5f), "2.5 / 5 is 0.5");
	}
	check(exported.find("n") != nullptr && exported.find("n")->values[0] == 5.f, "n is the samples taken");
	check(exported.find("id.instance") != nullptr && exported.find("id.instance")->values[0] == 7.f, "id.instance is the first sample's");
	check(exportChannelName("a b/c.d") == "a_b_c_d" && exportChannelName("") == "group", "channel names keep letters, digits and underscores");

	//through a file: every channel and attribute comes back
	const std::filesystem::path path = std::filesystem::temp_directory_path() / "aov_test_export.exr";
	const ImageIoStatus saved = saveChannelImage(path, exported);
	check(saved.ok, "the file saves: " + saved.message);
	ChannelImage loaded;
	const ImageIoStatus read = loadChannelImage(path, loaded);
	check(read.ok && loaded.channels.size() == exported.channels.size(), "and loads with every channel");
	bool same = read.ok;
	for (const ImageChannel& channel : exported.channels) {
		const ImageChannel* back = loaded.find(channel.name);
		same = same && back != nullptr && back->values == channel.values;
	}
	check(same, "each channel reads back bit for bit");
	std::vector<std::string> attributes;
	for (const ImageAttribute& a : loaded.attributes) {
		attributes.push_back(a.name);
	}
	for (const char* name : { "renderTimeSeconds", "samplesPerPixel", "cameraFromWorld", "NDCFromWorld", "MSE", "chromaticities", "vkguide:scene", "vkguide:kernels", "vkguide:settings", "vkguide:lightGroup0" }) {
		check(std::find(attributes.begin(), attributes.end(), name) != attributes.end(), fmt::format("the header has {}", name));
	}
	for (const ImageAttribute& a : loaded.attributes) {
		if (a.name == "vkguide:scene") {
			check(a.asText() == "scenes/test.gltf", "a string attribute reads back");
		}
	}
	//half floats: the values come back to half's precision
	const ImageIoStatus half = saveChannelImage(path, exported, ExrOptions { true });
	ChannelImage halved;
	check(half.ok && loadChannelImage(path, halved).ok && halved.find("Albedo.R") != nullptr && std::abs(halved.find("Albedo.R")->values[3] - albedo->values[3]) < 1e-3f, "a half-float file reads back to half's precision");
	std::filesystem::remove(path);
}

//---------------------------------------------------------------- the planted errors

bool runEstimators(const Resolver& resolve)
{
	const int before = g_failures;
	testConstant(resolve);
	testMisses(resolve);
	testWeights(resolve);
	testIds(resolve);
	testNormals(resolve);
	testVariance(resolve);
	return g_failures == before;
}

bool selfcheck()
{
	fmt::println("selfcheck: planting errors");
	g_quiet = true;
	const int before = g_failures;

	//the albedo over the surface weight rather than the total: a pixel half on an object comes out too bright
	const bool albedoCaught = !runEstimators([](const AovPixel& pixel) {
		AovResolved r = resolveAov(pixel);
		const float surfaceWeight = pixel.layers[(size_t)AovLayer::Weights].x;
		const float totalWeight = pixel.layers[(size_t)AovLayer::Weights].y;
		if (surfaceWeight != 0.f) {
			r.albedo *= totalWeight / surfaceWeight;
		}
		return r;
	});
	//the variance over n instead of n - 1
	const bool varianceCaught = !runEstimators([](const AovPixel& pixel) {
		AovResolved r = resolveAov(pixel);
		const float n = pixel.layers[(size_t)AovLayer::VarianceMean].w;
		if (n > 1.f) {
			r.variance *= (n - 1.f) / n;
			r.relativeVariance *= (n - 1.f) / n;
		}
		return r;
	});
	g_quiet = false;
	g_failures = before;
	fmt::println("selfcheck: the albedo error was {}, the variance error was {}", albedoCaught ? "caught" : "NOT caught", varianceCaught ? "caught" : "NOT caught");
	return albedoCaught && varianceCaught;
}

}

int main(int argc, char** argv)
{
	bool selfcheckOnly = false;
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--selfcheck") == 0) {
			selfcheckOnly = true;
		} else {
			fmt::println("usage: aov_test [--selfcheck]");
			return 2;
		}
	}

	testLayout();
	testExport();
	runEstimators([](const AovPixel& pixel) { return resolveAov(pixel); });
	if (g_failures > 0) {
		fmt::println("aov_test: {} check(s) FAILED", g_failures);
		return 1;
	}
	if (selfcheckOnly && !selfcheck()) {
		fmt::println("aov_test: --selfcheck FAILED, a planted error was not caught");
		return 1;
	}
	fmt::println("aov_test: all checks passed");
	return 0;
}
