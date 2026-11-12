"""Exercise production shutdown/font-release bodies with renderer lifetime sentinels."""
from pathlib import Path
import os
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import configure_ninja
import console_child


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@pytest.mark.parametrize("mutation,expected", [("none", 0), ("dangling", 11), ("unguarded", 12)])
def test_font_release_after_renderer_shutdown(tmp_path, mutation, expected):
    graphics = (ROOT / "GameOS/gameos/gameos_graphics.cpp").read_text(encoding="utf-8")
    capi = (ROOT / "GameOS/gameos/gameos_graphics_capi.cpp").read_text(encoding="utf-8")
    shutdown = function(graphics, "void gos_DestroyRenderer()")
    release = function(capi, "void __stdcall gos_DeleteFont(")
    if mutation == "dangling":
        shutdown = shutdown.replace("g_gos_renderer = nullptr;", "")
    if mutation == "unguarded":
        release = release.replace("if (!g_gos_renderer)\n        return;", "")
    source = r'''
#include <cstdlib>
#define __stdcall
#define gosASSERT(x) do { if (!(x)) std::exit(10); } while (false)
namespace mc2 { namespace overlap {
enum class Owner { Render };
void assertOwner(Owner, const char*) {}
}}
namespace mc2_debug_state { void writeShutdownState() {} }
namespace mc2_diag { void shutdown() {} }
struct gosPostProcess { void destroy() {} };
gosPostProcess* getGosPostProcess() { return nullptr; }
struct gosFont {};
using HGOSFONT3D = gosFont*;
bool alive = false;
int releases = 0;
struct gosRenderer {
    gosRenderer() { alive = true; }
    void destroy() { if (!alive) std::exit(13); }
    ~gosRenderer() { alive = false; }
    void deleteFont(gosFont*) {
        ++releases;
    }
};
gosRenderer* g_gos_renderer = nullptr;
gosRenderer* getGosRenderer() {
    // Reject invalid access before a member call, including the negative controls.
    if (!g_gos_renderer) std::exit(12);
    if (!alive) std::exit(11);
    return g_gos_renderer;
}
'''
    source += shutdown + "\n" + release + r'''
int main() {
    gosFont font;
    // Before creation, then a live release, then three late static template owners.
    gos_DeleteFont(&font);
    g_gos_renderer = new gosRenderer;
    gos_DeleteFont(&font);
    if (releases != 1) return 14;
    gos_DestroyRenderer();
    gos_DeleteFont(&font);
    gos_DeleteFont(&font);
    gos_DeleteFont(&font);
    return releases == 1 && !alive && !g_gos_renderer ? 0 : 15;
}
'''
    # The unguarded control must reach the post-destruction call, not the pre-init call.
    if mutation == "unguarded":
        source = source.replace("    gos_DeleteFont(&font);\n    g_gos_renderer = new", "    g_gos_renderer = new", 1)
    cpp = tmp_path / "font_shutdown.cpp"
    exe = tmp_path / "font_shutdown.exe"
    cpp.write_text(source, encoding="utf-8")
    if os.name == "nt":
        args = ["cl", "/nologo", "/EHsc", "/Od", str(cpp),
                "/Fo" + str(tmp_path / "font_shutdown.obj"), "/Fe" + str(exe)]
        assert configure_ninja.under_vcvars(subprocess.list2cmdline(args)) == 0
    else:
        subprocess.run(["c++", "-O0", "-fno-delete-null-pointer-checks", str(cpp), "-o", str(exe)], check=True)
    result = subprocess.run([str(exe)], **console_child.child_kwargs())
    assert result.returncode == expected
