"""Copy Ocarina of Time saves between Hookshot and Lichii (or Eden): the two keep the same files, so a copy is all it takes.

    python -I scripts/oot_saves.py list
    python -I scripts/oot_saves.py to-lichii    [--profile ID] [--keep-settings] [--dry-run] [--force]
    python -I scripts/oot_saves.py to-hookshot  [--profile ID] [--keep-settings] [--dry-run] [--force]

--hookshot DIR and --lichii DIR name the folders when they are not where they are looked for: Hookshot's data folder
(Linux: $XDG_DATA_HOME/hookshot, default ~/.local/share/hookshot) or its game folder on Windows (the one with image.bin
and user\\), and Lichii's or Eden's data folder (Linux: $XDG_DATA_HOME/eden, Windows: %APPDATA%\\eden). A folder deeper
in (the save/<user id> folder, or the 0400486024DEC000 folder) is accepted too. The side being overwritten is copied first
to <backup>/<direction>-<YYYYmmdd-HHMMSS> (Linux: $XDG_DATA_HOME/oot-save-backups, Windows:
%LOCALAPPDATA%\\Hookshot\\oot-save-backups; --backup-dir changes it). Close both games first: --force skips that check.
Standard library only; the guide is docs/saves-lichii.md.
"""
import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys

TITLE = "0400486024DEC000"
NO_ACCOUNT = "0000000000000000"
HEX32 = re.compile(r"^[0-9a-fA-F]{32}$")
GAMES = {"hookshot": "Hookshot", "lichii": "Lichii", "eden": "Eden"}   # (process names, lower case, without .exe)
DIRECTIONS = {"to-lichii": ("hookshot", "lichii"), "to-hookshot": ("lichii", "hookshot")}
WINDOWS = os.name == "nt"


class Fail(Exception):
    pass


def data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")


def default_backup_dir():
    if WINDOWS:
        base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
        return os.path.join(base, "Hookshot", "oot-save-backups")
    return os.path.join(data_home(), "oot-save-backups")


def subdirs(path):
    try:
        return sorted(n for n in os.listdir(path) if os.path.isdir(os.path.join(path, n)))
    except OSError:
        return []


def is_save_folder(path):
    return os.path.isfile(os.path.join(path, "account.sav")) or any(n.startswith("slot_") for n in subdirs(path))


def slots(path):
    return [n for n in subdirs(path) if n.startswith("slot_")]


def find_hookshot(root):
    """The Hookshot save folders under root, as {user id: path}. root may be the folder itself, its save folder, the
    user folder, the data folder or (Windows) the game folder."""
    if root is None:
        if WINDOWS:
            raise Fail("Hookshot's saves are in the user folder of your game folder on Windows: pass the game folder "
                       "with --hookshot (for example --hookshot C:\\Games\\Hookshot)")
        root = os.path.join(data_home(), "hookshot")
    if not os.path.isdir(root):
        raise Fail("no Hookshot folder at %s (start Hookshot once and save, or pass --hookshot)" % root)
    if HEX32.match(os.path.basename(os.path.normpath(root))) and is_save_folder(root):
        return {os.path.basename(os.path.normpath(root)): root}
    for rel in ((), ("save",), ("user", "save"), ("hookshot", "save")):
        base = os.path.join(root, *rel)
        found = {n: os.path.join(base, n) for n in subdirs(base) if HEX32.match(n)}
        if found:
            return found
    raise Fail("no Hookshot save folder (a 32-character user id folder) under %s: start Hookshot once and save, or "
               "pass --hookshot" % root)


def find_lichii(root):
    """The Lichii/Eden folders holding OoT saves under root, as {profile id: path}."""
    if root is None:
        if WINDOWS:
            appdata = os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Roaming")
            root = os.path.join(appdata, "eden")
        else:
            root = os.path.join(data_home(), "eden")
    if not os.path.isdir(root):
        raise Fail("no Lichii/Eden folder at %s (start the game in Lichii once and save, or pass --lichii)" % root)
    norm = os.path.normpath(root)
    if os.path.basename(norm).upper() == TITLE:
        return {os.path.basename(os.path.dirname(norm)): norm}
    bases = [os.path.join(root, *rel) for rel in
             (("nand", "user", "save", NO_ACCOUNT), ("user", "save", NO_ACCOUNT), ("save", NO_ACCOUNT), (NO_ACCOUNT,), ())]
    found = {}
    for base in bases:
        for profile in subdirs(base):
            for n in subdirs(os.path.join(base, profile)):
                if n.upper() == TITLE:
                    found[profile] = os.path.join(base, profile, n)
        if found:
            return found
    raise Fail("no Ocarina of Time save under %s (%s): start the game in Lichii once and save, or pass --lichii"
               % (root, TITLE))


