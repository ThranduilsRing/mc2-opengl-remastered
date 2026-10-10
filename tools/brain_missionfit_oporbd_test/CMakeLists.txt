# BRAIN-MISSIONFIT-OPORD-CONSUMER-1: standalone unit test for the pure mission.fit
# OPORD parser. Engine-free — compiles only the parser TU + the test driver.
cmake_minimum_required(VERSION 3.20)
project(brain_missionfit_oporbd_test CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")

if(NOT DEFINED REPO_ROOT)
    get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

add_executable(brain_missionfit_oporbd_test
    main.cpp
    "${REPO_ROOT}/code/brain_missionfit_oporbd.cpp"
)
mc2_link_gate(brain_missionfit_oporbd_test)
target_include_directories(brain_missionfit_oporbd_test PRIVATE "${REPO_ROOT}/code")
