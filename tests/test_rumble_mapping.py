"""The game's HD rumble goes to a controller's heavy and light motors by frequency, scaled by the menu's Rumble strength.

host/nn_hid.c rumble_map: each band's strength (rumble_motor) is split between the heavy and light motors by its
frequency (rumble_heavy_share: all heavy up to 160 Hz, all light from 240 Hz, a constant-power crossfade between). At
the Switch's default frequencies and the ones OoT sends, every band lands on the motor it did before, at the same
strength. host/pad_sdl.c rumble_strength: the Insert menu's Rumble strength, in percent, on a motor's 0..65535.

Run with: python -I -B tests/test_rumble_mapping.py
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HID = ROOT / "host" / "nn_hid.c"
PAD = ROOT / "host" / "pad_sdl.c"


def function(path, name):
    text = path.read_text(encoding="utf-8")
    found = re.search(re.escape(name) + r"\([^;{]*\)\s*\{", text)   # (the definition, not a declaration)
    if not found:
        raise RuntimeError("no definition of " + name)
    at = found.start()
    start = found.end() - 1
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return text[at:i + 1]
    raise RuntimeError("no end for " + name)


def defines(path, prefix):
    return "\n".join(re.findall(r"^#define %s\w* .*$" % prefix, path.read_text(encoding="utf-8"), re.M))


FIXTURE = r"""
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %%d %%s\n", __LINE__, #x); return 1; } } while (0)
%(defines)s
%(motor)s
%(share)s
%(map)s
%(strength)s
/* (the mapping before the frequency split: each band its own motor, (a + a^0.35) / 2 and less above a start frequency) */
static float old_band(float amp, float freq, float start, float width) {
    if (amp <= 0.0f) return 0.0f;
    const float scale = freq > start ? fmaxf(1.0f - (freq - start) / width, 0.3f) : 1.0f;
    return fminf((amp + powf(amp, 0.35f)) * 0.5f * scale, 1.0f);
}
static int near(float a, float b) { return fabsf(a - b) <= 1e-5f; }
int main(void) {
    /* 100%%, the defaults and what OoT sends (low about 100-130 Hz, high 250-300 Hz): the same as before, where the low band
       drove the heavy motor and the high band the light one, each with rumble_motor's falloff */
    static const float amps[] = {0.0f, 0.005f, 0.01f, 0.02f, 0.06f, 0.3f, 1.0f};
    static const float lows[] = {40.0f, 102.0f, 116.0f, 126.0f, 140.0f, 160.0f};
    static const float highs[] = {240.0f, 252.0f, 287.0f, 300.0f, 320.0f, 600.0f, 1250.0f};
    for (int a = 0; a < 7; a++)
        for (int b = 0; b < 7; b++)
            for (int l = 0; l < 6; l++)
                for (int h = 0; h < 7; h++) {
                    const float v[4] = {amps[a], lows[l], amps[b], highs[h]};
                    float m[2];
                    rumble_map(v, m);
                    CHECK(near(m[0], old_band(v[0], v[1], 140.0f, 400.0f)));
                    CHECK(near(m[1], old_band(v[2], v[3], 200.0f, 700.0f)));
                    const unsigned lo = (unsigned)(m[0] * 65535), hi = (unsigned)(m[1] * 65535);   /* (hos_sdl_rumble) */
                    CHECK(rumble_strength(lo, 100) == lo && rumble_strength(hi, 100) == hi);
                }
    {   /* the idle value (GetActualVibrationValue) is silence */
        const float v[4] = {0.0f, 160.0f, 0.0f, 320.0f};
        float m[2];
        rumble_map(v, m);
        CHECK(m[0] == 0.0f && m[1] == 0.0f);
    }

    /* the share: all heavy up to 160 Hz, all light from 240 Hz, falling between, half at their geometric middle */
    CHECK(rumble_heavy_share(40.0f) == 1.0f && rumble_heavy_share(160.0f) == 1.0f);
    CHECK(rumble_heavy_share(240.0f) == 0.0f && rumble_heavy_share(1250.0f) == 0.0f);
    CHECK(rumble_heavy_share(0.0f) == 1.0f && rumble_heavy_share(NAN) == 1.0f);
    CHECK(near(rumble_heavy_share(sqrtf(160.0f * 240.0f)), 0.5f));
    float last = 1.0f;
    for (float f = 160.0f; f <= 240.0f; f += 0.5f) {
        const float s = rumble_heavy_share(f);
        CHECK(s >= 0.0f && s <= 1.0f && s <= last);
        last = s;
    }

    {   /* a high band played low reaches the heavy motor, at its full strength */
        const float v[4] = {0.0f, 160.0f, 0.3f, 100.0f};
        float m[2];
        rumble_map(v, m);
        CHECK(m[0] == rumble_motor(0.3f, 100.0f, 200.0f, 700.0f) && m[0] > 0.0f && m[1] == 0.0f);
    }
    {   /* a low band played high, the light one (with the low band's falloff) */
        const float v[4] = {0.3f, 320.0f, 0.0f, 320.0f};
        float m[2];
        rumble_map(v, m);
        CHECK(m[0] == 0.0f && m[1] == rumble_motor(0.3f, 320.0f, 140.0f, 400.0f) && m[1] > 0.0f);
    }
    for (float f = 161.0f; f < 240.0f; f += 7.0f) {   /* in the crossfade, both motors, at constant power */
        const float v[4] = {0.3f, f, 0.0f, 320.0f};
        float m[2];
        rumble_map(v, m);
        const float band = rumble_motor(0.3f, f, 140.0f, 400.0f);
        CHECK(m[0] > 0.0f && m[1] > 0.0f && m[0] <= band && m[1] <= band);
        CHECK(near(m[0] * m[0] + m[1] * m[1], band * band));
    }

    /* the strength: Off is silence, it grows with the percentage and the motor's value, and stops at the top */
    static const int percents[] = {0, 25, 50, 75, 100, 125, 150};
    static const unsigned values[] = {0, 1, 300, 6880, 20000, 43690, 50000, 65535};
    for (int i = 0; i < 8; i++) CHECK(rumble_strength(values[i], 0) == 0 && rumble_strength(values[i], -5) == 0);
    for (int i = 0; i < 8; i++)
        for (int p = 1; p < 7; p++) CHECK(rumble_strength(values[i], percents[p]) >= rumble_strength(values[i], percents[p - 1]));
    for (int p = 0; p < 7; p++)
        for (int i = 1; i < 8; i++) CHECK(rumble_strength(values[i], percents[p]) >= rumble_strength(values[i - 1], percents[p]));
    CHECK(rumble_strength(20000, 50) == 10000 && rumble_strength(20000, 150) == 30000);
    CHECK(rumble_strength(50000, 150) == 0xFFFF && rumble_strength(65535, 150) == 0xFFFF && rumble_strength(65535, 125) == 0xFFFF);
    CHECK(rumble_strength(43690, 150) == 65535);
    return 0;
}
"""


class RumbleMappingTests(unittest.TestCase):
    def test_the_poll_scales_both_motors_by_the_menu(self):
        poll = function(PAD, "static void pads_poll")
        self.assertIn("rx_option(9, 100)", poll)
        self.assertIn("rumble_strength((uint16_t)rumble_low, strength)", poll)
        self.assertIn("rumble_strength((uint16_t)rumble_high, strength)", poll)
        self.assertLess(poll.index("rumble_strength("), poll.index("rumble_due("))   # (Off is a change: a stop is sent)
        self.assertIn("rumble_map(v, amp[dev])", function(HID, "static void vibration_send"))

    def test_mapping_and_strength(self):
        fixture = FIXTURE % {
            "defines": defines(HID, "RUMBLE_"),
            "motor": function(HID, "static float rumble_motor"),
            "share": function(HID, "static float rumble_heavy_share"),
            "map": function(HID, "static void rumble_map"),
            "strength": function(PAD, "static uint16_t rumble_strength"),
        }
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "rumble.c")
            with open(path, "w", encoding="utf-8") as f:
                f.write(fixture)
            exe = os.path.join(d, "rumble")
            cc = os.environ.get("CC", "cc")
            made = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", path, "-o", exe, "-lm"],
                                  capture_output=True, text=True)
            self.assertEqual(made.returncode, 0, made.stderr)
            ran = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)


if __name__ == "__main__":
    unittest.main()
