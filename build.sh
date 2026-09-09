#!/usr/bin/env bash
# Build firmware CYD + đóng gói 4 vùng flash vào tools/esp-flasher/firmware/<tên>/
# kèm manifest.json — launcher tự nhận qua /api/firmware
# Usage: ./build.sh [tên-sketch]   (mặc định cyd-album-v2)
set -euo pipefail

NAME="${1:-cyd-album-v2}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$ROOT/$NAME"
OUT="$ROOT/tools/esp-flasher/firmware/$NAME"
FQBN="esp32:esp32:esp32:PartitionScheme=huge_app"

[ -d "$SKETCH" ] || { echo "Không thấy sketch: $SKETCH"; exit 1; }

echo "==> Compile $NAME"
arduino-cli compile --fqbn "$FQBN" --output-dir /tmp/cyd-build "$SKETCH" 2>&1 | tail -3

B=/tmp/cyd-build
[ -f "$B/${NAME}.ino.bin" ] || { echo "Build fail: thiếu app bin"; exit 1; }

# boot_app0.bin nằm trong package esp32 của arduino-cli (không hardcode user)
BOOT_APP0="$(find "$HOME/.arduino15/packages/esp32" -name boot_app0.bin -path '*partitions*' 2>/dev/null | head -1)"
[ -n "$BOOT_APP0" ] || { echo "Không tìm thấy boot_app0.bin"; exit 1; }

mkdir -p "$OUT"
cp "$B/${NAME}.ino.bootloader.bin" "$OUT/bootloader.bin"
cp "$B/${NAME}.ino.partitions.bin"  "$OUT/partitions.bin"
cp "$BOOT_APP0"                     "$OUT/boot_app0.bin"
cp "$B/${NAME}.ino.bin"             "$OUT/app.bin"

VER="$(date +%Y%m%d-%H%M%S)"
python3 - "$OUT" "$NAME" "$VER" <<'PY'
import json, os, sys, hashlib
out, name, ver = sys.argv[1], sys.argv[2], sys.argv[3]
files = [("bootloader.bin", 0x1000), ("partitions.bin", 0x8000),
         ("boot_app0.bin", 0xe000), ("app.bin", 0x10000)]
items = []
for f, addr in files:
    p = os.path.join(out, f)
    items.append({"file": f, "address": addr, "size": os.path.getsize(p),
                  "sha256": hashlib.sha256(open(p,'rb').read()).hexdigest()[:16]})
json.dump({"name": name, "version": ver, "chip": "esp32", "files": items},
          open(os.path.join(out, "manifest.json"), "w"), indent=2)
print("manifest:", ver)
PY

rm -rf /tmp/cyd-build
echo "==> Xong: $OUT"
ls -la "$OUT"
