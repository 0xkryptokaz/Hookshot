"""The game's menu font reader (gpu/vk/vk_game_font.h) compiled with the vendored zstd decoder and run on files of its own
making (tests/test_game_font.cpp): the good case, truncated frames, a bad name offset, an inexact name, a wrong size word and
a wrong font tag. No game file is read.

    python -I -B tests/test_game_font.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ZSTD = ROOT / "third_party/zstd/lib"
SOURCES = ["common/debug.c", "common/entropy_common.c", "common/error_private.c", "common/fse_decompress.c", "common/xxhash.c",
           "common/zstd_common.c", "decompress/huf_decompress.c", "decompress/zstd_ddict.c", "decompress/zstd_decompress.c",
           "decompress/zstd_decompress_block.c"]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    # (the C compiler beside the C++ one: g++ -> gcc, clang++ -> clang, x86_64-...-g++ -> x86_64-...-gcc)
    cc = a.cxx[:-2] if a.cxx.endswith("++") else "cc"
    if Path(cc).name == "g" or cc.endswith("-g"):
        cc += "cc"
    flags = ["-O1", "-DZSTD_DISABLE_ASM", "-DZSTD_LEGACY_SUPPORT=0", "-DZSTD_MULTITHREAD=0", "-I" + str(ZSTD)]
    with tempfile.TemporaryDirectory(prefix="game-font-") as tmp:
        objs = []
        for i, f in enumerate(SOURCES):
            o = Path(tmp) / ("z%d.o" % i)
            subprocess.run([cc, "-c", str(ZSTD / f), "-o", str(o), "-w"] + flags, check=True)
            objs.append(str(o))
        exe = Path(tmp) / "game-font"
        # (-D_CRT_SECURE_NO_WARNINGS: clang on the MSVC headers marks fopen deprecated, an error with -Werror)
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        str(ROOT / "tests/test_game_font.cpp"), "-o", str(exe)]
                       + flags + objs, check=True)
        files = Path(tmp) / "romfs"
        subprocess.run([str(exe), str(files)], check=True)


if __name__ == "__main__":
    main()
