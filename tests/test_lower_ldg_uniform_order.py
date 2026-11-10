"""The Ampere -> Maxwell lowering takes LDG [URn+imm] with the load's ordering bits 77-78 both set (native op 0x981).

Two of OoT's compute programs stopped at it as "native global load uniform addressing mode": the uniform-address form
allowed bit 77 (as the register form does: a Maxwell LDG needs nothing for the ordering) but not 78 beside it. The load
is Maxwell LDG.E of the uniform register pair plus the offset, its size as before. Other unknown bits stay refused.
python -I -B tests/test_lower_ldg_uniform_order.py [--cxx clang++]
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using Shader::Ampere::Instruction;
static Instruction insn(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { Instruction i{}; i.words[0] = a; i.words[1] = b; i.words[2] = c; i.words[3] = d; return i; }
static const Instruction exit_insn = insn(0x0000794d, 0, 0x03800000, 0x000fea00);   // EXIT
static Shader::Ampere::LoweredProgram lower(Instruction i) { return Shader::Ampere::LowerToMaxwell(std::vector<Instruction>{i, exit_insn}); }
static const uint64_t* find_ldg(const Shader::Ampere::LoweredProgram& r) {
    for (const uint64_t& i : r.instructions) if ((i >> 52) == 0xeed) return &i;
    return nullptr;
}
static void check(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t dest, uint32_t offset, uint32_t size) {
    const auto r = lower(insn(w0, w1, w2, 0x000ea800));
    CHECK(r);
    const uint64_t* ldg = find_ldg(r);
    CHECK(ldg);
    CHECK((*ldg & 0xff) == dest);                          // Rd
    CHECK(((*ldg >> 20) & 0xffffff) == offset);            // the immediate offset
    CHECK(((*ldg >> 48) & 7) == size);                     // 32 or 128 bits
    CHECK(((*ldg >> 45) & 1) == 1);                        // .E: a 64-bit address
    const uint32_t address = (*ldg >> 8) & 0xff;
    CHECK(address != 255 && (address & 1) == 0);           // the uniform pair, in an aligned register pair
}
int main() {
    // LDG.E [UR4+0x2c] into R1 (32 bits) and [UR4+0x20] into R8 (128 bits), ordering bits 77 and 78 set (the log's two)
    check(0xff017981, 0x00002c04, 0x0c1ee900, 1, 0x2c, 4);
    check(0xff087981, 0x00002004, 0x0c1eed00, 8, 0x20, 6);
    // bit 77 alone and neither, as before
    check(0xff017981, 0x00002c04, 0x0c1ea900, 1, 0x2c, 4);
    check(0xff017981, 0x00002c04, 0x0c1e8900, 1, 0x2c, 4);
    // still refused: bit 76 in the uniform form, an odd uniform register
    CHECK(!lower(insn(0xff017981, 0x00002c04, 0x0c1ef900, 0x000ea800)));
    CHECK(!lower(insn(0xff017981, 0x00002c05, 0x0c1ee900, 0x000ea800)));
    std::printf("PASS: LDG [URn+imm] with ordering bits 77-78 lowers to Maxwell LDG.E\n");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=shutil.which("clang++") and "clang++" or "g++")
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="lower-ldg-order-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wno-everything" if "clang" in a.cxx else "-w",
                        "-I" + str(ROOT / "gpu" / "vk" / "eden"), "-I" + str(ROOT / "gpu" / "vk" / "compat"),
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
