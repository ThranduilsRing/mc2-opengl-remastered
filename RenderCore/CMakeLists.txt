# RenderCore/CMakeLists.txt
#
# KtxBasis.cpp includes vendored basisu headers whose debug asserts do not compile under clang (ambiguous safe_shift_left).
set_source_files_properties(KtxBasis.cpp PROPERTIES COMPILE_DEFINITIONS NDEBUG)
#
# KtxLoader.cpp promoted this from INTERFACE to STATIC.
# All INTERFACE consumers (renderworld, gameadapters, editorbridge) still link
# correctly; the PUBLIC include dir + compile feature propagate unchanged.

add_library(rendercore STATIC
    KtxLoader.cpp
    KtxBasis.cpp
    PipelineRegistry.cpp
    RenderDebugView.cpp
    RenderResourceRegistry.cpp
)
target_include_directories(rendercore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(rendercore PUBLIC cxx_std_17)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # TemporalInputs::Projection uses atomics larger than the native word size.
    target_link_libraries(rendercore PUBLIC atomic)
endif()

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET rendercore)
    target_link_libraries(rendercore PUBLIC mc2_gate basisu_transcoder)
endif()
