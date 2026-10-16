"""Independent CRUD expectations: positional names, unchanged bodies and explicit defaults."""
import re


# Authoring defaults come from the legacy constructor and model definition.
NEW_FIELDS = {
    "Title": ['st ""'], "Description": ['st ""'], "Priority": ["ul 1"], "ResourcePoints": ["ul 0"],
    **{key: ["b FALSE"] for key in (
        "PreviousPrimaryObjectiveMustBeComplete", "AllPreviousPrimaryObjectivesMustBeComplete", "DisplayMarker",
        "HiddenTrigger", "ActivateOnFlag", "ResetStatusOnFlag", "TitleUseResourceString", "DescriptionUseResourceString")},
    **{key: ["st \"\""] for key in ("ActivateFlagID", "ResetStatusFlagID", "ObjectiveModel")},
    **{key: ["ul 0"] for key in ("TitleResourceStringID", "DescriptionResourceStringID", "NumConditions", "NumActions", "NumFailureConditions", "NumFailureActions")},
    **{key: ["l -1"] for key in ("BaseColor", "HighlightColor", "HighlightColor2", "ModelID")},
    "MarkerX": ["f 0"], "MarkerY": ["f 0"], "ModelScale": ["f 1"], "ModelType": ["l 0"], "ModelID Version": ["ul 2"],
}


def blocks(data):
    headers = list(re.finditer(rb"(?m)^\[([^\]\r\n]+)\]", data))
    end = data.rfind(b"FITend")
    if not headers or end < headers[-1].end():
        raise ValueError("CRUD FIT framing missing")
    result = {}
    for i, header in enumerate(headers):
        name = header[1].decode("ascii")
        if name in result:
            raise ValueError("CRUD duplicate section")
        result[name] = data[header.end():headers[i + 1].start() if i + 1 < len(headers) else end]
    return data[:headers[0].start()], result, data[end:]


def grade_crud_edit(before, after, operation):
    if operation.startswith("entry-"):
        return grade_entry_edit(before, after, operation)
    from editor.tests.objective_census import objective_fields
    from editor.tests.legacy_reference import typed_fields
    prefix, original, ending = blocks(before)
    actual_prefix, actual, actual_ending = blocks(after)
    if prefix != actual_prefix or ending != actual_ending or before == after:
        raise ValueError("CRUD framing changed or output is a no-op")
    count_match = re.search(rb"ul NumObjectives\s*=\s*(\d+)", original["Team0Objectives"])
    if not count_match:
        raise ValueError("CRUD team count missing")
    count = int(count_match[1])
    if count < 3:
        raise ValueError("CRUD fixture needs three objectives")
    order = list(range(count))
    if operation == "objective-add":
        order.append(None)
    elif operation == "objective-copy":
        order.insert(2, 1)
    elif operation == "objective-delete":
        order.remove(1)
    elif operation == "objective-up":
        order[:2] = [1, 0]
    elif operation == "objective-down":
        order[1:3] = [2, 1]
    else:
        raise ValueError("unknown CRUD proof operation")
    expected = {key: value for key, value in original.items() if not re.match(r"Team0Objective\d+(?:\D|$)", key)}
    expected["Team0Objectives"] = (original["Team0Objectives"][:count_match.start(1)] + str(len(order)).encode() +
                                    original["Team0Objectives"][count_match.end(1):])
    for index, source in enumerate(order):
        name = f"Team0Objective{index}"
        if source is None:
            body = actual.pop(name, None)
            if body is None:
                raise ValueError("CRUD new objective missing")
            declarations = set()
            for line in body.splitlines():
                if not line.strip():
                    continue
                declaration = re.fullmatch(rb'(st|ul|l|f|b)[ \t]+([^=\r\n]+?)[ \t]*=[ \t]*("[^"\r\n]*"|[^"\r\n]+)', line)
                if not declaration or declaration[2] in declarations:
                    raise ValueError("CRUD new objective has malformed or duplicate declarations")
                declarations.add(declaration[2])
            fields = objective_fields(b"[" + name.encode() + b"]" + body)
            expected_fields = {f"{name}/{key}": value for key, value in NEW_FIELDS.items()}
            if typed_fields(fields) != typed_fields(expected_fields):
                raise ValueError("CRUD new objective defaults or empty lists differ")
            continue
        for key, value in original.items():
            match = re.fullmatch(rf"Team0Objective{source}(\D.*)?", key)
            if match:
                expected[name + (match[1] or "")] = value
    if actual != expected:
        changed = sorted(key for key in actual.keys() | expected.keys() if actual.get(key) != expected.get(key))
        raise ValueError("CRUD count/order/content/preserved bytes differ: " + ", ".join(changed[:8]))


def grade_crud_panel(directory):
    before = (directory / "fixture.fit").read_bytes()
    from scripts.run_editor_smoke import PANEL_OPERATIONS
    for operation in PANEL_OPERATIONS["objectives_crud_panel"]:
        grade_crud_edit(before, (directory / f"{operation}-edited.fit").read_bytes(), operation)


def grade_entry_edit(before, after, operation):
    prefix, expected, ending = blocks(before)
    actual_prefix, actual, actual_ending = blocks(after)
    if (prefix, ending) != (actual_prefix, actual_ending) or before == after:
        raise ValueError("entry framing changed or no-op export")
    _, number, action = operation.split("-")
    index = int(number)
    suffix = ("Condition", "Action", "FailureCondition", "FailureAction")[index]
    key = ("NumConditions", "NumActions", "NumFailureConditions", "NumFailureActions")[index]
    owner = "Team0Objective1"
    base = owner + suffix
    first, second = expected[base + "0"], expected[base + "1"]
    count = 2
    if action == "add":
        count = 3
        species = "BooleanFlagIsSet" if index % 2 == 0 else "SetBooleanFlag"
        expected[base + "2"] = (f'\r\nst {suffix}SpeciesString = "{species}"\r\n'
                                f'st FlagID = "{"created-flag" if index % 2 else ""}"\r\nb Value = TRUE\r\n').encode()
    elif action == "delete":
        count = 1
        expected[base + "0"] = second
        del expected[base + "1"]
    elif action in ("up", "down"):
        expected[base + "0"], expected[base + "1"] = second, first
    elif action == "flag":
        expected[base + "0"] = first.replace(b'"e6c-first"', f'"e6c-edited-{index}"'.encode()).replace(b"Value = FALSE", b"Value = TRUE")
    else:
        raise ValueError("unknown entry operation")
    if count != 2:
        expected[owner], changes = re.subn(rb"(ul " + key.encode() + rb"\s*=\s*)2\b", lambda m: m[1] + str(count).encode(), expected[owner])
        if changes != 1:
            raise ValueError("entry count fixture missing")
    if actual != expected:
        raise ValueError("entry count/order/flag payload/preserved bytes differ")
