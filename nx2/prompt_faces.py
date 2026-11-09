"""(hookshot) The button-prompt faces: everything that differs between controllers, in one place (nx2/prompt_assets.py builds them).

To add a face: an entry in FACES below and its name in host/prompts.c's faces[] and the Insert menu's Button prompts row
(gpu/vk/vk_menu.cpp tx_prompts). To change one: its entry. To rework a texture by hand: nx2/prompt_assets.py --dump <dir> writes
every image the build makes, as PNG, under the names it looks for in nx2/prompt_faces/<face>/: a PNG put there takes that image's
place (nx2/prompt_faces/README.md).

Each face:
  font     Kenney's Input Prompts font its icons come from (third_party/kenney_input)
  prefix   the glyph names' prefix in that font
  icons    each pad input's glyph name after the prefix ("/name": the whole name); an input left out keeps the game's icon. No
           Guide: the makers' logos are their trademarks, every face keeps the game's Home icon.
  drawing  the controller in the pause menu's controls page (System), drawn at the build like the game's Pro Controller
           (nx2/prompt_draw.py): (Kenney font, its controller glyph: the body; None, None: the Steam Deck's own shape) and each
           control (kind, centre x, y in the body's box 0..1, size: a fraction of the body's width, or (w, h) for a "pad" or
           "screen", the pad input whose label it shows): "stick", "dpad" (the Pro's own, scaled), "face", "small" (a disc
           with the label), "pad" (a touchpad), "screen". The build makes the page's tinted copy and the shadow from it. None:
           the game's Pro Controller with this face's labels on its face buttons.
  parts    where each pad input is in the picture, for the page's leader lines (picture pixels): (x, y, r), a stick's centre (its
           line ends on it), a button's centre and radius (its line ends just off the edge it comes from), a shoulder's top
           edge (r 0). The four face buttons are framed. Not a pad input's: boxes the lines keep off (x0, y0, x1, y1: a
           screen, a touchpad). SWITCH_PARTS: the game's Pro Controller's.
"""
import os

OVERRIDES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompt_faces")

# the d-pad's arms, the same names in each of Kenney's fonts (the outlined pad with the arm meant filled in)
_DPAD = {"up": "dpad_up_outline", "down": "dpad_down_outline", "left": "dpad_left_outline", "right": "dpad_right_outline"}


def _parts(axis, sticks, dpad, face, small, home, shoulders, **boxes):
    """a picture's parts: axis (x + its mirror's x), sticks ((x, y, r) left, right), dpad (x, y, an arm's distance, its r), face
    ((x, y) north, west, east, south, r), small ((x, y, r) back; start mirrored), home (x, y, r), shoulders ((x, y) trigger,
    bumper; the right ones mirrored), boxes (the lines keep off)"""
    (ls, rs), (dx, dy, arm, ar) = sticks, dpad
    (n, w, e, s, fr), (bx, by, br) = face, small
    (tx, ty), (sx, sy) = shoulders
    return dict(lstick=ls, rstick=rs, up=(dx, dy - arm, ar), down=(dx, dy + arm, ar), left=(dx - arm, dy, ar),
                right=(dx + arm, dy, ar), north=n + (fr,), west=w + (fr,), east=e + (fr,), south=s + (fr,), back=(bx, by, br),
                start=(axis - bx, by, br), guide=home, ltrigger=(tx, ty, 0), lshoulder=(sx, sy, 0), rtrigger=(axis - tx, ty, 0),
                rshoulder=(axis - sx, sy, 0), **boxes)


SWITCH_PARTS = _parts(1159, ((348, 227, 48), (683, 348, 48)), (455, 348, 45, 17),
                      ((800, 168), (731, 227), (869, 227), (800, 286), 32), (477, 161, 19), (631, 227, 21), ((375, 39), (290, 60)))

