"""The smoke harness must go red on a static-prop light row past the uploaded light table."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke_lib import logparse as lp  # noqa: E402

BAD = ("[STATIC_LIGHT_ROW v1] event=zero_row_reached_gpu class=out_of_range regIdx=7 typeID=3 leaves=1 "
       "lightIdx=2618 numLights=2 tableCount=2618 rngLightIdx=2618 multiCache=2618")


def _hits(line):
    return [b for b in lp.MARKER_BUCKETS if b.pattern.search(line)]


def test_out_of_range_row_is_red():
    hits = _hits(BAD)
    assert [h.bucket for h in hits] == ["static_light_row_out_of_range"]
    assert hits[0].severity == "red"


def test_census_and_healthy_lines_do_not_fire():
    assert not _hits("[STATIC_LIGHT_ROW v1] event=census flushes=300 out_of_range=0 empty_row=0")
