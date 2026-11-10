"""The local bake, end to end on this machine: game tree -> seed -> SPIR-V -> hookshot_programs.bin.

    python3 nx2/bake_ds/bake_all.py all --romfs <dir> --out <dir> [--dir <build dir>] [--stamp <hex16>]
                                    [--depth 0|1] [--variants all|0|1|2] [--limit N] [--no-spirv-val]

Steps (each also a subcommand):
  1. enumerate  nx2/bake_ds/romenum.py: BNSH/BAHS/NDSR/NDVE/.vsetb, SARC, zstd+dictionaries -> hookshot_shaders.bin
  2. translate  nx2/bake_ds/bake_ds (built by nx2/bake_ds/build.sh): Ampere -> Maxwell -> Eden IR -> SPIR-V,
                written as <out>/spv/*.spv and in the back end's own cache format <out>/hookshot_programs.bin
  3. verify     bake_ds verify (the game's reader over the bank) + spirv-val over every module, when installed

CPU only: no Vulkan device, no game directory. The only paths written are --out and the build directory.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import romenum   # noqa: E402
import fixture   # noqa: E402


def translator_stamp(root=ROOT):
    """The sha1 half of HOOKSHOT_SHADER_STAMP, exactly as nx2/build.py:693-699 computes it: every file under
    gpu/vk/{eden,sirit,compat} plus vk_shader.cpp, vk_bank.cpp, vk_state.h, vk_api.h, relative path + CRLF-
    normalized bytes, sorted, first 16 hex. The cache stamp also mixes in sizeof(Shader::Info)/sizeof(Program),
    the library marker and RX_VK_*/EDEN_* env; bake_ds does that half (disk_stamp)."""
    vk = os.path.join(root, "gpu", "vk")
    files = [os.path.join(d, f) for top in ("eden", "sirit", "compat")
             for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
    files = sorted(files) + [os.path.join(vk, n) for n in ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h")]
    h = hashlib.sha1()
    for f in files:
        with open(f, "rb") as src:
            h.update(os.path.relpath(f, vk).replace(os.sep, "/").encode() + src.read().replace(b"\r\n", b"\n"))
    return h.hexdigest()[:16]


def run(cmd, **kw):
    print("+", " ".join(cmd))
    return subprocess.run(cmd, **kw)


def enumerate_step(romfs, out):
    os.makedirs(out, exist_ok=True)
    seed = os.path.join(out, "hookshot_shaders.bin")
    manifest = os.path.join(out, "enum.json")
    r = romenum.enumerate_tree(romfs)
    romenum.write_seed(seed, r)
    with open(manifest, "w") as f:
        json.dump({"romfs": os.path.abspath(romfs), "dicts": r["dicts"], "stats": r["stats"],
                   "programs": len(r["programs"]), "stages": len(r["stages"]),
                   "kinds": {"+".join(map(str, k)): v for k, v in romenum.kinds_of(r).most_common()},
                   "files": r["files"], "stage_sources": r["sources"]}, f, indent=1)
    print("enumerate: %d programs from %d stages -> %s" % (len(r["programs"]), len(r["stages"]), seed))
    return seed


def build_tool(build_dir, tool):
    if tool and os.path.exists(tool):
        return tool
    exe = os.path.join(build_dir, "bake_ds")
    if not os.path.exists(exe):
        run([os.path.join(HERE, "build.sh")], env={**os.environ, "OUT": build_dir}, check=True)
    return exe


def translate_step(tool, seed, out, stamp=None, depth=0, variants="all", limit=0, ssbo_align=16):
    os.makedirs(out, exist_ok=True)
    if stamp is None:
        stamp = translator_stamp()   # (the build of this checkout; --stamp overrides)
        print("translator stamp:", stamp)
    cmd = [tool, "translate", "--seed", seed, "--out", out, "--depth", str(depth), "--variants", variants,
           "--ssbo-align", str(ssbo_align)]
    if stamp:
        cmd += ["--stamp", stamp]
    if limit:
        cmd += ["--limit", str(limit)]
    return run(cmd, check=False).returncode


def verify_step(tool, out):
    bank = os.path.join(out, "hookshot_programs.bin")
    rc = run([tool, "verify", "--bank", bank], check=False).returncode
    spv = os.path.join(out, "spv")
    val = shutil.which("spirv-val")
    checked = invalid = 0
    if val and os.path.isdir(spv):
        for name in sorted(os.listdir(spv)):
            if not name.endswith(".spv"):
                continue
            checked += 1
            if run([val, "--target-env", "vulkan1.3", os.path.join(spv, name)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=False).returncode:
                print("invalid:", name)
                invalid += 1
    summary = {"bank_ok": rc == 0, "spirv_val": val is not None, "spirv_files": checked, "spirv_invalid": invalid}
    with open(os.path.join(out, "verify.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print("verify:", summary)
    return 0 if rc == 0 and invalid == 0 else 1


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("command", choices=["all", "enumerate", "translate", "verify", "fixture"])
    p.add_argument("--romfs")
    p.add_argument("--out", default=os.path.join("/tmp/opencode", "bake_ds_out"))
    p.add_argument("--dir", dest="build_dir", default=os.path.join("/tmp/opencode", "ds"))
    p.add_argument("--tool")
    p.add_argument("--stamp")
    p.add_argument("--depth", type=int, default=0)
    p.add_argument("--variants", default="all")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--ssbo-align", type=int, default=16,
                   help="device minStorageBufferOffsetAlignment the SPIR-V is baked with (default 16, the Vulkan minimum)")
    p.add_argument("--with-dict", action="store_true")
    a = p.parse_args(argv)

    if a.command == "fixture":
        if not a.romfs:
            a.romfs = os.path.join(a.out, "romfs")
        os.makedirs(a.romfs, exist_ok=True)
        print(fixture.build(a.romfs, a.with_dict))
        return 0
    if not a.romfs:
        p.error("--romfs is needed")

    if a.command == "enumerate":
        enumerate_step(a.romfs, a.out)
        return 0
    if a.command == "translate":
        seed = os.path.join(a.romfs, "hookshot_shaders.bin")
        tool = build_tool(a.build_dir, a.tool)
        return translate_step(tool, seed, a.out, a.stamp, a.depth, a.variants, a.limit, a.ssbo_align)
    if a.command == "verify":
        return verify_step(build_tool(a.build_dir, a.tool), a.out)

    seed = enumerate_step(a.romfs, a.out)
    tool = build_tool(a.build_dir, a.tool)
    rc = translate_step(tool, seed, a.out, a.stamp, a.depth, a.variants, a.limit, a.ssbo_align)
    if rc not in (0, 1):   # (1: some programs failed to translate; still verify what was written)
        return rc
    return verify_step(tool, a.out)


if __name__ == "__main__":
    sys.exit(main())
