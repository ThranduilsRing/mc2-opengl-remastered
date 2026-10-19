// Device-free consumer boundary for the session texture service.
#pragma once
#include "../../RenderCore/TextureService.h"
namespace mc2vk {
bool hasUiImage(RenderCore::TextureHandle handle);
void attachUiImage(RenderCore::TextureHandle handle, std::shared_ptr<const RenderCore::KtxImage> image);
}
