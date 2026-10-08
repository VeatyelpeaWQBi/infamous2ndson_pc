"""Actual runtime harness: presentation data, isolation, bounds and consent failures."""
from paths import ROOT
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import runtime_benchmark as bench


class RuntimeBenchmarkTests(unittest.TestCase):
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
