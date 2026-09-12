#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Audit mockup screens for real text/box collisions only.

Flags:
  [COVERED]   a text run is overlapped by a box drawn AFTER it  -> gets hidden
  [OVERFLOW]  a text starts inside a box but sticks out of it   -> text spills over the edge
  [TXT-TXT]   two different text runs overlap
Texts sitting on the bare background (captions, clock, group labels) are fine.
"""
import importlib.util

REND = '/home/phuc/cyd-firmware/cyd-album-v2/render_all_screens.py'
spec = importlib.util.spec_from_file_location("rend", REND)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

LOG = []
_txt, _rect, _rrect = m.txt, m.rect, m.rrect


def bb(s):
    x, y, w, h = s
    return (x, y, x + w - 1, y + h - 1)


def ov(a, b):
    return not (a[2] < b[0] or b[2] < a[0] or a[3] < b[1] or b[3] < a[1])


def wrap_txt(im, s, x, y, fg, size=1):
    LOG.append(("txt", (x, y, len(s) * 6 * size, 8 * size), s))
    return _txt(im, s, x, y, fg, size)


def wrap_rect(fn):
    def w(im, x, y, ww, hh, *a, **k):
        LOG.append(("box", (x, y, ww, hh), ""))
        return fn(im, x, y, ww, hh, *a, **k)
    return w


m.txt = wrap_txt          # ctr() (unpatched) calls this -> logs once, correctly centred
m.rect = wrap_rect(_rect)
m.rrect = wrap_rect(_rrect)

SCREENS = [
    ("startup", lambda: m.startup()),
    ("clock", lambda: m.clock()),
    ("browser albums", lambda: m.browser(0, ["Acoustic", "Chill Lofi", "J-Pop"])),
    ("browser tracks", lambda: m.browser(1, ["01 - Intro.mp3", "02 - Rain.mp3", "03 - Midnight Rain.mp3"])),
    ("player", lambda: m.player()),
    ("settings A", lambda: m.settings()),
    ("wifi list", lambda: m.wifi()),
    ("keyboard masked", lambda: m.keyboard(False)),
    ("keyboard show", lambda: m.keyboard(True, ssid="TP-Link_5G")),
    ("touch test", lambda: m.calibration()),
    ("bt waiting", lambda: m.bt_screen("waiting")),
    ("bt success", lambda: m.bt_screen("success")),
    ("bt connected", lambda: m.bt_screen("connected", dev="Phuc's iPhone")),
    ("bt connected long name", lambda: m.bt_screen("connected", dev="Galaxy S23 Ultra 5G")),
]

total = 0
for name, fn in SCREENS:
    LOG.clear()
    fn()
    items = list(LOG)
    hits = []
    for i, (kind, sh, label) in enumerate(items):
        if kind != "txt":
            continue
        t = bb(sh)
        for k, s2, _ in items[i + 1:]:
            if k == "box" and ov(t, bb(s2)):
                hits.append(f"  [COVERED]  '{label[:26]}' {t} bi o {bb(s2)} ve de")
                break
        for k, s2, lb in items[i + 1:]:
            if k == "txt" and lb != label and ov(t, bb(s2)):
                hits.append(f"  [TXT-TXT]  '{label[:22]}' {t} vs '{lb[:22]}' {bb(s2)}")
                break
        start = (sh[0], sh[1])
        for k, s2, _ in items[:i]:
            if k != "box":
                continue
            b = bb(s2)
            if b[0] <= start[0] <= b[2] and b[1] <= start[1] <= b[3]:
                if not (t[0] >= b[0] and t[1] >= b[1] and t[2] <= b[2] and t[3] <= b[3]):
                    hits.append(f"  [OVERFLOW] '{label[:26]}' {t} tran khoi o {b}")
                break
    # tight-margin check: text inside a box with <3 px to an edge, or only
    # <4 px between a text and the next box (looks like the button "eats" the text)
    for i, (kind, sh, label) in enumerate(items):
        if kind != "txt":
            continue
        t = bb(sh)
        for k, s2, _ in items[:i]:
            if k != "box":
                continue
            b = bb(s2)
            if b[0] <= sh[0] <= b[2] and b[1] <= sh[1] <= b[3]:
                m_top, m_bot = sh[1] - b[1], b[3] - t[3]
                m_l, m_r = sh[0] - b[0], b[2] - t[2]
                worst = min(m_top, m_bot, m_l, m_r)
                if worst < 3:
                    hits.append(f"  [TIGHT {worst}px] '{label[:24]}' {t} trong o {b}")
                break
        for k, s2, _ in items[i + 1:]:
            if k != "box":
                continue
            b = bb(s2)
            if t[0] <= b[2] and b[0] <= t[2]:
                gap = b[1] - t[3] if b[1] >= t[3] else (t[1] - b[3] if t[1] >= b[3] else 0)
                if 0 < gap < 4:
                    hits.append(f"  [GAP {gap}px] '{label[:24]}' {t} sat o {b}")
                break
    if hits:
        print(f"\n=== {name} ===")
        print("\n".join(hits))
        total += len(hits)

print(f"\nTONG LOI THAT: {total}")
