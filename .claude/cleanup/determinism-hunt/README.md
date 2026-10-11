<!-- doc-status: current | reviewed:2026-10-11 -->
# Determinism-hunt instrumentation (temporary patches, never applied to a commit)

WHAT: the throw-away trace code that pinned the mc2_10 divergence (see `lane1-state.md`). `dtrace_tmp.h` goes in `code/`; a `DT(...)` line prints `[DT] f=<render frame> t=<scenario time> ...`
and is armed by `MC2_SIM_IDENTITY_TRACE_DETAIL=1` (the only per-run gate the launcher passes through that every run already sets).
- `dt_all_files.patch`: RNG state/count per frame, REQ/CALC/SETTGT (path and target changes), SADD/SREM (sensor contacts), WX (weather inputs), SETPOS/SCROLL (camera), VO (voice-over), ABL (script call trace
  for render frames 500-640). Taken at an older source state: apply file by file (`git apply --include=<file>`), fix hunks by hand.
- `dt_rng_frame.patch`: just the per-frame `RNGF state= calls=` line (sim_identity.cpp + a counter in gameos.cpp). Start here: the first frame where the shared random stream differs is the lead.
- `dt_mc24_bolt.patch`: per-frame `BOLT` line for every weapon bolt flying at a mech (distance, hit-node point, mech origin) for the mc2_24 bolt-timing race.
HOW: build, deploy, `py -3 tools/sim_determinism.py run <mission> --duration 30 --out <dir>` (set MC2_SIM_IDENTITY_TRACE_DETAIL=1 in the environment), `grep -a "^\[DT\]"` the two logs and diff. Name a return-address
stack with `llvm-symbolizer --obj=<install>/mc2.exe --relative-address 0x<offset>`.
WHY NOT COMMITTED: they print on hot paths and add a gameos.cpp counter; the lesson is the method (diff the RNG stream first), not the code.
