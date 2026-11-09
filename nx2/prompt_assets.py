"""(hookshot) The button prompts of other controllers: the game's button-icon font redrawn per controller face, from the user's RomFS
and Kenney's Input Prompts (CC0, third_party/kenney_input).

    python -I nx2/prompt_assets.py --romfs <dir> --out <game folder>/prompts

Writes <out>/<face>/External.bfotf for each face (switch, xbox, ps4, ps5, steamdeck). Every text in the game draws its button icons
from Font2/Scalable/External.*.bfotf (dialogue, the control hints, menus): A B X Y at U+E040..E043, L R ZL ZR at E044..E047, Plus
and Minus at E04B and E04C, the stick presses at E056 and E057, the d-pad at E060..E063. Each written font has one icon per pad
input at U+F700 + HOS_PAD_* (gpu/vk/vk_api.h), and each of those Switch buttons' code points in a cmap segment of its own: the
game (host/prompts.c) points each one at the icon of the pad input bound to that button when it serves the font, so the icons
follow the controls, changed ones too.

The textures with button icons (the ocarina's notes, the letters on black, the controller in the pause menu) the same way, with
Pillow, astc_encoder and texture2ddecoder: <out>/Common.bntx is the game's UI/Layout2Archive/Common bntx decompressed, and
<face>/textures.bin the encoded blocks to write over it for each Switch button on each pad input, which the game picks by the
controls when it serves the file.

The faces' fonts are larger than the game's, and the game drops a file larger than its entry in the resource size table: <out>/
rsizetable.zs is the game's table with the font's entry raised to fit the largest, served in its place.

A .bfotf is an OpenType font behind an 8-byte header and XORed per little-endian word: the header is the magic 0x18029a7f and the
font's size (big-endian), each XORed with the same key as the font. The .zs files are zstd frames made with the dictionary zs.zsdic
of Pack/ZsDic.pack.zs.
"""
import argparse
import io
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KENNEY = os.path.join(os.path.dirname(HERE), "third_party", "kenney_input")
FONT = "Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs"
MAGIC = 0x18029A7F
PAD_BASE = 0xF700
WIDE = 1.6   # (how much wider than the game's icon a wide Kenney icon may be)
# the HOS_PAD_* inputs (gpu/vk/vk_api.h), in order
PADS = ["south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder", "up", "down", "left",
        "right", "ltrigger", "rtrigger"]
# the Switch buttons' code points, in HOS_SW_* order (A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down), and hookshot's
# default controls for them (host/nn_hid.c pad_default: by position, A the right face button)
SWITCH = [0xE040, 0xE041, 0xE042, 0xE043, 0xE056, 0xE057, 0xE044, 0xE045, 0xE046, 0xE047, 0xE04B, 0xE04C, 0xE062, 0xE060, 0xE063,
          0xE061]
DEFAULT = ["east", "south", "north", "west", "lstick", "rstick", "lshoulder", "rshoulder", "ltrigger", "rtrigger", "start", "back",
           "left", "up", "right", "down"]
# the game's own icon for each pad input: what the Switch face shows
SWITCH_ICON = {"south": 0xE041, "east": 0xE040, "west": 0xE043, "north": 0xE042, "back": 0xE04C, "guide": 0xE04A, "start": 0xE04B,
               "lstick": 0xE056, "rstick": 0xE057, "lshoulder": 0xE044, "rshoulder": 0xE045, "up": 0xE060, "down": 0xE061,
               "left": 0xE062, "right": 0xE063, "ltrigger": 0xE046, "rtrigger": 0xE047}
# the icons that do not follow the controls, by the Kenney glyph's name after the face's prefix: the whole d-pad (the sticks keep the
# game's icons, an L or R cap with its arrows: right for any controller, where Kenney's are drawn in perspective)
STATIC = {0xE049: "dpad", 0xE064: "dpad_vertical_outline", 0xE065: "dpad_horizontal_outline"}
_DPAD = {"up": "dpad_up_outline", "down": "dpad_down_outline", "left": "dpad_left_outline",
         "right": "dpad_right_outline"}   # (the outlined pad with the arm meant filled in)
