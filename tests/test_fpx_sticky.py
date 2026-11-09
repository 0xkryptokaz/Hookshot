"""FPX1's sticky-flag rescue for single-precision FMUL/FNMUL/FMLA, game-free: bit-exact with the exact body.

a64_lift.py's a64_fpx_mul32_fma32 and a64_fpx_fma32_fma32 (the cold wrappers lifted code calls when the inline fast path
cannot prove its result) try a64_fpx_sticky32 before the exact body recomp_fpe_fma32: when every FPSR flag the Arm op
could raise is already set, one hardware FMA (or multiply) is the Arm result. This builds what the real build builds,
a64_fpx.h from a64_lift.fpx_header() and a64_suyu.h / a64_suyu.c from a64_rtgen.py (through suyu's translator,
a64emit.cpp, built here at -O0 into a temporary folder), and compiles tests/fpx_sticky_test.c against the real rt/a64.h
at the build's FP flags (-march=x86-64-v3 -ffp-contract=off -O2). Each wrapper's value and FPSR must equal
recomp_fpe_fma32's for:
  - 8M random operand triples (16M checks, seeded): zeros, subnormals, inf, NaN (quiet and signalling), the extreme
    normals, products near the underflow and overflow edges, exact and near cancellations, under random FPCR (FZ,
    DN, RMode, other bits, the kill switch) and FPSR states;
  - an edge sweep (~100M checks): every a with biased exponent <= 4 or >= 250 whose fraction is below 2^12, above
    2^23 - 2^12 or a multiple of 97, against 12 fixed b and 4 addend kinds, each under FZ on and off with the flags
    all set or IXC only.
Both runs must also see the rescue decide cases of both ops. About 15 s here (fpstub's harness, which this is cut down
from, saw no mismatch in 6.4e9 checks).

It has teeth: with the helper's UFC test removed by hand (a64_fpx_sticky32's `if (!(s & 8u))` made `if (0)`, so a tiny
result is kept with UFC clear), both runs fail: 654k of the random checks and 5.4M of the sweep's mismatch (FPSR
without UFC). So does dropping its IDC test (a flushed subnormal operand with IDC clear): 406k and 7.1M.

    python -I -B tests/test_fpx_sticky.py [--cc clang] [--cxx clang++]

Skipped (exit 0) on a host that is not x86-64 or has no C/C++ compiler (PATH, $CC/$CXX, or the build's own emsdk /
HOOKSHOT_MINGW toolchain), or on a CPU without x86-64-v3.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(ROOT, "third_party", "a64-kit")
sys.path.insert(0, os.path.join(KIT, "lift"))
sys.path.insert(0, os.path.join(KIT, "tools"))
import toolchain  # noqa: E402

RANDOM_TRIPLES, SEED, SWEEP_STRIDE = 8000000, 20261109, 97
FLAGS = ["-march=x86-64-v3", "-ffp-contract=off", "-fno-strict-aliasing", "-O2"]   # (nx2/build.py's lifted-code FP flags)


def skip(why):
    print("fpx sticky: skipped (%s)" % why)
    sys.exit(0)


def compiler(given, env, names, build_name):
    """--cc/--cxx, else $CC/$CXX, else PATH, else the toolchain the build itself uses (emsdk, HOOKSHOT_MINGW)"""
    for c in (given, os.environ.get(env)):
        if c:
            return shutil.which(c) or (c if os.path.isfile(c) else None)
    for n in names:
        if shutil.which(n):
            return shutil.which(n)
    c = toolchain.find(build_name, host=True)
    return c if os.path.isfile(c) else None


def target_flags(cc, tmp):
    """clang on Windows: the build's target and lld (as tools/toolchain.py, with its lld-link copy in tmp, not in tools/)"""
    if os.name != "nt" or "clang" not in os.path.basename(cc).lower():
        return []
    flags = toolchain.native_target(host=True)
    if toolchain.mingw(host=True):
        return flags + toolchain.native_link(host=True)
    here = os.path.dirname(cc)
    link = shutil.which("lld-link", path=here) or shutil.which("lld-link")
    if not link:
        lld = shutil.which("lld", path=here) or shutil.which("lld")
        if not lld:
            return flags
        link = os.path.join(tmp, "lld-link.exe")
        if not os.path.exists(link):
            shutil.copyfile(lld, link)
    return flags + ["-fuse-ld=lld", "-B" + os.path.dirname(link), "-Wl,/nodefaultlib:oldnames"]


def run(cmd, timeout=600):
    kw = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS | subprocess.CREATE_NO_WINDOW} if os.name == "nt" \
        else {"preexec_fn": lambda: os.nice(10)}
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                          timeout=timeout, **kw)


def build(cmd, what):
    r = run(cmd)
    if r.returncode:
        print(r.stdout)
        raise SystemExit("fpx sticky: building %s failed" % what)


def generate(cxx, tmp, flags):
    """a64_fpx.h, a64_suyu.h and a64_suyu.c, by the generators the lift runs (a64emit built into tmp)"""
    dll = os.path.join(tmp, "a64emit.dll" if os.name == "nt" else "liba64emit.so")
    build([cxx, "-std=c++20", "-O0", "-shared", "-Wno-everything", "-o", dll, os.path.join(KIT, "lift", "a64emit.cpp")]
          + flags + ([] if os.name == "nt" else ["-fPIC"]), "a64emit")
    import build_emit
    build_emit.build = lambda force=False: dll   # (not a64emit.dll beside the source)
    import a64_lift
    import a64_rtgen
    import a64emit
    try:
        a64_rtgen.write(tmp)
    finally:
        if a64emit._lib is not None:   # (unloaded, so the folder can go)
            import _ctypes
            (_ctypes.FreeLibrary if os.name == "nt" else _ctypes.dlclose)(a64emit._lib._handle)
            a64emit._lib = None
    with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:
        f.write(a64_lift.fpx_header({("mul32", "fma32", 2, 3), ("fma32", "fma32", 3, 3)}))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc")
    p.add_argument("--cxx")
    a = p.parse_args()
    if platform.machine().lower() not in ("x86_64", "amd64"):
        skip("not an x86-64 host: %s" % platform.machine())
    cc = compiler(a.cc, "CC", ("clang", "gcc", "cc"), "clang")
    cxx = compiler(a.cxx, "CXX", ("clang++", "g++", "c++"), "clang++")
    if not cc or not cxx:
        skip("no C compiler" if not cc else "no C++ compiler")
    t0 = time.time()
    failed = []
    with tempfile.TemporaryDirectory(prefix="hookshot-fpx-sticky-") as tmp:
        generate(cxx, tmp, target_flags(cxx, tmp))
        exe = os.path.join(tmp, "fpx_sticky_test.exe")
        build([cc] + target_flags(cc, tmp) + FLAGS + ["-D_CRT_SECURE_NO_WARNINGS", "-I" + os.path.join(KIT, "rt"),
              "-I" + tmp, os.path.join(ROOT, "tests", "fpx_sticky_test.c"), os.path.join(tmp, "a64_suyu.c"), "-o", exe],
              "the driver")
        for args in (["random", str(RANDOM_TRIPLES), str(SEED)], ["sweep", str(SWEEP_STRIDE)]):
            r = run([exe] + args)
            print(r.stdout.rstrip())
            if r.returncode == 3:
                skip("this CPU is not x86-64-v3")
            if r.returncode:
                failed.append(args[0])
    if failed:
        raise SystemExit("fpx sticky: FAIL (%s) in %.1f s" % (", ".join(failed), time.time() - t0))
    print("fpx sticky: pass in %.1f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
