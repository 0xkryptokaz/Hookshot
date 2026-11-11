"""The pipeline state lists (gpu/vk/vk_draw.cpp: hookshot_pipeline_states.bin, written as the game is played): show them, keep the
repository's sources of the list a release ships, and merge those into it.

The repository keeps the list a release ships as separate sources in assets/pipeline_states/ (README.md there):

    tour.bin.xz                 written only by the pipeline tour (scripts/pipeline_tour.py), replaced whole each time
    contrib/<name>.bin.xz       one per contributed list, added to and never edited by anyone else
    <source>.md                 beside each: where it comes from (who, GPU and driver, build, settings, route)

nx2/make_game.py merges them (the tour's first, then the contributed ones by name; each pipeline once) into the game folder's
hookshot_pipeline_states_shipped.bin, whose pipelines the first start's shader preparation builds before the title. The same
sources always merge into the same bytes.

    python -I nx2/pipeline_states.py <list>...                            what they hold
    python -I nx2/pipeline_states.py add <name> <list>... --who ... --gpu ... --build ... --settings ... --route ...
        a contributed list, contrib/<name>.bin.xz: the lists' pipelines that no other contributed list holds (to an existing
        <name>, its pipelines are kept and the new ones added), and its note contrib/<name>.md
    python -I nx2/pipeline_states.py tour <list>... --gpu ... --build ... --settings ... --route ...
        tour.bin.xz and tour.md replaced by the lists' pipelines (the pipeline tour runs it)
    python -I nx2/pipeline_states.py merge --out <file>                    the merged list (xz when <file> ends in .xz)
    python -I nx2/pipeline_states.py check                                 each source whole, of this build's format, not empty,
                                                                          with its note; the merge the same twice (CI runs it)
    python -I nx2/pipeline_states.py --out <file> <list>...               lists merged, each pipeline once (any lists)
    python -I nx2/pipeline_states.py --check <list>...                    each list whole and holding pipelines

add, tour, merge and check take --dir <sources folder> (default: the repository's assets/pipeline_states).

A list may be xz-compressed (read by its magic; written so when its name ends in .xz, with fixed settings, so the same records
always give the same bytes): the repository's are, so that its history stays small. xz, not zstd: the AppImage's Python (3.12)
has lzma but not compression.zstd.

A list holds no game code: each record is a pipeline's stage code keys (hashes of the game's shaders, which the player's own
hookshot_shaders.bin turns back into the shaders), the program variant's hash, and the formats and draw state baked into the pipeline.

    <list>: "HKSTATE1", u32 record size (528), then the records:
            u64 code[4] (vertex or mesh, fragment, task, the header-only stage), u64 variant,
            u32 mesh, topology, targets, samples, depth format, has depth, u32 color formats[8], u8 color types[8],
            u64 blend[8], u32 vertex attributes[16], u32 attribute count, u32 strides[16], u32 divisors[16],
            u32 blend enables, channel mask, polygon mode, depth clamp, alpha to coverage, pad,
            u32 answers, u32 questions, u32 answers[24] (stage, descriptor, image, type), u8 questions[32] (each answer as asked)

When the record's format changes, every source is converted (or the tour's made again): the layout of the folder stays.
"""
import argparse
import collections
import lzma
import os
import re
import struct
import sys

MAGIC = b"HKSTATE1"
SIZE = 528
SOURCES = ("assets", "pipeline_states")   # (under the repository's root: the sources of the list a release ships)
TOUR = "tour.bin.xz"
CONTRIB = "contrib"
EXT = ".bin.xz"
XZ_MAGIC = b"\xfd7zXZ\x00"
NAME = re.compile(r"^[a-z0-9]+(?:[-_.][a-z0-9]+)*$")   # (a contributed list's name: the same order on every file system)
# (a source's note: "- <field>: <text>" lines; Pipelines is written by this tool and must match the list)
FIELDS = ("Who", "GPU and driver", "Build", "Settings", "Route")
ARG = {"Who": "who", "GPU and driver": "gpu", "Build": "build", "Settings": "settings", "Route": "route"}
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path):
    """The list's bytes as the game reads them: unpacked when the file is xz"""
    with open(path, "rb") as f:
        data = f.read()
    if data[:6] == XZ_MAGIC:
        try:
            data = lzma.decompress(data)
        except lzma.LZMAError as e:
            raise SystemExit("%s: a damaged xz file (%s)" % (path, e))
    return data


def parse(data, path, strict=False):
    """The records of a list's bytes. Stops with the reason when they are not a list of this build's format. A record cut
    short at the end (a game that ended while it wrote) is left out, as the game leaves it out; strict: stops there too"""
    if data[:8] != MAGIC or len(data) < 12 or struct.unpack_from("<I", data, 8)[0] != SIZE:
        found = data[:8].decode("ascii", "replace") if data[:7] == MAGIC[:7] else "no list"
        raise SystemExit("%s: not a pipeline state list of this build's format (%s, %d-byte records; found %s): make it again "
                         "with this build (docs/development.md, \"The pipeline state list\")" % (path, MAGIC.decode(), SIZE, found))
    over = (len(data) - 12) % SIZE
    if over:
        if strict:
            raise SystemExit("%s: cut short (%d bytes over the last whole record)" % (path, over))
        print("%s: %d bytes of a record cut short at the end, left out" % (path, over), file=sys.stderr)
    body = data[12:len(data) - over]
    return [body[i:i + SIZE] for i in range(0, len(body), SIZE)]


