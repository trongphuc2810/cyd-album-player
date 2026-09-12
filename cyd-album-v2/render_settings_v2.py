#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Two redesigns of the Settings screen (same 6 items, consistent controls).

Current code: rows at SET_ROW_X=8/W=224/H=38, Y0=40, GAP=5 (y=40,83,126,169,212,255)
  -> ends y=293 (27 px margin).  Controls are NOT on one baseline:
  toggle pills at y+10 (h24), stepper buttons at y+7 (h30), text values at y+16,
  and the two idioms (66x24 pill vs 26x30 square) disagree.
Variants below put every control on the same baseline, one idiom per job, and group
the rows: A = grouped cards, B = flat list with dividers.
"""
import os, re
from PIL import Image, ImageDraw

SCR_W, SCR_H = 240, 320
src = open(os.path.expanduser('~/Arduino/libraries/TFT_eSPI/Fonts/glcdfont.c')).read()
hexes = re.findall(r'0x([0-9A-Fa-f]{2})', re.search(r'font\[\] PROGMEM = \{(.*?)\};', src, re.S).group(1))
GLY = [hexes[i * 5:(i + 1) * 5] for i in range(256)]

BG, TEXT, DIM = (0, 0, 0), (255, 255, 255), (120, 120, 120)
BTN, CYAN, TOPBG, OFF = (30, 30, 30), (88, 190, 245), (10, 10, 12), (70, 70, 70)
CTRL = (45, 45, 52)


def txt(im, s, x, y, fg, size=1):
    cx = x
    for ch in s:
        if ch != ' ':
            b = GLY[ord(ch) & 0xFF]
            for gx in range(5):
                v = int(b[gx], 16)
                for gy in range(8):   # 8th row = descender of g p q y ,
                    if v & (1 << gy):
                        for sx in range(size):
                            for sy in range(size):
                                px, py = cx + gx * size + sx, y + gy * size + sy
                                if 0 <= px < SCR_W and 0 <= py < SCR_H:
                                    im.putpixel((px, py), fg)
        cx += 6 * size


def tw(s, size=1):
    return len(s) * 6 * size


def rr(im, x, y, w, h, r, c, outline=None):
    ImageDraw.Draw(im).rounded_rectangle([x, y, x + w - 1, y + h - 1], r, fill=c, outline=outline)


def rect(im, x, y, w, h, c):
    ImageDraw.Draw(im).rectangle([x, y, x + w - 1, y + h - 1], fill=c)


def header(im):
    rect(im, 0, 0, SCR_W, 30, TOPBG)
    rect(im, 0, 29, SCR_W, 1, (40, 40, 48))
    txt(im, "Settings", 10, 12, CYAN, 1)
    txt(im, "< back", 176, 12, CYAN, 1)


def pill(im, x, y, on):
    """One toggle idiom: 50x24, text centred inside."""
    rr(im, x, y, 50, 24, 12, CYAN if on else OFF)
    s = "ON" if on else "OFF"
    txt(im, s, x + (50 - tw(s)) // 2, y + 9, BG if on else (200, 200, 205), 1)


def stepper(im, x, y, val):
    """One stepper idiom: [- ] value [ +], 26x26 each."""
    rr(im, x, y, 26, 26, 6, CTRL)
    txt(im, "-", x + 11, y + 10, TEXT, 1)
    txt(im, val, x + 32, y + 10, CYAN, 1)
    rr(im, x + 66, y, 26, 26, 6, CTRL)
    txt(im, "+", x + 77, y + 10, TEXT, 1)


def chevron(im, x, y, col=DIM):
    ImageDraw.Draw(im).line([x, y, x + 5, y + 4], fill=col)
    ImageDraw.Draw(im).line([x + 5, y + 4, x, y + 8], fill=col)


# ── Variant A: grouped cards with section headers ────────────────────────────
def variant_a():
    im = Image.new('RGB', (SCR_W, SCR_H), BG)
    header(im)
    groups = [
        ("DISPLAY", [("Display invert", "toggle", True), ("Brightness", "step", "60%")]),
        ("NETWORK", [("Bluetooth mode", "value", "SD mode"), ("WiFi", "toggle", True)]),
        ("SYSTEM", [("Recalibrate touch", "value", "run 4 taps"), ("Clock", "value", "view now")]),
    ]
    y = 37
    for gname, rows in groups:
        txt(im, gname, 14, y, CYAN, 1)
        y += 14
        for label, kind, val in rows:
            rr(im, 8, y, 224, 34, 8, BTN, outline=(38, 38, 44))
            txt(im, label, 18, y + 13, TEXT, 1)
            if kind == "toggle":
                pill(im, 170, y + 5, val)
            elif kind == "step":
                stepper(im, 142, y + 4, val)
            else:
                txt(im, val, 214 - tw(val), y + 13, DIM, 1)
            y += 38
        y += 4
    return im


# ── Variant B: flat list, one row per item, thin dividers ───────────────────
def variant_b():
    im = Image.new('RGB', (SCR_W, SCR_H), BG)
    header(im)
    rows = [("Display invert", "toggle", True), ("Bluetooth mode", "value", "SD mode"),
            ("Brightness", "step", "60%"), ("WiFi", "toggle", True),
            ("Recalibrate touch", "value", "run 4 taps"),
            ("Clock", "value", "view now")]
    y = 38
    for label, kind, val in rows:
        txt(im, label, 16, y + 12, TEXT, 1)
        if kind == "toggle":
            pill(im, 178, y + 6, val)
        elif kind == "step":
            stepper(im, 146, y + 5, val)
        else:
            txt(im, val, 218 - tw(val), y + 12, DIM, 1)
            chevron(im, 224, y + 11)
        rect(im, 8, y + 41, 224, 1, (26, 26, 30))
        y += 46
    return im


out = os.path.expanduser('~/ExportH/Screenshot')
os.makedirs(out, exist_ok=True)
ims = [("SHIPPED: variant A", variant_a()), ("(rejected) variant B", variant_b())]
for n, im in ims:
    im.resize((SCR_W * 3, SCR_H * 3), Image.NEAREST).save(f"{out}/settings-v2-{n.split()[1]}.png")

SC = 2
sheet = Image.new('RGB', (2 * SCR_W * SC + 3 * 12, SCR_H * SC + 34), (22, 22, 26))
d = ImageDraw.Draw(sheet)
for i, (n, im) in enumerate(ims):
    x = 12 + i * (SCR_W * SC + 12)
    sheet.paste(im.resize((SCR_W * SC, SCR_H * SC), Image.NEAREST), (x, 26))
    d.text((x, 8), n, fill=(225, 225, 230))
sheet.save(f"{out}/settings-v2-sheet.png")
print("rendered settings variants A/B")
