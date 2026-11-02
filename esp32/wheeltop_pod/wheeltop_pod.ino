// wheeltop_pod: an ESP32 board with two buttons that shifts a WHEELTOP EDS rear derailleur (RD).
//
// Two modes:
//   app (default)  The board connects to the RD the way the Wheeltop phone app does (BLE central).
//                  The native shifter stays paired and keeps working. On first start the board pairs with
//                  the nearest RD it finds and remembers it; `pair` or holding both buttons for 5 s pairs again.
//   pod            The board pretends to be the shifter (pod). The RD must be paired with the board instead
//                  of the native shifter. Kept for protocol research.
//
// The board is controlled with buttons on pins 4 and 5 (wiring in ../WIRING.md), over USB serial
// (115200, lines ending in \n) or from the Android app (../../android) over BLE: the board advertises as
// "WT Board" with a settings service whose command characteristic takes the u, d, pair and mode commands
// and whose status characteristic reports the state. Protocol: ../../wheeltop_eds_protocol_spec.md
//
// The board's BLE address is a random static address derived from the chip's MAC, so every board has
// its own address in pod mode too.
//
// Commands:
//   u / d              shift up / down
//   pair               forget the paired RD and pair with the nearest one (app mode)
//   mode app|pod       switch mode (restarts)
//   info               current settings
//   log                events since start-up
//   reset              restore defaults (restarts)
// Pod mode only:
//   f                  fine-tune on/off (the R/T switch)
//   type ox|tx-front|tx-left|tx-right   pod type (restarts)
//   mac AA:BB:CC:DD:EE:FF [public|random] | mac off   own BLE address (restarts)
//   raw 04 38 02 3e    send any notify frame on …0003
// Research:
//   s / a              scan 5 s: WHEELTOP devices only / everything
//   clone N            copy the address and advertisement of device N from the last scan (restarts)
//   gatt N             connect to device N from the scan and print its GATT with handles (needs mac off)
//   layout             print the handles of our own GATT

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <vector>

static const char *SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *RDWRITE_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // write, write-nr
static const char *BUTTONS_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // notify
// Settings service for the Android app
static const char *CFG_SERVICE_UUID = "b5f90001-1a2b-4c3d-8e9f-0a1b2c3d4e5f";
static const char *CFG_COMMAND_UUID = "b5f90002-1a2b-4c3d-8e9f-0a1b2c3d4e5f"; // write: a command line
static const char *CFG_STATUS_UUID = "b5f90003-1a2b-4c3d-8e9f-0a1b2c3d4e5f";  // read, notify: status
static const char *CFG_EVENT_UUID = "b5f90004-1a2b-4c3d-8e9f-0a1b2c3d4e5f";   // notify: "u"/"d" button presses
static const char *BOARD_NAME = "WT Board";

