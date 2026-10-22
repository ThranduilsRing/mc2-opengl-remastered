from vfx_oracle.evidence import extract
from vfx_oracle.check_strict_sim import check
import sim_determinism as sim
import pytest


def test_empty_or_old_engine_log_does_not_invent_rng_proof():
    result = extract("[SPAWN_PROBE] spec=Fireball\n")
    assert result["birth_count"] == 0
    assert result["rng"]["owner"] is None
    assert not result["draw_probe"]["unchanged"]


def test_frozen_draws_must_preserve_stream_and_remain_partial_in_scope():
    birth = "[VFX_BIRTH v1] spec=Fireball time=5 age=0 seed=0.2 stream=56465852 initial=42 state=123 draws=1 legacy_draws=10"
    draw = "[VFX_DRAW_RNG v1] class=CardCloud spec=Fireball time=5 age=0.2 seed=0.2 stream=56465852 state_before=123 state_after=123 draws_before=1 draws_after=1 legacy_before=10 legacy_after=10 unchanged=1"
    result = extract("\n".join([birth, draw, draw]))
    assert result["birth_count"] == 1 and result["rng"]["injected_stream_seen"]
    assert result["draw_probe"]["unchanged"] and result["draw_probe"]["frozen_repeats"] == 2
    assert result["draw_probe"]["scope"] == "CardCloud" and result["rng"]["owner"] is None
    assert not extract("\n".join([birth, draw, draw.replace("unchanged=1", "unchanged=0")]))["draw_probe"]["unchanged"]


def test_legacy_births_and_changing_ages_are_not_owned_or_frozen():
    result = extract("\n".join([
        "[VFX_BIRTH v1] spec=Puff stream=legacy initial=0 state=0 draws=0 legacy_draws=10",
        "[VFX_DRAW_RNG v1] spec=Puff time=5 age=0.1 seed=0.2 stream=legacy unchanged=1",
        "[VFX_DRAW_RNG v1] spec=Puff time=6 age=0.2 seed=0.2 stream=legacy unchanged=1"]))
    assert not result["rng"]["injected_stream_seen"]
    assert result["draw_probe"]["frozen_repeats"] == 0


@pytest.mark.parametrize("field,value", [("state_after", "124"), ("draws_after", "2"),
                                        ("legacy_after", "11"), ("state_before", "missing")])
def test_unchanged_log_flag_cannot_override_snapshot_observations(field, value):
    line = ("[VFX_DRAW_RNG v1] class=CardCloud spec=PPC time=5 age=0.2 seed=0.2 "
            "stream=56465852 state_before=123 state_after=123 draws_before=1 draws_after=1 "
            "legacy_before=10 legacy_after=10 unchanged=1")
    token = next(token for token in line.split() if token.startswith(field + "="))
    assert not extract(line.replace(token, field + "=" + value))["draw_probe"]["unchanged"]


@pytest.mark.parametrize("schema", [1, 2])
def test_strict_bits_cannot_pass_on_matching_graded_chains(schema):
    lines = [f"[SIMID v1] simframe={i} t=0 n=1 graded=aaaa visual=aaaa strict=aaaa "
             "nobj=1 objects=bbbb nabl=1 abl=cccc rng=1/2/3" for i in range(310)]
    digest = sim.chain if schema == 2 else sim.full_chain
    base = {"frames": 300, "chain": digest(lines, 300), "chain_schema": schema}
    assert check(base, [lines] * 4)["accepted"]
    counters = [line.replace("rng=1/2/3", "rng=4/5/6") for line in lines]
    assert check(base, [lines, lines, counters, counters])["accepted"] == (schema == 2)
    drift = [line.replace("strict=aaaa", "strict=bbbb") for line in lines]
    assert sim.visible_chain(lines) == sim.visible_chain(drift)
    assert not check(base, [lines, lines, drift, drift])["accepted"]
    assert not check(base, [drift] * 4)["accepted"]
    assert not check(base, [lines] * 3 + [lines[:299]])["accepted"]
    late = lines.copy()
    late[305] = late[305].replace("strict=aaaa", "strict=bbbb")
    assert not check(base, [lines] * 3 + [late])["accepted"]
    with pytest.raises(ValueError):
        check(base, [lines] * 2)
