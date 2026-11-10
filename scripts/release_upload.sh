#!/bin/bash
# A tag's GitHub release with these files attached. The Linux AppImage and Windows builder workflows both call it. For
# a release from the Release workflow, release-please has made it already, as a draft with its notes, and the files are
# only added. For a tag pushed by hand, the first to get there makes the release (from the tag, with GitHub's generated
# notes) and the other adds its files to it, so the two never race to make two releases. A version with a prerelease
# part (v0.2.0-rc.1) is a prerelease and never "latest".
# A file already on the release is replaced, so a run again on the same tag updates it. A release that is not a draft
# then gets its SHA256SUMS and the table of its files (scripts/release_files.sh finish), SHA256SUMS also written to
# $RELEASE_SUMS_OUT when that is set. Needs gh, with GH_TOKEN (contents: write) and GH_REPO set. No game data is
# involved: a file whose name looks like the game's or a dump's, or like NVIDIA's runtime files, is refused.
#
#   scripts/release_upload.sh <tag> <file> ...
set -euo pipefail
tag=$1
shift
for f in "$@"; do
    name=$(basename "$f")
    shopt -s nocasematch
    case $name in
        *.nso|*.nca|*.nsp|*.xci|*.nsz|*.xcz|*.nup|*.keys|*.bin|*romfs*|*exefs*|*nvngx*|*dlss*|*streamline*|sl.*.dll)
            echo "release_upload.sh: $name is not a release file (game data or NVIDIA files never go on a release)" >&2
            exit 1 ;;
    esac
    shopt -u nocasematch
done
if [[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+- ]]; then
    kind=(--prerelease --latest=false)
else
    kind=(--latest)
fi
if ! gh release view "$tag" >/dev/null 2>&1; then
    # (the other workflow can make it between the view and the create: its release is then the one used)
    gh release create "$tag" --verify-tag --title "$tag" --generate-notes "${kind[@]}" ||
        gh release view "$tag" >/dev/null
fi
gh release upload "$tag" --clobber "$@"
# (a draft is the Release workflow's: its publish job does this once both builds' files are there)
if [[ $(gh release view "$tag" --json isDraft --jq .isDraft) != true ]]; then
    "$(dirname "$0")/release_files.sh" finish "$tag" "${RELEASE_SUMS_OUT:-}"
fi
