#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Two redesign variants for the WiFi password (keyboard) screen.

Current code (cyd-album-v2.ino drawKeyboard()):
  SSID (10,4) · "Password:" (10,22) · dots (10,40) · kbPass plaintext (10,58)
  keys KB_Y0=86, KB_KEY_H=38, colW=24, 5 rows -> last key row ends y=276
  action row by=278, h=30 -> ends y=308  (12 px from the screen edge)
Problems: the masked value AND the plaintext are both on screen (duplicate line),
no frames/labels, no back button, and the block is packed edge-to-edge.
"""
import os, re
from PIL import Image, ImageDraw

SCR_W, SCR_H = 240, 320
src = open(os.path.expanduser('~/Arduino/libraries/TFT_eSPI/Fonts/glcdfont.c')).read()
hexes = re.findall(r'0x([0-9A-Fa-f]{2})', re.search(r'font\[\] PROGMEM = \{(.*?)\};', src, re.S).group(1))
GLY = [hexes[i * 5:(i + 1) * 5] for i in range(256)]

BG, TEXT, DIM = (0, 0, 0), (255, 255, 255), (120, 120, 120)
BTN, CYAN, TOPBG, FIELD = (30, 30, 30), (88, 190, 245), (10, 10, 12), (22, 22, 26)
SSID = "TP-Link_5G"
ROWS = ["qwertyuiop", "asdfghjkl", "zxcvbnm,.", "1234567890", "-_@#!.$"]


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


def rrect(im, x, y, w, h, r, c, outline=None):
    ImageDraw.Draw(im).rounded_rectangle([x, y, x + w - 1, y + h - 1], r, fill=c,
                                         outline=outline)


def rect(im, x, y, w, h, c):
    ImageDraw.Draw(im).rectangle([x, y, x + w - 1, y + h - 1], fill=c)


def keys(im, y0, kh, colw):
    for r, row in enumerate(ROWS):
        y = y0 + r * kh
        x0 = (SCR_W - len(row) * colw) // 2
        for c, k in enumerate(row):
            x = x0 + c * colw
            rrect(im, x + 1, y + 1, colw - 2, kh - 6, 4, BTN)
            txt(im, k, x + (colw - 6) // 2, y + kh // 2 - 9, TEXT, 1)


# ── Variant A: framed password box + back button + airier keys ────────────────
def variant_a(visible=False):
    im = Image.new('RGB', (SCR_W, SCR_H), BG)
    rect(im, 0, 0, SCR_W, 30, TOPBG)
    rect(im, 0, 29, SCR_W, 1, (40, 40, 48))
    txt(im, "WiFi password", 10, 12, CYAN, 1)
    txt(im, "< back", 186, 12, CYAN, 1)

    txt(im, "Network:", 10, 38, DIM, 1)
    txt(im, SSID, 76, 38, CYAN, 1)
    # one password field: masked by default, tap [show] to reveal (never both)
    rrect(im, 8, 56, 224, 30, 5, FIELD, outline=(70, 70, 78))
    shown = "phuc2026" if visible else "*" * 8
    txt(im, shown, 16, 66, TEXT, 1)
    rrect(im, 186, 60, 40, 22, 4, BTN)
    txt(im, "Hide" if visible else "Show", 192, 67, CYAN, 1)

    keys(im, 100, 32, 24)
    by = 264
    rrect(im, 8, by, 52, 30, 6, BTN); txt(im, "abc", 18, by + 12, TEXT, 1)
    rrect(im, 64, by, 40, 30, 6, BTN); txt(im, "del", 74, by + 12, TEXT, 1)
    rrect(im, 108, by, 40, 30, 6, BTN); txt(im, "cls", 118, by + 12, TEXT, 1)
    rrect(im, 152, by, 80, 30, 6, CYAN); txt(im, "Connect", 166, by + 12, BG, 1)
    txt(im, "tap Show to read the password", 12, 300, DIM, 1)
    return im


# ── Variant B (SHIPPED): exact geometry now in drawKeyboard() ────────────────
def variant_b(visible=False):
    im = Image.new('RGB', (SCR_W, SCR_H), BG)
    rect(im, 0, 0, SCR_W, 30, TOPBG)
    rect(im, 0, 29, SCR_W, 1, (40, 40, 48))
    txt(im, "WiFi password", 10, 12, CYAN, 1)
    txt(im, "< back", 186, 12, CYAN, 1)
    txt(im, SSID, 10, 34, CYAN, 1)
    txt(im, "Password:", 146, 34, DIM, 1)
    rrect(im, 8, 48, 224, 26, 5, FIELD, outline=(70, 70, 78))
    shown = "phuc2026" if visible else "*" * 8
    txt(im, shown, 14, 59, TEXT, 1)
    rrect(im, 184, 48, 46, 26, 5, BTN)
    txt(im, "Hide" if visible else "Show", 190, 59, CYAN, 1)
    keys(im, 86, 38, 24)
    by = 278
    rrect(im, 8, by, 58, 30, 6, BTN); txt(im, "abc", 16, by + 10, TEXT, 1)
    rrect(im, 70, by, 34, 30, 6, BTN); txt(im, "del", 76, by + 10, TEXT, 1)
    rrect(im, 108, by, 34, 30, 6, BTN); txt(im, "clr", 112, by + 10, TEXT, 1)
    rrect(im, 146, by, 86, 30, 6, CYAN); txt(im, "Connect", 158, by + 10, BG, 1)
    return im


out = os.path.expanduser('~/ExportH/Screenshot')
os.makedirs(out, exist_ok=True)
names = [("B-1-mask", variant_b(False)), ("B-2-show", variant_b(True))]
for n, im in names:
    im.resize((SCR_W * 3, SCR_H * 3), Image.NEAREST).save(f"{out}/wifi-kb-{n}.png")

SC = 2
sheet = Image.new('RGB', (2 * SCR_W * SC + 3 * 10, SCR_H * SC + 34), (22, 22, 26))
d = ImageDraw.Draw(sheet)
for i, (n, im) in enumerate(names):
    x = 10 + i * (SCR_W * SC + 10)
    sheet.paste(im.resize((SCR_W * SC, SCR_H * SC), Image.NEAREST), (x, 26))
    d.text((x, 8), {"B-1-mask": "shipped: masked", "B-2-show": "shipped: Show tapped"}[n], fill=(225, 225, 230))
sheet.save(f"{out}/wifi-kb-sheet.png")
print("rendered variants A/B")
