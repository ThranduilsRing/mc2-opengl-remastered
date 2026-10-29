"""Bridge reader compatibility; no MCP runtime dependency."""
import ast
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def observation_report(data):
    tree = ast.parse((ROOT / "scripts/mcp/mc2_render_state_server.py").read_text(encoding="utf-8"))
    fn = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
              and node.name == "get_runtime_observation")
    fn.decorator_list = []
    namespace = dict(json=json, _latest=lambda: data, _stale_banner=lambda: "",
                     _not_available=lambda: "unavailable")
    exec(compile(ast.Module(body=[fn], type_ignores=[]), "<bridge reader>", "exec"), namespace)
    return namespace[fn.name]()


def test_observation_reader_preserves_counters():
    data = {"frame": 300, "runtime_observation": {"brains": 13, "zone_ns": {"TOTAL": 123}},
            "runtime_resources": {"light_pool": 24, "static_active": 2600}}
    result = json.loads(observation_report(data))
    assert result["observation"] == data["runtime_observation"]
    assert result["resources"] == data["runtime_resources"]


def test_old_and_missing_snapshots():
    assert "predates bridge P2" in observation_report({"frame": 1})
    assert observation_report(None) == "unavailable"
