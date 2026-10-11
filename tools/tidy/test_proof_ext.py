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