// Advertisement of a real TX Right pod: company id ff ff, prefix, type 0x38, firmware 5.7, battery,
// unknown bytes and, in the last 6 bytes, the address of the RD the pod is paired with (little-endian).
// The last 6 bytes are filled in from the paired RD (see podMfr()).
static const std::vector<uint8_t> DEFAULT_MFR = {0xff, 0xff, 0x07, 0x0f, 0x00, 0x14, 0x55, 0x6a, 0x84, 0x38, 0x05, 0x07,
                                                 0x01, 0x4a, 0x90, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t PREFIX[] = {0x07, 0x0f, 0x00, 0x14, 0x55, 0x6a, 0x84};

enum : uint8_t {
  DOWN_PRESSED = 0x01, UP_PRESSED = 0x02,
  DOWN_FINE_PRESSED = 0x05, UP_FINE_PRESSED = 0x06,
  DOWN_RELEASED = 0x09, UP_RELEASED = 0x0a,
};

Preferences prefs;
std::vector<uint8_t> mfr;
String macStr;     // empty: the ESP32's factory address
uint8_t macType;   // BLE_ADDR_PUBLIC or BLE_ADDR_RANDOM
bool appMode = true;
String rdStr;      // paired RD, "aa:bb:cc:dd:ee:ff"; empty: not paired yet
uint8_t rdType;

volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE; // pod mode: connection from the RD
volatile int subscribers = 0;
volatile int periphConns = 0;  // connections where the board is the peripheral: the RD in pod mode and phones
uint32_t advDueAt = 0;         // restart advertising at this time (0: not scheduled)
uint8_t ownAddrType = BLE_OWN_ADDR_PUBLIC;
bool fineTune = false;
// Like the real pod (sniffer, 2026-11-02): it answers the RD's 20-byte write to …0002 with an empty notify
// on …0003 and about 1.7 s after connecting asks for 40–80 ms, latency 4, timeout 2 s.
// The real pod sends no periodic status frames.
volatile bool ackPending = false;
uint32_t connectedAt = 0;
bool paramsRequested = false;

// External buttons on an ESP32-C3 SuperMini: button between the pin and GND, internal pull-up.
static const int PIN_UP = 4;     // upper button: shift up
static const int PIN_DOWN = 5;   // lower button: shift down
static const int PIN_FINE = 6;   // switch to GND = T position (fine-tune), open = R
static const int PIN_LED = 8;    // blue LED of the SuperMini, lit on LOW
static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t HOLD_MS = 540;     // the real pod sends its hold code ~540 ms after the press
static const uint32_t REPEAT_MS = 300;   // app mode: a held button shifts again every 300 ms
static const uint32_t REPAIR_MS = 5000;  // app mode: both buttons held this long start pairing

struct Button {
  int pin;
  bool up;
  bool pressed = false;
  bool holdSent = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
  uint32_t repeatAt = 0;
};
Button buttonUp{PIN_UP, true}, buttonDown{PIN_DOWN, false};

// After the first connection the RD remembers its subscription and may not send it again,
// so frames go to any connected RD, not only a subscribed one.
bool rdConnected() { return connHandle != BLE_HS_CONN_HANDLE_NONE; }

// BLE events are also kept here: the RD connects right after power-up, before the Mac opens the port. Command log.
String eventLog;
void logEvent(const char *fmt, ...) {
  char line[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  Serial.print(line);
  if (eventLog.length() < 6000) eventLog += String(millis()) + " ms: " + line;
}

struct Found {
  NimBLEAddress addr;
  std::string mfr;
};
std::vector<Found> found;
bool scanAll = false;
bool userScan = false; // scan started by s/a rather than the search for the RD

// ---------- utilities ----------

String hex(const uint8_t *p, size_t n) {
  String s;
  char b[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof b, i ? " %02x" : "%02x", p[i]);
    s += b;
  }
  return s;
}

std::vector<uint8_t> parseHex(String s) {
  std::vector<uint8_t> out;
  s.replace(" ", "");
  s.replace(":", "");
  for (int i = 0; i + 1 < (int)s.length(); i += 2) out.push_back(strtoul(s.substring(i, i + 2).c_str(), nullptr, 16));
  return out;
}

// Offset of the prefix in manufacturer data, or -1. The real pod has company id ff ff before the prefix.
int prefixOffset(const uint8_t *m, size_t n) {
  for (size_t off = 0; off <= 2; off += 2)
    if (n >= off + sizeof PREFIX && memcmp(m + off, PREFIX, sizeof PREFIX) == 0) return off;
  return -1;
}

// Index of the type byte in mfr
int typeIndex() {
  int off = prefixOffset(mfr.data(), mfr.size());
  return off >= 0 && mfr.size() > (size_t)off + 7 ? off + 7 : -1;
}

uint8_t podType() { return typeIndex() >= 0 ? mfr[typeIndex()] : 0x37; }
bool isOX() { return podType() == 0x37; }

const char *typeName(uint8_t t) {
  switch (t) {
    case 0x37: return "OX";
    case 0x39: return "TX Front";
    case 0x36: return "TX Left";
    case 0x38: return "TX Right";
    default: return "?";
  }
}

bool isWheeltop(const std::string &m) { return prefixOffset((const uint8_t *)m.data(), m.size()) >= 0; }

bool phoneConnected() { return periphConns > (rdConnected() ? 1 : 0); }

// The RD takes only one app-style central, so in app mode the board leaves the RD to a phone app that is
// subscribed to button events, and forwards its button presses to it. Phones subscribed to the event characteristic:
static const int MAX_PHONES = 3;
volatile uint16_t eventConns[MAX_PHONES] = {BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE};

void setEventConn(uint16_t conn, bool on) {
  for (int i = 0; i < MAX_PHONES; i++)
    if (eventConns[i] == conn) eventConns[i] = BLE_HS_CONN_HANDLE_NONE;
  if (!on) return;
  for (int i = 0; i < MAX_PHONES; i++)
    if (eventConns[i] == BLE_HS_CONN_HANDLE_NONE) { eventConns[i] = conn; return; }
}

bool phoneListening() {
  for (int i = 0; i < MAX_PHONES; i++)
    if (eventConns[i] != BLE_HS_CONN_HANDLE_NONE) return true;
  return false;
}

// Commands from the phone are executed in loop(), not in the BLE task: some of them block or restart.
portMUX_TYPE cmdLock = portMUX_INITIALIZER_UNLOCKED;
std::vector<String> phoneCmds;

// ---------- pod GATT, handle for handle like the real pod ----------
// NimBLEServer always adds its own characteristics to services 1800 and 1801, which shifts the handles.
// The RD seems to write to the pod's remembered handles, so the table is built by hand
// (read from a real pod with the gatt command, 2026-10-23):
//   0x0001 1800  0x0003 2A00 "EDS TX" read/write, 0x0005 2A01, 0x0007 2A04, 0x0009 2AA6
//   0x000a 1801  empty
//   0x000b NUS   0x000d …0002 write/write-nr, 0x000f …0003 notify, 0x0010 CCCD
// The settings service comes after them, so the pod's handles stay the same.

static ble_uuid16_t uuidGap, uuidGatt, uuidName, uuidAppearance, uuidPpcp, uuidCar;
static ble_uuid128_t uuidNus, uuidRdWrite, uuidButtons, uuidCfg, uuidCfgCommand, uuidCfgStatus, uuidCfgEvent;
static ble_gatt_chr_def gapChrs[5], nusChrs[3], cfgChrs[4];
static ble_gatt_svc_def podSvcs[5];
static uint16_t hName, hAppearance, hPpcp, hCar, hRdWrite, hButtons, hCfgCommand, hCfgStatus, hCfgEvent;
static char devName[32] = "EDS TX";
static const uint8_t APPEARANCE[] = {0x00, 0x00};
static const uint8_t PPCP[] = {0x20, 0x00, 0x40, 0x00, 0x04, 0x00, 0xc8, 0x00}; // 40–80 ms, latency 4, timeout 2 s
static const uint8_t CAR[] = {0x01};

void setUuid16(ble_uuid16_t &u, uint16_t v) {
  u.u.type = BLE_UUID_TYPE_16;
  u.value = v;
}

void setUuid128(ble_uuid128_t &u, const char *str) {
  NimBLEUUID n(str);
  memcpy(&u, n.getBase(), sizeof u);
}

int gapAccess(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    const void *d = CAR;
    size_t n = sizeof CAR;
    if (attr == hName) { d = devName; n = strlen(devName); }
    else if (attr == hAppearance) { d = APPEARANCE; n = sizeof APPEARANCE; }
    else if (attr == hPpcp) { d = PPCP; n = sizeof PPCP; }
    logEvent("READ by RD <- 0x%04x\n", attr);
    return os_mbuf_append(ctxt->om, d, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr == hName) {
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ctxt->om, devName, sizeof devName - 1, &len);
    devName[len] = 0;
    logEvent("RD wrote name: %s\n", devName);
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

int nusAccess(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint8_t buf[64];
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len);
    logEvent("WRITE by RD -> 0x%04x: %s\n", attr, hex(buf, len).c_str());
    if (attr == hRdWrite && len == 20) {
      if (!appMode && !rdConnected()) connHandle = conn; // the RD may skip subscribing on a reconnect
      ackPending = true;
    }
    return 0;
  }
  return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

String statusText();

int cfgAccess(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr == hCfgStatus) {
    String st = statusText();
    return os_mbuf_append(ctxt->om, st.c_str(), st.length()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr == hCfgCommand) {
    char buf[32];
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf - 1, &len);
    buf[len] = 0;
    portENTER_CRITICAL(&cmdLock);
    if (phoneCmds.size() < 8) phoneCmds.push_back(buf);
    portEXIT_CRITICAL(&cmdLock);
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

void startAdv();
void saveRd(const NimBLEAddress &a);

int gapEvent(ble_gap_event *ev, void *arg) {
  ble_gap_conn_desc desc;
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      // Sometimes the status is not 0 but the link exists anyway (the RD then changes the MTU and subscribes),
      // so we trust the connection, not the status.
      // A central is the RD (pod mode) or a phone. The RD is recognised by its stored address or, when pairing,
      // by subscribing to the button characteristic.
      if (ev->connect.status == 0 || ble_gap_conn_find(ev->connect.conn_handle, &desc) == 0) {
        periphConns++;
        if (ble_gap_conn_find(ev->connect.conn_handle, &desc) == 0) {
          NimBLEAddress peer(desc.peer_id_addr);
          logEvent("Central connected %s, status %d, handle %d\n", peer.toString().c_str(), ev->connect.status,
                   ev->connect.conn_handle);
          if (!appMode && rdStr.length() && peer == NimBLEAddress(rdStr.c_str(), rdType)) connHandle = ev->connect.conn_handle;
        }
        // Like the real pod, don't advertise while the RD is still setting up the link.
        advDueAt = (millis() + 2000) | 1;
      } else {
        logEvent("Connection failed: %d\n", ev->connect.status);
        startAdv();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      logEvent("Disconnected, reason 0x%03x\n", ev->disconnect.reason);
      if (periphConns > 0) periphConns--;
      setEventConn(ev->disconnect.conn.conn_handle, false);
      if (ev->disconnect.conn.conn_handle == connHandle) {
        connHandle = BLE_HS_CONN_HANDLE_NONE;
        subscribers = 0;
        connectedAt = 0;
        paramsRequested = false;
      }
      startAdv(); // the real pod advertises again right after a disconnect
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      logEvent("Subscription to 0x%04x: notify=%d\n", ev->subscribe.attr_handle, ev->subscribe.cur_notify);
      if (ev->subscribe.attr_handle == hCfgEvent) setEventConn(ev->subscribe.conn_handle, ev->subscribe.cur_notify);
      if (ev->subscribe.attr_handle == hButtons && !appMode) {
        connHandle = ev->subscribe.conn_handle;
        subscribers = ev->subscribe.cur_notify;
        if (ble_gap_conn_find(connHandle, &desc) == 0) saveRd(NimBLEAddress(desc.peer_id_addr));
        advDueAt = 0;
        startAdv();
      }
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      logEvent("Pairing: encryption, status %d\n", ev->enc_change.status);
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
      logEvent("Pairing: the RD asks again, old key deleted\n");
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_MTU:
      logEvent("MTU %d, handle %d\n", ev->mtu.value, ev->mtu.conn_handle);
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      startAdv();
      break;
    case BLE_GAP_EVENT_NOTIFY_TX:
      break;
    default:
      logEvent("BLE event %d\n", ev->type);
  }
  return 0;
}

// Pod mode without the RD: like the real pod, manufacturer data only, no flags or service UUID; the scan
// response carries the board's name so the phone app can find it. Otherwise, while no phone is connected:
// the settings service UUID and the name, for the phone app.
enum AdvKind { ADV_NONE, ADV_POD, ADV_SETTINGS };
AdvKind advKind = ADV_NONE;

void startAdv() {
  AdvKind want = (!appMode && !rdConnected()) ? ADV_POD : phoneConnected() ? ADV_NONE : ADV_SETTINGS;
  if (ble_gap_adv_active()) {
    if (want == advKind) return;
    ble_gap_adv_stop();
  }
  advKind = want;
  if (want == ADV_NONE) return;
  std::vector<uint8_t> raw;
  if (want == ADV_POD) {
    raw = {(uint8_t)(mfr.size() + 1), 0xff};
    raw.insert(raw.end(), mfr.begin(), mfr.end());
  } else {
    raw = {0x02, 0x01, 0x06, 0x11, 0x07};
    raw.insert(raw.end(), uuidCfg.value, uuidCfg.value + 16);
  }
  ble_gap_adv_set_data(raw.data(), raw.size());
  std::vector<uint8_t> rsp = {(uint8_t)(strlen(BOARD_NAME) + 1), 0x09};
  rsp.insert(rsp.end(), BOARD_NAME, BOARD_NAME + strlen(BOARD_NAME));
  ble_gap_adv_rsp_set_data(rsp.data(), rsp.size());
  ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  int rc = ble_gap_adv_start(ownAddrType, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
  if (rc) Serial.printf("Advertising did not start: %d\n", rc);
}

void registerPodGatt() {
  setUuid16(uuidGap, 0x1800);
  setUuid16(uuidGatt, 0x1801);
  setUuid16(uuidName, 0x2a00);
  setUuid16(uuidAppearance, 0x2a01);
  setUuid16(uuidPpcp, 0x2a04);
  setUuid16(uuidCar, 0x2aa6);
  setUuid128(uuidNus, SERVICE_UUID);
  setUuid128(uuidRdWrite, RDWRITE_UUID);
  setUuid128(uuidButtons, BUTTONS_UUID);
  setUuid128(uuidCfg, CFG_SERVICE_UUID);
  setUuid128(uuidCfgCommand, CFG_COMMAND_UUID);
  setUuid128(uuidCfgStatus, CFG_STATUS_UUID);
  setUuid128(uuidCfgEvent, CFG_EVENT_UUID);

  memset(gapChrs, 0, sizeof gapChrs);
  gapChrs[0] = {.uuid = &uuidName.u, .access_cb = gapAccess, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE, .val_handle = &hName};
  gapChrs[1] = {.uuid = &uuidAppearance.u, .access_cb = gapAccess, .flags = BLE_GATT_CHR_F_READ, .val_handle = &hAppearance};
  gapChrs[2] = {.uuid = &uuidPpcp.u, .access_cb = gapAccess, .flags = BLE_GATT_CHR_F_READ, .val_handle = &hPpcp};
  gapChrs[3] = {.uuid = &uuidCar.u, .access_cb = gapAccess, .flags = BLE_GATT_CHR_F_READ, .val_handle = &hCar};

  memset(nusChrs, 0, sizeof nusChrs);
  nusChrs[0] = {.uuid = &uuidRdWrite.u, .access_cb = nusAccess, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP, .val_handle = &hRdWrite};
  nusChrs[1] = {.uuid = &uuidButtons.u, .access_cb = nusAccess, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &hButtons};

  memset(cfgChrs, 0, sizeof cfgChrs);
  cfgChrs[0] = {.uuid = &uuidCfgCommand.u, .access_cb = cfgAccess, .flags = BLE_GATT_CHR_F_WRITE, .val_handle = &hCfgCommand};
  cfgChrs[1] = {.uuid = &uuidCfgStatus.u, .access_cb = cfgAccess, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &hCfgStatus};
  cfgChrs[2] = {.uuid = &uuidCfgEvent.u, .access_cb = cfgAccess, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &hCfgEvent};

  memset(podSvcs, 0, sizeof podSvcs);
  podSvcs[0] = {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &uuidGap.u, .characteristics = gapChrs};
  podSvcs[1] = {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &uuidGatt.u}; // empty, like the pod
  podSvcs[2] = {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &uuidNus.u, .characteristics = nusChrs};
  podSvcs[3] = {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &uuidCfg.u, .characteristics = cfgChrs};

  ble_gatts_reset();
  int rc = ble_gatts_count_cfg(podSvcs);
  if (!rc) rc = ble_gatts_add_svcs(podSvcs);
  if (!rc) rc = ble_gatts_start();
  if (rc) Serial.printf("GATT setup failed: %d\n", rc);
}

// ---------- pod frames ----------

void sendFrame(const uint8_t *d, size_t n, bool quiet = false) {
  if (!quiet) Serial.printf("-> %s%s\n", hex(d, n).c_str(), rdConnected() ? "" : "  (RD not connected, frame dropped)");
  if (!rdConnected()) return;
  ble_gattc_notify_custom(connHandle, hButtons, ble_hs_mbuf_from_flat(d, n)); // no subscription check
}

void sendCode(uint8_t code, bool quiet = false) {
  if (isOX()) {
    uint8_t f[3] = {0x04, code, (uint8_t)(0x04 ^ code)};
    sendFrame(f, 3, quiet);
  } else {
    uint8_t t = podType();
    uint8_t f[4] = {0x04, t, code, (uint8_t)((0x04 + t + code) & 0xff)};
    sendFrame(f, 4, quiet);
  }
}

uint8_t pressCode(bool up) { return up ? (fineTune ? UP_FINE_PRESSED : UP_PRESSED) : (fineTune ? DOWN_FINE_PRESSED : DOWN_PRESSED); }

// ---------- app mode ----------
// The board connects to the RD like the Wheeltop phone app (central); the native shifter stays paired.
// Protocol: section 19 of the spec.
// Frame: fe 32 <session> <command> <length> <data> <CRC-16/MODBUS, high byte first>.

NimBLEClient *appClient = nullptr;
NimBLERemoteCharacteristic *appWrite = nullptr;
volatile bool rdFound = false;     // the scan found the RD to connect to (rdAddr)
NimBLEAddress rdAddr;
volatile bool haveSession = false;
volatile uint8_t session = 0;      // from the RD's hello reply; the RD silently ignores commands with another session
volatile int lastGear = -1;
uint32_t helloAt = 0;

// Pairing: collect the RDs seen during PAIR_WINDOW_MS and take the strongest one.
static const uint32_t PAIR_WINDOW_MS = 3000;
bool pairing() { return appMode && rdStr.isEmpty(); }
NimBLEAddress pairBest;
int pairBestRssi = -1000;
uint32_t pairFirstSeen = 0;

bool appConnected() { return appClient && appClient->isConnected(); }

// While pairing the board needs the RD itself.
bool yieldedToPhone() { return appMode && !pairing() && phoneListening(); }

void notifyPhones(const char *s) {
  for (int i = 0; i < MAX_PHONES; i++) {
    uint16_t c = eventConns[i];
    if (c != BLE_HS_CONN_HANDLE_NONE) ble_gatts_notify_custom(c, hCfgEvent, ble_hs_mbuf_from_flat(s, strlen(s)));
  }
}

// An RD advertises its own address in manufacturer data after company id ff ff (in display order, not
// little-endian) and calls itself "EDS OX…". Pods have the prefix 07 0f 00 14 55 6a 84 instead.
bool isRd(const NimBLEAdvertisedDevice *d) {
  std::string m = d->getManufacturerData();
  if (isWheeltop(m)) return false;
  if (d->haveName() && d->getName().rfind("EDS OX", 0) == 0) return true;
  const uint8_t *a = d->getAddress().getVal(); // little-endian
  if (m.size() < 8 || (uint8_t)m[0] != 0xff || (uint8_t)m[1] != 0xff) return false;
  for (int i = 0; i < 6; i++)
    if ((uint8_t)m[2 + i] != a[5 - i]) return false;
  return true;
}

void saveRd(const NimBLEAddress &a) {
  String s = a.toString().c_str();
  if (s == rdStr) return;
  rdStr = s;
  rdType = a.getType();
  prefs.putString("rd", rdStr);
  prefs.putUChar("rdtype", rdType);
  logEvent("Paired with RD %s\n", rdStr.c_str());
}

void forgetRd() {
  rdStr = "";
  prefs.remove("rd");
  pairBestRssi = -1000;
  pairFirstSeen = 0;
  rdFound = false;
  if (appConnected()) appClient->disconnect();
  logEvent("Pairing: wake the derailleur (move the bike), keep the board close to it\n");
}

uint16_t crc16modbus(const uint8_t *p, size_t n) {
  uint16_t c = 0xffff;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xa001 : c >> 1;
  }
  return c;
}

void appSend(uint8_t s, uint8_t cmd, const uint8_t *data, uint8_t len) {
  uint8_t f[40] = {0xfe, 0x32, s, cmd, len};
  memcpy(f + 5, data, len);
  size_t n = 5 + len;
  uint16_t c = crc16modbus(f, n);
  f[n++] = c >> 8;
  f[n++] = c & 0xff;
  if (appWrite && appConnected()) appWrite->writeValue(f, n, false);
}

void appHello() {
  helloAt = millis();
  appSend(0x00, 0x11, (const uint8_t *)"yOTmK50z", 8);
}

void appShift(bool up) {
  if (yieldedToPhone()) {
    Serial.printf("Shift %s: sent to the phone app\n", up ? "UP" : "DOWN");
    return notifyPhones(up ? "u" : "d");
  }
  Serial.printf("Shift %s%s\n", up ? "UP" : "DOWN", haveSession ? "" : ": no session with the RD, skipped");
  if (!haveSession) return;
  uint8_t v = up ? 0x02 : 0x01;
  appSend(session, 0x99, &v, 1);
}

// The RD's replies are XORed with the key (K − 0x32), K being the second byte of the frame.
void onRdNotify(NimBLERemoteCharacteristic *c, uint8_t *b, size_t n, bool isNotify) {
  if (n < 5 || b[0] != 0xfe) {
    logEvent("<- RD %s\n", hex(b, n).c_str());
    return;
  }
  uint8_t key = b[1] - 0x32, q[64];
  size_t m = min(n - 4, sizeof q);
  for (size_t i = 0; i < m; i++) q[i] = b[2 + i] ^ key;
  if (m >= 2 && q[1] == 0x11) {
    session = q[0];
    haveSession = true;
    logEvent("RD: session %02x, ready to shift\n", session);
  } else if (m >= 8 && q[1] == 0x98) { // gear report S 98 06 4f 00 21 00 GG 00, after every shift and about once a second
    if (q[7] != lastGear) logEvent("RD: gear %d\n", q[7]);
    lastGear = q[7];
  } else if (m >= 4 && q[1] == 0x99) {
    if (q[3]) logEvent("RD: shift not accepted, code %02x\n", q[3]);
  } else {
    logEvent("<- RD %s\n", hex(q, m).c_str());
  }
}

void appConnect() {
  rdFound = false;
  NimBLEDevice::getScan()->stop();
  if (!appClient) {
    appClient = NimBLEDevice::createClient();
    appClient->setConnectTimeout(5000);
  }
  logEvent("Connecting to RD %s\n", rdAddr.toString().c_str());
  if (!appClient->connect(rdAddr)) {
    logEvent("RD: connection failed\n");
    return;
  }
  NimBLERemoteService *svc = appClient->getService(SERVICE_UUID);
  NimBLERemoteCharacteristic *notify = svc ? svc->getCharacteristic(BUTTONS_UUID) : nullptr;
  appWrite = svc ? svc->getCharacteristic(RDWRITE_UUID) : nullptr;
  if (!notify || !appWrite || !notify->subscribe(true, onRdNotify)) {
    logEvent("RD: no NUS service or subscription failed\n");
    appWrite = nullptr;
    appClient->disconnect();
    return;
  }
  logEvent("Connected to RD, sending hello\n");
  appHello();
}

// Find the RD, connect, search again after a disconnect. If the RD doesn't answer the hello, repeat every 2 s.
// While pairing, the RD is remembered only after it answers the hello.
void appLoop() {
  static bool yielded = false;
  if (!appConnected() && appWrite) {
    appWrite = nullptr;
    haveSession = false;
    lastGear = -1;
    logEvent("RD disconnected%s\n", yieldedToPhone() ? "" : ", searching again");
  }
  if (yieldedToPhone()) {
    if (!yielded) {
      yielded = true;
      logEvent("Phone app connected: leaving the RD to it, buttons go to the phone\n");
      if (appConnected()) appClient->disconnect();
      NimBLEDevice::getScan()->stop();
      rdFound = false;
    }
    return;
  }
  if (yielded) {
    yielded = false;
    logEvent("Phone app gone: connecting to the RD again\n");
  }
  if (!appConnected()) {
    if (pairing() && !rdFound && pairFirstSeen && millis() - pairFirstSeen > PAIR_WINDOW_MS) {
      rdAddr = pairBest;
      rdFound = true;
      pairFirstSeen = 0;
      pairBestRssi = -1000;
    }
    if (rdFound) appConnect();
    else if (!NimBLEDevice::getScan()->isScanning()) NimBLEDevice::getScan()->start(0, false, true);
    return;
  }
  if (haveSession && pairing()) saveRd(rdAddr);
  if (!haveSession && millis() - helloAt > 2000) appHello();
}

// App mode: a press shifts once, a held button keeps shifting. Both buttons held for 5 s start pairing.
void pollAppButton(Button &b) {
  uint32_t now = millis();
  bool down = digitalRead(b.pin) == LOW;
  if (down != b.pressed && now - b.changedAt > DEBOUNCE_MS) {
    b.changedAt = now;
    b.pressed = down;
    if (down) {
      b.pressedAt = b.repeatAt = now;
      b.holdSent = false;
      if (!buttonUp.pressed || !buttonDown.pressed) appShift(b.up);
    }
  }
  if (buttonUp.pressed && buttonDown.pressed) {
    if (b.up && !b.holdSent && now - max(buttonUp.pressedAt, buttonDown.pressedAt) > REPAIR_MS) {
      b.holdSent = true;
      forgetRd();
    }
    return;
  }
  if (b.pressed && now - b.pressedAt > HOLD_MS && now - b.repeatAt > REPEAT_MS) {
    b.repeatAt = now;
    appShift(b.up);
  }
}

// Pod mode: like the real pod, press, one hold after ~540 ms (recorded 2026-10-23), release.
void pollButton(Button &b) {
  if (appMode) return pollAppButton(b);
  uint32_t now = millis();
  bool down = digitalRead(b.pin) == LOW;
  if (down != b.pressed && now - b.changedAt > DEBOUNCE_MS) {
    b.changedAt = now;
    b.pressed = down;
    if (down) {
      b.pressedAt = now;
      b.holdSent = false;
      Serial.printf("Button %s%s\n", b.up ? "UP" : "DOWN", fineTune ? " (fine-tune)" : "");
      sendCode(pressCode(b.up));
    } else {
      sendCode(b.up ? UP_RELEASED : DOWN_RELEASED);
    }
  }
  if (b.pressed && !b.holdSent && now - b.pressedAt > HOLD_MS) {
    b.holdSent = true;
    uint8_t hold = b.up ? (fineTune ? 0x08 : 0x04) : (fineTune ? 0x07 : 0x03);
    sendCode(hold);
  }
}

// Changes fine-tune only when the switch moves, so the f command still works.
void pollFineSwitch() {
  static uint32_t changedAt = 0;
  static bool last = digitalRead(PIN_FINE) == LOW;
  bool t = digitalRead(PIN_FINE) == LOW;
  if (t != last && millis() - changedAt > DEBOUNCE_MS) {
    changedAt = millis();
    last = t;
    fineTune = t;
    Serial.printf("Switch: %s\n", fineTune ? "T (fine-tune)" : "R");
  }
}

// Connected to the RD: steady. Searching: short flash once a second. Pairing: fast blinking.
// Left the RD to the phone app: slow blinking.
void updateLed() {
  bool on;
  if (pairing()) on = (millis() % 200) < 100;
  else if (yieldedToPhone()) on = (millis() % 2000) < 1000; // slow blinking: the phone app has the RD
  else if (appMode ? haveSession : rdConnected()) on = true;
  else on = (millis() % 1000) < 60;
  digitalWrite(PIN_LED, on ? LOW : HIGH);
}

void click(bool up) {
  if (appMode) return appShift(up);
  Serial.printf("Shift %s%s\n", up ? "UP" : "DOWN", fineTune ? " (fine-tune)" : "");
  sendCode(pressCode(up));
  delay(120);
  sendCode(up ? UP_RELEASED : DOWN_RELEASED);
}

// ---------- scanning ----------

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *d) override {
    if (appMode && !rdFound && !appConnected() && !yieldedToPhone() && isRd(d)) {
      if (!pairing()) {
        if (d->getAddress() == NimBLEAddress(rdStr.c_str(), rdType)) {
          rdAddr = d->getAddress();
          rdFound = true;
        }
      } else {
        if (!pairFirstSeen) pairFirstSeen = millis();
        if (d->getRSSI() > pairBestRssi) {
          pairBestRssi = d->getRSSI();
          pairBest = d->getAddress();
        }
      }
    }
    if (!userScan) return;
    std::string m = d->getManufacturerData();
    bool wt = isWheeltop(m);
    if (!wt && !scanAll) return;
    for (auto &f : found)
      if (f.addr == d->getAddress()) return;
    found.push_back({d->getAddress(), m});
    Serial.printf("%2u) %s (%s) rssi %d%s%s\n", (unsigned)found.size(), d->getAddress().toString().c_str(),
                  d->getAddress().getType() == BLE_ADDR_PUBLIC ? "public" : "random", d->getRSSI(),
                  d->haveName() ? ("  \"" + d->getName() + "\"").c_str() : "", wt ? "  <- WHEELTOP" : "");
    int off = wt ? prefixOffset((const uint8_t *)m.data(), m.size()) : -1;
    if (off >= 0 && m.size() > (size_t)off + 7) Serial.printf("    type 0x%02x %s\n", (uint8_t)m[off + 7], typeName(m[off + 7]));
    if (!m.empty()) Serial.printf("    mfr: %s\n", hex((const uint8_t *)m.data(), m.size()).c_str());
    Serial.printf("    raw: %s\n", hex(d->getPayload().data(), d->getPayload().size()).c_str());
  }
  void onScanEnd(const NimBLEScanResults &r, int reason) override {
    if (!userScan) return;
    userScan = false;
    Serial.printf("Scan finished, found: %u\n", (unsigned)found.size());
  }
};

