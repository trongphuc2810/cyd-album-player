#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Render EVERY screen of cyd-album-v2 at native 240x320, using the real TFT_eSPI
GLCD 5x7 font and the coordinates/colours ported from the sketch's draw*() functions.

This is a simulator render (no board on USB) — it is what the panel draws, except for
the panel's own invert/BGR setting.
"""
import os
from PIL import Image, ImageDraw

SCR_W, SCR_H = 240, 320
src = open(os.path.expanduser('~/Arduino/libraries/TFT_eSPI/Fonts/glcdfont.c')).read()
import re
hexes = re.findall(r'0x([0-9A-Fa-f]{2})', re.search(r'font\[\] PROGMEM = \{(.*?)\};', src, re.S).group(1))
GLY = [hexes[i * 5:(i + 1) * 5] for i in range(256)]

BG, TEXT, DIM = (0, 0, 0), (255, 255, 255), (120, 120, 120)
BTN, BTN_ACT, DIR, YEL = (30, 30, 30), (70, 70, 70), (35, 45, 70), (255, 200, 0)
CYAN, GREEN, TOPBG = (88, 190, 245), (60, 200, 90), (10, 10, 12)
TAPE_E, TAPE_B, REEL = (75, 75, 80), (42, 42, 46), (215, 45, 50)


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


def ctr(im, s, y, fg, size=1):
    txt(im, s, (SCR_W - tw(s, size)) // 2, y, fg, size)


def rect(im, x, y, w, h, c):
    ImageDraw.Draw(im).rectangle([x, y, x + w - 1, y + h - 1], fill=c)


def rrect(im, x, y, w, h, r, c, outline=None):
    ImageDraw.Draw(im).rounded_rectangle([x, y, x + w - 1, y + h - 1], r, fill=c,
                                         outline=outline)


def tri(im, a, b, c, col):
    ImageDraw.Draw(im).polygon([a, b, c], fill=col)


def circ(im, x, y, r, c):
    ImageDraw.Draw(im).ellipse([x - r, y - r, x + r, y + r], fill=c)


def bt_rune(im, cx, cy, col):
    d = ImageDraw.Draw(im)
    d.line([cx, cy - 11, cx, cy + 11], fill=col)
    d.line([cx, cy - 11, cx + 6, cy - 5], fill=col)
    d.line([cx + 6, cy - 5, cx, cy + 1], fill=col)
    d.line([cx, cy + 11, cx - 6, cy + 5], fill=col)
    d.line([cx - 6, cy + 5, cx, cy - 1], fill=col)


def new():
    return Image.new('RGB', (SCR_W, SCR_H), BG)


def pill(im, x, y, on):
    rrect(im, x, y, 50, 24, 12, CYAN if on else (70, 70, 70))
    t = "ON" if on else "OFF"
    txt(im, t, x + (50 - tw(t)) // 2, y + 9, BG if on else (200, 200, 205), 1)


def stepper(im, card_y, val):
    """No extra frame: the two 22x22 chips + value live inside the row's card."""
    for cx, ch in ((145, "-"), (201, "+")):
        rrect(im, cx, card_y + 6, 22, 22, 5, (45, 45, 52))
        txt(im, ch, cx + 8, card_y + 13, TEXT, 1)
    txt(im, val, 184 - tw(val) // 2, card_y + 14, CYAN, 1)


def set_row(im, y, label):
    """Card + label, shared by every settings row (single source of truth)."""
    rrect(im, 8, y, 224, 34, 8, BTN, outline=(38, 38, 44))
    txt(im, label, 18, y + 13, TEXT, 1)


def note_glyph(im, col):
    """Startup music note — exact coordinates from drawStartupScreen()."""
    circ(im, 96, 120, 5, col)
    circ(im, 114, 116, 5, col)
    rect(im, 102, 96, 6, 30, col)
    rect(im, 120, 92, 6, 30, col)
    rect(im, 104, 92, 20, 1, col)


def note_small(im, col):
    """Player-header note — exact coordinates from drawPlayer()."""
    circ(im, 9, 17, 3, col)
    circ(im, 17, 15, 3, col)
    rect(im, 11, 8, 3, 11, col)
    rect(im, 19, 6, 3, 11, col)
    rect(im, 12, 6, 8, 1, col)


# ── 1. startup ───────────────────────────────────────────────────────────────
def startup():
    im = new()
    ctr(im, "ALBUM PLAYER", 60, TEXT, 2)
    note_glyph(im, CYAN)
    ctr(im, "cyd-album-v2", 165, DIM, 1)
    ctr(im, "Loading /music ...", 230, DIM, 1)
    return im


# ── 2. clock ─────────────────────────────────────────────────────────────────
def clock():
    im = new()
    txt(im, "21:47", (SCR_W - 5 * 30) // 2, 96, YEL, 5)
    txt(im, "08", (SCR_W - 2 * 12) // 2, 180, DIM, 2)
    ctr(im, "FRI 11-09-2026", 224, YEL, 1)
    ctr(im, "tap to wake", 300, DIM, 1)
    return im


# ── 3+4. browser (albums / tracks) ───────────────────────────────────────────
def browser(level, items, sel=1, page="1/2"):
    im = new()
    rect(im, 0, 0, SCR_W, 30, BTN)
    txt(im, "Albums" if level == 0 else "< List", 10, 12, TEXT, 1)
    rrect(im, 170, 4, 62, 22, 4, BTN)
    txt(im, "SETT", 178, 11, CYAN, 1)
    if level == 0:
        rrect(im, 72, 4, 72, 22, 4, BTN)
        tri(im, (80, 10), (80, 20), (90, 15), CYAN)
        txt(im, "Player", 94, 12, TEXT, 1)
    rrect(im, 100, 36, 40, 18, 4, BTN)
    tri(im, (115, 39), (115, 51), (128, 45), CYAN)
    for i, it in enumerate(items):
        y = 55 + i * 24
        rrect(im, 6, y + 1, SCR_W - 12, 21, 5, DIR)
        txt(im, it, 12, y + 7, CYAN if (level == 1 and i == 2) else TEXT, 1)
    fY = SCR_H - 36
    rrect(im, 8, fY + 6, 58, 24, 5, BTN); txt(im, "PREV", 20, fY + 15, TEXT, 1)
    rrect(im, 174, fY + 6, 58, 24, 5, BTN); txt(im, "NEXT", 186, fY + 15, TEXT, 1)
    txt(im, page, 104, fY + 15, DIM, 1)
    return im


# ── 5. player ────────────────────────────────────────────────────────────────
VIS_FREQ = list(range(16))


def vis_bar_color(b, hn):
    t = b / 15.0
    r = min(255, int(20 + t * 120 + hn * 80))
    g = min(255, int(200 - t * 140 + hn * 40))
    bl = min(255, int(160 + (1 - t) * 60 + hn * 30))
    return (r, g, bl)


def player():
    im = new()
    # header 38 px
    rect(im, 0, 0, SCR_W, 38, TOPBG)
    rect(im, 0, 37, SCR_W, 1, (40, 40, 48))
    note_small(im, (170, 80, 240))
    txt(im, "3/12", 22, 8, TEXT, 2)
    txt(im, "Chill Lofi", 22, 26, TEXT, 1)
    rrect(im, 126, 5, 36, 20, 3, CYAN)
    txt(im, "INV", 134, 11, BG, 1)
    rrect(im, 166, 4, 70, 30, 4, (36, 36, 42))
    for row in range(3):
        ly = 11 + row * 7
        rrect(im, 180, ly + 1, 4, 4, 1, CYAN)
        rrect(im, 188, ly + 2, 30, 2, 1, CYAN)
    ctr(im, "Midnight Rain", 40, TEXT, 1)
    # cassette / spectrum
    rrect(im, 8, 52, 224, 108, 11, TAPE_E)
    rrect(im, 12, 56, 216, 100, 8, TAPE_B)
    rrect(im, 44, 62, 152, 12, 4, (50, 50, 56))
    rrect(im, 45, 63, 150, 10, 3, (28, 28, 32))
    txt(im, "SPECTRUM", 56, 66, CYAN, 1)
    levels = [.9, .72, .58, .83, .5, .62, .45, .38, .3, .25, .2, .36, .28, .18, .12, .08]
    for b in range(16):
        x = 18 + 2 + b * 12
        hh = int(levels[b] * 66)
        rect(im, x, 144 - 66, 10, 66, (4, 4, 8))
        if hh:
            rect(im, x, 144 - hh, 10, hh, vis_bar_color(b, levels[b]))
    # progress area y=164
    rect(im, 0, 164, SCR_W, 56, BG)
    rrect(im, 10, 166, 220, 8, 2, (22, 22, 26))
    rect(im, 12, 168, 88, 4, REEL)
    tri(im, (10, 182), (10, 188), (16, 185), TEXT)
    txt(im, "0:01:23", 22, 180, TEXT, 1)
    txt(im, "0:03:47", 170, 180, TEXT, 1)
    txt(im, "Chill Lofi", 10, 192, DIM, 1)
    txt(im, "MP3 320 kbps", 10, 204, CYAN, 1)
    # volume y=224
    for vx, vw in ((10, 56), (74, 92), (174, 56)):
        rrect(im, vx, 224, vw, 30, 6, BTN)
    rrect(im, 38 - 10, 236, 20, 6, 2, TEXT)
    s = "70%"
    txt(im, s, 74 + (92 - tw(s, 1)) // 2, 235, TEXT, 1)
    rrect(im, 200, 229, 4, 20, 1, TEXT)
    rrect(im, 192, 237, 20, 4, 2, TEXT)
    # transport y=262
    for vx, vw in ((10, 56), (74, 92), (174, 56)):
        rrect(im, vx, 262, vw, 42, 7, BTN)
    cy = 283
    rect(im, 20, cy - 14, 3, 28, TEXT)
    tri(im, (36, cy), (47, cy - 11), (47, cy + 11), TEXT)
    tri(im, (46, cy), (57, cy - 11), (57, cy + 11), TEXT)
    rect(im, 108, cy - 15, 7, 30, TEXT)
    rect(im, 125, cy - 15, 7, 30, TEXT)
    tri(im, (181, cy - 11), (181, cy + 11), (192, cy), TEXT)
    tri(im, (189, cy - 11), (189, cy + 11), (200, cy), TEXT)
    rect(im, 209, cy - 14, 3, 28, TEXT)
    return im


# ── 6. settings (variant A — grouped cards, shipped) ────────────────────────
SET_ROW_Y = [48, 86, 142, 180, 236, 274]
SET_GRP_Y = [37, 131, 225]


def settings():
    im = new()
    rect(im, 0, 0, SCR_W, 30, TOPBG)
    rect(im, 0, 29, SCR_W, 1, (40, 40, 48))
    txt(im, "Settings", 10, 12, CYAN, 1)
    txt(im, "< back", 176, 12, CYAN, 1)
    for gy, name in zip(SET_GRP_Y, ("DISPLAY", "NETWORK", "SYSTEM")):
        txt(im, name, 14, gy, CYAN, 1)
    set_row(im, SET_ROW_Y[0], "Display invert"); pill(im, 170, SET_ROW_Y[0] + 5, True)
    set_row(im, SET_ROW_Y[1], "Brightness");    stepper(im, SET_ROW_Y[1], "60%")
    set_row(im, SET_ROW_Y[2], "Bluetooth mode")
    txt(im, "SD mode", 214 - 42, SET_ROW_Y[2] + 13, DIM, 1)
    set_row(im, SET_ROW_Y[3], "WiFi")
    txt(im, "LAN-phuc", 60, SET_ROW_Y[3] + 13, GREEN, 1)
    pill(im, 170, SET_ROW_Y[3] + 5, True)
    set_row(im, SET_ROW_Y[4], "Recalibrate touch")
    txt(im, "run 4 taps", 214 - 60, SET_ROW_Y[4] + 13, DIM, 1)
    set_row(im, SET_ROW_Y[5], "Clock")
    txt(im, "view now", 214 - 48, SET_ROW_Y[5] + 13, DIM, 1)
    return im


# ── 7. wifi list (rows 24px, same density as the album list) ───────────────
def wifi():
    im = new()
    rect(im, 0, 0, SCR_W, 30, TOPBG)
    rect(im, 0, 29, SCR_W, 1, (40, 40, 48))
    txt(im, "WiFi", 10, 12, CYAN, 1)
    txt(im, "< back", 176, 12, CYAN, 1)
    nets = [("LAN-phuc", 3, False), ("Nha 5G", 4, True), ("Thuduc-Cafe", 2, True),
            ("iPhone cua Phuc", 3, False), ("TP-Link_5G", 4, True), ("(hidden)", 1, True),
            ("Viettel-2.4G", 2, True), ("Cafe-Saigon", 1, False), ("FPT-AP-7F", 2, True),
            ("Guest-OS", 1, False)]
    for i, (n, sig, lock) in enumerate(nets):           # 10 rows fit at 24 px each
        y = 40 + i * 24
        rrect(im, 6, y + 1, 228, 21, 5, DIR)
        txt(im, n, 12, y + 7, TEXT, 1)
        # fixed-width 4-bar signal meter (16x10 at x=188) so it can never run
        # into the lock at x=212, whatever the strength
        for b in range(4):
            bh = 2 + b * 2
            col = (CYAN if sig >= 3 else DIM) if b < sig else (30, 30, 34)
            rect(im, 188 + b * 4, y + 17 - bh, 3, bh, col)
        if lock:
            txt(im, "*", 212, y + 7, DIM, 1)
    txt(im, "Rescan", 10, 288, CYAN, 1)
    txt(im, "1/2", 104, 288, DIM, 1)
    txt(im, "More", 186, 288, CYAN, 1)
    return im


# ── 8. keyboard (variant B — shipped: header + one framed field) ────────────
def keyboard(visible=False, ssid="TP-Link_5G"):
    im = new()
    rect(im, 0, 0, SCR_W, 30, TOPBG)
    rect(im, 0, 29, SCR_W, 1, (40, 40, 48))
    txt(im, "WiFi password", 10, 12, CYAN, 1)
    txt(im, "< back", 186, 12, CYAN, 1)
    txt(im, ssid, 10, 34, CYAN, 1)
    txt(im, "Password:", 146, 34, DIM, 1)
    # one outer frame; the Show/Hide chip lives INSIDE it with a real margin
    rrect(im, 8, 46, 224, 28, 6, (22, 22, 26), outline=(70, 70, 78))
    txt(im, "phuc2026" if visible else "*" * 8, 14, 56, TEXT, 1)
    rrect(im, 186, 51, 38, 18, 4, (45, 45, 52))
    t = "Hide" if visible else "Show"
    txt(im, t, 186 + (38 - tw(t)) // 2, 56, CYAN, 1)
    rows = ["qwertyuiop", "asdfghjkl", "zxcvbnm,.", "1234567890", "-_@#!.$"]
    kh, colw = 38, 24
    for r, keys in enumerate(rows):
        y = 86 + r * kh
        x0 = (SCR_W - len(keys) * colw) // 2
        for c, k in enumerate(keys):
            x = x0 + c * colw
            rrect(im, x + 1, y + 1, colw - 2, kh - 6, 4, BTN)
            txt(im, k, x + (colw - 6) // 2, y + kh // 2 - 9, TEXT, 1)
    by = 278
    rrect(im, 8, by, 58, 30, 6, BTN);   txt(im, "abc", 16, by + 10, TEXT, 1)
    rrect(im, 70, by, 34, 30, 6, BTN);  txt(im, "del", 76, by + 10, TEXT, 1)
    rrect(im, 108, by, 34, 30, 6, BTN); txt(im, "clr", 112, by + 10, TEXT, 1)
    rrect(im, 146, by, 86, 30, 6, CYAN); txt(im, "Connect", 158, by + 10, BG, 1)
    return im


# ── 9. touch calibration ─────────────────────────────────────────────────────
def calibration():
    im = new()
    txt(im, "Touch calibration", 10, 12, CYAN, 1)
    ctr(im, "tap the crosshair (1/4)", 36, DIM, 1)
    cx, cy = 120, 170
    ImageDraw.Draw(im).line([cx - 12, cy, cx + 12, cy], fill=YEL)
    ImageDraw.Draw(im).line([cx, cy - 12, cx, cy + 12], fill=YEL)
    ImageDraw.Draw(im).ellipse([cx - 5, cy - 5, cx + 5, cy + 5], outline=YEL)
    ctr(im, "Calibration OK", 150, GREEN, 1)
    return im


# ── 10-12. Bluetooth mode ────────────────────────────────────────────────────
def ctrl_rows(im, vol, bright, cap):
    for y, pct, label in ((150, vol, cap), (210, bright, "Brightness")):
        rrect(im, 16, y, 40, 36, 7, BTN); txt(im, "-", 30, y + 10, TEXT, 2)
        rrect(im, 72, y, 96, 36, 7, BTN)
        s = f"{pct}%"
        txt(im, s, 72 + (96 - tw(s, 2)) // 2, y + 10, CYAN, 2)
        rrect(im, 184, y, 40, 36, 7, BTN); txt(im, "+", 198, y + 10, TEXT, 2)
        ctr(im, label, y + 46, DIM, 1)


def bt_screen(state, dev="Phuc's iPhone", speed="1411 kbps"):
    im = new()
    rect(im, 0, 0, SCR_W, 30, TOPBG)
    rect(im, 0, 29, SCR_W, 1, (40, 40, 48))
    txt(im, "Bluetooth", 10, 12, CYAN, 1)
    txt(im, "< SD", 196, 12, CYAN, 1)
    if state == "waiting":
        bt_rune(im, 120, 52, CYAN)
        ctr(im, "CYD-32-BP", 78, TEXT, 2)
        ctr(im, "waiting for phone ...", 112, DIM, 1)
        ctrl_rows(im, 70, 60, "Volume ESP (not connected)")
    elif state == "success":
        ctr(im, "SUCCESS", 76, GREEN, 2)
        ctr(im, dev, 108, TEXT, 1)
        ctr(im, "connected", 126, GREEN, 1)
        ctrl_rows(im, 100, 60, "ESP 100%   Phone 70%")
    else:
        bt_rune(im, 120, 52, GREEN)
        ctr(im, dev, 78, TEXT, 2)
        ctr(im, "connected", 112, GREEN, 1)
        ctr(im, f"SPEED {speed}", 132, CYAN, 1)
        ctrl_rows(im, 100, 60, "ESP 100%   Phone 70%")
    return im


SCREENS = [
    ("startup", startup()),
    ("clock", clock()),
    ("browser albums", browser(0, ["Acoustic", "Anime OST", "Chill Lofi", "J-Pop", "Rock Ballads", "Synthwave", "Vietnam Pop", "Workout"])),
    ("browser tracks", browser(1, ["01 - Intro.mp3", "02 - Rain.mp3", "03 - Midnight Rain.mp3", "04 - Outro.mp3"])),
    ("player", player()),
    ("settings A", settings()),
    ("wifi list", wifi()),
    ("keyboard (masked)", keyboard(False)),
    ("keyboard (Show)", keyboard(True)),
    ("touch test", calibration()),
    ("BT waiting", bt_screen("waiting")),
    ("BT success 5s", bt_screen("success")),
    ("BT connected", bt_screen("connected")),
]

out = os.path.expanduser('~/ExportH/Screenshot')
os.makedirs(out, exist_ok=True)
SC = 3
for i, (name, im) in enumerate(SCREENS, 1):
    slug = name.replace(' ', '-').replace('(', '').replace(')', '').lower()
    im.resize((SCR_W * SC, SCR_H * SC), Image.NEAREST).save(f"{out}/cyd-all-{i:02d}-{slug}.png")

# contact sheet: 4 columns x 3 rows at 2x with labels
COLS, S2, LBL = 4, 2, 20
ROWS = (len(SCREENS) + COLS - 1) // COLS   # never clip a screen off the sheet
sheet = Image.new('RGB', (COLS * SCR_W * S2 + 10 * (COLS + 1), ROWS * (SCR_H * S2 + LBL) + 12), (22, 22, 26))
for i, (name, im) in enumerate(SCREENS):
    r, c = divmod(i, COLS)
    x = 10 + c * (SCR_W * S2 + 10)
    y = 12 + r * (SCR_H * S2 + LBL)
    sheet.paste(im.resize((SCR_W * S2, SCR_H * S2), Image.NEAREST), (x, y))
    ImageDraw.Draw(sheet).text((x, y - 14), f"{i+1}. {name}", fill=(225, 225, 230))
sheet.save(f"{out}/cyd-all-sheet.png")
print(f"{len(SCREENS)} screens -> {out}/cyd-all-*.png (sheet: {sheet.size})")
