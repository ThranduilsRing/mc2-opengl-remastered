# tests/smoke/test_order_liveness_floor.py
# META-FIX design F: ORDER_LIVENESS_SUMMARY parser + runner floor — proves the
# mechanism can fail a run (red-capability leg, INFRA-3 test_marker_buckets
# precedent) and that absence-is-failure holds when the fixture is armed.

from scripts.run_smoke import (_check_order_liveness_floor,
                               _parse_order_liveness_summary)

PASS_LINE = ("ORDER_LIVENESS_SUMMARY issued_move=4 moved=3 refused=1 "
             "stalled=0 verdict=PASS")
FAIL_LINE = ("ORDER_LIVENESS_SUMMARY issued_move=4 moved=2 refused=0 "
             "stalled=2 verdict=FAIL")


def test_parse_pass_line():
    ol = _parse_order_liveness_summary("noise\n" + PASS_LINE + "\nmore noise\n")
    assert ol == {"issued_move": 4, "moved": 3, "refused": 1,
                  "stalled": 0, "verdict": "PASS"}


def test_parse_takes_last_line():
    # Re-armed missions may emit twice in one capture; the LAST summary wins
    # (reversed-scan, VEG parser precedent).
    ol = _parse_order_liveness_summary(FAIL_LINE + "\n" + PASS_LINE + "\n")
    assert ol is not None and ol["verdict"] == "PASS"


def test_parse_absent_returns_none():
    assert _parse_order_liveness_summary("no summary here\n") is None


def test_floor_pass_verdict_no_failures():
    ol = _parse_order_liveness_summary(PASS_LINE)
    assert _check_order_liveness_floor("mc2_24", ol) == []


def test_floor_fail_verdict_is_red():
    ol = _parse_order_liveness_summary(FAIL_LINE)
    fails = _check_order_liveness_floor("mc2_24", ol)
    assert len(fails) == 1 and "verdict=FAIL" in fails[0]


def test_floor_absence_is_failure():
    # The fixture dying silently must be red, not a silent pass.
    fails = _check_order_liveness_floor("mc2_16", None)
    assert len(fails) == 1 and "absent" in fails[0]
