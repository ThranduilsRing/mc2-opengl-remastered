add_executable(editor_objective_reader_tests EXCLUDE_FROM_ALL
    tests/test_main.cpp tests/test_objective_reader.cpp
    "${CMAKE_SOURCE_DIR}/tests/unit/heap_shim.cpp"
    "${CMAKE_SOURCE_DIR}/code/objective_text.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos/utils/mc2log.cpp"
    "${CMAKE_SOURCE_DIR}/mclib/file.cpp"
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
target_include_directories(editor_objective_reader_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/mclib" "${CMAKE_SOURCE_DIR}/code"
    "${CMAKE_SOURCE_DIR}/GameOS" "${CMAKE_SOURCE_DIR}/GameOS/include"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos" "${CMAKE_SOURCE_DIR}/3rdparty/include")
target_link_libraries(editor_objective_reader_tests PRIVATE editor_document mc2_gate ZLIB::ZLIB)
