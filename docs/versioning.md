# Versioning and releases

Hookshot follows [Semantic Versioning 2.0.0](https://semver.org/). Releases are made with
[release-please](https://github.com/googleapis/release-please): the version and the changelog come from the
[Conventional Commit](https://www.conventionalcommits.org/) titles of the pull requests merged into `main`.

The git tag is the source of truth. The game reads it through `nx2/version.py`, and the builder and the AppImage are
stamped from the same tag when they are packaged. `.release-please-manifest.json` holds the last released version for
release-please; it is changed only by the release pull request.

## The version number

`MAJOR.MINOR.PATCH`, with an optional prerelease suffix: `2.0.7`, `2.1.0`, `2.1.0-rc.1`. A release is tagged `v2.0.7`:
the `v` belongs to the tag, not to the version. Build metadata is not used in tags.

One version covers the whole project: the game (`hookshot.exe` / `hookshot`), the Vulkan back end, the lift pipeline
and its scripts, Hookshot Builder and the Linux AppImage. They are built from the same commit and released together,
so they never carry different versions.

The "public" surface the rules protect is the command line of `make_game.py` and the builder, the `RX_` and `HOS_`
switches, the files a build writes into a game folder (`hookshot_options.ini`, `hookshot_env.txt`, `language.txt`,
`hookshot_version.txt`), saves, and the format of `hookshot_log.txt`. The C sources and internal headers are not an
API and may change in any release.

## Pull request titles decide the version

Pull requests are squash-merged, so each one becomes a single commit on `main` whose subject is the pull request's
title. The *PR title* workflow (`.github/workflows/pr-title.yml`) checks that the title is a Conventional Commit:

```
type(scope): description
type(scope)!: description        (a breaking change)
```

`scope` is optional. A `!` before the colon, or `BREAKING CHANGE:` in the commit body, marks a breaking change. The
description becomes the changelog line. The allowed types and examples are in
[CONTRIBUTING.md](../CONTRIBUTING.md#pull-request-titles).

| Commit | Next version | Changelog section |
|---|---|---|
| any commit with `!` or `BREAKING CHANGE:` | major (`2.0.7` -> `3.0.0`) | its own section, and its type's |
| `feat` | minor (`2.0.7` -> `2.1.0`) | Features |
| `fix` | patch (`2.0.7` -> `2.0.8`) | Bug Fixes |
| `perf` | patch | Performance Improvements |
| `revert` | patch | Reverts |
| `refactor`, `docs`, `test`, `ci`, `build`, `chore` | no release | not listed |

A release takes the highest bump among its commits. A commit whose subject is not a Conventional Commit (a merge
commit, or a pull request merged without squashing) is not read, so its change makes no release and is not in the
changelog.

To choose the version yourself, put a `Release-As:` footer in the body of a commit on `main` (the squash-merge's
description works):

```
fix(vk): keep the swapchain when the window is moved

Release-As: 2.1.0
```

## How a release is made

The [Release workflow](../.github/workflows/release.yml) runs on every push to `main`:

1. release-please reads the commits since the last release. If any of them makes a release, it opens a pull request
   titled `chore: release <version>` (or updates the open one), which changes `.release-please-manifest.json` and puts
   the new entry at the top of `CHANGELOG.md`. The entry can be edited in that pull request before it is merged.
2. Merging the release pull request (squash or not; it is the one exception) is the release. On that push,
   release-please tags the merge commit `v<version>` and makes the GitHub release as a draft, with the changelog
   entry as its notes.
3. The same workflow calls the Linux AppImage and Windows builder workflows on the tag. Their release jobs attach
   the files with `scripts/release_upload.sh`: `Hookshot-v<version>-x86_64.AppImage` and `hookshot-linux.txt`;
   `HookshotBuilder.exe` (the setup) and `hookshot-setup.txt`. New update parts go to the `linux-parts` and
   `builder-parts` releases.
4. Once all four files are there, the draft is published and becomes "latest". Players' **Update** and the setup read
   the latest release's `hookshot-setup.txt`, so a release is never visible without its files.

If a build fails, the release stays a draft: re-run the failed jobs of that Release run. Files already on the release
are replaced, so a re-run never fails because of them. With the repository variable `CI_RELEASE_WINDOWS` set to
`false`, the Windows files are left for you to upload, and the draft for you to publish.

The release is made with the workflow's `GITHUB_TOKEN`, and the builds are called from the same workflow, so no
personal access token is needed. A pull request opened by `GITHUB_TOKEN` does not start other workflows, though, so
the release pull request has no checks of its own: it changes only the changelog and the manifest.

## Pre-releases

A release candidate (`2.1.0-rc.1`) is a prerelease on GitHub and never "latest". There are two ways to make one:

- Push a tag by hand: `git tag v2.1.0-rc.1 <commit>` and `git push origin v2.1.0-rc.1`. The tag starts the Linux
  AppImage and Windows builder workflows, which make the prerelease, with GitHub's generated notes, and attach the
  files. release-please is not involved, and the next release pull request does not change.
- Or put `Release-As: 2.1.0-rc.1` in a commit on `main`, and merge the release pull request. Every later release pull
  request then proposes another `-rc` version, so finish the series with `Release-As: 2.1.0`.

The first way is simpler, and it is how `v2.0.7-rc.1` was made.

## Hotfixes

Fix on `main` with a `fix:` pull request, and merge the release pull request. The project does not keep maintenance
branches for older versions: only the newest release is supported. A tag pushed by hand on another branch is built
and released the same way as a release candidate, but release-please only follows `main`. Merge that commit into
`main` afterwards, or the next release pull request does not know about the tag.

## Where the version appears

| Place | Where it comes from |
|---|---|
| git tag | release-please, or a tag pushed by hand; the source of truth |
| `.release-please-manifest.json` | the release pull request |
| `CHANGELOG.md` | the release pull request |
| `hookshot_log.txt` and the Insert menu | `nx2/version.py`: `HOOKSHOT_VERSION`, then `VERSION`/`version.txt`, then `git describe --tags --match "v[0-9]*"` |
| `hookshot_version.txt` in a build | the `VERSION` file written when the tools are packaged (`nx2/make_game.py`) |
| AppImage file name | `git describe --tags --match "v[0-9]*"` (`scripts/linux/appimage.sh`) |
| `HookshotBuilder.exe` | the MSBuild `Version` passed at publish time (`dotnet publish -p:Version=<version>`, the Windows builder workflow); the `csproj`'s `0.1.0` is only the default for a local build |

`nx2/version.py` also appends a fingerprint of the source files, `(source 1a2b3c4d)`. It is not a version: it tells
two builds of the same release apart, and it is the only identifier a build from a modified checkout has.

## The changelog

`CHANGELOG.md` is written by release-please, newest first, one section per release. Do not rewrite a released entry;
a correction is a new pull request and a new release. `changelog.txt` is the earlier changelog in Factorio's format,
written by hand for `0.1.0`. It is kept for history and is no longer updated.

## History

Tags from before versioning (`Release`, `amd`, `text-hotfix`, and so on) are snapshots of the old workflow. They stay
in the repository for history but are not releases: the tooling matches only `v[0-9]*` tags.

## Rules

- Do not edit a version number in a file, move a tag or reuse a version. The release pull request changes the
  manifest; nothing else does.
- Do not put game files, keys, builds or anything made from a dump in a release. `scripts/release_upload.sh` refuses
  file names that look like them, or like NVIDIA's runtime files.
