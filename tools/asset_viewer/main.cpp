/***************************************************************
 * FILENAME: main.cpp
 * DESCRIPTION: Entry point for mc2_asset_viewer (stage 1).
 ***************************************************************/
#include "build_identity.h"
#include <cstdio>
#include <cstring>
#include <SDL.h>
#include "tool_vk_gpu.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"
#include "AssetViewerApp.h"
#include "ArchiveIo.h"

int main(int argc, char* argv[])
{
    std::printf("%s\n", MC2_BUILD_FP_LITERAL);
    mc2PrintBuildFingerprint();
    // --pak list|extract|pack|verify: headless archive verbs (no window), exit 0/1.
    if (argc >= 2 && strcmp(argv[1], "--pak") == 0)
        return runPakVerb(argc - 2, argv + 2);

    // --smoke <fixtureDir> dispatch (Task 8 smoke test hook)
    if (argc >= 2 && strcmp(argv[1], "--smoke-decoder") == 0)
        return AssetViewerApp::runSmokeDecoder();

    if (argc >= 2 && strcmp(argv[1], "--smoke-ktx-parse") == 0)
        return AssetViewerApp::runSmokeKtxParse(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-fit") == 0)
        return AssetViewerApp::runSmokeFit();

    if (argc >= 2 && strcmp(argv[1], "--smoke-tiers") == 0)
        return AssetViewerApp::runSmokeTiers(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-sphere") == 0)
        return AssetViewerApp::runSmokeSphere();

    if (argc >= 2 && strcmp(argv[1], "--smoke-fit-material") == 0)
        return AssetViewerApp::runSmokeFitMaterial(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-tgl-load") == 0)
        return AssetViewerApp::runSmokeTglLoad(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-mesh-build") == 0)
        return AssetViewerApp::runSmokeMeshBuild(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-mesh-orient") == 0)
        return AssetViewerApp::runSmokeMeshOrient(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-spotlight") == 0)
        return AssetViewerApp::runSmokeSpotlight(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-link") == 0)
        return AssetViewerApp::runSmokeWorkbenchLink();

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-glb") == 0)
        return AssetViewerApp::runSmokeWorkbenchGlb(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-bind") == 0)
        return AssetViewerApp::runSmokeWorkbenchBind(argc >= 3 ? argv[2] : ".", argc >= 4 ? argv[3] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-validate") == 0)
        return AssetViewerApp::runSmokeWorkbenchValidate(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-export") == 0)
        return AssetViewerApp::runSmokeWorkbenchExport(
            argc >= 3 ? argv[2] : ".",
            argc >= 4 ? argv[3] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-workbench-reload") == 0)
        return AssetViewerApp::runSmokeWorkbenchReload(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-appearance-roster") == 0)
        return AssetViewerApp::runSmokeAppearanceRoster(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-texture-missing-warn") == 0)
        return AssetViewerApp::runSmokeTextureMissingWarn(argc >= 3 ? argv[2] : ".");

    if (argc >= 2 && strcmp(argv[1], "--smoke-lod-edit-validate") == 0)
        return AssetViewerApp::runSmokeLodEditValidate();

    if (argc >= 2 && strcmp(argv[1], "--smoke-central-merge-preserve") == 0)
        return AssetViewerApp::runSmokeCentralMergePreserve(
            argc >= 3 ? argv[2] : ".", argc >= 4 ? argv[3] : ".");


    // --view <textures <file> | materials | props <name.tgl> | workbench>: open a view at launch (screenshot runs, MC2_TOOL_CAPTURE).
    const char* viewKind = nullptr;
    const char* viewArg = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], "--view") == 0) {
            viewKind = argv[i + 1];
            viewArg = (i + 2 < argc && strncmp(argv[i + 2], "--", 2) != 0) ? argv[i + 2] : nullptr;
        }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        printf("SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }

    Uint32 winFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (toolvk::captureRequested()) winFlags |= SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow(
        "MC2 Asset Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        winFlags
    );
    if (!window)
    {
        printf("SDL_CreateWindow error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    toolvk::Shell vk;
    vk.clear[0] = 0.06f; vk.clear[1] = 0.065f; vk.clear[2] = 0.075f;
    if (!toolvk::init(vk, window, "mc2_asset_viewer") || !toolvk::gpuInit(&vk))
    {
        printf("Vulkan init failed\n");
        toolvk::gpuShutdown();
        toolvk::shutdown(vk);
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    {
        AssetViewerApp app;
        if (viewKind) app.scriptedView(viewKind, viewArg);

        bool running = true;
        while (running)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT)
                    running = false;
                if (event.type == SDL_WINDOWEVENT &&
                    event.window.event == SDL_WINDOWEVENT_CLOSE &&
                    event.window.windowID == SDL_GetWindowID(window))
                    running = false;
                if (event.type == SDL_DROPFILE){ app.onFileDropped(event.drop.file); SDL_free(event.drop.file); }
            }

            if (!toolvk::begin(vk)) { SDL_Delay(10); continue; }
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();

            app.drawUi();

            ImGui::Render();
            if (toolvk::end(vk)) running = false;
        }
    }   // app destroyed here: its previews free their toolvk textures/targets while the device is alive

    toolvk::gpuShutdown();
    toolvk::shutdown(vk);
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
