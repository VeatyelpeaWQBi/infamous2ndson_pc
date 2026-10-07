"""Retention, bounded writes, real child telemetry and synthetic fault evidence."""
from paths import ROOT,native_executable
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
from test_probe import package

class SessionTests(unittest.TestCase):
    def test_fps_uses_published_interval_and_does_not_invent_zero_on_stale_heartbeat(self):
        a={'tick_ms':1000,'presents':100}
        self.assertEqual(debug_session.heartbeat_fps({'tick_ms':2500,'presents':160},a),40)
        self.assertIsNone(debug_session.heartbeat_fps(a,a))
        self.assertIsNone(debug_session.heartbeat_fps({'tick_ms':3000,'presents':1},a))
    def test_selected_user_baseline_is_kept_within_existing_session_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp)
            for i in range(6):
                folder=base/f'session-{i}'; folder.mkdir()
                debug_session.save_json(folder/'status.json',{'collector':'infamous-debug-v1',
                    'finished':True,'retain_for_debug':i==0})
            debug_session.prune_sessions(base,keep=3)
            self.assertEqual({p.name for p in base.iterdir()},{'session-0','session-4','session-5'})

    def test_snapshot_requests_both_threads_pixels_and_render_bindings(self):
        with tempfile.TemporaryDirectory() as tmp,contextlib.redirect_stdout(io.StringIO()):
            base=Path(tmp); session=base/'session-fixture'; session.mkdir()
            debug_session.save_json(base/'active.json',{'session':str(session),'finished':False})
            with patch.object(debug_session,'collector_stopped',return_value=False):
                self.assertEqual(debug_session.control(base,'snapshot'),0)
            for name in ('request-snapshot','capture-next','render-frame-request'):
                self.assertTrue((session/name).is_file())

    def test_giant_unicode_message_is_valid_and_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'log'; writer=debug_session.RotatingWriter(path,128)
            writer.write('测'*100000); writer.close()
            self.assertLessEqual(path.stat().st_size,128)
            self.assertTrue(path.read_text(encoding='utf-8').endswith('\n'))
    def test_rotated_utf8_records_keep_newest_content_and_disk_bound(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'runtime.log'; writer=debug_session.RotatingWriter(path,128)
            for i in range(200): writer.write(f'{i}: 测试\n')
            writer.close()
            self.assertIn('199:',path.read_text(encoding='utf-8'))
            self.assertLessEqual(sum(p.stat().st_size for p in Path(tmp).iterdir()),256)
            self.assertEqual(len(list(Path(tmp).iterdir())),2)

    def test_retention_removes_only_completed_owned_sessions(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp)
            for i in range(6):
                folder=base/f'session-{i}'; folder.mkdir()
                debug_session.save_json(folder/'status.json',{'collector':'infamous-debug-v1','finished':i<5})
            unrelated=base/'session-unrelated'; unrelated.mkdir(); (unrelated/'keep.txt').write_text('keep')
            debug_session.prune_sessions(base,keep=3)
            self.assertFalse((base/'session-0').exists()); self.assertFalse((base/'session-1').exists())
            self.assertTrue((base/'session-5').exists()); self.assertTrue(unrelated.exists())

    def test_manual_control_refuses_path_outside_session_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp)/'debug'; base.mkdir()
            debug_session.save_json(base/'active.json',{'session':str(base.parent),'finished':False})
            with self.assertRaisesRegex(RuntimeError,'valid debug session'):
                debug_session.control(base,'snapshot')
            self.assertFalse((base.parent/'request-snapshot').exists())

    def test_abandoned_collector_sessions_are_retained_by_same_bounded_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp)
            for i in range(5):
                path=base/f'session-{i}'; path.mkdir()
                debug_session.save_json(path/'status.json',{'collector':'infamous-debug-v1','finished':False,'collector_pid':999999})
            with patch.object(debug_session,'collector_stopped',return_value=True):
                debug_session.prune_sessions(base,3)
            self.assertEqual(len(list(base.glob('session-*'))),3)

    def test_collector_tracks_one_requested_child_without_restarting(self):
        with tempfile.TemporaryDirectory() as tmp,contextlib.redirect_stdout(io.StringIO()):
            env=dict(os.environ,EXAMPLE_PASSWORD='must not be serialized')
            command=[sys.executable,'-c',"import time; print('child ready',flush=True); time.sleep(1.2)"]
            with patch.object(debug_session.subprocess,'Popen',wraps=debug_session.subprocess.Popen) as launch:
                status=debug_session.collect(command,ROOT,Path(tmp),{'title_id':'fixture'},env)
                self.assertEqual(launch.call_count,1)
            self.assertEqual(status,0)
            active=debug_session.read_json(Path(tmp)/'active.json'); session=Path(active['session'])
            launch_env=launch.call_args.kwargs['env']
            self.assertEqual(launch_env['BB_TOGGLE_FILE'],str(session/'optimizations-mask.txt'))
            self.assertEqual(launch_env['BB_CAPTURE_TRIGGER'],str(session/'render-frame-request'))
            self.assertTrue(active['finished']); self.assertIsNone(active['monitor_error'])
            metrics=[json.loads(line) for line in (session/'metrics.jsonl').read_text().splitlines()]
            self.assertTrue(metrics); self.assertIn('working_set_bytes',metrics[0])
            self.assertIn('cpu_seconds',metrics[0]); self.assertFalse(metrics[0]['possible_frame_stall'])
            self.assertNotIn('must not be serialized',(session/'manifest.json').read_text())
            self.assertIn('child ready',(session/'runtime.log').read_text())

    def test_launch_failure_is_preserved_and_not_retried(self):
        with tempfile.TemporaryDirectory() as tmp,contextlib.redirect_stdout(io.StringIO()):
            with patch.object(debug_session.subprocess,'Popen',side_effect=FileNotFoundError('fixture missing')) as launch:
                with self.assertRaises(FileNotFoundError):
                    debug_session.collect(['missing'],ROOT,Path(tmp),{'title_id':'fixture'})
                self.assertEqual(launch.call_count,1)
            session=next(Path(tmp).glob('session-*'))
            self.assertTrue(debug_session.read_json(session/'status.json')['finished'])

