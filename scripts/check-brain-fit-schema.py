#!/usr/bin/env python3
"""check-brain-fit-schema.py — BRAIN-FIT-SCHEMA-1

Validates mission_ai.fit files (Brain records) against the schema defined in
docs/render-backend-seams/SCOPE-BRAIN-FIT-SCHEMA-1.md.

Checks:
  R1  FAIL  unitRef missing or empty in a Brain block
  R2  FAIL  Duplicate unitRef within the same file
  R3  FAIL  OPORD.type not in the OPORD enum table
  R4  FAIL  Duplicate OPORD slot within the same Brain block
  R5  FAIL  OPORD.slot not in {Primary, Secondary, Tertiary}
  R6  FAIL  tactic.<Name> value is not a non-negative float
  R7  FAIL  SchemaVersion.version present but not integer
  W1  WARN  Unknown switch key not in registry (forward-compat)
  W2  WARN  Author alias used instead of canonical key
  W3  WARN  Unknown tactic name (forward-compat)
  W4  WARN  archetype= present (resolver deferred to BRAIN-ARCHETYPE-FIT-1)
  W5  WARN  mission_ai.fit found with no SchemaVersion block

Exit codes:
  0 — no FAILs (WARNs allowed)
  1 — at least one FAIL rule triggered
  2 — file parse error (malformed FIT syntax)

Missions with no *_ai.fit = PASS (legacy ABL fallback).

Usage:
  py -3 scripts/check-brain-fit-schema.py [--root <repo>] [--fixtures <dir>] [--quiet] [--json <out.json>]
"""

import argparse
import glob
import json
import os
import re
import sys

# ---------------------------------------------------------------------------
# Schema tables
# ---------------------------------------------------------------------------

# Valid OPORD type= values. Compositions (Sentry/Ambush/Scout) are valid but
# resolve to multi-step tac-order sequences at runtime.
OPORD_TYPES = {
    "Patrol", "Guard", "MoveTo", "Escort", "Attack",
    "Withdraw", "Capture", "Refit", "Follow", "HoldFire",
    "Sentry", "Ambush", "Scout",
    # carver_v_enhanced uses PlayerControlled for
    # player-driven units (277 uses). Real OPORD type — was an R3 FAIL before.
    "PlayerControlled",
}
OPORD_COMPOSITION = {"Sentry", "Ambush", "Scout"}

OPORD_SLOTS = {"Primary", "Secondary", "Tertiary"}

# Registered tactic names (initial vocabulary).
TACTIC_NAMES = {
    "IndirectFire", "HullDown", "FightingWithdraw", "Pursue", "HitAndRun",
    "StopAndFire", "Flank", "Joust", "Turret",
    # carver_v_enhanced tactic-weight names —
    # were W3 (unknown-tactic) warnings before.
    "Standard", "Suppress",
}

# Canonical switch key groups.
CANONICAL_SWITCH_GROUPS = {
    "combat", "doctrine", "sensor", "survival",
    "targeting", "movement", "faction", "clan_honor",
}

# Author alias → canonical key mapping (W2).
ALIAS_TO_CANONICAL = {
    "EngageRangeCheck":  "combat.engage_range_check",
    "engage_range":      "combat.engage_range_check",
    "HoldPosition":      "doctrine.hold_position",
    "hold_pos":          "doctrine.hold_position",
    "FireAtWill":        "doctrine.fire_at_will",
    "fire_will":         "doctrine.fire_at_will",
    "PassiveMode":       "sensor.passive_mode",
    "passive":           "sensor.passive_mode",
    "EnhancedScan":      "sensor.enhanced_scan",
    "sensor_boost":      "sensor.enhanced_scan",
    "EjectThreshold":    "survival.eject_threshold",
    "eject":             "survival.eject_threshold",
    "PreferMechs":       "targeting.prefer_mechs",
    "mech_priority":     "targeting.prefer_mechs",
    "JumpPreferred":     "movement.jump_preferred",
    "prefer_jump":       "movement.jump_preferred",
    "Zellbrigen":        "clan_honor.zellbrigen",
    "DezgraResponse":    "clan_honor.dezgra_response",
    "dezgra":            "clan_honor.dezgra_response",
}

