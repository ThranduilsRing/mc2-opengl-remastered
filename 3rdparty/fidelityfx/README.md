AMD FidelityFX SDK, MIT license (license.txt).
Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
Pinned tag v1.1.4, upstream commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55.
Unmodified ffx-api C headers (ffx_api, ffx_api_types, ffx_upscale, ffx_api_loader, vk/ffx_api_vk) and the signed prebuilt
bin/amd_fidelityfx_vk.dll (FSR 3.1 upscaler, Vulkan). The DLL is loaded at runtime by GameOS/gameos/vulkan_fsr_provider.cpp;
if it is missing the provider stays inactive. The DLL is 9.3 MB (over the 1 MB rule) because rebuilding it needs the full
dxc shader-permutation build.
