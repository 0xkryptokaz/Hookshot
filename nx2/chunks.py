"""Stable chunk boundaries for the lifted C (lift.py --write).

a64-kit's write() closes a chunk once it reaches CHUNK_BYTES of text, so a function that grows (a jump table found,
a fix in one form) moves every boundary after it, and build.py recompiles every chunk from there on: a fix to two
functions cost a full 45-minute rebuild. Here the boundaries are guest addresses, kept in work/image/chunks.txt:
a chunk ends after the last function below the next boundary, so only the chunks holding changed functions differ.
Without the file (or with --rechunk) the boundaries come from the sizes, as before, and are saved.

write()'s test is `size >= chunk_bytes`. chunk_bytes is a Boundaries instance: an int whose reflected comparison
(__le__, which Python tries first for a subclass on the right) decides from the function just emitted.
"""
import bisect
import os
import re

import a64_lift

FILE = "chunks.txt"


class Boundaries(int):
    def __new__(cls, size, starts, funcs):
        self = super().__new__(cls, size)
        self.starts = sorted(starts)
        self.recorded = []   # (without saved boundaries) where the size rule started a chunk
        self.current = None
        self.after = None    # the function emitted after `current`
        order = sorted(funcs)
        self.ends = set()
        for b in self.starts:
            i = bisect.bisect_left(order, b)
            if 0 < i < len(order):
                self.ends.add(order[i - 1])
        self.next_of = {a: b for a, b in zip(order, order[1:])}
        return self

    def __le__(self, size):   # `size >= self`
        e = self.current
        if self.starts:
            return e in self.ends
        if size >= int(self):
            nxt = self.next_of.get(e)
            if nxt is not None:
                self.recorded.append(nxt)
            return True
        return False


def load(img_dir):
    path = os.path.join(img_dir, FILE)
    if not os.path.exists(path):
        return []
    return [int(l, 16) for l in open(path) if l.strip() and not l.startswith("#")]


def save(img_dir, starts):
    with open(os.path.join(img_dir, FILE), "w") as f:
        f.write("# first guest address of each lifted C chunk after the first (nx2/chunks.py)\n")
        f.write("".join("%08x\n" % s for s in sorted(starts)))


def seed_from_gen(gen_dir):
    """The boundaries of an existing size-chunked lift: the first function defined in each code_NNN.c after the
    first"""
    starts = []
    pat = re.compile(r"^void f_([0-9a-f]+)\(Ctx \*__restrict A64_NOESC c\) \{")
    n = 1
    while os.path.exists(os.path.join(gen_dir, "code_%03d.c" % n)):
        for line in open(os.path.join(gen_dir, "code_%03d.c" % n)):
            m = pat.match(line)
            if m:
                starts.append(int(m.group(1), 16))
                break
        n += 1
    return starts


def write(L, gen_dir, img_dir, rechunk=False):
    """L.write(gen_dir) with stable boundaries; returns its files"""
    starts = [] if rechunk else load(img_dir)
    if not starts and not rechunk:
        starts = seed_from_gen(gen_dir)
    b = Boundaries(a64_lift.CHUNK_BYTES, starts, L.funcs)
    base_emit = L.emit_function

    def emit_function(e, texts):
        b.current = e
        return base_emit(e, texts)
    L.emit_function = emit_function
    try:
        files = L.write(gen_dir, chunk_bytes=b)
    finally:
        del L.emit_function
    # (the numbered chunks only: files has code_hot.c too, and counting it skipped the first stale chunk, linked twice)
    n = sum(1 for f in files if re.fullmatch(r"code_\d{3}\.c", os.path.basename(f)))
    while os.path.exists(os.path.join(gen_dir, "code_%03d.c" % n)):   # (fewer chunks than the last lift)
        os.remove(os.path.join(gen_dir, "code_%03d.c" % n))
        n += 1
    files += split_big(gen_dir, files)
    save(img_dir, starts or b.recorded)
    print("chunks: %d, boundaries %s" % (len(files), "kept" if starts else "from sizes (saved)"))
    return files


