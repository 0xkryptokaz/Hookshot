"""A masked clear whose layout, shader module or pipeline the driver did not make is not kept, and is tried again (#1787).

clear_pipeline (gpu/vk/vk_draw.cpp) makes the full-screen-triangle pipeline of a masked clear on its first use. It ignored
what vkCreatePipelineLayout and vkCreateShaderModule returned, so a failed layout or module went on into the next calls,
and it stored whatever cached_graphics gave back: a pipeline the driver refused was cached as VK_NULL_HANDLE, and that clear
was left out for the rest of the run, even when the failure was transient. Now each step's result is checked, a failure
keeps nothing (the vertex module is freed when the fragment module fails), and the key is tried again on its next clear,
at most kClearTries times, so a driver that always refuses it costs no compile every frame.

python -I -B tests/test_masked_clear_failure.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: clear_pipeline: " + why)


def main():
    draw = DRAW.read_text(encoding="utf-8")
    body = block(draw, "VkPipeline clear_pipeline(const Targets& at, int index, uint32_t mask, int kind) {")
    need(re.search(r"constexpr int kClearTries = \d+;", draw), "no bound on the tries of a key that failed")
    need("if (failed >= kClearTries) return VK_NULL_HANDLE;" in body, "a key that failed kClearTries times is tried again")
    need(body.index("if (failed >= kClearTries)") < body.index("vkCreateShaderModule"),
         "the bound is checked after the work it should save")
    # (every creation's result is looked at, and a failure goes out through fail, which counts it)
    for call, check in (("vkCreatePipelineLayout(", "if (lr != VK_SUCCESS || !made) return fail("),
                        ("vkCreateShaderModule(dev.device, &mi, nullptr, &vm)", "if (vr != VK_SUCCESS || !vm) return fail("),
                        ("vkCreateShaderModule(dev.device, &mi, nullptr, &fm)", "if (fr != VK_SUCCESS || !fm) {"),
                        ("cached_graphics(&ci, &pl)", "if (pr != VK_SUCCESS || !pl) {")):
        need(call in body, "lost the call " + call)
        need(check in body, "the result of %s is not checked" % call)
        need(body.index(call) < body.index(check), "the result of %s is checked before the call" % call)
    need(re.search(r"VkResult \w+ = vkCreatePipelineLayout\(dev\.device, &li, nullptr, &made\);", body),
         "the layout is made straight into clear_layout: a failed one would be kept")
    need(body.index("clear_layout = made;") > body.index("if (lr != VK_SUCCESS || !made)"), "a failed layout is kept")
    frag = block(body, "if (fr != VK_SUCCESS || !fm) {")
    need("vkDestroyShaderModule(dev.device, vm, nullptr);" in frag, "the vertex module leaks when the fragment one fails")
    pipe = block(body, "if (pr != VK_SUCCESS || !pl) {")
    need("return fail(" in pipe, "a failed pipeline is not counted as a failure")
    need(body.index("if (pr != VK_SUCCESS || !pl)") < body.index("clear_pipes[key] = pl;"),
         "the pipeline is cached before its result is checked")
    need(body.count("clear_pipes[key] =") == 1, "a pipeline is cached on more than one path")
    need("clear_failures.erase(key);" in body, "a key that worked keeps its count of failures")
    print("a masked clear that failed is not kept and is tried again, a bounded number of times: ok")


if __name__ == "__main__":
    main()
