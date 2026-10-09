"""Actual runtime harness: presentation data, isolation, bounds and consent failures."""
from paths import ROOT
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import runtime_benchmark as bench


class RuntimeBenchmarkTests(unittest.TestCase):
    def test_post_measurement_image_rejects_stale_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);session=root/'session';session.mkdir()
            (session/'frame-snapshot-0.json').write_text(json.dumps({'tick_ms':99}))
            (session/'frame-snapshot-0.bmp').write_bytes(b'old image')
            with patch.object(bench,'BASE',root):
                self.assertFalse(bench.preserve_phase_snapshot(session,100,200,root,'static')['available'])
                (session/'frame-snapshot-1.json').write_text(json.dumps({'tick_ms':150}))
                (session/'frame-snapshot-1.bmp').write_bytes(b'fresh image')
                result=bench.preserve_phase_snapshot(session,100,200,root,'static')
            self.assertTrue(result['available']);self.assertFalse(result['scene_verified'])
            self.assertEqual((root/'static-post.bmp').read_bytes(),b'fresh image')

    def test_post_measurement_capture_respects_existing_storage_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'frame-snapshot-1.json').write_text(json.dumps({'tick_ms':150}))
            (root/'frame-snapshot-1.bmp').write_bytes(b'fresh image')
            with patch.object(bench,'BASE',root),patch.object(bench,'LIMIT',1):
                with self.assertRaisesRegex(ValueError,'budget'):
                    bench.preserve_phase_snapshot(root,100,200,root,'static')

    def test_missing_or_neutral_input_rejects_camera_workload(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)
            self.assertFalse(bench.input_report(path,100,1100,'rx=180')['valid'])
            (path/'input.txt').write_text(''.join(f'{t} 1 0 128 128 128 128 0 0 0\n' for t in range(100,1101,50)))
            self.assertTrue(bench.input_report(path,100,1100,'')['valid'])
            self.assertFalse(bench.input_report(path,100,1100,'rx=180')['valid'])

    def test_delivered_camera_input_requires_broad_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)
            rows=''.join(f'{t} 1 0 128 128 180 128 0 0 0\n' for t in range(100,1101,50))
            (path/'input.txt').write_text(rows)
            (path/'input.txt.1').write_text(rows) # Rotation overlap must not double-count.
            report=bench.input_report(path,100,1100,'rx=180')
            self.assertTrue(report['valid']);self.assertEqual(report['samples'],21)
            (path/'input.txt').write_text('100 1 0 128 128 180 128 0 0 0\n')
            (path/'input.txt.1').unlink()
            self.assertFalse(bench.input_report(path,100,1100,'rx=180')['valid'])

    def test_cpu_sample_overlap_is_excluded_from_optimization_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            session=Path(directory)
            marker=session/'cpu-profile-test.json'
            marker.write_text(json.dumps({'record_type':'cpu-sampling-interval','start_tick_ms':150,'stop_tick_ms':160}))
            report={'phases':[{'name':'static','start_tick_ms':100,'stop_tick_ms':200}]}
            bench.mark_cpu_profile_interference(report,session)
            self.assertTrue(report['diagnostic_only'])
            with self.assertRaisesRegex(ValueError,'sampling overlaps'):bench.compare(report,{})
            marker.write_text(json.dumps({'record_type':'cpu-sampling-interval','start_tick_ms':50,'stop_tick_ms':60}))
            bench.mark_cpu_profile_interference(report,session)
            self.assertFalse(report['diagnostic_only'])
    def test_watchpoint_run_cannot_be_accepted_as_an_optimization(self):
        watched = {'native_manifest': {'settings': {'BB_HW_WATCH': 'watch.txt'}}}
        for previous, current in ((watched, {}), ({}, watched)):
            with self.assertRaisesRegex(ValueError, 'Hardware watchpoints'):
                bench.compare(previous, current)

    def test_current_checkpoint_freezes_save_without_duplicate_shader_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); base=root/'out/runtime-benchmark'
            source=root/'formal'; cache=root/'formal-cache'
            source.mkdir(); cache.mkdir(); (source/'save.dat').write_bytes(b'checkpoint A')
            (cache/'shader.bin').write_bytes(b'cached shader')
            description={'save_source':str(source),'cache_source':str(cache),
                         'seed_path':str(base/'seed-current'),'cache_output':str(base/'gpu-current'),
                         'save_bytes':12,'cache_bytes':13}
            before=bench.fingerprint(source)
            with patch.object(bench,'ROOT',root),patch.object(bench,'BASE',base),\
                 patch.object(bench,'plan',return_value=description),patch.object(bench,'check_game_stopped'):
                _, manifest=bench.prepare(True,'current')
                self.assertEqual(bench.fingerprint(source),before)
                self.assertFalse((base/'seed-current/gpu').exists())
                (source/'save.dat').write_bytes(b'checkpoint B')
                (base/'gpu-current/shader.bin').write_bytes(b'warmed independent cache')
                _, frozen=bench.prepare(True,'current')
                self.assertEqual(frozen['save_fingerprint'],manifest['save_fingerprint'])
                self.assertEqual((base/'seed-current/user/save.dat').read_bytes(),b'checkpoint A')
                self.assertEqual((base/'gpu-current/shader.bin').read_bytes(),b'warmed independent cache')
                self.assertEqual((cache/'shader.bin').read_bytes(),b'cached shader')

    def test_control_replace_retries_sharing_conflicts_and_bounds_acl_failures(self):
        from unittest.mock import Mock
        conflict=PermissionError('Windows sharing conflict');conflict.winerror=32
        pending=Mock();pending.replace.side_effect=[conflict,None]
        with patch.object(bench.time,'sleep'),patch.object(bench.time,'monotonic',return_value=0):
            bench.Session.replace_control(pending,Path('control.txt'))
        self.assertEqual(pending.replace.call_count,2)
        denied=PermissionError('Persistent access denial');denied.winerror=5
        pending.replace.side_effect=denied
        with patch.object(bench.time,'monotonic',side_effect=[0,3]),self.assertRaises(PermissionError):
            bench.Session.replace_control(pending,Path('control.txt'))
        missing=FileNotFoundError('Unrelated error');pending.replace.side_effect=missing
        with self.assertRaises(FileNotFoundError):
            bench.Session.replace_control(pending,Path('control.txt'))
    def test_launch_spec_utf8_title_roundtrips_without_system_codepage(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'launch.json'
            value={'profile':{'title':'inFAMOUS Second Son™ 繁體中文'}}
            bench.save_json(path,value)
            self.assertEqual(bench.load_json(path),value)
    def frames(self, directory, slow=False, gap=False, lost=False, resize=False):
        path=Path(directory)/'present-frames.csv'
        rows=['sequence,tick_ms,interval_ns,width,height,dropped_frames']
        tick=1000
        for i in range(100):
            ns=60_000_000 if slow and i==50 else 10_000_000
            tick+=ns//1_000_000
            rows.append(f'{i+1+(1 if gap and i>=50 else 0)},{tick},{ns},{1280 if resize and i>50 else 1920},1080,{int(lost)}')
        path.write_text('\n'.join(rows)+'\n')
        return tick

    def test_actual_present_fps_and_tail_stalls(self):
        with tempfile.TemporaryDirectory() as directory:
            end=self.frames(directory)
            value=bench.frame_report(directory,1000,end)
            self.assertEqual(value['source'],'fresh_game_present_host_clock')
            self.assertTrue(value['valid']);self.assertEqual(value['fps'],100)
            self.assertEqual(value['minimum_1s_fps'],100)
            end=self.frames(directory,slow=True)
            value=bench.frame_report(directory,1000,end)
            self.assertEqual(value['over_50ms'],1)
            self.assertFalse(value['stable_50']);self.assertFalse(value['stable_60'])
            self.assertLess(value['minimum_1s_fps'],100)

    def test_missing_dropped_resized_and_gapped_data_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError): bench.frame_report(directory,1000,2000)
            for kwargs in ({'lost':True},{'gap':True},{'resize':True}):
                end=self.frames(directory,**kwargs)
                with self.subTest(kwargs=kwargs):
                    value=bench.frame_report(directory,1000,end)
                    self.assertFalse(value['valid']);self.assertIsNone(value['minimum_1s_fps'])
            self.frames(directory)
            self.assertFalse(bench.frame_report(directory,1000,3000)['valid'])
            with self.assertRaises(ValueError): bench.frame_report(directory,2000,1000)

    def test_original_source_fingerprint_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'save.dat').write_bytes(b'actual save fixture')
            old=bench.fingerprint(root)
            self.assertEqual(bench.fingerprint(root),old)
            (root/'save.dat').write_bytes(b'changed save fixture')
            self.assertNotEqual(bench.fingerprint(root),old)

    def test_no_implicit_test_data_copy_without_consent(self):
        with patch.object(bench,'plan',return_value={}),patch.object(bench.shutil,'copytree') as copy:
            with self.assertRaisesRegex(ValueError,'allow-test-data'): bench.prepare(False)
            copy.assert_not_called()

    def test_unowned_or_escaped_directory_cannot_be_deleted(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);base=root/'out/runtime-benchmark';base.mkdir(parents=True)
            with patch.object(bench,'ROOT',root),patch.object(bench,'BASE',base):
                with self.assertRaisesRegex(ValueError,'unowned'): bench.verify_base()
                (base/'owner.json').write_text(json.dumps({'owner':bench.OWNER}))
                bench.verify_base()
                with self.assertRaises(ValueError): bench.remove_owned_child('seed')
                with self.assertRaises(ValueError): bench.remove_owned_child('../user')
            with patch.object(bench,'ROOT',root),patch.object(bench,'BASE',root/'outside'):
                with self.assertRaisesRegex(ValueError,'expected'): bench.verify_base()

    def test_command_cannot_target_another_process_or_arbitrary_action(self):
        session=object.__new__(bench.Session)
        with self.assertRaises(ValueError): session.command('kill 123')

    def test_storage_budget_and_runtime_comparison_are_enforced(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'data').write_bytes(b'12345')
            with patch.object(bench,'BASE',root),patch.object(bench,'LIMIT',4),self.assertRaisesRegex(ValueError,'budget'):
                bench.check_budget()
            end=self.frames(root)
            phase=bench.frame_report(root,1000,end);phase['name']='fixed'
            old={'schema':bench.OWNER,'game_sha256':'game','save_seed':{'save':'hash'},'warmup_seconds':45,
                 'phase_seconds':20,'host':{},'phases':[phase]}
            self.assertEqual(bench.compare(old,old)[0]['fps_change_percent'],0)
            self.assertEqual(bench.compare(json.loads(json.dumps(old)),old)[0]['fps_change_percent'],0)
            wrong={**old,'save_seed':{'save':'different'}}
            with self.assertRaisesRegex(ValueError,'save_seed'):bench.compare(old,wrong)


if __name__=='__main__': unittest.main()
