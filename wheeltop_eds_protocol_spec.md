# WHEELTOP EDS: BLE protocol of the shifter (pod), the rear derailleur and the app

An unofficial specification, written as a research log: later sections correct earlier ones.
Wheeltop publishes no documentation.

**Summary of what works (2026-11-03):**

- The rear derailleur (RD, advertises as `EDS OX2`) is a BLE central to its shifter (pod) and at the same time
  a peripheral to the Wheeltop phone app. The app protocol (section 19) shifts gears while the shifter stays
  paired; `esp32/wheeltop_pod` in app mode uses it.
- A board can also replace the shifter by emulating the pod (sections 10, 15, 17).

Sections 0–8 come from reading the BikeControl source:
`lib/bluetooth/devices/wheeltop/wheeltop_eds.dart` and `wheeltop_probe.dart`
(github.com/OpenBikeControl/bikecontrol, main branch, read 2026-10-05). Sections 9 and later are hardware tests.

Addresses of the author's devices are not published. Where they matter, the text says "the pod's address"
or "the RD's address".

## 0. What this describes and what it doesn't

- Described: **the pod (handlebar shifter) acts as a BLE peripheral and sends button events**.
- Not described (in BikeControl): what the rear derailleur (RD) does and how it replies.
  The code says outright that the RD's reply to the status frame is unknown.
- The pod accepts a central only while the RD hasn't "claimed" it.
  So in the stock system **the RD is the central and the pod is the peripheral**.
- Conclusion for "control the derailleur without the pod": don't send commands to the RD,
  **emulate the pod**. The emulator advertises as a peripheral and sends the RD button frames.
  (Hypothesis at this point; confirmed in section 10.)

## 1. Advertisement

Manufacturer-specific data. The pod has no BLE name. Offsets count from the start of the manufacturer data.
On some platforms the first two bytes (`07 0f`) are reported as a little-endian company id, on others
they stay in the payload (corrected in section 9: there is a company id `ff ff` before them).

| Offset | Length | Value |
|---|---|---|
| 0..6 | 7 | Constant prefix `07 0f 00 14 55 6a 84` |
| 7 | 1 | Device type (below) |
| 8 | 2 | Firmware version: major, minor (optional) |
| 10 | 2 | Battery voltage, big-endian, volts × 100 (e.g. 292 = 2.92 V) (optional) |

Version and battery are read only if the data is longer than 11 bytes.

Device types:

| Byte | Variant |
|---|---|
| `0x37` | OX |
| `0x39` | TX Front |
| `0x36` | TX Left |
| `0x38` | TX Right |

All variants use the same button protocol.

## 2. GATT

Nordic UART Service (NUS):

| Role | UUID |
|---|---|
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| Button events (notify) | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| Probable slot for writes from the RD | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

- In standard NUS 6e400002 is RX (write) and 6e400003 is TX (notify). BikeControl says Wheeltop uses them
  **the other way round**, buttons arriving as notify on `…0002` (wrong, see section 9).
- The BikeControl client subscribes to **all** notify/indicate characteristics of the service, since it doesn't
  know where the pod expects a reply.
- Button events are accepted only from `…0002`; other slots only if subscribed.
- For writes (keepalive probe) all writable characteristics are tried, `…0003` first.

## 3. Frame format (confirmed, from the pod)

The first byte is always `0x04` (PACKET_PREFIX). Two forms:

**OX, 3 bytes:** `04 code xor`, where `xor = 0x04 ^ code`.

**TX, 4 bytes:** `04 type code sum`, where
- `type` is the type byte from the advertisement (`0x36..0x39`; other values make the frame invalid),
- `sum = (0x04 + type + code) & 0xff`.

For codes without bit 2 set both checksums give the same result (a comment in the code).
A frame that fails the check is ignored.

### Example (computed from the formulas, not from logs)

| Event | OX (3 bytes) | TX Right, type=0x38 (4 bytes) |
|---|---|---|
| Top pressed (`0x02`) | `04 02 06` | `04 38 02 3e` |
| Top released (`0x0a`) | `04 0a 0e` | `04 38 0a 46` |
| Bottom pressed (`0x01`) | `04 01 05` | `04 38 01 3d` |

