# Retired GL sampler-unit census

The GL sampler-unit checker and generated JSON were removed with the GL renderer.
There is no regeneration command or `sampler_bindings` contract row.
Live Vulkan GLSL buffer, sampler and image descriptors are scanned by
`scripts/check-binding-slots.py` and classified per pipeline by
`scripts/check-vulkan-bindings.py`. Slang descriptors have separate Slang contracts.
