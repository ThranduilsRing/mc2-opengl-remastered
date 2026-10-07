---
name: greybeard
description: Force a meta-fix vs local-patch ruling before proposing or writing any fix. The greybeard graphics wizard does not patch the symptom - he finds the one upstream change that makes the whole bug class impossible or deletes the legacy mechanism outright. Invoke before any fix proposal; advisors must invoke before returning recommendations.
---

# Greybeard

A graybeard graphics engineer does not reach for the local patch.

## Execution mode

**Default: run inline.** The ruling is reasoning-heavy (no grep phase by default). Only spawn a
subagent if the user explicitly asks. Once a subsystem-pin conclusion is reached ("this is not a
particle bug, it is a texture-binding bug"), confirm it with a targeted grep yourself. The
reasoning comes first; the grep confirms it.

## Stance

Greybeard is the skeptical "what are we not seeing?" pass. It is invoked before fix design hardens.
Its job is not to optimize the patch — it is to challenge whether the patch is aimed at the right
subsystem at all.

Default assumptions:
- the symptom is real
- the obvious patch probably works locally
- the local patch may preserve the mechanism that caused the bug class

Greybeard must ask what upstream deletion, ownership correction, or substitutive path would make
the symptom impossible. It is permitted — expected — to say:

```
You are fixing the wrong layer.
This is not a particle bug; it is a texture-binding / state-inheritance bug.
This is not an editor bug; it is an extraction seam bug.
This is not a water bug; it is a reverse-Z convention leak.
```

Use greybeard when deciding *what problem to solve*. Not when designing the solution.

Before proposing or writing ANY fix, you produce an explicit ruling: is the obvious
fix a patch on a symptom, or is there a meta-fix that dissolves the cause?

This is a forcing function, not advice. Do not skip it because the patch
"obviously works." The patch usually works - that is exactly why the
mechanism it props up never gets deleted. This codebase has a documented
history of additive slices that net ~0ms because each one added a path
without removing the one it was meant to replace.

## The ruling (answer all five, in writing)

1. **Subsystem pin.** Which subsystem actually owns this artifact? If the
   symptom could belong to two systems (static vs dynamic shadow, raster
   sheet vs bake math, observer-effect vs real cost), pin it to one with
   first-hand evidence (overlay, RenderDoc export, telemetry) BEFORE
   theorizing a cause. A wrong pin fakes a confirmed root cause.

2. **Symptom vs cause.** State the proximate symptom and the upstream
   condition that makes it possible. The patch addresses the symptom. What
   addresses the condition?

3. **The meta-fix.** Name the single upstream change that makes this entire
   bug *class* impossible - not this instance. Often it is: delete a legacy
   mechanism that only exists as a workaround for a constraint that no
   longer holds (e.g. the shadow static/dynamic split existed only because
   CPU terrain projection was too heavy to re-render per frame; terrain is
   GPU-side now, so the split itself is the bug, not its symptoms). Modernize
   the shim; do not correct the shim.

4. **Substitutive test.** If this is an offload / fast-path / rework: does
   "done" mean the OLD path is DELETED and its consumers repointed - or are
   you adding a new path alongside the old one? Additive = not done. The
   CPU zone must be gone from the capture, not merely bypassed.

5. **Verdict.** Emit exactly one:
   - `META-FIX` - describe the upstream change, its blast radius, and the
     bug class it retires. Prefer this. A META-FIX verdict does not
     automatically authorize implementation in the current slice; it
     authorizes naming the upstream fix and deciding whether this slice
     should take it or file it as explicit debt.
   - `PATCH (justified)` - the meta-fix exists but is out of scope NOW.
     State the meta-fix anyway, file the debt explicitly (memory file or
     known-issue), and say what blocking/debt reason defers it. A patch
     with no named meta-fix and no debt justification is not allowed.
   - `PATCH (beta-blocker)` - for visible regressions blocking a release.
     Allowed only if: (1) the meta-fix is named, (2) the patch is
     substitutive enough to remove the visible broken path, (3) the
     remaining meta-fix debt is filed explicitly before the patch lands.

## Interactions with existing discipline

- Honors **minimal-touch / modern-when-touched**: the meta-fix is the
  modern standard you bring code to when you must touch it; it is not a
  license for unscoped cleanup. A standalone meta-fix slice still needs a
  blocking or debt justification.
- Pairs with **adversarial-plan-review**: greybeard rules patch-vs-meta
  *before* a plan exists; adversarial review grounds the chosen plan in
  code. Both required for architectural-endpoint / legacy-retirement work.
- Grounding rule still applies: every symbol you cite in the ruling is
  grep-verified at write-time.

## Output shape

Five numbered answers, then the one-line verdict. Terse. If `PATCH
(justified)` or `PATCH (beta-blocker)`, the named meta-fix and the
deferral/debt reason are mandatory fields, not optional prose.
