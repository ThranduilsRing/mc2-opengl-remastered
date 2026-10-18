cmake_minimum_required(VERSION 3.20)
# MC2 Asset Viewer standalone target.
#
# Keep SDL include resolution local to this target. The ImGui SDL backend
# includes "SDL.h", so the SDL2 include folder itself must be on the
# include path, not just the parent include directory.

# ---------------------------------------------------------------------------
# mc2_tglloader — isolated static lib that links the NS3 game-free TGL loader
# TU set (mclib + Stuff + platform_str + engine_stubs).
#
# ISOLATION RATIONALE: the engine TUs need PLATFORM_WINDOWS + LINUX_BUILD
# defines, and engine_stubs.cpp defines symbols (gos_*, Environment, eye, …)
# that may collide with anything in the viewer's imgui/SDL/UiEditor TUs.
# Putting them in their own STATIC lib with their own compile definitions and
# include paths keeps those defines off mc2_asset_viewer's TUs entirely. The
# viewer then links mc2_tglloader as a private dependency.
# ---------------------------------------------------------------------------

# zlib — FastFile/PacketFile/lzdecomp decompression. Prebuilt; search the
# vendored 3rdparty paths (worktree first, then the parent repo's copy).
find_library(MC2_ZLIB
    NAMES zlib zlibstatic
    PATHS
        "${CMAKE_SOURCE_DIR}/3rdparty/3rdparty/lib/x64"
        "${CMAKE_SOURCE_DIR}/3rdparty/lib/x64"
        "A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64"
    NO_DEFAULT_PATH
)
if(NOT MC2_ZLIB)
    message(FATAL_ERROR "zlib.lib not found — copy 3rdparty/3rdparty/lib/x64/zlib.lib from a built tree")
endif()

add_library(mc2_tglloader STATIC
    engine_stubs.cpp
    asset_viewer_game_stubs.cpp
    TglMeshLoader.cpp
    # --- mclib TUs (geometry loader closure) ---
    "${CMAKE_SOURCE_DIR}/mclib/msl.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/msl_load.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/msl_shape.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/msl_transform.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/msl_anim.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/tgl.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/tgl_parse.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/tgl_render.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/tgl_shadows.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/tgl_transform.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/tgl_transform_light.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/tgl_types.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/cident.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/mathfunc.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/src/platform_str.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/file.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/fastfile.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/ffile.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/packet.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos/utils/mc2log.cpp"   # file.cpp / packet.cpp log malformed content through the facade
    "${CMAKE_SOURCE_DIR}/mclib/lzdecomp.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/fst_hash.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/heap.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/paths.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/timing.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/cpu_proj_cost_split.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/projectz_trace.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/object_admission_predicate.cpp"
    # META-FIX design B PART 0: msl.cpp now emits [ASSET_LOAD v1] loader
    # classification via the INFRA-1 audit emitter (leaf TU, no GameOS dep).
    "${CMAKE_SOURCE_DIR}/mclib/mc2_audit.cpp"
    # LINK-CONFIG-FIX: tgl.cpp references render_contract::noteRenderPass and
    # msl.cpp references g_isFrameJobsWorker (frame_jobs.h). Both defining TUs
    # are otherwise compiled only into mc2.exe's source list, so this isolated
    # loader lib must carry them too. render_contract.cpp's GL calls are runtime-
    # gated and resolve against the GLEW/OpenGL the viewer exe links.
    "${CMAKE_SOURCE_DIR}/mclib/render_contract.cpp"
    # MC2-JOBS-CORE-1: frame_jobs.cpp -> job_system.cpp (+ jobs_fatal.cpp).
    "${CMAKE_SOURCE_DIR}/code/job_system.cpp"
    "${CMAKE_SOURCE_DIR}/code/jobs_fatal.cpp"
    # --- Stuff library TUs (game-free math + containers) ---
    "${CMAKE_SOURCE_DIR}/mclib/stuff/affinematrix.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/color.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/extentbox.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/linearmatrix.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/matrix.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/obb.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/plane.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/point3d.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/polar.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/random.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/rotation.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/scalar.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/sphere.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/unitvector.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/vector3d.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/vector4d.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/memoryblock.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/mstring.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/chain.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/hash.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/iterator.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/link.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/node.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/plug.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/registeredclass.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/safesocket.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/socket.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/sortedchain.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/sortedsocket.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/tree.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/filestream.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/filestreammanager.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/memorystream.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/notationfile.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/note.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/page.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/style.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/trace.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/stuff/stuff.cpp"
    # Tracy profiler client (needed because TRACY_ENABLE is set globally and
    # msl.cpp includes gos_profiler.h which includes Tracy.hpp inline-instantiating
    # the ConcurrentQueue that references GetProfiler/GetToken/rpmalloc/etc).
    "${CMAKE_SOURCE_DIR}/3rdparty/tracy/TracyClient.cpp"
)

target_compile_features(mc2_tglloader PRIVATE cxx_std_17)

target_compile_definitions(mc2_tglloader PRIVATE
    PLATFORM_WINDOWS
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
    # NOTE: deliberately NOT defining ENABLE_ASSIMP_IMPORTER
)

