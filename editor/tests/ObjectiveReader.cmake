add_executable(editor_objective_reader_tests EXCLUDE_FROM_ALL
    tests/test_main.cpp tests/test_objective_reader.cpp tests/test_mission_codec.cpp
    "${CMAKE_SOURCE_DIR}/tests/unit/heap_shim.cpp"
    "${CMAKE_SOURCE_DIR}/code/objective_text.cpp"
    "${CMAKE_SOURCE_DIR}/code/objective_flags.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos/utils/mc2log.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos/cook_cache.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/file.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/vfs.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/vfs_index.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/pak_zip.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/inifile.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/inifile_read.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/inifile_arrays.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/packet.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/ffile.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/fastfile.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/fst_hash.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/lzdecomp.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/lzcomp.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/err.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/src/platform_str.cpp")
target_compile_features(editor_objective_reader_tests PRIVATE cxx_std_23)
target_compile_definitions(editor_objective_reader_tests PRIVATE MC2_EDITOR_READER_TESTS=1 MC2_EDITOR_SMOKE_HOST=1)
target_sources(editor_objective_reader_tests PRIVATE
    shell/EntityPanels.cpp shell/BrainPathPanel.cpp shell/ObjectivePanel.cpp shell/ObjectivePresentation.cpp
    shell/PanelProof.cpp shell/ObjectiveProof.cpp shell/FileWorkflow.cpp shell/FileProof.cpp
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui/misc/cpp/imgui_stdlib.cpp")
target_include_directories(editor_objective_reader_tests PRIVATE shell)
target_link_libraries(editor_objective_reader_tests PRIVATE imgui SDL2::SDL2)
target_include_directories(editor_objective_reader_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/mclib" "${CMAKE_SOURCE_DIR}/code"
    "${CMAKE_SOURCE_DIR}/GameOS" "${CMAKE_SOURCE_DIR}/GameOS/include"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos" "${CMAKE_SOURCE_DIR}/3rdparty/include")
target_link_libraries(editor_objective_reader_tests PRIVATE editor_document mc2_gate ZLIB::ZLIB basisu_transcoder)
