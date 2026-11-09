"""RX_VK_RECORDER=sync: every vkCmd* the renderer makes through vk_work.h is packed with deep copies of its arguments, and the packed
calls reach the driver exactly as the direct ones do. No game, no GPU, no Vulkan library.

python -I -B tests/test_recorder_sync.py --headers <Vulkan-Headers/include> [--cxx clang++] [--bench N]

1. The sources: vk_work.h has a macro for each call vk_recorder.h packs and no other, each macro calls its packer, every vkCmd* call
   in a file that includes vk_work.h is one of them, and the files that make vkCmd* calls without it are the known ones (their calls
   do not go through the recorder: a recorder thread would reorder them).
2. tests/recorder_sync_test.cpp, built with the real vk_work.h and vk_recorder.h and a fake Vulkan that logs every call with its
   arguments dereferenced: a frame of the renderer's calls, direct vs packed (sync) vs packed and run later (deferred) while the
   caller poisons its argument memory after each call, must log the same bytes; all packed calls covered; the fallback order kept.
   Run with RX_VK_RECORDER=sync and without, so the switch is read as the renderer reads it.
   RX_VK_RECORDER=defer (step 1.5), against the fake's command buffer lifecycle: two command buffers recorded into interleaved,
   reset/begun/ended/submitted/waited for twice, a query read and a destruction, the caller's memory poisoned; a recording handed
   between two threads under the replay lock; a 2 KB soft budget; a 1 MB hard budget; the second chunk's allocation failing; a
   parallel replay running (par_active). Each must log what the direct calls log, no command outside its command buffer's recording,
   no exception out of a vkCmd*, no block lost. Run with RX_VK_RECORDER=defer (mode 2, logged once) and with RX_VK_PARALLEL=4 too
   (refused: off, logged once).
   RX_VK_RECORDER=thread (step 2): the same frame and lifecycle with every vkCmd* run by a recorder thread, the fake driver slowed
   (1 us a call) so the recorder lags, each fake command buffer flagged in use for each call (two calls at once on one are a
   violation), the caller's memory poisoned and each packet poisoned once run; a handoff under the lock; a submit while another
   command buffer's commands are in flight; a pool reset while recording; the lock let go, then the recording ended on another
   thread; an 8 KB ring (backpressure); a packet bigger than its room (made directly, in order); no memory for the ring; teardown with
   work queued (stop, join, lazy restart, vkDestroyDevice); par_active; >= 1e6 packets through a 16 KB ring, twice, hashed against
   the direct stream. --throw: a driver call that throws on the recorder thread ends the process (exit code 3, FATAL). Run with
   RX_VK_RECORDER=thread (mode 3, logged once) and with RX_VK_PARALLEL=4 (refused).
3. The test has teeth: vk_recorder.h with a copy left shallow (all of them, or only the descriptor writes' image infos), with the
   fallback's drain left out, with no drain before vkEndCommandBuffer, with the replay lock let go without a drain, with a throwing
   allocation (exception safety broken), with the failure's drain left out, or with the parallel refusal left out, must fail it.
   Thread (each run with --only thread, so the recorder thread's checks alone catch it): no wait before vkEndCommandBuffer, before
   vkQueueSubmit2, at the replay lock's release, before vkResetCommandPool; no backpressure (the ring overwritten); no join at
   teardown; the recorder's exception swallowed (--throw).
--bench N: the packet cost per draw on the fake (fakes that do nothing), ns, median of 5; and the recorder thread's.
--overlap F: the replay thread's time a frame, off vs thread, with a fake driver that takes time and drains every K draws.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
# (files that call vkCmd* without vk_work.h, and why that is right)
UNWRAPPED_OK = {
    "vk_coopmat.cpp": "the cooperative-matrix probe at start: its own command pool, buffer, submit and fence wait",
}
# (files that make a boundary call (RX_REC_BOUNDARIES: begin/end/reset, submit, wait, read, destroy) without vk_work.h, and why no packet
# can be held then: RX_VK_RECORDER=defer drains only through vk_work.h's macros and the replay lock)
BOUNDARY_OK = {
    "vk_coopmat.cpp": "the start-up probe: before any replay, its own vkCmd* direct (nothing held)",
    "vk_main.cpp": "create_swapchain: after device_wait_idle_locked() (vkDeviceWaitIdle through vk_work.h in vk_replay.cpp: drained)",
    "vk_shader.cpp": "program_cache_borrow's and a dropped try's own modules and layouts, which no recorded command uses",
}
# (the names a boundary has: a new one in a vk_work.h file must be in RX_REC_BOUNDARIES)
BOUNDARY_NAME = re.compile(r"\b(vk(?:Begin|End|Reset)\w*|vkQueue\w+|vkWait\w+|vkDeviceWaitIdle|vkDestroy\w+|vkFree\w+|"
                           r"vkGetQueryPoolResults|vkGetQueueCheckpointDataNV|vkGetDeviceFaultInfoEXT|vkGetSemaphoreCounterValue|"
                           r"vkAcquireNextImageKHR|vma(?:Destroy|Free|Invalidate)\w*)\s*\(")


def code(text):
    """the text without // and /* */ comments (string literals in these files hold no comment markers)"""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))


