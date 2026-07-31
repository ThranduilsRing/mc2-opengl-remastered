#!/usr/bin/env python3
"""REGISTRY-PARSER-TRUTH-1: the ONE parser for RenderCore/RendererFeatureRegistry.h.

WHY THIS MODULE EXISTS
----------------------
The registry header declares every MC2_* gate the engine knows about.  Four
different tools read it, and until 2026-07-31 three of them carried their own
private regex for the row grammar.  All three were wrong, in different ways, and
every one of them reported success:

  * scripts/build_review_pack.py     -- 10 of 359 rows under a torn read, exit 0,
                                        a well-formed page with unsourced
                                        defaults (fixed by PACK-FAILLOUD-1,
                                        399b2218f; this module is its parser,
                                        lifted verbatim and shared).
  * tools/repo_intel/env_index.py    -- 359 of 360 rows.  The dropped row made
                                        the gate_status MCP tool answer
                                        `registered: false, verdict: OK, flags: []`
                                        about MC2_LIGHTING_DEBUG_VIEW while its
                                        OWN reader list cited the registry line
                                        it had failed to parse.
  * scripts/flag-registry-auto-gen.py -- 248 of 360 rows, and its output is the
                                        TRACKED file docs/flags.auto.yaml, which
                                        asserted `registered: no` about 73 gates
                                        that are registered.

A fourth reader, scripts/check-env-registry.sh, greps quoted MC2_* names rather
than parsing rows, so row-grammar drops never reached it.  It keeps its own
string-aware comment stripper (TREE-CONSOLIDATE-1) and is deliberately NOT
consolidated here: it answers a different question (which names appear) with a
different tool (shell), and forcing it through this module would buy nothing.

Consolidation was applied with care, not enthusiasm: this repo already carries
the same broken comment-stripper idea copy-pasted into many private versions,
and the cure for that is one implementation with one set of invariants -- not a
fourth dialect.

THE THREE ROW-DROP CAUSES MEASURED ON HEAD 399b2218f
----------------------------------------------------
  1. ADJACENT STRING LITERALS.  C concatenates them, so a long doc is written as
       "first part "
       "second part"
     A grammar that expects exactly one literal before `}` drops the row.
     99 of the 112 rows flag-registry-auto-gen.py lost went this way -- by far
     the biggest cause, and the one nobody had guessed.
  2. INLINE COMMENT BETWEEN FIELDS, e.g.
       EnvVarKind::Feature,   // not Trace: it REPLACES the lit render output
     A grammar joining fields with `\\s*` drops the row.  This is the single row
     env_index.py lost, and it is the cause PACK-FAILLOUD-1 fixed next door.
  3. TRAILING COMMA AFTER THE DOC STRING, i.e. `"doc",\n}` rather than `"doc"\n}`.
     Legal C++, and 12 rows use it.

All three are legal C++ that a human will keep writing.  The grammar below
tolerates all three; the invariants below catch anything it does not.

THE INVARIANTS ARE SELF-REFERENTIAL ON PURPOSE
----------------------------------------------
No pinned counts, no floors, no "at least N rows" -- those go stale the day
someone adds a gate, and a stale guard gets deleted.  Instead:

  1. both arrays must be PRESENT,
  2. both must be brace-CLOSED (a truncated read is not a small registry),
  3. every row the file DECLARES must actually parse -- with the denominator
     scanned over the WHOLE FILE, independently of the brace-matching that
     extracts the bodies, so the two numbers cannot shrink together,
  4. every `EnvVarKind::` field in a table body must belong to a parsed row --
     a SECOND, independent denominator, added because (3) alone has a hole:
     ROW_OPENER_RE keys on `{ "MC2_..."`, so a row written with designated
     initializers (`{ .featureId = "MC2_X", ... }`) or with a comment between
     the brace and the name is invisible to BOTH (3)'s denominator and the entry
     grammar, and drops with the two counts still agreeing.  Injection-proven:
     before this invariant such a row was accepted silently at 360 rows.  Every
     row must name a kind, so this counts rows the name-anchored pattern cannot
     see,
  5. no two rows may publish the same key.

A legitimate edit that adds or retires gates moves every side of (3) and (4)
together and passes.  Only a parse that LOST rows fails.  That property is what
makes this safe to wire into a contract board.

Python 3, standard library only, no repo imports -- so every consumer can import
it without dragging in a dependency graph.
"""

