#!/usr/bin/env python3
"""Index statically resolvable executable callers of scripts/ and tools/.

Imports and command positions count; comments, target names, docs, fixture
strings and generated inventories do not. Dynamic dispatch is conservative:
unresolved paths need a reasoned allowlist entry, not a fabricated caller.
Exit 0: current/written; 1: stale or uncalled; 2: input/discovery failure.
"""
from __future__ import annotations

import argparse
import ast
from collections import defaultdict
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import shlex
import subprocess
import sys
import warnings
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts import console_child

SCRIPT_SUFFIXES = {".py", ".pyw", ".ps1", ".sh", ".bash", ".zsh", ".bat", ".cmd", ".pl", ".js", ".mjs", ".cjs", ".rb", ".vbs"}
EXCLUDED = {"tests/callers.json", "tests/callers_allowlist.json", "tests/verified.json", "tools/inventory.json"}
# These delivery inputs already exist before the coordinator can stage them.
# The fixed union keeps the pre-commit and post-commit snapshot identical.
DELIVERY_INPUTS = {"tools/callers.py", "tools/verified.py", "tools/host_inventory.py",
                   "tests/test_callers.py", "tests/test_verified.py", "tests/test_host_inventory.py",
                   "scripts/tests/test_contract_coverage.py", "scripts/tests/test_raw_gl_firewall.py",
                   "scripts/tests/test_deploy_disk_space.py", "scripts/tests/test_delivery_gate_defaults.py",
                   "scripts/tests/test_board_selftests.py", "scripts/tests/test_contract_harness_migration.py"}


def tracked_files(root: Path) -> list[str]:
    result = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            **console_child.child_kwargs(redirected=True))
    paths = {p.replace("\\", "/") for p in result.stdout.decode("utf-8").split("\0") if p}
    paths.update(p for p in DELIVERY_INPUTS if (root / p).is_file())
    return sorted(p for p in paths if (root / p).is_file())


def _remove_comment(line: str, suffix: str) -> str:
    if suffix in {".bat", ".cmd"}:
        return "" if re.match(r"\s*@?\s*(?:REM\b|::)", line, re.I) else line
    marker = "//" if suffix in {".js", ".mjs", ".cjs", ".ts", ".cpp", ".h"} else "#"
    quote = ""
    escaped = False
    for i, char in enumerate(line):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
        elif char in "'\"`":
            quote = char
        elif line.startswith(marker, i):
            return line[:i]
    return line


def _repo_relative(root: Path, reference: str) -> str:
    """Repo-relative POSIX form of a reference, so inventories are worktree-independent."""
    text = reference.replace("\\", "/")
    prefix = root.as_posix().rstrip("/") + "/"
    if text.casefold().startswith(prefix.casefold()):
        text = text[len(prefix):]
    return text


def _relative_inventory(root: Path, inventory: dict[str, Any]) -> dict[str, Any]:
    """Copy of an inventory whose references are all repo-relative POSIX paths."""
    normalized = json.loads(json.dumps(inventory))
    for item in normalized.get("scripts", []):
        for bucket in ("callers_by_basename", "callers_by_stem"):
            for hit in item.get(bucket, []):
                if isinstance(hit.get("reference"), str):
                    hit["reference"] = _repo_relative(root, hit["reference"])
    return normalized


