"""A game folder from the user's own dump: what the builder app (builder/) runs.

    python -I nx2/make_game.py --nup <file.nup> --out <folder> [--work <folder>] [--jobs <n>]
    python -I nx2/make_game.py --romfs <dir> --exefs <dir> --out <folder> [--work <folder>] [--jobs <n>]
    python -I nx2/make_game.py --check --nup <file.nup>       (or --exefs <dir>: is this the right game and version?)

The folder holds the executable and all it needs, started with a double click: nothing goes back into the repository, and no game
data ever comes with the tools. Lines the app reads, each on its own line of stdout:

    @@stage <id> <text>      a step begins
    @@progress <0..1>        how far that step is
    @@check <ok|bad> <text>  (--check) the answer
    @@done <folder>          finished
    @@error <text>           stopped
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PY = [sys.executable, "-I"]
BUILD_ID = "e0f3ccfa"   # (the version the patches, the hot list and the profile were made from)
BESIDE = ["hookshot.exe", "hookshot_vk.dll", "SDL2.dll", "wgpu_native.dll"]
LANGUAGES = [("en-US", "English (US)"), ("en-GB", "English (UK)"), ("fr", "French (France)"), ("fr-CA", "French (Canada)"), ("de", "German"),
             ("it", "Italian"), ("es", "Spanish (Spain)"), ("es-419", "Spanish (Latin America)"), ("pt-BR", "Portuguese (Brazil)"), ("nl", "Dutch"),
             ("pl", "Polish"), ("ru", "Russian"), ("ja", "Japanese"), ("ko", "Korean"), ("zh-Hans", "Chinese (Simplified)"),
             ("zh-Hant", "Chinese (Traditional)")]


def say(*words):
    print("@@" + " ".join(str(w) for w in words), flush=True)


def fail(text):
    say("error", text)
    sys.exit(1)


def run(cmd, env=None, on_line=None):
    e = dict(os.environ, PYTHONIOENCODING="utf-8")
    e.update(env or {})
    p = subprocess.Popen(cmd, cwd=ROOT, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                         errors="replace", bufsize=1)
    tail = []
    for line in p.stdout:
        line = line.rstrip()
        print(line, flush=True)
        tail = (tail + [line])[-12:]
        if on_line:
            on_line(line)
    if p.wait():
        why = [l for l in tail if "error:" in l.lower() and "CalledProcessError" not in l]
        fail("%s stopped: %s" % (os.path.basename(cmd[2]), why[-1] if why else tail[-1] if tail else "no output"))


def build_id(main):
    """the module id of an NSO, plain or compressed: the header is the same"""
    with open(main, "rb") as f:
        head = f.read(0x60)
    if head[:4] != b"NSO0":
        fail("%s is not a game executable" % main)
    return head[0x40:0x60].hex()


def check(a, work):
    check_windows_sdk()   # (first: said in plain words when the game is chosen, not after it has been read)
    if a.nup:
        probe = os.path.join(work, "probe")
        shutil.rmtree(probe, ignore_errors=True)
        run(PY + ["nx2/nup.py", a.nup, "--extract", probe, "--only", "exefs"])
        bid = build_id(os.path.join(probe, "exefs", "main"))
        shutil.rmtree(probe, ignore_errors=True)
    else:
        bid = build_id(os.path.join(a.exefs, "main"))
    if bid.startswith(BUILD_ID):
        say("check", "ok", "This is the version Hookshot supports (build %s)." % bid[:8])
    else:
        say("check", "bad", "This is build %s. Hookshot supports build %s." % (bid[:8], BUILD_ID))


def check_windows_sdk():
    """The compiler is bundled; Microsoft's C library and Windows SDK may not be, by their licences. Found now, not 40 minutes in."""
    sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "tools"))
    import tempfile
    import toolchain
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.c")
        with open(src, "w") as f:
            f.write(chr(10).join(("#include <stdio.h>", "#include <windows.h>", "int main(void) { return 0; }", "")))
        r = subprocess.run([toolchain.need(toolchain.find("clang"))] + toolchain.native_target() + ["-c", src, "-o", os.path.join(d, "t.o")],
                           capture_output=True, text=True)
    if r.returncode:
        fail("The Visual Studio C++ tools and the Windows SDK are needed. Install Visual Studio Build Tools with \"Desktop development "
             "with C++\" from visualstudio.microsoft.com, then build again.")


