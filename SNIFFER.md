# Sniffing the derailleur's BLE connections

The protocol in [wheeltop_eds_protocol_spec.md](wheeltop_eds_protocol_spec.md) was worked out by recording
the derailleur (RD) talking to the shifter and to the phone app. The stock nRF Sniffer couldn't follow these
connections because the RD uses Channel Selection Algorithm #2, so this repository has its own sniffer firmware.
A sniffer only listens, so it can't break pairing.

## Hardware

**Nordic nRF52840 Dongle** (PCA10059), the blue USB dongle, about $10–20.

## Building and flashing the firmware

Needs the Arm GNU toolchain (`arm-none-eabi-gcc` with newlib, e.g. the xPack build), a TinyUSB checkout
and `nrfutil` with `nrf5sdk-tools`. See `dongle/build.sh` for the paths it expects.

```sh
dongle/build.sh                     # builds dongle/sniffer/build/wheeltop_sniffer.zip
dongle/build.sh flash /dev/cu.usbmodemXXXX
```

To flash, put the dongle in bootloader mode: press its side RESET button, the LED pulses red, and a new
`/dev/cu.usbmodem…` port appears.

## Recording

```sh
python3 mac/dongle_sniff.py TARGET capture.pcap
```

`TARGET` is the address of the peripheral whose connections to follow:

- the shifter (pod), to record the RD connecting to it;
- the RD, to record the phone app connecting to it.

The board's `s`/`a` commands or any BLE scanner app show the addresses. The sniffer has to catch the
**start** of a connection (CONNECT_IND), so start recording first, then wake the devices (move the bike or
press a shifter button). The script prints ATT traffic as it goes, writes a `.pcap` for Wireshark
(DLT 256, Bluetooth LE LL with PHDR) and a `.txt` with the raw lines.

Recordings contain the addresses of your devices and of everything else nearby, so they are not in
this repository.
