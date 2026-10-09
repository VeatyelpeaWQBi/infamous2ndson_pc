from paths import ROOT
from pathlib import Path
import tempfile
import unittest
from settings_defaults import read_defaults,DEFAULTS
from run_windows import settings_value

class SettingsDefaultsTests(unittest.TestCase):
    def test_launcher_missing_keys_use_shared_defaults_and_file_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            config=Path(directory)/'settings.ini'
            self.assertEqual(settings_value(config,'live_resolution'),DEFAULTS['live_resolution'])
            config.write_text('show_fps=1\n',encoding='utf-8')
            self.assertEqual(settings_value(config,'show_fps'),'1')
            self.assertEqual(settings_value(config,'upscaler'),DEFAULTS['upscaler'])
            self.assertIsNone(settings_value(config,'unknown'))

    def test_ambiguous_or_malformed_defaults_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'table.inc'
            path.write_text('BB_SETTING("a", "1")\nBB_SETTING("a", "0")\n',encoding='utf-8')
            with self.assertRaisesRegex(ValueError,'Duplicate'):read_defaults(path)
            path.write_text('BB_SETTING(broken)\n',encoding='utf-8')
            with self.assertRaisesRegex(ValueError,'Invalid'):read_defaults(path)
