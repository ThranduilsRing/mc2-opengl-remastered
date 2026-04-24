# tests/smoke/fixtures/fake_mc2_pass.py
import sys, time, argparse

ap = argparse.ArgumentParser()
ap.add_argument("--profile"); ap.add_argument("--mission"); ap.add_argument("--duration", type=int, default=1)
a = ap.parse_args()

print("[INSTR v1] enabled: tgl_pool=0 destroy=0 gl_error_print=1 smoke=1 build=fake", flush=True)
print(f"[SMOKE v1] event=banner mode=passive mission={a.mission} profile={a.profile} duration={a.duration} seed=0xc0ffee", flush=True)
print(f"[TIMING v1] event=mission_ready elapsed_ms=500", flush=True)
for i in range(a.duration):
    print(f"[HEARTBEAT] frames={60*(i+1)} elapsed_ms={1000*(i+1)} fps=60.0", flush=True)
    time.sleep(1)
print("[PERF v1] avg_fps=60.0 p50_ms=16.70 p99_ms=19.10 p1low_fps=52.4 peak_ms=24.10 samples=120", flush=True)
print(f"[SMOKE v1] event=summary result=pass duration_actual_ms={1000*a.duration} frames={60*a.duration}", flush=True)
