#pragma once

// GPU SELF-TESTS: checks that need the Vulkan device, because they compare a shader with the C++
// mirror of it (docs/plans/completed/pbrt-verification-harness.md). The CPU mirrors guarantee
// nothing if the GLSL drifts from them; these run the GLSL on a fixed batch and compare.
//
// **Windows** -> **GPU self-tests** runs them and lists the results. Setting VKGUIDE_SELFTEST=1 in the
// environment runs them once at startup instead, prints the results and exits with 0 if they all
// passed and 1 if not, for a script.
//
// Adding one is a function and a row in gpuSelfTests() (gpu_selftest.cpp). The test's shader lives in
// shaders/test/, which the build compiles like any other.

#include <functional>
#include <span>
#include <string>
#include <vector>

class VulkanEngine;

struct SelfTestResult {
	bool passed { false };
	// the first line is the summary; the rest are detail, shown always
	std::vector<std::string> lines;
};

struct SelfTest {
	const char* name;
	std::function<SelfTestResult(VulkanEngine*)> run;
};

std::span<const SelfTest> gpuSelfTests();

// The **Windows** menu item and the results window
class GpuSelfTestPanel {
public:
	// runs every test now, on the calling thread, and keeps the results
	void runAll(VulkanEngine* engine);
	bool allPassed() const;
	// prints the results to stdout
	void print() const;

	bool* visibilityFlag() { return &m_visible; }
	void drawWindow(VulkanEngine* engine);

private:
	struct Entry {
		std::string name;
		SelfTestResult result;
	};
	std::vector<Entry> m_entries;
	bool m_visible { false };
	bool m_ran { false };
};
