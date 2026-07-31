#!/usr/bin/env python3
"""flag-registry-auto-gen.py — generate docs/flags.auto.yaml.

Sources:
  1. RenderCore/RendererFeatureRegistry.h (kFeatureTable + kAuxEnvVars
     EnvVarDesc entries) -> registered flags with kind/default/doc.
  2. getenv("MC2_*") / GetEnvironmentVariable*("MC2_*") sweep across
     mclib/ GameOS/ code/ RenderCore/ editor/ -> usage sites.
Cross-checks:
  - scripts/run_smoke.py quoted "MC2_*" literals (smoke env passthrough).
  - docs/tier1_env_vars.md mentions.

Output is deterministic (sorted, no timestamps): re-running on an
unchanged tree produces byte-identical docs/flags.auto.yaml.

Usage: py -3 scripts/flag-registry-auto-gen.py [--out PATH]
"""

import argparse
import os
import re
import sys

GENERATOR_VERSION = "1.0.0"

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
REGISTRY_HEADER = os.path.join("RenderCore", "RendererFeatureRegistry.h")
SWEEP_DIRS = ["mclib", "GameOS", "code", "RenderCore", "editor",
              "RenderWorld", "GameAdapters", "EditorBridge", "GuiRuntime",
              "gui", "mission", "netlib"]
SWEEP_EXTS = {".cpp", ".h", ".hpp", ".c", ".cc", ".inl"}
SMOKE_SCRIPT = os.path.join("scripts", "run_smoke.py")
TIER1_DOC = os.path.join("docs", "tier1_env_vars.md")
DEFAULT_OUT = os.path.join("docs", "flags.auto.yaml")

MAX_REFS = 3


def read_text(relpath):
    with open(os.path.join(REPO_ROOT, relpath), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


# ---------------------------------------------------------------------------
# 1. Registry parse
# ---------------------------------------------------------------------------

# One EnvVarDesc aggregate initializer:
#   { "FEATURE_ID", "ENV_VAR"|nullptr, EnvVarKind::Kind, true|false, "doc" }
ENTRY_RE = re.compile(
    r'\{\s*'
    r'"(?P<feature_id>[^"]*)"\s*,\s*'
    r'(?:"(?P<env_var>[^"]*)"|nullptr)\s*,\s*'
    r'EnvVarKind::(?P<kind>\w+)\s*,\s*'
    r'(?P<default_on>true|false)\s*,\s*'
    r'"(?P<doc>(?:[^"\\]|\\.)*)"\s*'
    r'\}',
    re.DOTALL,
)


def extract_array_body(text, array_name):
    """Return the text between '<array_name>[] = {' and its matching '};'."""
    m = re.search(re.escape(array_name) + r"\[\]\s*=\s*\{", text)
    if not m:
        raise SystemExit("ERROR: array %s not found in %s" % (array_name, REGISTRY_HEADER))
    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return text[start:i - 1]


def parse_registry():
    """Return dict env_var -> registry entry, plus total entry count."""
    text = read_text(REGISTRY_HEADER)
    entries = []
    for array in ("kFeatureTable", "kAuxEnvVars"):
        body = extract_array_body(text, array)
        for m in ENTRY_RE.finditer(body):
            entries.append({
                "feature_id": m.group("feature_id"),
                "env_var": m.group("env_var"),  # None if nullptr
                "kind": m.group("kind"),
                "default_on": m.group("default_on") == "true",
                "doc": m.group("doc"),
                "table": array,
            })
    return entries


# ---------------------------------------------------------------------------
# 2. getenv sweep
# ---------------------------------------------------------------------------

GETENV_RE = re.compile(
    r'(?:getenv|GetEnvironmentVariable[AW]?|_wgetenv|getenv_s'
    r'|envFlag|envFlagDefaultOn|envFlagOn)\s*\(\s*'
    r'L?"(MC2_[A-Z0-9_]+)"')


def sweep_getenv():
    """Return dict flag -> sorted list of 'relpath:line' reference sites."""
    refs = {}
    for d in SWEEP_DIRS:
        droot = os.path.join(REPO_ROOT, d)
        if not os.path.isdir(droot):
            continue
        for dirpath, dirnames, filenames in os.walk(droot):
            dirnames.sort()
            for fn in sorted(filenames):
                if os.path.splitext(fn)[1].lower() not in SWEEP_EXTS:
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, REPO_ROOT).replace("\\", "/")
                try:
                    with open(full, "r", encoding="utf-8", errors="replace") as f:
                        for lineno, line in enumerate(f, 1):
                            for m in GETENV_RE.finditer(line):
                                refs.setdefault(m.group(1), []).append("%s:%d" % (rel, lineno))
                except OSError:
                    continue
    for k in refs:
        refs[k].sort()
    return refs