from __future__ import annotations

import os
import re

__all__ = [
    "RegistryParseError",
    "parse_registry",
    "parse_registry_text",
    "declared_rows",
    "ENTRY_RE",
    "ROW_OPENER_RE",
    "TABLES",
]

#: The two arrays that together are the gate inventory.
TABLES = ("kFeatureTable", "kAuxEnvVars")

# Whitespace-or-comment gap between two structural tokens of a registry row.
# Applied ONLY between tokens -- never inside a string literal, whose contents
# the surrounding subpattern consumes atomically -- so a "//" or "/*" occurring
# inside a doc string can never be mistaken for a comment.  This is deliberately
# NOT a whole-file comment stripper: those get the string boundaries wrong, and
# this repo already carries several broken copies of that idea.
_GAP = r"(?:\s|//[^\n]*\n|/\*(?:[^*]|\*(?!/))*\*/)*"

#: One string literal, escapes consumed atomically.
_STR = r'"(?:[^"\\]|\\.)*"'

ENTRY_RE = re.compile(
    r"\{" + _GAP
    + r'"(?P<feature>MC2_[A-Z0-9_]+)"' + _GAP + "," + _GAP
    + r"(?P<env>" + _STR + r"|nullptr)" + _GAP + "," + _GAP
    + r"EnvVarKind::(?P<kind>\w+)" + _GAP + "," + _GAP
    + r"(?P<default>true|false)" + _GAP + "," + _GAP
    # doc: one or more ADJACENT literals (cause 1), then an OPTIONAL trailing
    # comma (cause 3), then the closing brace.
    + r"(?P<doc>" + _STR + r"(?:" + _GAP + _STR + r")*)" + _GAP + r",?" + _GAP
    + r"\}",
    re.S,
)

# INDEPENDENT denominator: how many rows the header DECLARES.  Deliberately
# scanned over the whole file rather than over the extracted table bodies, so
# it does not inherit _table_body()'s brace-counting assumptions -- if a stray
# brace inside a doc string ever truncated a body, the two numbers diverge and
# the mismatch is caught instead of shrinking both sides together.
ROW_OPENER_RE = re.compile(r'\{\s*"(MC2_[A-Z0-9_]+)"\s*,')

#: THIRD denominator, independent of the gate NAME.  ROW_OPENER_RE is anchored on
#: `{ "MC2_..."`, so it cannot see a row whose name is not the first token --
#: designated initializers, or a comment between the brace and the name.  Such a
#: row is invisible to the entry grammar AND to ROW_OPENER_RE at the same time,
#: so both counts stay equal and the row drops silently.  Every row must name a
#: kind, so counting kind fields sees rows the name-anchored pattern cannot.
KIND_FIELD_RE = re.compile(r"EnvVarKind::\w+")

#: One string literal, for splitting a captured doc back into its parts.
_STR_FINDALL = re.compile(_STR)


