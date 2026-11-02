#!/bin/zsh
# Builds the sniffer firmware for the nRF52840 Dongle and packages it for the dongle's bootloader.
#   ./build.sh            build dongle/sniffer/build/wheeltop_sniffer.zip
#   ./build.sh flash PORT also flash it (dongle in bootloader mode: press the side RESET button,
#                         the LED pulses red; PORT is its /dev/cu.usbmodem…)
set -e
HERE=${0:A:h}
TINYUSB=${TINYUSB:-$HOME/src/tinyusb}
ARMGCC=${ARMGCC:-$(ls -d $HOME/src/armgcc/*/bin 2>/dev/null | head -1)}
[[ -n $ARMGCC ]] && PATH=$ARMGCC:$PATH

# The TinyUSB build system wants the example inside its own tree
DST=$TINYUSB/examples/device/wheeltop_sniffer
rm -rf $DST && mkdir -p $DST && cp -R $HERE/sniffer/Makefile $HERE/sniffer/src $DST/
make -C $DST BOARD=nrf52840dongle -j8 all

OUT=$HERE/sniffer/build && mkdir -p $OUT
cp $DST/_build/nrf52840dongle/wheeltop_sniffer.hex $OUT/
nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req 0x00 --application-version 1 \
  --application $OUT/wheeltop_sniffer.hex $OUT/wheeltop_sniffer.zip
echo "Built: $OUT/wheeltop_sniffer.zip"

if [[ $1 == flash ]]; then
  nrfutil nrf5sdk-tools dfu usb-serial -pkg $OUT/wheeltop_sniffer.zip -p ${2:?give the dongle port}
fi
