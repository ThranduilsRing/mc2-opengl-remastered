# RenderCore/CMakeLists.txt
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

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET rendercore)
    target_link_libraries(rendercore PUBLIC mc2_gate basisu_transcoder)
endif()
