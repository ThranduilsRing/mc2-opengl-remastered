"""SDR golden generated verbatim from pre-seam shader 8580ab2c45d09e69."""
from pathlib import Path
import re
import subprocess
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child
import shader_common


def run(*args):
    return subprocess.check_output(list(args), text=True,
                                   **console_child.child_kwargs(redirected=True))


@pytest.mark.parametrize("mode", [0, 1, 2])
def test_real_sdr_shader(tmp_path, mode):
    src = ROOT / "shaders/vulkan/postprocess_visual.frag"
    spv, frozen = (str(tmp_path / n) for n in ("post.spv", "frozen.spv"))
    compiler = shader_common.find_tool("glslangValidator")
    run(compiler, "-V", str(src), "-o", spv)
    run(shader_common.find_tool("spirv-opt"), spv, "--set-spec-const-default-value",
        f"0:{mode}", "--freeze-spec-const", "--fold-spec-const-op-composite", "-o", frozen)
    run(shader_common.find_tool("spirv-val"), frozen)
    if mode != 0:
        return
    golden = Path(__file__).with_name("fixtures") / "postprocess_visual_sdr.frag"
    run(compiler, "-V", str(golden), "-o", str(tmp_path / "golden.spv"))
    text = run(compiler, "-E", str(src))
    seam = re.search(r"layout\(constant_id\s*=\s*0\).*?\n}\s*", text, re.S)
    body = re.search(r"if\s*\(OUTPUT_MODE == 0\)\s*{(.*?)\n\s*}\s*return color;\s*}", seam[0], re.S)[1]
    reconstructed, ordinary = re.search(r"if\s*\(reconstructed\) return (.*?);(.*)", body, re.S).groups()
    text = text[:seam.start()] + text[seam.end():]
    text = text.replace("outputTransform(color, true)", reconstructed)
    text = text.replace("color = outputTransform(color, false);", ordinary)
    normalize = lambda s: re.sub(r"\s+", "", re.sub(r"^#.*$", "", s, flags=re.M))
    assert normalize(text) == normalize(run(compiler, "-E", str(golden)))
