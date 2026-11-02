#!/bin/sh
# Builds the Mac tools. Info.plist is embedded in the Bluetooth binaries so macOS asks for Bluetooth access.
set -e
cd "$(dirname "$0")"
clang -O2 -fobjc-arc -framework Foundation -framework CoreBluetooth main.m -o wheeltop-shifter \
  -Wl,-sectcreate,__TEXT,__info_plist,Info.plist
echo "Built: $(pwd)/wheeltop-shifter"
clang -O2 -fobjc-arc -framework Cocoa board_buttons.m -o board-buttons
echo "Built: $(pwd)/board-buttons"
clang -O2 -fobjc-arc -framework Foundation -framework CoreBluetooth rd_app.m -o rd-app \
  -Wl,-sectcreate,__TEXT,__info_plist,Info.plist
echo "Built: $(pwd)/rd-app"
