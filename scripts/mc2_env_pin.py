#!/usr/bin/env python3
"""CAPTURE-ENV-PIN-1 -- pin the MC2_* environment a capture actually runs in.

WHY THIS EXISTS
---------------
`run_visual_capture.py` used to launch the engine with `os.environ.copy()`, so
every stray `MC2_*` in the operator's shell silently rode into the captured
frame, unrecorded.

CORRECTION (ENV-DROP-CLOSE-1). This paragraph used to open by contrasting that
with `run_smoke.py`, which it said "launches the engine with a from-scratch
`env={...}` dict, so an ambient `MC2_*` is DROPPED unless it appears in that
runner's explicit allowlist". THAT IS FALSE AND WAS ALREADY FALSE WHEN WRITTEN:
`scripts/smoke_lib/runner.py` builds the child env from `os.environ.copy()` too
(HARNESS-HYGIENE-3; enforced by `scripts/check-smoke-env-inheritance.py`). The
smoke runner's passthrough tuple drops nothing. The claim is corrected here
rather than deleted because this file was one of the places it was read back
out of, and it kept regenerating a false audit finding -- "gate X is not
allowlisted, therefore both arms of its A/B ran the same path and the result is
void" -- which cost real, valid evidence.

The scrub below is a genuine deny-by-default policy and applies to THIS
launcher only. It is safe because it is LOUD: what it removed is recorded in
`ambient_scrubbed` and warned about, and a deliberate deviation is declared.

That is not hypothetical. On 2026-07-2x this machine's already-open shells
carried three vars that had been removed from the user's registry:

    MC2_STATIC_POP_SPLIT=1
    MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE=1
    MC2_GPU_CULL_STATIC_FROZEN_RECORDS=1

`MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE=1` is default-OFF in the shipped game and
seeds every static prop with a flat 0.3,0.3,0.3 ambient fallback. Measured on the
same pose and the same exe, with only the env differing: 0 never-baked light
slots with the var set, 35 with it unset. It also moved a capture hash. Dozens of
captures were taken through that contamination and none of them recorded it.

THE CONTRACT
------------
1. DEFAULT TO THE SHIPPED CONFIGURATION. The base environment for an engine
   launch has *every* ambient `MC2_*` removed. Deviating requires an explicit
   opt-in, never an inherited one.
2. RECORD WHAT WAS ACTUALLY MEASURED. Every capture sidecar carries the full
   effective `MC2_*` set plus the list of ambient vars that were scrubbed, so a
   future reader can tell what the run really was.

Non-`MC2_*` variables (PATH, TEMP, SystemRoot, ...) pass through untouched --
scrubbing those would break process launch on Windows.
"""

from __future__ import annotations

import os
from typing import Any, Mapping

PREFIX = "MC2_"

#: Ambient vars that are transport/plumbing rather than engine-behaviour gates.
#: These are allowed through the scrub because dropping them breaks the harness
#: itself rather than changing what is rendered. Keep this list SHORT and
#: justify every entry -- it is the one hole in the "shipped by default" rule.
TRANSPORT_PASSTHROUGH = frozenset({
    # Where the engine writes its diagnostic JSONL. Output routing only.
    "MC2_DIAGNOSTIC_TRACE_FILE",
    # Which diagnostic tags are emitted. Read-only instrumentation.
    "MC2_DIAG_TAGS",
    # Debug-state dump for the MCP render-state bridge. Read-only.
    "MC2_DEBUG_STATE_DUMP",
    # Bookmark search path for the capture harness itself.
    "MC2_VISUAL_BOOKMARK_DIR",
    # The declaration channel itself (see DECLARE_VAR).
    "MC2_CAPTURE_ENV_DECLARE",
})