# ---------------------------------------------------------------------------
# 3. Cross-checks
# ---------------------------------------------------------------------------

def smoke_passthrough_flags():
    text = read_text(SMOKE_SCRIPT)
    return set(re.findall(r'"(MC2_[A-Z0-9_]+)"', text))


def tier1_doc_flags():
    text = read_text(TIER1_DOC)
    return set(re.findall(r'\b(MC2_[A-Z0-9_]+)\b', text))


# ---------------------------------------------------------------------------
# 4. Merge + emit
# ---------------------------------------------------------------------------

def yaml_quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_yaml(registry_entries, sweep_refs, smoke_set, tier1_set):
    by_env = {}
    for e in registry_entries:
        if e["env_var"]:
            by_env[e["env_var"]] = e

    all_flags = sorted(set(by_env) | set(sweep_refs))

    lines = []
    lines.append("# AUTO-GENERATED by scripts/flag-registry-auto-gen.py v%s -- DO NOT EDIT BY HAND." % GENERATOR_VERSION)
    lines.append("# Regenerate: py -3 scripts/flag-registry-auto-gen.py")
    lines.append("# Sources: RenderCore/RendererFeatureRegistry.h (registry),")
    lines.append("#          getenv sweep over mclib/ GameOS/ code/ RenderCore/ editor/,")
    lines.append("#          scripts/run_smoke.py passthrough allowlist, docs/tier1_env_vars.md.")
    lines.append("generator_version: %s" % yaml_quote(GENERATOR_VERSION))
    lines.append("registry_entry_count: %d" % len(registry_entries))
    lines.append("registry_env_flag_count: %d" % len(by_env))
    lines.append("sweep_flag_count: %d" % len(sweep_refs))
    lines.append("flag_count: %d" % len(all_flags))
    lines.append("flags:")
    for flag in all_flags:
        reg = by_env.get(flag)
        refs = sweep_refs.get(flag, [])
        lines.append("  - name: %s" % flag)
        if reg is not None:
            lines.append("    default: %s" % ("on" if reg["default_on"] else "off"))
            lines.append("    kind: %s" % reg["kind"])
            lines.append("    feature_id: %s" % reg["feature_id"])
            lines.append("    declared_in: RenderCore/RendererFeatureRegistry.h (%s)" % reg["table"])
            doc = reg["doc"]
            if len(doc) > 200:
                doc = doc[:197] + "..."
            lines.append("    doc: %s" % yaml_quote(doc))
        else:
            lines.append("    default: unknown")
            lines.append("    declared_in: null")
        if refs:
            lines.append("    referenced_at:")
            for r in refs[:MAX_REFS]:
                lines.append("      - %s" % r)
            if len(refs) > MAX_REFS:
                lines.append("    referenced_at_more: %d" % (len(refs) - MAX_REFS))
        else:
            lines.append("    referenced_at: []")
        lines.append("    registered: %s" % ("yes" if reg is not None else "no"))
        lines.append("    in_smoke_passthrough: %s" % ("yes" if flag in smoke_set else "no"))
        lines.append("    documented_in_tier1_env_vars: %s" % ("yes" if flag in tier1_set else "no"))
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO_ROOT, DEFAULT_OUT))
    args = ap.parse_args()

    registry_entries = parse_registry()
    sweep_refs = sweep_getenv()
    smoke_set = smoke_passthrough_flags()
    tier1_set = tier1_doc_flags()

    out = build_yaml(registry_entries, sweep_refs, smoke_set, tier1_set)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)

    by_env = {e["env_var"] for e in registry_entries if e["env_var"]}
    unregistered = sorted(set(sweep_refs) - by_env)
    print("registry entries: %d (%d with env vars)" % (len(registry_entries), len(by_env)))
    print("sweep flags:      %d" % len(sweep_refs))
    print("unregistered:     %d" % len(unregistered))
    print("wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
