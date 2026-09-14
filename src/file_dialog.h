#pragma once

// Native file dialogs. On macOS these are NSOpenPanel/NSSavePanel (file_dialog.mm); elsewhere
// the stub (file_dialog_stub.cpp) returns nothing, since this project only targets MoltenVK.
//
// Both run a modal loop on the calling (main) thread and return once the user picks or cancels,
// so call them between imgui frames rather than in the middle of building one.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// extensions are bare, e.g. {"gltf", "glb"}; empty means any file. Returns nothing on cancel
std::optional<std::filesystem::path> showOpenFileDialog(const std::string& title, const std::vector<std::string>& extensions, const std::filesystem::path& directory);

// the returned path always carries one of the given extensions (the first is appended if the
// user typed none). Returns nothing on cancel
std::optional<std::filesystem::path> showSaveFileDialog(const std::string& title, const std::vector<std::string>& extensions, const std::filesystem::path& directory, const std::string& defaultFileName);