def recorder_calls(text):
    m = re.search(r"#define RX_REC_CALLS\(V, A\)(.*?)\n\s*\nnamespace", text, re.S)
    if not m:
        raise SystemExit("RX_REC_CALLS not found in vk_recorder.h")
    return re.findall(r"\b[VA]\((vkCmd\w+)\)", m.group(1))


def check_sources():
    rec = (VK / "vk_recorder.h").read_text(encoding="utf-8")
    work = (VK / "vk_work.h").read_text(encoding="utf-8")
    calls = recorder_calls(rec)
    if len(calls) != len(set(calls)):
        raise SystemExit("RX_REC_CALLS names a call twice")
    macros = dict(re.findall(r"#define (vkCmd\w+)\(\.\.\.\) (.*)", work))
    problems = []
    if set(macros) != set(calls):
        problems.append("vk_work.h's macros and RX_REC_CALLS differ: only in vk_work.h %s, only in vk_recorder.h %s"
                        % (sorted(set(macros) - set(calls)), sorted(set(calls) - set(macros))))
    for f, body in macros.items():
        if "::mkwvk::rec::rx_%s(__VA_ARGS__)" % f not in body:
            problems.append("vk_work.h: %s does not call its packer: %s" % (f, body))
        if "::vkx::" in body:
            problems.append("vk_work.h: %s calls the driver directly" % f)
    for f in calls:   # (each packer defined: generated for V, written for A)
        written = "RX_REC_PACKER void pk_%s(" % f in rec and "auto& rx_%s = Route<::vkx::%s, pk_%s>::call;" % (f, f, f) in rec
        if not re.search(r"\bV\(%s\)" % f, rec) and not written:
            problems.append("vk_recorder.h: no packer for %s" % f)
    # (defer) the boundaries: RX_REC_BOUNDARIES and vk_work.h's drain-first macros agree
    m = re.search(r"#define RX_REC_BOUNDARIES\(K, M\)(.*?)\n\s*\n", rec, re.S)
    if not m:
        raise SystemExit("RX_REC_BOUNDARIES not found in vk_recorder.h")
    bounds = dict((f, k) for k, f in re.findall(r"\b([KM])\((\w+)\)", m.group(1)))
    bmacros = dict(re.findall(r"#define (\w+)\(\.\.\.\) \(::mkwvk::rec::boundary\(::mkwvk::rec::B_\w+\), (::vkx::|::)\w+\(__VA_ARGS__\)\)", work))
    for f, q in bmacros.items():
        if "#define %s(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_%s), %s%s(__VA_ARGS__))" % (f, f, q, f) not in work:
            problems.append("vk_work.h: %s's macro calls another function" % f)
    if set(bmacros) != set(bounds) or any(bmacros[f] != ("::vkx::" if k == "K" else "::") for f, k in bounds.items() if f in bmacros):
        problems.append("vk_work.h's boundary macros and RX_REC_BOUNDARIES differ: only in vk_work.h %s, only in vk_recorder.h %s"
                        % (sorted(set(bmacros) - set(bounds)), sorted(set(bounds) - set(bmacros))))
    unwrapped = {}
    unbounded = {}
    routed = 0
    for path in sorted(VK.glob("*.cpp")) + sorted(VK.glob("*.h")):
        if path.name in ("vk_recorder.h", "vk_work.h", "vk_loader.h"):
            continue
        text = code(path.read_text(encoding="utf-8", errors="replace"))
        used = sorted(set(re.findall(r"\b(vkCmd\w+)\s*\(", text)))
        bused = sorted(set(BOUNDARY_NAME.findall(text)))
        if path.suffix == ".h":   # (a header's calls would be changed or not by the file including it: none make any)
            if used or bused:
                problems.append("%s (a header) calls %s: move them into a .cpp that includes vk_work.h" % (path.name, used + bused))
            continue
        if '#include "vk_work.h"' in text:
            routed += 1
            bad = [c for c in used if c not in macros]
            if bad:
                problems.append("%s calls %s, which vk_work.h does not route through the recorder" % (path.name, bad))
            bad = [c for c in bused if c not in bounds]
            if bad:
                problems.append("%s calls %s, which vk_work.h does not drain before (RX_REC_BOUNDARIES)" % (path.name, bad))
            last = text.rfind("#include")
            if text.rfind('#include "vk_work.h"') != last:
                problems.append("%s: vk_work.h is not its last include" % path.name)
        else:
            if used:
                unwrapped[path.name] = used
            if bused:
                unbounded[path.name] = bused
    for name, used in unwrapped.items():
        why = UNWRAPPED_OK.get(name)
        print("not through the recorder: %s %s (%s)" % (name, ", ".join(used), why or "NOT EXPECTED"))
        if not why:
            problems.append("%s calls %s without vk_work.h: not through the recorder" % (name, used))
    for name in set(UNWRAPPED_OK) - set(unwrapped):
        print("(%s: now through the recorder, or no vkCmd* calls; its entry in UNWRAPPED_OK can go)" % name)
    for name, used in unbounded.items():
        why = BOUNDARY_OK.get(name)
        print("boundaries without a drain: %s %s (%s)" % (name, ", ".join(used), why or "NOT EXPECTED"))
        if not why:
            problems.append("%s calls %s without vk_work.h: RX_VK_RECORDER=defer would not drain before them" % (name, used))
    for name in set(BOUNDARY_OK) - set(unbounded):
        print("(%s: no boundary calls without vk_work.h now; its entry in BOUNDARY_OK can go)" % name)
    if problems:
        raise SystemExit("\n".join(problems))
    print("PASS sources: %d calls packed and %d boundaries drained, each with its vk_work.h macro; every call in the %d vk_work.h "
          "files routed; no header calls either" % (len(calls), len(bounds), routed))
    return calls


