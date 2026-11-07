#pragma once
// Built-in temporal upscaler providers (RenderCore::IUpscalerProvider). Present and the material samplers use only
// the RenderCore registry; this header exists so they can make sure the providers are registered.
namespace mc2vk {
// Registers the built-in providers in fallback order (vk_upscalers.cpp); idempotent. A new vendor adds one provider
// file and one line there.
void registerBuiltinUpscalers();
}
