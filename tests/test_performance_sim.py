"""Performance harness: corrupt inputs cannot pass; noise and scope remain explicit."""
from paths import ROOT
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import performance_sim as sim


def report(cpu_only=False):
    def measurement(times):
        return {'round_ms': times, 'median_ms': sorted(times)[1], 'mad_ratio': 0,
                'checksum': 7, 'hits': 3, 'subresource_checks': 4}
    return {'schema': 1, 'kind': 'synthetic_component_benchmark', 'cpu_only': cpu_only,
            'operations': 200000, 'rounds': 3, 'seed': 777, 'registry_checks': 3,
            'gpu': 'not_used' if cpu_only else 'test_gpu', 'host': {'cpu': 'test_cpu'}, 'perf_stats': True,
            'cases': [{'name': name, 'scope': 'test', 'equivalent': True,
                       'baseline': measurement([10., 10., 10.]),
                       'candidate': measurement([5., 5., 5.])}
                      for name in (sim.CASE_NAMES[:2] if cpu_only else sim.CASE_NAMES)]}


class CalibrationTests(unittest.TestCase):
    def write(self, directory, rows):
        path = Path(directory) / 'recording.csv'
        path.write_text('# recording_id=1\nsequence,tick_ms,interval_ns,texture_set_hits,texture_set_misses\n'
                        + '\n'.join(rows) + '\n', encoding='utf-8')
        return path

    def test_calibration_uses_activity_and_keeps_source_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(directory, ['1,1000,10000000,1,0', '2,70000,20000000,8000,20'])
            original = path.read_bytes()
            value = sim.calibration(path)
            self.assertEqual(value['frames'], 2)
            self.assertEqual(value['activity_frames'], 1)
            self.assertEqual(value['activity_fps'], 50)
            self.assertEqual(value['recommended_operations'], 500000)
            self.assertIn('assumed', value['interpretation'])
            self.assertEqual(path.read_bytes(), original)

    def test_invalid_recordings_rejected(self):
        bad = [['1,1,100,0,0', '1,2,100,0,0'], ['1,10,100,0,0', '2,9,100,0,0'],
               ['1,1,-1,0,0'], ['1,1,0,0,0'], ['1,1,100,-1,0'], ['1,1,100,0,18446744073709551616']]
        with tempfile.TemporaryDirectory() as directory:
            for rows in bad:
                with self.subTest(rows=rows), self.assertRaises(ValueError):
                    sim.calibration(self.write(directory, rows))

    def test_latest_recording_excludes_recovery_cache_and_outside_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / 'out/CUSA00309/debug'
            session = base / 'session-example'
            session.mkdir(parents=True)
            (session / 'performance-recording-1.csv').touch()
            (session / 'performance-recording-2.cache.csv').touch()
            active = base / 'active.json'
            active.write_text(json.dumps({'session': str(session)}))
            self.assertEqual(sim.latest_recording(root).name, 'performance-recording-1.csv')
            active.write_text(json.dumps({'session': str(root)}))
            with self.assertRaisesRegex(ValueError, 'outside'):
                sim.latest_recording(root)


class BenchmarkReportTests(unittest.TestCase):
    def validate(self, data, cpu_only=False):
        return sim.validate_native(data, cpu_only, 200000, 3, 777)

    def test_complete_report_and_explicit_cpu_subset(self):
        self.validate(report())
        self.validate(report(True), True)
        with self.assertRaises(ValueError):
            self.validate(report(True))

    def test_corrupt_native_results_never_pass(self):
        for mutation in ('checksum', 'nan', 'missing', 'wrong_seed', 'short', 'false_equivalence', 'wrong_median'):
            data = report()
            if mutation == 'checksum': data['cases'][0]['candidate']['checksum'] += 1
            if mutation == 'nan': data['cases'][0]['candidate']['round_ms'][0] = float('nan')
            if mutation == 'missing': data['cases'].pop()
            if mutation == 'wrong_seed': data['seed'] += 1
            if mutation == 'short': data['cases'][0]['candidate']['round_ms'].pop()
            if mutation == 'false_equivalence': data['cases'][0]['equivalent'] = False
            if mutation == 'wrong_median': data['cases'][0]['candidate']['median_ms'] = 2
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.validate(data)

    def test_stable_gain_regression_and_noisy_result_separated(self):
        data = report()
        data['cases'][1]['candidate'].update(round_ms=[12., 12., 12.], median_ms=12.)
        data['cases'][2]['candidate']['mad_ratio'] = .4
        value = sim.assess(data)
        self.assertEqual(value['cases'][0]['performance'], 'component_gain')
        self.assertEqual(value['cases'][1]['performance'], 'possible_regression')
        self.assertEqual(value['cases'][2]['performance'], 'inconclusive')
        self.assertFalse(value['game_fps_verified'])

    def test_comparison_rejects_changed_host_and_workload(self):
        previous = sim.assess(report())
        current = copy.deepcopy(previous)
        self.assertEqual(sim.compare(previous, current)[0]['status'], 'no_clear_change')
        for field in ('operations', 'seed', 'gpu', 'host', 'perf_stats'):
            current = copy.deepcopy(previous)
            current[field] = 'different'
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, field):
                sim.compare(previous, current)

    def test_semantic_change_rejected_and_model_gain_not_a_production_gate(self):
        previous = sim.assess(report())
        current = copy.deepcopy(previous)
        current['cases'][0]['candidate']['checksum'] += 1
        with self.assertRaisesRegex(ValueError, 'semantics'):
            sim.compare(previous, current)
        self.assertFalse(sim.assess(report(True))['stage_gain_confirmed'])
        production = report()
        for case in production['cases'][2:]:
            case['scope'] = 'production_Image_GetBarriers_cpu_time'
        self.assertTrue(sim.assess(production)['stage_gain_confirmed'])
        production['cases'][3]['candidate'].update(round_ms=[12., 12., 12.], median_ms=12.)
        self.assertFalse(sim.assess(production)['stage_gain_confirmed'])

    def test_two_report_retention_and_scope_caveats(self):
        with tempfile.TemporaryDirectory() as directory:
            path = sim.write_report(Path(directory), sim.assess(report(True)))
            first = path.read_bytes()
            sim.write_report(Path(directory), sim.assess(report(True)))
            self.assertEqual((Path(directory) / 'previous.json').read_bytes(), first)
            text = (Path(directory) / 'latest.md').read_text(encoding='utf-8')
            self.assertIn('未执行', text)
            self.assertIn('不代表游戏 FPS', text)
            self.assertEqual(len(list(Path(directory).glob('*.json'))), 2)

    def test_running_game_blocks_benchmark_without_termination(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            active = root / 'out/CUSA00309/debug/active.json'
            active.parent.mkdir(parents=True)
            active.write_text(json.dumps({'pid': 123, 'finished': False}))
            with patch('debug_session.ProcessMetrics') as process:
                process.return_value.read.return_value = {'ended_filetime': 0}
                with self.assertRaisesRegex(RuntimeError, 'Close the current game'):
                    sim.check_game_stopped(root)
                process.return_value.close.assert_called_once()

    def test_access_denied_not_treated_as_absent_game(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            active = root / 'out/CUSA00309/debug/active.json'
            active.parent.mkdir(parents=True)
            active.write_text(json.dumps({'pid': 123, 'finished': False}))
            error = PermissionError('denied'); error.winerror = 5
            with patch('debug_session.ProcessMetrics', side_effect=error), \
                    self.assertRaisesRegex(RuntimeError, 'Cannot determine'):
                sim.check_game_stopped(root)


if __name__ == '__main__':
    unittest.main()