def test_loader():
    """vk_loader.h without windows.h and the Win32 surface: the same declarations, on any system"""
    text = (VK / "vk_loader.h").read_text(encoding="utf-8")
    text = text.replace("#ifdef _WIN32\n#define VK_USE_PLATFORM_WIN32_KHR", "#if 0\n#define VK_USE_PLATFORM_WIN32_KHR", 1)
    text = text.replace("#include <windows.h>\n", "", 1)
    if "windows.h" in text or "#if 0" not in text:
        raise SystemExit("vk_loader.h's shape changed: update test_loader()")
    return text


def build(cxx, headers, out, recorder_text, name, extra=(), work_text=None):
    d = out / name
    d.mkdir()
    (d / "vk_loader.h").write_text(test_loader(), encoding="utf-8")
    (d / "vk_work.h").write_text(work_text or (VK / "vk_work.h").read_text(encoding="utf-8"), encoding="utf-8")
    (d / "vk_recorder.h").write_text(recorder_text, encoding="utf-8")
    exe = d / "recorder_sync_test.exe"
    cmd = [cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-missing-field-initializers", "-DVK_NO_PROTOTYPES",
           "-D_CRT_SECURE_NO_WARNINGS",
           *extra, "-I" + str(d), "-I" + str(headers), str(ROOT / "tests" / "recorder_sync_test.cpp"), "-o", str(exe)]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise SystemExit("%s: the build failed:\n%s" % (name, r.stdout))
    return exe


