"""tools/gate_table_gen.py: seed -> table rows, index rendering and the stage-A checks."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools")
sys.path.insert(0, TOOLS)
import gate_table_gen as tg  # noqa: E402


def rec(name, kind="Bool", rule="Std", default=False, **kw):
    r = {"name": name, "kind": kind, "rule": rule, "default": default, "owner": "code", "retireBy": "",
         "doc": "", "readers": [], "numericRange": None, "registered": False, "seedDefaultSource": "reader-idiom"}
    r.update(kw)
    return r


def rows(*recs, ledger=None, smoke=(), history=()):
    return tg.rows_from_seed({"gates": list(recs)}, ledger or {}, set(smoke), set(history))


def by_name(rs):
    return {r["name"]: r for r in rs}


def test_rows_are_sorted_and_include_the_gate_modules_own_rows():
    rs = rows(rec("MC2_ZED"), rec("MC2_ALPHA"))
    names = [r["name"] for r in rs]
    assert names == sorted(names)
    assert "MC2_CONFIG_STRICT" in names and "MC2_CONFIG_DUMP" in names


def test_bool_default_follows_the_rule_when_the_rule_implies_it():
    rs = by_name(rows(rec("MC2_A", rule="Lead0Kill", default=False), rec("MC2_B", rule="Presence", default=True),
                      rec("MC2_C", rule="Std", default=True)))
    assert rs["MC2_A"]["default"] is True      # unset is on under Lead0Kill
    assert rs["MC2_B"]["default"] is False     # unset is off under Presence
    assert rs["MC2_C"]["default"] is True      # Std carries the default


def test_partial_and_custom_rules_map_to_named_rules():
    r = by_name(rows(
        rec("MC2_P1", rule="Partial", default=True, readers=[{"at": "x", "idiom": "Partial[D,on,off,on,on,on,on,on]"}]),
        rec("MC2_P2", rule="Partial", default=False, readers=[{"at": "x", "idiom": "Partial[D,on,off,on,on,on,on,on]"}]),
        rec("MC2_P3", rule="Partial", default=False, readers=[{"at": "x", "idiom": "Partial[D,D,off,on,on,on,on,on]"}]),
        rec("MC2_C1", rule="Custom", default=True, readers=[{"at": "x", "idiom": "Custom[10001011]"}])))
    assert r["MC2_P1"]["rule"] == "Lead0Kill"
    assert r["MC2_P2"]["rule"] == "Lead0P"
    assert r["MC2_P3"]["rule"] == "Lead0Def"
    assert r["MC2_C1"]["rule"] == "DefOnAtoiNZ"


def test_numeric_rows_keep_the_prefix_parse_and_only_reject_style_ranges():
    r = by_name(rows(
        rec("MC2_I", kind="Int", rule="Prefix", default=64,
            numericRange={"lo": 16, "hi": 256, "style": "compare-reject"}),
        rec("MC2_F", kind="Float", rule="Prefix", default=1.5,
            numericRange={"lo": 0.0, "hi": 1.0, "style": "compare"}),
        rec("MC2_K", kind="Int", rule="Prefix", default=3,
            numericRange={"lo": 1, "hi": 9, "style": "clamp"})))
    assert r["MC2_I"]["rule"] == "Prefix" and r["MC2_I"]["range"] == ("range", 16, 256)
    assert r["MC2_F"]["range"] is None
    assert r["MC2_K"]["range"] == ("clampRange", 1, 9)


def test_enum_tokens_start_with_the_default_token():
    r = by_name(rows(rec("MC2_E", kind="Enum", default="", numericRange={"enum": ["a", "b"]}),
                     rec("MC2_E2", kind="Enum", default="a", numericRange={"enum": ["a", "b"]})))
    assert r["MC2_E"]["tokens"] == "|a|b"
    assert r["MC2_E2"]["tokens"] == "a|b"


def test_doc_is_one_ascii_line_within_the_limit():
    long_doc = "First sentence. " + "word " * 80 + "— end"
    r = by_name(rows(rec("MC2_D", doc=long_doc), rec("MC2_D2", doc="Short one. Second sentence.")))
    assert len(r["MC2_D"]["doc"]) <= tg.MAX_DOC and r["MC2_D"]["doc"].isascii()
    assert r["MC2_D2"]["doc"] == "Short one."


def test_flags_come_from_the_ledger_smoke_and_history_sets():
    r = by_name(rows(rec("MC2_ACTIVE_MOD"), rec("MC2_X"), rec("MC2_Y"),
                     ledger={"MC2_X": {"status": "divergent"}}, smoke=["MC2_Y"], history=["MC2_Y"]))
    assert r["MC2_ACTIVE_MOD"]["reread"] and r["MC2_ACTIVE_MOD"]["kind"] == "String"
    assert r["MC2_X"]["divergent"] and not r["MC2_Y"]["divergent"]
    assert r["MC2_Y"]["smokePassthrough"] and r["MC2_Y"]["history"]


def test_row_line_shapes():
    base = dict(name="MC2_A", kind="Bool", rule="Std", default=True, owner="code", retireBy="", doc="d", tokens="",
                range=None, reread=False, divergent=False, smokePassthrough=False, logTopic="", history=False,
                defaultUnknown=False)
    assert tg.row_line(base) == 'G("MC2_A", Bool, true, "code", "", "d"),'
    assert tg.row_line(dict(base, rule="Lead0Kill")).startswith('L(Lead0Kill, "MC2_A", Bool, true,')
    num = dict(base, kind="Int", rule="Prefix", default=64, range=("range", 16, 256), reread=True)
    assert tg.row_line(num) == 'P("MC2_A", Int, 64, "code", "", "d", reread | range(16, 256)),'
    flt = dict(base, kind="Float", rule="Prefix", default=754.0)
    assert ", Float, 754.0," in tg.row_line(flt)
    enum = dict(base, kind="Enum", default="a", tokens="a|b")
    assert tg.row_line(enum).endswith('tokens("a|b")),')


def test_cpp_string_escapes_quotes_backslashes_and_non_ascii():
    assert tg.cpp_str('a"b\\c') == '"a\\"b\\\\c"'
    assert tg.cpp_str("é") == '"\\u00e9"'


def test_index_lists_every_row_with_kind_rule_and_tokens():
    rs = rows(rec("MC2_A", kind="Enum", default="x", numericRange={"enum": ["y"]}), rec("MC2_B"))
    text = tg.render_index(rs)
    assert "kGateCount = %d;" % len(rs) in text
    assert '{"MC2_A", Kind::Enum, Rule::Std, "x|y"},' in text
    assert '{"MC2_B", Kind::Bool, Rule::Std, {}},' in text
    assert text.index('"MC2_A"') < text.index('"MC2_B"')


def _json_doc(rs):
    return {"version": 1, "count": len(rs), "gates": [
        {"name": r["name"], "kind": r["kind"], "rule": r["rule"], "default": r["default"], "owner": r["owner"],
         "retireBy": r["retireBy"], "doc": r["doc"], "tokens": r["tokens"], "range": None,
         "reread": r["reread"], "divergent": r["divergent"], "smokePassthrough": r["smokePassthrough"],
         "logTopic": ""} for r in rs]}


def _tree(tmp_path, stale_index=False):
    rs = rows(rec("MC2_A"), rec("MC2_B"))
    gdir = tmp_path / "Gates"
    gdir.mkdir()
    (gdir / "gate_table.json").write_text(json.dumps(_json_doc(rs)), encoding="utf-8")
    text = tg.render_index(rs)
    if stale_index:
        text = text.replace("MC2_A", "MC2_AA")
    (gdir / "gate_index.gen.h").write_text(text, encoding="utf-8", newline="\n")
    return str(tmp_path)


def test_check_accepts_a_matching_index(tmp_path):
    fails = tg.check(_tree(tmp_path))
    assert [f for f in fails if "stale" in f or "sorted" in f] == []


def test_check_flags_a_stale_index(tmp_path):
    fails = tg.check(_tree(tmp_path, stale_index=True))
    assert any("stale" in f for f in fails)


def test_check_flags_an_unsorted_table(tmp_path):
    root = _tree(tmp_path)
    p = tmp_path / "Gates" / "gate_table.json"
    doc = json.loads(p.read_text(encoding="utf-8"))
    doc["gates"].reverse()
    p.write_text(json.dumps(doc), encoding="utf-8")
    assert any("sorted" in f for f in tg.check(root))


def test_the_committed_table_is_consistent():
    assert tg.check(tg.ROOT) == []
