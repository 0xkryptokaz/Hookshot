"""(hookshot) The button-prompt faces: everything that differs between controllers, in one place (nx2/prompt_assets.py builds them).

To add a face: an entry in FACES below, its picture in nx2/prompt_faces/ (optional), and its name in host/prompts.c's faces[] and the
Insert menu's Button prompts row (gpu/vk/vk_menu.cpp tx_prompts). To change one: its entry, or its picture.

Each face:
  font     Kenney's Input Prompts font its icons come from (third_party/kenney_input)
  prefix   the glyph names' prefix in that font
  icons    each pad input's glyph name after the prefix ("/name": the whole name); an input left out keeps the game's icon. No
           Guide: the makers' logos are their trademarks, every face keeps the game's Home icon.
  picture  the controller in the pause menu's controls page (System), a 1160x680 RGBA PNG in nx2/prompt_faces/ drawn like the
           game's Pro Controller (Controller_Pro: flat greys, a dark outline); the build makes the page's tinted copy and the
           shadow from it. None: the game's Pro Controller with this face's labels on its face buttons.
  parts    where each pad input is in the picture, for the page's leader lines (picture pixels): (x, y, r), a stick's centre (its
           line ends on it), a button's centre and radius (its line ends just off the edge it comes from), a shoulder's top
           edge (r 0). The four face buttons are framed. Not a pad input's: boxes the lines keep off (x0, y0, x1, y1: a
           screen, a touchpad). SWITCH_PARTS: the game's Pro Controller's.
"""
import os

PICTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompt_faces")

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
        font="kenney_input_xbox_series.otf", prefix="xbox_", picture="xbox.png",
        parts=_parts(1158, ((380, 207, 40), (664, 354, 40)), (467, 364, 40, 15), ((769, 164), (700, 234), (838, 234), (769, 303), 30),
                     (513, 232, 16), (578, 147, 26), ((330, 48), (470, 57))),
        icons=dict(_DPAD, south="button_a", east="button_b", west="button_x", north="button_y", back="button_view",
                   start="button_menu", lshoulder="lb", rshoulder="rb", ltrigger="lt", rtrigger="rt", lstick="ls", rstick="rs")),
    "ps4": dict(
        font="kenney_input_playstation_series.otf", prefix="playstation_", picture="ps4.png",
        parts=_parts(1159, ((442, 362, 36), (719, 362, 36)), (338, 259, 47, 17), ((830, 215), (778, 267), (882, 267), (830, 319), 25),
                     (429, 163, 14), (580, 362, 16), ((320, 67), (455, 92)), touchpad=(479, 140, 680, 272)),
        icons=dict(_DPAD, south="button_cross", east="button_circle", west="button_square", north="button_triangle",
                   back="/playstation4_button_share", start="/playstation4_button_options", lshoulder="trigger_l1",
                   rshoulder="trigger_r1", ltrigger="trigger_l2", rtrigger="trigger_r2", lstick="button_l3", rstick="button_r3")),
    "ps5": dict(
        font="kenney_input_playstation_series.otf", prefix="playstation_", picture="ps5.png",
        parts=_parts(1159, ((459, 345, 36), (701, 345, 36)), (338, 242, 47, 17), ((829, 199), (777, 250), (881, 251), (829, 302), 25),
                     (414, 142, 14), (580, 347, 16), ((321, 52), (445, 64)), touchpad=(451, 105, 705, 254)),
        icons=dict(_DPAD, south="button_cross", east="button_circle", west="button_square", north="button_triangle",
                   back="/playstation5_button_create", start="/playstation5_button_options", lshoulder="trigger_l1",
                   rshoulder="trigger_r1", ltrigger="trigger_l2", rtrigger="trigger_r2", lstick="button_l3", rstick="button_r3")),
    "steamdeck": dict(
        font="kenney_input_steam_deck.otf", prefix="steamdeck_", picture="steamdeck.png",
        parts=_parts(1159, ((244, 255, 27), (915, 255, 27)), (129, 256, 26, 10), ((1028, 215), (991, 256), (1065, 256), (1028, 298), 16),
                     (279, 169, 11), (300, 496, 13), ((125, 131), (205, 129)), screen=(318, 150, 842, 480),
                     ltrackpad=(160, 371, 262, 473), rtrackpad=(898, 371, 1000, 473)),
        icons=dict(_DPAD, south="button_a", east="button_b", west="button_x", north="button_y", back="button_view",
                   start="button_options", lshoulder="button_l1", rshoulder="button_r1", ltrigger="button_l2",
                   rtrigger="button_r2")),
}


def picture(face):
    """the face's controller picture's path, or None"""
    name = FACES[face].get("picture")
    path = name and os.path.join(PICTURES, name)
    return path if path and os.path.exists(path) else None


def parts(face):
    """the face's picture's parts (the Switch face: the game's Pro Controller), or None"""
    return SWITCH_PARTS if face == "switch" else FACES[face].get("parts")
