#!/bin/sh
# Собирает wheeltop-shifter. Info.plist вшивается в бинарник, чтобы macOS спросила доступ к Bluetooth.
set -e
cd "$(dirname "$0")"
clang -O2 -fobjc-arc -framework Foundation -framework CoreBluetooth main.m -o wheeltop-shifter \
  -Wl,-sectcreate,__TEXT,__info_plist,Info.plist
echo "Готово: $(pwd)/wheeltop-shifter"
