"""The Insert menu's scaling rows: the Upscaler (opts.dlss) and the Render resolution's steps for it (gpu/vk/upscaler_rows.h),
their order on the Graphics tab, and options files from before still loading. No game or GPU required.

python -I -B tests/test_upscaler_rows.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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
    # saved options: the renamed rows keep their keys
    key = menu[menu.index("const char* ini_key(const Row& r) {"):]
    key = key[:key.index("\n}\n")]
    for need in ('&opts.resolution) return "resolution"', '&opts.upscale) return "upscaling"', '&opts.dlss) return "dlss"'):
        if need not in key:
            raise SystemExit("ini_key: %s is missing (an options file from before would lose it)" % need)
    if "ur::step(upscaler(), *r.value, dir)" not in menu or "ur::step_upscaler(upscaler(), dir, true)" not in menu:
        raise SystemExit("the Upscaler and Render resolution rows do not step through upscaler_rows.h")
    fixed = menu[menu.index("bool row_fixed(const int* value) {"):]
    fixed = fixed[:fixed.index("\n}\n")]
    for need in ("ur::upscaler_fixed(dlss_why() == DLSS_OK)", "&opts.reflex && !reflex_available()",
                 "ur::resolution_fixed(upscaler())"):
        if need not in fixed:
            raise SystemExit("row_fixed: %s is missing (a choice that cannot run here would be offered)" % need)
    if "const bool stays = row_fixed(r.value);" not in menu:
        raise SystemExit("the rows step past row_fixed")
    if "if (r.value == &opts.dlss) opts.resolution = ur::snap(upscaler(), opts.resolution);" not in menu:
        raise SystemExit("another Upscaler leaves the Render resolution off its steps")
    dlss = (ROOT / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
    mode = dlss[dlss.index("int dlss_mode() {"):]
    if "dlss_why() != DLSS_OK" not in mode[:mode.index("\n}\n")]:
        raise SystemExit("dlss_mode asks for DLSS where it cannot run (DLAA would hold the 3D at full scale for nothing)")
    with tempfile.TemporaryDirectory(prefix="upscaler-rows-") as tmp:
        exe = Path(tmp) / "upscaler-rows"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests" / "test_upscaler_rows.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