class Targets:
    def __init__(self, scripts, source, root):
        self.scripts = set(scripts)
        self.source, self.root = source, root
        self.names = defaultdict(list)
        self.stems = defaultdict(list)
        for path in scripts:
            self.names[PurePosixPath(path).name].append(path)
            self.stems[PurePosixPath(path).stem].append(path)

    def path(self, reference):
        reference = reference.replace("\\", "/").strip("\"'")
        root = self.root.as_posix().rstrip("/") + "/"
        if reference.casefold().startswith(root.casefold()):
            reference = reference[len(root):]
        current = "${CMAKE_CURRENT_SOURCE_DIR}/"
        if reference.startswith(current):
            reference = posixpath.join(posixpath.dirname(self.source), reference[len(current):])
        reference = re.sub(r"^\$\{?(?:ROOT|REPO_ROOT|PROJECT_SOURCE_DIR|CMAKE_SOURCE_DIR)\}?/", "", reference)
        normalized = posixpath.normpath(reference)
        if "/" in reference:
            candidates = [normalized, posixpath.normpath(posixpath.join(posixpath.dirname(self.source), reference))]
            return next((p for p in candidates if p in self.scripts), None)
        local = posixpath.join(posixpath.dirname(self.source), reference)
        if local in self.scripts:
            return local
        candidates = self.names.get(reference, [])
        return candidates[0] if len(candidates) == 1 else None

    def module(self, reference):
        path = reference.replace(".", "/")
        for candidate in (path + ".py", path + "/__init__.py"):
            if candidate in self.scripts:
                return candidate
        if "." not in reference:
            local = posixpath.join(posixpath.dirname(self.source), reference + ".py")
            if local in self.scripts:
                return local
            candidates = [p for p in self.stems.get(reference, []) if p.endswith(".py")]
            if len(candidates) == 1:
                return candidates[0]
        return None


def _shell_refs(command: str):
    """Only script operands of interpreters or commands at statement starts."""
    try:
        lexer = shlex.shlex(command.replace("\\", "/"), posix=True, punctuation_chars=";&|")
        lexer.whitespace_split = True
        lexer.commenters = ""
        tokens = list(lexer)
    except ValueError:
        return []
    refs = []
    active = False
    start = True
    for token in tokens:
        if token in {"&&", "||", ";", "|", "then", "do"}:
            active, start = False, True
            continue
        clean = token.rstrip(");")
        if clean == "__unresolved_argument__":
            active, start = False, False
            continue
        exe = PurePosixPath(clean).name.casefold().strip('"')
        if start and (re.fullmatch(r"(?:py|python[0-9.]*|python\.exe|py\.exe|bash|sh|pwsh|powershell|powershell\.exe|node|ruby|perl|qrenderdoc\.exe)", exe) or re.fullmatch(r"\$\{?(?:PY|PYTHON|Python3_EXECUTABLE|BASH|BASH_EXECUTABLE|PWSH)\}?", clean, re.I)):
            active, start = True, False
            continue
        if active and clean in {"-c", "-Command", "-EncodedCommand"}:
            active = False
            continue
        if clean == "-m" and active:
            refs.append(("module-next", ""))
            continue
        if refs and refs[-1][0] == "module-next":
            refs[-1] = ("module", clean)
            active = False
            continue
        if PurePosixPath(clean).suffix.lower() in SCRIPT_SUFFIXES and (active or start):
            refs.append(("path", clean.lstrip("@")))
            active = False
        if not clean.startswith("-"):
            start = clean in {"call", "&", ".", "source", "exec", "env"} or (start and "=" in clean)
    return refs


def _qualified(node):
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        return _qualified(node.value) + "." + node.attr
    return ""


