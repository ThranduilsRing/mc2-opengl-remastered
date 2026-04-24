# ASan MVP — Follow-up Findings

Running log of issues surfaced by the ASan build that are **not** what we
were originally hunting. Not blocking for heap-bug investigation work —
ASan still reports memory bugs correctly even with these present.

## 1. Rendering pipeline: black terrain + persistent FX trails

**Status:** open, not investigated.
**First observed:** 2026-04-24, first interactive `mc2-asan.exe` run on
`mc2_01` (base game). Build = `build64-asan/RelWithDebInfo` off commit
`a3f84a8 feat(build): add ASan MVP configuration`.

### Symptoms (ASan build only — normal build is fine)

- **Terrain renders black.** Mechs, UI, HUD, and object rendering all work
  normally. Combat functions — user verified by running around and shooting.
- **FX trails persist across frames.** When an effect fires (e.g. weapon
  impact, explosion), its pixels are not cleared on the next frame —
  successive frames composite on top, producing a "solitaire end-screen"
  trail pattern.
- No shader compile/link errors in the log.
- No `[GL_ERROR v1]` events.
- Game runs clean to `[SMOKE v1] result=pass` with `frames=1975` over 45 s.
- **ASan itself produced zero reports** during the run.

### Why this is ASan-only (analysis)

- Tracy is ruled out as a cause. `TRACY_ENABLE` is referenced only inside
  `3rdparty/tracy/` — zero MC2 source files check for it. Its macros are
  no-ops when undefined; disabling Tracy cannot change runtime behavior
  in MC2 code.
- That leaves `/fsanitize=address` codegen as the only variable. ASan
  inserts red zones, reorders stack locals, and changes memory layout.
  The classic symptom this produces: latent UB (uninitialized stack
  variable, OOB read of a stale pattern) that happened to work under the
  normal layout now reads different garbage.

### Hypothesis

Post-process / FBO pipeline UB. Symptom pattern ("persistent pixels" +
"nothing where terrain should be") is framebuffer-lifecycle, not
shader/vertex. Most likely site: uninitialized FBO attachment handle,
stale `glClear` mask/state, or a bad bind in `gos_postprocess.cpp`.
CLAUDE.md / `docs/amd-driver-rules.md` already document the FBO pipeline
as AMD-quirky, which is consistent with the class of bug ASan's layout
change would expose.

### Diagnostic path (for the future investigation)

In-game toggles to narrow the failure site (all from `mc2-asan.exe`):

- `F5` — terrain draw killswitch. If toggling terrain off leaves the
  world visibly black *everywhere* (not just where terrain was), the
  problem is upstream of terrain in the composite chain.
- `RAlt+4` — screen shadows toggle. If turning screen shadows off makes
  terrain visible again, the screen-shadow composite pass is the failure.
- `RAlt+6` / `RAlt+7` / `RAlt+9` — god rays / shorelines / static-prop
  debug-mode cycle. Whichever toggle makes the symptom change is on the
  failure path.
- `F3` — shadows on/off. Narrows whether it's shadow-related.

Code-level instrumentation to add before the next ASan rendering run:

1. `glGetError()` drain after every `glBindFramebuffer` in
   `gos_postprocess.cpp`. Log the first `GL_INVALID_*` that wasn't
   present in the non-ASan build.
2. Log FBO attachment handles at init (they should be stable, non-zero
   GL names). If one reads zero or garbage under ASan, that's the UB
   site.
3. Log the `GLbitfield` passed to each `glClear` call. An uninitialized
   mask that used to happen to be `GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT`
   could now be zero, which explains the "no clear" trail symptom.

### Why we are not fixing it now

The rendering regression does not affect ASan's ability to report heap
bugs — those fire regardless of what pixels end up on screen. The
original MVP goal (catch latent heap bugs on mod content: Carver5O,
Omnitech, Exodus) is unaffected. Come back to this as a dedicated
investigation once we have a heap-bug inventory from the mod-content
runs.

### If ASan runs start producing ambiguous results

Optional fallback experiment: rebuild the ASan build with Tracy
re-enabled (`-DTRACY_ENABLE` added manually alongside `MC2_ASAN=ON`).
Low-prior based on the grep analysis above, but it's a 10-minute
empirical check if other evidence later points at Tracy.
