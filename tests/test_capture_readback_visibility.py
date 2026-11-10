"""Run production capture reads against separate device bytes and a stale CPU cache."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--phase", choices=["all", "normal", "made", "log"], default="all")
    a = p.parse_args()
    source = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    start = source.index("        auto take = [&](uint64_t at_value, int64_t at_present) {")
    end = source.index("            // (RX_VK_DUMP_BLACK", start)
    take = source[start:end] + "\n(void)at_present; return px; };\n"
    helper_start = source.index("std::shared_ptr<std::vector<uint8_t>> shot_pixels(")
    helper_end = source.index("void do_present(", helper_start)
    helper = source[helper_start:helper_end]
    start = source.index("        if (shot && made_src && made_map) {")
    end = source.index("        } else if (shot)", start)
    made = source[start:end] + "\n}\n"
    start = source.index("void framelog_collect(int slot) {")
    end = source.index("    {   // (a picture blown out", start)
    collect = source[start:end] + "\ncaptured = std::move(rec); }\n"
    start = source.index("    vkWaitForFences(dev.device, 1, &f.done, VK_TRUE, UINT64_MAX);")
    end = source.index("    uint32_t index = 0;", start)
    fence = source[start:end]
    template = (ROOT / "tests/capture_readback_visibility_test.cpp").read_text(encoding="utf-8")
    fixture = template.replace("/* SHOT_PIXELS */", helper).replace("/* TAKE */", take).replace("/* MADE */", made).replace("/* COLLECT */", collect).replace("/* FENCE */", fence)
    with tempfile.TemporaryDirectory(prefix="capture-visibility-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(a.headers),
                        str(cpp), "-o", str(exe)], check=True)
        for phase in (["normal", "made", "log"] if a.phase == "all" else [a.phase]):
            for cache in ["noncoherent", "coherent"]:
                subprocess.run([str(exe), phase, cache], check=True, timeout=30)


if __name__ == "__main__":
    main()
