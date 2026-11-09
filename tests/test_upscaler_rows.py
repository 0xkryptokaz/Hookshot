"""The Insert menu's scaling rows: the Upscaler (opts.dlss) and the Render resolution's steps for it (gpu/vk/upscaler_rows.h),
their order on the Graphics tab, and options files from before still loading. No game or GPU required.

python -I -B tests/test_upscaler_rows.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, start):
    """A function's body from its first line to its closing brace at the start of a line"""
    at = source.find(start)
    if at < 0:
        raise SystemExit("%s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    # the picture's way, top to bottom; Reflex with the frame rows
    order = ['"Frame generation", &opts.framegen', '"Frame pacing", &opts.pace', '"NVIDIA Reflex", &opts.reflex',
             '"Output resolution", &opts.output_res', '"Upscaler", &opts.dlss', '"Render resolution", &opts.resolution',
             '"Window scaling filter", &opts.upscale', '"Sharpness", &opts.sharpness']
    at = [menu.find(row) for row in order]
    if -1 in at:
        raise SystemExit("vk_menu.cpp: a scaling row is missing: " + order[at.index(-1)])
    if at != sorted(at):
        raise SystemExit("vk_menu.cpp: the scaling rows are not in the picture's order")
    if '"NVIDIA DLSS", &opts.dlss' in menu:
        raise SystemExit("vk_menu.cpp: DLSS still has a row of its own")
    # saved options: the renamed rows keep their keys (data, so read from the source)
    key = function(menu, "const char* ini_key(const Row& r) {")
    for value, name in (("opts.resolution", "resolution"), ("opts.upscale", "upscaling"), ("opts.dlss", "dlss")):
        if not re.search(r"&%s\)\s*return\s*\"%s\"" % (re.escape(value), name), key):
            raise SystemExit("ini_key: %s is not saved as %s (an options file from before would lose it)" % (value, name))
    # the menu's rules are upscaler_rows.h's, checked by behaviour in test_upscaler_rows.cpp: here only that the menu uses them
    uses = {
        "row_fixed": ("ur::upscaler_fixed(", "ur::resolution_fixed(", "reflex_available()"),
        "row_shown": ("ur::upscaler_shown(dlss_built())",),
        "set_row": ("ur::step_upscaler(", "ur::step(", "ur::snap(", "row_fixed("),
    }
    for name, calls in uses.items():
        body = function(menu, "%s %s(" % ("void" if name == "set_row" else "bool", name))
        for call in calls:
            if call not in body:
                raise SystemExit("%s does not use %s (upscaler_rows.h)" % (name, call))
    dlss = (ROOT / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
    if "dlss_why() != DLSS_OK" not in function(dlss, "int dlss_mode() {"):
        raise SystemExit("dlss_mode asks for DLSS where it cannot run (DLAA would hold the 3D at full scale for nothing)")
    with tempfile.TemporaryDirectory(prefix="upscaler-rows-") as tmp:
        exe = Path(tmp) / "upscaler-rows"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests" / "test_upscaler_rows.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
