#!/bin/bash
# Install the Vulkan validation layer (VVL) on the box: LunarG Vulkan SDK 1.4.350.0 linux tarball (same version as the PC SDK and tools/gfxr).
# Run on the box (as root): scp this file over, then `bash install_vvl.sh`. Result: /workspace/toolchains/vvl-1.4.350.0/{explicit_layer.d,lib}, which box_run (VK_LAYER_PATH) and box_health probe.
# Installed 2026-11-10 (lane box-health-fix). Re-rented box: run this once after the first deploy.sh apply.
set -euo pipefail
V=1.4.350.0; T=/workspace/toolchains/vvl-$V
if [ ! -s "$T/lib/libVkLayer_khronos_validation.so" ] || [ ! -s "$T/explicit_layer.d/VkLayer_khronos_validation.json" ]; then
    [ "$(df --output=pcent /workspace | tail -1 | tr -dc '0-9')" -lt 70 ]
    mkdir -p "$T/lib" "$T/explicit_layer.d"
    D=$(mktemp -d /workspace/toolchains/vvl-download.XXXXXX)
    trap 'rm -r -- "$D"' EXIT
    curl -fL --max-time 900 -o "$D/sdk.tar.xz" https://sdk.lunarg.com/sdk/download/$V/linux/vulkansdk-linux-x86_64-$V.tar.xz
    echo "b65f068ab36263559da49d7cacd7e7b9df23824ca8b68ccc522a2b06f5725df2  $D/sdk.tar.xz" | sha256sum -c -
    tar -xJf "$D/sdk.tar.xz" -C "$D" --wildcards \
        '*/x86_64/lib/libVkLayer_khronos_validation.so' '*/x86_64/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json'
    sed "s#\"library_path\": *\"[^\"]*\"#\"library_path\": \"$T/lib/libVkLayer_khronos_validation.so\"#" \
        "$D"/*/x86_64/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json > "$T/explicit_layer.d/VkLayer_khronos_validation.json"
    cp "$D"/*/x86_64/lib/libVkLayer_khronos_validation.so "$T/lib/"
fi
# remote_setup.sh calls this installer: register the SDK ahead of the apt layer
# even for non-login SSH commands, which never source /etc/profile.d.
install -d /etc/vulkan/explicit_layer.d
install -m 644 "$T/explicit_layer.d/VkLayer_khronos_validation.json" /etc/vulkan/explicit_layer.d/VkLayer_khronos_validation.json
sha256sum "$T/lib/libVkLayer_khronos_validation.so"
grep -n 'library_path\|api_version' "$T/explicit_layer.d/VkLayer_khronos_validation.json"
ldd "$T/lib/libVkLayer_khronos_validation.so"