def read(path, strict=False):
    return parse(load(path), path, strict)


def check_shipped(path):
    """A list a release ships: present, whole, of this build's format and not empty; its records. Stops with the reason"""
    if not os.path.isfile(path):
        raise SystemExit("%s is missing, and a release must ship it (docs/development.md, \"The pipeline state list\")" % path)
    recs = read(path, strict=True)
    if not recs:
        raise SystemExit("%s holds no pipelines" % path)
    return recs


def pack(recs):
    """A list's bytes, unpacked: what the game reads"""
    return MAGIC + struct.pack("<I", SIZE) + b"".join(recs)


def compress(data):
    # (fixed settings, and xz stores no time or name: the same records always give the same file. Preset 6: an 8 MB window, as
    # much as a list needs; 9's 64 MB would only cost the reader memory)
    return lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=6 | lzma.PRESET_EXTREME)


def write(path, recs):
    data = pack(recs)
    if path.endswith(".xz"):
        data = compress(data)
    with open(path + ".new", "wb") as f:
        f.write(data)
    os.replace(path + ".new", path)


def unique(lists):
    """The records of lists of records, each once, in the order first seen"""
    out, seen = [], set()
    for recs in lists:
        for r in recs:
            if r not in seen:
                seen.add(r)
                out.append(r)
    return out


def describe(recs):
    programs = collections.Counter()
    kinds = collections.Counter()
    for r in recs:
        code = struct.unpack_from("<4Q", r, 0)
        mesh = struct.unpack_from("<I", r, 40)[0]
        programs[(code, mesh)] += 1
        kinds["mesh" if mesh else "graphics"] += 1
    return "%d pipelines (%s) of %d programs" % (len(recs), ", ".join("%s %d" % kv for kv in sorted(kinds.items())), len(programs))


# ---- the repository's sources ----

def sources_dir(root=None):
    return os.path.join(root or ROOT, *SOURCES)


def note_path(source):
    return source[:-len(EXT)] + ".md"


def sources(d):
    """The sources of the shipped list in merge order: the tour's (when there is one), then the contributed lists by name. Stops
    on a file in contrib/ that is not a list with a name the order can rely on"""
    out = []
    if os.path.isfile(os.path.join(d, TOUR)):
        out.append(os.path.join(d, TOUR))
    c = os.path.join(d, CONTRIB)
    names = sorted(os.listdir(c)) if os.path.isdir(c) else []
    for n in names:
        if n.endswith(".md") and n[:-3] + EXT in names:
            continue
        if not n.endswith(EXT) or not NAME.match(n[:-len(EXT)]):
            raise SystemExit("%s: not a contributed list (contrib/ holds <name>%s and <name>.md, the name in lower-case letters, "
                             "digits, '-', '_' and '.')" % (os.path.join(c, n), EXT))
        out.append(os.path.join(c, n))
    return out


def merged(d, strict=True):
    """The shipped list's records: the sources', each pipeline once, in merge order. strict: stops on a source that does not
    read; else it is left out with a line saying so (the build's way: nx2/make_game.py)"""
    lists = []
    for path in sources(d):
        try:
            lists.append(read(path, strict=True))
        except SystemExit as e:
            if strict:
                raise
            print("left out of the shipped pipeline state list: %s" % e, flush=True)
    return unique(lists)


def read_note(path):
    fields = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                m = re.match(r"^- ([^:]+): (.*\S)\s*$", line)
                if m:
                    fields[m.group(1)] = m.group(2)
    except OSError:
        return None
    return fields