# Pilot stat canonical + GDD aliases (W2 on alias; stat.* keys are forward-compat W1 only).
STAT_ALIASES = {
    "Leadership":      "stat.leadership",
    "Discipline":      "stat.discipline",
    "Gunnery":         "stat.gunnery",
    "Piloting":        "stat.piloting",
    "Sensors":         "stat.sensors",
    "Aggressiveness":  "stat.aggressiveness",
    "Courage":         "stat.courage",
}

FALLBACK_POLICIES = {"HoldPosition", "HoldFire", "Withdraw", "LoopPrimary", "RequestOrders"}

# ---------------------------------------------------------------------------
# FIT parser
# ---------------------------------------------------------------------------
# Parses the typed-block FIT format used by FitIniFile (mclib/inifile.cpp).
# Grammar (simplified):
#   file     ::= (block | kv)*
#   block    ::= NAME '{' (block | kv)* '}'
#   kv       ::= KEY '=' VALUE
# Comments: // to end-of-line.
# Tokens are whitespace-separated; strings are optionally double-quoted.

class ParseError(Exception):
    pass

def _tokenize(text):
    """Yield (type, value) tokens from FIT text. Types: NAME, EQ, LBRACE, RBRACE, STR, NUM."""
    i = 0
    n = len(text)
    while i < n:
        # skip whitespace
        if text[i] in ' \t\r\n':
            i += 1
            continue
        # line comment
        if text[i:i+2] == '//':
            while i < n and text[i] != '\n':
                i += 1
            continue
        # braces
        if text[i] == '{':
            yield ('LBRACE', '{')
            i += 1
            continue
        if text[i] == '}':
            yield ('RBRACE', '}')
            i += 1
            continue
        # equals
        if text[i] == '=':
            yield ('EQ', '=')
            i += 1
            continue
        # quoted string
        if text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == '\\':
                    j += 1
                j += 1
            yield ('STR', text[i+1:j])
            i = j + 1
            continue
        # bare token (name, number, or unquoted value)
        j = i
        while j < n and text[j] not in ' \t\r\n{}="\\':
            j += 1
        yield ('TOKEN', text[i:j])
        i = j

def _parse_blocks(tokens, pos):
    """
    Parse a sequence of top-level blocks/kvs from token list.
    Returns list of dicts: {'_type': 'block', 'name': ..., 'children': [...]}
                        or {'_type': 'kv', 'key': ..., 'value': ...}
    """
    result = []
    tlist = tokens
    n = len(tlist)

    while pos < n:
        tok_type, tok_val = tlist[pos]
        if tok_type == 'RBRACE':
            break
        if tok_type in ('TOKEN', 'STR'):
            name = tok_val
            pos += 1
            if pos < n and tlist[pos][0] == 'LBRACE':
                # block
                pos += 1  # consume {
                children, pos = _parse_blocks(tlist, pos)
                if pos >= n or tlist[pos][0] != 'RBRACE':
                    raise ParseError(f"Expected '}}' after block '{name}'")
                pos += 1  # consume }
                result.append({'_type': 'block', 'name': name, 'children': children})
            elif pos < n and tlist[pos][0] == 'EQ':
                # key = value
                pos += 1  # consume =
                if pos < n and tlist[pos][0] in ('TOKEN', 'STR'):
                    value = tlist[pos][1]
                    pos += 1
                else:
                    value = ''
                result.append({'_type': 'kv', 'key': name, 'value': value})
            else:
                # bare name with no = or {  — treat as empty kv
                result.append({'_type': 'kv', 'key': name, 'value': ''})
        else:
            pos += 1  # skip unexpected token

    return result, pos


def parse_fit(text):
    """Parse FIT text into a list of top-level nodes."""
    tokens = list(_tokenize(text))
    nodes, _ = _parse_blocks(tokens, 0)
    return nodes

# ---------------------------------------------------------------------------
# Checker logic
# ---------------------------------------------------------------------------

class Finding:
    def __init__(self, severity, rule, filepath, brain_ref, message):
        self.severity = severity   # 'FAIL' | 'WARN'
        self.rule = rule
        self.filepath = filepath
        self.brain_ref = brain_ref  # e.g. "Brain@Warrior0" or "file"
        self.message = message

    def __str__(self):
        tag = f"Brain@{self.brain_ref}" if self.brain_ref else "file"
        return f"{self.severity:<4}  brain_fit  {os.path.basename(self.filepath)}  {tag}: {self.rule} {self.message}"


