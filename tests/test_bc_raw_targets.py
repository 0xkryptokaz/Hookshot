"""A BC texture can be rendered through a raw (one texel a block) view, and the pass over it is sized in blocks.

OoT renders the slices of two 64^3 BC4 noise volumes as RGBA16UI targets while an area loads (program 53509044401d78d2).
BC images had no colour attachment usage, so those views were not made; the render area was the texels' 64 x 64 over the
16 x 16 view, and the program was left out by default after it lost the device on first starts. The volumes stayed zero
and the foliage's dithered LOD crossfade, which reads them, left trees bare at mid distance (issue 1151).

python -I -B tests/test_bc_raw_targets.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class BcRawTargetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
        cls.draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")

    def test_bc_images_take_colour_attachment_usage(self):
        raw = self.tex.index("Raw storage views address one compressed block per texel")
        branch = self.tex[raw:self.tex.index("CUBE_COMPATIBLE_BIT", raw)]
        self.assertIn("VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT", branch)
        self.assertRegex(branch, r"x\.usage \|= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;")
        self.assertIn("x.type == VK_IMAGE_TYPE_3D", branch)

    def test_raw_views_of_bc_targets_are_sized_in_blocks(self):
        targets = block(self.draw, "Targets targets(NvnState& s)")
        self.assertRegex(targets, r"if \(bc\(tex->format\) && !bc\(f\)\) w = \(w \+ 3\) / 4, h = \(h \+ 3\) / 4;")
        self.assertLess(targets.index("w = (w + 3) / 4"), targets.index("at.width = at.width ?"))

    def test_the_noise_pass_is_not_left_out_by_default(self):
        line = re.search(r'const char\* list = .*RX_VK_SKIP_PROG.*;', self.draw).group(0)
        self.assertNotIn("53509044401d78d2", line)
        self.assertIn('""', line)


if __name__ == "__main__":
    unittest.main()