def _python_refs(content, source, root):
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", SyntaxWarning)
            tree = ast.parse(content, filename=source)
    except SyntaxError:
        return []
    scopes = {tree: {"parent": None, "bindings": {}, "aliases": {}}}
    contexts = {}
    refs = []

    def locate(node, scope):
        contexts[node] = scope
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Lambda)):
            parent = scopes[scope]["parent"] if isinstance(scope, ast.ClassDef) else scope
            scopes[node] = {"parent": parent, "bindings": {}, "aliases": {}}
            scope = node
            if hasattr(node, "args"):
                parameters = [*node.args.posonlyargs, *node.args.args, *node.args.kwonlyargs]
                parameters += [p for p in (node.args.vararg, node.args.kwarg) if p]
                for parameter in parameters:
                    scopes[scope]["bindings"][parameter.arg] = [None]
        for child in ast.iter_child_nodes(node):
            locate(child, scope)

    locate(tree, tree)

    def lookup(node, name, key):
        scope = contexts[node]
        while scope is not None:
            entries = scopes[scope][key]
            if name in entries:
                return True, entries[name], scope
            if key == "aliases" and name in scopes[scope]["bindings"]:
                return True, "", scope  # local variables/parameters shadow module aliases
            scope = scopes[scope]["parent"]
        return False, None, None

    def value(node, seen=frozenset()):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return node.value
        if _qualified(node) == "sys.executable":
            present, alias, _ = lookup(node, "sys", "aliases")
            return "python" if present and alias == "sys" else None
        if isinstance(node, ast.Name):
            present, entries, scope = lookup(node, node.id, "bindings")
            identity = (scope, node.id)
            if present:
                if identity in seen or any(entry is None for entry in entries):
                    return None
                distinct = {ast.dump(entry) for entry in entries}
                if len(distinct) == 1:
                    return value(entries[0], seen | {identity})
                return None  # branch/reassignment ambiguity is not executable evidence
            if node.id == "__file__":
                return str(root / source)
            if node.id in {"ROOT", "REPO_ROOT", "REPO"}:
                return str(root)
        if isinstance(node, ast.Attribute) and node.attr == "parent":
            parent = value(node.value, seen)
            if isinstance(parent, str):
                return str(Path(parent).parent)
        if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Attribute) and node.value.attr == "parents":
            parent = value(node.value.value, seen)
            if isinstance(parent, str) and isinstance(node.slice, ast.Constant) and isinstance(node.slice.value, int):
                try:
                    return str(Path(parent).parents[node.slice.value])
                except IndexError:
                    return None
        if isinstance(node, (ast.List, ast.Tuple)):
            return [value(x, seen) for x in node.elts]
        if isinstance(node, ast.BinOp):
            left, right = value(node.left, seen), value(node.right, seen)
            if isinstance(node.op, ast.Add) and isinstance(left, list) and isinstance(right, list):
                return left + right
            if isinstance(left, str) and isinstance(right, str):
                if isinstance(node.op, ast.Div):
                    return left.rstrip("/") + "/" + right
                if isinstance(node.op, ast.Add):
                    return left + right
        if isinstance(node, ast.Call):
            name = _qualified(node.func)
            if name in {"str", "Path", "pathlib.Path"} and node.args:
                return value(node.args[0], seen)
            if isinstance(node.func, ast.Attribute) and node.func.attr in {"resolve", "absolute"}:
                return value(node.func.value, seen)
            if name in {"os.path.join", "posixpath.join"}:
                parts = [value(arg, seen) for arg in node.args]
                if all(isinstance(p, str) for p in parts):
                    return "/".join(parts)
        return None

    for node in ast.walk(tree):
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    scopes[contexts[node]]["bindings"].setdefault(target.id, []).append(node.value)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            scopes[contexts[node]]["bindings"].setdefault(node.target.id, []).append(node.value)
        if isinstance(node, ast.Import):
            for alias in node.names:
                scopes[contexts[node]]["aliases"][alias.asname or alias.name] = alias.name
                refs.append((node.lineno, "module", alias.name, "import"))
        elif isinstance(node, ast.ImportFrom):
            module = node.module or ""
            if node.level:
                parent = PurePosixPath(source).parent.parts
                module = ".".join((*parent[:len(parent) - node.level + 1], *filter(None, module.split("."))))
            refs.append((node.lineno, "module", module, "import"))
            for alias in node.names:
                qualified = module + "." + alias.name
                scopes[contexts[node]]["aliases"][alias.asname or alias.name] = qualified
                refs.append((node.lineno, "module", qualified, "import"))
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        name = _qualified(node.func)
        head, _, tail = name.partition(".")
        present, alias, _ = lookup(node, head, "aliases")
        name = (alias if present else head) + ("." + tail if tail else "")
        if name in {"importlib.import_module", "__import__"} and node.args:
            module = value(node.args[0])
            if isinstance(module, str):
                refs.append((node.lineno, "module", module, "import_module"))
        elif name in {"importlib.util.spec_from_file_location", "runpy.run_path"}:
            index = 1 if name.endswith("spec_from_file_location") else 0
            if len(node.args) > index and isinstance(ref := value(node.args[index]), str):
                refs.append((node.lineno, "path", ref, "load module"))
        elif name in {"subprocess.run", "subprocess.Popen", "subprocess.call", "subprocess.check_call",
                      "subprocess.check_output", "os.system", "os.popen", "nofocus.spawn"}:
            command = value(node.args[0]) if node.args else None
            if isinstance(command, list):
                command = " ".join(shlex.quote(x) if isinstance(x, str) else "__unresolved_argument__" for x in command)
            if isinstance(command, str):
                refs.extend((node.lineno, kind, ref, "command") for kind, ref in _shell_refs(command))
    return refs


