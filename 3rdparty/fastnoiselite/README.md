# FastNoiseLite

C++ and GLSL sources differ from upstream only by removal of trailing whitespace, so the repository whitespace check stays clean.

https://github.com/Auburn/FastNoiseLite | tag v1.1.1 | commit 7ccfbc16eb1c932568f177d63a9ba51d89bbe516 | MIT

FastNoiseLite.h: https://github.com/Auburn/FastNoiseLite/tree/v1.1.1 | tag v1.1.1 | upstream sha256 47a29750dfe13d15651febd235f3c26b1cc57af7c6e26583b838fd7ab5215dc2 | vendored sha256 bc210e500685abf895b2d559a51e63c1fe7dc2da671eeea5003dc7ce98f89d32
FastNoiseLite.glsl: https://github.com/Auburn/FastNoiseLite/tree/v1.1.1 | tag v1.1.1 | upstream sha256 71c9af239e4f75617c74f74a9266fd944550edfa4ddc31920703e2cdb836cef2 | vendored sha256 c4f90dd88916900c6e2bb97f03d485f1278f789bb745e52591e48e9164b01a80
LICENSE: https://github.com/Auburn/FastNoiseLite/tree/v1.1.1 | tag v1.1.1 | sha256 c08da3239b919c12f4ec616b457a8dd0fc923c8102c8a742c4006fcb5de28fb0

The C++ impact atlas uses Value noise at frequency 1, remapped to [0,1], preserving its cubic lattice and mask ranges while changing its hash pattern.

Vulkan consumers can include `include/fastnoiselite.glsl` with GL_GOOGLE_include_directive enabled. The wrapper resolves the unmodified vendor port and CMake tracks it for SPIR-V rebuilds.

No shader noise replaced: `shaders/include/noise.hglsl` and `shaders/vulkan/shoreline.frag` use Ashima simplex (terrain/water/cloud shapes); `shaders/vulkan/fog_oob.frag` uses a sine-hashed cubic value field; `shaders/vulkan/postprocess_visual.frag` uses interleaved gradient dither. FastNoiseLite changes the spatial patterns or noise spectrum, so none is an equivalent drop-in. CPU `terrain_surface_cook.cpp` also mirrors the Ashima field and participates in cooked asset hashes; left intact.

Kit follow-up only: `tools/scifi_kit/art/blend.py` uses Blender ShaderNodeTexNoise, mathutils.noise, and its own hashf. That lane is untouched.
