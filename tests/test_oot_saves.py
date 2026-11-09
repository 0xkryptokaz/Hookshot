"""scripts/oot_saves.py, game-free: finding the folders, copying both ways, the backups, the guards. Temp folders only.

python -I -B tests/test_oot_saves.py
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("oot_saves", ROOT / "scripts" / "oot_saves.py")
oot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oot)

UID = "0123456789abcdef0123456789abcdef"
PROFILE = "FEDCBA9876543210FEDCBA9876543210"


def make_save(folder, tag, slots=("slot_00", "slot_01")):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "account.sav").write_bytes(b"account-" + tag)
    (folder / "option.sav").write_bytes(b"option-" + tag)
    for s in slots:
        (folder / s).mkdir()
        (folder / s / "progress.sav").write_bytes(b"progress-" + tag + s.encode())
        (folder / s / "caption.sav").write_bytes(b"caption-" + tag + s.encode())


def tree(folder):
    return {str(p.relative_to(folder)): p.read_bytes() for p in sorted(folder.rglob("*")) if p.is_file()}


class Case(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.data = self.tmp / "share"
        self.hs = self.data / "hookshot" / "save" / UID
        self.li = self.data / "eden" / "nand" / "user" / "save" / oot.NO_ACCOUNT / PROFILE / oot.TITLE
        make_save(self.hs, b"HS")
        make_save(self.li, b"LI", slots=("slot_00",))
        self.backups = self.tmp / "backups"
        env = mock.patch.dict(os.environ, {"XDG_DATA_HOME": str(self.data)})
        env.start()
        self.addCleanup(env.stop)
        idle = mock.patch.object(oot, "running_games", return_value=[])
        self.running = idle.start()
        self.addCleanup(idle.stop)

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = oot.main(list(args) + ["--backup-dir", str(self.backups)])
        return code, out.getvalue(), err.getvalue()

    def backup_dirs(self):
        return sorted(self.backups.iterdir()) if self.backups.exists() else []


class Discovery(Case):
    def test_linux_layout(self):
        self.assertEqual(oot.find_hookshot(None), {UID: str(self.hs)})
        self.assertEqual(oot.find_lichii(None), {PROFILE: str(self.li)})

    def test_windows_layout_through_overrides(self):
        game = self.tmp / "Hookshot"
        win_hs = game / "user" / "save" / UID
        make_save(win_hs, b"W")
        appdata = self.tmp / "AppData" / "Roaming" / "eden"
        win_li = appdata / "nand" / "user" / "save" / oot.NO_ACCOUNT / PROFILE / oot.TITLE
        make_save(win_li, b"W")
        self.assertEqual(oot.find_hookshot(str(game)), {UID: str(win_hs)})
        self.assertEqual(oot.find_lichii(str(appdata)), {PROFILE: str(win_li)})
        with mock.patch.object(oot, "WINDOWS", True):
            with self.assertRaises(oot.Fail):
                oot.find_hookshot(None)
            with mock.patch.dict(os.environ, {"APPDATA": str(self.tmp / "AppData" / "Roaming")}):
                self.assertEqual(oot.find_lichii(None), {PROFILE: str(win_li)})

    def test_deeper_folders_accepted(self):
        self.assertEqual(oot.find_hookshot(str(self.hs)), {UID: str(self.hs)})
        self.assertEqual(oot.find_lichii(str(self.li)), {PROFILE: str(self.li)})

    def test_missing_folders(self):
        with self.assertRaises(oot.Fail):
            oot.find_hookshot(str(self.tmp / "nothing"))
        (self.tmp / "empty").mkdir()
        with self.assertRaises(oot.Fail):
            oot.find_lichii(str(self.tmp / "empty"))

    def test_ambiguous_profiles_listed(self):
        other = "11111111111111111111111111111111"
        make_save(self.data / "eden" / "nand" / "user" / "save" / oot.NO_ACCOUNT / other / oot.TITLE, b"O")
        before = tree(self.li)
        code, out, err = self.run_main("to-lichii")
        self.assertEqual(code, 1)
        self.assertIn(PROFILE, err)
        self.assertIn(other, err)
        self.assertIn("--profile", err)
        self.assertEqual(tree(self.li), before)
        self.assertEqual(self.backup_dirs(), [])
        code, out, err = self.run_main("to-lichii", "--profile", other.lower())
        self.assertEqual(code, 0, err)


class Copy(Case):
    def test_to_lichii(self):
        before = tree(self.li)
        code, out, err = self.run_main("to-lichii")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.li), tree(self.hs))
        (backup,) = self.backup_dirs()
        self.assertTrue(backup.name.startswith("to-lichii-"))
        self.assertEqual(tree(backup), before)

    def test_to_hookshot(self):
        before = tree(self.hs)
        code, out, err = self.run_main("to-hookshot")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.hs), tree(self.li))
        self.assertNotIn("slot_01", tree(self.hs))
        (backup,) = self.backup_dirs()
        self.assertTrue(backup.name.startswith("to-hookshot-"))
        self.assertEqual(tree(backup), before)

    def test_keep_settings(self):
        code, out, err = self.run_main("to-lichii", "--keep-settings")
        self.assertEqual(code, 0, err)
        self.assertEqual((self.li / "option.sav").read_bytes(), b"option-LI")
        self.assertEqual((self.li / "account.sav").read_bytes(), b"account-HS")
        self.assertEqual((self.li / "slot_01" / "progress.sav").read_bytes(), b"progress-HSslot_01")

    def test_dry_run_changes_nothing(self):
        before = (tree(self.hs), tree(self.li))
        self.running.return_value = ["Lichii"]   # (a dry run does not mind)
        code, out, err = self.run_main("to-lichii", "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertEqual((tree(self.hs), tree(self.li)), before)
        self.assertEqual(self.backup_dirs(), [])
        self.assertIn("Would copy", out)

    def test_source_without_slots(self):
        for s in ("slot_00", "slot_01"):
            for f in (self.hs / s).iterdir():
                f.unlink()
            (self.hs / s).rmdir()
        before = tree(self.li)
        code, out, err = self.run_main("to-lichii")
        self.assertEqual(code, 1)
        self.assertEqual(tree(self.li), before)

    def test_list(self):
        code, out, err = self.run_main("list")
        self.assertEqual(code, 0, err)
        self.assertIn(UID, out)
        self.assertIn(PROFILE, out)
        self.assertIn("slot_01", out)


class Guard(Case):
    def test_refuses_while_running(self):
        self.running.return_value = ["Hookshot"]
        before = (tree(self.hs), tree(self.li))
        code, out, err = self.run_main("to-lichii")
        self.assertEqual(code, 1)
        self.assertIn("Hookshot", err)
        self.assertEqual((tree(self.hs), tree(self.li)), before)
        self.assertEqual(self.backup_dirs(), [])

    def test_force(self):
        self.running.return_value = ["Lichii"]
        code, out, err = self.run_main("to-lichii", "--force")
        self.assertEqual(code, 0, err)
        self.assertEqual(tree(self.li), tree(self.hs))

    def test_process_names(self):
        out = '"hookshot.exe","12","Console","1","10 K"\r\n"Eden.exe","3","Console","1","1 K"\r\n"notepad.exe","4","x","1","1 K"\r\n'
        # (the real function, not the test's mock of it)
        real = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(real)
        with mock.patch.object(real, "WINDOWS", True), \
             mock.patch.object(real.subprocess, "run", return_value=mock.Mock(stdout=out)):
            self.assertEqual(real.running_games(), ["Eden", "Hookshot"])


if __name__ == "__main__":
    unittest.main()