def _config_refs(content):
    try:
        config = json.loads(content)
    except ValueError:
        return []
    refs = []
    def walk(value):
        if isinstance(value, dict):
            if isinstance(value.get("command"), str):
                command = value["command"]
                if isinstance(value.get("args"), list):
                    command += " " + " ".join(shlex.quote(x) for x in value["args"] if isinstance(x, str))
                line = next((n for n, text in enumerate(content.splitlines(), 1)
                             if json.dumps(value["command"]) in text), 1)
                refs.extend((line, kind, ref, "config command") for kind, ref in _shell_refs(command))
            for child in value.values():
                walk(child)
        elif isinstance(value, list):
            for child in value:
                walk(child)
    walk(config)
    return refs


def driven_target_sources(cmake: str, runner: str) -> set[str]:
    """Sources explicitly registered to executables the runner builds and runs."""
    cmake = re.sub(r'#[^\n]*', '', cmake)
    runner = "\n".join(_remove_comment(line, ".py") for line in runner.splitlines())
    covered = set()
    for target, body in re.findall(r'\badd_executable\s*\(\s*(\w+)\b([^)]*)\)', cmake):
        built = re.search(r'[\'"]--target[\'"]\s*,\s*[\'"]' + re.escape(target) + r'[\'"]', runner)
        executed = re.search(r'[\'"][^\'"\n]*/' + re.escape(target) + r'\.exe[\'"]', runner)
        if built and executed:
            covered.update(re.findall(r'\btests/unit/(test_[\w]+\.cpp)\b', body))
    return covered


def build_inventory(root: Path, paths: list[str] | None = None, *, excluded=(), read_text=None) -> dict[str, Any]:
    root = Path(root).resolve()
    files = sorted(set(p.replace("\\", "/") for p in (tracked_files(root) if paths is None else paths)))
    omit = EXCLUDED | set(excluded)
    scripts = sorted(p for p in files if p.split("/")[0] in {"scripts", "tools"}
                     and PurePosixPath(p).suffix.lower() in SCRIPT_SUFFIXES)
    items = {p: {"path": p, "basename": PurePosixPath(p).name, "stem": PurePosixPath(p).stem,
                 "callers_by_basename": [], "callers_by_stem": []} for p in scripts}
    found = {}
    targets = Targets(scripts, "", root)   # index built once; only `source` changes per file
    for source in files:
        if source in omit or PurePosixPath(source).name in {"callers.json", "callers_allowlist.json"}:
            continue
        suffix = PurePosixPath(source).suffix.lower()
        cmake = suffix == ".cmake" or PurePosixPath(source).name == "CMakeLists.txt"
        hook = "/hooks/" in source or source.startswith(".githooks/")
        if suffix not in SCRIPT_SUFFIXES | {".json", ".yaml", ".yml"} and not cmake and not hook:
            continue
        try:
            content = read_text(source) if read_text else (root / source).read_text(encoding="utf-8-sig", errors="replace")
        except (OSError, UnicodeError):
            continue
        lines = content.splitlines()
        targets.source = source
        if suffix in {".py", ".pyw"}:
            refs = _python_refs(content, source, root)
        elif suffix == ".json":
            config_name = PurePosixPath(source).name
            live_config = config_name in {".mcp.json", "mcp.json"} or (
                source.startswith((".claude/", ".vscode/", ".githooks/"))
                and config_name in {"settings.json", "settings.local.json", "tasks.json", "hooks.json"})
            refs = _config_refs(content) if live_config else []
        else:
            refs = []
            if cmake:
                clean_content = "\n".join(_remove_comment(line, suffix) for line in lines)
                for match in re.finditer(r"\bCOMMAND\s+([\s\S]*?)(?=\b(?:COMMAND|COMMENT|WORKING_DIRECTORY|DEPENDS|VERBATIM|OUTPUT)\b|\n\s*\)|\Z)", clean_content):
                    n = clean_content[:match.start()].count("\n") + 1
                    refs.extend((n, kind, ref, "cmake command") for kind, ref in _shell_refs(match.group(1)))
            for n, original in enumerate(lines, 1):
                line = _remove_comment(original, suffix)
                if cmake:
                    if match := re.search(r"\binclude\s*\(([^)]+)", line, re.I):
                        refs.append((n, "path", match.group(1).strip(), "cmake include"))
                    continue
                if re.match(r"\s*(?:run_check\w*|run_nightly|add_check)\s+", line):
                    quoted = re.findall(r'"([^"\n]*)"', line)
                    line = quoted[-1] if quoted else ""
                refs.extend((n, kind, ref, "command") for kind, ref in _shell_refs(line))
        for n, kind, ref, evidence in refs:
            target = targets.module(ref) if kind == "module" else targets.path(ref)
            if not target or target == source:
                continue
            bucket = "callers_by_stem" if kind == "module" else "callers_by_basename"
            key = target, bucket, source, n, evidence
            found[key] = {"path": source, "line": n, "evidence": evidence,
                          "reference": _repo_relative(root, ref),
                          "text": lines[n - 1].strip()[:240] if n <= len(lines) else ""}
    for (target, bucket, _, _, _), hit in sorted(found.items()):
        items[target][bucket].append(hit)
    return {"schema_version": 2, "scripts": [items[p] for p in scripts]}


