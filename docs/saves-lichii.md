# One save in both Hookshot and Lichii

[Hookshot](https://github.com/NotRexed/Hookshot) (the native recompilation) and
[Lichii](https://github.com/zortos293/Lichii) (a Switch 2 emulator, an Eden fork) both run Ocarina of Time on the same
PC, and they use the **same save files**. You can play in one, copy the save across and carry on in the other at the
same spot: handy to compare how the two look, or to use whichever runs better that day. Lichii keeps its data in
Eden's folder, so this works for Eden too.

You need your own copy of the game, and for Lichii the keys and firmware from your own console. Nothing here comes
with game files.

## Where the saves are

| | Linux | Windows |
|---|---|---|
| Hookshot | `~/.local/share/hookshot/save/<user id>/` (`$XDG_DATA_HOME/hookshot` when it is set) | `<game folder>\user\save\<user id>\` |
| Lichii / Eden | `~/.local/share/eden/nand/user/save/0000000000000000/<profile id>/0400486024DEC000/` | `%APPDATA%\eden\nand\user\save\0000000000000000\<profile id>\0400486024DEC000\` |

- Hookshot's `<user id>` is one folder with a 32-character hex name. The Linux folder is the same one the AppImage and
  Steam Deck (SteamOS) builds use. `--user <folder>` puts it somewhere else
  ([command line arguments](../readme.md#running-the-game-from-the-command-line)): then `<folder>\save\<user id>\`.
- Lichii's `<profile id>` is the Switch user profile you play with. `0400486024DEC000` is the game's title id.
- Lichii and Eden use `user\` beside the program (Linux: in the folder you start it from) instead of the paths above
  when that folder exists (their "portable" mode). Look inside it for `nand\user\save\...`.
- The Lichii/Eden paths come from Lichii's source (`src/common/fs/path_util.cpp`, `savedata_factory.cpp`). On Windows
  they have not been tried here.

Both hold the same files:

```
account.sav
option.sav            (the game's own settings)
slot_00/ ... slot_NN/ (one per save slot)
  progress.sav
  caption.sav
```

They are byte for byte the same format, so nothing is converted: the files are copied across.

## Copy with the script

`scripts/oot_saves.py` (Python 3, nothing to install) finds both folders and copies the slots, `account.sav` and
`option.sav` one way. It backs up the side it overwrites first. From the Hookshot folder (or a clone):

```sh
python -I scripts/oot_saves.py list          # both folders, their slots and when each was saved
python -I scripts/oot_saves.py to-lichii     # Hookshot -> Lichii
python -I scripts/oot_saves.py to-hookshot   # Lichii -> Hookshot
```

On Windows Hookshot's folder is your game folder, which the script cannot guess, so name it:

```powershell
python -I scripts\oot_saves.py to-lichii --hookshot "C:\Games\Hookshot"
python -I scripts\oot_saves.py to-hookshot --hookshot "C:\Games\Hookshot"
```

| Option | What it does |
|---|---|
| `--dry-run` | Says what it would copy and where, and changes nothing |
| `--keep-settings` | Leaves the target's `option.sav` alone, so each game keeps its own settings |
| `--profile <id>` | The Lichii profile to use. Needed when more than one profile has OoT saves: the script lists them and stops rather than pick one |
| `--hookshot <folder>` | Hookshot's data folder (Windows: the game folder), or its `save\<user id>` folder |
| `--lichii <folder>` | Lichii's or Eden's data folder (for example a portable `user` folder), or the `0400486024DEC000` folder |
| `--backup-dir <folder>` | Where the backup goes |
| `--force` | Copies even if Hookshot or Lichii seems to be running |

The script refuses to run while it sees Hookshot, Lichii or Eden running. The side being overwritten is copied to
`oot-save-backups/<to-lichii or to-hookshot>-<date>-<time>/` first: in `~/.local/share` on Linux, in
`%LOCALAPPDATA%\Hookshot` on Windows. To undo a copy, put the backup's contents back.

## Copy by hand

Close both games, then copy the **contents** of the folder you are leaving into the folder you are going to (see the
table above). Keep a copy of the target first. Delete the target's old `slot_*` folders before copying, so a slot that
only exists on that side does not stay mixed in with the new save.

## Before you copy

- **Close both games first.** A game that is running can write its save again on exit and undo the copy.
- **Never run both on the same save at the same time.** Pointing both at one folder with a symlink may work, since the
  files are the same, but it has not been tried.
- **Settings follow the save.** `option.sav` holds the game's own settings (camera, text speed, ...) and is copied too.
  Use `--keep-settings` (or skip that file by hand) if you want different settings in each.
- **Profiles.** If your Lichii or Eden has several user profiles, make sure you copy to and from the one you play on.
- **Backups.** The script keeps one for every copy. Delete old ones when you no longer need them.
- Start each game once and save in it before the first copy, so that its save folder exists.

## What was tested

- Linux (Arch-based), Hookshot v2.0.5 and Lichii built from source on the same day.
- A Hookshot save copied to Lichii loads in Lichii at the same spot (Kokiri Forest, the spawn by the flower patch).
- A Lichii save copied to Hookshot arrives identical, but has not been played through on the Hookshot side.
- The script's copies and backups are covered by `tests/test_oot_saves.py`, on temporary folders.
- **Not tested:** Windows (the paths above come from Hookshot's and Lichii's source), and the script running against
  a real Windows install.
