# tests/smoke/fixtures/fake_mc2_hang.py
import sys, time
print("[INSTR v1] enabled: smoke=1 build=fake", flush=True)
print("[SMOKE v1] event=banner mode=passive mission=hang profile=stock duration=1 seed=0x0", flush=True)
# Intentionally hang past any runner cap.
time.sleep(600)
