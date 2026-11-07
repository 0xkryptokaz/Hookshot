"""Stable chunk boundaries for the lifted C (lift.py --write).

a64-kit's write() closes a chunk once it reaches CHUNK_BYTES of text, so a function that grows (a jump table found,
a fix in one form) moves every boundary after it, and build.py recompiles every chunk from there on: a fix to two
functions cost a full 45-minute rebuild. Here the boundaries are guest addresses, kept in work/image/chunks.txt:
a chunk ends after the last function below the next boundary, so only the chunks holding changed functions differ.
Without the file (or with --rechunk) the boundaries come from the sizes, as before, and are saved.

write()'s test is `size >= chunk_bytes`. chunk_bytes is a Boundaries instance: an int whose reflected comparison
(__le__, which Python tries first for a subclass on the right) decides from the function just emitted.

The functions' C is made by several processes (fork, so Linux and macOS; one process elsewhere): each emits a run of
consecutive functions from the lifter as it stands, and write() takes the texts in its own order, as if it had made
them. The lifter's counters (stats, unhandled, ijump, and any passed in `counters`, such as poll.stats) get each
process's additions. The output is the one-process output, byte for byte (tests/test_parallel_emit.py).
"""
import bisect
import collections
import multiprocessing
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


_job = None   # (the forked workers' view: the emitter, the instruction texts and the counters, inherited)


def _counts(counters):
    return [collections.Counter(c) for c in counters]


def _emit_run(es):
    emit, texts, counters = _job
    before = _counts(counters)
    out = [(e,) + tuple(emit(e, texts)) for e in es]
    return out, [collections.Counter({k: v - b.get(k, 0) for k, v in c.items() if v != b.get(k, 0)})
                 for b, c in zip(before, counters)]


def emit_jobs():
    """Processes for the emit: HOOKSHOT_LIFT_JOBS, else the cores this process may use (at most 16; each holds its run's
    texts and the pages of the lift it touches). 1 where there is no fork"""
    if "fork" not in multiprocessing.get_all_start_methods():
        return 1
    if os.environ.get("HOOKSHOT_LIFT_JOBS"):
        return max(1, int(os.environ["HOOKSHOT_LIFT_JOBS"]))
    try:
        n = len(os.sched_getaffinity(0))
    except AttributeError:
        n = os.cpu_count() or 1
    return max(1, min(16, n))


def parallel_emit(emit, order, texts, counters, jobs, run=2000):
    """(e, text, refs) for every function of order, in order, emitted by `jobs` forked processes"""
    global _job
    if jobs <= 1 or len(order) < 2 * run:
        for e in order:
            yield (e,) + tuple(emit(e, texts))
        return
    import gc
    _job = (emit, texts, counters)
    gc.collect()
    gc.freeze()   # (the lift's objects out of the collector's sight: its passes would write to every page the children share)
    try:
        ctx = multiprocessing.get_context("fork")
        with ctx.Pool(jobs) as pool:
            for out, deltas in pool.imap(_emit_run, [order[i:i + run] for i in range(0, len(order), run)]):
                for c, d in zip(counters, deltas):
                    for k, v in d.items():
                        c[k] = c.get(k, 0) + v
                yield from out
    finally:
        _job = None
        gc.unfreeze()


def write(L, gen_dir, img_dir, rechunk=False, counters=(), jobs=None):
    """L.write(gen_dir) with stable boundaries; returns its files. counters: dicts the emit adds to besides the
    lifter's own (poll.stats); jobs: emit processes (emit_jobs())"""
    starts = [] if rechunk else load(img_dir)
    if not starts and not rechunk:
        starts = seed_from_gen(gen_dir)
    b = Boundaries(a64_lift.CHUNK_BYTES, starts, L.funcs)
    base_emit = L.emit_function
    jobs = emit_jobs() if jobs is None else jobs
    made = {"it": None}

    def emit_function(e, texts):
        b.current = e
        if made["it"] is None:   # (the first call: write() has its texts now, and asks for the functions in sorted order)
            made["it"] = parallel_emit(base_emit, sorted(L.funcs), texts,
                                       [L.stats, L.unhandled, L.ijump] + list(counters), jobs)
        got = next(made["it"], None)
        if got is None or got[0] != e:
            raise SystemExit("chunks: write() asked for f_%08x, the emit made %s" % (e, "f_%08x" % got[0] if got else "nothing"))
        return got[1], got[2]
    L.emit_function = emit_function
    try:
        files = L.write(gen_dir, chunk_bytes=b)
    finally:
        del L.emit_function
        if made["it"] is not None:
            made["it"].close()   # (the processes go now, not when the generator is collected)
    # (the numbered chunks only: files has code_hot.c too, and counting it skipped the first stale chunk, linked twice)
    n = sum(1 for f in files if re.fullmatch(r"code_\d{3}\.c", os.path.basename(f)))
    while os.path.exists(os.path.join(gen_dir, "code_%03d.c" % n)):   # (fewer chunks than the last lift)
        os.remove(os.path.join(gen_dir, "code_%03d.c" % n))
        n += 1
    save(img_dir, starts or b.recorded)
    print("chunks: %d, boundaries %s" % (len(files), "kept" if starts else "from sizes (saved)"))
    return files
