#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BT screen v5 — SAVED phone name (AVRCP peer name, persisted in NVS).

Two placements for the remembered name:
  A = back in its old slot (size 2 under the rune) - prominent
  B = a small size-1 line right under the state word - keeps the approved layout
When no name has ever been saved, both variants look exactly like the approved
v4 screen (nothing is drawn) - no placeholder, per Phúc's rule.
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

BT_CTRL_L, BT_CTRL_LW, BT_CTRL_BAR, BT_CTRL_BARW, BT_CTRL_PLUS, BT_CTRL_H = 16, 40, 72, 96, 184, 36
BT_ROW_VOL_Y, BT_ROW_BRI_Y = 150, 210
BT_CLK_X, BT_CLK_Y, BT_CLK_W, BT_CLK_H = 8, 272, 224, 34


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
    ImageDraw.Draw(img).rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r,
                                           fill=c, outline=outline, width=1 if outline else 0)


def bt_rune(img, cx, cy, col):
    d = ImageDraw.Draw(img)
    d.line([cx, cy - 11, cx, cy + 11], fill=col)
    d.line([cx, cy - 11, cx + 6, cy - 5], fill=col)
    d.line([cx + 6, cy - 5, cx, cy + 1], fill=col)
    d.line([cx, cy + 11, cx - 6, cy + 5], fill=col)
    d.line([cx - 6, cy + 5, cx, cy - 1], fill=col)


def header(img):
    fill_rect(img, 0, 0, SCR_W, 30, TOPBG)
    ImageDraw.Draw(img).line([0, 29, SCR_W - 1, 29], fill=EDGE)
    draw_text(img, "Bluetooth", 10, 12, CYAN, 1)
    draw_text(img, "< SD", 196, 12, CYAN, 1)


def ctrl_row(img, y, value, caption):
    rrect(img, BT_CTRL_L, y, BT_CTRL_LW, BT_CTRL_H, 7, BTN)
    draw_text(img, "-", BT_CTRL_L + BT_CTRL_LW // 2 - 6, y + 10, TEXT, 2)
    rrect(img, BT_CTRL_BAR, y, BT_CTRL_BARW, BT_CTRL_H, 7, BTN)
    draw_text(img, value, BT_CTRL_BAR + BT_CTRL_BARW // 2 - textw(value, 2) // 2, y + 10, CYAN, 2)
    rrect(img, BT_CTRL_PLUS, y, BT_CTRL_LW, BT_CTRL_H, 7, BTN)
    draw_text(img, "+", BT_CTRL_PLUS + BT_CTRL_LW // 2 - 6, y + 10, TEXT, 2)
    center(img, caption, y + BT_CTRL_H + 10, DIM, 1)


def clock_btn(img):
    rrect(img, BT_CLK_X, BT_CLK_Y, BT_CLK_W, BT_CLK_H, 7, BTN, outline=CYAN)
    draw_text(img, "Clock", BT_CLK_X + BT_CLK_W // 2 - textw("Clock", 1) // 2, BT_CLK_Y + 13, CYAN, 1)


def panel(state, variant, name):
    img = Image.new('RGB', (SCR_W, SCR_H), BG)
    header(img)
    conn = (state == 'connected')
    bt_rune(img, 120, 52, GREEN if conn else CYAN)

    if variant == 'A':
        # name back in its old slot; dim while not linked, bright when linked
        center(img, name, 78, TEXT if conn else DIM, 2)
        center(img, "connected" if conn else "waiting for phone ...", 112, GREEN if conn else DIM, 1)
        if conn:
            center(img, "SPEED 1411 kbps", 132, CYAN, 1)
    else:
        center(img, "connected" if conn else "waiting for phone ...", 100, GREEN if conn else DIM, 1)
        center(img, name, 116, CYAN if conn else DIM, 1)
        if conn:
            center(img, "SPEED 1411 kbps", 132, CYAN, 1)

    ctrl_row(img, BT_ROW_VOL_Y, "70%", "Volume")
    ctrl_row(img, BT_ROW_BRI_Y, "70%", "Brightness")
    clock_btn(img)
    return img


NAME = "Phuc's iPhone"
cells = [("A  ·  waiting (ten da luu)", panel('waiting', 'A', NAME)),
         ("A  ·  connected", panel('connected', 'A', NAME)),
         ("B  ·  waiting (ten da luu)", panel('waiting', 'B', NAME)),
         ("B  ·  connected", panel('connected', 'B', NAME))]

SCALE, PAD, LABEL_H = 2, 10, 16
cw, ch = SCR_W * SCALE, SCR_H * SCALE
cols = 2
rows = (len(cells) + cols - 1) // cols
sheet = Image.new('RGB', (PAD + cols * (cw + PAD), PAD + rows * (ch + LABEL_H + PAD)), (24, 24, 28))
dr = ImageDraw.Draw(sheet)
for i, (label, img) in enumerate(cells):
    r, c = divmod(i, cols)
    x = PAD + c * (cw + PAD)
    y = PAD + r * (ch + LABEL_H + PAD)
    dr.text((x + 2, y + 2), label, fill=(230, 230, 235))
    sheet.paste(img.resize((cw, ch), Image.NEAREST), (x, y + LABEL_H))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mockup_bt_v5.png')
sheet.save(out)
print(out, sheet.size)
