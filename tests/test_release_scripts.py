"""Run the release scripts (scripts/release_files.sh, release_upload.sh, release_promote.sh) against a stand-in gh.

No network: the stand-in keeps the repository's releases, tags and runs in a JSON file and answers the REST calls the
scripts make, a listing of releases with the drafts in it, as GitHub does for a token that can push. It checks what
v2.1.0's promotion got wrong: a release found by listing, drafts too, so a failed lookup is never "no release" and a
draft never gets a published release made beside it; the files go to the release by its id; Promote refuses a
version already published, a tag with two releases and a second run in progress; and the record is made once.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

# (Linux only: bash and jq)
RUN_ALL_SKIP = {"windows": "the release scripts are bash, run by Linux runners; exercised on Linux"}

ROOT = Path(__file__).resolve().parents[1]
REPO = "owner/repo"
COMMIT = "c" * 40

# The stand-in gh: `gh api` for the endpoints the scripts use, `gh release create` and `gh release view --json body`.
# --jq is run with the real jq. FAKE_GH_FAIL names calls that fail as GitHub's do now and then (HTTP 502).
STUB = r'''#!/usr/bin/env python3
import hashlib, json, os, subprocess, sys
from urllib.parse import urlsplit, parse_qs
STATE = os.environ["FAKE_GH_STATE"]
REPO = os.environ["GH_REPO"]
with open(STATE) as f:
    s = json.load(f)
def save():
    with open(STATE, "w") as f:
        json.dump(s, f)
def out(value, jq=None):
    text = json.dumps(value)
    if jq:
        text = subprocess.run(["jq", "-r", jq], input=text, capture_output=True, text=True, check=True).stdout
        sys.stdout.write(text)
    else:
        print(text)
def fail(code, message):
    save()
    sys.stderr.write("gh: %s (HTTP %d)\n" % (message, code))
    sys.exit(1)
def release(rid):
    for r in s["releases"]:
        if r["id"] == int(rid):
            return r
    fail(404, "Not Found")
def public(r):
    return {k: v for k, v in r.items() if k != "assets"}
def asset(a):
    return {k: v for k, v in a.items() if k != "content"}
argv = sys.argv[1:]
s["calls"].append(argv)
fails = os.environ.get("FAKE_GH_FAIL", "").split(",")
if argv[:2] == ["release", "create"]:
    tag = argv[2]
    if "release-create" in fails:
        fail(502, "Bad Gateway")
    draft = "--draft" in argv
    if not draft and any(r["tag_name"] == tag and not r["draft"] for r in s["releases"]):
        fail(422, "Validation Failed: already_exists")
    body = "generated notes" if "--generate-notes" in argv else ""
    if "--notes-file" in argv:
        body = open(argv[argv.index("--notes-file") + 1]).read()
    s["next_id"] += 1
    s["releases"].append({"id": s["next_id"], "tag_name": tag, "draft": draft, "prerelease": "--prerelease" in argv,
                          "published_at": None if draft else "now", "html_url": "https://example/" + tag,
                          "body": body, "assets": []})
    save()
    sys.exit(0)
if argv[:2] == ["release", "view"]:
    tag = argv[2]
    rs = [r for r in s["releases"] if r["tag_name"] == tag and not r["draft"]]
    if not rs:
        fail(404, "release not found")
    out({"body": rs[0]["body"]}, argv[argv.index("--jq") + 1] if "--jq" in argv else None)
    save()
    sys.exit(0)
assert argv[0] == "api", argv
args = argv[1:]
path = None
method, jq, data, fields = "GET", None, None, {}
i = 0
while i < len(args):
    a = args[i]
    if a == "-X": method = args[i + 1]; i += 2
    elif a == "--jq": jq = args[i + 1]; i += 2
    elif a == "-H": i += 2
    elif a == "--paginate": i += 1
    elif a == "--input":
        src = args[i + 1]
        data = sys.stdin.buffer.read() if src == "-" else open(src, "rb").read()
        i += 2
    elif a in ("-f", "-F"):
        k, v = args[i + 1].split("=", 1); fields[k] = v; i += 2
    elif path is None and not a.startswith("-"): path = a; i += 1
    else: raise SystemExit("stand-in gh: unknown argument " + a)
url = urlsplit(path)
query = parse_qs(url.query)
parts = url.path.strip("/").split("/")
assert parts[:3] == ["repos"] + REPO.split("/"), path
p = parts[3:]
if p == ["releases"] and method == "GET":
    if "releases-list" in fails:
        fail(502, "Bad Gateway")
    out([public(r) for r in s["releases"]], jq)
elif p[:2] == ["releases", "tags"]:
    rs = [r for r in s["releases"] if r["tag_name"] == p[2] and not r["draft"]]
    if not rs:
        fail(404, "Not Found")
    out(public(rs[0]), jq)
elif p[:2] == ["releases", "assets"] and method == "DELETE":
    for r in s["releases"]:
        r["assets"] = [a for a in r["assets"] if a["id"] != int(p[2])]
elif p[:2] == ["releases", "assets"]:
    for r in s["releases"]:
        for a in r["assets"]:
            if a["id"] == int(p[2]):
                sys.stdout.write(a["content"])
                save()
                sys.exit(0)
    fail(404, "Not Found")
elif len(p) == 3 and p[0] == "releases" and p[2] == "assets" and method == "POST":
    r = release(p[1])
    name = query["name"][0]
    if any(a["name"] == name for a in r["assets"]):
        fail(422, "already_exists")
    s["next_id"] += 1
    r["assets"].append({"id": s["next_id"], "name": name, "content": data.decode(),
                        "digest": "sha256:" + hashlib.sha256(data).hexdigest()})
elif len(p) == 3 and p[0] == "releases" and p[2] == "assets":
    out([asset(a) for a in release(p[1])["assets"]], jq)
elif len(p) == 2 and p[0] == "releases" and method == "PATCH":
    r = release(p[1])
    change = json.loads(data)
    r.update(change)
    if change.get("draft") is False and not r.get("published_at"):
        r["published_at"] = "now"
    out(public(r), jq)
elif len(p) == 2 and p[0] == "releases":
    out(public(release(p[1])), jq)
elif p[:3] == ["git", "ref", "tags"]:
    sha = s["tags"].get(p[3])
    if not sha:
        fail(404, "Not Found")
    out({"object": {"type": "commit", "sha": sha}}, jq)
elif p == ["git", "refs"] and method == "POST":
    s["tags"][fields["ref"].removeprefix("refs/tags/")] = fields["sha"]
elif p == ["commits", "main"]:
    out({"sha": s["main"]}, jq)
elif p[0] == "compare":
    out({"status": "ahead"}, jq)
elif p[:2] == ["actions", "runs"]:
    out({"workflow_id": 7}, jq)
elif p[:2] == ["actions", "workflows"]:
    out({"workflow_runs": [r for r in s["runs"] if r["status"] == query["status"][0]]}, jq)
else:
    raise SystemExit("stand-in gh: no endpoint " + method + " " + path)
save()
'''


def rel(rid, tag, draft, prerelease, body="", assets=()):
    return {"id": rid, "tag_name": tag, "draft": draft, "prerelease": prerelease,
            "published_at": None if draft else "then", "html_url": "https://example/" + tag, "body": body,
            "assets": [{"id": rid * 100 + n, "name": name, "content": name,
                        "digest": "sha256:" + hashlib.sha256(name.encode()).hexdigest()}
                       for n, name in enumerate(assets)]}


@unittest.skipIf(os.name == "nt" or not shutil.which("jq") or not shutil.which("bash"), "bash and jq")
class ReleaseScripts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="release-scripts-")
        self.dir = Path(self.temp.name)
        tools = self.dir / "bin"
        tools.mkdir()
        (tools / "gh").write_text(STUB, encoding="utf-8")
        (tools / "gh").chmod(0o755)
        # (no waiting between retries)
        (tools / "sleep").write_text("#!/bin/sh\n", encoding="utf-8")
        (tools / "sleep").chmod(0o755)
        self.state_file = self.dir / "state.json"
        self.files = self.dir / "dist"
        self.files.mkdir()
        for name in ("Hookshot-v2.1.0-x86_64.AppImage", "hookshot-linux.txt"):
            (self.files / name).write_text("built " + name, encoding="utf-8")
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"], GH_REPO=REPO,
                        GH_TOKEN="x", FAKE_GH_STATE=str(self.state_file))
        for k in ("GITHUB_RUN_ID", "GITHUB_SHA", "GITHUB_OUTPUT", "RELEASE_SUMS_OUT", "RELEASE_FROM_WORKFLOW"):
            self.env.pop(k, None)

    def tearDown(self):
        self.temp.cleanup()

    def given(self, releases, tags=None, runs=()):
        self.state_file.write_text(json.dumps({
            "releases": releases, "tags": tags or {}, "runs": list(runs), "main": COMMIT, "next_id": 1000,
            "calls": []}), encoding="utf-8")

    def state(self):
        return json.loads(self.state_file.read_text(encoding="utf-8"))

    def run_script(self, script, *args, cwd=None, **env):
        result = subprocess.run(["bash", str(ROOT / "scripts" / script), *args], capture_output=True, text=True,
                                env=dict(self.env, **env), cwd=cwd or self.dir, timeout=60)
        return result

    def upload(self, tag="v2.1.0", **env):
        return self.run_script("release_upload.sh", tag, *sorted(str(p) for p in self.files.iterdir()), **env)

    def creates(self):
        return [c for c in self.state()["calls"] if c[:2] == ["release", "create"]]

    # release_upload.sh

    def test_draft_gets_the_files_by_id_and_stays_a_draft(self):
        self.given([rel(5, "v2.1.0", True, False, "promote notes"), rel(4, "v2.1.0-rc.2", False, True)])
        r = self.upload(RELEASE_FROM_WORKFLOW="true")
        self.assertEqual(r.returncode, 0, r.stderr)
        draft = self.state()["releases"][0]
        self.assertTrue(draft["draft"])
        self.assertEqual(draft["body"], "promote notes")
        self.assertEqual(sorted(a["name"] for a in draft["assets"]), ["Hookshot-v2.1.0-x86_64.AppImage", "hookshot-linux.txt"])
        self.assertFalse(self.creates())
        uploads = [a for c in self.state()["calls"] for a in c if "uploads.github.com" in a]
        self.assertTrue(uploads and all("/releases/5/assets?" in u for u in uploads), uploads)

    def test_a_file_again_replaces_the_one_there(self):
        self.given([rel(5, "v2.1.0", True, False, assets=["hookshot-linux.txt"])])
        r = self.upload(RELEASE_FROM_WORKFLOW="true")
        self.assertEqual(r.returncode, 0, r.stderr)
        linux = [a for a in self.state()["releases"][0]["assets"] if a["name"] == "hookshot-linux.txt"]
        self.assertEqual([a["content"] for a in linux], ["built hookshot-linux.txt"])

    def test_listing_that_fails_makes_no_release(self):
        # (v2.1.0: a lookup of the draft that failed read as "no release", and a published "latest" one was made)
        for from_workflow in ("true", "false"):
            with self.subTest(from_workflow=from_workflow):
                self.given([rel(5, "v2.1.0", True, False)])
                r = self.upload(RELEASE_FROM_WORKFLOW=from_workflow, FAKE_GH_FAIL="releases-list")
                self.assertNotEqual(r.returncode, 0)
                self.assertFalse(self.creates())
                self.assertEqual(len(self.state()["releases"]), 1)
                self.assertTrue(self.state()["releases"][0]["draft"])

    def test_release_workflow_tag_without_its_draft_stops(self):
        self.given([])
        r = self.upload(RELEASE_FROM_WORKFLOW="true")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("has no release", r.stderr)
        self.assertFalse(self.creates())
        self.assertFalse(self.state()["releases"])

    def test_tag_pushed_by_hand_gets_its_release_and_sums(self):
        self.given([], tags={"v2.1.0": COMMIT})
        r = self.upload(RELEASE_SUMS_OUT=str(self.dir / "SUMS"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.creates()), 1)
        (made,) = self.state()["releases"]
        self.assertFalse(made["draft"])
        names = sorted(a["name"] for a in made["assets"])
        self.assertEqual(names, ["Hookshot-v2.1.0-x86_64.AppImage", "SHA256SUMS", "hookshot-linux.txt"])
        sums = next(a["content"] for a in made["assets"] if a["name"] == "SHA256SUMS")
        self.assertEqual(sums, (self.dir / "SUMS").read_text(encoding="utf-8"))
        self.assertIn("Which file do I download?", made["body"])
        check = self.run_script("release_files.sh", "check", "v2.1.0", "hookshot-linux.txt")
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)

    def test_two_releases_of_a_tag_are_refused(self):
        self.given([rel(5, "v2.1.0", True, False), rel(6, "v2.1.0", False, False)])
        r = self.upload(RELEASE_FROM_WORKFLOW="true")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("has 2 releases", r.stderr)
        self.assertTrue(all(not x["assets"] for x in self.state()["releases"]))

    # release_files.sh publish

    def test_publish_by_id(self):
        for tag, prerelease, latest in (("v2.1.0-rc.3", True, "false"), ("v2.1.0", False, "true")):
            with self.subTest(tag=tag):
                self.given([rel(9, tag, True, prerelease)])
                r = self.run_script("release_files.sh", "publish", tag)
                self.assertEqual(r.returncode, 0, r.stderr)
                (done,) = self.state()["releases"]
                self.assertEqual((done["draft"], done["prerelease"], done["make_latest"]), (False, prerelease, latest))

    # release_promote.sh prepare

    def prepare(self, rc="v2.1.0-rc.2", **env):
        return self.run_script("release_promote.sh", "prepare", rc, GITHUB_SHA=COMMIT,
                               GITHUB_OUTPUT=str(self.dir / "out"), **env)

    def rcs(self):
        return [rel(1, "v2.1.0-rc.1", False, True, "rc.1 notes"), rel(2, "v2.1.0-rc.2", False, True, "rc.2 notes")]

    def test_prepare_makes_the_tag_and_one_draft(self):
        self.given(self.rcs(), tags={"v2.1.0-rc.1": "a" * 40, "v2.1.0-rc.2": COMMIT})
        r = self.prepare()
        self.assertEqual(r.returncode, 0, r.stderr)
        state = self.state()
        self.assertEqual(state["tags"]["v2.1.0"], COMMIT)
        (draft,) = [x for x in state["releases"] if x["tag_name"] == "v2.1.0"]
        self.assertTrue(draft["draft"])
        self.assertIn("rc.2 notes", draft["body"])
        self.assertIn("tag=v2.1.0", (self.dir / "out").read_text(encoding="utf-8"))

    def test_prepare_again_goes_on_with_its_draft(self):
        self.given(self.rcs() + [rel(3, "v2.1.0", True, False, "old")], tags={"v2.1.0-rc.2": COMMIT, "v2.1.0": COMMIT})
        r = self.prepare()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(self.creates())
        (draft,) = [x for x in self.state()["releases"] if x["tag_name"] == "v2.1.0"]
        self.assertEqual(draft["id"], 3)
        self.assertIn("rc.2 notes", draft["body"])

    def test_prepare_refuses_a_published_version(self):
        self.given(self.rcs() + [rel(3, "v2.1.0", False, False)], tags={"v2.1.0-rc.2": COMMIT, "v2.1.0": COMMIT})
        r = self.prepare()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("v2.1.0 is already published", r.stderr)
        self.assertEqual(self.state()["releases"][-1]["body"], "")

    def test_prepare_refuses_beside_another_promote_run(self):
        self.given(self.rcs(), tags={"v2.1.0-rc.2": COMMIT},
                   runs=[{"id": 11, "status": "in_progress", "html_url": "https://example/runs/11"},
                         {"id": 12, "status": "in_progress", "html_url": "https://example/runs/12"}])
        r = self.prepare(GITHUB_RUN_ID="12")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("another Promote run is in progress: https://example/runs/11", r.stderr)
        self.assertNotIn("v2.1.0", self.state()["tags"])

    def test_prepare_refuses_a_tag_with_two_releases(self):
        self.given(self.rcs() + [rel(3, "v2.1.0", True, False), rel(4, "v2.1.0", True, False)],
                   tags={"v2.1.0-rc.2": COMMIT, "v2.1.0": COMMIT})
        r = self.prepare()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("has 2 releases", r.stderr)

    # release_promote.sh record

    def test_record_finds_the_release_candidate_and_is_made_once(self):
        self.given(self.rcs() + [rel(3, "v2.1.0", False, False)],
                   tags={"v2.1.0-rc.1": "a" * 40, "v2.1.0-rc.2": COMMIT, "v2.1.0": COMMIT})
        work = self.dir / "checkout"
        work.mkdir()
        (work / ".release-please-manifest.json").write_text('{".": "2.1.0-rc.3"}\n', encoding="utf-8")
        (work / "CHANGELOG.md").write_text("# Changelog\n\n## [2.1.0-rc.3](x) (2026-11-11)\n\n* a fix\n",
                                           encoding="utf-8")
        r = self.run_script("release_promote.sh", "record", "v2.1.0", cwd=work)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads((work / ".release-please-manifest.json").read_text(encoding="utf-8")), {".": "2.1.0"})
        changelog = (work / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertIn("Release candidate [v2.1.0-rc.2]", changelog)
        self.assertLess(changelog.index("## [2.1.0]"), changelog.index("## [2.1.0-rc.3]"))
        again = self.run_script("release_promote.sh", "record", "v2.1.0", cwd=work)
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual((work / "CHANGELOG.md").read_text(encoding="utf-8"), changelog)

    # the workflow

    def test_whole_promote_run_is_one_at_a_time(self):
        flow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        top = flow[:flow.index("\njobs:")]
        self.assertIn("\nconcurrency:\n  group: ${{ github.event_name == 'workflow_dispatch' && 'release-promote'", top)
        # (a job in the same group as its own run would wait for itself)
        self.assertEqual(flow.count("release-promote'"), 1)
        self.assertIn("  release:\n    types: [published]", top)
        self.assertNotIn("gh release edit", flow)
        for name in ("linux-appimage.yml", "windows-builder.yml"):
            text = (ROOT / ".github/workflows" / name).read_text(encoding="utf-8")
            self.assertIn("RELEASE_FROM_WORKFLOW: ${{ inputs.tag && 'true' || 'false' }}", text, name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    args, remaining = parser.parse_known_args()
    ROOT = args.source
    unittest.main(argv=[sys.argv[0]] + remaining)
