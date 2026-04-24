# Tier2 baselines — 2026-04-24

Run command: `python scripts/run_smoke.py --tier tier2 --duration 60 --baseline-update --keep-logs --kill-existing`
Wallclock: ~28 min
Result: 23/24 passing

## Per-mission results

| Mission | Result | avg_fps | p1low | peak_ms | Bucket (if fail) |
|---------|--------|---------|-------|---------|------------------|
| mc2_01  | PASS   | 175.2   | 141.6 | 150.0   |                  |
| mc2_02  | PASS   | 120.2   | 70.9  | 161.0   |                  |
| mc2_03  | PASS   | 134.8   | 87.5  | 239.1   |                  |
| mc2_04  | PASS   | 186.0   | 82.9  | 167.8   |                  |
| mc2_05  | PASS   | 108.5   | 85.7  | 197.0   |                  |
| mc2_06  | PASS   | 124.4   | 57.9  | 231.6   |                  |
| mc2_07  | PASS   | 138.7   | 70.6  | 140.4   |                  |
| mc2_08  | PASS   | 141.5   | 70.2  | 18.0    |                  |
| mc2_09  | PASS   | 220.1   | 140.9 | 10.7    |                  |
| mc2_10  | PASS   | 143.2   | 68.5  | 45.0    |                  |
| mc2_11  | PASS   | 154.7   | 73.0  | 140.9   |                  |
| mc2_12  | PASS   | 85.8    | 45.2  | 231.6   |                  |
| mc2_13  | PASS   | 146.0   | 90.9  | 74.0    |                  |
| mc2_14  | PASS   | 116.3   | 50.3  | 215.4   |                  |
| mc2_15  | PASS   | 163.9   | 88.0  | 123.4   |                  |
| mc2_16  | PASS   | 105.9   | 42.5  | 72.4    |                  |
| mc2_17  | PASS   | 153.2   | 68.6  | 102.7   |                  |
| mc2_18  | PASS   | 116.4   | 52.3  | 151.2   |                  |
| mc2_19  | PASS   | 109.4   | 53.1  | 226.4   |                  |
| mc2_20  | FAIL   | -       | -     | -       | timeout          |
| mc2_21  | PASS   | 82.8    | 26.8  | 270.8   |                  |
| mc2_22  | PASS   | 130.4   | 58.9  | 178.3   |                  |
| mc2_23  | PASS   | 110.2   | 47.4  | 284.4   |                  |
| mc2_24  | PASS   | 93.3    | 58.1  | 157.5   |                  |

## Failures

### mc2_20 — timeout
- First-error excerpt: `STOPSYNTAX ERROR data/missions/warriors/mc2_20_mogm_mog.abl [line 207] - (type 16) Incompatible types "corewait"`
- Root cause: ABL `corewait` function is unimplemented in our engine. The runtime emits an error on every frame, producing ~1.17M lines of log spam. The frame loop runs but renders 0 frames (mission_ready fires at 3.4s, engine_init_done at 4.6s, then 0 frames before walltime cap at 120s).
- Decision: LOG-ONLY — real engine bug (missing ABL stub for `corewait`). Mission does load; gameplay is just blocked by the ABL error loop.
- Rationale: This is an unimplemented ABL extension, similar to the class of missing ABL stubs tracked in the MCO/Omnitech integration work. Not a skip candidate; the mission content is valid. Fix requires adding a `corewait` stub or implementing the feature.

## Perf outliers

- `mc2_09`: avg_fps=220.1 p1low=140.9 peak_ms=10.7 — fastest mission, very low peak frame time; simple open terrain with few objects
- `mc2_21`: avg_fps=82.8 p1low=26.8 peak_ms=270.8 — lowest p1% in the set; heavy object density causing occasional stutter spikes
- `mc2_23`: avg_fps=110.2 p1low=47.4 peak_ms=284.4 — highest peak_ms in the set; late-campaign mission with large object count

## Manifest changes made

None. mc2_20 timeout is a real engine bug (ABL `corewait` unimplemented), not a skip-worthy content gap.

## Suggested followups (not done in this session)

- **File engine issue: `corewait` ABL stub missing for mc2_20.** The ABL file `mc2_20_mogm_mog.abl` line 207 and multiple escort variants use `corewait`, which is type 16 (Incompatible types) at compile time. Registering a no-op `corewait` stub (analogous to other ABL stubs registered for MCO) would likely unblock this mission. Reference: `omnitech_abl_stubs_session.md` for the stub registration pattern.
- `mc2_21` p1low=26.8 is notably lower than the rest of the set — may be worth investigating object-density spikes; could be a TGL pool pressure point on load entry.
- Consider adding `mc2_20` back after the `corewait` stub is implemented; it's the only tier2 mission currently skipped from baselines.
