<!-- doc-status: current | reviewed:2026-10-13 -->
# Vulkan validation in the smoke tier

WHAT: a Vulkan smoke run can turn on the Khronos validation layer (`VK_LAYER_KHRONOS_validation`) and have every message of the layer graded. The engine prints
each message as one line, `[VK_VALIDATION v1] severity=<error|warning|info> id=<message id> text="<one line>"` (`GameOS/gameos/vk_validation_line.h`, printed by
`GameOS/gameos/vulkan_context.cpp`). `scripts/smoke_lib/vk_validation.py` parses the lines; the smoke runner grades any `severity=error` line as the red bucket
`vk_validation_error`, counts the warnings (never red), and the report has a `Vulkan validation:` line with the error and warning counts and the distinct
message ids per mission.

WHY: until now the layer's messages went to stderr as free text that nothing graded, so a Vulkan mission could be green with validation errors in its log, and a
requested-but-missing layer silently ran without coverage (best-practices review, "Validation test tier"). A validation tier that cannot fail on a missing layer
looks green with no coverage, so a requested layer that the loader does not offer prints `severity=error id=layer_unavailable` and the mission goes red.
The same holds for a run that asks (`MC2_VULKAN_VALIDATION` in the child's environment) and never prints `layer_enabled`, for example a GL fallback with no refusal or a mission that never
creates the Vulkan context: the runner grades it red with the bucket `vk_validation_unconfirmed` (`scripts/smoke_lib/gates.py`).

## Running it

Validation is for smoke and development runs; a player never has it (the layer is off unless the environment asks, and nothing in the launcher asks).

```
MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1 MC2_VULKAN_VALIDATION=1 \
    py -3 scripts/run_smoke.py --tier tier1 --deploy lane2 --continue --wait-timeout 3600
```

`MC2_VULKAN_VALIDATION` takes the presets of `docs/tier1_env_vars.md` (`1` / `core`, `sync` for synchronization validation, `best-practices`, `gpu-assisted`,
`debug-printf`). The run needs the SDK's layer visible to the Vulkan loader: the installer registers it, or `VK_LAYER_PATH` names the SDK `Bin` directory
(see `docs/host-tools.md`). The SDK's `vkconfig` tunes the layer's own settings (message limit, extra validation groups) for the same run; it does not replace the variable, because the
engine routes the layer's messages into its log through a debug-utils messenger that it creates only when `MC2_VULKAN_VALIDATION` is set. The `info id=layer_enabled` line is the proof that a clean mission was actually validated: a report line without `layer not confirmed on`.

## Reading the numbers

The layer limits repeats of one message id itself (ten by default), so the counts are messages delivered, not draw calls affected; a count of 10 for one id means
"at least ten". The ids of a run are listed next to the counts. Fixing the ids is separate work from grading them: the first run of the tier on the lane install
is recorded in `.claude/cleanup/lane2-state.md`.

## Tests

`tests/smoke/test_vk_validation.py` (the parser, the grading, the report line, the red bucket, a missing layer) and the `vk validation line:` cases in
`tests/unit/test_gate.cpp` (one message is always one line, the id has no spaces, the text no quotes or newlines and is capped).