## 4. Codes

| Code | Meaning | BikeControl handling |
|---|---|---|
| `0x01` | Bottom button pressed (shift down) | press |
| `0x02` | Top button pressed (shift up) | press |
| `0x03` | Bottom held | ignored (repeats every ~500 ms) |
| `0x04` | Top held | ignored |
| `0x05` | Bottom pressed, fine-tune mode | press |
| `0x06` | Top pressed, fine-tune mode | press |
| `0x07` | Bottom held, fine-tune | ignored |
| `0x08` | Top held, fine-tune | ignored |
| `0x09` | Bottom released | release (same code for R and T) |
| `0x0a` | Top released | release (same code for R and T) |
| `0x10` | Status/"hello" frame of the TX firmware | see section 5 |

The pod has two physical buttons and a slider. Position "R" gives the normal codes `0x01/0x02`,
position "T" gives the fine-tune codes `0x05/0x06`. The pod **doesn't know** the current gear;
it only reports button states.

A typical press: press (`01`/`02`), then hold repeats (`03`/`04`) every ~500 ms, then release (`09`/`0a`).
Other valid codes aren't described; an unknown code is just logged.

## 5. Status frame and keepalive (TX firmware)

- A TX pod sends `04 type 10 sum` at 1 Hz, starting about 300 ms after connecting.
- Without a reply **the pod drops the connection after three frames**.
- The correct reply is unknown. BikeControl tries candidates, one per connection, and logs whether the
  connection survived 5 s:
  1. `04 type 10 sum` (echo)
  2. `04 type 11 sum` (code+1)
  3. `04 type 0f sum` (opcode `0x0f` "from a field log")
  4. `04 10 14` (3 bytes, XOR, echo)
  5. `04 11 15` (3 bytes, XOR, ack)
  6. `09 01 00 00` (a pod reply seen in a log, verbatim)
  7. `01`
- The comments imply opcode `0x0f` and frame `09 01 00 00` were seen in traffic, but not from which side.
- For the OX firmware no status frame is mentioned.
- The probe results aren't recorded in the source.

(Section 17: the real pod sends no status frames to the RD at all.)

## 6. Connection behaviour

- The pod accepts a central only if the RD hasn't claimed it: while the RD is connected, a phone can't connect
  (UI hint `wheeltopClaimedByDerailleurHint`). BikeControl's auto-connect gives up after 3 attempts.
- If the connection drops during a hold, the release may never arrive, so the "pressed" state should be reset
  on disconnect.

## 7. Open questions at the start

1. Is bonding/pairing needed between the RD and the pod (and keys, if so)?
2. How does the RD find the pod: by the advertisement prefix, by MAC, by a binding made in the Wheeltop app?
3. What does the RD send the pod after connecting, and what does it expect for TX status frames?
4. Does the RD accept frames from an emulator without a binding?
5. How does the Wheeltop app bind a pod to an RD and configure gears (a separate protocol)?
6. ANT+ mode (Garmin) is only described in marketing.

## 8. Plan to test "emulate the pod"

1. Take an ESP32 (NimBLE) or a phone/PC that can be a BLE peripheral.
2. Bring up NUS with the same UUIDs and advertise manufacturer data with the prefix and type `0x37`
   (OX, no status frame).
3. After the RD connects, send notify frames `04 02 06` (up) and `04 0a 0e` (release).
4. Record the real pod+RD traffic to compare advertisements and RD writes.
5. If the RD rejects the emulator, a binding or bonding is probably needed; record pairing.

## 9. Hardware check (2026-10-23)

The Mac connected to a real pod as a central (`mac/wheeltop-shifter --listen`) while the RD was asleep.
This corrects sections above.

**Device:** TX Right (`0x38`), firmware 5.7, battery 3.30 V.

**Advertisement.** Manufacturer data as macOS reports it, with the company id:

```
ff ff 07 0f 00 14 55 6a 84 38 05 07 01 4a 90 6a 00 <RD address, 6 bytes little-endian>
```

- Company id `ff ff`; **the prefix `07 0f 00 14 55 6a 84` follows it**, at offset 2.
  Offsets in section 1 count from the start of the prefix.
