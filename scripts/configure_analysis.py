"""configure_analysis.py -- configure-only Ninja tree for clangd (compile_commands.json).
build64 uses the VS generator (no compile_commands). This configures build-analysis/
with Ninja under the vcvars64 env, same cache options as bootstrap_worktree_build.py,
then copies compile_commands.json to the repo root. NEVER builds, never touches build64.
Usage: py -3 scripts/configure_analysis.py
"""
import os, shutil, subprocess, sys

VS = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
CMAKE_DIR = VS + "/Common7/IDE/CommonExtensions/Microsoft/CMake"
VCVARS = VS + "/VC/Auxiliary/Build/vcvars64.bat"


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
    tp = f"{root}/3rdparty"
    cmake = shutil.which("cmake") or CMAKE_DIR + "/CMake/bin/cmake.exe"
    ninja = shutil.which("ninja") or CMAKE_DIR + "/Ninja/ninja.exe"
    args = [cmake, "-S", root, "-B", f"{root}/build-analysis", "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DMC2_IMGUI=ON",
            f"-DCMAKE_PREFIX_PATH={tp}/cmake",
            f"-DGLEW_INCLUDE_DIR={tp}/include",
            f"-DGLEW_LIBRARY={tp}/lib/x64/glew32.lib",
            f"-DGLEW_SHARED_LIBRARY_RELEASE={tp}/lib/x64/glew32.lib",
            f"-DGLEW_STATIC_LIBRARY_RELEASE={tp}/lib/x64/glew32s.lib",
            f"-DZLIB_INCLUDE_DIR={tp}/include",
            f"-DZLIB_LIBRARY={tp}/lib/x64/zlib.lib"]
    cmd = f'call "{VCVARS}" >nul && ' + subprocess.list2cmdline(args)
    r = subprocess.run(cmd, shell=True)
    if r.returncode:
        sys.exit("configure failed")
    src = f"{root}/build-analysis/compile_commands.json"
    shutil.copyfile(src, f"{root}/compile_commands.json")
    print("OK -> compile_commands.json")


if __name__ == "__main__":
    main()
