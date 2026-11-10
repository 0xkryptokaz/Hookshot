"""The local bake's three steps on synthetic-but-real containers: enumerate -> translate -> bank (bake-deepseek).

Synthetic data only (nx2/bake_ds/fixture.py): no game files, no GPU. The enumeration is compared against
nx2/shader_census.py's readers on the same bytes; the translation/key part needs the offline translator,
built by nx2/bake_ds/build.sh (pass --tool, or set BAKE_DS_TOOL; without it that half is skipped, not failed).

    python -I -B tests/test_bake_ds.py [--tool /tmp/opencode/ds/bake_ds]
"""
import argparse
import collections
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
sys.path.insert(0, os.path.join(ROOT, "nx2", "bake_ds"))
import romenum  # noqa: E402
import fixture  # noqa: E402
import bake_all  # noqa: E402
import shader_census as census  # noqa: E402

CTL = 0xD8
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
M64 = (1 << 64) - 1


def fnv(data, h=FNV_OFFSET):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def code_key(control, code):
    """vk_shader.cpp:135 code_key"""
    return fnv(bytes(code), fnv(bytes(control[:CTL])))


def attr_type(vattr):
    """vk_shader.cpp:701 attribute_type_of over nvn_va_st.fmt"""
    fmt = vattr & 0xFF
    uints = {0x03, 0x0F, 0x1B, 0x27, 0x08, 0x14, 0x20, 0x2C, 0x0B, 0x17, 0x23, 0x2F, 0x3E}
    sints = {0x04, 0x10, 0x1C, 0x28, 0x09, 0x15, 0x21, 0x2D, 0x0C, 0x18, 0x24, 0x30, 0x6A}
    if fmt in uints:
        return 2   # UnsignedInt
    if fmt in sints:
        return 1   # SignedInt
    if fmt == 0x6B:
        return 4   # UnsignedScaled
    if fmt == 0x6C:
        return 3   # SignedScaled
    if 0x51 <= fmt <= 0x68:
        return 3 if fmt & 1 else 4
    return 0       # Float


