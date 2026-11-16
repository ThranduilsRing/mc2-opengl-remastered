"""Regression tests for the gated layout macro checker's CMake parsing."""
import importlib.util
from pathlib import Path

import pytest

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


@pytest.mark.parametrize("kind,params", [
    ("function", "destination"), ("function", "destination other"),
    ("function", ""), ("macro", "destination"), ("macro", "destination other"),
])
def test_uncalled_definitions_never_grant_coverage(kind, params):
    text = f"""
        {kind}(unused {params})
            target_compile_definitions(gameos PRIVATE MC2_LAYOUT)
            enable_layout(gameos)
        end{kind}()
        function(enable_layout destination)
            target_compile_definitions(${{destination}} PRIVATE MC2_LAYOUT)
        endfunction()
    """
    assert checker.parse_macro_target_defs({"test": text}) == {}


@pytest.mark.parametrize("fake_call", [
    'message("enable_layout(gameos)")', '# enable_layout(gameos)',
    'message("target_compile_definitions(gameos PRIVATE MC2_LAYOUT)")',
    '# target_compile_definitions(gameos PRIVATE MC2_LAYOUT)',
    'message([=[\nenable_layout(gameos)\n]=])',
    '#[=[\nenable_layout(gameos)\n]=]',
    'message(\n enable_layout(gameos)\n)',
])
def test_literals_and_comments_never_execute_commands(fake_call):
    text = """
        function(enable_layout destination)
            target_compile_definitions(${destination} PRIVATE MC2_LAYOUT)
        endfunction()
    """ + fake_call
    assert checker.parse_macro_target_defs({"test": text}) == {}


@pytest.mark.parametrize("kind", ["function", "macro"])
def test_quoted_helper_argument(kind):
    text = f"""
        {kind}(enable_layout destination)
            target_compile_definitions(${{destination}} PRIVATE MC2_LAYOUT)
        end{kind}()
        enable_layout("gameos") # enable_layout(phantom)
    """
    assert checker.parse_macro_target_defs({"test": text}) == {
        "MC2_LAYOUT": [("gameos", "PRIVATE", "test")]}


@pytest.mark.parametrize("kind", ["function", "macro"])
@pytest.mark.parametrize("ending", ["", "endmacro()", "endfunction()"])
def test_nested_or_unbalanced_definitions_fail_checker(kind, ending, monkeypatch):
    text = (f"{kind}(outer destination)\nfunction(inner)\n"
            "target_compile_definitions(gameos PRIVATE MC2_LAYOUT)\n"
            f"endfunction()\n{ending}\nouter(gameos)")
    monkeypatch.setattr(checker, "read_cmake_text", lambda: {"test": text})
    assert checker.main() == 1


@pytest.mark.parametrize("text", [
    "function(unclosed destination)", "macro(unclosed)",
    "function(wrong)\nendmacro()", "endfunction()",
    "target_compile_definitions(gameos PRIVATE MC2_LAYOUT",
])
def test_unbalanced_blocks_or_commands_fail(text):
    with pytest.raises(ValueError):
        checker.parse_macro_target_defs({"test": text})
