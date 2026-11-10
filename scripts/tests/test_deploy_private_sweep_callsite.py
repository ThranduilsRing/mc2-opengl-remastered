"""Pin the deploy path's call to the stale `.private-<pid>` sweep (the sweep itself is tested in test_deploy_private_sweep)."""
import ast
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "deploy_payload.py"


def _sweep_call_sites(source):
    """(enclosing function, line) of every sweep_stale_private_temps(...) call."""
    tree = ast.parse(source)
    sites = []
    for fn in ast.walk(tree):
        if isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
            for node in ast.walk(fn):
                if (isinstance(node, ast.Call) and getattr(node.func, "id", None) == "sweep_stale_private_temps"
                        and fn.name != "sweep_stale_private_temps"):
                    sites.append((fn.name, node.lineno))
    return sites


def _from_payload_line(source, func):
    tree = ast.parse(source)
    for fn in ast.walk(tree):
        if isinstance(fn, ast.FunctionDef) and fn.name == func:
            for node in ast.walk(fn):
                if (isinstance(node, ast.Attribute) and node.attr == "from_payload"
                        and isinstance(node.ctx, ast.Load)):
                    return node.lineno
    return None


def test_sweep_is_called_on_the_deploy_path_before_from_payload():
    source = SRC.read_text(encoding="utf-8")
    sites = _sweep_call_sites(source)
    assert sites, "deploy_payload no longer calls sweep_stale_private_temps: killed-deploy temps would accumulate"
    func, line = sites[0]
    assert func == "main", f"sweep moved out of main() into {func}()"
    fp = _from_payload_line(source, func)
    assert fp is not None and line < fp, "sweep must run before the --from-payload branch"


def test_the_pin_fails_when_the_call_is_removed():
    source = SRC.read_text(encoding="utf-8").replace("sweep_stale_private_temps(target)", "pass", 1)
    assert not _sweep_call_sites(source)