- After the battery there are 9 more bytes: `90 6a 00` and 6 bytes that turned out to be the paired RD's
  address (section 11).
- No name or service UUIDs in the advertisement; connectable.
- The pod advertises after a button press and after every disconnect (about 0.4 s later).

**GATT. The characteristic roles are as in standard NUS, not reversed (section 2 was wrong):**

| UUID | Properties | Role |
|---|---|---|
| `6e400002-…` | write, write without response | written by the central (RD) |
| `6e400003-…` | notify | button frames and status frames |

No other services or characteristics.

**Frames.** Match the TX formulas of section 3: `04 38 code sum`. Codes seen: `01`, `02`, `04` (top held),
`09`, `0a`, `10`. Fine-tune codes weren't captured.

**Status frame.** `04 38 10 4c` about every 1.02 s, the first ~0.4–0.9 s after subscribing.
Without a reply the pod drops the connection after ~3.6 s, i.e. after three status frames. Button presses don't
extend it. The correct reply is still unknown (to the Mac; the RD gets no status frames, section 17).

## 10. Pod emulation works (2026-10-23)

Confirmed: the RD shifts on commands from the Mac through an ESP32-C3 (`esp32/wheeltop_pod`, controlled by
`mac/wheeltop-shifter --esp32`). 22 presses, all frames arrived.

What it took:

- **The real pod's address** (random static). The RD connects to exactly that address. No bonding or pairing.
- **The advertisement byte for byte**: only manufacturer data (`18 ff ff ff 07 0f …`), no flags, no scan response.
- **GATT like the pod's**: `…0002` (write, write-nr) first, then `…0003` (notify); buttons go as notify on `…0003`.
- **Advertising right after a disconnect**: the RD connected ~0.5 s after the board started.

Observations:

- The RD wrote nothing to `…0002` during the session, though the board sent status frames every second.
  The board doesn't drop the link itself, so emulation needs no status-frame reply.
- macOS caches GATT by address: connecting to the board after the real pod, the Mac subscribed using the old handle.
  The RD didn't behave like that.
- The real pod must be far away during the test: it has the same address as the board.

## 11. RD advertisement and pod binding (2026-10-23)

The ESP32 saw the RD itself while it was awake and not connected to the board:

```
address <RD address> (random), name "EDS OX2"
mfr: ff ff <RD address, 6 bytes in display order> 52 04 23 50 cc 22 00 00
```

- The RD advertises with the name `EDS OX2`. Its manufacturer data carries its own address after company id `ff ff`.
- **The tail of the pod's advertisement is the RD's address reversed** (little-endian):
  the pod announces which RD it is bound to. `90 6a 00` before it is still unknown.
- The other bytes of the RD advertisement (`52 04 23 50 cc 22 00 00`) are unknown (see also section 18).

## 12. Lost pairing (2026-10-23)

After experiments with a board advertising with the pod's address, the real pod stopped controlling the RD:
the pod's LED was red, and the RD advertised as `EDS OX2` all the time without connecting to the pod or the board.
Re-pairing by Wheeltop's instructions fixed it.

- `EDS OX2` on air is not a sign of pairing mode: after re-pairing, with everything working, the Mac saw both
  the RD's `EDS OX2` advertisement and the pod's.
- Cause unknown at this point. Suspect: the board and the pod on air at once with the same address.
  Keep the pod without its battery while the board runs.
- Re-pairing **didn't change** the pod's advertisement (same 23 bytes, same RD address in the tail),
  so the board needn't copy it again.
- After re-pairing, with the pod's battery out, the RD connected to the board right after power-up without
  moving the bike, and a shift commanded from the Mac worked.

## 13. Pod GATT with handles (read by the board, 2026-10-23)

| Handle | What | Value |
|---|---|---|
| `0x0001` | service 1800 (GAP), up to `0x0009` | |
| `0x0003` | 2A00 name, read/write | `"EDS TX"` |
| `0x0005` | 2A01 appearance, read | `00 00` |
| `0x0007` | 2A04 connection parameters, read | `20 00 40 00 04 00 c8 00` (40–80 ms, latency 4, timeout 2 s) |
| `0x0009` | 2AA6 central address resolution, read | `01` |
| `0x000a` | service 1801 (GATT), **empty** | |
| `0x000b` | NUS service | |
| `0x000d` | `…0002` write, write-nr | |
| `0x000f` | `…0003` notify | |
| `0x0010` | CCCD of `…0003` | |

