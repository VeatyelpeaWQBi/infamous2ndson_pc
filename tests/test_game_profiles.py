"""Fingerprint, title isolation and launcher routing with synthetic game files."""
from paths import ROOT
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import game_profiles
import run_windows


def sfo_fixture(title):
    key=b'TITLE_ID\0'; value=title.encode()+b'\0'
    return (struct.pack('<4sIIII',b'\0PSF',0x101,36,36+len(key),1)+
            struct.pack('<HHIII',0,0x204,len(value),len(value),0)+key+value)


class GameProfiles(unittest.TestCase):
    def test_locale_configuration_is_loaded_and_invalid_values_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'infamous-locale.json'
            self.assertEqual(game_profiles.console_locale(tmp)['system_language'],10)
            path.write_text(json.dumps({'system_language':1,'confirm_button':'cross'}))
            self.assertEqual(game_profiles.console_locale(tmp)['confirm_button'],'cross')
            for key,value in (('system_language',True),('system_language',99),('timezone_minutes',841),
                              ('timezone_minutes','480'),('confirm_button','invalid'),('region','invalid')):
                path.write_text(json.dumps({key:value}))
                with self.assertRaises(ValueError): game_profiles.console_locale(tmp)
    def test_launcher_debug_route_uses_collector_without_launching_second_process(self):
        with tempfile.TemporaryDirectory() as tmp:
            data=Path(tmp); digest=self.game(data)
            env=dict(os.environ,BB_PREBUILT='1',BB_DEBUG_SESSION='1',BB_DATA_DIR=str(data),BB_GAME_DIR=str(data))
            with patch.dict(os.environ,env,clear=True),patch.object(sys,'argv',['run_windows.py']), \
                 patch.object(game_profiles,'SECOND_SON_SHA256',digest),patch.object(run_windows,'run',return_value=0), \
                 patch('debug_session.collect',return_value=139) as collector, \
                 patch.object(run_windows.subprocess,'call') as direct:
                self.assertEqual(run_windows.main(),139)
                self.assertEqual(collector.call_count,1); direct.assert_not_called()
                command,cwd,base,profile=collector.call_args.args
                self.assertEqual(Path(command[0]),ROOT/'out/bb-probe.exe')
                self.assertEqual(base,data/'out/CUSA00309/debug')
                self.assertEqual(profile['title_id'],'CUSA00309')
    def test_prepare_child_output_preserves_unicode_under_gbk_environment(self):
        with patch.dict(os.environ, {'PYTHONIOENCODING': 'gbk'}):
            title=run_windows.run([sys.executable, '-c', "print('inFAMOUS Second Son\\u2122')"], capture=True)
        self.assertEqual(title, 'inFAMOUS Second Son\u2122')

    def game(self, root, title='CUSA00309'):
        (root/'sce_sys').mkdir(); (root/'sce_sys/param.sfo').write_bytes(sfo_fixture(title))
        (root/'art').mkdir(); (root/'eboot.bin').write_bytes(b'audited executable fixture')
        return hashlib.sha256((root/'eboot.bin').read_bytes()).hexdigest()

    def test_unknown_executable_rejected_even_with_matching_title(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); self.game(root)
            with self.assertRaisesRegex(ValueError,'not the audited build'):
                game_profiles.select_profile(root)

    def test_unsupported_title_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); self.game(root,'CUSA99999')
            with self.assertRaisesRegex(ValueError,'Unsupported title'):
                game_profiles.select_profile(root)

    def test_fingerprint_and_resource_directory_are_both_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); digest=self.game(root)
            with patch.object(game_profiles,'SECOND_SON_SHA256',digest):
                profile=game_profiles.select_profile(root)
                self.assertFalse(profile['address_patches']); self.assertEqual(profile['resource_root'],'art')
                (root/'art').rmdir()
                with self.assertRaisesRegex(ValueError,'art resource'):
                    game_profiles.select_profile(root)

    def test_launcher_ignores_inherited_bloodborne_patches_and_separates_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            data=Path(tmp); digest=self.game(data)
            commands=[]; prepared=[]
            def prepare(arguments, **kwargs):
                prepared.append([str(a) for a in arguments]); return 0
            def launch(arguments,cwd):
                commands.append([str(a) for a in arguments])
                self.assertEqual(os.environ['BB_UPSCALER'],'none')
                self.assertEqual(os.environ['BB_GAME_PROFILE'],'infamous')
                self.assertEqual(os.environ['BB_DRAW_PIPE'],'0')
                self.assertEqual(os.environ['BB_LANGUAGE'],'10')
                self.assertEqual(os.environ['BB_REGION'],'HK')
                self.assertEqual(os.environ['BB_TIMEZONE_MINUTES'],'480')
                self.assertEqual(os.environ['BB_ENTER_BUTTON'],'0')
                self.assertEqual(os.environ['BB_USER_DIR'],str(data/'profiles/CUSA00309/user'))
                self.assertNotIn('BB_RENDER_RES',os.environ)
                self.assertNotIn('BB_PATCHES',os.environ)
                return 0
            env=dict(os.environ,BB_PREBUILT='1',BB_DATA_DIR=str(data),BB_GAME_DIR=str(data),
                     BB_CONFIG=str(ROOT/'bbport.ini'),BB_USER_DIR='user',BB_RENDER_RES='800x450',
                     BB_PATCHES='Uncap FPS++',BB_MODS_ENABLED='1',BB_UPSCALER='dlss',BB_FPS='uncap',BB_DRAW_PIPE='1',
                     BB_LANGUAGE='1',BB_REGION='US',BB_TIMEZONE_MINUTES='-480',BB_ENTER_BUTTON='1')
            with patch.dict(os.environ,env,clear=True), patch.object(sys,'argv',['run_windows.py','--timeout','180']), \
                 patch.object(game_profiles,'SECOND_SON_SHA256',digest), \
                 patch.object(run_windows,'run',side_effect=prepare), \
                 patch.object(run_windows.subprocess,'call',side_effect=launch):
                self.assertEqual(run_windows.main(),0)
            self.assertEqual(len(commands),1)
            self.assertEqual(Path(commands[0][0]),ROOT/'out/bb-probe.exe')
            self.assertEqual(Path(commands[0][1]),data/'out/CUSA00309/boot-linked.bin')
            stages=[Path(c[1]).name for c in prepared]
            self.assertEqual(stages,['prepare.py','link_modules.py','content_profile.py'])
            self.assertIn('--scoped-imports',prepared[1])
            self.assertEqual(struct.unpack('<8sQQ',(data/'out/CUSA00309/patches.bin').read_bytes()),
                             (b'BBPATCH2',0x400000,0))
            self.assertEqual((data/'eboot.bin').read_bytes(),b'audited executable fixture')