#: HARNESS-CONTROL vars (BACKGROUND-SMOKE-1). These govern WINDOWING, FOCUS and
#: CURSOR -- where the OS puts the window and who owns the pointer -- not what
#: the renderer draws into it. They are harness plumbing and must ALWAYS reach
#: the child, because the alternative is an agent run taking over the user's
#: display while he is using the machine for something else.
#:
#: This is deliberately a SEPARATE class from TRANSPORT_PASSTHROUGH so the
#: sidecar can record them under their own key: a reader must be able to tell
#: "the harness forced this" from "the operator declared a render gate".
#:
#: HONESTY NOTE -- MC2_WINDOWED and MC2_WIDTH/MC2_HEIGHT are not pixel-neutral.
#: They change the DRAWABLE SIZE (fullscreen-desktop pins the drawable to the
#: desktop; windowed pins it to ResolutionX/Y), so they can change a capture's
#: output resolution. That is why they are recorded explicitly rather than
#: folded into `effective` alone -- golden provenance must show them.
HARNESS_CONTROL = frozenset({
    # Never take the display fullscreen for an agent run.
    "MC2_WINDOWED",
    "MC2_WIDTH",
    "MC2_HEIGHT",
    # Never ClipCursor, never warp the pointer, never activate the window
    # (drives SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN engine-side).
    "MC2_NO_CURSOR_GRAB",
    # NOFOCUS-FINISH-1: never occupy visible desktop area at all. The four
    # above make the window small, windowed and unactivated -- it is still ON
    # the user's screen. Only this one keeps it off (gos_render.cpp creates it
    # hidden at (-32000,-32000) and re-asserts that origin after every mode
    # change). It was MISSING here, and because this policy is deny-by-default,
    # scripts/run_visual_capture.py -- which builds its child env through
    # build_pinned_env() rather than nofocus.apply_env() -- delivered an inert
    # var to the engine and fell back to the 250ms external poller. That is
    # precisely the multi-second desktop-covering flash NOFOCUS-HARDEN-1 exists
    # to delete. Pixel-neutral: the GL drawable keeps its full size, so golden
    # capture resolution is unchanged (unlike MC2_WIDTH/MC2_HEIGHT below).
    "MC2_WINDOW_OFFSCREEN",
})

#: Declaration channel for PROGRAMMATIC callers.
#:
#: `--env-declare` covers a human at a shell. But in-repo harnesses
#: (tools/visual_verify/capture_ab.py, ...) drive A/B comparisons by
#: setting gate vars in a child env and invoking run_visual_capture.py as a
#: subprocess. Without a channel, the scrub would silently drop exactly the gate
#: those harnesses are testing -- turning an A/B into two identical runs, which
#: is the same class of silent-inertness bug this whole lane exists to kill.
#:
#: Such a caller sets MC2_CAPTURE_ENV_DECLARE to a comma-separated list of the
#: MC2_* names it deliberately set. Those names keep their ambient values;
#: everything else is still scrubbed.
DECLARE_VAR = "MC2_CAPTURE_ENV_DECLARE"