// ---------- GATT dump ----------

String props(const NimBLERemoteCharacteristic *c) {
  String s;
  if (c->canRead()) s += " read";
  if (c->canWrite()) s += " write";
  if (c->canWriteNoResponse()) s += " write-nr";
  if (c->canNotify()) s += " notify";
  if (c->canIndicate()) s += " indicate";
  return s;
}

// Connects to a device as a central and prints all services, characteristics and descriptors with handles.
void dumpGatt(const NimBLEAddress &addr) {
  ble_gap_adv_stop();
  NimBLEClient *c = NimBLEDevice::createClient();
  Serial.printf("Connecting to %s…\n", addr.toString().c_str());
  if (!c->connect(addr)) {
    Serial.println("Connection failed");
  } else {
    for (auto *svc : c->getServices(true)) {
      Serial.printf("service %s  handles 0x%04x-0x%04x\n", svc->getUUID().toString().c_str(), svc->getStartHandle(), svc->getEndHandle());
      for (auto *chr : svc->getCharacteristics(true)) {
        Serial.printf("  char %s  value 0x%04x [%s ]\n", chr->getUUID().toString().c_str(), chr->getHandle(), props(chr).c_str());
        if (chr->canRead()) {
          NimBLEAttValue v = chr->readValue();
          String text;
          for (size_t i = 0; i < v.size(); i++) text += isprint(v[i]) ? (char)v[i] : '.';
          Serial.printf("    reads: %s  \"%s\"\n", hex(v.data(), v.size()).c_str(), text.c_str());
        }
        for (auto *dsc : chr->getDescriptors(true))
          Serial.printf("    desc %s  0x%04x\n", dsc->getUUID().toString().c_str(), dsc->getHandle());
      }
    }
    c->disconnect();
  }
  NimBLEDevice::deleteClient(c);
  Serial.println("GATT dump finished");
  if (!appMode) startAdv();
}