# face: (Kenney font, glyph prefix, the pad inputs' glyphs after the prefix; no Guide: the makers' logos are their trademarks, every
# face keeps the game's Home icon)
FACES = {
    "xbox": ("kenney_input_xbox_series.otf", "xbox_", dict(
        _DPAD, south="button_a", east="button_b", west="button_x", north="button_y", back="button_view",
        start="button_menu", lshoulder="lb", rshoulder="rb", ltrigger="lt", rtrigger="rt", lstick="ls", rstick="rs")),
    "ps4": ("kenney_input_playstation_series.otf", "playstation_", dict(
        _DPAD, south="button_cross", east="button_circle", west="button_square", north="button_triangle",
        back="/playstation4_button_share", start="/playstation4_button_options", lshoulder="trigger_l1", rshoulder="trigger_r1",
        ltrigger="trigger_l2", rtrigger="trigger_r2", lstick="button_l3", rstick="button_r3")),
    "ps5": ("kenney_input_playstation_series.otf", "playstation_", dict(
        _DPAD, south="button_cross", east="button_circle", west="button_square", north="button_triangle",
        back="/playstation5_button_create", start="/playstation5_button_options", lshoulder="trigger_l1",
        rshoulder="trigger_r1", ltrigger="trigger_l2", rtrigger="trigger_r2", lstick="button_l3", rstick="button_r3")),
    "steamdeck": ("kenney_input_steam_deck.otf", "steamdeck_", dict(
        _DPAD, south="button_a", east="button_b", west="button_x", north="button_y", back="button_view",
        start="button_options", lshoulder="button_l1", rshoulder="button_r1", ltrigger="button_l2", rtrigger="button_r2")),
}


def zs_dictionary(romfs):
    """zs.zsdic, out of Pack/ZsDic.pack.zs (a SARC, itself zstd without a dictionary)"""
    import zstandard
    with open(os.path.join(romfs, "Pack", "ZsDic.pack.zs"), "rb") as f:
        b = zstandard.ZstdDecompressor().decompressobj().decompress(f.read())
    data, = struct.unpack_from("<I", b, 12)
    hl, = struct.unpack_from("<H", b, 4)
    n, = struct.unpack_from("<H", b, hl + 6)
    names = hl + 12 + 16 * n + 8
    for i in range(n):
        attr, start, end = struct.unpack_from("<III", b, hl + 12 + 16 * i + 4)
        o = names + (attr & 0xFFFFFF) * 4
        if b[o:b.index(b"\0", o)] == b"zs.zsdic":
            return b[data + start:data + end]
    raise SystemExit("prompts: no zs.zsdic in Pack/ZsDic.pack.zs")


def read_zs(romfs, rel, dictionary):
    import zstandard
    d = zstandard.ZstdDecompressor(dict_data=zstandard.ZstdCompressionDict(dictionary))
    with open(os.path.join(romfs, rel), "rb") as f:
        return d.decompressobj().decompress(f.read())


def size_table(romfs, dictionary, game_size, font_size):
    """the game's resource size table (System/Resource/ResourceSizeTable.*.rsizetable.zs: "RESTBL", u32 version, u32 name length,
    u32 hashed entries, u32 named entries; the hashed ones u32 CRC-32 of the path, u32 size, by hash) with the External font's
    entry raised from the game's font (game_size bytes) to fit one of font_size: the game drops a file larger than its entry. None
    when it fits already"""
    import zlib
    folder = os.path.join(romfs, "System", "Resource")
    name = next((n for n in sorted(os.listdir(folder)) if n.startswith("ResourceSizeTable.") and n.endswith(".rsizetable.zs")),
                None)
    if not name:
        raise SystemExit("prompts: no System/Resource/ResourceSizeTable.*.rsizetable.zs")
    b = bytearray(read_zs(romfs, f"System/Resource/{name}", dictionary))
    if b[:6] != b"RESTBL":
        raise SystemExit(f"prompts: {name} is not a RESTBL table")
    hashed, = struct.unpack_from("<I", b, 14)
    want = zlib.crc32(FONT[:-3].encode())
    for i in range(hashed):
        at = 22 + 8 * i
        h, size = struct.unpack_from("<II", b, at)
        if h == want:
            fit = font_size + size - game_size   # (the entry's allowance over the file kept: 288 bytes)
            if fit <= size:
                return None
            struct.pack_into("<I", b, at + 4, fit)
            return bytes(b)
    raise SystemExit(f"prompts: {name} has no entry for {FONT[:-3]}")


