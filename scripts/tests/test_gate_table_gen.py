"""tools/gate_table_gen.py and tools/gate_registry_gen.py: index rendering, the derived registry rows and yaml,
and the stage-A checks; scripts/gate_table_reader.py: the JSON reader the Python tools share."""
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import gate_registry_gen as grg  # noqa: E402
import gate_table_gen as tg  # noqa: E402
import gate_table_reader as gtr  # noqa: E402


def gate(name, kind="Bool", rule="Std", default=False, registry=None, **kw):
    g = {"name": name, "kind": kind, "rule": rule, "default": default, "owner": "code", "retireBy": "",
         "doc": "doc of " + name, "tokens": "", "range": None, "reread": False, "divergent": False,
         "smokePassthrough": False, "verified": False, "registry": registry, "logTopic": ""}
    g.update(kw)
    return g


def doc_of(*gates):
    return {"version": 1, "count": len(gates), "gates": sorted(gates, key=lambda g: g["name"])}


def test_cpp_string_escapes_quotes_backslashes_and_non_ascii():
    assert tg.cpp_str('a"b\\c') == '"a\\"b\\\\c"'
    assert tg.cpp_str("é") == '"\\u00e9"'


def test_index_lists_every_row_with_kind_rule_and_tokens():
    rs = tg.rows_from_json(doc_of(gate("MC2_A", kind="Enum", tokens="x|y"), gate("MC2_B")))
    text = tg.render_index(rs)
    assert "kGateCount = 2;" in text
    assert '{"MC2_A", Kind::Enum, Rule::Std, "x|y"},' in text
    assert '{"MC2_B", Kind::Bool, Rule::Std, {}},' in text
    assert text.index('"MC2_A"') < text.index('"MC2_B"')


def _tree(tmp_path, stale_index=False):
    doc = doc_of(gate("MC2_A"), gate("MC2_B"))
    gdir = tmp_path / "Gates"
    gdir.mkdir()
    (gdir / "gate_table.json").write_text(json.dumps(doc), encoding="utf-8")
    text = tg.render_index(tg.rows_from_json(doc))
    if stale_index:
        text = text.replace("MC2_A", "MC2_AA")
    (gdir / "gate_index.gen.h").write_text(text, encoding="utf-8", newline="\n")
    return str(tmp_path)


def test_check_accepts_a_matching_index(tmp_path):
    fails = tg.check(_tree(tmp_path))
    assert [f for f in fails if "stale" in f and "index" in f or "sorted" in f] == []


def test_check_flags_a_stale_index(tmp_path):
    assert any("gate_index.gen.h is stale" in f for f in tg.check(_tree(tmp_path, stale_index=True)))


def test_check_flags_an_unsorted_table(tmp_path):
    root = _tree(tmp_path)
    p = tmp_path / "Gates" / "gate_table.json"
    d = json.loads(p.read_text(encoding="utf-8"))
    d["gates"].reverse()
    p.write_text(json.dumps(d), encoding="utf-8")
    assert any("sorted" in f for f in tg.check(root))


def test_the_committed_table_is_consistent():
    assert tg.check(tg.ROOT) == []


def test_exclusive_lower_bound_survives_the_json_round_trip():
    g = gate("MC2_X_RADIUS", kind="Float", rule="Prefix", default=0.6,
             range={"lo": 0, "hi": 1, "clamp": False, "loExclusive": True})
    (row,) = tg.rows_from_json(doc_of(g))
    assert row["range"] == ("rangeAbove", 0, 1)


def test_top_level_args_ignores_commas_in_strings_and_calls():
    assert tg._top_level_args('G("MC2_X", Bool, false, "o", "", "a, b (c) d"') == 6
    assert tg._top_level_args('L(Presence, "MC2_X", Bool, false, "o", "", "d", passthrough | range(0.0, 1.0)') == 8


def test_verified_must_match_the_typed_readers(tmp_path):
    (tmp_path / "code").mkdir()
    (tmp_path / "code" / "a.cpp").write_text(
        'int f() { return mc2::gate::get<"MC2_READ_ME">(); }\n// get<"MC2_IN_A_COMMENT">\n', encoding="utf-8")
    (tmp_path / "Gates").mkdir()
    (tmp_path / "Gates" / "g.cpp").write_text('get<"MC2_INSIDE_GATES">', encoding="utf-8")
    assert tg.typed_readers(str(tmp_path)) == {"MC2_READ_ME"}


def test_override_counter_matches_the_using_enum_spelling(tmp_path):
    (tmp_path / "code").mkdir()
    (tmp_path / "code" / "a.cpp").write_text(
        'a = gate::get<"MC2_A", Rule::Presence>(); b = gate::get<"MC2_B", Presence>(); c = gate::get<"MC2_C">();\n',
        encoding="utf-8")
    assert tg.count_overrides(str(tmp_path)) == 2


# ---------------------------------------------------------------------------
# Registry rows and yaml, generated from the table
# ---------------------------------------------------------------------------

HEADER = ("// header\nenum class RendererFeature : int {\n    A = 0,   // MC2_A_GATE\n    B = 1,   // MC2_B_GATE\n    COUNT = 2,\n};\n"
          "static constexpr EnvVarDesc kFeatureTable[] = {\n    OLD ROWS\n};\n\n"
          "static constexpr EnvVarDesc kAuxEnvVars[] = {\n    OLD AUX\n};\n\ninline void dump() {}\n")