def _mask(text: str, blank_strings: bool) -> str:
    """Blank out comments (and optionally string literals), preserving length.

    A character-by-character state machine, NOT a regex strip.  This repo has
    repeatedly shipped regex comment-strippers that get string boundaries wrong
    -- most recently one where a block-comment opener inside a doc string made
    the stripper discard the header from that line to EOF, so every gate below
    it reported UNREGISTERED.  The three states here are exactly the three that
    bug conflated: inside a string, inside a comment, neither.

    Length and newlines are preserved so offsets stay comparable with the
    unmasked text -- that is what lets the caller ask "is the character at this
    offset inside a comment?" without re-scanning.

    `blank_strings=False` keeps literals intact, which the DECLARED-row scan
    needs (its pattern is anchored on the gate name, so blanking strings would
    blank the very thing it counts).
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            if blank_strings:
                for k in range(i, min(j + 1, n)):
                    if out[k] != "\n":
                        out[k] = " "
            i = j + 1
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j == -1 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def _mask_strings_and_comments(text: str) -> str:
    return _mask(text, blank_strings=True)


def _mask_comments(text: str) -> str:
    return _mask(text, blank_strings=False)


class RegistryParseError(Exception):
    """The gate registry did not parse cleanly -- refuse to answer from it.

    Every consumer must treat this as "I could not determine this", NEVER as
    "the gate is not registered".  Those two are the same bytes to a naive
    caller and opposite facts to a user.
    """


def _unquote(lit: str) -> str:
    """Join and unescape one or more ADJACENT C string literals."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', lit)
    s = "".join(parts)
    return (
        s.replace("\\\\", "\x00")
        .replace('\\"', '"')
        .replace("\\n", "\n")
        .replace("\\t", "\t")
        .replace("\x00", "\\")
    )


def _table_body(text: str, name: str):
    """-> (body, status) where status is 'ok' | 'missing' | 'unclosed'.

    'unclosed' means the array opened but its matching '}' was never reached --
    i.e. the text ran out mid-table.  That is what a TRUNCATED or mid-write read
    of the header looks like, and the caller must treat it as a hard error: the
    rows past the cut are simply absent, with nothing in the returned body to
    say so.
    """
    m = re.search(r"EnvVarDesc\s+%s\s*\[\s*\]\s*=\s*\{" % re.escape(name), text)
    if not m:
        return "", "missing"
    depth, i = 1, m.end()
    while depth and i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    if depth:
        return text[m.end(): i], "unclosed"
    return text[m.end(): i - 1], "ok"


def declared_rows(text: str) -> list:
    """Every gate row the header DECLARES, by featureId, in file order.

    This is invariant (3)'s denominator.  It is intentionally a cruder pattern
    than ENTRY_RE: it can only over-count relative to a correct parse, never
    under-count, so "declared > parsed" always means rows were lost.

    Scanned with COMMENTS BLANKED (string literals kept, since the pattern is
    anchored on the gate name).  Commenting a row out is an ordinary maintainer
    action, and counting commented-out rows as declared made the parser refuse a
    perfectly legitimate edit -- a false positive found by this lane's own
    refutation battery, not in production.  String literals are NOT blanked
    here, and the masker is string-aware, so a row-shaped fragment quoted inside
    a doc string still cannot be miscounted as a declaration.
    """
    return [m.group(1) for m in ROW_OPENER_RE.finditer(_mask_comments(text))]


