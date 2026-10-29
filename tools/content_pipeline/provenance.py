"""Deterministic, tool-only cook.json records, keyed by output filename."""
import hashlib
import json
from pathlib import Path


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def tool_version(*paths):
    """Pin implementation/dependency bytes rather than a mutable executable path."""
    return sha256(b"".join(bytes.fromhex(sha256(Path(p).read_bytes())) for p in paths))


def record(output, *, source_hash, tool, version, params):
    output = Path(output)
    path = output.parent / "cook.json"
    doc = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {"version": 1, "assets": {}}
    doc["assets"][output.name] = {
        "sourceHash": source_hash, "tool": tool, "toolVersion": version,
        "params": params, "outputHash": sha256(output.read_bytes()),
    }
    temp = path.with_suffix(".json.tmp")
    temp.write_text(json.dumps(doc, sort_keys=True, indent=2) + "\n", encoding="utf-8", newline="\n")
    temp.replace(path)
