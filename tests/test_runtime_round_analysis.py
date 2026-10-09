"""Prevent missing or duplicate F11 data from producing misleading comparisons."""
from paths import ROOT
from pathlib import Path
import tempfile
import unittest

from analyze_runtime_round import summarize


class RuntimeRoundAnalysisTests(unittest.TestCase):
    def report(self):
        return {'executable_sha256': 'exe', 'game_sha256': 'game', 'save_seed': {},
                'native_manifest': {'settings': {}},
                'phases': [{'name': 'static', 'valid': True, 'start_tick_ms': 100,
                            'stop_tick_ms': 200, 'fps': 50, 'p95_ms': 22}]}

    def test_finalized_data_excludes_native_journal_and_outside_phase(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            (session / 'performance-recording-1.csv').write_text(
                '# F11 recording\nsequence,tick_ms,command_cpu_ns\n'
                '1,99,999\n2,110,10\n3,190,30\n4,201,999\n', encoding='utf-8')
            (session / 'performance-recording-1.cache.csv').write_text(
                'sequence,tick_ms,command_cpu_ns\n2,110,999\n', encoding='utf-8')
            phase = summarize(self.report(), session)['phases'][0]
            self.assertEqual(phase['frames'], 2)
            self.assertEqual(phase['means_per_frame']['command_cpu_ns'], 20)

    def test_missing_or_invalid_measurement_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'No finalized F11'):
                summarize(self.report(), Path(directory))
            report = self.report()
            report['phases'][0]['valid'] = False
            with self.assertRaisesRegex(ValueError, 'Invalid frame measurement'):
                summarize(report, Path(directory))