def parse_registry_text(text: str, where: str = "<text>") -> dict:
    """parse_registry() for an in-memory header.  Same invariants, same raises."""
    declared = declared_rows(text)

    reg: dict = {}
    parsed: list = []
    kind_fields = 0
    for table in TABLES:
        body, status = _table_body(text, table)
        if status == "missing":
            raise RegistryParseError(
                "table %s not found in %s -- the header is truncated, renamed "
                "or unreadable; refusing to answer from a partial gate table"
                % (table, where))
        if status == "unclosed":
            raise RegistryParseError(
                "table %s in %s opens but never closes -- the file is "
                "truncated (read %d of %d chars); refusing to answer"
                % (table, where, len(body), len(text)))
        # Both masks of this body, once. Offsets line up with `body` because the
        # masker preserves length.
        body_nc = _mask_comments(body)
        kind_fields += len(KIND_FIELD_RE.findall(_mask(body_nc, blank_strings=True)))
        for m in ENTRY_RE.finditer(body):
            # A row sitting INSIDE a /* ... */ is commented out, not declared.
            # ENTRY_RE runs on the raw body (so doc strings survive intact), so
            # it happily matches such a row; the comment mask is what tells them
            # apart. Without this the kind count and the parsed count disagreed
            # and a commented-out row was refused as a lost row.
            if body_nc[m.start()] != "{":
                continue
            env = m.group("env")
            entry = {
                "featureId": m.group("feature"),
                "envVar": None if env == "nullptr" else _unquote(env),
                "kind": m.group("kind"),
                "defaultOn": m.group("default") == "true",
                "doc": _unquote(m.group("doc")),
                "table": table,
            }
            parsed.append(entry)
            key = entry["envVar"] or entry["featureId"]
            if key in reg:
                raise RegistryParseError(
                    "two registry rows publish the same key %r (%s in %s, and "
                    "%s in %s) -- a caller would be shown one of two conflicting "
                    "defaults with no way to tell which"
                    % (key, reg[key]["featureId"], reg[key]["table"],
                       entry["featureId"], table))
            reg[key] = entry

    if len(parsed) != len(declared):
        got = set(x["featureId"] for x in parsed)
        lost = [d for d in declared if d not in got]
        if lost:
            raise RegistryParseError(
                "%s declares %d gate rows but only %d parsed -- %d row(s) were "
                "silently skipped: %s%s.  A row is skipped when it does not match "
                "the entry grammar (a stray token between fields, an unterminated "
                "string) or when the file was read mid-write.  Refusing to answer "
                "from a gate table that is missing rows the source declares."
                % (where, len(declared), len(parsed), len(declared) - len(parsed),
                   ", ".join(lost[:8]), " ..." if len(lost) > 8 else ""))
        # The other direction: the entry grammar matched rows the declared-row
        # scan could not see.  Not harmless -- the two scans disagree about what
        # a row IS, so neither count can be trusted as the denominator.
        raise RegistryParseError(
            "%s: the entry grammar parsed %d rows but the declared-row scan "
            "found only %d -- the two independent counts disagree about what a "
            "row is, so neither can serve as the denominator.  Refusing to "
            "answer until they are reconciled." % (where, len(parsed), len(declared)))

    if kind_fields != len(parsed):
        raise RegistryParseError(
            "%s: %d EnvVarKind:: field(s) inside the gate tables but %d row(s) "
            "parsed -- %d row(s) carry a kind and were NOT parsed.  This is the "
            "hole the declared-row count alone cannot see: a row whose gate name "
            "is not its first token (designated initializers, or a comment "
            "between the brace and the name) is invisible to BOTH the entry "
            "grammar and the declared-row scan, so it drops with those two "
            "counts still agreeing.  Refusing to answer from a gate table that "
            "is missing rows."
            % (where, kind_fields, len(parsed), kind_fields - len(parsed)))
    return reg


def parse_registry(header_path) -> dict:
    """Return {ENV_VAR or featureId: {featureId, envVar, kind, defaultOn, doc, table}}.

    Raises RegistryParseError rather than returning a partial result.  Callers
    MUST NOT convert that into a negative answer: "I could not parse the
    registry" and "this gate is not in the registry" must stay distinguishable
    all the way to whoever is reading the output.
    """
    header_path = str(header_path)
    # strict, not errors="replace": U+FFFD substituted into a gate NAME would
    # drop that row from the parse AND from the declared-row count, hiding
    # itself from the very check meant to catch it.  A header that does not
    # decode is not the source we think we are reading -- refuse.
    try:
        with open(header_path, encoding="utf-8", errors="strict") as fh:
            text = fh.read()
    except FileNotFoundError as exc:
        # Distinct from "empty registry".  The old env_index returned {} for
        # both, so "the header is not where I looked" was indistinguishable
        # from "no gate is registered" -- a silent-degradation path in its own
        # right, and the second one this module closes.
        raise RegistryParseError(
            "%s does not exist -- refusing to report an empty gate registry "
            "for a header that was never read" % header_path) from exc
    except OSError as exc:
        raise RegistryParseError(
            "%s could not be read (%s) -- refusing to report an empty gate "
            "registry" % (header_path, exc)) from exc
    except UnicodeDecodeError as exc:
        raise RegistryParseError(
            "%s is not valid UTF-8 (%s) -- refusing to answer from a gate table "
            "parsed from undecodable bytes" % (header_path, exc)) from exc

    return parse_registry_text(text, header_path)