// BLE_UUID16_DECLARE is a C compound literal and doesn't compile in C++, so UUIDs live in NimBLEUUID.
void showChr(const ble_uuid_t *svc, uint16_t chr, const char *name) {
  uint16_t def, val, cccd;
  NimBLEUUID c(chr), cccdUuid((uint16_t)0x2902);
  if (ble_gatts_find_chr(svc, c.getBase(), &def, &val) == 0) {
    Serial.printf("  0x%04x %s: declaration 0x%04x, value 0x%04x", chr, name, def, val);
    if (ble_gatts_find_dsc(svc, c.getBase(), cccdUuid.getBase(), &cccd) == 0) Serial.printf(", CCCD 0x%04x", cccd);
    Serial.println();
  }
}

// Our own GATT: find the known services and characteristics to compare handles with the real pod.
void printLayout() {
  uint16_t h;
  NimBLEUUID gapUuid((uint16_t)0x1800), gattUuid((uint16_t)0x1801), cccdUuid((uint16_t)0x2902);
  const ble_uuid_t *gap = gapUuid.getBase(), *gatt = gattUuid.getBase();
  if (ble_gatts_find_svc(gap, &h) == 0) Serial.printf("service 1800 (GAP) from 0x%04x\n", h);
  showChr(gap, 0x2a00, "name");
  showChr(gap, 0x2a01, "appearance");
  showChr(gap, 0x2a04, "connection parameters");
  showChr(gap, 0x2aa6, "address resolution");
  if (ble_gatts_find_svc(gatt, &h) == 0) Serial.printf("service 1801 (GATT) from 0x%04x\n", h);
  showChr(gatt, 0x2a05, "service changed");
  showChr(gatt, 0x2b29, "client features");
  showChr(gatt, 0x2b2a, "database hash");
  showChr(gatt, 0x2b3a, "server features");
  NimBLEUUID svc(SERVICE_UUID), rd(RDWRITE_UUID), bt(BUTTONS_UUID);
  if (ble_gatts_find_svc(svc.getBase(), &h) == 0) Serial.printf("service NUS from 0x%04x\n", h);
  uint16_t def, val, cccd;
  if (ble_gatts_find_chr(svc.getBase(), rd.getBase(), &def, &val) == 0)
    Serial.printf("  …0002: declaration 0x%04x, value 0x%04x\n", def, val);
  if (ble_gatts_find_chr(svc.getBase(), bt.getBase(), &def, &val) == 0) {
    Serial.printf("  …0003: declaration 0x%04x, value 0x%04x", def, val);
    if (ble_gatts_find_dsc(svc.getBase(), bt.getBase(), cccdUuid.getBase(), &cccd) == 0) Serial.printf(", CCCD 0x%04x", cccd);
    Serial.println();
  }
}

