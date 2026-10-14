"""Independent file coverage must reject omitted and corrupted variant evidence."""
import copy
from pathlib import Path
import sys
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from editor.tests.run_catalog_proofs import compare_resolution, expected_files


class CatalogResolutionTests(unittest.TestCase):
    def test_variant_coverage_is_independent_of_probe_and_rejects_corruption(self):
        root = Path(__file__).resolve().parents[2] / "scratch/e5a-fix-resolution-tests" / uuid.uuid4().hex
        root.mkdir(parents=True)
        inputs = {
            "data/art/buildings.csv": b"header\nmech,1,30400,MECH,0,,1,0,1,icon,,1\noptional,1,30400,MECH,1,,1,0,1,icon,,1\n",
            "data/objects/object2.pak": b"definitions",
            "data/objects/mech.csv": b"Variant,Stock\n",
        }
        def independent(key):
            return inputs[key], "base-loose", root / "source" / key
        expected = expected_files(independent)
        self.assertEqual(set(expected), set(inputs))
        report = {"files": {key: {"layer": layer, "source": str(path)} for key, (_, layer, path) in expected.items()}}
        for key, contents in inputs.items():
            path = root / "output" / key
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        output = root / "output"
        self.assertEqual(len(compare_resolution(output, report, expected)), 3)
        omitted = copy.deepcopy(report)
        del omitted["files"]["data/objects/mech.csv"]
        with self.assertRaisesRegex(ValueError, "coverage disagreement"):
            compare_resolution(output, omitted, expected)
        variant = output / "data/objects/mech.csv"
        variant.unlink()
        with self.assertRaisesRegex(ValueError, "coverage disagreement"):
            compare_resolution(output, omitted, expected)
        with self.assertRaises(FileNotFoundError):
            compare_resolution(output, report, expected)
        variant.write_bytes(b"Variant,Corrupted\n")
        with self.assertRaisesRegex(ValueError, "oracle disagreement"):
            compare_resolution(output, report, expected)


if __name__ == "__main__":
    unittest.main()
