"""Chooser persistence/cancellation, with hidden windows owned by this test."""
from paths import ROOT
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import tkinter as tk
import infamous_language_menu as menu
import game_profiles

class LanguageMenuTests(unittest.TestCase):
    def fixture(self,path):
        cache=path/'game/art/cache'; cache.mkdir(parents=True)
        for asset in ('chinese','english','korean'): (cache/f'lang_{asset}_text.xpps').write_bytes(b'fixture')
        return path/'game'

    def test_only_languages_with_existing_text_resources_are_offered(self):
        with tempfile.TemporaryDirectory() as tmp:
            data=Path(tmp); game=self.fixture(data)
            (game/'art/cache/lang_korean_text.xpps').unlink()
            self.assertEqual([code for _,code in menu.available_languages(game)],[10,1])

    def test_cancel_does_not_change_saved_configuration(self):
        with tempfile.TemporaryDirectory() as tmp:
            data=Path(tmp); game=self.fixture(data)
            original=json.dumps(game_profiles.SECOND_SON_LOCALE).encode()
            (data/'infamous-locale.json').write_bytes(original)
            root=tk.Tk(); root.withdraw()
            chooser=menu.LanguageMenu(data,game,root)
            chooser.language.current(1); chooser.cancel_button.invoke()
            self.assertEqual(chooser.result,1)
            self.assertEqual((data/'infamous-locale.json').read_bytes(),original)

    def test_start_saves_choice_and_next_window_restores_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            data=Path(tmp); game=self.fixture(data)
            root=tk.Tk(); root.withdraw(); chooser=menu.LanguageMenu(data,game,root)
            self.assertEqual(chooser.options[chooser.language.current()][1],10)
            self.assertTrue(chooser.show_fps.get())
            chooser.fps_checkbox.invoke()
            chooser.language.current(1); chooser.button.current(1); chooser.start_button.invoke()
            self.assertEqual(chooser.result,0)
            locale=game_profiles.console_locale(data)
            self.assertEqual(locale,dict(region='HK',system_language=1,timezone_minutes=480,confirm_button='cross',show_fps=False))
            root=tk.Tk(); root.withdraw(); second=menu.LanguageMenu(data,game,root)
            self.assertEqual(second.options[second.language.current()][1],1)
            self.assertEqual(second.button.current(),1)
            self.assertFalse(second.show_fps.get()); second.cancel()

    def test_write_failure_keeps_window_open_and_configuration_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            data=Path(tmp); game=self.fixture(data)
            root=tk.Tk(); root.withdraw(); chooser=menu.LanguageMenu(data,game,root)
            with patch.object(menu,'save_selection',side_effect=OSError('test write failure')):
                chooser.start_button.invoke()
            self.assertEqual(chooser.result,1); self.assertTrue(root.winfo_exists())
            self.assertIn('无法保存',chooser.error.get()); self.assertFalse((data/'infamous-locale.json').exists())
            chooser.cancel()