// ---------- settings ----------

// Default pod advertisement with the paired RD's address in the last 6 bytes.
std::vector<uint8_t> podMfr() {
  std::vector<uint8_t> m = DEFAULT_MFR;
  if (rdStr.length()) {
    const uint8_t *a = NimBLEAddress(rdStr.c_str(), rdType).getVal();
    memcpy(m.data() + m.size() - 6, a, 6);
  }
  return m;
}

void loadPrefs() {
  prefs.begin("wheeltop", false);
  rdStr = prefs.getString("rd", "");
  rdType = prefs.getUChar("rdtype", BLE_ADDR_RANDOM);
  size_t n = prefs.getBytesLength("mfr");
  if (n) {
    mfr.resize(n);
    prefs.getBytes("mfr", mfr.data(), n);
  } else {
    mfr = podMfr();
  }
  macStr = prefs.getString("mac", "");
  macType = prefs.getUChar("atype", BLE_ADDR_PUBLIC);
  appMode = prefs.getString("mode", "app") == "app";
}

void restart() {
  Serial.println("Restarting…");
  Serial.flush();
  delay(100);
  ESP.restart();
}

// Status for the phone app: key=value pairs separated by ';'.
String statusText() {
  bool connected = appMode ? appConnected() : rdConnected();
  String s = String("mode=") + (appMode ? "app" : "pod");
  s += ";paired=" + (rdStr.length() ? rdStr : String("-"));
  s += ";rd=" + String(connected ? 1 : 0);
  s += ";ready=" + String((appMode ? haveSession : rdConnected()) ? 1 : 0);
  s += ";pairing=" + String(pairing() ? 1 : 0);
  s += ";yield=" + String(yieldedToPhone() ? 1 : 0);
  s += ";gear=" + String(lastGear);
  s += ";addr=" + String(NimBLEDevice::getAddress().toString().c_str());
  return s;
}