# ---------------------------------------------------------------------------
# SELF-TEST
# ---------------------------------------------------------------------------
#
# Two halves, and the FIRST is the one nothing was testing.
#
# TOLERANCE.  Every row this parser has ever lost was lost to legal C++ that a
# narrower grammar rejected -- adjacent string literals, an inline comment
# between fields, a trailing comma.  A refusal-only test cannot catch a grammar
# that has been "simplified" back into dropping those, because the invariants
# would then fire on the real header and someone would relax THEM to make the
# board green.  So each tolerance arm first PROVES the construct still occurs in
# the live header (INCONCLUSIVE, not PASS, if it does not), then requires the
# parser to accept it and to reproduce its full content.
#
# REFUSAL.  Injected into the REAL header, never a fixture: a fixture stops
# resembling the file it stands for, and this session has already seen a check
# pass while blind because its fixture had drifted.

def _has_comment_between_fields(raw: str) -> bool:
    """True when a // or /* sits between two fields of the row.

    Decided by blanking string literals FIRST, so a "//" that is merely text
    inside a doc string is not mistaken for a comment -- the same conflation
    that once made a regex stripper discard this header from a doc string to
    EOF and report every gate below it as unregistered.
    """
    outside = _STR_FINDALL.sub("", raw)
    return "//" in outside or "/*" in outside


def _find_row_spans(text):
    """-> [(featureId, raw_row_text)] for every row the entry grammar sees."""
    spans = []
    for table in TABLES:
        body, status = _table_body(text, table)
        if status != "ok":
            continue
        for m in ENTRY_RE.finditer(body):
            spans.append((m.group("feature"), m.group(0), m.group("doc")))
    return spans


