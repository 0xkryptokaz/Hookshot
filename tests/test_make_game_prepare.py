"""Game-free checks of make_game's --game and --prepare (RecompLauncher): the copy chosen, and the progress lines.

Run with: python -I tests/test_make_game_prepare.py
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class PrepareTests(unittest.TestCase):
    def tearDown(self):
        make_game.PREPARE = False

    def parse(self, *argv):
        with mock.patch.object(sys, "argv", ["make_game.py", *argv]):
            return make_game.parse_args()

    def test_game_file_is_the_nup(self):
        a = self.parse("--game", "game.nup", "--out", "o")
        self.assertEqual((a.nup, a.romfs, a.exefs, a.prepare), ("game.nup", None, None, False))
        self.assertFalse(make_game.PREPARE)

    def test_game_folder_is_romfs_and_exefs(self):
        with tempfile.TemporaryDirectory() as d:
            a = self.parse("--game", d, "--out", "o")
        self.assertEqual((a.nup, a.romfs, a.exefs), (None, os.path.join(d, "romfs"), os.path.join(d, "exefs")))

    def test_prepare_defaults_the_out_folder(self):
        a = self.parse("--game", "game.nup", "--prepare")
        self.assertTrue(make_game.PREPARE)
        self.assertEqual(a.out, os.path.join(os.path.expanduser("~"), "Games", "Hookshot"))
        self.assertEqual(self.parse("--game", "g.nup", "--prepare", "--out", "o").out, "o")

    def test_progress_lines(self):
        make_game.PREPARE = True
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            make_game.say("stage", "compile", "Compiling")
            make_game.say("progress", "0.5")
            make_game.say("times", "1", "1", "-1")
            make_game.say("done", "o")
        self.assertEqual(out.getvalue().splitlines(), ["progress 0.250 Compiling", "progress 0.600 Compiling", "progress 1.000 Done"])

    def test_without_prepare_the_app_lines_are_unchanged(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            make_game.say("stage", "read", "Reading your game files")
        self.assertEqual(out.getvalue(), "@@stage read Reading your game files\n")


if __name__ == "__main__":
    unittest.main()
