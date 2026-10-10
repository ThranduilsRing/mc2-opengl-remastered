add_executable(model_override_tests
  test_model_override_registry.cpp
  ${CMAKE_SOURCE_DIR}/mclib/model_override_registry.cpp)
target_include_directories(model_override_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/mclib
  ${CMAKE_SOURCE_DIR}/3rdparty/include)
target_compile_features(model_override_tests PRIVATE cxx_std_20)
target_link_libraries(model_override_tests PRIVATE mc2_gate)
add_test(NAME model_override_tests
  COMMAND model_override_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
