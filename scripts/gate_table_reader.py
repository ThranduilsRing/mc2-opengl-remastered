#!/usr/bin/env python3
"""gate_table_reader -- read the compiled gate table (Gates/gate_table.json) from Python tools.

The JSON is emitted by tools/gate_dump from the C++ table that the engine itself compiles, so a reader
here never parses C++. It replaces scripts/registry_parse.py, which parsed RendererFeatureRegistry.h.

  load_table(root)            the JSON document, or GateTableError when it is missing / not valid
  gates(root)                 {name: row} for every gate
  registry_entries(root)      {key: {featureId, envVar, kind, defaultOn, doc, table}} for the gates that have a
                              registry entry, in the order of the generated header (kFeatureTable by index,
                              then kAuxEnvVars); key is the env var, or the featureId for a row with none
  registry_entries_from_text  the same from JSON text (a git revision's blob)

A reader that cannot read the table raises; it never returns an empty answer, so "I could not read the
source" can not be reported as "this gate is not registered".
"""
import json
import os

TABLE_REL = "Gates/gate_table.json"
ONLY_REL = "Gates/feature_only_rows.json"


class GateTableError(Exception):
    pass


def _read(root, rel):
    p = os.path.join(str(root), rel.replace("/", os.sep))
    try:
        with open(p, "r", encoding="utf-8") as fh:
            return fh.read()
    except OSError as ex:
        raise GateTableError("cannot read %s: %s" % (rel, ex))


def parse_table(text):
    try:
        doc = json.loads(text)
    except ValueError as ex:
        raise GateTableError("%s is not valid JSON: %s" % (TABLE_REL, ex))
    if not isinstance(doc, dict) or not isinstance(doc.get("gates"), list) or doc.get("count") != len(doc["gates"]):
        raise GateTableError("%s is not a complete gate table (count/gates mismatch)" % TABLE_REL)
    return doc


def load_table(root):
    return parse_table(_read(root, TABLE_REL))


def gates(root):
    return {g["name"]: g for g in load_table(root)["gates"]}


def registry_entries_from_text(table_text, only_text=None):
    doc = parse_table(table_text)
    only = json.loads(only_text) if only_text else []
    feat, aux = [], []
    for g in doc["gates"]:
        r = g.get("registry")
        if not r:
            continue
        e = {"featureId": r["featureId"], "envVar": g["name"], "kind": r["kind"], "defaultOn": r["defaultOn"],
             "doc": g["doc"], "table": "kFeatureTable" if r["order"] >= 0 else "kAuxEnvVars"}
        (feat if r["order"] >= 0 else aux).append((r["order"], g["name"], e))
    for o in only:
        e = {"featureId": o["featureId"], "envVar": None, "kind": o["kind"], "defaultOn": o["defaultOn"],
             "doc": o["doc"], "table": "kFeatureTable" if o["order"] >= 0 else "kAuxEnvVars"}
        (feat if o["order"] >= 0 else aux).append((o["order"], o["featureId"], e))
    feat.sort(key=lambda t: t[0])
    aux.sort(key=lambda t: (t[1], t[2]["featureId"]))
    out = {}
    for _o, key, e in feat + aux:
        out[key] = e
    return out


def registry_entries(root):
    try:
        only = _read(root, ONLY_REL)
    except GateTableError:
        only = None
    return registry_entries_from_text(_read(root, TABLE_REL), only)
