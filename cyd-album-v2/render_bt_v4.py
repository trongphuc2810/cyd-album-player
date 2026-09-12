#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Render the proposed Bluetooth screen v4 (name row + ESP/Phone readout removed,
Clock button added) at native 240x320 with the TFT_eSPI GLCD font, 2x for review.

Two variants:
  A = volume/brightness rows stay exactly where they are today (150 / 210)
  B = reflowed: rune + status + SPEED pulled up, rows at 128 / 192, bigger Clock

Output: mockup_bt_v4.png  (4 panels: A-waiting, A-connected, B-waiting, B-connected)
"""
import re, os
from PIL import Image, ImageDraw

SCR_W, SCR_H = 240, 320
FONT = os.path.expanduser('~/Arduino/libraries/TFT_eSPI/Fonts/glcdfont.c')
src = open(FONT).read()
m = re.search(r'font\[\] PROGMEM = \{(.*?)\};', src, re.S)
hexes = re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1))
GLY = [hexes[i * 5:(i + 1) * 5] for i in range(256)]

BG    = (12, 12, 16)
TEXT  = (255, 255, 255)
DIM   = (150, 150, 158)
CYAN  = (88, 190, 245)
GREEN = (60, 200, 90)
BTN   = (38, 38, 44)
TOPBG = (10, 10, 12)
EDGE  = (40, 40, 48)

# control-row constants (copied from cyd-album-v2.ino)
BT_CTRL_L, BT_CTRL_LW, BT_CTRL_BAR, BT_CTRL_BARW, BT_CTRL_PLUS, BT_CTRL_H = 16, 40, 72, 96, 184, 36


def glyph_px(ch):
    b = GLY[ord(ch) & 0xFF]
    return [(x, y) for x in range(5) for y in range(8) if int(b[x], 16) & (1 << y)]


def draw_text(img, s, x, y, fg, size=1):
    cx = x
    for ch in s:
        if ch != ' ':
            for (gx, gy) in glyph_px(ch):
                for sx in range(size):
                    for sy in range(size):
                        px, py = cx + gx * size + sx, y + gy * size + sy
                        if 0 <= px < SCR_W and 0 <= py < SCR_H:
                            img.putpixel((px, py), fg)
        cx += 6 * size
    return cx


def textw(s, size=1):
    return len(s) * 6 * size


def center(img, s, y, fg, size=1):
    draw_text(img, s, (SCR_W - textw(s, size)) // 2, y, fg, size)


def fill_rect(img, x, y, w, h, c):
    ImageDraw.Draw(img).rectangle([x, y, x + w - 1, y + h - 1], fill=c)


def rrect(img, x, y, w, h, r, c, outline=None):
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r, fill=c,
                        outline=outline, width=1 if outline else 0)


def bt_rune(img, cx, cy, col):
    d = ImageDraw.Draw(img)
    d.line([cx, cy - 11, cx, cy + 11], fill=col)
    d.line([cx, cy - 11, cx + 6, cy - 5], fill=col)
    d.line([cx + 6, cy - 5, cx, cy + 1], fill=col)
    d.line([cx, cy + 11, cx - 6, cy + 5], fill=col)
    d.line([cx - 6, cy + 5, cx, cy - 1], fill=col)


def header(img):
    fill_rect(img, 0, 0, SCR_W, 30, TOPBG)
    d = ImageDraw.Draw(img)
    d.line([0, 29, SCR_W - 1, 29], fill=EDGE)
    draw_text(img, "Bluetooth", 10, 12, CYAN, 1)
    draw_text(img, "< SD", 196, 12, CYAN, 1)


def ctrl_row(img, y, value, caption=None):
    rrect(img, BT_CTRL_L, y, BT_CTRL_LW, BT_CTRL_H, 7, BTN)
    draw_text(img, "-", BT_CTRL_L + BT_CTRL_LW // 2 - 6, y + 10, TEXT, 2)
    rrect(img, BT_CTRL_BAR, y, BT_CTRL_BARW, BT_CTRL_H, 7, BTN)
    draw_text(img, value, BT_CTRL_BAR + BT_CTRL_BARW // 2 - textw(value, 2) // 2, y + 10, CYAN, 2)
    rrect(img, BT_CTRL_PLUS, y, BT_CTRL_LW, BT_CTRL_H, 7, BTN)
    draw_text(img, "+", BT_CTRL_PLUS + BT_CTRL_LW // 2 - 6, y + 10, TEXT, 2)
    if caption:
        center(img, caption, y + BT_CTRL_H + 10, DIM, 1)   # +10 to match the sketch


def clock_btn(img, x, y, w, h, label_size=1, filled=False):
    rrect(img, x, y, w, h, 7, CYAN if filled else BTN, outline=CYAN)
    col = BG if filled else CYAN
    draw_text(img, "Clock", x + w // 2 - textw("Clock", label_size) // 2,
              y + (h - 8 * label_size) // 2, col, label_size)


def panel(state, L):
    img = Image.new('RGB', (SCR_W, SCR_H), BG)
    header(img)
    conn = (state == 'connected')

    bt_rune(img, 120, L['rune_cy'], GREEN if conn else CYAN)
    if conn:
        center(img, "connected", L['status_y'], GREEN, 1)
        center(img, "SPEED 1411 kbps", L['speed_y'], CYAN, 1)
    else:
        center(img, "waiting for phone ...", L['status_y'], DIM, 1)

    ctrl_row(img, L['vol_y'], "70%", "Volume")
    ctrl_row(img, L['bri_y'], "70%", "Brightness")
    clock_btn(img, *L['clock'], label_size=L['clock_size'], filled=L['clock_filled'])
    return img


VARIANTS = [
    # A: rows untouched (150 / 210) — smallest possible change
    dict(name="A", rune_cy=52, status_y=112, speed_y=132, vol_y=150, bri_y=210,
         clock=(8, 272, 224, 34), clock_size=1, clock_filled=False),
    # B: reflowed — rune/status/SPEED up, rows 128/192, roomier Clock
    dict(name="B", rune_cy=58, status_y=84, speed_y=102, vol_y=128, bri_y=192,
         clock=(8, 262, 224, 36), clock_size=2, clock_filled=True),
]

SCALE = 2
PAD = 10
LABEL_H = 16
cells = []
for L in VARIANTS:
    for st in ('waiting', 'connected'):
        cells.append((f"{L['name']}  ·  {st}", panel(st, L)))

cols = 2
cw, ch = SCR_W * SCALE, SCR_H * SCALE
rows = (len(cells) + cols - 1) // cols
W = PAD + cols * (cw + PAD)
H = PAD + rows * (ch + LABEL_H + PAD)
sheet = Image.new('RGB', (W, H), (24, 24, 28))
dr = ImageDraw.Draw(sheet)
for i, (label, img) in enumerate(cells):
    r, c = divmod(i, cols)
    x = PAD + c * (cw + PAD)
    y = PAD + r * (ch + LABEL_H + PAD)
    dr.text((x + 2, y + 2), label, fill=(230, 230, 235))
    sheet.paste(img.resize((cw, ch), Image.NEAREST), (x, y + LABEL_H))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mockup_bt_v4.png')
sheet.save(out)
print(out, sheet.size)
