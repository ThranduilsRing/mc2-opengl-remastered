// Device-free consumer boundary for the session texture service.
#pragma once
#include "../../RenderCore/TextureService.h"
#include "gameos.hpp"
uint64_t gos_GetUiTextureId(DWORD handle);
uint64_t gos_GetMaterialTextureId(DWORD handle);
namespace mc2vk {
bool uiTextureServiceReady();
void finishUiTextureLoading();
bool hasUiImage(RenderCore::TextureHandle handle);
void invalidateUiImage(RenderCore::TextureHandle handle);
void requestUiImage(RenderCore::TextureHandle handle, std::function<std::shared_ptr<const RenderCore::KtxImage>()> decode);
void attachUiImage(RenderCore::TextureHandle handle, std::shared_ptr<const RenderCore::KtxImage> image);
}