void printInfo() {
  Serial.printf("Paired RD: %s\n", rdStr.length() ? rdStr.c_str() : "none");
  Serial.printf("BLE address: %s%s\n", NimBLEDevice::getAddress().toString().c_str(),
                macStr.length() ? " (custom)" : " (from the chip MAC)");
  if (appMode) {
    Serial.printf("Mode: app. RD connected: %s, session: %s, gear: %d\n", appConnected() ? "yes" : "no",
                  haveSession ? "yes" : "no", lastGear);
    return;
  }
  Serial.println("Mode: pod");
  Serial.printf("Pod type: 0x%02x %s\n", podType(), typeName(podType()));
  Serial.printf("Advertisement (mfr): %s\n", hex(mfr.data(), mfr.size()).c_str());
  Serial.printf("RD connected: %s, subscribed: %s, fine-tune: %s, advertising: %s\n", rdConnected() ? "yes" : "no",
                subscribers ? "yes" : "no", fineTune ? "on" : "off", ble_gap_adv_active() ? "on" : "OFF");
}

// ---------- commands ----------

void handle(String line) {
  line.trim();
  if (!line.length()) return;
  String cmd = line, arg;
  int sp = line.indexOf(' ');
  if (sp > 0) {
    cmd = line.substring(0, sp);
    arg = line.substring(sp + 1);
    arg.trim();
  }

  if (cmd == "u") click(true);
  else if (cmd == "d") click(false);
  else if (cmd == "pair") {
    if (!appMode) { Serial.println("Pod mode: power the RD briefly, then press a board button within ~8 s"); return; }
    forgetRd();
  } else if (cmd == "mode") {
    if (arg != "app" && arg != "pod") { Serial.println("mode app|pod"); return; }
    prefs.putString("mode", arg);
    restart();
  } else if (cmd == "f") {
    fineTune = !fineTune;
    Serial.printf("Fine-tune: %s\n", fineTune ? "on (T)" : "off (R)");
  } else if (cmd == "s" || cmd == "a") {
    scanAll = cmd == "a";
    userScan = true;
    found.clear();
    Serial.printf("Scanning 5 s (%s)…\n", scanAll ? "all devices" : "WHEELTOP only");
    NimBLEDevice::getScan()->start(5000, false, true);
  } else if (cmd == "clone") {
    int i = arg.toInt();
    if (i < 1 || i > (int)found.size()) { Serial.println("No such number, run s first"); return; }
    Found &f = found[i - 1];
    if (f.mfr.empty()) { Serial.println("The device has no manufacturer data"); return; }
    prefs.putBytes("mfr", f.mfr.data(), f.mfr.size());
    prefs.putString("mac", f.addr.toString().c_str());
    prefs.putUChar("atype", f.addr.getType());
    Serial.printf("Copied %s. Turn off the real pod, or the addresses will clash.\n", f.addr.toString().c_str());
    restart();
  } else if (cmd == "type") {
    uint8_t t = arg == "ox" ? 0x37 : arg == "tx-front" ? 0x39 : arg == "tx-left" ? 0x36 : arg == "tx-right" ? 0x38 : 0;
    if (!t) { Serial.println("type ox|tx-front|tx-left|tx-right"); return; }
    if (typeIndex() < 0) mfr = podMfr();
    mfr[typeIndex()] = t;
    prefs.putBytes("mfr", mfr.data(), mfr.size());
    restart();
  } else if (cmd == "mac") {
    if (arg == "off") {
      prefs.remove("mac");
    } else {
      int sp2 = arg.indexOf(' ');
      String a = sp2 > 0 ? arg.substring(0, sp2) : arg;
      bool random = sp2 > 0 && arg.substring(sp2 + 1) == "random";
      if (parseHex(a).size() != 6) { Serial.println("mac AA:BB:CC:DD:EE:FF [public|random] | mac off"); return; }
      a.toLowerCase();
      prefs.putString("mac", a);
      prefs.putUChar("atype", random ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC);
    }
    restart();
  } else if (cmd == "raw") {
    std::vector<uint8_t> d = parseHex(arg);
    if (d.empty()) { Serial.println("raw 04 02 06"); return; }
    sendFrame(d.data(), d.size());
  } else if (cmd == "gatt") {
    int i = arg.toInt();
    if (i < 1 || i > (int)found.size()) { Serial.println("No such number, run s first"); return; }
    if (macStr.length()) { Serial.println("Run mac off first: the board uses a pod's address now"); return; }
    dumpGatt(found[i - 1].addr);
  } else if (cmd == "layout") {
    printLayout();
  } else if (cmd == "reset") {
    prefs.clear();
    restart();
  } else if (cmd == "log") {
    Serial.print(eventLog.length() ? eventLog : String("no events\n"));
    Serial.printf("Bonding keys stored: %d\n", NimBLEDevice::getNumBonds());
  } else if (cmd == "info") printInfo();
  else Serial.printf("Unknown command: %s\n", line.c_str());
}