HEAD = re.compile(r"^(?:/\* .* \*/\n)?void f_[0-9a-f]+\(Ctx \*__restrict A64_NOESC c\) \{", re.M)
PART = re.compile(r"code_(\d{3})_(\d+)\.c")
# (a function's compile time grows with the square of its size: LLVM's passes over one function of tens of thousands of blocks. Fitted to
# a cold build of the 48 chunks at -O1 on a Ryzen 9 7900X: seconds ~ 1.5 x MB + 4.4 x MB^2 per function, so a function of QUAD bytes
# costs twice its size. A 24 MB chunk of small functions took ~45 s; one of 20 MB holding a few 2 MB functions, 350 s)
QUAD = 350000
COSTS = "chunk_costs.txt"   # (in the gen folder: each file's estimated compile cost, for build.py's order)


def fn_cost(size):
    """A function's estimated compile cost, in bytes of plain code"""
    return size + size * size // QUAD


def text_cost(text):
    """The estimated compile cost of a C file of lifted functions"""
    marks = [m.start() for m in HEAD.finditer(text)] + [len(text)]
    return marks[0] + sum(fn_cost(b - a) for a, b in zip(marks, marks[1:]))


def write_costs(gen_dir, costs):
    """Adds or replaces files' costs in chunk_costs.txt; costs: {file name: cost}"""
    path = os.path.join(gen_dir, COSTS)
    known = {}
    if os.path.exists(path):
        for line in open(path):
            name, _, n = line.partition(" ")
            if n.strip().isdigit() and os.path.exists(os.path.join(gen_dir, name)):
                known[name] = int(n)
    known.update(costs)
    with open(path, "w", newline="\n") as f:
        f.write("".join("%s %d\n" % kv for kv in sorted(known.items())))


def split_big(gen_dir, files, budget=None):
    """Chunks whose estimated compile time (fn_cost) is well past a plain chunk's, cut at function starts into parts of about equal
    cost: code_NNN.c, code_NNN_1.c ... A chunk of the giant functions took 350 s, alone at the end of the build, where a chunk of
    ordinary functions takes 45; the boundaries kept in chunks.txt also let chunks grow (44 MB beside 25). The first part keeps the
    name, so seed_from_gen and the boundaries are as before. Writes chunk_costs.txt for every chunk; returns the new parts' paths"""
    budget = budget or a64_lift.CHUNK_BYTES
    made, costs = [], {}
    for path in files:
        m = re.fullmatch(r"code_(\d{3})\.c", os.path.basename(path))
        if not m:
            continue
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
        marks = [h.start() for h in HEAD.finditer(text)]
        ends = marks[1:] + [len(text)]
        cost = [fn_cost(b - a) for a, b in zip(marks, ends)]
        total = sum(cost)
        if total <= budget * 3 // 2 or len(marks) < 2:
            costs[os.path.basename(path)] = marks[0] + total if marks else len(text)
            continue
        parts = -(-total // budget)
        cuts, run, k = [0], 0, 1   # (indexes into marks where a part starts)
        for i, c in enumerate(cost):
            if i and run >= total * k // parts:
                cuts.append(i)
                k = run * parts // total + 1   # (past a giant function: the next share, not a sliver after it)
            run += c
        bounds = [marks[i] for i in cuts] + [len(text)]
        preamble = text[:marks[0]]
        for j in range(len(cuts)):
            out = path if j == 0 else os.path.join(gen_dir, "code_%s_%d.c" % (m.group(1), j))
            piece = text[:bounds[1]] if j == 0 else preamble + text[bounds[j]:bounds[j + 1]]
            with open(out, "w", encoding="utf-8", newline="\n") as f:
                f.write(piece)
            costs[os.path.basename(out)] = len(preamble) + sum(cost[cuts[j]:cuts[j + 1] if j + 1 < len(cuts) else len(cost)])
            if j:
                made.append(out)
        print("chunks: %s (%d MB, cost %d MB) in %d parts" % (os.path.basename(path), len(text) >> 20, total >> 20, len(cuts)))
    for f in os.listdir(gen_dir):   # (parts of an earlier lift that this one did not make)
        if PART.fullmatch(f) and os.path.join(gen_dir, f) not in made:
            os.remove(os.path.join(gen_dir, f))
    write_costs(gen_dir, costs)
    return made