def reg(fid, kind="Feature", order=-1, on=False):
    return {"featureId": fid, "kind": kind, "order": order, "defaultOn": on}


def _registry_doc():
    return doc_of(
        gate("MC2_B_GATE", registry=reg("MC2_FEATURE_B", order=1, on=True), doc="second"),
        gate("MC2_A_GATE", registry=reg("MC2_FEATURE_A", order=0), doc='first "quoted"'),
        gate("MC2_Z_TRACE", registry=reg("MC2_TRACE_Z", kind="Trace")),
        gate("MC2_C_TRACE", registry=reg("MC2_TRACE_C", kind="Trace", on=True)),
        gate("MC2_UNREGISTERED"))


def test_header_rows_are_generated_in_enum_order_and_aux_rows_by_name():
    text = grg.patch_header(HEADER, grg.entries(_registry_doc(), []))
    assert text.index('"MC2_FEATURE_A"') < text.index('"MC2_FEATURE_B"')
    assert text.index('"MC2_TRACE_C"') < text.index('"MC2_TRACE_Z"')
    assert "MC2_UNREGISTERED" not in text
    assert 'first \\"quoted\\"' in text
    assert "OLD ROWS" not in text and "OLD AUX" not in text
    assert text.startswith("// header\nenum class RendererFeature") and text.endswith("inline void dump() {}\n")


def test_header_generation_is_idempotent_and_keeps_crlf():
    ents = grg.entries(_registry_doc(), [])
    once = grg.patch_header(HEADER, ents)
    assert grg.patch_header(once, ents) == once
    crlf = grg.patch_header(HEADER.replace("\n", "\r\n"), ents)
    assert "\r\n" in crlf and "\n" not in crlf.replace("\r\n", "")


def test_a_gap_in_the_feature_order_is_refused():
    d = doc_of(gate("MC2_A", registry=reg("MC2_FEATURE_A", order=0)), gate("MC2_C", registry=reg("MC2_FEATURE_C", order=2)))
    with pytest.raises(SystemExit):
        grg.feature_rows(grg.entries(d, []))


def test_rows_without_an_env_var_come_from_the_feature_only_list():
    only = [{"featureId": "MC2_FEATURE_REVERSE_Z", "kind": "Feature", "order": 0, "defaultOn": True, "doc": "always on"}]
    d = doc_of(gate("MC2_B", registry=reg("MC2_FEATURE_B", order=1)))
    text = grg.patch_header(HEADER.replace("// MC2_A_GATE", "// (no env var)").replace("// MC2_B_GATE", "// MC2_B"), grg.entries(d, only))
    assert text.index('"MC2_FEATURE_REVERSE_Z"') < text.index('"MC2_FEATURE_B"')
    assert "nullptr" in text


def test_yaml_is_a_pure_function_of_the_table():
    d = _registry_doc()
    y = grg.render_yaml(d, [])
    assert y == grg.render_yaml(d, [])
    assert y.count("  - name: ") == 5
    assert "registry: {feature_id: MC2_FEATURE_B, kind: Feature, order: 1, default_on: true}" in y
    assert 'doc: "first \\"quoted\\""' in y


def test_the_reader_orders_registry_entries_like_the_generated_header():
    d = _registry_doc()
    entries = gtr.registry_entries_from_text(json.dumps(d), "[]")
    assert list(entries) == ["MC2_A_GATE", "MC2_B_GATE", "MC2_C_TRACE", "MC2_Z_TRACE"]
    assert entries["MC2_B_GATE"]["table"] == "kFeatureTable"
    assert entries["MC2_Z_TRACE"]["table"] == "kAuxEnvVars"


def test_the_reader_refuses_a_torn_or_inconsistent_table():
    good = json.dumps(_registry_doc())
    for bad in (good[:40], good.replace('"count": 5', '"count": 6')):
        with pytest.raises(gtr.GateTableError):
            gtr.registry_entries_from_text(bad, "[]")


def test_the_committed_header_and_yaml_are_current():
    d = gtr.load_table(tg.ROOT)
    assert grg.check_all(tg.ROOT, d) == []


def test_the_reader_refuses_a_missing_feature_only_list_a_gap_in_the_order_and_unsorted_names():
    good = json.dumps(_registry_doc())
    with pytest.raises(gtr.GateTableError):
        gtr.registry_entries_from_text(good)                     # reverse-Z would be silently dropped
    assert gtr.registry_entries_from_text(good, None, without_only=True)
    gap = doc_of(gate("MC2_A", registry=reg("MC2_FEATURE_A", order=0)), gate("MC2_C", registry=reg("MC2_FEATURE_C", order=2)))
    with pytest.raises(gtr.GateTableError):
        gtr.registry_entries_from_text(json.dumps(gap), "[]")
    unsorted = _registry_doc()
    unsorted["gates"].reverse()
    with pytest.raises(gtr.GateTableError):
        gtr.registry_entries_from_text(json.dumps(unsorted), "[]")


def test_enum_names_are_emitted_and_checked_against_the_table():
    ents = grg.entries(_registry_doc(), [])
    text = grg.patch_header(HEADER, ents)
    assert "// [0] A" in text and "// [1] B" in text
    with pytest.raises(SystemExit):
        grg.patch_header(HEADER.replace("// MC2_B_GATE", "// MC2_SOMETHING_ELSE"), ents)
    with pytest.raises(SystemExit):
        grg.patch_header(HEADER.replace("    B = 1,   // MC2_B_GATE\n", ""), ents)