@unittest.skipUnless(native_executable('bb-probe').exists(),'build.bat --build-tests required')
class NativeDiagnosticsTests(unittest.TestCase):
    def test_f10_short_presses_record_three_marks_without_pad_reads(self):
        with tempfile.TemporaryDirectory() as tmp,contextlib.redirect_stdout(io.StringIO()):
            status=debug_session.collect([native_executable('pad-test'),'--debug-hotkey'],ROOT,Path(tmp),{'title_id':'fixture'})
            self.assertEqual(status,0)
            session=Path(debug_session.read_json(Path(tmp)/'active.json')['session'])
            marks=[line for line in (session/'input.txt').read_text().splitlines() if line.startswith('# MARK ')]
            self.assertEqual(len(marks),3)
            log=(session/'runtime.log').read_text()
            self.assertEqual(log.count('DEBUG_MARK'),3)
            self.assertEqual(log.count('F10 performance mark saved'),3)
            self.assertNotIn('F10 thread snapshot signaled',log)
            self.assertFalse((session/'capture-next').exists())
            self.assertFalse((session/'render-frame-request').exists())

    def test_explicit_deep_f10_retains_threads_and_render_capture(self):
        with tempfile.TemporaryDirectory() as tmp,contextlib.redirect_stdout(io.StringIO()):
            env=dict(os.environ,BB_F10_DEEP='1')
            status=debug_session.collect([native_executable('pad-test'),'--debug-hotkey'],ROOT,Path(tmp),{'title_id':'fixture'},env)
            self.assertEqual(status,0)
            session=Path(debug_session.read_json(Path(tmp)/'active.json')['session'])
            self.assertEqual((session/'runtime.log').read_text().count('F10 thread snapshot signaled'),3)
            self.assertTrue((session/'capture-next').exists())
            self.assertTrue((session/'render-frame-request').exists())

    def test_synthetic_guest_fault_keeps_exit_registers_and_all_thread_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp,contextlib.redirect_stdout(io.StringIO()):
            base=Path(tmp); image=base/'boot.bin'; image.write_bytes(package(bytes.fromhex('0f0b')))
            status=debug_session.collect([native_executable('bb-probe'),image,'--cpu-only'],ROOT,base/'debug',{'title_id':'fixture'})
            self.assertEqual(status,139)
            session=Path(debug_session.read_json(base/'debug/active.json')['session'])
            log=(session/'runtime.log').read_text(encoding='utf-8')
            self.assertIn('Guest fault 0xc000001d at guest+0x0',log)
            self.assertIn('rax=',log); self.assertIn('Thread dump:',log); self.assertIn('Thread dump end',log)
            state=debug_session.read_json(session/'status.json')
            self.assertTrue(state['abnormal_exit']); self.assertTrue(state['faults'])

    def test_virtual_pad_test_is_recorded_automatically_without_f9(self):
        with tempfile.TemporaryDirectory() as tmp,contextlib.redirect_stdout(io.StringIO()):
            status=debug_session.collect([native_executable('pad-test')],ROOT,Path(tmp),{'title_id':'fixture'})
            self.assertEqual(status,0)
            session=Path(debug_session.read_json(Path(tmp)/'active.json')['session'])
            lines=(session/'input.txt').read_text().splitlines()
            self.assertTrue(lines)
            samples=[list(map(int,line.split())) for line in lines if not line.startswith('#')]
            self.assertTrue(all(len(sample)==10 for sample in samples))
            self.assertTrue(any(sample[2] for sample in samples))