// ---------- setup / loop ----------

void setup() {
  Serial.begin(115200);
  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DOWN, INPUT_PULLUP);
  pinMode(PIN_FINE, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  delay(300);
  fineTune = digitalRead(PIN_FINE) == LOW;
  loadPrefs();

  // A public address is replaced before Bluetooth starts, a random one after init.
  if (macStr.length() && macType == BLE_ADDR_PUBLIC) {
    std::vector<uint8_t> m = parseHex(macStr);
    if (esp_iface_mac_addr_set(m.data(), ESP_MAC_BT) != ESP_OK) Serial.println("Could not set the MAC");
  }

  NimBLEDevice::init("");
  // Without a custom address the board uses a random static address made from its own MAC:
  // unique per board and stable across restarts.
  String own;
  if (macStr.length()) {
    if (macType == BLE_ADDR_RANDOM) own = macStr;
  } else {
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_BT);
    char a[18];
    snprintf(a, sizeof a, "%02x:%02x:%02x:%02x:%02x:%02x", m[0] | 0xc0, m[1], m[2], m[3], m[4], m[5]);
    own = a;
  }
  if (own.length()) {
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    NimBLEDevice::setOwnAddr(NimBLEAddress(own.c_str(), BLE_ADDR_RANDOM));
    ownAddrType = BLE_OWN_ADDR_RANDOM;
  }
  // Allow pairing without a PIN: if the RD asks for it, the log shows it.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  registerPodGatt();
  startAdv();

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCB(), false);
  scan->setActiveScan(true);

  Serial.println("WHEELTOP board ready. Commands: u d pair mode info log reset | f type mac raw | s a clone gatt layout");
  printInfo();
  if (pairing()) logEvent("Pairing: wake the derailleur (move the bike), keep the board close to it\n");
}

