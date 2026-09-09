#!/usr/bin/env python3
"""ESP32 Flasher server: static UI + /api/firmware (danh sách .bin mới nhất)."""
import json
import os
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

BASE = os.path.dirname(os.path.abspath(__file__))
FW_DIR = os.path.join(BASE, "firmware")


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=BASE, **kw)

    def do_GET(self):
        if self.path.split("?")[0] == "/api/firmware":
            items = []
            if os.path.isdir(FW_DIR):
                for d in os.listdir(FW_DIR):
                    mp = os.path.join(FW_DIR, d, "manifest.json")
                    man = None
                    if os.path.isfile(mp):
                        try:
                            man = json.load(open(mp))
                        except Exception:
                            pass
                    items.append({
                        "name": d,
                        "manifest": man,
                        "mtime": int(os.stat(os.path.join(FW_DIR, d)).st_mtime),
                    })
            items.sort(key=lambda x: x["mtime"], reverse=True)
            body = json.dumps({"firmwares": items}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    os.makedirs(FW_DIR, exist_ok=True)
    srv = ThreadingHTTPServer(("127.0.0.1", 9121), Handler)
    print("esp-flasher on http://127.0.0.1:9121")
    srv.serve_forever()
