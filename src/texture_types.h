#pragma once

// How a texture of the texture library (src/texture_library.h) is read, as plain data with no Vulkan, so the material
// model (src/rt_material.h) and the importers can name it

#include <compare>
#include <cstdint>

// how a texture's 8-bit texels are read: sRGB-encoded color, or data (normals, roughness, coverage) taken as they are.
// A float image is always linear
enum class TextureColorSpace : uint8_t {
	Srgb,
	Linear
};

// the sampler's addressing, as glTF's and pbrt's wrap modes name it. Black is pbrt's "black": zero outside [0, 1]
enum class TextureWrap : uint8_t {
	Repeat,
	Clamp,
	Mirror,
	Black
};

struct TextureSampling {
	TextureWrap wrapU { TextureWrap::Repeat };
	TextureWrap wrapV { TextureWrap::Repeat };
	// glTF's magFilter and minFilter NEAREST; linear otherwise. Mip levels are always blended linearly
	bool nearestMag { false };
	bool nearestMin { false };

	auto operator<=>(const TextureSampling&) const = default;
};

// how an 8-bit file encodes its values, for a pbrt imagemap's "encoding" (pbrt-v4 ColorEncoding)
enum class TextureEncoding : uint8_t {
	Srgb,
	Linear,
	Gamma
};

