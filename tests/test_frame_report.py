from paths import ROOT
import tempfile
from pathlib import Path
import unittest
from frame_report import summarize


class FrameReportTests(unittest.TestCase):
    def test_rotated_rows_marks_percentiles_and_missing_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)
            header='sequence,tick_ms,interval_ns,dropped_frames,bind_ns\n'
            (path/'frames.csv.1').write_text(header+'1,1000,16000000,0,2000000\n2,1016,16000000,0,3000000\n')
            (path/'frames.csv').write_text(header+'2,1016,16000000,0,3000000\n4,1066,50000000,1,4000000\n5,1167,101000000,1,5000000\n6,unfinished')
            (path/'input.txt').write_text('# MARK 1040 F10\n')
            report=summarize(path)
            self.assertEqual(report['overall']['frames'],4)
            self.assertEqual(report['overall']['p50_ms'],16)
            self.assertEqual(report['overall']['p99_ms'],101)
            self.assertEqual(report['overall']['over_50ms'],1)
            self.assertEqual(report['overall']['mean_timers_ms_per_flip']['bind_ns'],3.5)
            self.assertEqual(report['marks'][0]['before']['frames'],2)
            self.assertEqual(report['marks'][0]['after']['frames'],2)
            self.assertEqual(report['sequence_gaps'],1)
            self.assertEqual(report['dropped_frames'],1)
            self.assertEqual(report['malformed_rows'],1)
            self.assertFalse(report['gpu_execution_time_available'])

    def test_absent_telemetry_is_unavailable_not_good_performance(self):
        with tempfile.TemporaryDirectory() as tmp:
            report=summarize(tmp)
            self.assertEqual(report['overall'],{'frames':0})
            self.assertIsNone(report['first_tick_ms'])
