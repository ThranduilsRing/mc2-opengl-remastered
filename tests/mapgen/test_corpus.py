import hashlib
import json
import unittest
from pathlib import Path

from tools.mapgen.analyze import archetype, clusters
from tools.mapgen.common import load_json, output_path

ROOT = Path(__file__).resolve().parents[2]


class CorpusTests(unittest.TestCase):
    def test_profile_matches_committed_provenance(self):
        profile = load_json(ROOT / "tools/mapgen/profile.json")
        manifest = ROOT / "docs/mapgen/extraction_manifest.json"
        canonical = json.dumps(load_json(manifest), sort_keys=True, separators=(",", ":")).encode()
        self.assertEqual(profile["index_sha256"], hashlib.sha256(canonical).hexdigest())
        self.assertEqual(profile["groups"]["stock"]["missions"],24)
        self.assertEqual(len(profile["groups"]),7)
        self.assertEqual(load_json(manifest)["errors"],[])
        for stats in profile["groups"]["stock"]["archetypes"].values():
            self.assertGreater(stats["count"],0)

    def test_clustering_respects_teams_and_ignores_foliage(self):
        objs = [{"id": str(i), "cell": [20+i,20], "team": i%2,
                 "class": "turret", "type_name": "turret"} for i in range(4)]
        objs.append({"id": "tree", "cell": [20,20], "team": 0, "class": "tree", "type_name": "tree"})
        groups = clusters(objs)
        self.assertEqual(sorted(len(group) for group in groups),[2,2])
        self.assertEqual({archetype(group,[]) for group in groups},{"turret_battery"})

    def test_install_output_is_refused(self):
        with self.assertRaisesRegex(ValueError,"read-only install"):
            output_path("A:/Games/mc2-opengl/mc2-win64-v0.4/data/mapgen.json")