def link_or_copy(src, dst):
    """the romfs by hard links where the disk allows: 26 GB not written twice"""
    files = [os.path.join(d, f) for d, _, fs in os.walk(src) for f in fs]
    for i, s in enumerate(files):
        t = os.path.join(dst, os.path.relpath(s, src))
        os.makedirs(os.path.dirname(t), exist_ok=True)
        if not os.path.exists(t):
            try:
                os.link(s, t)
            except OSError:
                shutil.copy2(s, t)
        if i % 500 == 0:
            say("progress", "%.3f" % (i / max(1, len(files))))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--nup")
    p.add_argument("--romfs")
    p.add_argument("--exefs")
    p.add_argument("--out")
    p.add_argument("--work", default=os.path.join(os.environ.get("LOCALAPPDATA", ROOT), "HookshotBuilder"))
    p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    p.add_argument("--language", default="en-US", help="the game's language: a code from LANGUAGES, written to language.txt")
    p.add_argument("--check", action="store_true")
    a = p.parse_args()
    os.makedirs(a.work, exist_ok=True)
    if a.check:
        return check(a, a.work)
    if not a.out or not (a.nup or (a.romfs and a.exefs)):
        fail("needs --out, and --nup or --romfs with --exefs")
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work)
    os.makedirs(out, exist_ok=True)
    check_windows_sdk()
    exefs = os.path.join(work, "exefs")
    romfs = os.path.join(out, "romfs")

    say("stage", "read", "Reading your game files")
    shutil.rmtree(exefs, ignore_errors=True)
    if a.nup:
        game = os.path.join(out, ".game")
        run(PY + ["nx2/nup.py", a.nup, "--extract", game])
        shutil.move(os.path.join(game, "exefs"), exefs)
        if not os.path.isdir(romfs):
            shutil.move(os.path.join(game, "romfs"), romfs)
        shutil.rmtree(game, ignore_errors=True)
    else:
        shutil.copytree(a.exefs, exefs)   # (unpacked in place: the user's copy stays as it is)
        run(PY + ["nx2/unpack.py", exefs])
        link_or_copy(a.romfs, romfs)
    bid = build_id(os.path.join(exefs, "main"))
    if not bid.startswith(BUILD_ID):
        fail("This is build %s. Hookshot supports build %s." % (bid[:8], BUILD_ID))

    # (made once on the development PC: lists of addresses and counts, no game code)
    for name in ("hot_funcs.txt", "hot_order.txt", "hookshot_pieces.prof", "hookshot.res"):
        src = os.path.join(ROOT, "work", name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(work, name))
    os.makedirs(os.path.join(work, "image"), exist_ok=True)
    src = os.path.join(ROOT, "work", "image", "chunks.txt")   # (where the lifted code's files begin: addresses)
    if os.path.exists(src):
        shutil.copy2(src, os.path.join(work, "image", "chunks.txt"))
    env = {"OOT_WORK": work, "OOT_EXEFS": exefs, "OOT_ROMFS": romfs, "OOT_DUMP": out, "RX_RELEASE": "1", "A64_HOT_CHUNK": "1"}
    if os.path.exists(os.path.join(work, "hot_funcs.txt")):
        env["A64_HOT_FUNCS"] = os.path.join(work, "hot_funcs.txt")
    if os.path.exists(os.path.join(work, "hookshot_pieces.prof")):
        env["RX_PGO"] = os.path.join(work, "hookshot_pieces.prof")

    say("stage", "lift", "Translating the game to PC code")
    run(PY + ["nx2/lift.py", "--standalone", "--write", "--exefs", exefs, "--work", work], env)
    run(PY + ["nx2/split_hot.py", os.path.join(work, "sa", "gen")], env)   # (eight files side by side: ~6 minutes, not 79)

    say("stage", "compile", "Compiling")
    done = {"n": 0, "of": 1}

    def compiled(line):
        m = re.match(r"(\d+) files, ", line)
        if m:
            done["of"] = int(m.group(1))
        elif re.match(r"\s+\S+\s+\d+ MB\s+\d+s", line):
            done["n"] += 1
            say("progress", "%.3f" % (done["n"] / done["of"]))
    run(PY + ["nx2/build.py", "--standalone", "--jobs", str(a.jobs)], env, compiled)

    say("stage", "finish", "Putting the game folder together")
    built = os.path.join(work, "sa", "build")
    for name in BESIDE:
        if os.path.exists(os.path.join(built, name)):
            shutil.copy2(os.path.join(built, name), os.path.join(out, name))
    shutil.copy2(os.path.join(work, "sa", "image", "image.bin"), os.path.join(out, "image.bin"))
    language = os.path.join(out, "language.txt")   # (the same text as host/nn_applet.c writes)
    with open(language, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The game's language. Change the line below to one of the codes listed here, save, and start the game again.\n\n%s\n\n" % a.language)
        f.writelines("# %-8s %s\n" % l for l in LANGUAGES)
    say("done", out)


if __name__ == "__main__":
    main()
