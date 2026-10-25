# cmake/Mc2SimFpContract.cmake
#
# The sim-math numerical contract, enforced on the command line of every first-party translation unit (root project after the
# vendored subdirectories, and every standalone project through Mc2Standalone.cmake).
#
# WHAT: /fp:precise for every compile; under clang-cl also -ffp-contract=off (clang-cl's /fp:precise means -ffp-contract=on,
# which may fuse a*b+c inside one expression; MSVC /fp:precise does not contract). /fp:fast, /fp:contract, /fp:strict,
# -ffast-math, -ffp-model=fast|strict and -ffp-contract=fast|on are refused at configure time when they are in the global flags.
# WHY: the SIMID chain is bit-identical only while every sim TU rounds the same way; a flag that reached some TUs (or all TUs
# under one compiler) would drift it silently. The preprocessor guard that used to live in mclib/math/glm_config.h only ran in TUs
# that included GLM and was bypassed under clang-cl; this is per command line instead. scripts/check-sim-fp-contract.py checks
# the generated compile database (every code/, mclib/ and GameOS/ TU) and the CMake sources.

set(MC2_SIM_FP_FORBIDDEN_RE "[-/](fp:(fast|contract|strict)|ffast-math|ffp-model=(fast|strict)|ffp-contract=(fast|on))")

foreach(_mc2_fp_var CMAKE_CXX_FLAGS CMAKE_C_FLAGS CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS_RELEASE CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_DEBUG CMAKE_C_FLAGS_DEBUG)
    if("${${_mc2_fp_var}}" MATCHES "${MC2_SIM_FP_FORBIDDEN_RE}")
        message(FATAL_ERROR "${_mc2_fp_var} carries '${CMAKE_MATCH_0}': the sim-math contract is /fp:precise without contraction "
                            "(cmake/Mc2SimFpContract.cmake)")
    endif()
endforeach()

if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/fp:precise>")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/clang:-ffp-contract=off>")
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fno-fast-math>"
                        "$<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>")
endif()
