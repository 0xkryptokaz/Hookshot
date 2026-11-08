"""The swapchain takes an sRGB UNORM surface format, not the last UNORM one in the list.

RADV on Wayland lists every surface format twice, in SRGB_NONLINEAR_KHR and then in PASS_THROUGH_EXT. Keeping the last
B8G8R8A8/R8G8B8A8_UNORM entry chose PASS_THROUGH_EXT: the compositor showed the picture without colour management,
grey on an HDR desktop (Hyprland).
python -I -B tests/test_swapchain_colorspace.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8")
    start = source.index("vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, dev.surface, &n, formats.data());")
    loop = source[start:source.index("vkGetPhysicalDeviceSurfacePresentModesKHR", start)]
    if "VK_FORMAT_B8G8R8A8_UNORM" not in loop:
        raise SystemExit("the surface format choice moved: update this test")
    if "VK_COLOR_SPACE_SRGB_NONLINEAR_KHR" not in loop:
        raise SystemExit("the surface format choice ignores the colour space (PASS_THROUGH_EXT on Wayland)")
    if "break;" not in loop:
        raise SystemExit("the surface format choice keeps the last match instead of the first")
    print("PASS: the swapchain takes the first sRGB UNORM surface format")


if __name__ == "__main__":
    main()