target_include_directories(mc2_tglloader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # tools/asset_viewer/ — for TglMeshLoader.h
    "${CMAKE_SOURCE_DIR}/mclib"
    "${CMAKE_SOURCE_DIR}/mclib/stuff"
    "${CMAKE_SOURCE_DIR}/GameOS/include"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    # GL/glew.h needed as a header (tgl.cpp -> gos_static_prop_batcher.h). No GL .lib linked.
    "${CMAKE_SOURCE_DIR}/3rdparty/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/tracy"
    "${CMAKE_SOURCE_DIR}"
)

# mc2_tglloader's public headers (mclib + GameOS/include + TglMeshLoader.h)
# must be visible to mc2_asset_viewer TUs.
target_include_directories(mc2_tglloader PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"          # tools/asset_viewer/ — exposes TglMeshLoader.h
    "${CMAKE_SOURCE_DIR}/mclib"
    "${CMAKE_SOURCE_DIR}/mclib/stuff"
    "${CMAKE_SOURCE_DIR}/GameOS/include"
)

# mc2_gate: the mclib TUs compiled here read their gates through the typed registry.
target_link_libraries(mc2_tglloader PRIVATE ${MC2_ZLIB} mc2_gate)

# ---------------------------------------------------------------------------
# End mc2_tglloader
# ---------------------------------------------------------------------------

set(ASSET_VIEWER_SOURCES
    main.cpp
    AssetViewerApp.cpp
    SphereMesh.cpp
    LocalPbrMaterialBackend.cpp
    MaterialTextureLoader.cpp
    MaterialPreviewPBR.cpp
    MaterialSlots.cpp
    FitMaterialLoader.cpp
    # Task 2: mesh GPU upload + 3D preview
    MeshGpu.cpp
    MeshPreview3D.cpp
    # Task 3: model browser (Static Props panel)
    ModelBrowser.cpp
    # remaining sources added in later tasks
    TextureExtensions.cpp
    TextureMetadata.cpp
    TexturePreview2D.cpp
    FileBrowser.cpp
    AssetTypeSidebar.cpp
    TextureInspectorPanel.cpp
    TextureDecoderRegistry.cpp
    LegacyImageDecoder.cpp
    Ktx2Decoder.cpp
    "${CMAKE_SOURCE_DIR}/RenderCore/KtxLoader.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos/utils/Image.cpp"
    # engine-independent override-manifest parser (validates/round-trips models.json)
    "${CMAKE_SOURCE_DIR}/mclib/model_override_registry.cpp"
    # S1: glTF/GLB loader, mod workbench, override manifest
    GlbMeshLoader.cpp
    ModWorkbench.cpp
    AppearanceRoster.cpp
    OverrideManifest.cpp
    # S2: mod workbench side-by-side panel
    ModWorkbenchPanel.cpp
    # S3: semantic validation
    WorkbenchValidation.cpp
    # S4: bundle export + registry round-trip gate
    BundleExport.cpp
    # S5: safe reversible central models.json merge
    CentralManifestMerge.cpp
    # Backend-A v2: GLSL #include inliner for engine shader compilation
    ShaderIncludeResolver.cpp
    # Backend-A v2 Task 3: shader contract report model + ImGui view
    ShaderContractReport.cpp
    # Backend-A v2 Task 5: engine-shader preview surface (stub: not ported to Vulkan)
    ModelPreviewEngineShader.cpp
)

add_executable(mc2_asset_viewer ${ASSET_VIEWER_SOURCES})

target_compile_features(mc2_asset_viewer PRIVATE cxx_std_17)

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 CONFIG REQUIRED)
endif()

if(NOT TARGET OpenGL::GL)
    find_package(OpenGL REQUIRED)
endif()

if(NOT TARGET GLEW::GLEW)
    find_package(GLEW REQUIRED)
endif()

set(MC2R_AV_SDL_INCLUDE_DIRS)

if(DEFINED SDL2_INCLUDE_DIRS)
    list(APPEND MC2R_AV_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
endif()

if(DEFINED SDL2_INCLUDE_DIR)
    list(APPEND MC2R_AV_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
endif()

foreach(_mc2r_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_mc2r_prefix}/include/SDL2/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_prefix}/include/SDL2")
    endif()

    if(EXISTS "${_mc2r_prefix}/include/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_prefix}/include")
    endif()
endforeach()

if(DEFINED VCPKG_TARGET_TRIPLET)
    foreach(_mc2r_av_vcpkg_root
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}"
    )
        if(EXISTS "${_mc2r_av_vcpkg_root}/include/SDL2/SDL.h")
            list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_av_vcpkg_root}/include/SDL2")
        endif()

        if(EXISTS "${_mc2r_av_vcpkg_root}/include/SDL.h")
            list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_av_vcpkg_root}/include")
        endif()
    endforeach()
endif()

