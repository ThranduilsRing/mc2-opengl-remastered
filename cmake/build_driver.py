"""Compiler selection and complete stage logs shared by the two build entry points."""
from pathlib import Path
import re
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import configure_ninja as cn
import console_child

PRESETS = ("clang-cl-dev", "clang-cl-nmake", "msvc-ninja-dev")


def is_clang(preset):
    return preset in ("clang-cl-dev", "clang-cl-nmake")


def tree_name(default, preset):
    if preset is None:
        return default
    prefix = "clangcl-nmake" if preset == "clang-cl-nmake" else "clangcl" if is_clang(preset) else "msvc"
    return f"build-{prefix}-{default.removeprefix('build64-').removeprefix('build-')}"


def configure_args(root, src, tree, preset):
    if preset is None:
        return ["-S", str(src), "-B", str(tree), "-A", "x64"]
    args = ["-S", str(src), "-B", str(tree), "-G", "NMake Makefiles" if preset == "clang-cl-nmake" else "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DMC2_CXX_STANDARD=23"]
    if preset != "clang-cl-nmake":
        args += [f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}"]
    if is_clang(preset):
        args += [f"-DCMAKE_TOOLCHAIN_FILE={Path(root).as_posix()}/cmake/clangcl-toolchain.cmake"]
    else:
        args += ["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"]
    return args


def verify_compiler(tree, preset):
    if preset is None:
        return
    expected = "Clang" if is_clang(preset) else "MSVC"
    files = list(Path(tree).glob("CMakeFiles/*/CMakeCXXCompiler.cmake"))
    if len(files) != 1:
        raise RuntimeError(f"{tree}: missing or ambiguous C++ compiler evidence")
    evidence = files[0].read_text(encoding="utf-8")
    def value(key):
        match = re.search(r'set\(' + key + r' "([^"]*)"\)', evidence)
        return match[1] if match else ""
    compiler = value("CMAKE_CXX_COMPILER")
    if value("CMAKE_CXX_COMPILER_ID") != expected:
        raise RuntimeError(f"{tree}: expected {expected}, got {value('CMAKE_CXX_COMPILER_ID')} ({compiler})")
    if expected == "Clang" and (value("CMAKE_CXX_COMPILER_FRONTEND_VARIANT") != "MSVC"
                               or Path(compiler).stem.lower() != "clang-cl"):
        raise RuntimeError(f"{tree}: expected clang-cl MSVC frontend, got {compiler}")
    print(f"[compiler] {tree}: {expected} {value('CMAKE_CXX_COMPILER_VERSION')} {compiler}", flush=True)
    c_files = list(Path(tree).glob("CMakeFiles/*/CMakeCCompiler.cmake"))
    if c_files:
        if len(c_files) != 1:
            raise RuntimeError(f"{tree}: ambiguous C compiler evidence")
        c_evidence = c_files[0].read_text(encoding="utf-8")
        if f'set(CMAKE_C_COMPILER_ID "{expected}")' not in c_evidence:
            raise RuntimeError(f"{tree}: C compiler does not match {expected}")
        if expected == "Clang" and ('set(CMAKE_C_COMPILER_FRONTEND_VARIANT "MSVC")' not in c_evidence
                                    or not re.search(r'set\(CMAKE_C_COMPILER "[^"]*clang-cl(?:\.exe)?"\)', c_evidence, re.I)):
            raise RuntimeError(f"{tree}: C compiler is not clang-cl with the MSVC frontend")


def run(cmd, root, stage, phase, log_dir=None, vcvars=False, capture=False):
    shell = False
    if vcvars:
        cmd = f'call "{cn.VCVARS}" >nul && {subprocess.list2cmdline(cmd)}'
        shell = True
    if log_dir is not None:
        path = Path(log_dir) / f"{stage}.{phase}.log"
        path.parent.mkdir(parents=True, exist_ok=True)
        print(f"[log] {path}", flush=True)
        with path.open("w", encoding="utf-8", newline="\n") as log:
            log.write((cmd if shell else subprocess.list2cmdline(cmd)) + "\n")
            log.flush()
            result = subprocess.run(cmd, cwd=root, shell=shell, stdout=log, stderr=subprocess.STDOUT,
                                    **console_child.child_kwargs(redirected=True))
        output = path.read_text(encoding="utf-8", errors="replace")
        return result.returncode, output
    if capture:
        result = subprocess.run(cmd, cwd=root, shell=shell, capture_output=True, text=True,
                                errors="replace", **console_child.child_kwargs(redirected=True))
        return result.returncode, result.stdout + result.stderr
    return subprocess.run(cmd, cwd=root, shell=shell, **console_child.child_kwargs()).returncode, ""


def build_args(cm, tree, config, targets, jobs=None, preset=None, keep_going=False):
    cmd = [cm, "--build", str(tree), "--config", config]
    if targets:
        cmd += ["--target", *targets]
    if jobs and preset != "clang-cl-nmake":
        cmd += ["--parallel", str(jobs).lstrip("j")]
    if preset and keep_going:
        cmd += ["--", "/K"] if preset == "clang-cl-nmake" else ["--", "-k", "0"]
    return cmd
