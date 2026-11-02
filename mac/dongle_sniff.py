#!/usr/bin/env python3
"""Читает свисток с прошивкой dongle/sniffer, печатает ATT и пишет .pcap для Wireshark.

    python3 dongle_sniff.py [ПОРТ] [ФАЙЛ.pcap] [ЦЕЛЬ]

ЦЕЛЬ — адрес устройства, за чьими соединениями следить (по умолчанию шифтер c0:00:00:00:00:01).
Для переключателя: e0:00:00:00:00:03, тогда central — телефон, peripheral — переключатель.

Без порта берёт первый /dev/cu.usbmodem*, который отвечает строкой "# alive".
Ctrl+C — остановить. Рядом с .pcap пишется .txt со всеми строками свистка.
"""
import glob, os, select, struct, sys, termios, time, tty

ADV_AA = 0x8E89BED6
ATT_OPS = {0x01: "ERROR", 0x02: "MTU_REQ", 0x03: "MTU_RSP", 0x08: "READ_TYPE_REQ", 0x09: "READ_TYPE_RSP",
           0x0a: "READ_REQ", 0x0b: "READ_RSP", 0x10: "READ_GROUP_REQ", 0x11: "READ_GROUP_RSP",
           0x04: "FIND_INFO_REQ", 0x05: "FIND_INFO_RSP", 0x12: "WRITE_REQ", 0x13: "WRITE_RSP",
           0x52: "WRITE_CMD", 0x1b: "NOTIFY", 0x1d: "INDICATE", 0x1e: "CONFIRM"}
LL_OPS = {0x00: "CONNECTION_UPDATE", 0x01: "CHANNEL_MAP", 0x02: "TERMINATE", 0x03: "ENC_REQ",
          0x04: "ENC_RSP", 0x05: "START_ENC_REQ", 0x0c: "VERSION_IND", 0x08: "FEATURE_REQ",
          0x09: "FEATURE_RSP", 0x0e: "SLAVE_FEATURE_REQ", 0x14: "LENGTH_REQ", 0x15: "LENGTH_RSP",
          0x16: "PHY_REQ", 0x17: "PHY_RSP", 0x18: "PHY_UPDATE_IND", 0x07: "UNKNOWN_RSP", 0x0f: "CONN_PARAM_REQ"}


def rf_index(ch):  # номер RF-канала 0..39 (2402 МГц = 0) для заголовка pcap
    if ch == 37: return 0
    if ch == 38: return 12
    if ch == 39: return 39
    return ch + 1 if ch <= 10 else ch + 2


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.HUPCL | termios.CLOCAL
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def lines(fd):
    buf = b""
    while True:
        r, _, _ = select.select([fd], [], [], 0.5)
        if not r:
            yield None
            continue
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            continue
        if not chunk:
            raise SystemExit("Свисток отключился")
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.decode(errors="replace").strip()


def find_port():
    for p in sorted(glob.glob("/dev/cu.usbmodem*")):
        try:
            fd = open_port(p)
        except OSError:
            continue
        deadline = time.time() + 3
        for line in lines(fd):
            if line and line.startswith("# alive"):
                return p, fd
            if time.time() > deadline:
                break
        os.close(fd)
    raise SystemExit("Не нашёл свисток с прошивкой сниффера")


