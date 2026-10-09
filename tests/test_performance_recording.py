from paths import ROOT
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import debug_session
from recover_performance_recording import RecordingJournal, read_frames, recover, analyze, merge_native_journal

HEADER = 'sequence,tick_ms,interval_ns,dropped_frames,draws,compile_count,bind_ns\n'


class PerformanceRecordingTests(unittest.TestCase):
    def test_wrapped_recording_join_preserves_same_tick_slow_and_catchup_frames(self):
        journal=[{'sequence':i+100,'tick_ms':1000+i//2,'interval_ns':223000000 if i==3 else i*100,
                  'draws':i,'dropped_frames':0} for i in range(1,7)]
        native=[{k:(r[k]-100 if k=='sequence' else r[k]) for k in r if k!='dropped_frames'}
                for r in journal[2:]]
        native.append({'sequence':7,'tick_ms':1004,'interval_ns':700,'draws':7})
        result=merge_native_journal(native,journal)
        self.assertEqual([r['sequence'] for r in result],list(range(101,108)))
        self.assertEqual(result[2]['interval_ns'],223000000)
        self.assertEqual(len(result),7)
        conflicting=[dict(r) for r in native]; conflicting[0]['draws']=999
        with self.assertRaises(ValueError): merge_native_journal(conflicting,journal)
        self.assertEqual(journal[0]['sequence'],101)

    def test_binding_detail_survives_recording_and_legacy_data_stays_readable(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'recording.csv'
            path.write_text('sequence,tick_ms,interval_ns,texture_bind_ns,buffer_bind_ns,sampler_bind_ns,texture_set_hits,texture_set_misses,texture_set_revalidated\n'
                            '1,1016,16000000,4000000,3000000,1000000,9,1,4\n'
                            '2,1032,16000000,6000000,5000000,2000000,7,3,2\n')
            result=analyze(path)['overall']
            self.assertEqual(result['mean_timers_ms']['texture_bind_ns'],5)
            self.assertEqual(result['texture_sets'],{'hits':16,'misses':4,
                'reused_after_unrelated_registry_changes':6,'hit_rate':.8})
            path.write_text(HEADER+'1,1016,16000000,0,10,0,100\n')
            self.assertNotIn('texture_sets',analyze(path)['overall'])
    def session(self, path):
        (path/'status.json').write_text(json.dumps({'collector':'infamous-debug-v1','finished':True}))
        (path/'runtime.log').write_text('PERF_RECORDING START id=1 tick_ms=1000\n')

    def test_rotated_overlap_partial_tail_is_recovered_once_and_sources_are_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp); self.session(path)
            (path/'frames.csv.1').write_text(HEADER+'1,999,16000000,0,10,0,100\n2,1010,16000000,0,20,0,200\n3,1026,16000000,0,30,0,300\n')
            (path/'frames.csv').write_text(HEADER+'3,1026,16000000,0,30,0,300\n4,1042,16000000,0,40,0,400\n5,unfinished')
            results=recover(path); self.assertEqual(results[0]['frames'],2)
            self.assertEqual(results[0]['malformed_source_rows'],1)
            self.assertTrue(results[0]['interrupted']); self.assertIsNone(results[0]['tail_missing_frames'])
            _, rows, invalid=read_frames(path/'performance-recording-1.csv')
            self.assertEqual([r['draws'] for r in rows],[30,40]); self.assertEqual(invalid,0)
            self.assertEqual(recover(path),[])
            self.assertTrue((path/'frames.csv.1').exists()); self.assertTrue((path/'frames.csv').exists())
            self.assertFalse(list(path.glob('*.recovery-next')))

    def test_journal_preserves_frames_lost_to_rotation_and_is_deleted_after_verified_merge(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp); self.session(path); journal=RecordingJournal(path)
            journal.observe('PERF_RECORDING START id=1 tick_ms=1000')
            (path/'frames.csv').write_text(HEADER+'1,1016,16000000,0,10,0,100\n2,1032,16000000,0,20,0,200\n')
            journal.checkpoint(); journal.checkpoint()
            cache=path/'performance-recording-1.cache.csv'
            self.assertEqual(len(read_frames(cache)[1]),2)
            (path/'frames.csv').write_text(HEADER+'3,1048,16000000,0,30,0,300\n')
            journal.checkpoint()
            result=recover(path)[0]
            self.assertEqual(result['frames'],3); self.assertEqual(result['sequence_gaps'],0)
            self.assertEqual(result['temporary_recording_caches_deleted'],[cache.name]); self.assertFalse(cache.exists())

    def test_conflicting_data_and_live_session_refuse_recovery_without_deleting_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp); self.session(path)
            (path/'frames.csv').write_text(HEADER+'1,1016,16000000,0,10,0,100\n')
            (path/'performance-recording-1.cache.csv').write_text(HEADER+'1,1016,16000000,0,999,0,100\n')
            with self.assertRaisesRegex(ValueError,'Conflicting'): recover(path)
            self.assertTrue((path/'performance-recording-1.cache.csv').exists())
            (path/'status.json').write_text(json.dumps({'collector':'infamous-debug-v1','finished':False}))
            with self.assertRaisesRegex(ValueError,'finished'): recover(path)

    def test_journal_disk_limit_does_not_stop_game_and_native_file_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp); self.session(path); journal=RecordingJournal(path); journal.Limit=1
            journal.observe('PERF_RECORDING START id=1 tick_ms=1000')
            (path/'frames.csv').write_text(HEADER+'1,1016,16000000,0,10,0,100\n')
            journal.checkpoint(); self.assertFalse((path/'performance-recording-1.cache.csv').exists())
            target=path/'performance-recording-1.csv'; target.write_text(HEADER+'1,1016,16000000,0,10,0,100\n')
            before=target.read_bytes(); self.assertEqual(recover(path),[]); self.assertEqual(target.read_bytes(),before)
            self.assertEqual(analyze(target)['overall']['frames'],1)

    def test_native_stop_cleans_only_matching_cache_and_retains_uncovered_samples(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp); self.session(path)
            target=path/'performance-recording-1.csv'
            target.write_text('sequence,tick_ms,interval_ns,draws,compile_count,bind_ns\n1,1016,16000000,10,0,100\n')
            cache=path/'performance-recording-1.cache.csv'
            cache.write_text(HEADER+'99,1016,16000000,0,10,0,100\n')
            recover(path); self.assertFalse(cache.exists())
            cache.write_text(HEADER+'99,1016,16000000,0,10,0,100\n100,1032,16000000,0,20,0,200\n')
            recover(path); self.assertTrue(cache.exists())

    def test_missing_source_ranges_are_reported_not_filled_with_fake_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp); self.session(path)
            (path/'frames.csv').write_text(HEADER+'50,2016,16000000,7,10,0,100\n52,2048,16000000,7,20,0,200\n')
            result=recover(path)[0]
            self.assertEqual(result['start_gap_ms'],1000)
            self.assertEqual(result['source_dropped_frames'],7)
            self.assertEqual(result['sequence_gaps'],1); self.assertEqual(result['frames'],2)

    def test_real_child_abrupt_exit_automatically_finishes_f11_without_relaunch(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            script = ("import os,time; from pathlib import Path; p=Path(os.environ['BB_DEBUG_DIR']); "
                      "print('PERF_RECORDING START id=1 tick_ms=1000',flush=True); "
                      f"(p/'frames.csv').write_text({HEADER!r}+'1,1016,16000000,0,10,0,100\\n2,1032,16000000,0,20,0,200\\n'); "
                      "time.sleep(1.2); os._exit(20)")
            with patch.object(debug_session.subprocess,'Popen',wraps=debug_session.subprocess.Popen) as launch:
                self.assertEqual(debug_session.collect([sys.executable,'-c',script],ROOT,Path(tmp),{'title_id':'fixture'}),20)
                self.assertEqual(launch.call_count,1)
            state=debug_session.read_json(Path(tmp)/'active.json'); path=Path(state['session'])
            self.assertEqual(state['recovered_recordings'][0]['frames'],2)
            self.assertTrue((path/'performance-recording-1.summary.json').exists())
            self.assertFalse((path/'performance-recording-1.cache.csv').exists())
