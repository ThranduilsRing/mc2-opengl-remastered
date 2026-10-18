#!/usr/bin/env python3
"""SUBSYSTEM-HARNESS-ARC / DEPLOY-ASSET-CONTRACT-HARNESS-1

First Python harness on the arc. Validates the deploy payload's FILE CONTRACT by
importing the REAL constants from scripts/deploy_payload.py (the single source of
truth) and checking them against the filesystem — no duplicated path lists, no
deploy execution, no GL, no game.

Default suite covers the source-tracked payload (must exist in the repo):
  SUPPORT_SCRIPTS, EDITOR_SUPPORT_TREES, GAME_COOK_TOOLS, BUILDING_PBR_PAYLOAD
Plus regression guards (no stale v0.4c target; entries are repo-relative and do
not escape src_root).

Out of scope (build artifacts, absent in a clean checkout): FFMPEG_DLLS, the
launcher exe, mc2.exe. Shader runtime inventory is already covered by
shader_contract_harness.

Run:
  py -3 tools/deploy_asset_contract_harness/deploy_asset_contract_harness.py --list
  py -3 tools/deploy_asset_contract_harness/deploy_asset_contract_harness.py --json
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(_HERE))   # tools/<harness>/ -> repo root

# Shared Python harness framework + the real deploy script (source of truth).
sys.path.insert(0, os.path.join(_REPO_ROOT, "tools", "contract_harness_common"))
sys.path.insert(0, os.path.join(_REPO_ROOT, "scripts"))

from contract_harness import Harness, Ctx           # noqa: E402
import deploy_payload as dp                          # noqa: E402

SRC_ROOT = _REPO_ROOT   # the constants are joined with --source-root at deploy time


def _exists(rel):
    return os.path.exists(os.path.join(SRC_ROOT, rel))


def _check_all_exist(t, label, rels):
    missing = [r for r in rels if not _exists(r)]
    for r in missing:
        t.fail(f"{label}: missing payload path: {r}")
    print(f"    {label}: {len(rels)} entries, {len(missing)} missing", file=sys.stderr)
    return not missing


# ---- tests -----------------------------------------------------------------

def test_support_scripts_exist(t: Ctx) -> bool:
    # SUPPORT_SCRIPTS is {profile: [script, ...]} — flatten, dedup.
    rels = sorted({s for group in dp.SUPPORT_SCRIPTS.values() for s in group})
    _check_all_exist(t, "SUPPORT_SCRIPTS", rels)
    return t.failures == 0


def test_editor_support_trees_exist(t: Ctx) -> bool:
    for tree in dp.EDITOR_SUPPORT_TREES:
        if not os.path.isdir(os.path.join(SRC_ROOT, tree)):
            t.fail(f"EDITOR_SUPPORT_TREES: missing dir: {tree}")
    print(f"    EDITOR_SUPPORT_TREES: {len(dp.EDITOR_SUPPORT_TREES)} dirs", file=sys.stderr)
    return t.failures == 0


def test_game_cook_tools_exist(t: Ctx) -> bool:
    _check_all_exist(t, "GAME_COOK_TOOLS", list(dp.GAME_COOK_TOOLS))
    return t.failures == 0


def test_building_pbr_payload_exists(t: Ctx) -> bool:
    _check_all_exist(t, "BUILDING_PBR_PAYLOAD", list(dp.BUILDING_PBR_PAYLOAD))
    return t.failures == 0


def test_target_presets_allowlisted(t: Ctx) -> bool:
    # This used to be `no_stale_v04c_target`, a
    # whole-file grep for the substring "v0.4c" in deploy_payload.py. That check
    # rotted the moment DEPLOY_ALLOWLIST gained the legitimate shared deploy
    # folder "mc2-win64-v0.4c" — the grep cannot tell an allowlist member from a
    # mis-pointed preset, so it failed on correct code.
    #
    # The invariant actually worth guarding (the deploy target is mc2-win64-v0.4)
    # is structural and version-agnostic:
    #   1. the "game" preset must not point at a 0.4c folder (that folder is the
    #      EDITOR install; the historical bug was the game preset aimed there);
    #   2. every preset's target dir must be a DEPLOY_ALLOWLIST member, so a
    #      preset can never bypass DEPLOY-TARGET-GUARD.
    # Neither clause pins a version string, so bumping the release does not
    # require touching this harness.
    for name, preset in dp.TARGET_PRESETS.items():
        target = preset.get("target_dir", "")
        base = os.path.basename(target.rstrip("/\\"))
        if base not in dp.DEPLOY_ALLOWLIST:
            t.fail(f"preset '{name}' target not in DEPLOY_ALLOWLIST: {base}")
        if name == "game" and "0.4c" in base:
            t.fail(f"game preset points at the editor/0.4c install: {base}")
    print(f"    target presets checked: {len(dp.TARGET_PRESETS)} "
          f"(allowlist has {len(dp.DEPLOY_ALLOWLIST)})", file=sys.stderr)
    return t.failures == 0


def test_payload_paths_repo_relative_no_escape(t: Ctx) -> bool:
    # Every source-tracked payload entry must be a normalized repo-relative path
    # that resolves inside src_root (no absolute paths, no .. escape).
    entries = []
    entries += [s for group in dp.SUPPORT_SCRIPTS.values() for s in group]
    entries += list(dp.EDITOR_SUPPORT_TREES)
    entries += list(dp.GAME_COOK_TOOLS)
    entries += list(dp.BUILDING_PBR_PAYLOAD)
    root_real = os.path.realpath(SRC_ROOT)
    for e in entries:
        if os.path.isabs(e):
            t.fail(f"payload entry is absolute (must be repo-relative): {e}")
            continue
        resolved = os.path.realpath(os.path.join(SRC_ROOT, e))
        if resolved != root_real and not resolved.startswith(root_real + os.sep):
            t.fail(f"payload entry escapes src_root: {e}")
    print(f"    checked {len(entries)} payload entries for relative/no-escape",
          file=sys.stderr)
    return t.failures == 0


# Demo failure (in_default=False): proves the existence-check failure path on a
# deliberately-absent payload entry. Runs only via --test, never in the suite.
def test_missing_payload_detected(t: Ctx) -> bool:
    bogus = "data/__intentionally_absent_payload__.bin"
    t.check(_exists(bogus), f"expected (for demo) but absent: {bogus}")  # intentionally fails
    return t.failures == 0


def main():
    h = Harness("deploy_asset_contract_harness")
    h.add("support_scripts_exist",            test_support_scripts_exist)
    h.add("editor_support_trees_exist",       test_editor_support_trees_exist)
    h.add("game_cook_tools_exist",            test_game_cook_tools_exist)
    h.add("building_pbr_payload_exists",      test_building_pbr_payload_exists)
    h.add("target_presets_allowlisted",       test_target_presets_allowlisted)
    h.add("payload_paths_repo_relative_no_escape", test_payload_paths_repo_relative_no_escape)
    h.add("missing_payload_detected",         test_missing_payload_detected, in_default=False)
    return h.run()


if __name__ == "__main__":
    raise SystemExit(main())