def run(exe, env_mode, *args, parallel=None, timeout=600):
    env = dict(os.environ)
    for k in ("RX_VK_RECORDER", "RX_VK_PARALLEL", "RX_VK_RECORDER_POISON", "RX_VK_RECORDER_RING_MB", "RX_VK_RECORDER_STATS"):
        env.pop(k, None)
    if env_mode:
        env["RX_VK_RECORDER"] = env_mode
    if parallel:
        env["RX_VK_PARALLEL"] = parallel
    try:
        return subprocess.run([str(exe), *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as e:   # (a hang: reported as exit code None)
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return subprocess.CompletedProcess(e.cmd, None, out + "\nTIMEOUT after %d s" % timeout)


# (the environment runs: RX_VK_RECORDER, RX_VK_PARALLEL; the mode the renderer reads; the log lines it must write, once)
ENV_RUNS = (
    ("sync", None, "env mode 1", ()),
    (None, None, "env mode 0", ()),
    ("defer", None, "env mode 2", ("LOG: vk: RX_VK_RECORDER=defer:",)),
    ("defer", "1", "env mode 2", ("LOG: vk: RX_VK_RECORDER=defer:",)),
    ("defer", "4", "env mode 0", ("LOG: vk: RX_VK_RECORDER=defer refused: RX_VK_PARALLEL=4",)),
    ("thread", None, "env mode 3", ("LOG: vk: RX_VK_RECORDER=thread: vkCmd* packed",)),
    ("thread", "1", "env mode 3", ("LOG: vk: RX_VK_RECORDER=thread: vkCmd* packed",)),
    ("thread", "4", "env mode 0", ("LOG: vk: RX_VK_RECORDER=thread refused: RX_VK_PARALLEL=4",)),
)


def env_run_ok(exe, env_mode, parallel, want, logs, show=True):
    r = run(exe, env_mode, parallel=parallel)
    if show:
        print("---- RX_VK_RECORDER=%s RX_VK_PARALLEL=%s\n%s" % (env_mode or "(unset)", parallel or "(unset)", r.stdout.rstrip()))
    lines = r.stdout.splitlines()
    ok = r.returncode == 0 and want in lines and "ALL PASS" in lines
    for l in logs:   # (exactly once)
        ok = ok and sum(1 for x in lines if x.startswith(l)) == 1
    if not logs:
        ok = ok and not any(x.startswith(("LOG: vk: RX_VK_RECORDER=defer", "LOG: vk: RX_VK_RECORDER=thread: vkCmd* packed",
                                          "LOG: vk: RX_VK_RECORDER=thread refused")) for x in lines)
    return ok, r


def no_wait(work_h, f):
    """vk_work.h with f's boundary macro calling the driver at once (no drain, no wait)"""
    return broken(work_h, "#define %s(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_%s), ::vkx::%s(__VA_ARGS__))" % (f, f, f),
                  "#define %s(...) (::vkx::%s(__VA_ARGS__))" % (f, f), f)


def broken(text, old, new, what):
    if text.count(old) != 1:
        raise SystemExit("vk_recorder.h's shape changed (%s): update the broken variants" % what)
    return text.replace(old, new)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--headers", required=True, help="the Vulkan headers' include folder")
    p.add_argument("--bench", type=int, default=0, help="also time the packets: N draws a run")
    p.add_argument("--overlap", type=int, default=0, help="also model the overlap: F frames a run")
    a = p.parse_args()
    check_sources()

    work = ROOT / "work"
    work.mkdir(exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="recorder-sync-", dir=work))
    rec = (VK / "vk_recorder.h").read_text(encoding="utf-8")
    exe = build(a.cxx, a.headers, out, rec, "real")
    failed = False
    for env_mode, parallel, want, logs in ENV_RUNS:
        if not env_run_ok(exe, env_mode, parallel, want, logs)[0]:
            failed = True
    if failed:
        raise SystemExit("FAIL: the packed calls differ from the direct ones, or the mode or its log line is wrong")
    r = run(exe, None, "--throw", timeout=60)   # (the recorder thread's exception: loud, exit code 3)
    print("---- --throw: exit code %s\n%s" % (r.returncode, r.stdout.rstrip()))
    if r.returncode != 3 or "FATAL: RX_VK_RECORDER=thread" not in r.stdout:
        raise SystemExit("FAIL: an exception on the recorder thread did not end the process loudly")

    work_h = (VK / "vk_work.h").read_text(encoding="utf-8")
    # (each: what is broken, vk_recorder.h, vk_work.h, the environment it runs in)
    variants = [
        ("shallow copies (dup hands the caller's pointer on)",
         broken(rec, "        std::memcpy(static_cast<void*>(d), p, sizeof(T) * n);\n        return d;\n",
                "        (void)d;\n        return p;\n", "dup"), work_h, ("sync", None)),
        ("descriptor writes' image infos not copied",
         broken(rec, "c[i].pImageInfo = e.dup(w[i].pImageInfo, w[i].descriptorCount);",
                "c[i].pImageInfo = w[i].pImageInfo;", "push descriptors"), work_h, ("sync", None)),
        ("fallback without the drain (order)",
         broken(rec, "    else drain();\n    t_arena.fallbacks++;", "    t_arena.fallbacks++;", "fallback"), work_h, ("sync", None)),
        ("defer: no drain before vkEndCommandBuffer",
         rec, no_wait(work_h, "vkEndCommandBuffer"), (None, None)),
        ("defer: the replay lock let go without a drain",
         broken(rec, "        boundary(B_REPLAY_UNLOCK);\n        t_prod = nullptr;", "        t_prod = nullptr;", "unlock"), work_h, (None, None)),
        ("defer: exception safety broken (a throwing chunk allocation)",
         broken(broken(rec, "::operator new(cap, std::align_val_t(64), std::nothrow)", "::operator new(cap, std::align_val_t(64))", "new"),
                "void* alloc(size_t n, size_t align) noexcept {", "void* alloc(size_t n, size_t align) {", "noexcept"), work_h, (None, None)),
        ("defer: an allocation failure's call made before the held ones (no drain)",
         broken(rec, "            if (md == DEFER) drain();   // (what is held first", "            if (0) drain();   // (what is held first",
                "failure drain"), work_h, (None, None)),
        ("defer: the parallel refusal left out (RX_VK_PARALLEL=4)",
         broken(rec, "        if (!parallel_configured()) return e[0] == 'd' ? DEFER : THREAD;", "        return e[0] == 'd' ? DEFER : THREAD;",
                "parallel"), work_h, ("defer", "4")),
        # (the recorder thread's: run with --only thread)
        ("thread: no wait before vkEndCommandBuffer", rec, no_wait(work_h, "vkEndCommandBuffer"), ("--only", None)),
        ("thread: no wait before vkQueueSubmit2", rec, no_wait(work_h, "vkQueueSubmit2"), ("--only", None)),
        ("thread: the replay lock let go without waiting for the recorder",
         broken(rec, "        boundary(B_REPLAY_UNLOCK);\n        t_prod = nullptr;", "        t_prod = nullptr;", "unlock"), work_h, ("--only", None)),
        ("thread: a command pool reset without waiting for the recorder", rec, no_wait(work_h, "vkResetCommandPool"), ("--only", None)),
        ("thread: no backpressure (the ring overwritten)",
         broken(broken(rec, "    void wait_room(uint64_t need) {\n", "    void wait_room(uint64_t need) {\n        if (need) return;\n", "room"),
                "        lim = base + off + std::min<uint64_t>(tail, cap - (w - r_seen));", "        lim = base + off + tail;", "lim"), work_h,
         ("--only", None)),
        ("thread: no join at teardown", broken(rec, "    th.join();\n    ready.store(false);", "    ready.store(false);", "join"), work_h,
         ("--only", None)),
        ("thread: the recorder thread's exception swallowed",
         broken(rec, "        recorder_fatal(e.what());", "        (void)e;", "fatal"), work_h, ("--throw", None)),
        ("thread: the parallel refusal left out (RX_VK_PARALLEL=4)",
         broken(rec, "        if (!parallel_configured()) return e[0] == 'd' ? DEFER : THREAD;", "        return e[0] == 'd' ? DEFER : THREAD;",
                "parallel"), work_h, ("thread", "4")),
    ]
    for i, (what, text, wtext, (env_mode, parallel)) in enumerate(variants):
        exe_b = build(a.cxx, a.headers, out, text, "broken%d" % i, work_text=wtext)
        if parallel:
            refused = [e for e in ENV_RUNS if e[0] == env_mode and e[1] == "4"][0]
            ok, r = env_run_ok(exe_b, env_mode, parallel, "env mode 0", refused[3], show=False)
            caught = not ok
        elif env_mode == "--only":
            r = run(exe_b, None, "--only", "thread", timeout=120)
            caught = r.returncode != 0
        elif env_mode == "--throw":
            r = run(exe_b, None, "--throw", timeout=30)
            caught = not (r.returncode == 3 and "FATAL: RX_VK_RECORDER=thread" in r.stdout)
        else:
            r = run(exe_b, env_mode)
            caught = r.returncode != 0
        lines = [l for l in r.stdout.splitlines() if l.startswith(("MISMATCH", "FAIL", "PASS sync", "PASS deferred", "env mode", "TIMEOUT",
                                                                     "the recorder"))]
        if not lines:
            lines = ["(no output: exit code %s)" % r.returncode]
        print("---- broken: %s -> %s (exit code %s)\n  %s" % (what, "caught" if caught else "NOT CAUGHT", r.returncode, "\n  ".join(lines[:6])))
        if not caught:
            raise SystemExit("FAIL: a broken recorder (%s) passed" % what)
    if a.bench or a.overlap:   # (with the renderer's CPU flags, nx2/build.py's CPU: RX_MARCH or x86-64-v3)
        fast = build(a.cxx, a.headers, out, rec, "bench", ["-march=" + os.environ.get("RX_MARCH", "x86-64-v3"), "-ffp-contract=off"])
        if a.bench:
            r = run(fast, None, "--bench", str(a.bench))
            print(r.stdout.rstrip())
        if a.overlap:
            r = run(fast, None, "--overlap", str(a.overlap))
            print(r.stdout.rstrip())
    shutil.rmtree(out, ignore_errors=True)
    print("PASS recorder sync: identical streams, poisoned caller memory, coverage, fallback order; broken variants caught")


if __name__ == "__main__":
    main()
