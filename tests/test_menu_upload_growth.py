"""Execute production menu uploads with allocation/copy spies, including growth failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--phase", choices=["all", "failure"], default="all")
    a = p.parse_args()
    source = (a.source / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    actual = block(source, "struct Bitmap {") + ";\n" + block(source, "struct Upload {") + ";\n"
    template = (ROOT / "tests/menu_upload_growth_test.cpp").read_text(encoding="utf-8")
    fixture = template.replace("/* ACTUAL_STRUCTS */", actual).replace("/* ACTUAL_FUNCTION */", block(source, "void copy_over("))
    with tempfile.TemporaryDirectory(prefix="menu-upload-growth-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-I" + str(a.headers), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe), a.phase], check=True, timeout=30)


if __name__ == "__main__":
    main()