def _kv_dict(nodes):
    """Collect key=value pairs from a node list (non-block children only)."""
    return {n['key']: n['value'] for n in nodes if n['_type'] == 'kv'}

def _child_blocks(nodes, name=None):
    """Return block children, optionally filtered by block name."""
    return [n for n in nodes if n['_type'] == 'block' and (name is None or n['name'] == name)]


def _validate_brain(brain_node, filepath, findings):
    """Validate a single Brain {} block. Returns unitRef (possibly empty)."""
    kvs = _kv_dict(brain_node['children'])
    opord_blocks = _child_blocks(brain_node['children'], 'OPORD')

    # R1: unitRef present and non-empty
    unit_ref = kvs.get('unitRef', None)
    if unit_ref is None or unit_ref.strip() == '':
        findings.append(Finding('FAIL', 'R1', filepath, unit_ref or '(missing)',
                                 "unitRef missing or empty"))
        unit_ref = '(missing)'

    tag = unit_ref

    # archetype W4
    if 'archetype' in kvs:
        findings.append(Finding('WARN', 'W4', filepath, tag,
                                 f"archetype='{kvs['archetype']}' present; resolver deferred to BRAIN-ARCHETYPE-FIT-1"))

    # Validate keys
    for key, val in kvs.items():
        if key in ('unitRef', 'archetype', 'fallback_policy'):
            continue

        # tactic.<Name>
        if key.startswith('tactic.'):
            tactic_name = key[len('tactic.'):]
            # R6: value must be non-negative float
            try:
                fval = float(val)
                if fval < 0.0:
                    findings.append(Finding('FAIL', 'R6', filepath, tag,
                                             f"tactic '{key}' = {val} is negative (must be >= 0.0)"))
            except ValueError:
                findings.append(Finding('FAIL', 'R6', filepath, tag,
                                         f"tactic '{key}' value '{val}' is not a float"))
            # W3: unknown tactic name
            if tactic_name not in TACTIC_NAMES:
                findings.append(Finding('WARN', 'W3', filepath, tag,
                                         f"unknown tactic name '{tactic_name}' (forward-compat)"))
            continue

        # threshold.* — numeric, no registry rule, just pass through as unknown switch
        if key.startswith('threshold.'):
            continue

        # stat.* — forward-compat W1
        if key.startswith('stat.'):
            findings.append(Finding('WARN', 'W1', filepath, tag,
                                     f"unknown switch key '{key}' (stat.* keys are forward-compat)"))
            continue

        # GDD stat alias → W2
        if key in STAT_ALIASES:
            findings.append(Finding('WARN', 'W2', filepath, tag,
                                     f"alias '{key}' -> '{STAT_ALIASES[key]}' (use canonical stat.* key)"))
            continue

        # Switch alias → W2
        if key in ALIAS_TO_CANONICAL:
            findings.append(Finding('WARN', 'W2', filepath, tag,
                                     f"alias '{key}' -> '{ALIAS_TO_CANONICAL[key]}'"))
            continue

        # Canonical switch key: must be group.name
        if '.' in key:
            group = key.split('.')[0]
            if group not in CANONICAL_SWITCH_GROUPS:
                findings.append(Finding('WARN', 'W1', filepath, tag,
                                         f"unknown switch key '{key}' (unrecognised group '{group}')"))
        else:
            findings.append(Finding('WARN', 'W1', filepath, tag,
                                     f"unknown switch key '{key}' (no group prefix)"))

    # fallback_policy — forward-compat; unknown values are WARN W1
    fp = kvs.get('fallback_policy', None)
    if fp is not None and fp not in FALLBACK_POLICIES:
        findings.append(Finding('WARN', 'W1', filepath, tag,
                                 f"unknown fallback_policy '{fp}'"))

    # OPORD blocks
    seen_slots = {}
    for opord in opord_blocks:
        okvs = _kv_dict(opord['children'])
        slot = okvs.get('slot', '')
        otype = okvs.get('type', '')

        # R5: slot must be Primary/Secondary/Tertiary
        if slot not in OPORD_SLOTS:
            findings.append(Finding('FAIL', 'R5', filepath, tag,
                                     f"OPORD.slot '{slot}' not in {{Primary,Secondary,Tertiary}}"))

        # R4: duplicate slot within same Brain
        if slot in seen_slots:
            findings.append(Finding('FAIL', 'R4', filepath, tag,
                                     f"duplicate OPORD slot '{slot}'"))
        seen_slots[slot] = True

        # R3: OPORD.type must be in enum
        if otype not in OPORD_TYPES:
            findings.append(Finding('FAIL', 'R3', filepath, tag,
                                     f"unknown OPORD type '{otype}'"))
        elif otype in OPORD_COMPOSITION:
            # Composition: normalize note (WARN-level info, not a problem)
            pass  # valid, resolves to multi-step sequence at runtime

    return unit_ref


