"""The translator is told the truth about a GPU whose graphics stages run wider than NVN's 32-lane warps.

AMD's GPUs pin only compute to 32 lanes (requiredSubgroupSizeStages 0x20 on an RX 5500M): a fragment stage runs at 64, so the
profile asks for the long path that keeps each 32-lane guest warp to its half (warp_size_potentially_larger_than_guest), and
the broadcast fallbacks walk all the host's lanes. On AMD's proprietary driver fragment-stage shuffles take the broadcast
fallback (Lichii #273: a basket in Link's house drew as a white blob), and a depth-compare texture is never an integer image
(#273: the shadow samples of the lighting). The profile's choices are in the program cache's stamp.

python -I -B tests/test_amd_fragment_warp.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
SPIRV = VK / "eden" / "shader_recompiler" / "backend" / "spirv"


def main():
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    profile = shader[shader.index("Shader::Profile make_profile()"):shader.index("Shader::HostTranslateInfo host_info()")]
    for needle, why in (
        ("dev.subgroup_size > 32 && (dev.sg32_stages & VK_SHADER_STAGE_ALL_GRAPHICS)",
         "a graphics stage running wider than 32 lanes is not told to the translator"),
        ("p.max_subgroup_size = p.warp_size_potentially_larger_than_guest ? dev.subgroup_max : 32",
         "the broadcast fallbacks do not walk the host's upper lanes"),
        ("p.has_broken_spirv_fragment_shuffle = dev.driver_id == VK_DRIVER_ID_AMD_PROPRIETARY",
         "AMD's proprietary driver keeps native fragment-stage shuffles"),
    ):
        if needle not in profile:
            raise SystemExit(why)
    # (stages pinned to 32 lanes keep the short path: the long one in every compute program was new, longer SPIR-V that AMD
    # compiled for hours, and they need none of it)
    if "EmitSPIRV(profile_pinned(), rt, t->program, bindings)" not in shader:
        raise SystemExit("compute programs take the long warp path though they are pinned to 32 lanes")
    if shader.count("EmitSPIRV(fallback ? profile_pinned() : profile()") != 2:
        raise SystemExit("the mesh fallback's compute stages take the long warp path though they are pinned to 32 lanes")
    stamp = shader[shader.index("uint64_t disk_stamp()"):]
    if "profile().warp_size_potentially_larger_than_guest" not in stamp.split("return")[0]:
        raise SystemExit("programs translated for another GPU's warps are kept in the program cache")
    main_cpp = (VK / "vk_main.cpp").read_text(encoding="utf-8")
    if "dev.driver_id = drv.driverID" not in main_cpp or "dev.subgroup_size = sg.subgroupSize" not in main_cpp:
        raise SystemExit("the driver or its subgroup size is not read")
    warp = (SPIRV / "emit_spirv_warp.cpp").read_text(encoding="utf-8")
    if "ctx.profile.has_broken_spirv_fragment_shuffle && ctx.stage == Stage::Fragment" not in warp:
        raise SystemExit("fragment-stage shuffles do not take the broadcast fallback")
    if warp.count("UseShuffleFallback(ctx)") < 2:
        raise SystemExit("absolute and relative shuffles do not both take the fallback")
    texture = (VK / "eden" / "shader_recompiler" / "ir_opt" / "texture_pass.cpp").read_text(encoding="utf-8")
    if "!flags.is_depth && is_texture_pixel_format_integer(cbuf)" not in texture:
        raise SystemExit("a depth-compare texture can be an integer image")
    print("AMD fragment warps, shuffles and depth-compare textures ok")


if __name__ == "__main__":
    main()
