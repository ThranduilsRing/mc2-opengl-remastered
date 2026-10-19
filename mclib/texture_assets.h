// Texture source API. Its temporary manager adapter keeps existing source and lifetime semantics until the P9 store replacement.
#pragma once
#include <gameos.hpp>
#include <cstdint>
#include <memory>
namespace RenderCore { struct KtxImage; }

namespace mc2::textures {
DWORD load(const char* path, gos_TextureFormat format, DWORD hints, DWORD variant=0, DWORD lifetime=0);
DWORD findAndRetain(const char* path, gos_TextureFormat format, DWORD hints, DWORD variant=0, DWORD lifetime=0);
DWORD retain(DWORD asset);
void release(DWORD asset);
DWORD sourceId(DWORD asset);
DWORD fromPixels(DWORD* pixels, gos_TextureFormat format, DWORD hints, DWORD width, DWORD bytesPerPixel=4);
DWORD fromRawPixels(DWORD* pixels, gos_TextureFormat format, DWORD hints, DWORD width, DWORD bytesPerPixel=4);
long save(DWORD asset,const char* path);
long refresh(const char* match,long& missing);
DWORD uvScale(DWORD asset);
bool logicalSize(DWORD asset,DWORD& w,DWORD& h);
const char* name(DWORD asset);
bool isDecoded(DWORD asset);
void setSessionLifetime(DWORD asset,DWORD value);
void pin(DWORD asset);
void unpin(DWORD asset);
DWORD pinCount(DWORD asset);
uint64_t uiId(DWORD asset);
std::shared_ptr<const RenderCore::KtxImage> image(DWORD asset);
DWORD storageWidth(DWORD asset);
} // namespace mc2::textures
