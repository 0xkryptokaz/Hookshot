"""Exercise Linux Builder's detached Play path with an authored executable."""
import ast
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace

RUN_ALL_SKIP = {"windows": "uses the Linux Builder's /bin/sh launch path"}

BUILDER = Path(__file__).resolve().parents[1] / "builder/linux"


def main():
    if os.name != "posix":
        return
    path = BUILDER / "hookshot_builder.py"
    play = next(node for node in ast.parse(path.read_text(encoding="utf-8")).body
                if isinstance(node, ast.FunctionDef) and node.name == "play")
    processes = []

    def launch(command, **options):
        assert options["start_new_session"]
        processes.append(subprocess.Popen(command, **options))

    namespace = dict(HERE=str(BUILDER), os=os,
                     linux_update=SimpleNamespace(game_exe=lambda folder: str(Path(folder) / "hookshot")),
                     make_game=SimpleNamespace(game_env=lambda: dict(os.environ)),
                     subprocess=SimpleNamespace(Popen=launch, DEVNULL=subprocess.DEVNULL))
    exec(compile(ast.Module(body=[play], type_ignores=[]), str(path), "exec"), namespace)

    with tempfile.TemporaryDirectory(prefix="hookshot-play-") as directory:
        folder = Path(directory)
        game = folder / "hookshot"
        game.write_text(
            '#!/bin/sh\n'
            'printf "stdout sentinel\\n"\n'
            'printf "stderr sentinel\\n" >&2\n'
            'head -c 1048576 /dev/zero >&2\n'
            'touch completed\n'
            'exit 73\n', encoding="utf-8")
        game.chmod(0o700)
        for _ in range(2):
            namespace["play"](str(folder))
        for process in processes:
            assert process.wait(timeout=10) == 0
        log = folder / "hookshot_startup.log"
        data = log.read_bytes()
        assert data == b"stderr sentinel\n" + bytes(65536 - len(b"stderr sentinel\n")) + b"\nHookshot exited with status 73\n"
        assert (folder / "completed").is_file() and log.stat().st_mode & 0o777 == 0o600
        assert not list(folder.glob(".hookshot-startup.*"))

        # A non-writable destination must not prevent the game from running.
        log.unlink()
        log.mkdir()
        (folder / "completed").unlink()
        namespace["play"](str(folder))
        processes[-1].wait(timeout=10)
        assert (folder / "completed").is_file() and not list(log.iterdir())
        assert not list(folder.glob(".hookshot-startup.*"))
    print("PASS: detached Play, bounded stderr, exit status, concurrent launches and publication failure")


if __name__ == "__main__":
    main()
