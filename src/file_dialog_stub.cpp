#include <file_dialog.h>

#include <fmt/format.h>

//this project only targets macOS; on any other platform the dialogs simply report that

std::optional<std::filesystem::path> showOpenFileDialog(const std::string& title, const std::vector<std::string>&, const std::filesystem::path&)
{
	fmt::println("{}: native file dialogs are only implemented for macOS", title);
	return std::nullopt;
}

std::optional<std::filesystem::path> showSaveFileDialog(const std::string& title, const std::vector<std::string>&, const std::filesystem::path&, const std::string&)
{
	fmt::println("{}: native file dialogs are only implemented for macOS", title);
	return std::nullopt;
}
