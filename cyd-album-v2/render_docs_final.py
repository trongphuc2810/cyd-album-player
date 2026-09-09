#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Render docs image for the final dual-mode firmware:
SD browser, player, Settings (Bluetooth mode row), BT mode screen."""
import re, os
from PIL import Image, ImageDraw

SCR_W, SCR_H = 240, 320
src = open(os.path.expanduser('~/Arduino/libraries/TFT_eSPI/Fonts/glcdfont.c')).read()
m = re.search(r'font\[\] PROGMEM = \{(.*?)\};', src, re.S)
hexes = re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1))
GLY = [hexes[i*5:(i+1)*5] for i in range(256)]

def glyph_px(ch):
    b = GLY[ord(ch) & 0xFF]
    out = []
    for x in range(5):
        v = int(b[x], 16)
        for y in range(7):
            if v & (1 << y):
                out.append((x, y))
    return out

def textw(s, size=1): return len(s) * 6 * size

def draw_text(img, s, x, y, fg, bg=None, size=1):
    cx = x
    d = ImageDraw.Draw(img)
    if bg is not None:
        d.rectangle([x, y, x + textw(s, size), y + 8*size], fill=bg)
    for ch in s:
        if ch != ' ':
            for (gx, gy) in glyph_px(ch):
                for sx in range(size):
                    for sy in range(size):
                        px, py = cx + gx*size + sx, y + gy*size + sy
                        if 0 <= px < img.width and 0 <= py < img.height:
                            img.putpixel((px, py), fg)
        cx += 6 * size
    return cx

def round_rect(img, x, y, w, h, r, c):
    ImageDraw.Draw(img).rounded_rectangle([x, y, x+w-1, y+h-1], radius=r, fill=c)

def center(img, s, y, size, col):
    draw_text(img, s, (SCR_W - textw(s, size))//2, y, col, size=size)

BG=(0,0,0); WHITE=(255,255,255); DIM=(120,120,120)
BTN=(30,30,30); BAR=(70,70,70); CYAN=(88,190,245)
GREEN=(60,200,90)

def bt_rune(img, cx, cy, col):
    d = ImageDraw.Draw(img)
    d.line([cx, cy-11, cx, cy+11], fill=col)
    d.line([cx, cy-11, cx+6, cy-5], fill=col)
    d.line([cx+6, cy-5, cx, cy+1], fill=col)
    d.line([cx, cy+11, cx-6, cy+5], fill=col)
    d.line([cx-6, cy+5, cx, cy-1], fill=col)

def header(img, title):
    round_rect(img, 0, 0, SCR_W, 30, 0, BAR)
    draw_text(img, title, 10, 6, WHITE, BAR, 2)

def ctrl_row(img, y, val, label, val_col=CYAN):
    round_rect(img, 18, y, 44, 44, 7, BTN)
    draw_text(img, "-", 38, y+14, WHITE, BTN, 2)
    round_rect(img, 70, y, 100, 44, 7, BTN)
    center(img, val, y+14, 2, val_col)
    round_rect(img, 176, y, 44, 44, 7, BTN)
    draw_text(img, "+", 196, y+14, WHITE, BTN, 2)
    center(img, label, y+52, 1, DIM)

def bt_connected():
    img = Image.new('RGB', (SCR_W, SCR_H), BG)
    header(img, "Bluetooth mode")
    draw_text(img, "< SD", 196, 12, CYAN, BAR)
    bt_rune(img, 120, 58, GREEN)
    center(img, "Phuc's iPhone", 84, 2, WHITE)
    center(img, "connected", 118, 1, GREEN)
    center(img, "device: Phuc's iPhone", 132, 1, DIM)
    ctrl_row(img, 150, "100%", "ESP 100%   Phone 70%")
    ctrl_row(img, 210, "60%", "Brightness")
    return img

def settings_bt():
    img = Image.new('RGB', (SCR_W, SCR_H), BG)
    round_rect(img, 0, 0, SCR_W, 30, 0, BTN)
    draw_text(img, "Settings", 10, 6, WHITE, BTN, 2)
    draw_text(img, "< back", 170, 10, CYAN, BTN)
    x, w, h, y0, gap = 8, 224, 38, 40, 5
    rows = [("Display invert", "OFF", False),
            ("Bluetooth mode", None, None),
            ("Brightness", "60%", True),
            ("WiFi", "LAN-phuc", True),
            ("Recalibrate touch", "run 4 taps", False),
            ("Clock", "view now", False)]
    y = y0
    for label, val, toggle in rows:
        round_rect(img, x, y, w, h, 8, BTN)
        draw_text(img, label, 18, y+8, WHITE, BTN)
        if val is not None:
            if toggle and val:
                round_rect(img, 150, y+10, 66, 24, 12, CYAN if label=="Display invert" else BAR)
            if label == "Bluetooth mode":
                draw_text(img, "SD mode", 118, y+8, DIM, BTN)
                draw_text(img, ">", 204, y+11, DIM, BTN)
            elif label == "Brightness":
                draw_text(img, "-", 165, y+13, WHITE, BAR)
                draw_text(img, val, SCR_W//2-9, y+8, WHITE, BTN)
                draw_text(img, "+", 213, y+13, WHITE, BAR)
            elif label in ("Display invert","WiFi"):
                s = val.upper() if label=="Display invert" else val
                c = DIM if label=="WiFi" else (COL_ON := (CYAN if val=="ON" else DIM))
                draw_text(img, s, 166, y+16, c if label=="Display invert" else WHITE, BTN)
            else:
                draw_text(img, val, 150, y+16, DIM, BTN)
        else:
            draw_text(img, "run 4 taps", 150, y+16, DIM, BTN) if label=="Recalibrate touch" else None
        y += h + gap
    return img

def browser():
    img = Image.new('RGB', (SCR_W, SCR_H), BG)
    round_rect(img, 0, 0, SCR_W, 30, 0, BTN)
    draw_text(img, "Albums", 10, 6, WHITE, BTN, 2)
    albums = ["Acoustic", "Anime OST", "Chill Lofi", "J-Pop", "Rock Ballads"]
    y = 40
    for i, a in enumerate(albums):
        sel = (i == 1)
        round_rect(img, 8, y, 224, 48, 8, (50,60,90) if sel else BTN)
        draw_text(img, a, 24, y+16, WHITE if sel else DIM)
        y += 54
    return img

def player():
    img = Image.new('RGB', (SCR_W, SCR_H), BG)
    header(img, "Now Playing")
    center(img, "Chill Lofi", 40, 1, DIM)
    center(img, "Midnight Rain", 60, 2, WHITE)
    center(img, "02:14 / 03:47", 92, 1, CYAN)
    img2 = ImageDraw.Draw(img)
    img2.rectangle([20, 108, 220, 112], fill=BAR)
    img2.rectangle([20, 108, 140, 112], fill=CYAN)
    # spectrum bars
    for i, h in enumerate([20, 45, 30, 60, 25, 50, 35, 55, 28, 40]):
        img2.rectangle([24+i*20, 200-h, 38+i*20, 200], fill=CYAN)
    round_rect(img, 10, 238, 56, 44, 7, BTN); draw_text(img, "-", 28, 255, WHITE, BTN, 2)
    round_rect(img, 74, 238, 92, 44, 7, BTN); center(img, "10%", 255, 2, CYAN)
    round_rect(img, 174, 238, 56, 44, 7, BTN); draw_text(img, "+", 193, 255, WHITE, BTN, 2)
    return img

ims = [browser(), player(), settings_bt(), bt_connected()]
W = SCR_W*4 + 30
combo = Image.new('RGB', (W, SCR_H+4), (18,18,22))
for i, im in enumerate(ims):
    combo.paste(im, (i*(SCR_W+10), 2))
combo = combo.resize((W*2, (SCR_H+4)*2), Image.NEAREST)
combo.save('docs-screens.png')
print('saved docs-screens.png', combo.size)
