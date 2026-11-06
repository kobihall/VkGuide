#pragma once

// THE DETECTORS' READINGS (docs/plans/completed/physoptics-detectors.md), shown where the render's picture is: a detector
// is a sensor, a render target as a camera is, and the Raytraced Output window adds these to its picture while it shows
// one (RaytraceRenderer::drawOutputWindow()). The readings are the render's (src/rt_detectors.h); this file only shows them.

#include <cstdint>
#include <string>

#include <rt_detectors.h>

class VulkanEngine;
struct SceneOptic;

// the display settings of a detector's picture, above it
void drawDetectorPictureSettings(DetectorFilms::Entry& entry);
// below the picture: the power, its error and the paths; the centroid and widths, peak, encircled power and profiles of a
// detector with an image; a spectrometer's spectrum. `hoverPixel` is the film pixel under the cursor, or -1
void drawDetectorReadout(VulkanEngine* engine, const DetectorFilms::Entry& entry, int hoverX, int hoverY);
// a detector's element panel: make it the render target
void drawDetectorTargetButton(VulkanEngine* engine, const SceneOptic& optic);

// "4.997 mW", "12.3 uW", in the unit that keeps the number between 1 and 1000; per nm for a monochromatic reading
std::string formatPower(double watts, bool perNanometer = false);
