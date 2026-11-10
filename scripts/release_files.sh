#!/bin/bash
# What a release gets once its files are on it: a "Which file do I download?" table under its notes, and SHA256SUMS,
# the SHA-256 of every other file on it in sha256sum's format (sha256sum -c --ignore-missing SHA256SUMS checks a
# download). The sums are GitHub's own digests of the uploaded files; a file without one is downloaded and hashed.
# The Release workflow's publish job runs this for a release before publishing it, and scripts/release_upload.sh for
# a tag pushed by hand (and the nightly). Needs gh, with GH_TOKEN (contents: write) and GH_REPO set.
#
#   scripts/release_files.sh table <tag>             the table, in Markdown
#   scripts/release_files.sh finish <tag> [<out>]    the table under the notes (once), and SHA256SUMS uploaded (also
#                                                    written to <out>). Two runs at once (the Linux and Windows release
#                                                    jobs of a tag) leave the sums of every file: each one checks the
#                                                    uploaded SHA256SUMS against the files after uploading it.
#   scripts/release_files.sh check <tag> <name> ...  each named file and SHA256SUMS are on the release, and SHA256SUMS
#                                                    lists every other file on it, with its digest
set -euo pipefail
MARK='<!-- release-files -->'

table() {
    local appimage="the \`.AppImage\`"
    [[ $1 =~ ^v[0-9] ]] && appimage="\`Hookshot-$1-x86_64.AppImage\`"
    cat <<EOF
$MARK
### Which file do I download?

| On | Download |
|---|---|
| Windows | \`HookshotBuilder.exe\`, the setup |
| Linux, Steam Deck | $appimage |

You need your own copy of the game. \`hookshot-setup.txt\`, \`hookshot-linux.txt\` and the \`.zsync\` are for the updaters. To check a download: \`sha256sum -c --ignore-missing SHA256SUMS\`, or \`gh attestation verify <file> -R $GH_REPO\`.
EOF
}

sums() {   # tag -> SHA256SUMS of the release's other files, sorted by name
    local name digest dir
    gh release view "$1" --json assets --jq '.assets[] | select(.name != "SHA256SUMS") | "\(.name)\t\(.digest // "")"' |
        LC_ALL=C sort | while IFS=$'\t' read -r name digest; do
            if [[ ! $digest =~ ^sha256:[0-9a-f]{64}$ ]]; then
                dir=$(mktemp -d)
                gh release download "$1" --pattern "$name" --dir "$dir"
                digest=sha256:$(sha256sum "$dir/$name" | cut -d' ' -f1)
                rm -rf "$dir"
            fi
            printf '%s  %s\n' "${digest#sha256:}" "$name"
        done
}

uploaded() {   # tag -> the SHA256SUMS on the release (nothing when there is none)
    gh release download "$1" --pattern SHA256SUMS --output - 2>/dev/null || true
}

finish() {
    local tag=$1 out=${2:-} body dir i
    body=$(gh release view "$tag" --json body --jq .body)
    dir=$(mktemp -d)
    trap 'rm -rf "$dir"' RETURN
    if [[ $body != *"$MARK"* ]]; then
        { if [[ -n $body ]]; then printf '%s\n\n' "$body"; fi; table "$tag"; } >"$dir/notes.md"
        gh release edit "$tag" --notes-file "$dir/notes.md" >/dev/null
    fi
    for i in 1 2 3 4 5; do
        sums "$tag" >"$dir/SHA256SUMS"
        if [[ -s $dir/SHA256SUMS ]] && cmp -s "$dir/SHA256SUMS" <(uploaded "$tag"); then
            cat "$dir/SHA256SUMS"
            if [[ -n $out ]]; then cp "$dir/SHA256SUMS" "$out"; fi
            return 0
        fi
        (( i == 1 )) || sleep 5
        gh release upload "$tag" --clobber "$dir/SHA256SUMS"
    done
    echo "release_files.sh: $tag: SHA256SUMS still differs from the files after 5 uploads" >&2
    return 1
}

check() {
    local tag=$1 names want ok=0
    shift
    names=$(gh release view "$tag" --json assets --jq '.assets[].name')
    echo "$names"
    for want in "$@" SHA256SUMS; do
        grep -qxF "$want" <<<"$names" || { echo "::error::$tag has no $want"; ok=1; }
    done
    if (( ok == 0 )) && ! cmp -s <(sums "$tag") <(uploaded "$tag"); then
        echo "::error::$tag: SHA256SUMS does not list every file with its digest"
        ok=1
    fi
    return $ok
}

case ${1:-} in
    table) table "$2" ;;
    finish) finish "$2" "${3:-}" ;;
    check) shift; check "$@" ;;
    *) sed -n '8,14p' "$0" >&2; exit 2 ;;
esac