def pick(found, name, flag, wanted=None):
    if wanted is not None:
        for k, v in found.items():
            if k.lower() == wanted.lower():
                return k, v
        raise Fail("%s has no profile %s; it has: %s" % (name, wanted, ", ".join(sorted(found))))
    if len(found) > 1:
        lines = "".join("\n  %s  %s" % (k, found[k]) for k in sorted(found))
        raise Fail("%s has OoT saves in %d profiles, so pick one with %s:%s" % (name, len(found), flag, lines))
    return next(iter(found.items()))


def running_games():
    """Names of Hookshot, Lichii or Eden processes that are running."""
    names = set()
    if WINDOWS:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            image = line.split('","')[0].strip('"').lower()
            if image.endswith(".exe") and image[:-4] in GAMES:
                names.add(GAMES[image[:-4]])
    else:
        for pid in filter(str.isdigit, os.listdir("/proc")):
            try:
                with open("/proc/%s/cmdline" % pid, "rb") as f:
                    argv0 = f.read().split(b"\0")[0].decode("utf-8", "replace")
                with open("/proc/%s/comm" % pid, encoding="utf-8", errors="replace") as f:
                    comm = f.read().strip()
            except OSError:
                continue
            for candidate in (comm, os.path.basename(argv0)):
                low = candidate.lower()
                if low in GAMES:
                    names.add(GAMES[low])
                elif low.endswith(".appimage") and re.match(r"(lichii|eden)[-_.]", low):
                    names.add(GAMES["eden" if low.startswith("eden") else "lichii"])
    return sorted(names)


def describe(path):
    out = []
    for s in slots(path):
        p = os.path.join(path, s, "progress.sav")
        if os.path.isfile(p):
            when = datetime.datetime.fromtimestamp(os.path.getmtime(p)).strftime("%Y-%m-%d %H:%M")
            out.append("%s (saved %s)" % (s, when))
        else:
            out.append(s)
    return out


def show(name, found):
    print("%s:" % name)
    for k in sorted(found):
        print("  %s  %s" % (k, found[k]))
        for line in describe(found[k]) or ["(no slots)"]:
            print("      " + line)


def copy_saves(src, dst, backup, keep_settings, dry_run):
    names = slots(src)
    if not names:
        raise Fail("%s has no slot_* folders: nothing to copy" % src)
    files = [f for f in ("account.sav", "option.sav") if os.path.isfile(os.path.join(src, f))]
    if keep_settings and "option.sav" in files:
        files.remove("option.sav")
    print("%s %s -> %s" % ("Would back up" if dry_run else "Backing up", dst, backup))
    print("%s %d slot(s) (%s) and %s from %s to %s" % ("Would copy" if dry_run else "Copying", len(names), ", ".join(names),
          ", ".join(files) or "no loose files", src, dst))
    if keep_settings:
        print("option.sav kept as it is on the target")
    if dry_run:
        print("Dry run: nothing was changed")
        return
    os.makedirs(os.path.dirname(backup), exist_ok=True)
    shutil.copytree(dst, backup)
    for s in slots(dst):
        shutil.rmtree(os.path.join(dst, s))
    for s in names:
        shutil.copytree(os.path.join(src, s), os.path.join(dst, s))
    for f in files:
        shutil.copy2(os.path.join(src, f), os.path.join(dst, f))
    print("Done. Backup of the old %s in %s" % (dst, backup))


def run(args):
    hs = find_hookshot(args.hookshot)
    li = find_lichii(args.lichii)
    if args.command == "list":
        show("Hookshot", hs)
        show("Lichii", li)
        return
    hs_id, hs_path = pick(hs, "Hookshot", "--hookshot <the user id folder>")
    li_id, li_path = pick(li, "Lichii", "--profile <id>", args.profile)
    paths = {"hookshot": hs_path, "lichii": li_path}
    src, dst = (paths[side] for side in DIRECTIONS[args.command])
    if not args.dry_run and not args.force:
        busy = running_games()
        if busy:
            raise Fail("close %s first (or pass --force): a running game can write its save again and undo the copy"
                       % " and ".join(busy))
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = os.path.join(args.backup_dir or default_backup_dir(), "%s-%s" % (args.command, stamp))
    copy_saves(src, dst, backup, args.keep_settings, args.dry_run)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], prog="oot_saves.py")
    p.add_argument("command", choices=["list", "to-lichii", "to-hookshot"])
    p.add_argument("--hookshot", metavar="DIR", help="Hookshot's data folder (Windows: the game folder)")
    p.add_argument("--lichii", metavar="DIR", help="Lichii's or Eden's data folder")
    p.add_argument("--profile", metavar="ID", help="the Lichii profile id, when several have OoT saves")
    p.add_argument("--backup-dir", metavar="DIR", help="where the backup of the overwritten side goes")
    p.add_argument("--keep-settings", action="store_true", help="leave the target's option.sav (the game's settings) alone")
    p.add_argument("--dry-run", action="store_true", help="say what would happen and change nothing")
    p.add_argument("--force", action="store_true", help="copy even if Hookshot or Lichii seems to be running")
    args = p.parse_args(argv)
    try:
        run(args)
    except Fail as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
