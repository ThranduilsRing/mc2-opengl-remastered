"""Read-only executor reports use native failures after GL ambient probes retire."""
import ast
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]


def report(frame_graph):
    # Exercise the actual formatter without starting the MCP server or loading it.
    tree = ast.parse((ROOT / 'scripts/mcp/mc2_render_state_server.py').read_text(encoding='utf-8'))
    function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                    and node.name == 'get_executor_health')
    function.decorator_list = []
    namespace = dict(_latest=lambda: {'frame': 300, 'frame_graph': frame_graph},
        _stale_banner=lambda: '', _file_age_s=lambda: 0,
        _get_liveness=lambda: {'live': True})
    exec(compile(ast.Module(body=[function], type_ignores=[]), '<executor formatter>', 'exec'), namespace)
    return namespace['get_executor_health']()


def test_native_clean_report_does_not_claim_retired_ambient_samples():
    result = report({'fbo_samples': 100, 'fbo_mismatches': 0,
                     'executor_validated_top_level_passes': 100, 'executor_top_level_failures': 0})
    assert result.startswith('EXECUTOR CLEAN')
    assert 'Native target availability' in result
    assert 'ambient_probe_samples:' not in result
    assert 'viewport_probe_samples:' not in result


@pytest.mark.parametrize('field', ['executor_top_level_failures', 'executor_validation_failures', 'fbo_mismatches'])
def test_native_failure_cannot_be_reported_clean(field):
    result = report({'fbo_samples': 100, field: 2})
    assert result.startswith('EXECUTOR FAIL')
    assert '=2' in result.splitlines()[0]
