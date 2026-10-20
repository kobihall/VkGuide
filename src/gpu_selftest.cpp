#include <gpu_selftest.h>

#include <cstdio>

#include <imgui.h>

#include <vk_engine.h>

SelfTestResult runBsdfParityTest(VulkanEngine* engine);

std::span<const SelfTest> gpuSelfTests()
{
	static const SelfTest tests[] = {
		{ "BSDF parity (crt_scatter.glsl against src/bsdf.cpp)", runBsdfParityTest },
	};
	return tests;
}

void GpuSelfTestPanel::runAll(VulkanEngine* engine)
{
	m_entries.clear();
	for (const SelfTest& test : gpuSelfTests()) {
		m_entries.push_back({ test.name, test.run(engine) });
	}
	m_ran = true;
}

bool GpuSelfTestPanel::allPassed() const
{
	for (const Entry& entry : m_entries) {
		if (!entry.result.passed) {
			return false;
		}
	}
	return m_ran;
}

void GpuSelfTestPanel::print() const
{
	for (const Entry& entry : m_entries) {
		std::printf("%s: %s\n", entry.result.passed ? "PASS" : "FAIL", entry.name.c_str());
		for (const std::string& line : entry.result.lines) {
			std::printf("    %s\n", line.c_str());
		}
	}
	std::fflush(stdout);
}

void GpuSelfTestPanel::drawWindow(VulkanEngine* engine)
{
	if (!m_visible) {
		return;
	}
	if (ImGui::Begin("GPU self-tests", &m_visible)) {
		if (ImGui::Button("Run")) {
			runAll(engine);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Runs shaders on a fixed batch and compares them with their C++ mirrors.\nThe render pauses for the length of the run.");
		}
		if (!m_ran) {
			ImGui::TextDisabled("Not run yet");
		}
		for (const Entry& entry : m_entries) {
			ImGui::PushStyleColor(ImGuiCol_Text, entry.result.passed ? ImVec4(0.4f, 0.9f, 0.4f, 1.f) : ImVec4(1.f, 0.4f, 0.3f, 1.f));
			ImGui::TextUnformatted(entry.result.passed ? "PASS" : "FAIL");
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextWrapped("%s", entry.name.c_str());
			for (const std::string& line : entry.result.lines) {
				ImGui::TextWrapped("    %s", line.c_str());
			}
		}
	}
	ImGui::End();
}