def write_note(path, title, fields, recs):
    lines = ["# %s" % title, "", "Hashes, formats and draw state only: no game code or data (assets/pipeline_states/README.md).", ""]
    lines += ["- %s: %s" % (k, fields[k]) for k in FIELDS]
    lines += ["- Pipelines: %d" % len(recs), ""]
    with open(path + ".new", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    os.replace(path + ".new", path)


def check_sources(d):
    """Each source whole, of this build's format, holding pipelines each once, with its note (every field, and the count); the
    merge giving the same bytes twice, and every source's pipelines. The merged records. Stops with the reasons"""
    problems = []
    paths = sources(d)
    if not paths:
        raise SystemExit("%s holds no pipeline state lists, and a release must ship one (docs/development.md, \"The pipeline "
                         "state list\")" % d)
    seen = set()
    for path in paths:
        rel = os.path.relpath(path, d)
        try:
            with open(path, "rb") as f:
                if f.read(6) != XZ_MAGIC:
                    problems.append("%s: not xz-compressed (write it with this tool)" % rel)
            recs = check_shipped(path)
        except SystemExit as e:
            problems.append(str(e))
            continue
        if len(set(recs)) != len(recs):
            problems.append("%s: %d pipelines more than once" % (rel, len(recs) - len(set(recs))))
        note = read_note(note_path(path))
        if note is None:
            problems.append("%s: no note beside it (%s): where its pipelines come from" % (rel, os.path.basename(note_path(path))))
        else:
            for k in FIELDS:
                if not note.get(k):
                    problems.append("%s: its note has no \"- %s: ...\" line" % (rel, k))
            if note.get("Pipelines") != str(len(recs)):
                problems.append("%s: its note says %s pipelines, the list holds %d" % (rel, note.get("Pipelines"), len(recs)))
        new = len(set(recs) - seen)
        seen |= set(recs)
        print("%s: %s, %d not in the sources before it" % (rel, describe(recs), new))
    if problems:
        raise SystemExit("\n".join(problems))
    a, b = pack(merged(d)), pack(merged(d))
    recs = parse(a, d, strict=True)
    if a != b or set(recs) != seen or len(recs) != len(seen):
        raise SystemExit("%s: the merge is not reproducible, or misses pipelines of its sources" % d)
    print("merged: %s" % describe(recs))
    return recs


def contribute(d, name, lists, fields):
    """contrib/<name>.bin.xz: the lists' pipelines no other contributed list holds (not the tour's, which is replaced whole: a
    pipeline left out for it would be lost when the tour no longer finds it), after the ones it holds already"""
    if not NAME.match(name):
        raise SystemExit("%s: a name of lower-case letters, digits, '-', '_' and '.' (e.g. <github user>-<what was played>)" % name)
    c = os.path.join(d, CONTRIB)
    os.makedirs(c, exist_ok=True)
    path = os.path.join(c, name + EXT)
    old = read(path, strict=True) if os.path.isfile(path) else []
    others = set()
    for p in sources(d):
        if os.path.dirname(p) == c and p != path:
            others.update(read(p, strict=True))
    given = unique(read(p) for p in lists)
    recs = unique([old, [r for r in given if r not in others]])
    note = read_note(note_path(path)) or {}
    for k in FIELDS:
        if fields.get(k):
            note[k] = fields[k]
        if not note.get(k):
            raise SystemExit("%s: say where the list comes from (--%s)" % (name, ARG[k]))
    if len(recs) == len(old):
        print("%s: none of the %d pipelines are new" % (name, len(given)))
        return recs
    write(path, recs)
    write_note(note_path(path), name, note, recs)
    print("%s: %d new of %d given; %s" % (os.path.relpath(path, d), len(recs) - len(old), len(given), describe(recs)))
    return recs


def tour(d, lists, fields):
    """tour.bin.xz and its note replaced by the lists' pipelines"""
    recs = unique(read(p) for p in lists)
    if not recs:
        raise SystemExit("the tour's lists hold no pipelines")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, TOUR)
    write(path, recs)
    write_note(note_path(path), "The pipeline tour", dict(fields, Who="the pipeline tour (scripts/pipeline_tour.py)"), recs)
    print("%s: %s" % (TOUR, describe(recs)))
    return recs



def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] in ("add", "tour", "merge", "check"):
        p = argparse.ArgumentParser(prog="pipeline_states.py " + argv[0])
        p.add_argument("--dir", default=sources_dir())
        if argv[0] == "add":
            p.add_argument("name")
        if argv[0] in ("add", "tour"):
            p.add_argument("lists", nargs="+")
            for k, a in ARG.items():
                if not (argv[0] == "tour" and k == "Who"):
                    p.add_argument("--" + a, default="", help=k)
        if argv[0] == "merge":
            p.add_argument("--out", required=True)
        a = p.parse_args(argv[1:])
        fields = {k: getattr(a, v, "").strip() for k, v in ARG.items()}
        if argv[0] == "add":
            contribute(a.dir, a.name, a.lists, fields)
        elif argv[0] == "tour":
            for k in FIELDS[1:]:
                if not fields[k]:
                    raise SystemExit("tour: say what it was made with (--%s)" % ARG[k])
            tour(a.dir, a.lists, fields)
        elif argv[0] == "merge":
            recs = merged(a.dir)
            write(a.out, recs)
            print("%s: %s" % (a.out, describe(recs)))
        else:
            check_sources(a.dir)
        return 0
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("lists", nargs="+")
    p.add_argument("--out")
    p.add_argument("--check", action="store_true", help="each list must be whole and hold pipelines")
    a = p.parse_args(argv)
    lists = []
    for path in a.lists:
        recs = check_shipped(path) if a.check else read(path)
        print("%s: %s" % (path, describe(recs)))
        lists.append(recs)
    if a.out:
        recs = unique(lists)
        write(a.out, recs)
        print("%s: %s" % (a.out, describe(recs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
