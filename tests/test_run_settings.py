"""Exercise the Windows launcher across restarts with synthetic game/preparation inputs."""
from paths import ROOT
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import run_windows


class RestartResolutionTests(unittest.TestCase):
    def run_restarts(self, explicit=False, live=False, ini_extra='', caps=None, bare_path=False):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            out = data / 'out'
            out.mkdir()
            # The real patch compiler validates against this synthetic ELF load segment.
            elf = bytearray(120)
            struct.pack_into('<Q', elf, 0x20, 64)
            struct.pack_into('<HH', elf, 0x36, 56, 1)
            struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0, 0, 0, 0x6000000, 0)
            (out / 'eboot.elf').write_bytes(elf)
            (data / 'eboot.bin').touch()
            config = data / 'bbport.ini'
            config.write_text('upscaler=fsr3\npreset=1\noutput_res=1280x720\n' + ini_extra)
            environments = []
            original_run = run_windows.run

            def launch(command, cwd):
                self.assertTrue(str(command[0]).endswith('bb-probe.exe'))
                environments.append({key: os.environ.get(key) for key in
                                     ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES')})
                stage = len(environments) - 1
                if stage < 2:
                    config.write_text('upscaler=fsr3\npreset=4\noutput_res=' +
                                      ('1280x720' if stage == 0 else '1920x1080') + '\n')
                return 0

            def run(arguments, capture=False, check=True, env=None):
                script = Path(arguments[1]).name if len(arguments) > 1 else ''
                if script == 'mods.py':
                    return str(data)
                if script in ('prepare.py', 'link_libc.py', 'link_modules.py', 'content_profile.py'):
                    return '' if capture else 0
                if str(arguments[0]).endswith('bb-gpu-capabilities.exe'):
                    return str(caps or 0)
                return original_run(arguments, capture=capture, check=check, env=env)

            env = dict(os.environ, BB_PREBUILT='1', BB_DATA_DIR=str(data), BB_CONFIG=str(config),
                       BB_GAME_DIR=str(data), BB_MODS_ENABLED='0', BB_FPS='uncap', BB_PATCHES='',
                       BB_PATCHES_DIR=str(data / 'patches'), BB_PATCHES_CONFIG=str(data / 'patches.json'))
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES', 'BB_LIVE_RES', 'BB_PROBE'):
                env.pop(key, None)
            if explicit:
                env['BB_RENDER_RES'] = '800x450'
            if live:
                env['BB_LIVE_RES'] = '1'
            if bare_path:
                env['PATH'] = str(Path(os.environ['WINDIR']) / 'System32')
            with patch.dict(os.environ, env, clear=True), patch.object(sys, 'argv', ['run_windows.py']), \
                    patch.object(run_windows, 'run', side_effect=run), \
                    patch.object(run_windows.subprocess, 'call', side_effect=launch):
                for _ in range(3):
                    self.assertEqual(run_windows.main(), 0)
            return environments

    def test_outputs_other_than_1080p_patch_the_render_size_and_restarts_recompute_it(self):
        rows = self.run_restarts()
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], ['854x480', '426x240', None])
        self.assertEqual([row['BB_OUTPUT_RES'] for row in rows], ['1280x720', '1280x720', None])
        self.assertEqual([row['BB_AUTO_RENDER_RES'] for row in rows], ['1', '1', None])

    def test_live_resolution_keeps_guest_sizes_native(self):
        rows = self.run_restarts(live=True)
        self.assertTrue(all(row['BB_RENDER_RES'] is None for row in rows))
        self.assertTrue(all(row['BB_OUTPUT_RES'] is None for row in rows))
        self.assertTrue(all(row['BB_AUTO_RENDER_RES'] is None for row in rows))

    def test_live_resolution_setting_and_gpu_check(self):
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=1\n')[0]['BB_RENDER_RES'])
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=0\n', caps=1)[0]['BB_RENDER_RES'], '854x480')
        self.assertEqual(self.run_restarts(caps=1)[0]['BB_RENDER_RES'], '854x480')
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=auto\n', caps=1)[0]['BB_RENDER_RES'])
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=auto\n', caps=0)[0]['BB_RENDER_RES'], '854x480')

    def test_live_resolution_does_not_require_unix_shell_tools(self):
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=0\n', caps=1,
                                           bare_path=True)[0]['BB_RENDER_RES'], '854x480')
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=auto\n', caps=1,
                                            bare_path=True)[0]['BB_RENDER_RES'])

    def test_explicit_render_override_survives_restart(self):
        rows = self.run_restarts(explicit=True)
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], ['800x450'] * 3)
        self.assertTrue(all(row['BB_AUTO_RENDER_RES'] is None for row in rows))
