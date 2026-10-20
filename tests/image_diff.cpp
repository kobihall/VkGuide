// Compares two images (.exr or .pfm) of the same size: RMSE, relative MSE, MAPE and the mean
// difference, the measures the render panel reports against a reference. For the verification
// scripts, and for a render of this tracer against one of pbrt's.
//
// Run:  ./bin/image_diff <image> <reference> [--scale S]    (S multiplies the image first)

#include <image_io.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

int main(int argc, char** argv)
{
	if (argc < 3) {
		fmt::println("usage: image_diff <image> <reference> [--scale S]");
		return 2;
	}
	float scale = 1.f;
	for (int i = 3; i + 1 < argc; i++) {
		if (!std::strcmp(argv[i], "--scale")) {
			scale = (float)std::atof(argv[++i]);
		}
	}
	Image image;
	Image reference;
	const ImageIoStatus a = loadImage(argv[1], image);
	const ImageIoStatus b = loadImage(argv[2], reference);
	if (!a.ok || !b.ok) {
		fmt::println("{}", a.ok ? b.message : a.message);
		return 2;
	}
	for (float& v : image.rgb) {
		v *= scale;
	}
	ImageError error;
	if (!compareImages(image, reference, error)) {
		fmt::println("the images differ in size: {} x {} against {} x {}", image.width, image.height, reference.width, reference.height);
		return 2;
	}
	fmt::println("rmse {:.6g}  relative mse {:.6g}  mape {:.4f}  mean difference {:+.6g}  non-finite {}", error.rmse, error.relativeMse, error.mape, error.meanDifference, error.nonFinite);
	return 0;
}