def selftest(header_path) -> int:
    import tempfile

    header_path = str(header_path)
    try:
        text = open(header_path, encoding="utf-8").read()
    except OSError as exc:
        print("SELF-TEST INCONCLUSIVE: cannot read %s (%s)" % (header_path, exc))
        return 1

    failures = 0

    # -- control ------------------------------------------------------------
    try:
        reg = parse_registry_text(text, header_path)
    except RegistryParseError as exc:
        print("  FAIL  the LIVE registry was refused: %s" % exc)
        return 1
    print("  OK    live registry parses: %d rows (%d declared, %d kind fields)"
          % (len(reg), len(declared_rows(text)),
             sum(len(KIND_FIELD_RE.findall(_mask_strings_and_comments(_table_body(text, t)[0])))
                 for t in TABLES)))

    spans = _find_row_spans(text)

    # -- tolerance ----------------------------------------------------------
    # Cause 1: adjacent string literals (99 of the 112 rows one parser lost).
    multi = [(f, raw, doc) for f, raw, doc in spans
             if len(_STR_FINDALL.findall(doc)) > 1]
    if not multi:
        print("  INCONCLUSIVE  no row uses adjacent string literals any more -- "
              "this arm no longer proves anything; re-anchor it")
        failures += 1
    else:
        f, raw, doc = multi[0]
        joined = _unquote(doc)
        first = _unquote(_STR_FINDALL.findall(doc)[0])
        ok = f in [e["featureId"] for e in reg.values()] and len(joined) > len(first)
        print("  %s  tolerates adjacent string literals   (%d such rows, e.g. %s: "
              "doc %d chars, first literal alone %d)"
              % ("OK  " if ok else "FAIL", len(multi), f, len(joined), len(first)))
        failures += 0 if ok else 1

    # Cause 2: an inline comment between two fields of a row.  A comment counts
    # only if it survives OUTSIDE every string literal -- a "//" inside a doc
    # string is just text, and mistaking the two is the exact bug that has
    # broken comment handling in this repo before.
    commented = [f for f, raw, doc in spans if _has_comment_between_fields(raw)]
    if not commented:
        print("  INCONCLUSIVE  no row carries an inline comment between fields -- "
              "re-anchor this arm")
        failures += 1
    else:
        ids = set(e["featureId"] for e in reg.values())
        ok = all(c in ids for c in commented)
        print("  %s  tolerates inline comments between fields (%d such rows, e.g. %s)"
              % ("OK  " if ok else "FAIL", len(commented), commented[0]))
        failures += 0 if ok else 1

    # Cause 3: a trailing comma after the doc string.
    trailing = [f for f, raw, doc in spans if re.search(r'"\s*,\s*\}\s*$', raw)]
    if not trailing:
        print("  INCONCLUSIVE  no row uses a trailing comma after its doc -- "
              "re-anchor this arm")
        failures += 1
    else:
        ids = set(e["featureId"] for e in reg.values())
        ok = all(t in ids for t in trailing)
        print("  %s  tolerates a trailing comma after the doc  (%d such rows, e.g. %s)"
              % ("OK  " if ok else "FAIL", len(trailing), trailing[0]))
        failures += 0 if ok else 1

    # -- refusal ------------------------------------------------------------
    anchor = spans[-1][1] if spans else ""
    arms = []

    # A row form invisible to BOTH the entry grammar and the declared-row scan.
    arms.append((
        "a designated-initializer row (invisible to the name-anchored scan)",
        text.replace(
            "static constexpr EnvVarDesc kAuxEnvVars[] = {",
            "static constexpr EnvVarDesc kAuxEnvVars[] = {\n"
            '    { .featureId = "MC2_SELFTEST_HOLE", .envVar = "MC2_SELFTEST_HOLE",\n'
            "      .kind = EnvVarKind::Trace, .defaultOn = false, .doc = \"probe\" },\n", 1),
        "EnvVarKind:: field"))

    # Two rows publishing the same key.
    if anchor:
        arms.append((
            "two rows publishing the same key",
            text.replace(anchor, anchor + ",\n    " + anchor, 1),
            "same key"))

    # A stray token between fields that is NOT a comment -- still unparseable.
    stray = re.search(r"(EnvVarKind::\w+,)(\s*\n\s*)(true|false,)", text)
    if stray:
        arms.append((
            "a stray token between two fields",
            text[:stray.start()] + stray.group(1) + stray.group(2) + "0, "
            + stray.group(3) + text[stray.end():],
            "silently skipped"))

    # A table emptied of rows but left structurally intact is NOT an error --
    # see the note in the report; it is a legitimate (if drastic) source edit.
    tmpdir = tempfile.mkdtemp(prefix="mc2_regparse_")
    tmp = os.path.join(tmpdir, "RendererFeatureRegistry.h")
    try:
        for label, blob, expect in arms:
            if blob == text:
                print("  FAIL  arm %r did not modify anything -- BLIND" % label)
                failures += 1
                continue
            with open(tmp, "w", encoding="utf-8", newline="") as fh:
                fh.write(blob)
            try:
                got = parse_registry(tmp)
            except RegistryParseError as exc:
                if expect in str(exc):
                    print("  OK    refused: %s" % label)
                else:
                    print("  FAIL  refused %s but for the wrong reason: %s" % (label, exc))
                    failures += 1
            else:
                print("  FAIL  ACCEPTED %s -- %d rows published from a registry "
                      "that lost one" % (label, len(got)))
                failures += 1
    finally:
        try:
            os.remove(tmp)
            os.rmdir(tmpdir)
        except OSError:
            pass

    if failures:
        print("SELF-TEST FAIL: %d arm(s) did not behave" % failures)
        return 1
    print("SELF-TEST PASS: the shared registry parser tolerates every legal row "
          "form the header actually uses, and refuses a row it cannot see")
    return 0


if __name__ == "__main__":
    import argparse

    _here = os.path.dirname(os.path.abspath(__file__))
    _default = os.path.join(os.path.dirname(_here), "RenderCore",
                            "RendererFeatureRegistry.h")
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--registry", default=_default)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--count", action="store_true",
                    help="print the parsed row count and exit")
    a = ap.parse_args()
    if a.selftest:
        raise SystemExit(selftest(a.registry))
    try:
        _reg = parse_registry(a.registry)
    except RegistryParseError as _exc:
        print("REFUSED: %s" % _exc)
        raise SystemExit(2)
    print("%d rows" % len(_reg))
    raise SystemExit(0)
