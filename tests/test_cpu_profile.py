"""Native diagnostic identity guard and balanced thread suspension/resumption."""
from paths import ROOT
import csv
import io
import os
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import profile_runtime_cpu as profile

class CpuProfilerInputTests(unittest.TestCase):
    def fixture(self,root):
        session=root/'out/runtime-benchmark/run/debug/session-test'
        session.mkdir(parents=True)
        exe=root/'out/bb-probe.exe';exe.write_bytes(b'fingerprint fixture')
        state={'collector':'infamous-debug-v1','pid':123,'finished':False,
               'latest':{'created_filetime':456}}
        manifest={'pid':123,'command':[str(exe)],
                  'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()}
        (session/'status.json').write_text(json.dumps(state))
        (session/'manifest.json').write_text(json.dumps(manifest))
        return session,exe,state
    def test_completed_reused_or_changed_process_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);session,exe,state=self.fixture(root)
            output=root/'out/reverse-engineering/sample.csv'
            with patch.object(profile,'ROOT',root):
                self.assertEqual(profile.capture_inputs(session,output)[:2],(123,456))
                for update in ({'finished':True},{'pid':124},{'latest':{}}):
                    (session/'status.json').write_text(json.dumps(state|update))
                    with self.assertRaises(ValueError):profile.capture_inputs(session,output)
                (session/'status.json').write_text(json.dumps(state))
                exe.write_bytes(b'changed since launch')
                with self.assertRaisesRegex(ValueError,'Executable changed'):
                    profile.capture_inputs(session,output)
    def test_only_isolated_session_and_new_diagnostic_csv_are_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);session,_,_=self.fixture(root)
            with patch.object(profile,'ROOT',root):
                with self.assertRaisesRegex(ValueError,'isolated benchmark'):
                    profile.capture_inputs(root/'out/CUSA00309/debug/session-user',root/'out/reverse-engineering/test.csv')
                for output in (root/'game/sample.csv',root/'out/reverse-engineering/sample.json'):
                    with self.assertRaisesRegex(ValueError,'new output'):
                        profile.capture_inputs(session,output)
                output=root/'out/reverse-engineering/sample.csv';output.parent.mkdir()
                output.write_bytes(b'keep existing diagnostic')
                with self.assertRaisesRegex(ValueError,'new output'):profile.capture_inputs(session,output)
                self.assertEqual(output.read_bytes(),b'keep existing diagnostic')

class NativeCpuProfilerTests(unittest.TestCase):
    def run_tool(self,*args):
        return subprocess.run([str(ROOT/'out/cpu-profile.exe'),*args],
                              capture_output=True,text=True,timeout=10)
    def test_self_sampling_resumes_worker_and_records_cpu_progress(self):
        result=self.run_tool('--self-test')
        self.assertEqual(result.returncode,0,result.stderr)
        lines=result.stdout.splitlines()
        self.assertIn('diagnostic=1',lines[0])
        rows=list(csv.DictReader(io.StringIO('\n'.join(line for line in lines if not line.startswith('#')))))
        self.assertTrue(rows)
        self.assertTrue(any(int(row['cpu_delta_100ns'])>0 for row in rows))
        self.assertTrue(all(int(row['rip'],16)>0 for row in rows))
    def test_other_executable_or_creation_time_is_rejected_before_sampling(self):
        result=self.run_tool(str(os.getpid()),'100',str(ROOT/'out/cpu-profile.exe'),'1')
        self.assertEqual(result.returncode,2,result.stderr)
        self.assertIn('process identity mismatch',result.stderr)
        self.assertNotIn('tick_ms,',result.stdout)
