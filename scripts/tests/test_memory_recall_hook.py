"""The UserPromptSubmit recall hook (.claude/hooks/memory_recall.py): retired memories are never recalled; hits are framed as historical."""
import importlib.util
import io
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("memory_recall", ROOT / ".claude/hooks/memory_recall.py")
hook = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hook)


def _mem(root, rel, desc):
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"---\nname: x\ndescription: {desc}\n---\nbody\n", encoding="utf-8")


def test_retired_memories_are_skipped_and_header_is_historical(tmp_path, monkeypatch, capsys):
    _mem(tmp_path, "process/lane-slot-acquire-live.md", "lane slot acquire keeps the branch tip")
    _mem(tmp_path, "process/lane-slot-acquire-old.md", "SUPERSEDED (W133): lane slot acquire resets the branch")
    _mem(tmp_path, "process/lane-slot-acquire-older.md", "**HISTORICAL** lane slot acquire one-lane era")
    monkeypatch.setattr(hook, "MEM_DIR", str(tmp_path).replace("\\", "/"))
    monkeypatch.setattr("sys.stdin", io.StringIO(json.dumps({"prompt": "lane slot acquire branch"})))
    hook.main()
    out = capsys.readouterr().out
    assert out.startswith("Historical candidates; verify against current rules:")
    assert "acquire-live.md" in out and "acquire-old.md" not in out and "acquire-older.md" not in out


def test_registered_with_a_five_second_timeout():
    hooks = json.loads((ROOT / ".claude/settings.json").read_text(encoding="utf-8"))["hooks"]["UserPromptSubmit"][0]["hooks"]
    assert any("memory_recall.py" in h["command"] and h["timeout"] == 5 for h in hooks)
