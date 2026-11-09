#!/usr/bin/env python3
"""Verdict of the box `unit` row from doctest's XML report (mc2_tests -r=xml -o=unit.xml).

Doctest's console output prints every failed CHECK of a `doctest::should_fail()` case (the seeded negative controls,
e.g. demo_intentional_fail) and of crash-by-design children (job_system fatal_if_aborts_and_traces), so grepping it
for ERROR: reported controls as red. The XML says exactly which cases failed for real (test_case_success="false"):
a control that fails as designed is counted as a control; a control that does NOT fail is a real failure.

    unit_report.py unit.xml [unit_out.txt]     exit 0 = no real failure; prints one summary line, FAIL lines otherwise
"""
import sys
import xml.etree.ElementTree as ET


def main(argv):
    xml_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else None
    try:
        root = ET.parse(xml_path).getroot()
    except (ET.ParseError, OSError) as exc:
        print(f"FAIL unit: no complete doctest XML ({exc}); the binary crashed or was killed")
        if out_path:
            try:
                with open(out_path, "rb") as fh:
                    for ln in fh.read().decode("utf-8", "replace").splitlines()[-15:]:
                        print("FAIL   " + ln)
            except OSError:
                pass
        return 1
    controls = real = 0
    for case in root.iter("TestCase"):
        res = case.find("OverallResultsAsserts")
        if res is None:
            continue  # skipped (box_exclude.txt or filtered)
        if case.get("should_fail") == "true" and res.get("test_case_success") == "true":
            controls += 1
        if res.get("test_case_success") != "false":
            continue
        real += 1
        print(f"FAIL {case.get('filename')}:{case.get('line')}: {case.get('name')}")
        for expr in case.iter("Expression"):
            if expr.get("success") == "false":
                text = " ".join((expr.findtext("Original") or "?").split())
                print(f"FAIL   {expr.get('type')}( {text} ) at {expr.get('filename')}:{expr.get('line')}")
        for exc in case.iter("Exception"):
            print("FAIL   exception: " + " ".join((exc.text or "").split()))
    tot = root.find("OverallResultsTestCases")
    summary = dict(tot.attrib) if tot is not None else {}
    print(f"unit: {summary.get('successes', '?')} cases passed, {real} failed, {summary.get('skipped', '?')} skipped; "
          f"{controls} seeded negative controls failed as designed (should_fail, counted as controls, not red)")
    return 1 if real else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