NimBLEServer lays this out differently (NUS from `0x000e`, the buttons' CCCD at `0x0013`, three characteristics
in 1801). If the RD writes to the pod's remembered handles, on such a board the subscription to `0x0010` lands
in `…0002` and the write to `0x000d` in GATT "client supported features". Suspected cause of lost pairing.
The board was changed: its GATT table is built by hand and matches the pod handle for handle.

## 14. Third lost pairing (2026-10-23)

After re-pairing: the board connected to the **real pod** as a central and read its GATT; then the pod's battery
was removed, the board was tested with the Mac on its factory address, then given the pod's address again.
The RD woke up and stayed on air for over a minute but didn't connect to the board. The pod with its battery
was red and didn't shift, so pairing was lost before the board got the pod's address back.

- The red pod's advertisement was the same 23 bytes. Lost pairing isn't visible in the pod's advertisement.
- The RD's advertisement at different times: `52 04 23 50 bc 22 00 0f`, `… cc 22 00 00`, `… ac 22 00 00` (red pod).
- Main suspect: a foreign connection to the pod (the board reading its GATT, earlier `--listen` from the Mac).

## 15. Why pairing was lost and the first fix (2026-10-23)

The board's event log (`log` command) showed the RD's first connection after the board started:

```
1237 ms: MTU 247
1567 ms: CONNECT from <RD address>, status 26 (BLE_HS_EENCRYPT_KEY_SZ), but the link is up
1667 ms: LINK_ESTAB (38)
2167 ms: subscription to 0x000f, notify=1
2267 ms: WRITE by RD -> 0x000d: 03 01 00 00 01 01 03 22 00 00 00 00 00 00 00 00 00 00 00 00
```

- No BLE encryption or pairing: no ENC_CHANGE events, 0 keys stored.
- CONNECT arrives with a non-zero status although the link is up. The old firmware treated it as a failure:
  it didn't record the connection, didn't send status frames, and restarted advertising with the pod's address
  while connected. Now the connection is taken from CONNECT, MTU or SUBSCRIBE, whichever shows it.
- Right after subscribing the RD writes 20 bytes to `…0002`. Meaning unknown.
- After the fix a shift commanded from the board worked.
- But after switching the board off, the RD didn't come back to it, and the real pod was red.
  So a session with the board broke pairing even when shifting worked.
- Main theory: the 20 bytes from the RD are a request the real pod answers and the board didn't.
  It had to be sniffed in a real RD↔pod session.
- Loss no. 3 happened without the RD connecting to the board, right after the board connected to the pod
  as a central. A foreign connection to the pod seems to break pairing too.

## 16. Sniffer: how the RD connects to the pod (2026-11-02)

Captured with an nRF52840 dongle running nRF Sniffer 5.0.0.

CONNECT_IND from the RD to the pod. In two captures without following it arrived with a good CRC and the same
parameters: TxAdd Random, RxAdd Public, CSA #1, 1M, window 5, offset 0, interval 24 (30 ms), latency 0,
timeout 400 (4 s), channel map `1f ff 00 00 00`, **hop 0**.

In follow mode the same request arrived with a bad CRC and other values (map `ff ff ff ff 1f`, hop 9–14),
and the sniffer never followed the connection. The RD's signal at the dongle was weak, −69 to −80 dBm.

