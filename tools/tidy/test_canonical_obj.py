"""What: tests for canonical_obj.py with hand-built COFF objects - a symbol-table reorder (new relocation indices) keeps the digest, a real difference changes it.
Why: the proof for 1,700 header rewrites rests on this digest; it must forgive layout order and nothing else."""
import os, struct, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import canonical_obj as co
import objhash


def sym(name, secno, cls, value=0):
    n = name.encode()
    assert len(n) <= 8
    return struct.pack("<8sIhHBB", n, value, secno, 0, cls, 0)


def build(code, symbols, reloc_target_index, extra_data=b""):
    """One .text section (code + a relocation to symbols[reloc_target_index]) and the given (name, secno, cls, value) symbols."""
    hdr = 20
    sec_hdr = 40
    data_off = hdr + sec_hdr
    rel_off = data_off + len(code)
    sym_off = rel_off + 10
    reloc = struct.pack("<IIH", 1, reloc_target_index, 4)
    section = struct.pack("<8sIIIIIIHHI", b".text", 0, 0, len(code), data_off, rel_off, 0, 1, 0, 0x60000020)
    syms = b"".join(sym(*s) for s in symbols)
    header = struct.pack("<HHIIIHH", 0x8664, 1, 0, sym_off, len(symbols), 0, 0)
    return header + section + code + reloc + syms + struct.pack("<I", 4) + extra_data


def digest(raw):
    with tempfile.NamedTemporaryFile(suffix=".obj", delete=False) as fh:
        fh.write(raw)
        path = fh.name
    try:
        return co.canonical_digest(path)["code_sha256"], objhash.obj_digest(path)["code_sha256"]
    finally:
        os.unlink(path)


CODE = b"\x48\x8b\x05\x00\x00\x00\x00\xc3"
AB = [(".text", 1, 3), ("alpha", 1, 2), ("beta", 0, 2)]
BA = [(".text", 1, 3), ("beta", 0, 2), ("alpha", 1, 2)]


def test_symbol_table_order_does_not_change_the_canonical_digest():
    a = digest(build(CODE, AB, 2))            # relocation to beta at index 2
    b = digest(build(CODE, BA, 1))            # beta moved to index 1, the relocation follows it
    assert a[0] == b[0]
    assert a[1] != b[1]                       # plain objhash would call these different


def test_a_different_relocation_target_or_code_byte_changes_it():
    base = digest(build(CODE, AB, 2))[0]
    assert digest(build(CODE, AB, 1))[0] != base                      # relocation now targets alpha
    assert digest(build(CODE[:-1] + b"\x90", AB, 2))[0] != base      # a code byte


def test_symbol_offsets_inside_a_section_are_layout_but_names_are_not():
    base = digest(build(CODE, AB, 2))[0]
    moved = [(".text", 1, 3), ("alpha", 1, 2, 4), ("beta", 0, 2)]
    renamed = [(".text", 1, 3), ("alphb", 1, 2), ("beta", 0, 2)]
    assert digest(build(CODE, moved, 2))[0] == base
    assert digest(build(CODE, renamed, 2))[0] != base
