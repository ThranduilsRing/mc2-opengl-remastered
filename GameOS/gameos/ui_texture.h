// Device-free consumer boundary for the session texture service.
#pragma once
#include "../../RenderCore/TextureService.h"
#include "gameos.hpp"
uint64_t gos_GetUiTextureId(DWORD handle);
namespace mc2vk {
bool hasUiImage(RenderCore::TextureHandle handle);
void attachUiImage(RenderCore::TextureHandle handle, std::shared_ptr<const RenderCore::KtxImage> image);
}