# Common vcpkg layout for this repo's build64 tree.
foreach(_mc2r_av_vcpkg_root
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x86-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-linux"
)
    if(EXISTS "${_mc2r_av_vcpkg_root}/include/SDL2/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_av_vcpkg_root}/include/SDL2")
    endif()

    if(EXISTS "${_mc2r_av_vcpkg_root}/include/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_av_vcpkg_root}/include")
    endif()
endforeach()

if(MC2R_AV_SDL_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES MC2R_AV_SDL_INCLUDE_DIRS)
endif()

target_include_directories(mc2_asset_viewer PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends"
    "${CMAKE_SOURCE_DIR}/3rdparty/stb"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    "${CMAKE_SOURCE_DIR}/ui_editor"
    "${CMAKE_SOURCE_DIR}/RenderCore"
    ${MC2R_AV_SDL_INCLUDE_DIRS}
)

target_link_libraries(mc2_asset_viewer PRIVATE imgui mc2_gate)

# Assimp (glTF importer only, per root CMake ASSIMP_BUILD_GLTF_IMPORTER=ON).
target_link_libraries(mc2_asset_viewer PRIVATE assimp::assimp)

if(TARGET SDL2::SDL2)
    target_link_libraries(mc2_asset_viewer PRIVATE SDL2::SDL2)
elseif(DEFINED SDL2_LIBRARIES)
    target_link_libraries(mc2_asset_viewer PRIVATE ${SDL2_LIBRARIES})
else()
    message(FATAL_ERROR "SDL2 target or SDL2_LIBRARIES was not found for mc2_asset_viewer")
endif()

if(TARGET SDL2::SDL2main)
    target_link_libraries(mc2_asset_viewer PRIVATE SDL2::SDL2main)
endif()

# The viewer draws through Vulkan only (tools/common mc2_tool_vk). OpenGL/GLEW stay on the link line solely because the isolated
# mc2_tglloader engine closure (render_contract.cpp, tgl.cpp -> gos_static_prop_batcher.h) still references GL symbols; no GL context is
# ever created.
target_link_libraries(mc2_asset_viewer PRIVATE OpenGL::GL)

if(WIN32)
    target_link_libraries(mc2_asset_viewer PRIVATE ole32 windowscodecs uuid)
    # Tracy needs Psapi (EnumProcessModules, GetModuleInformation, etc.).
    target_link_libraries(mc2_asset_viewer PRIVATE Psapi)
endif()

target_link_libraries(mc2_asset_viewer PRIVATE GLEW::GLEW)

# Link the TGL loader lib (NS3 game-free loader + engine_stubs, isolated).
target_link_libraries(mc2_asset_viewer PRIVATE mc2_tglloader)

# Vulkan window + offscreen-render layer shared by the viewport tools.
if(NOT TARGET mc2_tool_vk)
    add_subdirectory("${CMAKE_SOURCE_DIR}/tools/common" "${CMAKE_BINARY_DIR}/tools_common")
endif()
target_link_libraries(mc2_asset_viewer PRIVATE mc2_tool_vk)

# GLSL 450 preview shaders -> SPIR-V next to the exe (shaders_vk/), loaded at runtime by toolvk::shaderDir().
set(MC2_AV_SHADERS mesh_lit.vert mesh_lit.frag pbr_sphere.vert pbr_sphere.frag)
if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    find_program(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
endif()
if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(FATAL_ERROR "glslangValidator not found (Vulkan SDK): needed to compile tools/asset_viewer/shaders_vk")
endif()
set(MC2_AV_SPV_DIR "$<TARGET_FILE_DIR:mc2_asset_viewer>/shaders_vk")
set(MC2_AV_SPV_OUT)
foreach(_sh IN LISTS MC2_AV_SHADERS)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders_vk/${_sh}.spv"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders_vk"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders_vk/${_sh}"
                -o "${CMAKE_CURRENT_BINARY_DIR}/shaders_vk/${_sh}.spv"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders_vk/${_sh}"
        COMMENT "glslangValidator ${_sh}"
        VERBATIM)
    list(APPEND MC2_AV_SPV_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders_vk/${_sh}.spv")
endforeach()
add_custom_target(mc2_asset_viewer_shaders DEPENDS ${MC2_AV_SPV_OUT})
add_dependencies(mc2_asset_viewer mc2_asset_viewer_shaders)
add_custom_command(TARGET mc2_asset_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MC2_AV_SPV_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MC2_AV_SPV_OUT} "${MC2_AV_SPV_DIR}"
    VERBATIM)

# The viewer also needs GameOS/gameos headers for engine_stubs-exposed types.
# mc2_tglloader already exposes mclib + GameOS/include as PUBLIC; add gameos:
target_include_directories(mc2_asset_viewer PRIVATE
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    "${CMAKE_SOURCE_DIR}/3rdparty/tracy"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/mclib"                       # model_override_registry.h
    "${CMAKE_SOURCE_DIR}/3rdparty/include"            # nlohmann/json.hpp (registry TU only)
    "${CMAKE_SOURCE_DIR}/3rdparty/assimp/include"     # <assimp/Importer.hpp>
    "${CMAKE_BINARY_DIR}/out/3rdparty/assimp/include" # generated assimp config.h
)
