#pragma once

// Small imgui pieces shared by the file-loading UI (the "File" menu popups and the raytracer
// scene panel's save/load controls), so every status line looks and behaves the same.

#include <imgui.h>

#include <vk_types.h>

// a coloured status line for the last file operation: green on success, red on failure. It
// stays until the next operation replaces it, so it cannot be missed the way a timed toast can
inline void drawIoResultLine(const IoResult& result)
{
	if (result.message.empty()) {
		return;
	}

	ImGui::PushStyleColor(ImGuiCol_Text, result.ok ? ImVec4(0.55f, 0.9f, 0.55f, 1.f) : ImVec4(1.f, 0.45f, 0.45f, 1.f));
	ImGui::TextWrapped("%s", result.message.c_str());
	ImGui::PopStyleColor();
}
