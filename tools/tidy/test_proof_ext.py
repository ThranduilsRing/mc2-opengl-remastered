"""What: tests for proof_ext.py - the opt-in LAYOUT-ONLY acceptance of the rewriters' object proof.
Why: a lane must not widen the proof by accident; the default stays plain identity and only an explicit flag accepts a static-data reorder."""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import objhash
import proof_ext
import test_objhash as to


class Probe(proof_ext.LayoutMixin):
    def __init__(self, before):
        self.before = {"o": before}


def digests(tmp_path, second_data=to.B):
    a, b = to._pair_in(tmp_path, "p", second_data=second_data)
    return objhash.obj_digest(a, per_symbol=True), objhash.obj_digest(b, per_symbol=True)


def test_default_is_plain_identity_and_the_flag_accepts_only_layout_only(tmp_path):
    before, after = digests(tmp_path)
    p = Probe(before)
    assert p.differs("o", after) is True                          # plain proof: a reorder is a change
    p.layout_only_ok = True
    assert p.differs("o", after) is False and p.differs("o", before) is False
    _, changed = to._pair_in(tmp_path, "q", second_data=b"BBBC")
    assert p.differs("o", objhash.obj_digest(changed, per_symbol=True)) is True     # a changed byte is never accepted
    assert p.differs("o", None) is True


def test_order_only_is_a_separate_opt_in_and_diff_never_passes(tmp_path):
    before, after = digests(tmp_path)
    p = Probe(before)
    p.order_only_ok = True
    assert p.differs("o", after) is True                          # LAYOUT-ONLY is not ORDER-ONLY: the other flag stays off
    verdict, _ = objhash.classify(before, after, before["persym"], after["persym"])
    assert verdict == "LAYOUT-ONLY"
    _, changed = to._pair_in(tmp_path, "q", second_data=b"BBBC")
    p.layout_only_ok = True
    assert p.differs("o", objhash.obj_digest(changed, per_symbol=True)) is True


def test_allow_entry_is_bounded_to_its_object_and_headers(tmp_path):
    ab = to._rd(tmp_path, "ab.obj", to.A + to.B, [("alpha", 0, 2, 2), ("beta", 4, 2, 2)], 1)
    ba = to._rd(tmp_path, "ba.obj", to.B + to.A, [("beta", 0, 2, 2), ("alpha", 4, 2, 2)], 0)

    class P(Probe):
        headers = ["h1.h", "h2.h"]
        edited = {"h1.h": b"x", "h2.h": b"y"}
        on_disk = {"h1.h": b"x", "h2.h": b"orig"}                 # only h1 is applied

    entry = {"object": "o", "class": "unreferenced-data-dropped", "headers": ["h1.h"], "reason": "t"}
    p = P(ab)
    assert p.differs("o", ba) is True                             # no ledger entry: the .rdata reorder is a DIFF
    p.allow = [entry]
    assert p.differs("o", ba) is False and p.accepted("o", ba).startswith("ALLOWED")
    p.allow = [dict(entry, headers=["h2.h"])]                     # the applied header is not the entry's trigger
    assert p.differs("o", ba) is True
    p.allow = [dict(entry, object="other")]
    assert p.differs("o", ba) is True
    note = proof_ext.acceptance_note({"o": "ALLOWED x"})
    assert "o  [ALLOWED x]" in note and note.startswith("\n\nObjects accepted")
