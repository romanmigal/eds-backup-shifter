#!/bin/sh
# Собирает wheeltop-shifter. Info.plist вшивается в бинарник, чтобы macOS спросила доступ к Bluetooth.
set -e
cd "$(dirname "$0")"
clang -O2 -fobjc-arc -framework Foundation -framework CoreBluetooth main.m -o wheeltop-shifter \
  -Wl,-sectcreate,__TEXT,__info_plist,Info.plist
echo "Готово: $(pwd)/wheeltop-shifter"
clang -O2 -fobjc-arc -framework Cocoa board_buttons.m -o board-buttons
echo "Готово: $(pwd)/board-buttons"
clang -O2 -fobjc-arc -framework Foundation -framework CoreBluetooth rd_app.m -o rd-app \
  -Wl,-sectcreate,__TEXT,__info_plist,Info.plist
echo "Готово: $(pwd)/rd-app"