# carver mission.fit Brain {} dialect.
# Differs from the _ai.fit schema: OPORD is expressed as PrimaryOPORD/SecondaryOPORD/
# TertiaryOPORD { type = ... } blocks (not OPORD { slot=... }), and tactic weights as a
# Tactics { <Name> = <weight> } block (not tactic.<Name> = keys). Rule codes are M-prefixed
# to distinguish from the _ai.fit rules.

# Non-tactic numeric knobs that legitimately appear inside Tactics {} — not tactic names.
TACTICS_KNOBS = {"AttackerHelpRadius", "DefenderHelpRadius", "EngageRadius"}


def _validate_brain_mission(brain_node, filepath, findings):
    """Validate a carver mission.fit Brain {} block. Returns a display tag."""
    kvs = _kv_dict(brain_node['children'])
    tag = kvs.get('sourceABLBrain', '') or kvs.get('unitRef', '') or '(brain)'

    # MW4: archetype resolver deferred (mirror W4).
    if 'archetype' in kvs:
        findings.append(Finding('WARN', 'MW4', filepath, tag,
                                 f"archetype='{kvs['archetype']}' present; resolver deferred to BRAIN-ARCHETYPE-FIT-1"))

    for blk in _child_blocks(brain_node['children']):
        name = blk['name']
        bkvs = _kv_dict(blk['children'])
        # *OPORD blocks (PrimaryOPORD / SecondaryOPORD / TertiaryOPORD): validate type=.
        if name.endswith('OPORD'):
            otype = bkvs.get('type', '')
            if otype == '':
                continue  # blank type = OPORD slot present but unset — carver does this; allowed
            if otype not in OPORD_TYPES:
                findings.append(Finding('FAIL', 'M3', filepath, tag,
                                         f"{name}.type '{otype}' not in OPORD enum"))
        # Tactics { <Name> = <weight> } block.
        elif name == 'Tactics':
            for k, v in bkvs.items():
                if k in TACTICS_KNOBS:
                    continue  # numeric knob, not a tactic name
                # M6: weight must be a non-negative float.
                try:
                    fv = float(v)
                    if fv < 0.0:
                        findings.append(Finding('FAIL', 'M6', filepath, tag,
                                                 f"tactic '{k}' = {v} is negative (must be >= 0.0)"))
                except ValueError:
                    findings.append(Finding('FAIL', 'M6', filepath, tag,
                                             f"tactic '{k}' value '{v}' is not a float"))
                # MW3: unknown tactic name (forward-compat).
                if k not in TACTIC_NAMES:
                    findings.append(Finding('WARN', 'MW3', filepath, tag,
                                             f"unknown tactic name '{k}' (forward-compat)"))
    return tag


def check_mission_file(filepath, quiet=False):
    """Check a carver mission.fit file's Brain {} blocks. Returns list of Finding."""
    findings = []
    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
    except OSError as e:
        findings.append(Finding('FAIL', 'R0', filepath, 'file', f"cannot open: {e}"))
        return findings
    try:
        nodes = parse_fit(text)
    except ParseError as e:
        findings.append(Finding('FAIL', 'PARSE', filepath, 'file', f"parse error: {e}"))
        return findings

    brain_blocks = _child_blocks(nodes, 'Brain')
    for brain in brain_blocks:
        _validate_brain_mission(brain, filepath, findings)

    n_fails = sum(1 for f in findings if f.severity == 'FAIL')
    if n_fails == 0 and not quiet:
        print(f"PASS  brain_mission  {os.path.basename(filepath)}  {len(brain_blocks)} Brain block(s) validated")
    return findings


