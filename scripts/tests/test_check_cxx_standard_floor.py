"""check-cxx-standard-floor.py: standalone CMake projects that read gates must build as C++20+."""
import importlib.util
import os
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(os.path.dirname(HERE), "check-cxx-standard-floor.py")

spec = importlib.util.spec_from_file_location("check_cxx_standard_floor", SCRIPT)
floor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(floor)


def _write(root, rel, text):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(textwrap.dedent(text))


ROOT_CMAKE = """
    project(mc2)
    set(MC2_CXX_STANDARD 23 CACHE STRING "C++ standard (20|23)")
    add_subdirectory("./mclib" "./out/mclib")
"""


def _tree(tmp_path, cmake_body, source_body, root_cmake=ROOT_CMAKE):
    root = str(tmp_path)
    _write(root, "CMakeLists.txt", root_cmake)
    _write(root, "tools/h/CMakeLists.txt", cmake_body)
    _write(root, "tools/h/h.cpp", source_body)
    return root


def test_standalone_c17_reading_a_gate_fails(tmp_path):
    root = _tree(tmp_path,
                 "project(h CXX)\nset(CMAKE_CXX_STANDARD 17)\nadd_executable(h h.cpp)\n",
                 'int f() { return getenv("MC2_X") != nullptr; }\n')
    assert floor.main(["--root", root]) == 1


def test_standalone_with_shared_include_passes(tmp_path):
    root = _tree(tmp_path,
                 'project(h CXX)\ninclude("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")\n'
                 "add_executable(h h.cpp)\n",
                 'int f() { return getenv("MC2_X") != nullptr; }\n')
    assert floor.main(["--root", root]) == 0


def test_standalone_c17_not_reading_a_gate_passes(tmp_path):
    root = _tree(tmp_path,
                 "project(h CXX)\nset(CMAKE_CXX_STANDARD 17)\nadd_executable(h h.cpp)\n",
                 "int f() { return 1; }\n")
    assert floor.main(["--root", root]) == 0


def test_gate_header_include_counts_as_a_gate_read(tmp_path):
    root = _tree(tmp_path,
                 "project(h CXX)\nset(CMAKE_CXX_STANDARD 17)\nadd_executable(h h.cpp)\n",
                 '#include "Gates/gate.h"\nint f() { return 1; }\n')
    assert floor.main(["--root", root]) == 1


def test_cpp_included_by_a_listed_source_is_followed(tmp_path):
    root = _tree(tmp_path,
                 "project(h CXX)\nset(CMAKE_CXX_STANDARD 17)\nadd_executable(h h.cpp)\n",
                 '#include "inner.cpp"\n')
    _write(root, "tools/h/inner.cpp", 'int f() { return getenv("MC2_X") != nullptr; }\n')
    assert floor.main(["--root", root]) == 1


def test_cxx_std_feature_counts_when_high_enough(tmp_path):
    root = _tree(tmp_path,
                 "project(h CXX)\nadd_executable(h h.cpp)\ntarget_compile_features(h PRIVATE cxx_std_20)\n",
                 'int f() { return getenv("MC2_X") != nullptr; }\n')
    assert floor.main(["--root", root]) == 0


def test_project_added_from_the_root_inherits_its_standard(tmp_path):
    root = str(tmp_path)
    _write(root, "CMakeLists.txt", ROOT_CMAKE)
    _write(root, "mclib/CMakeLists.txt", "project(mclib)\nadd_library(mclib m.cpp)\n")
    _write(root, "mclib/m.cpp", 'int f() { return getenv("MC2_X") != nullptr; }\n')
    assert floor.main(["--root", root]) == 0


def test_root_offering_cxx17_fails(tmp_path):
    root = _tree(tmp_path,
                 'project(h CXX)\ninclude("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")\nadd_executable(h h.cpp)\n',
                 "int f() { return 1; }\n",
                 root_cmake='project(mc2)\nset(MC2_CXX_STANDARD 23 CACHE STRING "C++ standard (17|20|23)")\n')
    assert floor.main(["--root", root]) == 1


def test_the_real_tree_passes():
    assert floor.main([]) == 0
