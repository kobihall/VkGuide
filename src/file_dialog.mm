#include <file_dialog.h>

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace {

NSArray<UTType*>* contentTypes(const std::vector<std::string>& extensions)
{
	NSMutableArray<UTType*>* types = [NSMutableArray array];
	for (const std::string& extension : extensions) {
		//unregistered extensions (glTF has no system type) come back as dynamic types, which
		//still filter correctly
		UTType* type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:extension.c_str()]];
		if (type != nil) {
			[types addObject:type];
		}
	}
	return types;
}

NSURL* directoryUrl(const std::filesystem::path& directory)
{
	if (directory.empty()) {
		return nil;
	}
	return [NSURL fileURLWithPath:[NSString stringWithUTF8String:directory.string().c_str()] isDirectory:YES];
}

bool hasAllowedExtension(const std::filesystem::path& path, const std::vector<std::string>& extensions)
{
	std::string ext = path.extension().string();
	if (!ext.empty() && ext[0] == '.') {
		ext.erase(0, 1);
	}
	for (const std::string& allowed : extensions) {
		if (ext == allowed) {
			return true;
		}
	}
	return false;
}

}

std::optional<std::filesystem::path> showOpenFileDialog(const std::string& title, const std::vector<std::string>& extensions, const std::filesystem::path& directory)
{
	@autoreleasepool {
		NSOpenPanel* panel = [NSOpenPanel openPanel];
		panel.title = [NSString stringWithUTF8String:title.c_str()];
		panel.message = panel.title;
		panel.canChooseFiles = YES;
		panel.canChooseDirectories = NO;
		panel.allowsMultipleSelection = NO;
		if (!extensions.empty()) {
			panel.allowedContentTypes = contentTypes(extensions);
		}
		if (NSURL* url = directoryUrl(directory)) {
			panel.directoryURL = url;
		}

		if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
			return std::nullopt;
		}
		return std::filesystem::path(panel.URL.path.UTF8String);
	}
}

std::optional<std::filesystem::path> showSaveFileDialog(const std::string& title, const std::vector<std::string>& extensions, const std::filesystem::path& directory, const std::string& defaultFileName)
{
	@autoreleasepool {
		NSSavePanel* panel = [NSSavePanel savePanel];
		panel.title = [NSString stringWithUTF8String:title.c_str()];
		panel.message = panel.title;
		panel.canCreateDirectories = YES;
		if (!extensions.empty()) {
			panel.allowedContentTypes = contentTypes(extensions);
		}
		if (NSURL* url = directoryUrl(directory)) {
			panel.directoryURL = url;
		}
		if (!defaultFileName.empty()) {
			panel.nameFieldStringValue = [NSString stringWithUTF8String:defaultFileName.c_str()];
		}

		if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
			return std::nullopt;
		}

		std::filesystem::path chosen(panel.URL.path.UTF8String);
		if (!extensions.empty() && !hasAllowedExtension(chosen, extensions)) {
			chosen += "." + extensions.front();
		}
		return chosen;
	}
}
