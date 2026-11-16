"""Regression tests for the gated layout macro checker's CMake parsing."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "gated_layout", Path(__file__).resolve().parents[1] / "check-gated-layout-macros.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def test_single_parameter_function_expands_at_literal_calls():
    defs = checker.parse_macro_target_defs({
        "helper.cmake": """
            function(enable_layout destination)
                target_compile_definitions(${destination} PRIVATE MC2_LAYOUT=1 OTHER)
                target_compile_definitions(${destination} PUBLIC MC2_SHARED)
            endfunction(enable_layout)
            function(unused destination)
                target_compile_definitions(phantom PRIVATE MC2_UNUSED)
            endfunction()
        """,
        "CMakeLists.txt": """
            target_compile_definitions(direct INTERFACE MC2_DIRECT=0)
            ENABLE_LAYOUT(gameos)
            enable_layout(gameos_editor)
            enable_layout(gameos-extra)
            # enable_layout(commented)
            enable_layout(${dynamic})
            enable_layout(extra arguments)
        """,
    })
    targets = ["gameos", "gameos_editor", "gameos-extra"]
    assert defs == {
        "MC2_DIRECT": [("direct", "INTERFACE", "CMakeLists.txt")],
        "MC2_LAYOUT": [(t, "PRIVATE", "CMakeLists.txt") for t in targets],
        "MC2_SHARED": [(t, "PUBLIC", "CMakeLists.txt") for t in targets],
    }