#: A test module under scripts/tests is collected by the scripts pytest suite (scripts/run_scripts_tests.py), so
#: pytest discovery is its caller by definition: a new test file needs no allowlist row and no folding.
PYTEST_COLLECTED = re.compile(r"^scripts/tests/test_[^/]+\.py$")


def uncalled_scripts(inventory, allowlist=None):
    return [item["path"] for item in inventory["scripts"]
            if not item["callers_by_basename"] and not item["callers_by_stem"] and item["path"] not in (allowlist or {})
            and not PYTEST_COLLECTED.match(item["path"])]


def check_inventory(inventory, allowlist=None):
    errors = [f"tracked script has no caller and is not allowlisted: {p}" for p in uncalled_scripts(inventory, allowlist)]
    known = {item["path"] for item in inventory["scripts"]}
    errors += [f"allowlist entry is not a tracked script: {p}" for p in sorted(set(allowlist or {}) - known)]
    return errors


def _unique_allow_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate allowlist key: {key}")
        result[key] = value
    return result


def _read_allowlist(path):
    if not path.exists():
        return {}
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_allow_keys)
    if not isinstance(value, dict) or any(not isinstance(k, str) or not isinstance(v, str) or not v.strip() for k, v in value.items()):
        raise ValueError("caller allowlist must map paths to nonempty reasons")
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--allowlist", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    # The inventory is computed at check time and not tracked (it churned on every lane and conflicted at merges);
    # --output dumps it to a file for inspection only.
    output = args.output
    allowlist_path = args.allowlist or root / "tests/callers_allowlist.json"
    try:
        excluded = [p.resolve().relative_to(root).as_posix() for p in (output, allowlist_path) if p and p.resolve().is_relative_to(root)]
        inventory = build_inventory(root, excluded=excluded)
        allowlist = _read_allowlist(allowlist_path)
        if output:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8", newline="\n")
        if args.check:
            errors = check_inventory(inventory, allowlist)
            if errors:
                print("\n".join(errors), file=sys.stderr)
                return 1
            print(f"caller inventory current: {len(inventory['scripts'])} scripts")
        else:
            print(f"{len(inventory['scripts'])} scripts, {len(uncalled_scripts(inventory, allowlist))} unallowlisted without callers")
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
