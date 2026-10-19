// tools/asset_viewer/MaterialTextureLoader.h
#pragma once
#include <cstdint>
#include <string>

enum class MaterialSlotKind { BaseColor, Normal, Orm, Emissive };

// Decodes an image file (KTX2 via RenderCore; PNG/JPG/BMP/TGA via stb_image) and uploads it to a Vulkan
// texture with a SLOT-AWARE internalformat:
//   BaseColor/Emissive -> R8G8B8A8_SRGB (the sampler linearizes on sample)
//   Normal/Orm         -> R8G8B8A8_UNORM (linear)
// Returns the toolvk texture id, or 0 on failure (errorOut set). The caller owns
// the texture and must toolvk::destroyTexture it.
uint32_t MaterialTextureLoader_Load(const std::string& path,
                                    MaterialSlotKind kind,
                                    std::string* errorOut);
