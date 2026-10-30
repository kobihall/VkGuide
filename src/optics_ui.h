#pragma once

// The optical library's UI (docs/plans/completed/physoptics-optical-materials.md): the coating and glass combos the material and
// element panels use, the Glasses and Coatings folders of the Materials window with their entries' details, the Optics
// section of the Render Settings window, and the Optics menu. The Optical bench capability registers the folders, the
// section and the menu (src/scene_capabilities.cpp); the combos work in any panel.

#include <string>

#include <rt_scene_types.h>

class OpticalLibrary;
class VulkanEngine;

// the library the combos read, set once by the engine
void setOpticsUiLibrary(OpticalLibrary* library);

// a coating by key; true when the choice changed
bool drawCoatingCombo(const char* label, std::string& key);
// a glass by name, from every catalog; with `allowDefault` an empty name ("the kind's default") is offered too
bool drawGlassCombo(const char* label, std::string& name, bool allowDefault);

// the surrounding medium as the library's glass name
const char* surroundingGlass(SurroundingMedium medium);

// the Materials window's Glasses, Coatings and Optics folders, and the details of a glass or coating they selected
void drawOpticalLibraryFolders(VulkanEngine* engine);
bool drawOpticalLibraryDetails(VulkanEngine* engine);

// Render Settings -> Optics
void drawOpticsRenderSettings(VulkanEngine* engine);
// the Optics menu: Load glass catalog..., Import coating curve...
void drawOpticsMenu(VulkanEngine* engine);