def unwrap(blob):
    key = struct.unpack_from("<I", blob, 0)[0] ^ MAGIC
    size = struct.unpack(">I", struct.pack("<I", struct.unpack_from("<I", blob, 4)[0] ^ key))[0]
    body = b"".join(struct.pack("<I", w ^ key) for w in struct.unpack_from(f"<{(len(blob) - 8) // 4}I", blob, 8))
    return key, body[:size]


def wrap(key, otf):
    otf += b"\0" * (-len(otf) % 4)
    head = struct.pack("<II", MAGIC ^ key, struct.unpack("<I", struct.pack(">I", len(otf)))[0] ^ key)
    return head + b"".join(struct.pack("<I", w ^ key) for w in struct.unpack(f"<{len(otf) // 4}I", otf))


def cmap4(mapping, single):
    """a cmap table of one format 4 subtable (for Unicode BMP, platforms 0 and 3) from {code: glyph id}: runs of codes with
    consecutive glyphs share a segment, and each code of `single` has one of its own; every segment by idDelta alone"""
    segs = []
    for c in sorted(mapping):
        g = mapping[c]
        if segs and c not in single and segs[-1][1] == c - 1 and segs[-1][1] not in single and \
                (segs[-1][2] + c) & 0xFFFF == g:
            segs[-1][1] = c
        else:
            segs.append([c, c, (g - c) & 0xFFFF])
    segs.append([0xFFFF, 0xFFFF, 1])
    n = len(segs)
    search = 2 * (1 << (n.bit_length() - 1))
    sub = struct.pack(">HHHHHHH", 4, 16 + 8 * n, 0, 2 * n, search, (search // 2).bit_length() - 1, 2 * n - search)
    sub += b"".join(struct.pack(">H", s[1]) for s in segs) + b"\0\0"
    sub += b"".join(struct.pack(">H", s[0]) for s in segs)
    sub += b"".join(struct.pack(">H", s[2]) for s in segs)
    sub += b"\0\0" * n
    return struct.pack(">HHHHIHHI", 0, 2, 0, 3, 20, 3, 1, 20) + sub


def face_font(otf, face):
    """the game's font (decrypted) with this face's icons: a pad icon per input at PAD_BASE + its number, and the Switch buttons'
    code points on the default controls' icons"""
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.tables.DefaultTable import DefaultTable

    game = TTFont(io.BytesIO(otf))
    gset, cmap = game.getGlyphSet(), dict(game.getBestCmap())
    top = game["CFF "].cff.topDictIndex[0]
    chars = top.CharStrings
    order = list(game.getGlyphOrder())

    def bounds(glyphset, name):
        p = BoundsPen(glyphset)
        glyphset[name].draw(p)
        return p.bounds

    def add(name, charstring, width):
        chars.charStringsIndex.append(charstring)
        chars.charStrings[name] = len(chars.charStringsIndex) - 1
        top.charset.append(name)
        order.append(name)
        game["hmtx"].metrics[name] = (width, 0)

    if face == "switch":
        icon = {p: cmap[SWITCH_ICON[p]] for p in PADS}
        for i, p in enumerate(PADS):
            src = chars[icon[p]]
            src.decompile()
            add(f"pad_{p}", src, game["hmtx"][icon[p]][0])
    else:
        fname, prefix, names = FACES[face]
        kenney = TTFont(os.path.join(KENNEY, fname))
        kset = kenney.getGlyphSet()

        def kname(s):
            return s[1:] if s.startswith("/") else prefix + s

        def draw(target, kn):
            """the Kenney glyph scaled into the box of the game's glyph `target` (as a square around its centre); a wide one (the
            Menu and View pills on the square Plus and Minus) as high as the box, up to WIDE times as wide, the glyph widened"""
            gb, kb = bounds(gset, target), bounds(kset, kn)
            side = max(gb[2] - gb[0], gb[3] - gb[1])
            s = min(side * WIDE / (kb[2] - kb[0]), side / (kb[3] - kb[1]))
            over = max(0, s * (kb[2] - kb[0]) - side)
            dx = (gb[0] + gb[2] + over) / 2 - s * (kb[0] + kb[2]) / 2
            dy = (gb[1] + gb[3]) / 2 - s * (kb[1] + kb[3]) / 2
            width = game["hmtx"][target][0] + round(over)
            pen = T2CharStringPen(width, gset)
            kset[kn].draw(TransformPen(pen, (s, 0, 0, s, dx, dy)))
            cs = chars[target]
            return pen.getCharString(private=cs.private, globalSubrs=cs.globalSubrs), width

        for p in PADS:
            kn = kname(names[p]) if p in names else None
            if kn not in kset:   # (no such button on this face, the PlayStation's PS button: the game's own icon)
                kn = None
            target = cmap[SWITCH_ICON[p]]
            if kn:
                add(f"pad_{p}", *draw(target, kn))
            else:
                src = chars[target]
                src.decompile()
                add(f"pad_{p}", src, game["hmtx"][target][0])
        for c, s in STATIC.items():
            kn = prefix + s
            if c in cmap and kn in kset:
                cs, _ = draw(cmap[c], kn)
                chars[cmap[c]] = cs

    game.setGlyphOrder(order)
    gid = {n: i for i, n in enumerate(order)}
    mapping = {c: gid[n] for c, n in cmap.items() if c <= 0xFFFF}
    for i, p in enumerate(PADS):
        mapping[PAD_BASE + i] = gid[f"pad_{p}"]
    for c, p in zip(SWITCH, DEFAULT):
        mapping[c] = gid[f"pad_{p}"]
    t = DefaultTable("cmap")
    t.data = cmap4(mapping, set(SWITCH))
    game["cmap"] = t
    out = io.BytesIO()
    game.save(out)
    return out.getvalue()


# ---- the textures with button icons (UI/Layout2Archive/Common.*.bntx: 878 textures, ASTC 4x4 and BC4, the Switch 2's block-linear
# layout). prompts/Common.bntx is the game's file, decompressed; prompts/<face>/textures.bin the blocks to write over it, for each
# Switch button and each pad input it can be bound to (host/prompts.c writes those of the controls in use):
#     "HPTX", u32 count, then count times: u8 Switch button (HOS_SW_*), u8 pad input (HOS_PAD_*), u16 0, u32 runs,
#     runs times: u32 offset in the file, u32 length, the bytes
TEXTURES = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs"
SW = {n: i for i, n in enumerate(["a", "b", "x", "y", "ls", "rs", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down"])}
# the ocarina's notes (white icons, dark letters) and the white letters on black (KeyTexFont, BC4)
NOTES = {"OcarinaButtonA_00": "a", "OcarinaButtonB_00": "b", "OcarinaButtonX_00": "x", "OcarinaButtonY_00": "y",
         "OcarinaButtonL_00": "l", "OcarinaButtonR_00": "r"}
LETTERS = {"KeyTexFontA_00": "a", "KeyTexFontB_00": "b", "KeyTexFontX_00": "x", "KeyTexFontY_00": "y"}
# the Pro Controller in the pause menu's controls page: the face buttons' discs (centre, radius in pixels), by the pad input
# there (a picture of the controller: its own labels, whatever the controls)
PICTURES = ("Controller_Pro", "Controller_Pro1")
PICTURE_BUTTONS = {"east": (869, 227, 31), "south": (800, 286, 31), "north": (800, 168, 31), "west": (732, 227, 31)}


def bntx_textures(b):
    """{name: (BRTI offset, format, width, height, GOBs per block, data offset)} of a Switch 2 BNTX (platform "Ounc")"""
    count, = struct.unpack_from("<I", b, 0x24)
    info, = struct.unpack_from("<Q", b, 0x28)
    out = {}
    for i in range(count):
        p, = struct.unpack_from("<Q", b, info + 8 * i)
        no, = struct.unpack_from("<Q", b, p + 0x60)
        nl, = struct.unpack_from("<H", b, no)
        fmt, = struct.unpack_from("<I", b, p + 0x1C)
        w, h = struct.unpack_from("<II", b, p + 0x24)
        bh, = struct.unpack_from("<I", b, p + 0x34)
        mips, = struct.unpack_from("<Q", b, p + 0x70)
        data, = struct.unpack_from("<Q", b, mips)
        out[b[no + 2:no + 2 + nl].decode()] = (p, fmt, w, h, 1 << bh, data)
    return out


def gob_offset(x, y, gobs, pitch):
    """a byte's place in a block-linear texture (x in bytes, y in rows of blocks): the Switch 2's GOB, the Tegra X1's with
    offset bits 5 and 6 swapped (gpu/render/rx_nvn.cpp bl_offset)"""
    a = (y // (8 * gobs)) * 512 * gobs * pitch + (x // 64) * 512 * gobs + (y % (8 * gobs) // 8) * 512
    return a + (((x & 63) >> 5) << 8 | ((y & 7) >> 2) << 7 | ((x & 31) >> 4) << 6 | ((y >> 1) & 1) << 5 | (y & 1) << 4 | (x & 15))


def decode(b, t):
    """a texture as a PIL image (RGBA, or L for BC4)"""
    import texture2ddecoder
    from PIL import Image
    _, fmt, w, h, gobs, data = t
    bpe = 16 if fmt >> 8 == 0x2D else 8
    bw, bh = (w + 3) // 4, (h + 3) // 4
    pitch = (bw * bpe + 63) // 64
    lin = b"".join(b[data + o:data + o + bpe] for o in (gob_offset(x * bpe, y, gobs, pitch) for y in range(bh) for x in range(bw)))
    if bpe == 16:
        return Image.frombytes("RGBA", (bw * 4, bh * 4), texture2ddecoder.decode_astc(lin, bw * 4, bh * 4, 4, 4), "raw",
                               "BGRA").crop((0, 0, w, h))
    return Image.frombytes("L", (bw * 4, bh * 4), lin, "bcn", 4).crop((0, 0, w, h))


def encode_bc4(im):
    """BC4 by each block's darkest and lightest, the 6 levels between"""
    px, out = im.load(), bytearray()
    for by in range(0, im.height, 4):
        for bx in range(0, im.width, 4):
            v = [px[bx + i % 4, by + i // 4] for i in range(16)]
            hi, lo = max(v), min(v)
            idx = 0
            for i, c in enumerate(v):
                t = round((c - lo) * 7 / (hi - lo)) if hi > lo else 7
                idx |= (0 if t == 7 else 1 if t == 0 else 8 - t) << (3 * i)   # (codes: 0 the lightest, 1 the darkest, 2..7 between)
            out += bytes([hi, lo]) + idx.to_bytes(6, "little")
    return bytes(out)


def encode_astc(im, srgb):
    import astc_encoder as A
    ctx = A.ASTCContext(A.ASTCConfig(A.ASTCProfile.LDR_SRGB if srgb else A.ASTCProfile.LDR, 4, 4, 1, A.ASTCQualityPreset.THOROUGH))
    return ctx.compress(A.ASTCImage(A.ASTCType.U8, im.width, im.height, 1, im.tobytes()), A.ASTCSwizzle.from_str("RGBA"))


def runs(t, im, box=None):
    """the file's bytes for the blocks of im (the whole texture's size) inside box (pixels; all of it by default), as (offset, bytes)
    runs"""
    from PIL import Image
    _, fmt, w, h, gobs, data = t
    bpe = 16 if fmt >> 8 == 0x2D else 8
    bw, bh = (w + 3) // 4, (h + 3) // 4
    x0, y0, x1, y1 = box or (0, 0, w, h)
    bx0, by0, bx1, by1 = max(0, x0 // 4), max(0, y0 // 4), min(bw, (x1 + 3) // 4), min(bh, (y1 + 3) // 4)
    part = Image.new(im.mode, (bw * 4, bh * 4))   # (only the box's blocks are encoded)
    part.paste(im, (0, 0))
    part = part.crop((bx0 * 4, by0 * 4, bx1 * 4, by1 * 4))
    blocks = encode_astc(part.convert("RGBA"), (fmt & 0xFF) == 6) if bpe == 16 else encode_bc4(part.convert("L"))
    pw = bx1 - bx0
    pitch = (bw * bpe + 63) // 64
    pieces = sorted((data + gob_offset(x * bpe, y, gobs, pitch),
                     blocks[((y - by0) * pw + x - bx0) * bpe:((y - by0) * pw + x - bx0 + 1) * bpe])
                    for y in range(by0, by1) for x in range(bx0, bx1))
    out = []
    for off, piece in pieces:   # (blocks next to each other in the file: one run)
        if out and out[-1][0] + len(out[-1][1]) == off:
            out[-1][1] += piece
        else:
            out.append([off, bytearray(piece)])
    return out


class Icons:
    """a face's icon for each pad input, drawn from a font (the game's own for the Switch face, else Kenney's)"""

    def __init__(self, face, otf):
        from fontTools.ttLib import TTFont
        if face == "switch":
            self.path, self.code = io.BytesIO(otf), {p: SWITCH_ICON[p] for p in PADS}
        else:
            fname, prefix, names = FACES[face]
            self.path = os.path.join(KENNEY, fname)
            rev = {n: c for c, n in TTFont(self.path).getBestCmap().items()}
            self.code = {}
            for p in PADS:
                n = names.get(p)
                n = n and (n[1:] if n.startswith("/") else prefix + n)
                if n in rev:
                    self.code[p] = rev[n]

    def masks(self, pad, size):
        """(the icon, its silhouette with the holes filled, the holes) as L masks of size x size, the icon centred in it"""
        from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont
        if isinstance(self.path, io.BytesIO):
            self.path.seek(0)
        f = ImageFont.truetype(self.path, size * 4)
        g = Image.new("L", (size * 8, size * 8), 0)
        ImageDraw.Draw(g).text((size * 2, size * 2), chr(self.code[pad]), font=f, fill=255)
        g = g.crop(g.getbbox())
        s = size / max(g.size)
        g = g.resize((max(1, round(g.width * s)), max(1, round(g.height * s))), Image.LANCZOS)
        icon = Image.new("L", (size, size), 0)
        icon.paste(g, ((size - g.width) // 2, (size - g.height) // 2))
        outside = Image.new("L", (size + 4, size + 4), 255)   # (a margin: every side's outside joins the corner's)
        outside.paste(ImageChops.invert(icon.point(lambda v: 255 if v > 127 else 0)), (2, 2))
        ImageDraw.floodfill(outside, (0, 0), 128)   # (what the corner reaches: the outside)
        shape = outside.crop((2, 2, size + 2, size + 2)).point(lambda v: 0 if v == 128 else 255)
        inner = shape.filter(ImageFilter.MinFilter(5))   # (the soft rim of the icon is not a hole)
        holes = ImageChops.multiply(inner, ImageChops.invert(icon))
        return icon, shape, holes


def face_textures(b, tex, face, otf):
    """textures.bin's entries for this face: [(Switch button, pad input, runs)], Switch button 0xff for those always written"""
    from PIL import Image, ImageDraw
    icons = Icons(face, otf)
    entries = []

    def keep(sw, pad):   # (the Switch face's default controls: the game's own textures)
        return pad in icons.code and not (face == "switch" and DEFAULT[SW[sw]] == pad)

    for name, sw in NOTES.items():
        t = tex[name]
        w, h = t[2], t[3]
        for pad in PADS:
            if not keep(sw, pad):
                continue
            icon, shape, holes = icons.masks(pad, int(min(w, h) * 0.9))
            im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            o = ((w - icon.width) // 2, (h - icon.height) // 2)
            im.paste(Image.new("RGBA", icon.size, (255, 255, 255, 255)), o, shape)
            im.paste(Image.new("RGBA", icon.size, (0, 0, 0, 255)), o, holes)
            entries.append((SW[sw], PADS.index(pad), runs(t, im)))
    for name, sw in LETTERS.items():
        t = tex[name]
        w, h = t[2], t[3]
        for pad in PADS:
            if not keep(sw, pad):
                continue
            icon, shape, holes = icons.masks(pad, int(min(w, h) * 0.9))
            letter = holes if holes.getbbox() else icon   # (an icon with no letter in it: its shape)
            im = Image.new("L", (w, h), 0)
            im.paste(letter, ((w - letter.width) // 2, (h - letter.height) // 2))
            entries.append((SW[sw], PADS.index(pad), runs(t, im)))
    for name in PICTURES if face != "switch" else ():
        t = tex[name]
        im = decode(b, t)
        px = im.load()
        for pad, (cx, cy, r) in PICTURE_BUTTONS.items():
            fill, ink = px[cx + r - 6, cy], (205, 205, 205, 255)
            ImageDraw.Draw(im).ellipse((cx - r, cy - r, cx + r, cy + r), fill=fill)
            size = int(2 * r * 0.92)
            icon, shape, holes = icons.masks(pad, size)
            disc = sum(i * n for i, n in enumerate(shape.histogram())) / 255 / size / size > 0.7   # (a disc with a letter in it: the letter)
            mark = holes if disc else icons.masks(pad, int(size * 0.7))[0]
            im.paste(Image.new("RGBA", mark.size, ink), (cx - mark.width // 2, cy - mark.height // 2), mark)
        xs, ys = [c[0] for c in PICTURE_BUTTONS.values()], [c[1] for c in PICTURE_BUTTONS.values()]
        r = max(c[2] for c in PICTURE_BUTTONS.values())
        entries.append((0xFF, 0, runs(t, im, (min(xs) - r, min(ys) - r, max(xs) + r + 1, max(ys) + r + 1))))
    return entries


def write_textures(path, entries):
    with open(path, "wb") as f:
        f.write(b"HPTX" + struct.pack("<I", len(entries)))
        for sw, pad, rs in entries:
            f.write(struct.pack("<BBHI", sw, pad, 0, len(rs)))
            for off, data in rs:
                f.write(struct.pack("<II", off, len(data)) + bytes(data))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--faces", default=",".join(["switch"] + list(FACES)))
    ap.add_argument("--no-textures", action="store_true")
    a = ap.parse_args()
    dictionary = zs_dictionary(a.romfs)
    game_font = read_zs(a.romfs, FONT, dictionary)
    key, otf = unwrap(game_font)
    textures = None
    if not a.no_textures:
        try:
            import astc_encoder  # noqa: F401
            import PIL  # noqa: F401
            import texture2ddecoder  # noqa: F401
            textures = read_zs(a.romfs, TEXTURES, dictionary)
        except ImportError:
            print("prompts: the textures keep the Switch's icons (no astc_encoder, Pillow or texture2ddecoder)", file=sys.stderr)
    if textures:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "Common.bntx"), "wb") as f:
            f.write(textures)
        tex = bntx_textures(textures)
    largest = 0
    for face in a.faces.split(","):
        d = os.path.join(a.out, face)
        os.makedirs(d, exist_ok=True)
        font = wrap(key, face_font(otf, face))
        largest = max(largest, len(font))
        with open(os.path.join(d, "External.bfotf"), "wb") as f:
            f.write(font)
        if textures:
            write_textures(os.path.join(d, "textures.bin"), face_textures(textures, tex, face, otf))
        print(f"prompts: {face}", file=sys.stderr)
    table, path = size_table(a.romfs, dictionary, len(game_font), largest), os.path.join(a.out, "rsizetable.zs")
    if table:
        import zstandard
        with open(path, "wb") as f:
            f.write(zstandard.ZstdCompressor(level=3).compress(table))
        print(f"prompts: the size table lets the font have {largest} bytes", file=sys.stderr)
    elif os.path.exists(path):
        os.remove(path)


if __name__ == "__main__":
    main()
