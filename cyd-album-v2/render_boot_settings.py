#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mockups for the new boot WiFi phase + the Settings screen without the WiFi row.

Sheet 1 (mockup_boot_wifi.png): the boot-time network phase
  1. saved network -> startup screen + "WiFi: <ssid>" + "Fetching time (NTP) ..."
  2. no saved network -> the WiFi list (same geometry as SD mode today)
  3. password keyboard (same as SD mode today)
  4. connect failed -> "WiFi unreachable", boot continues into the chosen mode

Sheet 2 (mockup_settings_noWifi.png): Settings after removing the WiFi row
  now = today's 6 rows; A = 5 rows pulled up (roomy bottom); B = 5 rows spread evenly
"""
import os, re, importlib.util
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("v5", os.path.join(HERE, "render_bt_v5.py"))
v5 = importlib.util.module_from_spec(spec); spec.loader.exec_module(v5)

BG, TEXT, DIM, CYAN, GREEN = v5.BG, v5.TEXT, v5.DIM, v5.CYAN, v5.GREEN
BTN = v5.BTN
EDGE = (38, 38, 44)
CTRLBG = (45, 45, 52)
GRAY = (70, 70, 70)
OFFTXT = (200, 200, 205)
TOPBG = v5.TOPBG
SCR_W = 240
t = v5.draw_text
c = v5.center
w = v5.textw
rr = v5.rrect
fr = v5.fill_rect

# ── boot screen (drawStartupScreen) ────────────────────────────────────────
def boot_screen(ssid=None, state="ntp"):
    img = Image.new('RGB', (240, 320), BG)
    t(img, "ALBUM PLAYER", 48, 60, TEXT, 2)
    d = ImageDraw.Draw(img)
    d.ellipse([91, 115, 101, 125], fill=CYAN)
    d.ellipse([109, 111, 119, 121], fill=CYAN)
    d.rectangle([102, 96, 107, 125], fill=CYAN)
    d.rectangle([120, 92, 125, 121], fill=CYAN)
    d.line([104, 92, 124, 92], fill=CYAN)
    c(img, "cyd-album-v2", 165, DIM, 1)
    if ssid:
        c(img, "WiFi: " + ssid, 208, CYAN, 1)
    if state == "ntp":
        c(img, "Fetching time (NTP) ...", 226, CYAN, 1)
    elif state == "fail":
        c(img, "WiFi unreachable", 226, (255, 0, 0), 1)
    return img

# ── WiFi list (drawWifiList) ───────────────────────────────────────────────
WL_Y0, WL_ITEMH, WL_VIS = 42, 24, 10
NETS = [("Phuc-Home", -50, True), ("TP-Link_5G", -62, True), ("Viettel-2G", -70, True),
        ("CongTy_Wifi", -76, True), ("CafeFree", -82, False), ("iPhone cua Phuc", -66, False),
        ("Nha_Hang_01", -85, True), ("TP-Link_2.4", -88, True)]

def wifi_list():
    img = Image.new('RGB', (240, 320), BG)
    fr(img, 0, 0, SCR_W, 30, BTN)
    t(img, "WiFi", 10, 6, TEXT, 2)
    t(img, "< back", 176, 10, CYAN, 1)
    for i, (name, rssi, locked) in enumerate(NETS[:WL_VIS]):
        y = WL_Y0 + i * WL_ITEMH
        rr(img, 6, y + 1, 228, WL_ITEMH - 3, 5, (26, 26, 32))
        t(img, name[:24], 12, y + 7, TEXT, 1)
        lvl = 4 if rssi > -55 else 3 if rssi > -65 else 2 if rssi > -75 else 1
        for b in range(4):
            bh = 2 + b * 2
            col = CYAN if (b < lvl and lvl >= 3) else (DIM if b < lvl else (30, 30, 34))
            ImageDraw.Draw(img).rectangle([188 + b * 4, y + 17 - bh, 190 + b * 4, y + 17], fill=col)
        if locked:
            t(img, "*", 212, y + 7, DIM, 1)
    return img

# ── password keyboard (drawKeyboard) ──────────────────────────────────────
KB_ROWS = ["qwertyuiop", "asdfghjkl", "zxcvbnm,.", "1234567890", "-_@#!.$"]
KB_Y0, KB_KEY_H = 86, 38

def keyboard(pass_shown="secret12"):
    img = Image.new('RGB', (240, 320), BG)
    fr(img, 0, 0, SCR_W, 30, TOPBG)
    ImageDraw.Draw(img).line([0, 29, 239, 29], fill=(40, 40, 48))
    t(img, "WiFi password", 10, 12, CYAN, 1)
    t(img, "< back", 186, 12, CYAN, 1)
    t(img, "Phuc-Home", 10, 34, CYAN, 1)
    t(img, "Password:", 146, 34, DIM, 1)
    rr(img, 8, 46, 224, 28, 6, (22, 22, 26), outline=(70, 70, 78))
    shown = pass_shown if pass_shown else ""
    t(img, shown, 14, 56, TEXT, 1)
    rr(img, 186, 51, 38, 18, 4, CTRLBG)
    t(img, "Show", 186 + (38 - w("Show", 1)) // 2, 56, CYAN, 1)
    colW = 24
    for row, r in enumerate(KB_ROWS):
        x0 = (SCR_W - len(r) * colW) // 2
        y = KB_Y0 + row * KB_KEY_H
        for ci, ch in enumerate(r):
            x = x0 + ci * colW
            special = (row == 4)
            rr(img, x + 1, y + 1, colW - 2, KB_KEY_H - 6, 4, (40, 40, 48) if special else CTRLBG)
            t(img, ch, x + (colW - 6) // 2, y + KB_KEY_H // 2 - 9,
              (180, 180, 180) if special else TEXT, 1)
    by = KB_Y0 + len(KB_ROWS) * KB_KEY_H + 2
    for x, bw, label, bg, fg in ((8, 58, "abc", BTN, TEXT), (70, 34, "del", BTN, TEXT),
                                 (108, 34, "clr", BTN, TEXT), (146, 86, "Connect", CYAN, BG)):
        rr(img, x, by, bw, 30, 6, bg)
        t(img, label, x + (bw - w(label, 1)) // 2, by + 10, fg, 1)
    return img

# ── Settings ──────────────────────────────────────────────────────────────
SET_ROW_X, SET_CARD_W, SET_CARD_H = 8, 224, 34
SET_PILL_X, SET_PILL_W, SET_PILL_H = 170, 50, 24
SET_STEP_L, SET_STEP_R, SET_STEP_BTN = 145, 201, 22

ROWS = [("Display invert", "toggle"), ("Brightness", "stepper"), ("Bluetooth mode", "bt"),
        ("WiFi", "wifi"), ("Recalibrate touch", "action4"), ("Clock", "action5")]

def settings(rows_y, grp_y, skip_wifi=True, title="Settings"):
    """rows_y: {original row index -> card top}. Row 3 = WiFi (omitted when skip_wifi)."""
    img = Image.new('RGB', (240, 320), BG)
    fr(img, 0, 0, SCR_W, 30, TOPBG)
    ImageDraw.Draw(img).line([0, 29, 239, 29], fill=(40, 40, 48))
    t(img, title, 10, 12, CYAN, 1)
    t(img, "< back", 176, 12, CYAN, 1)
    t(img, "DISPLAY", 14, grp_y[0], CYAN, 1)
    t(img, "NETWORK", 14, grp_y[1], CYAN, 1)
    t(img, "SYSTEM", 14, grp_y[2], CYAN, 1)
    for i, y in rows_y.items():
        rr(img, SET_ROW_X, y, SET_CARD_W, SET_CARD_H, 8, BTN, outline=EDGE)
    for name, i in (("Display invert", 0), ("Brightness", 1), ("Bluetooth mode", 2),
                    ("WiFi", 3), ("Recalibrate touch", 4), ("Clock", 5)):
        if i in rows_y:
            t(img, name, 18, rows_y[i] + 13, TEXT, 1)
    y = rows_y[0]                     # invert pill
    rr(img, SET_PILL_X, y + 5, SET_PILL_W, SET_PILL_H, 12, CYAN)
    t(img, "ON", SET_PILL_X + (SET_PILL_W - w("ON", 1)) // 2, y + 14, BG, 1)
    y = rows_y[1]                     # brightness stepper
    for x, s in ((SET_STEP_L, "-"), (SET_STEP_R, "+")):
        rr(img, x, y + 6, SET_STEP_BTN, SET_STEP_BTN, 5, CTRLBG)
        t(img, s, x + 8, y + 13, TEXT, 1)
    t(img, "60%", 184 - w("60%", 1), y + 14, TEXT, 1)
    y = rows_y[2]                     # bluetooth mode value
    t(img, "SD mode", 214 - w("SD mode", 1), y + 13, DIM, 1)
    if 3 in rows_y:                   # wifi row (today's build)
        y = rows_y[3]
        t(img, "Phuc-Home", 60, y + 13, GREEN, 1)
        rr(img, SET_PILL_X, y + 5, SET_PILL_W, SET_PILL_H, 12, CYAN)
        t(img, "ON", SET_PILL_X + (SET_PILL_W - w("ON", 1)) // 2, y + 14, BG, 1)
    t(img, "run 4 taps", 214 - w("run 4 taps", 1), rows_y[4] + 13, CYAN, 1)
    t(img, "view now", 214 - w("view now", 1), rows_y[5] + 13, CYAN, 1)
    return img

NOW6_Y = {0: 48, 1: 86, 2: 142, 3: 180, 4: 236, 5: 274}
NOW6_G = [37, 131, 225]
A_Y = {0: 48, 1: 86, 2: 142, 4: 198, 5: 236}          # pulled up, 50 px bottom margin
A_G = [37, 131, 187]
B_Y = {0: 58, 1: 96, 2: 159, 4: 222, 5: 260}          # spread evenly, 26 px bottom margin
B_G = [47, 148, 211]


def sheet(cells, path, cols=2):
    S, PAD, LH = 2, 10, 16
    cw, ch = 240 * S, 320 * S
    rows = (len(cells) + cols - 1) // cols
    img = Image.new('RGB', (PAD + cols * (cw + PAD), PAD + rows * (ch + LH + PAD)), (24, 24, 28))
    dr = ImageDraw.Draw(img)
    for i, (lab, p) in enumerate(cells):
        r, cc = divmod(i, cols)
        x = PAD + cc * (cw + PAD); y = PAD + r * (ch + LH + PAD)
        dr.text((x + 2, y + 2), lab, fill=(230, 230, 235))
        img.paste(p.resize((cw, ch), Image.NEAREST), (x, y + LH))
    img.save(path); print(path, img.size)


sheet([("1 · Boot: có WiFi đã lưu → kết nối + lấy giờ", boot_screen("Phuc-Home", "ntp")),
       ("2 · Boot: chưa lưu → danh sách WiFi (như SD)", wifi_list()),
       ("3 · Boot: nhập mật khẩu (như SD)", keyboard()),
       ("4 · Boot: không kết nối được → vẫn vào mode", boot_screen("Phuc-Home", "fail"))],
      os.path.join(HERE, "mockup_boot_wifi.png"))

sheet([("HIỆN TẠI · 6 hàng (còn WiFi)", settings(NOW6_Y, NOW6_G, skip_wifi=False, title="Settings")),
       ("A · 5 hàng (bỏ WiFi) dồn lên, chừa đáy 50px", settings(A_Y, A_G)),
       ("B · 5 hàng (bỏ WiFi) trải đều, chừa đáy 26px", settings(B_Y, B_G))],
      os.path.join(HERE, "mockup_settings_noWifi.png"), cols=3)
