# tests/smoke/fixtures/fake_mc2_fault.py
#
# SMOKE-SYNTHETIC-FIXTURES-ALL-BUCKETS — one parametrized fault fixture that
# emits the exact stdout sentinels + exit code for each smoke failure bucket,
# so the gate evaluator can be exercised end-to-end (spawn -> parse -> gate)
# rather than only on hand-built LogSummary dataclasses.
#
# Each --fault value is engineered to ISOLATE a single bucket: it emits an
# otherwise-clean run (so no incidental bucket fires) plus exactly the one
# sentinel/exit condition under test. Sentinel strings are copied verbatim
# from scripts/smoke_lib/logparse.py — keep them in lockstep.
#
# The companion timeout bucket is covered by fake_mc2_hang.py; the clean-pass
# baseline by fake_mc2_pass.py. This fixture covers the remaining 10 buckets.
import argparse
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--fault", required=True)
# Accept (and ignore) the smoke flags the runner appends via _build_argv.
ap.add_argument("--profile")
ap.add_argument("--mission")
ap.add_argument("--duration", type=int, default=1)
a = ap.parse_args()


def emit(line):
    print(line, flush=True)


def instr():
    emit("[INSTR v1] enabled: tgl_pool=1 destroy=0 gl_error_print=1 smoke=1 build=fake")


def banner():
    emit(f"[SMOKE v1] event=banner mode=passive mission={a.mission} "
         f"profile={a.profile} duration={a.duration} seed=0xfa017")


def play_ok():
    """A normal load->play->summary=pass tail (used to isolate additive buckets)."""
    emit("[TIMING v1] event=mission_ready elapsed_ms=500")
    emit("[HEARTBEAT] frames=60 elapsed_ms=1000 fps=60.0")
    emit("[PERF v1] avg_fps=60.0 p50_ms=16.70 p99_ms=19.10 p1low_fps=52.4 peak_ms=24.10 samples=120")
    emit("[SMOKE v1] event=summary result=pass duration_actual_ms=1000 frames=60")


f = a.fault

# ---- instrumentation_missing: omit the [INSTR v1] banner entirely ----------
if f == "instrumentation_missing":
    banner()
    play_ok()
    sys.exit(0)

# Every other fault carries a valid instrumentation banner.
instr()
banner()

# ---- crash_silent: nonzero exit, no summary, no crash marker ----------------
# No mission_ready/heartbeat: keep it a pure "vanished mid-load" so the only
# bucket that can fire is crash_silent (summary None + nonzero exit).
if f == "crash_silent":
    sys.exit(134)  # abnormal exit, but engine never printed a crash banner

# ---- crash_no_summary: crash marker present, no summary, nonzero exit -------
if f == "crash_no_summary":
    emit("CRASH: access violation at 0xdeadbeef")
    sys.exit(1)

# ---- engine_reported_fail: clean run but summary result=fail ----------------
if f == "engine_reported_fail":
    emit("[TIMING v1] event=mission_ready elapsed_ms=500")
    emit("[HEARTBEAT] frames=60 elapsed_ms=1000 fps=60.0")
    emit("[SMOKE v1] event=summary result=fail reason=objective_lost stage=play")
    sys.exit(0)

# ---- gl_error: pass run + one GL error line (additive bucket) ---------------
if f == "gl_error":
    emit("[GL_ERROR v1] code=0x0500 GL_INVALID_ENUM op=glDrawElements")
    play_ok()
    sys.exit(0)

# ---- pool_null: pass run + a TGL pool null report (additive bucket) ---------
if f == "pool_null":
    emit("[TGL_POOL v1] event=summary nulls=3 total=512")
    play_ok()
    sys.exit(0)

# ---- asset_oob: pass run + an out-of-bounds blit (additive bucket) ----------
if f == "asset_oob":
    emit("[ASSET_SCALE v1] event=oob_blit src=mechicon dst=512 clamped=1")
    play_ok()
    sys.exit(0)

# ---- shader_error: pass run + a shader load/compile failure -----------------
if f == "shader_error":
    emit("Shader filename: gos_terrain.frag failed to load shader (compile error)")
    play_ok()
    sys.exit(0)

# ---- shader_error_gl_prop: the real GL compile-failure log shape ------------
if f == "shader_error_gl_prop":
    emit("[SHADER ERROR] CompileShader: ERROR: 0:12: 'foo' : undeclared identifier")
    emit("[GPUPROPS] failed to compile/link static_prop shader pair — GPU path disabled")
    play_ok()
    sys.exit(0)

# ---- shader_error_vk: VK pipeline creation failure --------------------------
if f == "shader_error_vk":
    emit("[VK_COLOR_PROP] TIER-1 vkCreateGraphicsPipelines failed.")
    play_ok()
    sys.exit(0)

# ---- missing_file: pass run + a missing-required-file line ------------------
if f == "missing_file":
    emit("Missing file: data/tgl/critical.fst")
    play_ok()
    sys.exit(0)

# ---- heartbeat_freeze_load: load heartbeat, never reaches mission_ready -----
# Gate is wallclock-based: with heartbeat_timeout_load_s=0 in the test, any
# positive gap between the last load heartbeat and end-of-run trips the freeze.
if f == "heartbeat_freeze_load":
    emit("[HEARTBEAT] frames=10 elapsed_ms=200 fps=50.0")
    import time
    time.sleep(0.2)  # ensure walltime advances past the last heartbeat stamp
    sys.exit(0)

# ---- heartbeat_freeze_play: mission_ready reached, no play heartbeat at all -
if f == "heartbeat_freeze_play":
    emit("[HEARTBEAT] frames=10 elapsed_ms=200 fps=50.0")  # load-phase only
    emit("[TIMING v1] event=mission_ready elapsed_ms=500")
    sys.exit(0)

sys.stderr.write(f"unknown --fault {f!r}\n")
sys.exit(2)
