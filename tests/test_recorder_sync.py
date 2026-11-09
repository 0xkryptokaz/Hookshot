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
3. The test has teeth: vk_recorder.h with a copy left shallow (all of them, or only the descriptor writes' image infos), or with the
   fallback's drain left out, must fail it.
--bench N: the packet cost per draw on the fake (fakes that do nothing), ns, median of 5.
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
    unwrapped = {}
    for path in sorted(VK.glob("*.cpp")):
        text = code(path.read_text(encoding="utf-8", errors="replace"))
        used = sorted(set(re.findall(r"\b(vkCmd\w+)\s*\(", text)))
        if not used:
            continue
        if '#include "vk_work.h"' in text:
            bad = [c for c in used if c not in macros]
            if bad:
                problems.append("%s calls %s, which vk_work.h does not route through the recorder" % (path.name, bad))
            last = text.rfind("#include")
            if text.rfind('#include "vk_work.h"') != last:
                problems.append("%s: vk_work.h is not its last include" % path.name)
        else:
            unwrapped[path.name] = used
    for name, used in unwrapped.items():
        why = UNWRAPPED_OK.get(name)
        print("not through the recorder: %s %s (%s)" % (name, ", ".join(used), why or "NOT EXPECTED"))
        if not why:
            problems.append("%s calls %s without vk_work.h: not through the recorder" % (name, used))
    for name in set(UNWRAPPED_OK) - set(unwrapped):
        print("(%s: now through the recorder, or no vkCmd* calls; its entry in UNWRAPPED_OK can go)" % name)
    if problems:
        raise SystemExit("\n".join(problems))
    print("PASS sources: %d calls packed, each with its vk_work.h macro; every call in the %d vk_work.h files routed"
          % (len(calls), sum(1 for p in VK.glob("*.cpp") if '#include "vk_work.h"' in p.read_text(encoding="utf-8", errors="replace"))))
    return calls


def test_loader():
    """vk_loader.h without windows.h and the Win32 surface: the same declarations, on any system"""
    text = (VK / "vk_loader.h").read_text(encoding="utf-8")
    text = text.replace("#ifdef _WIN32\n#define VK_USE_PLATFORM_WIN32_KHR", "#if 0\n#define VK_USE_PLATFORM_WIN32_KHR", 1)
    text = text.replace("#include <windows.h>\n", "", 1)
    if "windows.h" in text or "#if 0" not in text:
        raise SystemExit("vk_loader.h's shape changed: update test_loader()")
    return text


def build(cxx, headers, out, recorder_text, name, extra=()):
    d = out / name
    d.mkdir()
    (d / "vk_loader.h").write_text(test_loader(), encoding="utf-8")
    shutil.copy(VK / "vk_work.h", d / "vk_work.h")
    (d / "vk_recorder.h").write_text(recorder_text, encoding="utf-8")
    exe = d / "recorder_sync_test.exe"
    cmd = [cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-missing-field-initializers", "-DVK_NO_PROTOTYPES",
           "-D_CRT_SECURE_NO_WARNINGS",
           *extra, "-I" + str(d), "-I" + str(headers), str(ROOT / "tests" / "recorder_sync_test.cpp"), "-o", str(exe)]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise SystemExit("%s: the build failed:\n%s" % (name, r.stdout))
    return exe


def run(exe, env_mode, *args):
    env = dict(os.environ)
    env.pop("RX_VK_RECORDER", None)
    if env_mode:
        env["RX_VK_RECORDER"] = env_mode
    return subprocess.run([str(exe), *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, timeout=600)


def broken(text, old, new, what):
    if text.count(old) != 1:
        raise SystemExit("vk_recorder.h's shape changed (%s): update the broken variants" % what)
    return text.replace(old, new)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--headers", required=True, help="the Vulkan headers' include folder")
    p.add_argument("--bench", type=int, default=0, help="also time the packets: N draws a run")
    a = p.parse_args()
    check_sources()

    work = ROOT / "work"
    work.mkdir(exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="recorder-sync-", dir=work))
    rec = (VK / "vk_recorder.h").read_text(encoding="utf-8")
    exe = build(a.cxx, a.headers, out, rec, "real")
    failed = False
    for env_mode, want in (("sync", "env mode 1"), (None, "env mode 0")):
        r = run(exe, env_mode)
        print("---- RX_VK_RECORDER=%s\n%s" % (env_mode or "(unset)", r.stdout.rstrip()))
        if r.returncode or want not in r.stdout or "ALL PASS" not in r.stdout:
            failed = True
    if failed:
        raise SystemExit("FAIL: the packed calls differ from the direct ones")

    variants = {
        "shallow copies (dup hands the caller's pointer on)":
            broken(rec, "        std::memcpy(static_cast<void*>(d), p, sizeof(T) * n);\n        return d;\n",
                   "        (void)d;\n        return p;\n", "dup"),
        "descriptor writes' image infos not copied":
            broken(rec, "c[i].pImageInfo = e.dup(w[i].pImageInfo, w[i].descriptorCount);",
                   "c[i].pImageInfo = w[i].pImageInfo;", "push descriptors"),
        "fallback without the drain (order)":
            broken(rec, "    drain();\n    t_arena.fallbacks++;", "    t_arena.fallbacks++;", "fallback"),
    }
    for i, (what, text) in enumerate(variants.items()):
        r = run(build(a.cxx, a.headers, out, text, "broken%d" % i), "sync")
        caught = r.returncode != 0
        lines = [l for l in r.stdout.splitlines() if l.startswith(("MISMATCH", "FAIL", "PASS sync", "PASS deferred"))]
        print("---- broken: %s -> %s\n  %s" % (what, "caught" if caught else "NOT CAUGHT", "\n  ".join(lines)))
        if not caught:
            raise SystemExit("FAIL: a broken recorder (%s) passed" % what)
    if a.bench:   # (with the renderer's CPU flags, nx2/build.py's CPU: RX_MARCH or x86-64-v3)
        fast = build(a.cxx, a.headers, out, rec, "bench", ["-march=" + os.environ.get("RX_MARCH", "x86-64-v3"), "-ffp-contract=off"])
        r = run(fast, None, "--bench", str(a.bench))
        print(r.stdout.rstrip())
    shutil.rmtree(out, ignore_errors=True)
    print("PASS recorder sync: identical streams, poisoned caller memory, coverage, fallback order; broken variants caught")


if __name__ == "__main__":
    main()
