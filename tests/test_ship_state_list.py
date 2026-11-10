"""The pipeline state list a release ships (work/pipeline_states.bin): the repository's copy reads, and every release path
carries it. v2.0.8 to v2.0.14 shipped none (their app parts' tools/work and the Linux tools part's work/ held only the play
profile), and a new install's first field load was drawn white for seconds while its pipelines compiled in play.

  - the committed list is a list (nx2/pipeline_states.py reads it) and holds pipelines;
  - scripts/linux/appimage.sh copies it into the AppImage's tree, and stops without it;
  - scripts/release_parts.py package puts it in the Windows builder's tools/work and stops without it, and the earlier
    release's profile (release_parts.py profile) never replaces it.

Game-free: the list holds shader hashes and draw state, and the rest is synthetic.
    python -I -B tests/test_ship_state_list.py
"""
import argparse
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIST = ROOT / "work" / "pipeline_states.bin"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class ShippedList(unittest.TestCase):
    def setUp(self):
        self.tool = load("pipeline_states", ROOT / "nx2" / "pipeline_states.py")
        self.tmp = Path(tempfile.mkdtemp(prefix="hookshot-ship-states-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fake_list(self, n):
        return self.tool.MAGIC + struct.pack("<I", self.tool.SIZE) + b"".join(
            struct.pack("<Q", i + 1) + bytes(self.tool.SIZE - 8) for i in range(n))

    def test_committed_list_reads(self):
        self.assertTrue(LIST.is_file(), "work/pipeline_states.bin is not in the repository")
        recs = self.tool.read(str(LIST))
        self.assertGreater(len(recs), 0, "work/pipeline_states.bin holds no pipelines")
        self.assertEqual(LIST.stat().st_size, 12 + len(recs) * self.tool.SIZE, "a partial record at the end")
        self.assertIn("!work/pipeline_states.bin", (ROOT / ".gitignore").read_text(encoding="utf-8"))

    @unittest.skipUnless(sys.platform.startswith("linux"), "the AppImage is made on Linux")
    def test_appimage_carries_it(self):
        script = (ROOT / "scripts/linux/appimage.sh").read_text(encoding="utf-8")
        start = script.index("if [ -d work ]; then")
        end = script.index("\n", script.index('cp work/pipeline_states.bin "$T/work/"', start)) + 1
        block = script[start:end]

        def run(cwd):
            out = self.tmp / ("tree-" + cwd.name)
            out.mkdir()
            r = subprocess.run(["bash", "-eu", "-o", "pipefail", "-c", block], cwd=cwd, env=dict(os.environ, T=str(out)),
                               capture_output=True, text=True)
            return r, out

        r, out = run(ROOT)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((out / "work" / "pipeline_states.bin").read_bytes(), LIST.read_bytes())
        empty = self.tmp / "checkout"
        (empty / "work").mkdir(parents=True)
        r, _ = run(empty)
        self.assertNotEqual(r.returncode, 0, "appimage.sh went on without work/pipeline_states.bin")
        self.assertIn("work/pipeline_states.bin is missing", r.stdout + r.stderr)

    def release_parts(self, with_list):
        """release_parts.py with its ROOT a small checkout: nx2/pipeline_states.py and, when with_list, a work/ list"""
        root = self.tmp / "repo"
        (root / "nx2").mkdir(parents=True)
        (root / "work").mkdir()
        shutil.copy(ROOT / "nx2" / "pipeline_states.py", root / "nx2")
        (root / "LICENSE").write_text("license")
        if with_list:
            (root / "work" / "pipeline_states.bin").write_bytes(self.fake_list(3))
        rp = load("release_parts_under_test", ROOT / "scripts" / "release_parts.py")
        rp.ROOT = str(root)
        rp.tracked = lambda *paths: []   # (git ls-files: none of the sources are needed here)
        return rp, root

    def package(self, rp, setup_list=None):
        publish = self.tmp / "publish"
        publish.mkdir(exist_ok=True)
        (publish / "HookshotBuilder.exe").write_bytes(b"MZ")
        setup = self.tmp / "setup.exe"
        setup.write_bytes(b"MZ")
        out = self.tmp / "package"
        a = argparse.Namespace(out=str(out), publish=str(publish), setup=str(setup), list=setup_list, python_packages=None)
        return rp.package(a), out

    def test_windows_package_carries_it(self):
        rp, root = self.release_parts(True)
        # (the earlier release's app part: its profile, and an older list that must not replace the repository's)
        app = self.tmp / "HookshotBuilder-app-0.zip"
        with zipfile.ZipFile(app, "w") as z:
            z.writestr("HookshotBuilder/tools/work/hot_funcs.txt", "hot")
            z.writestr("HookshotBuilder/tools/work/pipeline_states.bin", self.fake_list(1))
        rp.named_part = lambda list_path, name: str(app) if name == "app" else None
        rc, out = self.package(rp, setup_list="setup.txt")
        self.assertEqual(rc, 0)
        work = out / "tools" / "work"
        self.assertEqual((work / "hot_funcs.txt").read_text(), "hot")
        self.assertEqual((work / "pipeline_states.bin").read_bytes(), (root / "work" / "pipeline_states.bin").read_bytes(),
                         "the package's list is not the repository's")
        # (the workflows' "Play profile" step extracts into the checkout's work/ too: the repository's list stays)
        rp.profile(argparse.Namespace(list="setup.txt", work=str(root / "work")))
        self.assertEqual(len(self.tool.read(str(root / "work" / "pipeline_states.bin"))), 3)
        self.assertEqual((root / "work" / "hot_funcs.txt").read_text(), "hot")

    def test_windows_package_stops_without_it(self):
        rp, _ = self.release_parts(False)
        rc, _ = self.package(rp)
        self.assertEqual(rc, 1, "the package was made without work/pipeline_states.bin")


if __name__ == "__main__":
    unittest.main()
