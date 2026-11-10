# Notes for AI coding agents

These are the points of [CONTRIBUTING.md](CONTRIBUTING.md) an agent most often gets wrong. CONTRIBUTING.md is the full
set of rules and wins where the two differ; the [development guide](docs/development.md) and the
[design notes](docs/design.md) explain the pipeline and the architecture.

## Never

- Commit, upload or paste game files, keys or anything made from them: dumps, `romfs`/`exefs`, `work/`, build output
  (`hookshot.exe`, `hookshot_vk.dll`, `hookshot_programs.bin`, pipeline caches), saves, `hookshot_options.ini`,
  `hookshot_env.txt`. Check `git status` before every commit. Screenshots and `hookshot_log.txt` are fine.
- Edit `third_party/`. Add a new folder with a `PROVENANCE.md` instead.
- Make the GPU replay read live guest memory ([design notes, section 4](docs/design.md#4-the-lessons-from-mkw-as-rules-here)).

## Changes

- One topic per pull request. No drive-by refactors or reformatting of code you did not need to touch.
- Match the style of the file you edit. New renderer switches use `RX_`, runtime switches `HOS_`; they are off by
  default and free while off.
- Python runs with `-I`, and tests use the standard library only.
- Add or extend a test in `tests/` for a fix, and run `python -I -B tests/run_all.py` before you open the PR.

## Commits

- Conventional Commit subjects (`feat:`, `fix:`, `docs:`, `test:`, `ci:` ...): they decide the next release
  ([docs/versioning.md](docs/versioning.md)).
- Record the assistance with `Assisted-by:` and `Co-Authored-By:` trailers, as CONTRIBUTING.md asks.

## Pull request descriptions

- **Open with why the PR is needed**, in one or two plain sentences a player could follow: the bug it fixes, the
  feature it adds or the cost it removes. Not a list of files, and not how it works.
- Then what changed, how it was tested (GPU, driver, game area, fps before and after; the commands run) and the
  issue it fixes.
- **Include screenshots when the change shows on screen**: graphics, the menu, the start-up screen, overlays,
  messages. Before and after for a fix, the new screen for a feature, at the sizes or settings you checked.
- Say plainly what was not tested (a GPU vendor, Windows, a game area) rather than leaving it out.
- Keep it short and in plain words.