FACES = {
    "xbox": dict(
        font="kenney_input_xbox_series.otf", prefix="xbox_",
        drawing=("kenney_input_xbox_series.otf", "controller_xboxseries", [
            ("stick", .259, .258, .150, "lstick"), ("dpad", .363, .530, .140, None), ("stick", .600, .515, .150, "rstick"),
            ("face", .644, .303, .076, "west"), ("face", .727, .424, .076, "south"), ("face", .727, .182, .076, "north"),
            ("face", .810, .303, .076, "east"), ("small", .498, .152, .070, "guide"), ("small", .420, .300, .046, "back"),
            ("small", .576, .300, .046, "start")]),
        parts=_parts(1158, ((380, 207, 40), (664, 354, 40)), (467, 364, 40, 15), ((769, 164), (700, 234), (838, 234), (769, 303), 30),
                     (513, 232, 16), (578, 147, 26), ((330, 48), (470, 57))),
        icons=dict(_DPAD, south="button_a", east="button_b", west="button_x", north="button_y", back="button_view",
                   start="button_menu", lshoulder="lb", rshoulder="rb", ltrigger="lt", rtrigger="rt", lstick="ls", rstick="rs")),
    "ps4": dict(
        font="kenney_input_playstation_series.otf", prefix="playstation_",
        drawing=("kenney_input_playstation_series.otf", "controller_playstation4", [
            ("dpad", .207, .333, .175, None), ("stick", .333, .532, .135, "lstick"), ("stick", .666, .531, .135, "rstick"),
            ("pad", .499, .233, (.248, .262), None), ("face", .738, .348, .064, "west"), ("face", .800, .248, .064, "north"),
            ("face", .800, .447, .064, "south"), ("face", .863, .348, .064, "east"), ("small", .318, .150, .040, "back"),
            ("small", .682, .150, .040, "start"), ("small", .500, .530, .046, "guide")]),
        parts=_parts(1159, ((442, 362, 36), (719, 362, 36)), (338, 259, 47, 17), ((830, 215), (778, 267), (882, 267), (830, 319), 25),
                     (429, 163, 14), (580, 362, 16), ((320, 67), (455, 92)), touchpad=(479, 140, 680, 272)),
        icons=dict(_DPAD, south="button_cross", east="button_circle", west="button_square", north="button_triangle",
                   back="/playstation4_button_share", start="/playstation4_button_options", lshoulder="trigger_l1",
                   rshoulder="trigger_r1", ltrigger="trigger_l2", rtrigger="trigger_r2", lstick="button_l3", rstick="button_r3")),
    "ps5": dict(
        font="kenney_input_playstation_series.otf", prefix="playstation_",
        drawing=("kenney_input_playstation_series.otf", "controller_playstation5", [
            ("dpad", .207, .310, .175, None), ("stick", .353, .498, .135, "lstick"), ("stick", .644, .498, .135, "rstick"),
            ("pad", .498, .196, (.312, .278), None), ("face", .737, .325, .064, "west"), ("face", .799, .232, .064, "north"),
            ("face", .799, .419, .064, "south"), ("face", .862, .326, .064, "east"), ("small", .300, .130, .040, "back"),
            ("small", .698, .130, .040, "start"), ("small", .500, .500, .046, "guide")]),
        parts=_parts(1159, ((459, 345, 36), (701, 345, 36)), (338, 242, 47, 17), ((829, 199), (777, 250), (881, 251), (829, 302), 25),
                     (414, 142, 14), (580, 347, 16), ((321, 52), (445, 64)), touchpad=(451, 105, 705, 254)),
        icons=dict(_DPAD, south="button_cross", east="button_circle", west="button_square", north="button_triangle",
                   back="/playstation5_button_create", start="/playstation5_button_options", lshoulder="trigger_l1",
                   rshoulder="trigger_r1", ltrigger="trigger_l2", rtrigger="trigger_r2", lstick="button_l3", rstick="button_r3")),
    "steamdeck": dict(
        font="kenney_input_steam_deck.otf", prefix="steamdeck_",
        drawing=(None, None, [   # (the real Deck's proportions: Kenney's icon is a shorter one)
            ("screen", .500, .440, (.500, .800), None), ("dpad", .072, .300, .078, None), ("stick", .182, .300, .080, "lstick"),
            ("stick", .818, .300, .080, "rstick"), ("pad", .150, .700, (.100, .255), None), ("pad", .850, .700, (.100, .255), None),
            ("face", .925, .200, .034, "north"), ("face", .925, .400, .034, "south"), ("face", .890, .300, .034, "west"),
            ("face", .960, .300, .034, "east"), ("small", .215, .090, .026, "back"), ("small", .785, .090, .026, "start"),
            ("small", .235, .880, .030, "guide")]),
        parts=_parts(1159, ((244, 255, 27), (915, 255, 27)), (129, 256, 26, 10), ((1028, 215), (991, 256), (1065, 256), (1028, 298), 16),
                     (279, 169, 11), (300, 496, 13), ((125, 131), (205, 129)), screen=(318, 150, 842, 480),
                     ltrackpad=(160, 371, 262, 473), rtrackpad=(898, 371, 1000, 473)),
        icons=dict(_DPAD, south="button_a", east="button_b", west="button_x", north="button_y", back="button_view",
                   start="button_options", lshoulder="button_l1", rshoulder="button_r1", ltrigger="button_l2",
                   rtrigger="button_r2")),
}


def override(face, name):
    """the path of a hand-made image taking the place of the build's (name: as prompt_assets.py --dump writes it), or None"""
    path = os.path.join(OVERRIDES, face, name)
    return path if os.path.exists(path) else None


def parts(face):
    """the face's picture's parts (the Switch face: the game's Pro Controller), or None"""
    return SWITCH_PARTS if face == "switch" else FACES[face].get("parts")
