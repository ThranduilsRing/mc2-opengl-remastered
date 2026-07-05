// VULKAN-SHADER-TOOLCHAIN-1 -- standalone probe harness.
// Compiled ONLY under MC2_VULKAN (see CMake). Links the Vulkan loader + the
// skeleton TU. No GL, no SDL, no window, no game. Runs the headless
// shader-module load probe against the compiled fullscreen .spv files.
//
// Usage: vulkan_shader_probe [spv-dir]   (default arg: shaders/vulkan)
// Exit 0 = both fullscreen shader modules created+destroyed OK; 1 otherwise.
#include "vulkan_backend_skeleton.h"

#include <cstdio>

int main(int argc, char** argv) {
    const char* spvDir = (argc > 1) ? argv[1] : "shaders/vulkan";
    std::printf("[vulkan_shader_probe] spvDir=%s\n", spvDir);
    bool caps     = mc2_vulkan_probe();
    bool shaders  = mc2_vulkan_probe_shaders(spvDir);
    // VULKAN-FULLSCREEN-TRIANGLE-1: full headless offscreen render + readback.
    bool triangle = mc2_vulkan_probe_triangle(spvDir);
    // VULKAN-DESCRIPTOR-SMOKE-1: headless descriptor-set plumbing probe. With
    // MC2_VULKAN_VALIDATION set, zero validation errors is the real proof.
    bool descriptors = mc2_vulkan_probe_descriptors();
    // VK-BOOTSTRAP-INTEGRATE-1: swapchain probe (create+query+destroy, no present).
    // Needs a hidden SDL Vulkan window; on a display-less host it fail-softs.
    bool swapchain = mc2_vulkan_probe_swapchain();
    // VULKAN-EDGEFOG-SYNTHETIC-FIXTURE-1: shader-math oracle for the edge-fog port.
    // Runs the SHIPPED edge_fog .spv on a known input and checks GPU vs a CPU ref.
    bool edgefogFixture = mc2_vulkan_probe_edgefog_fixture(spvDir);
    // VULKAN-OOB-FOG-ISLAND-1: shader-math oracle for the OOB-fog port.
    bool oobfogFixture = mc2_vulkan_probe_oobfog_fixture(spvDir);
    // Fail-soft re-check: a bogus spv dir must log + return false, no crash.
    bool failSoft = !mc2_vulkan_probe_triangle("this/dir/does/not/exist");
    std::printf("[vulkan_shader_probe] caps=%d shaders=%d triangle=%d descriptors=%d swapchain=%d edgefog_fixture=%d oobfog_fixture=%d failSoftOK=%d\n",
                caps ? 1 : 0, shaders ? 1 : 0, triangle ? 1 : 0, descriptors ? 1 : 0,
                swapchain ? 1 : 0, edgefogFixture ? 1 : 0, oobfogFixture ? 1 : 0, failSoft ? 1 : 0);
    return (caps && shaders && triangle && descriptors && swapchain && edgefogFixture && oobfogFixture && failSoft) ? 0 : 1;
}
