"""campaign_liveness: parse [MOVER v2] logs and judge enemy liveness (offline, no game)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import campaign_liveness as cl  # noqa: E402

SAMPLE = (Path(__file__).parent / "data" / "mover_v2_sample.txt").read_text()


def mover(i, team, x, hp=1.0, target=-1, brain=0, mstate=1, psteps=0, name="Harasser"):
    return (f"[MOVER v2] id={i} name={name} team={team} pos={x:.1f},0.0,0.0 hp={hp:.2f} pilot=P order=0/normal "
            f"target={target} brain={brain} mstate={mstate} pgoal=0.0,0.0,0.0 pstep=0/{psteps} pcost=0 tgtpos=0.0,0.0,0.0")


def log(samples, tail=""):
    out = []
    for t, movers in samples:
        out.append(f"[MOVER v2 begin] t={t:.1f} count={len(movers)}")
        out += movers
    return "\n".join(out) + "\n" + tail


def test_real_log_parses():
    segs = cl.parse_log(SAMPLE)
    assert len(segs) == 1 and len(segs[0]["samples"]) == 4
    first = segs[0]["samples"][0]["movers"]
    assert len(first) == 31
    assert first[0]["name"] == "Werewolf" and first[0]["team"] == 0 and first[0]["id"] == 512
    assert abs(first[0]["pos"][0] - 2496.0) < 0.1 and first[0]["hp"] == 1.0


def test_real_log_window_too_short():
    r = cl.analyse(SAMPLE)[0]["summary"]
    assert r["enemies"] == 26 and r["movers"] == 31
    assert r["verdict"] == "window_too_short"      # 6 s of samples is not a liveness window


def test_moving_enemy_passes_and_split_by_mission():
    a = log([(0.0, [mover(1, 0, 0), mover(2, 1, 0)]), (20.0, [mover(1, 0, 0), mover(2, 1, 90.0, target=1)])],
            "[SOAK] autowin mission=mc2_01 t=41.0\n")
    b = log([(0.0, [mover(1, 0, 0), mover(2, 1, 0)]), (30.0, [mover(1, 0, 0), mover(2, 1, 0)])])
    res = cl.analyse(a + b)
    assert [r["summary"]["mission"] for r in res] == ["mc2_01", None]
    assert res[0]["summary"]["verdict"] == "pass"
    assert res[0]["summary"]["enemies_moved"] == 1
    assert res[1]["summary"]["verdict"] == "fail_no_ai"      # stood still for 30 s, never targeted


def test_dead_on_arrival_flagged():
    t = log([(0.0, [mover(1, 0, 0), mover(2, 1, 0, hp=0.0)]), (15.0, [mover(1, 0, 0), mover(2, 1, 50, hp=0.0)])])
    s = cl.analyse(t)[0]["summary"]
    assert s["verdict"] == "fail_hp_near_zero" and s["enemies_hp_near_zero_at_start"] == [2]


def test_ai_on_without_moving():
    t = log([(0.0, [mover(2, 1, 0)]), (15.0, [mover(2, 1, 1, brain=3, psteps=4)])])
    tr = cl.analyse(t)[0]["movers"][0]
    assert tr["ai_on"] and not tr["moved"]


def test_garbage_lines_ignored():
    assert cl.parse_mover_line("[MOVER v2] id=notanumber") is None
    assert cl.parse_log("nothing here\n[SOAK] autowin mission=x\n") == []


def test_far_idle_enemy_is_dormant_not_failure():
    txt = log([(0.0, [mover(1, 0, 0), mover(2, 1, 5000.0)]), (30.0, [mover(1, 0, 0), mover(2, 1, 5000.0)])])
    assert cl.analyse(txt)[0]["summary"]["verdict"] == "dormant_unprovoked"
