#include <vk_engine.h>

#include <command_line.h>

int main(int argc, char* argv[])
{
	CommandLine commandLine;
	std::string error;
	if (!parseCommandLine(argc, argv, commandLine, error)) {
		fmt::println(stderr, "vulkan_guide: {}\n", error);
		fmt::println(stderr, "{}", commandLineUsage());
		return 2;
	}
	if (commandLine.help) {
		fmt::println("{}", commandLineUsage());
		return 0;
	}

	VulkanEngine engine;
	engine.setCommandLine(std::move(commandLine));

	engine.init();

	engine.run();

	engine.cleanup();

	return engine.exitCode();
}