def describe(aa, pdu, role):
    hdr, ln, pl = pdu[0], pdu[1], pdu[2:]
    if aa == ADV_AA:
        t = hdr & 0x0F
        names = {0: "ADV_IND", 3: "SCAN_REQ", 4: "SCAN_RSP", 5: "CONNECT_IND"}
        if t == 5 and len(pl) >= 34:
            ll = pl[12:]
            return ("CONNECT_IND %s -> %s aa=%08x interval=%d timeout=%d chm=%s hop=%d sca=%d" % (
                ":".join("%02x" % b for b in pl[5::-1]), ":".join("%02x" % b for b in pl[11:5:-1]),
                int.from_bytes(ll[0:4], "little"), int.from_bytes(ll[10:12], "little"),
                int.from_bytes(ll[14:16], "little"), ll[16:21].hex(), ll[21] & 31, ll[21] >> 5))
        return None
    llid = hdr & 3
    if ln == 0:
        return None  # пустые пакеты
    if llid == 3:
        return "%s LL %s %s" % (role, LL_OPS.get(pl[0], "0x%02x" % pl[0]), pl[1:].hex(" "))
    if llid == 2 and len(pl) >= 5:
        l2len, cid = struct.unpack_from("<HH", pl)
        a = pl[4:]
        if cid == 4 and a:
            op = a[0]
            name = ATT_OPS.get(op, "ATT 0x%02x" % op)
            if op in (0x12, 0x52, 0x1b, 0x1d, 0x0a) and len(a) >= 3:
                h = int.from_bytes(a[1:3], "little")
                return "%s %s handle 0x%04x: %s" % (role, name, h, a[3:].hex(" "))
            return "%s %s %s" % (role, name, a[1:].hex(" "))
        return "%s L2CAP cid 0x%04x %s" % (role, cid, a.hex(" "))
    return "%s LL data (продолжение) %s" % (role, pl.hex(" "))


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else None
    out = sys.argv[2] if len(sys.argv) > 2 else time.strftime("dongle-%Y%m%d-%H%M%S.pcap")
    target = sys.argv[3].lower() if len(sys.argv) > 3 else None
    central, periph = ("RD ->", "   <- POD") if not target or target.startswith("c6:d5") else ("PHONE ->", "   <- RD")
    if port:
        fd = open_port(port)
    else:
        port, fd = find_port()
    print("Свисток на", port, "| пишу", out)
    if target:
        os.write(fd, ("t " + target.replace(":", "") + "\n").encode())

    pcap = open(out, "wb")
    pcap.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 256))  # DLT_BLUETOOTH_LE_LL_WITH_PHDR
    txt = open(os.path.splitext(out)[0] + ".txt", "w")

    t0_host, t0_dev, last_dev, wraps = time.time(), None, 0, 0
    event_seen = {}
    try:
        for line in lines(fd):
            if line is None:
                continue
            txt.write(line + "\n")
            if line.startswith("#"):
                if not line.startswith("# alive"):
                    print(time.strftime("%H:%M:%S"), line)
                continue
            parts = line.split()
            if len(parts) != 8 or parts[0] != "P":
                continue
            t, ch, rssi, crc = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
            aa, event, pdu = int(parts[5], 16), int(parts[6]), bytes.fromhex(parts[7])

            if t0_dev is None:
                t0_dev = t
            if t < last_dev:
                wraps += 1
            last_dev = t
            ts = t0_host + ((t + (wraps << 32)) - t0_dev) / 1e6

            flags = 0x0001 | 0x0002 | 0x0010 | 0x0400 | (0x0800 if crc else 0)
            phdr = struct.pack("<BbbBIH", rf_index(ch), -rssi, 0, 0, aa, flags)
            body = phdr + struct.pack("<I", aa) + pdu + b"\0\0\0"
            pcap.write(struct.pack("<IIII", int(ts), int((ts % 1) * 1e6), len(body), len(body)) + body)
            pcap.flush()

            # Первый пакет события шлёт переключатель (central), следующий — шифтер, и так по очереди
            if aa != ADV_AA:
                key = (aa, event)
                n = event_seen.get(key, 0)
                event_seen[key] = n + 1
                if len(event_seen) > 5000:
                    event_seen.clear()
                role = central if n % 2 == 0 else periph
            else:
                role = ""
            d = describe(aa, pdu, role) if crc else None
            if d:
                print("%s ev%-5d ch%-2d %4d dBm  %s" % (time.strftime("%H:%M:%S"), event, ch, -rssi, d))
    except KeyboardInterrupt:
        pass
    finally:
        pcap.close()
        txt.close()
        print("\nСохранено:", out)


if __name__ == "__main__":
    main()