void loop() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      handle(buf);
      buf = "";
    } else {
      buf += c;
    }
  }
  pollButton(buttonUp);
  pollButton(buttonDown);
  pollFineSwitch();
  updateLed();
  if (appMode) appLoop();
  if (advDueAt && (int32_t)(millis() - advDueAt) >= 0) {
    advDueAt = 0;
    startAdv();
  }
  // Commands from the phone app
  String cmd;
  portENTER_CRITICAL(&cmdLock);
  if (!phoneCmds.empty()) {
    cmd = phoneCmds.front();
    phoneCmds.erase(phoneCmds.begin());
  }
  portEXIT_CRITICAL(&cmdLock);
  if (cmd == "u" || cmd == "d" || cmd == "pair" || cmd == "mode app" || cmd == "mode pod") {
    logEvent("Phone: %s\n", cmd.c_str());
    handle(cmd);
  }
  // Tell the phone when the status changes
  static String lastStatus;
  static uint32_t statusAt = 0;
  if (millis() - statusAt > 300) {
    statusAt = millis();
    String st = statusText();
    if (st != lastStatus) {
      lastStatus = st;
      ble_gatts_chr_updated(hCfgStatus);
    }
  }
  if (ackPending) {
    ackPending = false;
    static const uint8_t none = 0;
    sendFrame(&none, 0, true);
    logEvent("Reply to RD: empty notify\n");
  }
  // The link may appear without CONNECT (status 26), so the connection time is set here.
  if (rdConnected() && connectedAt == 0) connectedAt = millis();
  if (rdConnected() && !paramsRequested && millis() - connectedAt > 1700) {
    paramsRequested = true;
    ble_gap_upd_params p = {};
    p.itvl_min = 32;               // 40 ms
    p.itvl_max = 64;               // 80 ms
    p.latency = 4;
    p.supervision_timeout = 200;   // 2 s
    int rc = ble_gap_update_params(connHandle, &p);
    logEvent("Requested 40–80 ms, latency 4: %d\n", rc);
  }
}
