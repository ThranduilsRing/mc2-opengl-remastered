#!/usr/bin/env python3
"""Snapshot maintained host tool locations without launching the tools.

PATH, explicit MC2_TOOL_* file overrides, and bounded install-root patterns
are probes, not execution/version tests. Missing optional tools do not fail.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import glob
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
from collections.abc import Callable, Mapping
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts import console_child

BOARD_ROWS = (
    'run_check "verified_ledger" "py -3 tools/verified.py check"',
    'run_check "script_callers" "py -3 tools/callers.py --check"',
)

#: One line per harness tool a lane reaches for on this host; kept here so regeneration never drops them.
HARNESS_NOTES = (
    "- `py -3 scripts/check-staged-hygiene.py --staged` (alias of `--index`): pre-commit scan of the index for new control bytes"
    " (0x00, 0x08, ...), mixed endings and CRLF/LF flips of tracked files; the board runs `--range`.",
)


def collect_inventory(*, which: Callable = shutil.which, find_spec: Callable = importlib.util.find_spec,
                      environ: Mapping[str, str] | None = None, exists: Callable = os.path.isfile,
                      is_dir: Callable = os.path.isdir, glob_paths: Callable = glob.glob,
                      repo_root: Path = ROOT) -> list[dict[str, str]]:
    """All host discovery probes are injectable; no recursive drive scan occurs."""
    env = os.environ if environ is None else environ
    rows: list[dict[str, str]] = []
    pf = [env[key] for key in ("ProgramFiles", "ProgramFiles(x86)") if env.get(key)]
    downloads = str(Path(env.get("USERPROFILE", str(Path.home()))) / "Downloads")

    def expand(patterns):
        for pattern in patterns:
            matches = sorted(glob_paths(pattern), reverse=True) if glob.has_magic(pattern) else [pattern]
            yield from matches

    def probe(label, key, names, patterns=(), note="", directory=False):
        predicate = is_dir if directory else exists
        override = "MC2_TOOL_" + key
        candidates = []
        if env.get(override):
            candidates.append((env[override], override))
        candidates.extend((path, "PATH:" + name) for name in names if (path := which(name)))
        candidates.extend((path, "install root") for path in expand(patterns))
        found = next(((str(Path(path)), source) for path, source in candidates if predicate(path)), None)
        if env.get(override) and not predicate(env[override]):
            note = f"Invalid {override}; fallback discovery used. " + note
        row = {"tool": label, "status": "available" if found else "not found",
               "path": found[0] if found else "", "source": found[1] if found else "",
               "override": override, "note": note}
        rows.append(row)
        return row["path"]

    render_roots = [env["RENDERDOC_ROOT"]] if env.get("RENDERDOC_ROOT") else []
    render_roots += [str(Path(p) / "RenderDoc") for p in pf]
    probe("RenderDoc command", "RENDERDOCCMD", ("renderdoccmd", "renderdoccmd.exe"),
          [str(Path(p) / "renderdoccmd.exe") for p in render_roots])
    probe("RenderDoc embedded API host", "QRENDERDOC", ("qrenderdoc", "qrenderdoc.exe"),
          [str(Path(p) / "qrenderdoc.exe") for p in render_roots],
          "qrenderdoc has an embedded Python API; this does not prove standalone interpreter compatibility.")
    try:
        spec = find_spec("renderdoc")
    except (ImportError, ValueError):
        spec = None
    origin = getattr(spec, "origin", None)
    module_path = None
    note = "Standalone current-interpreter import spec; API execution is not tested."
    if origin and origin not in {"built-in", "frozen"}:
        candidate = Path(origin).resolve()
        if not candidate.is_relative_to(repo_root.resolve()) and exists(str(candidate)):
            module_path = str(candidate)
        else:
            note += " Repository shadow or absent module file rejected."
    elif spec is not None:
        note += " Namespace-only module rejected."
    rows.append({"tool": "RenderDoc Python module", "status": "available" if module_path else "not found",
                 "path": module_path or "", "source": "import spec" if module_path else "", "override": "",
                 "note": note})

    sdk_patterns = [env["VULKAN_SDK"]] if env.get("VULKAN_SDK") else []
    sdk_patterns.append("C:/VulkanSDK/*")
    sdk = probe("Vulkan SDK", "VULKAN_SDK", (), sdk_patterns, directory=True)
    sdk_bins = [str(Path(sdk) / p) for p in ("Bin", "Bin32")] if sdk else []
    for label, key, names in (
        ("gfxrecon-capture", "GFXRECON_CAPTURE", ("gfxrecon-capture.exe", "gfxrecon-capture",
          "gfxrecon-capture.py", "gfxrecon-capture-vulkan.py")),
        ("gfxrecon-replay", "GFXRECON_REPLAY", ("gfxrecon-replay.exe", "gfxrecon-replay")),
        ("vkconfig", "VKCONFIG", ("vkconfig.exe", "vkconfig")),
    ):
        probe(label, key, names, [str(Path(p) / name) for p in sdk_bins for name in names],
              "Python capture launchers require a Python interpreter." if key == "GFXRECON_CAPTURE" else "")
    layers = [p for p in env.get("VK_LAYER_PATH", "").split(os.pathsep) if p] + sdk_bins
    if sdk:
        layers.append(str(Path(sdk) / "ExplicitLayers"))
    probe("Vulkan validation layer manifest", "VALIDATION_LAYER", (),
          [str(Path(p) / name) for p in layers for name in
           ("VkLayer_khronos_validation.json", "VkLayer_KHRONOS_validation.json")],
          "Manifest presence does not prove driver/layer loadability.")

    tracy_roots = [env["TRACY_ROOT"]] if env.get("TRACY_ROOT") else []
    tracy_roots += [str(Path(downloads) / "TracyProfiler"), str(Path(downloads) / "TracyProfiler" / "*")]
    tracy_roots += [str(Path(p) / "TracyProfiler") for p in pf]
    for label, key, names in (
        ("Tracy capture", "TRACY_CAPTURE", ("tracy-capture.exe", "tracy-capture", "capture.exe")),
        ("Tracy csvexport", "TRACY_CSVEXPORT", ("tracy-csvexport.exe", "tracy-csvexport", "csvexport.exe")),
    ):
        probe(label, key, names, [str(Path(p) / name) for p in tracy_roots for name in names])

    vs_roots = [env["VSINSTALLDIR"]] if env.get("VSINSTALLDIR") else []
    vs_roots += [str(Path(p) / "Microsoft Visual Studio" / "*" / "*") for p in pf]
    llvm_roots = [env["LLVM_ROOT"]] if env.get("LLVM_ROOT") else []
    llvm_roots += [str(Path(p) / "LLVM") for p in pf]
    llvm_bins = [str(Path(p) / "bin") for p in llvm_roots]
    llvm_bins += [str(Path(p) / "VC" / "Tools" / "Llvm" / "x64" / "bin") for p in vs_roots]
    llvm_bins += [str(Path(p) / "VC" / "Tools" / "Llvm" / "bin") for p in vs_roots]
    for label, key in (("clang-tidy", "CLANG_TIDY"), ("clang-cl", "CLANG_CL"), ("llvm-symbolizer", "LLVM_SYMBOLIZER")):
        probe(label, key, (label, label + ".exe"), [str(Path(p) / (label + ".exe")) for p in llvm_bins])
    probe("cppcheck", "CPPCHECK", ("cppcheck", "cppcheck.exe"),
          [str(Path(p) / "Cppcheck" / "cppcheck.exe") for p in pf])
    probe("jscpd", "JSCPD", ("jscpd", "jscpd.cmd", "jscpd.exe"),
          [str(Path(env["APPDATA"]) / "npm" / "jscpd.cmd")] if env.get("APPDATA") else [])
    verifier_paths = [str(Path(p) / "Windows Kits" / "10" / "App Certification Kit" / "appverif.exe") for p in pf]
    if env.get("SystemRoot"):
        verifier_paths.append(str(Path(env["SystemRoot"]) / "System32" / "appverif.exe"))
    probe("Application Verifier", "APPVERIF", ("appverif", "appverif.exe"), verifier_paths)

    amd_roots = [env["AMD_DEVTOOLS_ROOT"]] if env.get("AMD_DEVTOOLS_ROOT") else []
    amd_roots += [str(Path(p) / prefix) for p in pf
                  for prefix in ("RadeonDeveloperToolSuite*", "AMD/RadeonDeveloperToolSuite*", "AMD")]
    for label, key, names in (
        ("AMD Radeon Developer Panel", "RDP", ("RadeonDeveloperPanel.exe",)),
        ("AMD Radeon GPU Profiler (RGP)", "RGP", ("RadeonGPUProfiler.exe", "RadeonGpuProfiler.exe", "rgp.exe")),
        ("AMD Radeon GPU Detective (RGD)", "RGD", ("rgd.exe", "RadeonGPUDetective.exe")),
    ):
        probe(label, key, names, [str(Path(p) / sub / name) for p in amd_roots
                                 for sub in ("", "*", "*/*") for name in names])
    vc_roots = [env["VCToolsInstallDir"]] if env.get("VCToolsInstallDir") else []
    vc_roots += [str(Path(p) / "VC" / "Tools" / "MSVC" / "*") for p in vs_roots]
    probe("dumpbin", "DUMPBIN", ("dumpbin", "dumpbin.exe"),
          [str(Path(p) / "bin" / host / "x64" / "dumpbin.exe") for p in vc_roots for host in ("Hostx64", "Hostx86")])
    return rows


def snapshot_provenance() -> dict[str, str]:
    result = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True,
                            **console_child.child_kwargs(redirected=True))
    return {"utc": datetime.now(timezone.utc).isoformat(timespec="seconds"), "host": platform.node(),
            "python": sys.version.split()[0], "interpreter": sys.executable, "platform": platform.platform(),
            "source_sha": result.stdout.strip() if result.returncode == 0 else "unknown",
            "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


def render_markdown(rows: list[dict[str, str]], provenance: dict[str, str]) -> str:
    lines = ["# Host tool inventory", "", "Generated by `py -3 tools/host_inventory.py --output docs/host-tools.md`.", ""]
    lines += [f"- {key}: `{value}`" for key, value in provenance.items()]
    lines += ["", "This is a host snapshot of file/spec discovery, not version or execution validation.",
              "Overrides take precedence over PATH and maintained install roots. `MC2_TOOL_*` names below accept exact files",
              "(VULKAN_SDK accepts a directory); root hints: VULKAN_SDK, RENDERDOC_ROOT, TRACY_ROOT, LLVM_ROOT,",
              "VSINSTALLDIR, VCToolsInstallDir, AMD_DEVTOOLS_ROOT, VK_LAYER_PATH.", "",
              "| Tool | Status | Location | Discovery / override | Note |", "|---|---|---|---|---|"]
    for row in rows:
        values = [row.get(key, "").replace("|", "\\|").replace("\n", " ") for key in ("tool", "status", "path")]
        values += [row.get("source", "") + " / " + row.get("override", ""), row.get("note", "")]
        lines.append("| " + " | ".join(values) + " |")
    lines += ["", "## Usage and related evidence", "",
              "`py -3 tools/host_inventory.py --json` emits the same snapshot and provenance as JSON; `--output <path>` saves it.",
              "Exit 0 means the inventory completed, including missing optional tools; exit 2 means input/probe/output failure.",
              "Standalone RenderDoc and qrenderdoc's embedded API are separate probes.",
              "SDK Python capture launchers are covered by [LunarG capture tools](https://vulkan.lunarg.com/doc/view/latest/windows/capture_tools.html).",
              "Related: [Vulkan review](../.claude/cleanup/vk-best-practices-review-2026-10-12.md),",
              "[editor inventory](../.claude/cleanup/editor-dialog-inventory.md), [brain status](../.claude/cleanup/abl-brain-status.md),",
              "[proof ledger](../tools/verified.py), [caller inventory](../tools/callers.py).", "",
              "Caller evidence covers statically resolved imports and executable command positions; docs and generated files never satisfy the check.",
              "[Caller exceptions](../tests/callers_allowlist.json) identify pytest discovery, manual entry points and existing cull-review candidates.",
              "Exceptions are explicit baseline entries, not evidence of live use; new uncalled scripts fail until reviewed.",
              "The fixed delivery-input union in callers.py keeps these unstaged tools/tests identical before and after coordinator staging.", "",
              "## Vulkan layers from the SDK: validation and GFXReconstruct", "",
              "Both layers come from the Vulkan SDK rows above; the loader finds them through the SDK installer's registration or `VK_LAYER_PATH` (the SDK `Bin`).",
              "- Validation (`VK_LAYER_KHRONOS_validation`): a Vulkan smoke run with `MC2_VULKAN_VALIDATION=1` grades every message of the layer,",
              "  an error is the red bucket `vk_validation_error`. Recipe, counts and the report line: [Vulkan validation](vk-validation.md).",
              "- GFXReconstruct capture/replay proof: `py -3 tools/visual_verify/vk_replay.py proof --install <install> --bookmark <stem> --out <dir>`.",
              "  `capture` runs one Vulkan bookmark capture (`tools/visual_verify/capture.py` with the Vulkan umbrella, so the engine goes through the harness: background-only,",
              "  host lease) with `VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_gfxreconstruct`, `GFXRECON_CAPTURE_FILE` and `GFXRECON_CAPTURE_FRAMES` set (the variables",
              "  `gfxrecon-capture-vulkan.py` sets; `vk_replay.py env` prints them), `replay` runs `gfxrecon-replay --swapchain offscreen --screenshots <frames>` under the same",
              "  lease (no surface and no window: `--wsi headless` still tries the captured Win32 surface and fails), and `compare` hashes the decoded RGB pixels of the engine's own PNG of the presented frame (the live frame) and of every replayed frame:",
              "  exit 0 when one replayed frame equals the live one, 1 when none does, 2 when a tool failed. `--dry-run` prints the commands.",
              "  The SDK's capture and replay tools have no compare mode of their own; the hash comparison is this script's.", "",
              "## Harness tooling", "", *HARNESS_NOTES, "",
              "## Proposed check-contracts.sh rows (orchestrator only)", "",
              "Regeneration preserves this section by emitting it from the generator. Insert inside the real board before its closing `fi`.",
              "The ledger row validates schema and commit identity without requiring any recorded proof.",
              "The callers row requires current generated data and reasoned exceptions; neither row records a successful engine test.",
              "", "```bash", *BOARD_ROWS, "```", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--json", action="store_true", help="emit structured snapshot instead of Markdown")
    args = parser.parse_args(argv)
    try:
        rows, provenance = collect_inventory(), snapshot_provenance()
        text = (json.dumps({"schema_version": 1, "provenance": provenance, "tools": rows}, indent=2) + "\n"
                if args.json else render_markdown(rows, provenance))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding="utf-8", newline="\n")
        print(text, end="")
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
