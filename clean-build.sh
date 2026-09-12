#!/usr/bin/env bash
# Clean rebuild of BOTH firmware variants + merged bins for flashing.
#   ./clean-build.sh            (lite first, then normal)
set -uo pipefail
cd /home/phuc/cyd-firmware

ESP=~/.arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool
MERGE=(--chip esp32 merge_bin -o merged.bin --flash_mode dio --flash_freq 40m --flash_size 4MB
       0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 app.bin)

echo "=== wipe build caches ==="
rm -rf /tmp/cyd-build /tmp/cyd-lite
rm -rf ~/.cache/arduino/sketches/*cyd-album* 2>/dev/null || true

echo "=== 1/2 clean build LITE (sounds from /sounds on SD) ==="
./build.sh --lite || { echo "LITE BUILD FAIL"; exit 1; }
cd tools/esp-flasher/firmware/cyd-album-v2-lite && "$ESP" "${MERGE[@]}" | tail -1
cp merged.bin /home/phuc/ExportH/firmware/cyd-album-v2-final-lite.bin
md5sum merged.bin | cut -c1-16

echo "=== 2/2 clean build NORMAL (embedded PCM chimes) ==="
cd /home/phuc/cyd-firmware
./build.sh || { echo "NORMAL BUILD FAIL"; exit 1; }
cd tools/esp-flasher/firmware/cyd-album-v2 && "$ESP" "${MERGE[@]}" | tail -1
cp merged.bin /home/phuc/ExportH/firmware/cyd-album-v2-final-normal.bin
md5sum merged.bin | cut -c1-16

echo "=== DONE ==="
ls -la /home/phuc/ExportH/firmware/
