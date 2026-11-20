# Zstandard 1.5.7 compressor (BSD) for the pak writer: linked only into mc2_asset_viewer and mc2_tests. See VERSION.txt.
# Every external symbol is renamed (zstd_prefix.h), so it coexists with the 1.4.9 decoder in basisu/zstd.
add_library(mc2_zstd_compress STATIC zstd_mc2.c)
target_include_directories(mc2_zstd_compress PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(mc2_zstd_compress PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    # zstd.c redefines a few aliases (HUF_isError -> ERR_isError) that zstd_prefix.h already renamed; harmless.
    target_compile_options(mc2_zstd_compress PRIVATE /wd4005)
endif()
