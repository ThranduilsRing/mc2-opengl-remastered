#!/bin/bash
# Install the Vulkan validation layer (VVL) on the box: LunarG Vulkan SDK 1.4.350.0 linux tarball (same version as the PC SDK and tools/gfxr).
# Run on the box (as root): scp this file over, then `bash install_vvl.sh`. Result: /workspace/toolchains/vvl-1.4.350.0/{explicit_layer.d,lib}, which box_run (VK_LAYER_PATH) and box_health probe.
# Installed 2026-11-10 (lane box-health-fix). Re-rented box: run this once after the first deploy.sh apply.
V=1.4.350.0; D=/workspace/toolchains/dl; T=/workspace/toolchains/vvl-$V
mkdir -p $D $T/explicit_layer.d $T/lib
[ -s $D/vulkansdk-linux-x86_64-$V.tar.xz ] || curl -fL -m 900 -o $D/vulkansdk-linux-x86_64-$V.tar.xz https://sdk.lunarg.com/sdk/download/$V/linux/vulkansdk-linux-x86_64-$V.tar.xz
cd $D; rm -rf x; mkdir x
tar -xJf vulkansdk-linux-x86_64-$V.tar.xz -C x --wildcards '*libVkLayer_khronos_validation.so' '*VkLayer_khronos_validation.json'
find x -name 'libVkLayer_khronos_validation.so' -exec cp {} $T/lib/ \;
find x -name 'VkLayer_khronos_validation.json' | head -1 | xargs -I{} cp {} $T/explicit_layer.d/
sed -i 's#"library_path": *"[^"]*"#"library_path": "'$T'/lib/libVkLayer_khronos_validation.so"#' $T/explicit_layer.d/VkLayer_khronos_validation.json
rm -rf x
grep -n 'library_path\|api_version' $T/explicit_layer.d/*.json; ls -la $T/lib
