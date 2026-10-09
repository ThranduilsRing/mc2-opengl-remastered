# Warnings-as-errors for implicit narrowing, per target.
#
# WHY: size_t->int/long truncation (C4267), lossy arithmetic conversion (C4244),
# printf/scanf format-vs-argument mismatch (C4477), int->pointer of a smaller
# int (C4312) and signed/unsigned compares (C4018) are real 64-bit bugs. Once a
# target reads zero for them they must stay zero; the codes are promoted to
# errors only for targets that are clean, so noisy ones keep building.
#
# To bring a target in: get it to zero, then add its name to the call below.
# Third-party and not-yet-clean targets are deliberately absent.

option(MC2_NARROWING_ERRORS "Promote narrowing/format/pointer-size warnings to errors in clean targets" ON)

set(MC2_NARROWING_WARNING_CODES 4267 4244 4477 4312 4018)

function(mc2_narrowing_errors)
    if(NOT MC2_NARROWING_ERRORS OR NOT MSVC)
        return()
    endif()
    foreach(_target IN LISTS ARGN)
        if(NOT TARGET ${_target})
            message(FATAL_ERROR "mc2_narrowing_errors: no such target '${_target}'")
        endif()
        foreach(_code IN LISTS MC2_NARROWING_WARNING_CODES)
            target_compile_options(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/we${_code}>")
        endforeach()
    endforeach()
endfunction()

mc2_narrowing_errors(
    mlr
    fx_trace
    rendercore
    renderworld
    gameadapters
    editorbridge
    gui
    gui_runtime
    windows
)
