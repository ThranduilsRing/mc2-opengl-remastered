"""Independent census of the stock objective FIT blocks (no editor executable)."""
import collections
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from run_editor_smoke import StockInputs, STOCK_MISSIONS

AUTHORING_KEYS = (
    "Title", "Description", "Priority", "ResourcePoints", "PreviousPrimaryObjectiveMustBeComplete",
    "AllPreviousPrimaryObjectivesMustBeComplete", "DisplayMarker", "MarkerX", "MarkerY", "HiddenTrigger",
    "ActivateOnFlag", "ActivateFlagID", "ResetStatusOnFlag", "ResetStatusFlagID", "TitleUseResourceString",
    "TitleResourceStringID", "DescriptionUseResourceString", "DescriptionResourceStringID", "ObjectiveModel",
    "ModelType", "BaseColor", "HighlightColor", "HighlightColor2", "ModelScale", "ModelID", "ModelID Version",
)


def grade_typed_objectives(inputs, output):
    from editor.tests.legacy_reference import typed_fields
    for mission in STOCK_MISSIONS:
        source = objective_fields((inputs / f"{mission}.fit").read_bytes())
        expected = {k: v for k, v in source.items()
                    if re.fullmatch(r"(?:Team\d+)?Objective\d+", k.partition("/")[0]) and k.partition("/")[2] in AUTHORING_KEYS}
        actual = objective_fields((output / f"{mission}.objectives.fit").read_bytes())
        if typed_fields(actual) != typed_fields(expected):
            raise ValueError(f"typed objective model disagrees with independent stock decode: {mission}")


def grade_objective_edit(before, after, key):
    from editor.tests.legacy_reference import typed_fields
    original = objective_fields(before)
    actual = objective_fields(after)
    path = "Team0Objective1/" + key
    prefix, value = original[path][0].split(" ", 1)
    if prefix == "st":
        expected = "E6a objective\n\nsecond line\n" if key in ("Title", "Description") else "E6a objective scalar"
        saved = actual.get(path, [""])[0]
        if not saved.startswith('st "') or not saved.endswith('"'):
            raise ValueError(f"objective text declaration wrong: {key}")
        text = saved[4:-1].replace("\r\n", "\n")
        # The real FIT reader joins the first two physical lines before objective text conversion.
        text = text.replace("\n", "", 1)
        if text != expected:
            raise ValueError(f"objective scalar saved value wrong: {key}")
    elif prefix == "b":
        expected = "FALSE" if value == "TRUE" else "TRUE"
    elif prefix == "f":
        expected = "123.25"
    else:
        expected = "2" if key == "Priority" else "13" if key == "ModelType" else "17"
    if prefix != "st" and (path not in actual or typed_fields({path: [prefix + " " + expected]}) != typed_fields({path: actual[path]})):
        raise ValueError(f"objective scalar saved value wrong: {key}")
    # Unedited spans, including comments and opaque fields, must be byte-identical.
    pattern = rb"(?m)(^\[Team0Objective1\]\r?\n(?:(?!^\[)[\s\S])*?^\w+[ \t]+" + re.escape(key.encode()) + rb'[ \t]*=[ \t]*)("[^"]*"|[^ \t/\r\n]+)'
    def masked(data):
        result, count = re.subn(pattern, lambda m: m[1] + b"<edited>", data)
        if count != 1:
            raise ValueError("objective edit target is missing or ambiguous")
        return result
    if masked(before) != masked(after) or before == after:
        raise ValueError("objective scalar edit changed unrelated bytes or did nothing")


def objective_fields(data):
    """Stock flat objective syntax includes spaced keys such as ModelID Version."""
    result = {}
    block = ""
    pattern = re.compile(r'(?m)^[ \t]*\[([^]\r\n]+)\][ \t]*\r?$|^[ \t]*(\w+)[ \t]+([^=\r\n]+?)[ \t]*=[ \t]*("[^"]*"|[^"\r\n]*)')
    for match in pattern.finditer(data.decode("latin-1")):
        if match[1] is not None:
            block = match[1]
        elif re.fullmatch(r"(?:Team\d+)?Objective\d+.*|(?:Team\d+)?Objectives|Team\d+BooleanFlags|Objectives Version", block):
            value = match[4]
            if not value.startswith('"'):
                value = value.partition("//")[0].rstrip()
            result[block + "/" + match[3]] = [match[2] + " " + value]
    return result


def census(stock):
    fields = collections.Counter()
    payloads = collections.defaultdict(collections.Counter)
    lists = collections.Counter()
    hashes = {}
    versions = collections.Counter()
    flag_fields = collections.Counter()
    objectives = 0
    for mission in STOCK_MISSIONS:
        data = stock.read(f"data/missions/{mission}.fit")
        hashes[mission] = hashlib.sha256(data).hexdigest()
        parsed = objective_fields(data)
        versions[parsed["Objectives Version/Version"][0]] += 1
        blocks = collections.defaultdict(dict)
        for path, values in parsed.items():
            block, _, key = path.partition("/")
            blocks[block][key] = values
        for block, values in blocks.items():
            if re.fullmatch(r"Team\d+BooleanFlags", block):
                flag_fields.update(values.keys())
            if re.fullmatch(r"(?:Team\d+)?Objective\d+", block):
                objectives += 1
                fields.update(values.keys())
            match = re.fullmatch(r"(?:Team\d+)?Objective\d+(FailureCondition|FailureAction|Condition|Action)\d+", block)
            if match:
                group = match[1]
                species = values[group + "SpeciesString"][0].partition('"')[2].rpartition('"')[0]
                lists[group + "/" + species] += 1
                payloads[species].update(values.keys())
    return dict(missions=hashes, objectives=objectives, versions=dict(versions), teamBooleanFlags=dict(sorted(flag_fields.items())), fields=dict(sorted(fields.items())),
                lists=dict(sorted(lists.items())), payloads={k: dict(sorted(v.items())) for k, v in sorted(payloads.items())})


if __name__ == "__main__":
    report = census(StockInputs(Path("A:/Games/mc2-opengl/mc2-win64-lane3")))
    result = json.dumps(report, indent=2) + "\n"
    if len(sys.argv) > 1:
        Path(sys.argv[1]).write_text(result, encoding="utf-8", newline="\n")
    print(result)
