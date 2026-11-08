"""NVIDIA DLSS's camera jitter for Ocarina of Time 1.1.1: C added after a few instructions of the game (a64_lift.py Lifter.after).

DLSS (gpu/vk/vk_dlss.cpp) resolves detail finer than a pixel from frames whose camera moved by a different fraction of a
pixel each time. The game's cameras do not move so: these sites give the scene camera's projection a sub-pixel offset each
frame. sead's perspective projection keeps an offset of the frustum's centre at +0xb4 (x) and +0xb8 (y), in fractions of
its width and height, which its matrix update (and the game's three inlined copies of it) reads right after the field of
view and aspect ratio at +0xac. The sites are those loads (the same ones nx2/oot_wide.py's ratio patch follows): the
offset is written before it is read, for cameras of the picture's ratio. host/rx_backend.c hos_dlss_jitter picks it; it
is none (the game as shipped) unless DLSS runs, and the renderer reads the offset back from the camera's uniform buffer.
"""

import oot_wide

# (main offset, the original instruction word there, the register the aspect ratio was loaded into, the projection's register)
SITES = (
    (0x3485cc0, 0x2d558400, 1, 0),    # sead's perspective projection's matrix update: ldp s0, s1, [x0, #0xac]
    (0x4f1e8, 0x2d558ea0, 3, 21),     # its inlined copies
    (0x23b9a8, 0x2d558c00, 3, 0),
    (0x33b82c, 0x2d558440, 1, 2),
)


def _text(vreg, base):
    # (16:9, or the window's ratio once the widescreen patch before this one has put it in: the picture's cameras)
    return ("{ extern int hos_dlss_jitter(float out[2]); extern volatile float hos_wide_ratio; float _j[2]; "
            "if (hos_dlss_jitter(_j)) { float _r; memcpy(&_r, c->vreg[%d], 4); const float _w = hos_wide_ratio; "
            "const float _d = _r - (16.0f / 9.0f), _e = _w > 0.0f ? _r - _w : 1.0f; "
            "if ((_d <= 0.01f && _d >= -0.01f) || (_e <= 0.01f && _e >= -0.01f)) { uint32_t _u[2]; memcpy(_u, _j, 8); "
            "recomp_store32(c, c->x[%d] + 0xb4, _u[0]); recomp_store32(c, c->x[%d] + 0xb8, _u[1]); } } }"
            "   /* (nx2/oot_jitter.py: DLSS camera jitter) */" % (vreg, base, base))


def install(lifter, img):
    """The sites into lifter.after, after what is there; a site whose word differs (another game version) is left out"""
    n = 0
    for off, word, vreg, base in SITES:
        pc = oot_wide.MAIN + off
        try:
            have = img.word(pc)
        except Exception:
            have = None
        if have != word:
            print("dlss jitter: main+%#x holds %s, not %08x: left out" % (off, "?" if have is None else "%08x" % have, word))
            continue
        lifter.after[pc] = (lifter.after[pc] + "\n" if pc in lifter.after else "") + _text(vreg, base)
        n += 1
    print("dlss jitter: %d of %d sites" % (n, len(SITES)))
    return n
