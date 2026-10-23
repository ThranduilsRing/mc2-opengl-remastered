"""Counterexamples for annuli, MOVE rays, links and declared counterplay."""
import unittest

import numpy as np
from tools.mission_codec import move_fields

from tools.mapgen.fields_of_fire import (C, MoveLOS, active_ids, corridor_samples,
                                         coverage, evaluate, reachable)


def scene():
    return MoveLOS(np.zeros((20, 20)), np.full((60, 60), 0x402, dtype='<u4'))


def turret(ident='gun', x=0, lo=0, hi=300, indirect=False):
    return {'id': ident, 'class': 'turret', 'cell': [30, int(30+x/C)], 'team': 1,
            'parent_link': None, 'parent_cell': None, 'position_wu': [x, 0, 0],
            'fire': {'weapons': [{'min_wu': lo, 'max_wu': hi, 'indirect': indirect}],
                     'muzzle_wu': 20, 'acquisition_wu': 500, 'yaw_deg_s': 20}}


class FieldsOfFireTests(unittest.TestCase):
    def test_extracted_stock_source_is_read_only(self):
        import tempfile
        from pathlib import Path
        from tools.mapgen.fields_of_fire import audit_source
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'objects').mkdir()
            (root/'objects/object2.pak').write_bytes(b'fixture')
            before = sorted(p.relative_to(root).as_posix() for p in root.rglob('*'))
            self.assertEqual(audit_source(root).read('data/objects/object2.pak')[0], b'fixture')
            self.assertEqual(before, sorted(p.relative_to(root).as_posix() for p in root.rglob('*')))

    def test_empty_batches_and_visual_range(self):
        self.assertEqual(coverage([], [[0, 0]], scene(), None)[0].shape, (0, 1))
        self.assertEqual(coverage([turret()], [], scene(), None)[0].shape, (1, 0))
        gun = turret()
        gun['fire']['visual_range_wu'] = 100
        self.assertFalse(coverage([gun], [[101, 0]], scene(), None)[0].any())
        gun['fire'].pop('visual_range_wu')
        gun['fire']['visual_ranges_wu'] = [100]*256
        self.assertFalse(coverage([gun], [[101, 0]], scene(), None)[0].any())
        self.assertTrue(coverage([gun], [[100, 0]], scene(), None)[0].any())

    def test_mandatory_corridor_cannot_drop_blocked_width_samples(self):
        s = scene()
        guns = [turret('a', -1.6*C), turret('b', 1.6*C)]
        plan = {'id': 'battery', 'turrets': ['a', 'b'], 'difficulty': 'easy',
                'entrance': [0, 0], 'inward': [0, -1], 'sector_half_deg': 10,
                'approaches': [{'id': 'front', 'points': [[0, 4*C], [0, -C]], 'width_wu': C}]}
        s.cells[28, 30] = 2
        s.fields = move_fields(s.cells)
        result = evaluate({'objects': guns, 'batteries': [plan]}, s)
        self.assertTrue(any('full-width' in error for error in result['errors']))

    def test_disconnected_support_and_unassigned_turret_fail(self):
        guns = [turret('a', -400), turret('b', 400)]
        plan = {'id': 'battery', 'turrets': ['a', 'b'], 'difficulty': 'easy',
                'entrance': [0, 0], 'inward': [0, -1], 'sector_half_deg': 10,
                'approaches': [{'id': 'front', 'points': [[0, 200], [0, 0]]}]}
        layout = {'objects': guns, 'batteries': [plan]}
        result = evaluate(layout, scene())
        self.assertTrue(any('disconnected mutual-support' in e for e in result['errors']))
        self.assertEqual(result['batteries'][0]['support_components'], [['a'], ['b']])
        layout['objects'].append(turret('unassigned', 900))
        self.assertTrue(any('unassigned' in e for e in evaluate(layout, scene())['incomplete']))

    def test_observer_loss_recomputes_conditional_coverage(self):
        s = scene()
        gun = turret(indirect=True)
        s.cells[29:32, 32] |= 4 << 18
        s.fields = move_fields(s.cells)
        observer = {'id': 'spotter', 'class': 'mech', 'position_wu': [200, 100, 50],
                    'team': 1, 'visual_range_wu': 500, 'parent_link': None}
        plan = {'id': 'battery', 'turrets': ['gun'], 'difficulty': 'easy',
                'entrance': [0, 0], 'inward': [1, 0], 'sector_half_deg': 10,
                'observers': [observer],
                'approaches': [{'id': 'front', 'points': [[190, 0], [210, 0]], 'width_wu': 1}]}
        result = evaluate({'objects': [gun], 'batteries': [plan]}, s)
        battery = result['batteries'][0]
        self.assertGreater(battery['approaches']['front']['vehicle']['conditional_indirect_coverage'], 0)
        lost = battery['states']['observer_lost']
        self.assertEqual(lost['lost_observers'], ['spotter'])
        self.assertEqual(lost['approaches']['front']['conditional_indirect_coverage'], 0)

    def test_hash_includes_weapon_data_and_forest_policy(self):
        layout = {'objects': [turret()], 'batteries': []}
        s = scene()
        first = evaluate(layout, s)['inputs_sha256']
        layout['objects'][0]['fire']['weapons'][0]['max_wu'] += 1
        second = evaluate(layout, s)['inputs_sha256']
        self.assertNotEqual(first, second)
        s.tree_limit += 1
        self.assertNotEqual(second, evaluate(layout, s)['inputs_sha256'])

    def test_difficulty_and_flaw_defaults(self):
        from tools.mapgen.batteries import plan
        from tools.mapgen.generate import plan_cells
        from tools.mapgen.fields_of_fire import DIFFICULTIES
        import copy
        cells = plan_cells(180, 7)
        for difficulty in DIFFICULTIES:
            layout = plan(copy.deepcopy(cells), difficulty)
            by_id = {o['id']: o for o in layout['objects']}
            lo, hi, skill, *_ = DIFFICULTIES[difficulty]
            self.assertTrue(layout['batteries'])
            for battery in layout['batteries']:
                self.assertLessEqual(lo, len(battery['turrets']))
                self.assertLessEqual(len(battery['turrets']), hi)
                self.assertEqual(bool(battery['flaw']), difficulty != 'hard')
                self.assertTrue(all(by_id[i]['pilot_skill'] == skill for i in battery['turrets']))
            for enabled in (True, False):
                layout = plan(copy.deepcopy(cells), difficulty, enabled)
                self.assertTrue(all(bool(b['flaw']) == enabled for b in layout['batteries']))

    def test_generated_difficulties_use_connected_firing_lines(self):
        from tools.mapgen.generate import generate
        from tools.mapgen.fields_of_fire import DIFFICULTIES
        for difficulty in DIFFICULTIES:
            with self.subTest(difficulty=difficulty):
                layout = generate(180, 7, difficulty=difficulty)
                report = layout['fields_of_fire']
                self.assertIn(report['status'], ('PASS', 'WARN'))
                self.assertTrue(all(len(b['support_components']) == 1 for b in report['batteries']))
                self.assertTrue(all(b['flaw_reachable'] is True for b in report['batteries']) if difficulty != 'hard'
                                else all(b['flaw_reachable'] is None for b in report['batteries']))

    def test_generated_flaw_variants_are_real_reachable_opportunities(self):
        from tools.mapgen.generate import generate
        from tools.mapgen.fields_of_fire import FLAWS
        for flaw in FLAWS:
            with self.subTest(flaw=flaw):
                layout = generate(180, 7, flaw=True, flaw_type=flaw)
                self.assertIn(layout['fields_of_fire']['status'], ('PASS', 'WARN'))
                for battery in layout['fields_of_fire']['batteries']:
                    self.assertTrue(battery['flaw_reachable'])
                    if flaw in ('flank_route', 'lrm_pocket'):
                        self.assertEqual(battery['flaw_target_firing_turrets'], 0)
                    elif flaw == 'exposed_generator':
                        self.assertTrue(battery['flaw_target_visible'])

    def test_range_open_inner_closed_outer_and_acquisition(self):
        gun = turret(lo=150, hi=600)
        points = [[150, 0], [150.01, 0], [500, 0], [500.01, 0]]
        d, i = coverage([gun], points, scene(), None)
        self.assertEqual(d.tolist(), [[False, True, True, False]])
        self.assertFalse(i.any())

    def test_weapons_are_separate_annuli(self):
        gun = turret(lo=300, hi=900)
        points = [[100, 0], [300, 0], [301, 0]]
        self.assertEqual(coverage([gun], points, scene(), None)[0].tolist(), [[False, False, True]])
        gun['fire']['weapons'].append({'min_wu': 0, 'max_wu': 300, 'indirect': False})
        self.assertTrue(coverage([gun], points, scene(), None)[0].all())

    def test_forest_counts_samples_and_local_height_is_not_passability(self):
        s = scene()
        start, end = s.world([30, 10]), s.world([30, 22])
        source = [*start, 20]
        s.cells[30, 14] |= (4 << 18) | 0x10000000
        s.fields = move_fields(s.cells)
        self.assertTrue(s.rays(source, [end])[0][0])
        s.cells[30, 14:19] |= (4 << 18) | 0x10000000
        s.fields = move_fields(s.cells)
        visible, reason = s.rays(source, [end])
        self.assertFalse(visible[0]); self.assertEqual(reason[0], 'forest')
        s.cells[:] = 0x402  # Impassable alone is not a LOS wall.
        s.cells[30, 14] = 2
        s.fields = move_fields(s.cells)
        self.assertTrue(s.rays(source, [end])[0][0])
        s.cells[30, 14] |= 4 << 18
        s.fields = move_fields(s.cells)
        self.assertFalse(s.rays(source, [end])[0][0])

    def test_extent_skips_source_blocker_and_batch_sources_match(self):
        s = scene(); start, end = s.world([30, 10]), s.world([30, 22])
        s.cells[30, 10] |= 4 << 18
        s.fields = move_fields(s.cells)
        self.assertFalse(s.rays([*start, 20], [end])[0][0])
        self.assertTrue(s.rays([*start, 20], [end], start_extent=60)[0][0])
        sources = np.array([[*start, 20], [*start, 100]])
        self.assertEqual(s.rays(sources, [end, end])[0].tolist(), [False, True])

    def test_alternating_terrain_triangles(self):
        s = scene()
        s.vertices[10, 10], s.vertices[10, 11] = 0, 100
        s.vertices[11, 10], s.vertices[11, 11] = 50, 200
        self.assertAlmostEqual(float(s.elevation([96, -32])), 100.)
        self.assertAlmostEqual(float(s.elevation([32, -96])), 75.)

    def test_indirect_requires_live_team_observer(self):
        s = scene(); gun = turret(indirect=True)
        target = [200, 0]
        s.cells[29:32, 32] |= 4 << 18
        s.fields = move_fields(s.cells)
        observer = {'id': 'spotter', 'position_wu': [200, 100, 50], 'team': 1, 'visual_range_wu': 500}
        d, i = coverage([gun], [target], s, None)
        self.assertFalse(d.any() or i.any())
        self.assertTrue(coverage([gun], [target], s, None, active={'gun', 'spotter'}, observers=[observer])[1].any())
        self.assertFalse(coverage([gun], [target], s, None, active={'gun'}, observers=[observer])[1].any())

    def test_power_chain_capture_and_independent_turret(self):
        gun = turret(); gun['parent_link'] = 'control'
        control = {'id': 'control', 'team': 1, 'parent_link': 'generator'}
        generator = {'id': 'generator', 'team': 1, 'parent_link': None}
        self.assertIn('gun', active_ids([gun, control, generator])[0])
        self.assertNotIn('gun', active_ids([gun, control, generator], ['generator'])[0])
        control['team'] = 0
        self.assertTrue(active_ids([gun, control, generator])[1])
        self.assertIn('gun', active_ids([turret()])[0])
        control['team'] = 1; generator['parent_link'] = 'control'
        self.assertTrue(any('cycle' in e for e in active_ids([gun, control, generator])[1]))

    def test_corridor_width_and_reachable_footprint(self):
        points, weights, stations = corridor_samples({'points': [[0, 0], [4*C, 0]], 'width_wu': 2*C})
        self.assertAlmostEqual(weights.sum(), 4*C)
        np.testing.assert_allclose(sorted(set(points[:, 1])), [-C, -C/2, 0, C/2, C])
        s = scene(); s.cells[:] = 2; s.cells[30, 5:20] = 0x402
        s.fields = move_fields(s.cells)
        self.assertTrue(reachable(s, (30, 5), (30, 19), C))
        self.assertFalse(reachable(s, (30, 5), (30, 19), 2*C))

    def test_flaw_optional_and_never_excuses_frontal_hole(self):
        guns = [turret('a', -1.6*C), turret('b', 1.6*C)]
        plan = {'id': 'battery', 'turrets': ['a', 'b'], 'difficulty': 'easy',
                'entrance': [0, 0], 'inward': [0, -1], 'sector_half_deg': 10,
                'approaches': [{'id': 'front', 'points': [[0, 4*C], [0, -C]], 'width_wu': C}], 'flaw': None}
        layout = {'objects': guns, 'batteries': [plan]}
        self.assertIn(evaluate(layout, scene(), None)['status'], ('PASS', 'WARN'))
        plan['flaw'] = {'type': 'flank_route', 'start': [0, 4*C], 'target': [0, -C]}
        for gun in guns:
            gun['fire']['weapons'][0]['min_wu'] = 300
        self.assertTrue(any('frontal' in e for e in evaluate(layout, scene(), None)['errors']))
        s = scene(); s.cells[30, 30] &= np.uint32(~0x400 & 0xffffffff)
        s.fields = move_fields(s.cells); plan['flaw']['target'] = s.world([30, 30]).tolist()
        self.assertTrue(any('flaw unreachable' in e for e in evaluate(layout, s, None)['errors']))

    def test_no_green_empty_denominator(self):
        self.assertEqual(evaluate({'objects': [turret()], 'batteries': []}, scene(), None)['status'], 'INCOMPLETE')


if __name__ == '__main__':
    unittest.main()
