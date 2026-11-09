# Box capture identity

Owner symptom: "ab.py refuses comparison because captures lack capture_run.json install_identity".

From a committed lane with this fix in its ancestry, run:

```powershell
py -3 tools/box_run.py --gpu 0 -- ab:claude/nifty-mendeleev
```

Before is the published nifty ref, after is the lane's HEAD. Both arms use the same
lane install sequentially, with verified Linux payload manifests. The tip's capture
tooling runs both arms, even when nifty predates the fix. The row captures
`mc2_01_saved_205644` (day) and `mc2_10_saved_204523` (night), checks ancestry and
data manifest equality, and writes montages under `ab.aN/compare/` in the fetched
box proof directory printed by box_run. Open both montages; MAD is not acceptance.
Use an explicit ancestor SHA instead of the ref to pin an older before revision.

State: implementation and targeted fault tests written. Offline PASS: five modules
(`test_box_capture_identity`, `test_ab_base_guard`, `test_ab_vk_only`,
`test_capture_focus`, `test_box_queue`) via run_scripts_tests.py, 2026-11-09.
The broader existing test_box_hygiene module exceeded two minutes in local bash;
only this run's test process tree was stopped. No hygiene pass claimed.
Next: commit, real nifty/nifty day+night A/B, owning contract rows, final lint,
one Windows compile, precheck, archive receipts and release slot.
