"""The compiled CAS path must reach cubic reconstruction at reduced scale."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child


def test_compiled_cas_reconstructs_every_tap(tmp_path):
    sdk = Path(os.environ.get("VULKAN_SDK", "")) / "Bin"
    compiler = shutil.which("glslangValidator") or str(sdk / "glslangValidator.exe")
    disassembler = shutil.which("spirv-dis") or str(sdk / "spirv-dis.exe")
    spv = tmp_path / "post.spv"
    subprocess.run([compiler, "-V", "-o", str(spv),
                    str(ROOT / "shaders/vulkan/postprocess_visual.frag")], check=True,
                   capture_output=True, **console_child.child_kwargs(redirected=True))
    text = subprocess.check_output([disassembler, str(spv)], text=True,
                                  **console_child.child_kwargs(redirected=True))
    functions = {}
    for name, body in re.findall(r"(%\w+) = OpFunction .*?\n(.*?)OpFunctionEnd", text, re.S):
        functions[name] = re.findall(r"OpFunctionCall %\w+ (%\w+)", body)
    cas = next(calls for name, calls in functions.items() if name.startswith("%casFilter_"))
    assert len(cas) == 9
    for tap in cas:
        assert tap.startswith("%reconstructedColor_")
        assert any(call.startswith("%upscaleColor_") for call in functions[tap])
        assert any(call.startswith("%gradeColor_") for call in functions[tap])
