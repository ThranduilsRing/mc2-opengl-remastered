"""Native lighting pins must see calls in files that also define the kernel."""
import copy
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('lighting_divergence', ROOT / 'scripts/check-lighting-divergence.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def test_definition_and_call_in_same_native_shader_are_both_counted(tmp_path, monkeypatch):
    shader = tmp_path / 'mech.vert'
    shader.write_text('vec3 calc_light(int i) { return vec3(1); }\n'
                      'void main() { vec3 c = calc_light(0); }\n')
    key = 'shaders/vulkan/mech_tier1.vert'
    monkeypatch.setattr(checker, 'shader_files', lambda: {key: shader})
    monkeypatch.setattr(checker, 'CPP_FLOOR_GATES', ())
    live = checker.scan()
    assert live['calc_light_definition'] == [key]
    assert live['calc_light_callers'] == [key]


def test_missing_native_call_is_a_broken_census():
    live = checker.scan()
    live['calc_light_callers'] = []
    assert any('calc_light_callers' in problem for problem in checker.self_check(live))


def test_changed_native_floor_fails_without_repinning(monkeypatch):
    live = copy.deepcopy(checker.scan())
    path, name, value = live['ambient_floor_scalar'][0]
    live['ambient_floor_scalar'][0] = (path, name, str(float(value) + 0.1))
    monkeypatch.setattr(checker, 'scan', lambda: live)
    monkeypatch.setattr('sys.argv', ['check-lighting-divergence.py'])
    assert checker.main() == 1


def test_commented_floor_does_not_create_a_second_pin():
    source = 'const float kAmbientFloor = 0.35; /* const float kAmbientOther = 0.7; */'
    assert len(checker.RE_SCALAR_FLOOR.findall(checker.strip_comments(source))) == 1
