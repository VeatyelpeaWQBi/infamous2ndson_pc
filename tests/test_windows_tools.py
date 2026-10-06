"""Windows entry-point contracts: no installs, no implicit build/download during tests."""
from paths import ROOT
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import windows_tools


class WindowsEntryTests(unittest.TestCase):
    def test_explicit_existing_msys_root_wins_over_path(self):
        with patch.dict(os.environ, {'BB_MSYS2': r'D:\existing tools\msys64'}), \
                patch.object(windows_tools.shutil, 'which', return_value=r'C:\other\clang64\bin\clang.exe'):
            self.assertEqual(windows_tools.msys_root(), Path(r'D:\existing tools\msys64'))

    def test_test_command_requires_native_executables_and_does_not_build(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(windows_tools, 'ROOT', Path(temporary)), \
                patch.object(windows_tools.subprocess, 'call') as execute:
            with self.assertRaisesRegex(RuntimeError, 'Missing test artifacts'):
                windows_tools.test()
            execute.assert_not_called()

    def test_build_uses_existing_bash_and_preserves_paths_with_spaces(self):
        with tempfile.TemporaryDirectory(prefix='bb windows ') as temporary:
            root = Path(temporary)
            bash = root / 'usr/bin/bash.exe'
            bash.parent.mkdir(parents=True)
            bash.touch()
            with patch.dict(os.environ, {'BB_MSYS2': str(root)}), \
                    patch.object(windows_tools.subprocess, 'call', return_value=23) as execute:
                self.assertEqual(windows_tools.build(build_tests=True), 23)
                args, kwargs = execute.call_args
                self.assertEqual(args[0][0], str(bash))
                self.assertIn('--build-tests', args[0][-1])
                self.assertEqual(kwargs['env']['BB_ALLOW_DOWNLOADS'], '0')
                self.assertEqual(kwargs['env']['MSYSTEM'], 'CLANG64')
                self.assertEqual(kwargs['env']['MSYS2_PATH_TYPE'], 'inherit')
                self.assertEqual(kwargs['env']['BB_PROJECT_ROOT'], str(ROOT))
                self.assertIn('cygpath', args[0][-1])

    def test_missing_existing_bash_fails_without_starting_a_process(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.dict(os.environ, {'BB_MSYS2': temporary}), \
                patch.object(windows_tools.subprocess, 'call') as execute:
            with self.assertRaisesRegex(RuntimeError, 'Existing MSYS2 not found'):
                windows_tools.build()
            execute.assert_not_called()

    def test_batch_test_entry_propagates_python_failure(self):
        # The common batch dispatcher must not swallow ERRORLEVEL inside an IF block.
        with tempfile.TemporaryDirectory() as temporary:
            script = Path(temporary) / 'exit_code.py'
            script.write_text('raise SystemExit(23)\n')
            import sys
            env = dict(os.environ, BB_PYTHON=sys.executable)
            command = 'call ' + subprocess.list2cmdline([str(ROOT / 'scripts/windows_python.bat'), str(script)])
            result = subprocess.run([os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/c', command],
                                    cwd=ROOT, env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