def check_file(filepath, quiet=False):
    """Check a single *_ai.fit file. Returns list of Finding."""
    findings = []

    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
    except OSError as e:
        findings.append(Finding('FAIL', 'R0', filepath, 'file', f"cannot open: {e}"))
        return findings

    try:
        nodes = parse_fit(text)
    except ParseError as e:
        findings.append(Finding('FAIL', 'PARSE', filepath, 'file', f"parse error: {e}"))
        return findings

    # SchemaVersion
    sv_blocks = _child_blocks(nodes, 'SchemaVersion')
    has_schema_version = bool(sv_blocks)
    if not has_schema_version:
        findings.append(Finding('WARN', 'W5', filepath, 'file',
                                 "no SchemaVersion block"))
    else:
        sv_kvs = _kv_dict(sv_blocks[0]['children'])
        ver_str = sv_kvs.get('version', None)
        if ver_str is not None:
            try:
                int(ver_str)
            except (ValueError, TypeError):
                findings.append(Finding('FAIL', 'R7', filepath, 'file',
                                         f"SchemaVersion.version '{ver_str}' is not an integer"))

    # Brain blocks
    brain_blocks = _child_blocks(nodes, 'Brain')
    seen_refs = {}
    for brain in brain_blocks:
        unit_ref = _validate_brain(brain, filepath, findings)
        # R2: duplicate unitRef
        if unit_ref and unit_ref != '(missing)':
            if unit_ref in seen_refs:
                findings.append(Finding('FAIL', 'R2', filepath, unit_ref,
                                         f"duplicate unitRef '{unit_ref}'"))
            seen_refs[unit_ref] = True

    # Summary PASS line if no FAILs and not quiet
    n_brains = len(brain_blocks)
    n_fails = sum(1 for f in findings if f.severity == 'FAIL')
    if n_fails == 0 and not quiet:
        print(f"PASS  brain_fit  {os.path.basename(filepath)}  {n_brains} Brain block(s) validated")

    return findings


def find_ai_fit_files(root):
    """Find all *_ai.fit files under data/missions/ in repo root."""
    pattern = os.path.join(root, 'data', 'missions', '**', '*_ai.fit')
    return glob.glob(pattern, recursive=True)


_BRAIN_BLOCK_RE = re.compile(r'\bBrain\s*\{')


def find_mission_fit_files(root):
    """BRAIN-SCHEMA-CARVER-COMPAT-1: mission.fit files that contain a Brain {} block.
    Stock mission.fit (legacy ABL, no Brain {}) is skipped via a cheap substring
    pre-filter — so this pass has zero false positives on non-carver content."""
    pattern = os.path.join(root, 'data', 'missions', '**', 'mission.fit')
    out = []
    for fp in glob.glob(pattern, recursive=True):
        try:
            with open(fp, 'r', encoding='utf-8', errors='replace') as f:
                if _BRAIN_BLOCK_RE.search(f.read()):
                    out.append(fp)
        except OSError:
            pass
    return out


