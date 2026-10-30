"""Vulkan motion outputs must not be mistaken for GL object IDs."""
import shutil
import subprocess
from pathlib import Path

import pytest
from scripts import console_child

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize('shader,declaration,expected', [
    ('vulkan/vfx_particle.frag', 'layout(location = 2) out vec4 o_motion;', 0),
    ('vulkan/vfx_particle.frag', 'layout(location = 2) out uint o_id;', 1),
    ('vulkan/vfx_particle.frag', 'layout(location = 4) out uvec4 o_id;', 1),
    ('vfx_mesh.frag', 'layout(location = 2) out vec4 o_id;', 1),
])
def test_vfx_output_policy(tmp_path, shader, declaration, expected):
    scripts = tmp_path / 'scripts'
    scripts.mkdir()
    checker = scripts / 'check-vfx-no-objectid.sh'
    shutil.copyfile(ROOT / 'scripts' / checker.name, checker)
    target = tmp_path / 'shaders' / shader
    target.parent.mkdir(parents=True)
    target.write_text(declaration + '\n', encoding='utf-8')
    result = subprocess.run(['sh', str(checker)], capture_output=True, text=True,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == expected, result.stdout + result.stderr