Conclusion (corrected in section 17: hop 0 was corruption, it's actually CSA #2): the stock nRF Sniffer can't
follow this connection, so the dongle needs its own firmware.

## 17. The full RD↔pod conversation (2026-11-02, own dongle firmware)

Captured with the nRF52840 dongle running the firmware in `dongle/` (reader `mac/dongle_sniff.py`).

**Section 16 is wrong.** The Nordic sniffer received CONNECT_IND with corrupted bits. The real parameters (good CRC):

- header `e5`: **ChSel = 1 (CSA #2)**, TxAdd and RxAdd random;
- window 5, offset 0, interval 24 (30 ms), latency 0, timeout 400 (4 s);
- all 37 channels, hop 11–15 (unused with CSA #2), SCA 0.

The session (who sends is clear from the timing within each event):

| Event | Who | What |
|---|---|---|
| 0 | RD → pod | ATT MTU_REQ 247 |
| 0 | pod → RD | ATT MTU_REQ 247 (the pod also starts an MTU exchange) |
| 1–3 | both | LL_LENGTH_REQ/RSP 251/2120, ATT MTU_RSP 247 |
| 5–6 | RD → pod, reply | FIND_BY_TYPE_VALUE (NUS lookup) → service 0x000b–0xffff |
| 7–12 | RD → pod | READ_BY_TYPE 0x2803, FIND_INFO: 0x000d write, 0x000f notify, 0x0010 CCCD |
| 13 | RD → pod | WRITE_REQ 0x0010 = `01 00` (subscribe), then WRITE_CMD 0x000d = `03 01 00 00 01 01 03 20 00…` (20 bytes) |
| 13 | pod → RD | WRITE_RSP |
| 14 | pod → RD | **NOTIFY 0x000f with no data (length 0)**, the reply to the 20 bytes |
| 68 (~1.7 s) | pod → RD | L2CAP CONN_PARAM_UPDATE_REQ: 32–64 (40–80 ms), latency 4, timeout 200 (2 s) |
| 69–70 | RD → pod | accepted, LL_CONNECTION_UPDATE_IND: interval 64 (80 ms), latency 4, timeout 200 |
| later | pod → RD | buttons: `04 38 02 3e` / `04 38 0a 46` (up), `04 38 01 3d` / `04 38 09 45` (down) |

What the real pod **doesn't** send: status frames `04 38 10 4c` once a second. In 10 s it sent only button frames.
The board sent them, and the RD may have taken them as a command. Main suspect for the lost pairing.

Byte 8 of the RD's 20 bytes changes between sessions (`22`, `20`, `1e`). Looks like a counter or a charge level;
it doesn't affect pairing.

Board changes: an empty notify on …0003 after the 20 bytes; a parameter request (40–80 ms, latency 4, timeout 2 s)
1.7 s after connecting; no status frames.

**Verified 2026-11-02.** With the fixed firmware the RD connected, sent the 20 bytes with `20` (as to the real pod),
got the empty notify and shifted on a command from the board. Then the board was switched off and the battery put
back in the shifter: the shifter worked at once, pairing was intact.

## 18. Two shifters at once? (2026-11-02)

The board was given its own random address, with the same advertisement as the shifter (RD address in the tail).

- Shifter and board on air together: the RD connected only to the shifter.
- Shifter's battery out, board alone: the RD **didn't connect** to the board.

So outside pairing the RD connects by the remembered shifter address, not by advertisement content.

Pairing by Wheeltop's instructions: power the derailleur for a second, then press any button on the shifter.
Pairing both (shifter and board) in one go: the RD keeps only the one whose button was pressed first after
pairing started; the other is then ignored. **The RD holds one shifter.**

### How pairing goes (captures and the board's log)

- The shifter's advertisement doesn't change on a button press or during pairing.
- The RD's advertisement (`EDS OX2`, manufacturer data `ff ff <RD address> 52 04 23 4f ac 22 00 YY`):
  `YY` = `0f` while a shifter is connected, `00` without one. A `9c` instead of `ac` was a reception error.
- Powering the RD starts pairing. The RD connected to the **board with its new address** (TX Right),
  although the native shifter was also on air: normal discovery, subscription, 20 bytes `03 01 00 00 01 01 03 1e …`,
  empty notify. No button was pressed on the board; after ~8 s the RD unsubscribed and disconnected (reason 0x208).
- So in pairing mode the RD connects to Wheeltop shifters with any address and waits for a button press.
  A TX Left (`0x36`) board was ignored in pairing mode: this RD apparently doesn't take left shifters.
- In pairing mode the RD may first (before service discovery) write to handle `0x0000`:
  `03 00 00 00 01 01 03 1e …` and `03 01 00 00 01 01 03 1e …`.

Verified: the board with its own address pairs like a shifter (a button press while the RD is connected in
pairing mode). Either the board or the shifter works, whichever was paired last; no batteries need removing.

The remaining path: "board as the phone". The RD advertises as `EDS OX2` even with a shifter connected,
and the Wheeltop app shifts gears directly.

## 19. Wheeltop app ↔ RD protocol (2026-11-03)

The phone (central) connects to the RD (`EDS OX2`, peripheral) **at the same time** as the shifter. No encryption.
The RD has the same NUS: the phone writes to `0x000d` (WRITE_CMD), the RD replies with notify on `0x000f`,
CCCD `0x0010`. The phone switches the link to 2M PHY and updates the channel map; the dongle sniffer copes.

Phone frame: `fe 32 <session> <command> <length> <data> <CRC>`, where CRC is **CRC-16/MODBUS** over the whole
frame (from `fe`), sent high byte first. The session byte is `00` in the hello, then the RD's session (here `41`).

| Frame | What |
|---|---|
| `fe 32 00 11 08 79 4f 54 6d 4b 35 30 7a` | hello, data is ASCII `yOTmK50z`; the RD replied `fe 73 00 50 40 00 df 21` |
| `fe 32 41 fa 01 80` | ? |
| `fe 32 41 fb 03 00 NN 51` | read page NN = 0…3; the RD replies with text (below) |
| `fe 32 41 64 01 38` | ? (`0x38` is the TX Right type) |
| `fe 32 41 97 00` | ? |
| `fe 32 41 99 01 02` | **shift up** |
| `fe 32 41 99 01 01` | **shift down** |

The `fb` pages are text: `L_Ver/L_POWER/R_Ver/R_POWER/Q_Ver/Q_POWER/H_Ver:23.4,3/H_POWER:798/H_NAME:EDS OX2/
TOTAL_CNT:12/NUM:10/…/KeySwitch/RacingMode/HRunCnt/RunTime/ErrList/ERRCODE/H_GEA[1..12]` (gear positions),
`ANT_Ver:6.4`, `SYSFLAG`. `L_`/`R_` are probably the left and right shifters.

**Scrambling and the session.** A frame from either side is `fe K <bytes> <CRC>`. The bytes between K and the CRC
are XORed with the key `(K − 0x32) mod 256`. The phone sends K = `0x32`, so its frames are plain; the RD picks K
at random. Plain form: `<session> <command> <length> <data>`.

- The phone's hello: session `00`, command `11`, data `yOTmK50z`. The RD replies `<S> 11 01 <S>`, S being the
  session number (random each time).
- Every later command uses this S: `fe 32 S 99 01 02` shifts up. **The RD silently ignores commands with another S.**
- Shift reply: `S 99 01 00`.
- Gear report from the RD (after every shift, including shifts from the shifter, and about once a second):
  `S 98 06 4f 00 21 00 GG 00`, GG being the gear number (09, 0a, 0b…).

**Verified:** `mac/rd-app` (the Mac as the app) shifts gears, and the native shifter keeps working.

**On the board (2026-11-03):** `esp32/wheeltop_pod` in app mode does the same: it finds the RD by the name
`EDS OX…` or its own address in the manufacturer data, connects, subscribes to `…0003`, sends the hello, takes the
session from the reply, and the buttons send `99 01 02/01`. Verified with the native shifter paired: both work.
The hello and the `99` commands are enough; the rest of the app's start-up (`fa`, `fb`, `64`, `97`) isn't needed.

## 20. The board's own settings service

The board advertises as `WT Board` (name in the scan response) and has a settings service for the Android app,
placed after the NUS service so the pod's handles don't change. It is described in `android/README.md`.
In pod mode the board advertises the pod's manufacturer data (plus the name in the scan response) until the RD
connects, then the settings service for the phone. It recognises the RD by its stored address or by its
subscription to the button characteristic; any other central is treated as a phone.

The RD accepts the shifter plus **only one** app-style central (found 2026-11-03): with the board in app mode
connected, the phone app gets no hello reply. So the board yields: while a phone is subscribed to the event
characteristic `b5f90004-…` (and the board isn't pairing), the board in app mode disconnects from the RD and stops
looking for it, reports `yield=1`, and sends its button presses to the phone as `u`/`d` notifications; the phone
shifts. When the phone unsubscribes or disconnects, the board connects to the RD again.