def check_fixtures(fixtures_dir, quiet=False):
    """Check all .fit files in the fixtures directory."""
    if not os.path.isdir(fixtures_dir):
        return []
    pattern = os.path.join(fixtures_dir, '*.fit')
    return glob.glob(pattern)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description='Validate mission_ai.fit Brain records (BRAIN-FIT-SCHEMA-1)')
    parser.add_argument('--root', default='.', metavar='DIR',
                        help='Worktree root (default: current dir)')
    parser.add_argument('--fixtures', default=None, metavar='DIR',
                        help='Fixture directory (default: <root>/scripts/fixtures/brain_fit)')
    parser.add_argument('--quiet', action='store_true',
                        help='Suppress per-key output; print only PASS/FAIL summary')
    parser.add_argument('--json', default=None, metavar='FILE',
                        help='Write structured results to JSON file')
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    fixtures_dir = args.fixtures or os.path.join(root, 'scripts', 'fixtures', 'brain_fit')
    # Separate fixtures for the mission.fit Brain {} dialect.
    mission_fixtures_dir = os.path.join(root, 'scripts', 'fixtures', 'brain_fit_mission')

    # Two independent passes:
    #  1. REAL missions (data/missions/**/*_ai.fit): any FAIL = a real contract
    #     failure. No *_ai.fit anywhere = legacy ABL fallback (PASS).
    #  2. FIXTURE self-test: each fixture must match the verdict implied by its
    #     filename — fail_* must produce >=1 FAIL; everything else (valid_/warn_/
    #     composition_) must produce 0 FAIL. A mismatch means the checker logic
    #     itself regressed. Negative fixtures FAILING is the EXPECTED outcome and
    #     must never turn the CI gate red.
    mission_files = find_ai_fit_files(root)
    fixture_files = check_fixtures(fixtures_dir)
    # mission.fit Brain {} pass + its fixture self-test.
    mission_fit_files = find_mission_fit_files(root)
    mission_fixture_files = check_fixtures(mission_fixtures_dir)

    all_findings = []
    mission_fail = 0
    warn_count = 0

    # Pass 1 — real missions
    for fpath in mission_files:
        findings = check_file(fpath, quiet=args.quiet)
        for f in findings:
            all_findings.append(f)
            if f.severity == 'FAIL':
                mission_fail += 1
            elif f.severity == 'WARN':
                warn_count += 1
            if not args.quiet:
                print(str(f))

    # Pass 2 — fixture self-test (expected-verdict inversion)
    selftest_fail = 0
    for fpath in sorted(fixture_files):
        name = os.path.basename(fpath)
        findings = check_file(fpath, quiet=args.quiet)
        has_fail = any(f.severity == 'FAIL' for f in findings)
        expect_fail = name.startswith('fail_')
        for f in findings:
            all_findings.append(f)
            if f.severity == 'WARN':
                warn_count += 1
            if not args.quiet:
                print(str(f))
        if has_fail != expect_fail:
            selftest_fail += 1
            print(f"FAIL  brain_fit  {name}  self-test mismatch: "
                  f"expected {'FAIL' if expect_fail else 'no FAIL'}, "
                  f"got {'FAIL' if has_fail else 'no FAIL'}")

    # Pass 1b — real carver mission.fit Brain {} blocks (only files with a Brain {} block).
    for fpath in mission_fit_files:
        findings = check_mission_file(fpath, quiet=args.quiet)
        for f in findings:
            all_findings.append(f)
            if f.severity == 'FAIL':
                mission_fail += 1
            elif f.severity == 'WARN':
                warn_count += 1
            if not args.quiet:
                print(str(f))

    # Pass 2b — mission.fit fixture self-test (same fail_* convention).
    for fpath in sorted(mission_fixture_files):
        name = os.path.basename(fpath)
        findings = check_mission_file(fpath, quiet=args.quiet)
        has_fail = any(f.severity == 'FAIL' for f in findings)
        expect_fail = name.startswith('fail_')
        for f in findings:
            all_findings.append(f)
            if f.severity == 'WARN':
                warn_count += 1
            if not args.quiet:
                print(str(f))
        if has_fail != expect_fail:
            selftest_fail += 1
            print(f"FAIL  brain_mission  {name}  self-test mismatch: "
                  f"expected {'FAIL' if expect_fail else 'no FAIL'}, "
                  f"got {'FAIL' if has_fail else 'no FAIL'}")

    total = len(mission_files) + len(fixture_files) + len(mission_fit_files) + len(mission_fixture_files)
    if total == 0:
        if not args.quiet:
            print("PASS  brain_fit  (no *_ai.fit files found — legacy ABL fallback)")
        if args.json:
            with open(args.json, 'w') as jf:
                json.dump({'result': 'PASS', 'files': [], 'findings': []}, jf, indent=2)
        sys.exit(0)

    fail_count = mission_fail + selftest_fail
    if not args.quiet:
        print(f"\nbrain_fit: {len(mission_files)} ai.fit + {len(mission_fit_files)} mission.fit + "
              f"{len(fixture_files) + len(mission_fixture_files)} fixture file(s) - "
              f"{mission_fail} FAIL, {selftest_fail} self-test mismatch, {warn_count} WARN")

    if args.json:
        out = {
            'result': 'FAIL' if fail_count > 0 else 'PASS',
            'mission_files': mission_files,
            'mission_fit_files': mission_fit_files,
            'fixture_files': fixture_files,
            'mission_fixture_files': mission_fixture_files,
            'mission_fail': mission_fail,
            'selftest_fail': selftest_fail,
            'findings': [
                {'severity': f.severity, 'rule': f.rule,
                 'file': f.filepath, 'ref': f.brain_ref, 'message': f.message}
                for f in all_findings
            ],
        }
        with open(args.json, 'w') as jf:
            json.dump(out, jf, indent=2)

    sys.exit(1 if fail_count > 0 else 0)


if __name__ == '__main__':
    main()
