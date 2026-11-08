"""Linux updates download a part only when its checksum changed, and the launcher starts the installed builder.

python -I -B tests/test_linux_update.py
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder" / "linux"))
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "scripts" / "linux"))
import linux_update  # noqa: E402
import make_parts  # noqa: E402


def appdir(path):
    """A tiny AppDir: one file in each part, plus the launcher script an install copies to AppRun."""
    tools = path / "usr" / "share" / "hookshot"
    (tools / "builder" / "linux").mkdir(parents=True)
    shutil.copyfile(ROOT / "builder" / "linux" / "AppRun", tools / "builder" / "linux" / "AppRun")
    (tools / "VERSION").write_text("v1\n", encoding="utf-8")
    (tools / "nx2").mkdir()
    (tools / "nx2" / "marker.py").write_text("print('one')\n", encoding="utf-8")
    for dest in ("usr/python", "usr/lib/hookshot/emsdk", "usr/lib/hookshot/ffmpeg", "usr/lib/hookshot/sysroot"):
        folder = path / dest
        folder.mkdir(parents=True)
        (folder / "kept").write_text(dest + "\n", encoding="utf-8")
    lib = path / "usr" / "lib" / "hookshot"
    (lib / "hookshot_vk.so").write_bytes(b"vk")
    (lib / "libSDL2-2.0.so.0").write_bytes(b"sdl")
    (path / "AppRun").write_text("#!/bin/sh\n", encoding="utf-8")
    (path / "AppRun").chmod(0o755)


class UpdateTests(unittest.TestCase):
    def test_the_button_matches_the_windows_builder(self):
        self.assertEqual(linux_update.choose_mode("v1", "v2", "v2", False, True), "update-game")
        self.assertEqual(linux_update.choose_mode("v2", "v2", "v2", False, True), "up-to-date")
        self.assertEqual(linux_update.choose_mode("v1", "v1", None, False, True), "up-to-date")
        self.assertEqual(linux_update.choose_mode("v1", "v1", "v2", False, True), "update-game")
        self.assertEqual(linux_update.choose_mode(None, "v1", "v2", False, False), "update-builder")
        self.assertEqual(linux_update.choose_mode(None, "v2", "v2", False, False), "build")
        self.assertEqual(linux_update.choose_mode("v2", "v2", "v2", True, True), "build")

    def test_a_changed_part_is_downloaded_and_the_rest_are_kept(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-parts-") as tmp:
            root = Path(tmp)
            old = root / "old"
            appdir(old)
            first = root / "parts1"
            argv = sys.argv
            try:
                sys.argv = ["make_parts.py", str(old), "v1", str(first)]
                make_parts.main()
            finally:
                sys.argv = argv
            manifest = linux_update.parse_list((first / "hookshot-linux.txt").read_text(encoding="utf-8"))
            home = root / "home"
            os.environ["HOOKSHOT_BUILDER_HOME"] = str(home)
            self.addCleanup(lambda: os.environ.pop("HOOKSHOT_BUILDER_HOME", None))
            downloaded = []

            def get(url):
                downloaded.append(url)
                name = url.rsplit("/", 1)[-1]
                return (first / name).read_bytes()

            linux_update.install(manifest, str(old), manifest, get=get)
            self.assertEqual(downloaded, [])
            installed = home / "hookshot" / "builder" / "versions" / "v1"
            self.assertTrue((installed / "AppRun").stat().st_mode & stat.S_IXUSR)
            self.assertEqual((installed / "usr" / "share" / "hookshot" / "nx2" / "marker.py").read_text(encoding="utf-8"), "print('one')\n")

            new = root / "new"
            shutil.copytree(old, new)
            (new / "usr" / "share" / "hookshot" / "nx2" / "marker.py").write_text("print('two')\n", encoding="utf-8")
            second = root / "parts2"
            try:
                sys.argv = ["make_parts.py", str(new), "v2", str(second)]
                make_parts.main()
            finally:
                sys.argv = argv
            nxt = linux_update.parse_list((second / "hookshot-linux.txt").read_text(encoding="utf-8"))
            downloaded.clear()

            def get2(url):
                downloaded.append(url)
                name = url.rsplit("/", 1)[-1]
                return (second / name).read_bytes()

            linux_update.install(nxt, str(installed), manifest, get=get2)
            self.assertEqual(len(downloaded), 1)
            self.assertIn("usr-share-hookshot", downloaded[0])
            built = home / "hookshot" / "builder" / "versions" / "v2"
            self.assertEqual((built / "usr" / "share" / "hookshot" / "nx2" / "marker.py").read_text(encoding="utf-8"), "print('two')\n")
            self.assertEqual((built / "usr" / "lib" / "hookshot" / "hookshot_vk.so").read_bytes(), b"vk")
            self.assertFalse((home / "hookshot" / "builder" / "versions" / "v1").exists())

    def test_the_sysroot_path_is_not_part_of_an_object_key(self):
        import build
        handle, name = tempfile.mkstemp()
        os.write(handle, b"int x;\n")
        os.close(handle)
        try:
            one = build.object_key(name, ["--sysroot=/tmp/.mount_aaa/usr/lib/hookshot/sysroot", "-O2"], b"deps")
            two = build.object_key(name, ["--sysroot=/tmp/.mount_bbb/usr/lib/hookshot/sysroot", "-O2"], b"deps")
        finally:
            os.remove(name)
        self.assertEqual(one, two)

    def test_the_folder_launcher_starts_the_builder(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-folder-") as tmp:
            root = Path(tmp)
            game = root / "game"
            (game / "bin").mkdir(parents=True)
            (game / "image.bin").write_bytes(b"image")
            started = root / "started"
            builder = root / "builder"
            builder.write_text("#!/bin/sh\nprintf '%%s\\n' \"$0\" \"$@\" > '%s'\n" % started, encoding="utf-8")
            builder.chmod(0o755)
            (game / "hookshot_version.txt").write_text("version=v1\nbuilder=%s\n" % builder, encoding="utf-8")
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder" / "linux" / "hookshot-folder", script)
            script.chmod(0o755)
            env = os.environ.copy()
            env["HOOKSHOT_BUILDER_HOME"] = str(root / "none")
            ran = subprocess.run([str(script)], env=env, check=False, text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            lines = started.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[1], "--launch")
            self.assertEqual(lines[2], str(game))

    def test_the_folder_launcher_starts_the_game_when_no_builder_is_installed(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-folder-") as tmp:
            root = Path(tmp)
            game = root / "game"
            (game / "bin").mkdir(parents=True)
            (game / "bin" / "hookshot").write_text("#!/bin/sh\nprintf 'game\\n'\n", encoding="utf-8")
            (game / "bin" / "hookshot").chmod(0o755)
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder" / "linux" / "hookshot-folder", script)
            script.chmod(0o755)
            env = os.environ.copy()
            env["HOOKSHOT_BUILDER_HOME"] = str(root / "none")
            ran = subprocess.run([str(script)], env=env, check=False, text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            self.assertEqual(ran.stdout.strip(), "game")


if __name__ == "__main__":
    unittest.main()
