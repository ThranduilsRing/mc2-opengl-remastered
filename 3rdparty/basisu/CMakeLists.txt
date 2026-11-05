# Basis Universal transcoder (Apache-2.0) + its Zstd decompressor (BSD): KTX2 UASTC / ETC1S -> BC1/3/7 at load. See VERSION.txt.
add_library(basisu_transcoder STATIC
    transcoder/basisu_transcoder.cpp
    zstd/zstddeclib.c
)
target_include_directories(basisu_transcoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(basisu_transcoder PUBLIC BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1)
set_target_properties(basisu_transcoder PROPERTIES POSITION_INDEPENDENT_CODE ON)