def declarations_from_channel(
    environ: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """Resolve MC2_CAPTURE_ENV_DECLARE into {name: ambient value}."""
    src = dict(os.environ if environ is None else environ)
    raw = src.get(DECLARE_VAR, "")
    out: dict[str, str] = {}
    for name in (n.strip() for n in raw.split(",")):
        if not name:
            continue
        if not name.startswith(PREFIX):
            raise ValueError(
                f"{DECLARE_VAR} only accepts {PREFIX}* names, got {name!r}")
        if name in src:
            out[name] = src[name]
        # A declared-but-absent name is legitimate: "this gate is deliberately
        # OFF for this arm" (an A/B driver pops the var for its OFF state).
        # Recording the intent matters; there is no value to carry.
    return out


def ambient_mc2(environ: Mapping[str, str] | None = None) -> dict[str, str]:
    """Every MC2_* variable present in the ambient environment."""
    src = os.environ if environ is None else environ
    return {k: v for k, v in sorted(src.items()) if k.startswith(PREFIX)}


def scrubbable_ambient(environ: Mapping[str, str] | None = None) -> dict[str, str]:
    """Ambient MC2_* vars that are NOT transport plumbing -- i.e. the ones that
    can change what the engine renders. These are the contaminants."""
    return {k: v for k, v in ambient_mc2(environ).items()
            if k not in TRANSPORT_PASSTHROUGH and k not in HARNESS_CONTROL}


def build_pinned_env(
    harness_sets: Mapping[str, str],
    declared: Mapping[str, str] | None = None,
    *,
    environ: Mapping[str, str] | None = None,
) -> tuple[dict[str, str], dict[str, Any]]:
    """Build the launch environment for an engine capture, and the record of it.

    harness_sets -- MC2_* the harness itself must set to do its job
                    (MC2_SMOKE_MODE, MC2_VISUAL_CAPTURE_DIR, ...). Always applied.
    declared     -- MC2_* the operator explicitly opted into for THIS run
                    (--env-declare). This is the only sanctioned way to deviate
                    from the shipped configuration.

    Returns (env, record). `env` is a full environment dict safe to hand to
    subprocess.Popen(env=...). `record` is JSON-serialisable and belongs in the
    sidecar verbatim.
    """
    src = dict(os.environ if environ is None else environ)
    # Channel declarations first, so an explicit --env-declare can override them.
    merged_declared = declarations_from_channel(src)
    channel_names = sorted(merged_declared)
    merged_declared.update(dict(declared or {}))
    declared = merged_declared

    scrubbed = scrubbable_ambient(src)
    passthrough = {k: v for k, v in ambient_mc2(src).items()
                   if k in TRANSPORT_PASSTHROUGH}
    # BACKGROUND-SMOKE-1: windowing/focus/cursor always survives the scrub.
    # Ambient value first; a harness_set below still wins, which is what makes
    # the no-focus default apply even to a totally clean shell.
    harness_control = {k: v for k, v in ambient_mc2(src).items()
                       if k in HARNESS_CONTROL}

    # Base: the real environment with EVERY behaviour-affecting MC2_* removed.
    env = {k: v for k, v in src.items() if not k.startswith(PREFIX)}
    env.update(passthrough)
    env.update(harness_control)

    # A declared var overrides the scrub -- that is the whole point of declaring.
    # Track which declarations rescued an ambient value vs introduced a new one,
    # because "I declared it" and "it happened to already be set" read the same
    # in the final env but are very different provenance.
    redeclared = {k: v for k, v in declared.items() if k in scrubbed}
    env.update(harness_sets)
    env.update(declared)

    still_scrubbed = {k: v for k, v in scrubbed.items() if k not in declared}

    record = {
        "policy": "pinned-shipped",
        "policy_doc": "scripts/mc2_env_pin.py",
        # The full MC2_* set the engine actually saw. This is the answer to
        # "what was measured?" and it is what the ratchet reads.
        "effective": {k: v for k, v in sorted(env.items())
                      if k.startswith(PREFIX)},
        "harness_set": dict(sorted(harness_sets.items())),
        "declared": dict(sorted(declared.items())),
        "declared_via_channel": channel_names,
        "transport_passthrough": dict(sorted(passthrough.items())),
        # BACKGROUND-SMOKE-1: windowing/focus/cursor actually delivered to the
        # child, from EITHER the ambient shell or harness_sets. Recorded on its
        # own key because MC2_WINDOWED / MC2_WIDTH / MC2_HEIGHT move the
        # drawable size and therefore can move a golden's resolution -- a
        # reader must see them without digging through `effective`.
        "harness_control": {k: v for k, v in sorted(env.items())
                            if k in HARNESS_CONTROL},
        # Contaminants that were present in the shell and REMOVED. Non-empty
        # here means the operator's shell was dirty; the run is still correct
        # (they were scrubbed) but the ratchet flags it so the shell gets fixed.
        "ambient_scrubbed": dict(sorted(still_scrubbed.items())),
        "ambient_redeclared": dict(sorted(redeclared.items())),
        "clean_shell": not still_scrubbed,
    }
    return env, record


def parse_declarations(pairs: list[str] | None) -> dict[str, str]:
    """Parse repeatable --env-declare KEY=VALUE arguments."""
    out: dict[str, str] = {}
    for raw in pairs or []:
        if "=" not in raw:
            raise ValueError(
                f"--env-declare expects KEY=VALUE, got {raw!r}")
        k, v = raw.split("=", 1)
        k = k.strip()
        if not k.startswith(PREFIX):
            raise ValueError(
                f"--env-declare only accepts {PREFIX}* variables, got {k!r}")
        out[k] = v
    return out


def format_warning(record: Mapping[str, Any]) -> str | None:
    """Operator-facing warning when the shell was dirty. None if clean."""
    scrubbed = record.get("ambient_scrubbed") or {}
    if not scrubbed:
        return None
    items = ", ".join(f"{k}={v}" for k, v in scrubbed.items())
    return (
        f"[env-pin] WARNING: {len(scrubbed)} ambient MC2_* variable(s) were "
        f"present in this shell and have been SCRUBBED so the capture runs the "
        f"shipped configuration: {items}. "
        f"The capture is valid; your shell is not. Fix the shell (open a new "
        f"one) or pass --env-declare KEY=VALUE if the deviation is deliberate."
    )
