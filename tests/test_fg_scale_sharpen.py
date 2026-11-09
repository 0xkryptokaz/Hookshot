"""The window pass's Sharpness (gpu/vk/shaders/fg_scale.comp): its neighbours are one picture (source) pixel apart, not one
window pixel (inside the same picture pixel when the window is the larger, so Sharpness did next to nothing), and its
per-pixel math, the shader's own sharpen() compiled as C++, is the picture unchanged at 0, never past the lightest or
darkest pixel around it nor outside 0..1 (no halos, no clipping) at any setting, and steeper edges as the setting rises.
With glslangValidator the shader compiles, and with the version the header names, fg_scale_comp_spv.h is its output.

python -I -B tests/test_fg_scale_sharpen.py [--cxx g++] [--glslang glslangValidator]
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


# (just enough of GLSL's vec3 for sharpen())
SHIM = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
struct vec3 {
    float r, g, b;
    vec3() : r(0), g(0), b(0) {}
    explicit vec3(float v) : r(v), g(v), b(v) {}
    vec3(float x, float y, float z) : r(x), g(y), b(z) {}
};
static vec3 map2(vec3 a, vec3 b, float (*f)(float, float)) { return {f(a.r, b.r), f(a.g, b.g), f(a.b, b.b)}; }
static float fadd(float a, float b) { return a + b; }
static float fsub(float a, float b) { return a - b; }
static float fmul(float a, float b) { return a * b; }
static float fdiv(float a, float b) { return a / b; }
static float fmin_(float a, float b) { return std::min(a, b); }
static float fmax_(float a, float b) { return std::max(a, b); }
static vec3 operator+(vec3 a, vec3 b) { return map2(a, b, fadd); }
static vec3 operator-(vec3 a, vec3 b) { return map2(a, b, fsub); }
static vec3 operator*(vec3 a, vec3 b) { return map2(a, b, fmul); }
static vec3 operator/(vec3 a, vec3 b) { return map2(a, b, fdiv); }
static vec3 operator+(float a, vec3 b) { return vec3(a) + b; }
static vec3 operator-(float a, vec3 b) { return vec3(a) - b; }
static vec3 operator*(float a, vec3 b) { return vec3(a) * b; }
static vec3 operator/(vec3 a, float b) { return a / vec3(b); }
static vec3 min(vec3 a, vec3 b) { return map2(a, b, fmin_); }
static vec3 max(vec3 a, vec3 b) { return map2(a, b, fmax_); }
static float min(float a, float b) { return std::min(a, b); }
static float max(float a, float b) { return std::max(a, b); }
static float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
static vec3 clamp(vec3 v, vec3 lo, vec3 hi) { return min(max(v, lo), hi); }
'''

FIXTURE = r'''
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
static unsigned seed = 12345;
static float rnd() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }
static vec3 rnd3() { return {rnd(), rnd(), rnd()}; }
static float ch(vec3 v, int i) { return i == 0 ? v.r : i == 1 ? v.g : v.b; }
// a blurred edge (the picture as it reaches the window pass), sampled one picture pixel apart around x
static float edge(float x) { return 0.1f + 0.8f / (1.0f + std::exp(-2.0f * x)); }
int main() {
    const float amounts[] = {0.0f, 0.1f, 0.4f, 0.8f, 1.0f};
    for (int i = 0; i < 200000; i++) {
        const vec3 c = rnd3(), n = rnd3(), s = rnd3(), w = rnd3(), e = rnd3();
        for (float a : amounts) {
            const vec3 o = sharpen(c, n, s, w, e, a);
            for (int k = 0; k < 3; k++) {
                const float lo = std::min({ch(c, k), ch(n, k), ch(s, k), ch(w, k), ch(e, k)});
                const float hi = std::max({ch(c, k), ch(n, k), ch(s, k), ch(w, k), ch(e, k)});
                CHECK(ch(o, k) >= lo - 1e-6f && ch(o, k) <= hi + 1e-6f);   // no halo: inside the neighbourhood
                CHECK(ch(o, k) >= 0.0f && ch(o, k) <= 1.0f);                 // no clipping
                CHECK(std::isfinite(ch(o, k)));
                if (a == 0.0f) CHECK(ch(o, k) == ch(c, k));                  // 0: the picture unchanged
            }
        }
    }
    // flat light, black and white stay as they are at full strength
    for (float v : {0.0f, 0.5f, 1.0f}) {
        const vec3 o = sharpen(vec3(v), vec3(v), vec3(v), vec3(v), vec3(v), 1.0f);
        CHECK(o.r == v && o.g == v && o.b == v);
    }
    // a blurred edge gets steeper as the setting rises: the pixel on the light side lighter, never past the light beyond it
    float last = edge(0.6f);
    for (float a : {0.25f, 0.5f, 0.75f, 1.0f}) {
        const vec3 c(edge(0.6f)), mid(edge(0.6f));
        const vec3 o = sharpen(c, mid, mid, vec3(edge(-0.4f)), vec3(edge(1.6f)), a);
        CHECK(o.r > last && o.r <= edge(1.6f));
        last = o.r;
    }
    CHECK(last - edge(0.6f) > 0.01f);   // (a visible step, not a rounding)
    std::printf("PASS: sharpen() is the picture at 0, stays inside its neighbourhood and 0..1, and steepens edges with the setting\n");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--glslang", default="glslangValidator")
    a = p.parse_args()
    comp = SHADERS / "fg_scale.comp"
    text = comp.read_text(encoding="utf-8")
    body = block(text, "void main()")
    sharp = body[body.index("if (pc.sharp > 0.0)"):]
    if "textureSize(src, 0)" not in sharp:
        raise SystemExit("the sharpen's neighbours are not spaced by the picture's size (textureSize(src, 0))")
    spacing = re.search(r"const vec2 e = ([^;]*);", sharp)
    if not spacing or "textureSize(src, 0)" not in spacing.group(1):
        raise SystemExit("the neighbour spacing e is not one picture pixel")
    for tap in ("uv + vec2(0.0, -e.y)", "uv + vec2(0.0, e.y)", "uv + vec2(-e.x, 0.0)", "uv + vec2(e.x, 0.0)"):
        if tap not in sharp:
            raise SystemExit("missing the neighbour at %s" % tap)
    lobe = re.search(r"const float kMaxLobe = [^;]*;", text).group(0)
    fn = block(text, "vec3 sharpen(")
    fixture = SHIM + lobe.replace("const float", "static const float") + "\nstatic " + fn + FIXTURE
    with tempfile.TemporaryDirectory(prefix="fg-sharpen-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Wno-unused-function", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        glslang = shutil.which(a.glslang)
        if not glslang:
            print("SKIP: no %s, the shader's compile not checked" % a.glslang)
            return
        out = Path(tmp) / "fg_scale_comp_spv.h"
        r = subprocess.run([glslang, "-V", "--target-env", "vulkan1.2", "--vn", "FG_SCALE_COMP_SPV", "-o", str(out), str(comp)],
                           capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("fg_scale.comp does not compile:\n" + r.stdout + r.stderr)
        header = (SHADERS / "fg_scale_comp_spv.h").read_bytes()
        made = out.read_bytes()
        if made.splitlines()[0] != header.splitlines()[0]:
            print("PASS: the shader compiles (glslang %s, the header's is %s: not compared)" %
                  (made.splitlines()[0].decode().strip(), header.splitlines()[0].decode().strip()))
            return
        if made != header:
            raise SystemExit("fg_scale_comp_spv.h is not fg_scale.comp compiled: glslangValidator -V --target-env vulkan1.2 "
                             "--vn FG_SCALE_COMP_SPV -o fg_scale_comp_spv.h fg_scale.comp")
        print("PASS: fg_scale_comp_spv.h is fg_scale.comp compiled")


if __name__ == "__main__":
    main()