def generic_used(sph, g):
    if len(sph) < 0x18 + g // 2 + 1:   # (a compute stage's code area has no header)
        return False
    nib = (sph[0x18 + g // 2] >> ((g % 2) * 4)) & 0xF
    return nib != 0


def mesh_key(task, mesh, fs):
    """vk_shader.cpp:744 mesh_key, with no pass-through primitive header (the seed path passes null)"""
    key = fnv(b"mesh", mesh["key"])
    if task:
        key = ((key * FNV_PRIME) & M64) ^ task["key"]
    if fs:
        key = ((key * FNV_PRIME) & M64) ^ fs["key"]
    return fnv(bytes(8), key)


def graphics_key(vs, fs, attrs):
    """vk_shader.cpp:733 graphics_key with the colour types the seed path passes (all zero)"""
    key = vs["key"] ^ ((fs["key"] * 31) & M64 if fs else 0)
    for i in range(16):
        t = attr_type(attrs[i]) if generic_used(vs["code"][:0x50], i) else 0
        key = ((key * FNV_PRIME) & M64) ^ (t << i)
    return fnv(bytes(8), key)


def parse_seed(path):
    d = open(path, "rb").read()
    assert d[:8] == b"HKSEED01", "seed magic"
    nprog, nst = struct.unpack_from("<II", d, 8)
    at, stages = 16, []
    for _ in range(nst):
        key, st, cs, bs = struct.unpack_from("<QIII", d, at)
        at += 20
        ctl = d[at:at + cs]
        code = d[at + cs:at + cs + bs]
        at += cs + bs
        stages.append({"stage": st, "control": ctl, "code": code, "key": code_key(ctl, code), "stored": key})
    programs = []
    for _ in range(nprog):
        n = struct.unpack_from("<I", d, at)[0]
        programs.append(list(struct.unpack_from("<%dI" % n, d, at + 4)))
        at += 4 + 4 * n
    return stages, programs


def run_tests(tool):
    tmp = tempfile.mkdtemp(prefix="bake_ds_")
    rom = os.path.join(tmp, "romfs")
    fixture.build(rom, with_dict=True)

    # 1. the enumerator reads every container exactly as nx2/shader_census.py does
    dicts = romenum.dictionaries(rom)
    assert len(dicts) == 1, "the fixture's dictionary pack"
    compared = 0
    class ZS:   # (census's unpack interface over romenum's libzstd)
        class ZstdError(Exception):
            pass
        @staticmethod
        def ZstdDecompressor(**kw):
            class D:
                def decompress(self, data, max_output_size=None):
                    return romenum.zstd().decompress(data, dicts)
            return D()
    zs = ZS()
    for path in romenum.archives(rom):
        raw = open(path, "rb").read()
        for k, data in enumerate(census.blobs(census.unpack(raw, dicts, zs), dicts, zs)):
            mine = list(romenum.programs_in(data, collections.Counter()))
            theirs = list(census.programs_in(data, collections.Counter()))
            assert len(mine) == len(theirs), (path, k, len(mine), len(theirs))
            for a, b in zip(mine, theirs):
                assert [(s[0], s[1], s[2]) for s in a] == [(s[0], s[1], s[2]) for s in b], (path, k)
            compared += 1
    assert compared >= 8, "every fixture container was compared (%d)" % compared

    # 1b. the translator half of the cache stamp is nx2/build.py's formula (an authored temp tree, as
    #     tests/test_shader_stamp_inputs.py checks the production block itself)
    vk = os.path.join(tmp, "stamp/gpu/vk")
    for rel in ("eden/a.cpp", "sirit/a.cpp", "compat/a.h", "vk_shader.cpp", "vk_bank.cpp", "vk_state.h",
                "vk_api.h", "vk_draw.cpp"):
        f = os.path.join(vk, rel)
        os.makedirs(os.path.dirname(f), exist_ok=True)
        open(f, "wb").write(b"authored input\n")
    first = bake_all.translator_stamp(os.path.join(tmp, "stamp"))
    open(os.path.join(vk, "vk_draw.cpp"), "wb").write(b"unrelated renderer edit\n")
    assert bake_all.translator_stamp(os.path.join(tmp, "stamp")) == first, "unrelated source entered the stamp"
    open(os.path.join(vk, "vk_bank.cpp"), "wb").write(b"changed bank semantics\r\n")
    assert bake_all.translator_stamp(os.path.join(tmp, "stamp")) != first, "bank edit did not change the stamp"

    # 2. enumerate -> HKSEED01, with the bone generic variants present
    r = romenum.enumerate_tree(rom, verbose=False)
    seed_path = os.path.join(tmp, "hookshot_shaders.bin")
    romenum.write_seed(seed_path, r)
    stages, programs = parse_seed(seed_path)
    assert all(s["key"] == s["stored"] for s in stages), "seed keys are the runtime's FNV-1a identity"
    assert len(programs) == 16, len(programs)
    kinds = collections.Counter(tuple(sorted(stages[i]["stage"] for i in ids)) for ids in programs)
    assert kinds[(0, 1)] == 10 and kinds[(5,)] == 5 and kinds[(1, 6, 7)] == 1, kinds
    bones = [ids for ids in programs if any(generic_used(stages[i]["code"][:0x50], 6) for i in ids)]
    assert len(bones) == 1, "Bones.bfsha's vertex program"

    # 3. the Python key mirror over the seed (this is the contract bake_ds implements)
    expect = {}
    for ids in programs:
        by = {stages[i]["stage"] & 7: stages[i] for i in ids}
        if 5 in by:
            expect[by[5]["key"]] = 1
        elif 7 in by:
            expect[mesh_key(by.get(6), by[7], by.get(1))] = 1
        else:
            vs, fs = by[0], by.get(1)
            attrs = [0] * 16
            keys = {graphics_key(vs, fs, attrs)}
            if generic_used(vs["code"][:0x50], 6):
                attrs[6] = 0x27
                keys.add(graphics_key(vs, fs, attrs))
                if generic_used(vs["code"][:0x50], 7):
                    attrs[7] = 0x27
                    keys.add(graphics_key(vs, fs, attrs))
            for k in keys:
                expect[k] = expect.get(k, 0) + 1
    assert bones and len(expect) == 18, "18 program slots (16 programs, Bones x3)"

    # 4. translate, bank, validate (needs the offline translator)
    if not tool:
        print("SKIP translate/bank: no --tool or BAKE_DS_TOOL (build it with nx2/bake_ds/build.sh)")
        print("tests/test_bake_ds.py: enumeration, parity and key mirror passed")
        return 0
    assert os.path.exists(tool), tool
    out = os.path.join(tmp, "bake")
    rc = subprocess.run([tool, "translate", "--seed", seed_path, "--out", out, "--stamp", "0123456789abcdef"],
                        check=False).returncode
    assert rc == 0, rc
    report = [json.loads(line) for line in open(os.path.join(out, "bake_report.jsonl"))]
    got = {int(line["key"], 16): 1 for line in report if line["status"] == "translated"}
    assert len(report) == 18, len(report)
    assert got == expect, (len(got), len(expect), sorted(set(got) ^ set(expect))[:4])
    # (each SPIR-V file is a module with the right magic, and every one passes spirv-val when installed)
    spv_dir = os.path.join(out, "spv")
    mods = sorted(os.listdir(spv_dir))
    assert len(mods) == 32, len(mods)       # 10 graphics x2 + 2 bone variants x2 + 5 compute + task/mesh/fragment
    for name in mods:
        words = open(os.path.join(spv_dir, name), "rb").read()
        assert len(words) >= 20 and struct.unpack_from("<I", words, 0)[0] == 0x07230203, name
    val = shutil.which("spirv-val")
    if val:
        for name in mods:
            p = subprocess.run([val, "--target-env", "vulkan1.3", os.path.join(spv_dir, name)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            assert p.returncode == 0, (name, p.stderr.decode()[:400])
    # (the bank reads back through a copy of program_cache_load's reader)
    p = subprocess.run([tool, "verify", "--bank", os.path.join(out, "hookshot_programs.bin")],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert p.returncode == 0, p.stdout.decode()
    assert b"18 records" in p.stdout and b"0 bad" in p.stdout, p.stdout
    # (compute layout plan: the c0 table the run time will build)
    comp = [line for line in report if line["kind"] == "compute"]
    assert comp and any(line["cs"]["end"] >= 16 for line in comp)
    # (the device-alignment guess is plumbed into both profile/host_info and the manifest)
    alt = os.path.join(tmp, "bake_align")
    rc = subprocess.run([tool, "translate", "--seed", seed_path, "--out", alt, "--stamp", "0123456789abcdef",
                         "--limit", "1", "--ssbo-align", "64", "--no-bank"], check=False).returncode
    assert rc == 0, rc
    man = json.load(open(os.path.join(alt, "bake_manifest.json")))
    assert man["ssbo_alignment"] == 64, man
    print("tests/test_bake_ds.py: enumeration, parity, keys, 18 translations, 32 modules, bank verified")
    return 0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--tool", default=os.environ.get("BAKE_DS_TOOL"))
    a = p.parse_args()
    return run_tests(a.tool)


if __name__ == "__main__":
    sys.exit(main())
