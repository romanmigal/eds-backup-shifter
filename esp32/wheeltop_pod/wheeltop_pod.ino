// wheeltop_pod: ESP32 притворяется шифтером (подом) WHEELTOP EDS.
// Мак шлёт команды по USB (115200, строки с \n), ESP32 шлёт заднему переключателю (RD) кадры кнопок.
// Или без Мака: кнопки на ножках 4 и 5, переключатель R/T на 6 (схема в ../WIRING.md).
// Протокол: ../../wheeltop_eds_protocol_spec.md
//
// Команды:
//   u / d              передача вверх / вниз (press, 120 мс, release)
//   f                  fine-tune вкл/выкл (ползунок R/T)
//   s / a              скан 5 с: только поды WHEELTOP / все устройства
//   clone N            скопировать адрес и рекламу пода N из последнего скана (с перезагрузкой)
//   type ox|tx-front|tx-left|tx-right   тип пода (с перезагрузкой)
//   mac AA:BB:CC:DD:EE:FF [public|random] | mac off   свой BLE-адрес (с перезагрузкой)
//   raw 04 38 02 3e    отправить произвольный кадр notify на …0003
//   gatt N             подключиться к устройству N из скана и вывести его GATT с хэндлами (нужен mac off)
//   layout             вывести хэндлы своего GATT
//   reset              вернуть настройки по умолчанию
//   info               текущие настройки

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <vector>

static const char *SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
// Как в стандартном NUS, проверено на настоящем TX Right 2026-10-23 (спецификация говорила наоборот).
static const char *RDWRITE_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // write, write-nr: запись от RD
static const char *BUTTONS_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // notify: кнопки

// Реклама настоящего пода TX Right, снятая Маком 2026-10-23: company id ff ff, префикс, тип 0x38,
// прошивка 5.7, батарея 3.30 В и ещё 9 байт неизвестного назначения.
static const std::vector<uint8_t> DEFAULT_MFR = {0xff, 0xff, 0x07, 0x0f, 0x00, 0x14, 0x55, 0x6a, 0x84, 0x38, 0x05, 0x07,
                                                 0x01, 0x4a, 0x90, 0x6a, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0xe0};
static const uint8_t PREFIX[] = {0x07, 0x0f, 0x00, 0x14, 0x55, 0x6a, 0x84};

enum : uint8_t {
  DOWN_PRESSED = 0x01, UP_PRESSED = 0x02,
  DOWN_FINE_PRESSED = 0x05, UP_FINE_PRESSED = 0x06,
  DOWN_RELEASED = 0x09, UP_RELEASED = 0x0a,
};

Preferences prefs;
std::vector<uint8_t> mfr;
String macStr;     // пусто: заводской адрес ESP32
uint8_t macType;   // BLE_ADDR_PUBLIC или BLE_ADDR_RANDOM

volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE; // соединение с RD
volatile int subscribers = 0;
uint8_t ownAddrType = BLE_OWN_ADDR_PUBLIC;
bool fineTune = false;
// Как у настоящего пода (сниффер, 2026-11-02, mac/sniff12.pcap):
// на 20 байт от RD в …0002 под отвечает пустым notify на …0003,
// а примерно через 1,7 с после подключения просит параметры 40–80 мс, latency 4, таймаут 2 с.
// Статус-кадров раз в секунду настоящий под RD не шлёт.
volatile bool ackPending = false;
uint32_t connectedAt = 0;
bool paramsRequested = false;

// Внешние кнопки на ESP32-C3 SuperMini: кнопка между ножкой и GND, подтяжка внутренняя.
static const int PIN_UP = 4;     // верхняя кнопка: передача вверх
static const int PIN_DOWN = 5;   // нижняя кнопка: передача вниз
static const int PIN_FINE = 6;   // переключатель на GND = положение T (fine-tune), разомкнут = R
static const int PIN_LED = 8;    // синий светодиод SuperMini, горит при LOW
static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t HOLD_MS = 540; // настоящий под шлёт код удержания через ~540 мс после нажатия

struct Button {
  int pin;
  bool up;
  bool pressed = false;
  bool holdSent = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
};
Button buttonUp{PIN_UP, true}, buttonDown{PIN_DOWN, false};

// RD после первого подключения помнит подписку и может не присылать её снова,
// поэтому кадры шлём любому подключённому, а не только подписанному.
bool rdConnected() { return connHandle != BLE_HS_CONN_HANDLE_NONE; }

// События BLE копятся и здесь: RD подключается сразу после включения, раньше, чем Мак откроет порт. Команда log.
String eventLog;
void logEvent(const char *fmt, ...) {
  char line[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  Serial.print(line);
  if (eventLog.length() < 6000) eventLog += String(millis()) + " мс: " + line;
}

struct Found {
  NimBLEAddress addr;
  std::string mfr;
};
std::vector<Found> found;
bool scanAll = false;

// ---------- утилиты ----------

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

// Смещение префикса в manufacturer data или -1. У настоящего пода перед префиксом стоит company id ff ff.
int prefixOffset(const uint8_t *m, size_t n) {
  for (size_t off = 0; off <= 2; off += 2)
    if (n >= off + sizeof PREFIX && memcmp(m + off, PREFIX, sizeof PREFIX) == 0) return off;
  return -1;
}

// Индекс байта типа в mfr
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

// ---------- GATT как у настоящего пода ----------
// NimBLEServer всегда добавляет свои характеристики в сервисы 1800 и 1801, и хэндлы съезжают.
// RD, похоже, пишет по запомненным хэндлам пода, поэтому таблица собрана вручную,
// хэндл в хэндл как у пода (снято командой gatt 2026-10-23):
//   0x0001 1800  0x0003 2A00 "EDS TX" read/write, 0x0005 2A01, 0x0007 2A04, 0x0009 2AA6
//   0x000a 1801  пустой
//   0x000b NUS   0x000d …0002 write/write-nr, 0x000f …0003 notify, 0x0010 CCCD

static ble_uuid16_t uuidGap, uuidGatt, uuidName, uuidAppearance, uuidPpcp, uuidCar;
static ble_uuid128_t uuidNus, uuidRdWrite, uuidButtons;
static ble_gatt_chr_def gapChrs[5], nusChrs[3];
static ble_gatt_svc_def podSvcs[4];
static uint16_t hName, hAppearance, hPpcp, hCar, hRdWrite, hButtons;
static char devName[32] = "EDS TX";
static const uint8_t APPEARANCE[] = {0x00, 0x00};
static const uint8_t PPCP[] = {0x20, 0x00, 0x40, 0x00, 0x04, 0x00, 0xc8, 0x00}; // 40–80 мс, latency 4, таймаут 2 с
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
    logEvent("ЧТЕНИЕ от RD <- 0x%04x\n", attr);
    return os_mbuf_append(ctxt->om, d, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr == hName) {
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ctxt->om, devName, sizeof devName - 1, &len);
    devName[len] = 0;
    logEvent("ЗАПИСЬ имени от RD: %s\n", devName);
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

int nusAccess(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint8_t buf[64];
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len);
    logEvent("ЗАПИСЬ от RD -> 0x%04x: %s\n", attr, hex(buf, len).c_str());
    if (attr == hRdWrite && len == 20) ackPending = true;
    return 0;
  }
  return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

void startAdv();

int gapEvent(ble_gap_event *ev, void *arg) {
  ble_gap_conn_desc desc;
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      // Бывает, что статус не 0, а связь всё равно есть (RD потом меняет MTU и подписывается),
      // поэтому верим наличию соединения, а не статусу.
      if (ev->connect.status == 0 || ble_gap_conn_find(ev->connect.conn_handle, &desc) == 0) {
        connHandle = ev->connect.conn_handle;
        if (ble_gap_conn_find(connHandle, &desc) == 0)
          logEvent("Подключился central %s, статус %d, handle %d\n", NimBLEAddress(desc.peer_id_addr).toString().c_str(),
                        ev->connect.status, connHandle);
      } else {
        logEvent("Подключение не удалось: %d\n", ev->connect.status);
        startAdv();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      logEvent("Отключился, причина 0x%03x\n", ev->disconnect.reason);
      connHandle = BLE_HS_CONN_HANDLE_NONE;
      subscribers = 0;
      connectedAt = 0;
      paramsRequested = false;
      startAdv(); // настоящий под снова рекламируется сразу после разрыва
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      logEvent("Подписка на 0x%04x: notify=%d\n", ev->subscribe.attr_handle, ev->subscribe.cur_notify);
      if (!rdConnected()) connHandle = ev->subscribe.conn_handle;
      if (ev->subscribe.attr_handle == hButtons) subscribers = ev->subscribe.cur_notify;
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      logEvent("Сопряжение: шифрование, статус %d\n", ev->enc_change.status);
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
      logEvent("Сопряжение: RD просит заново, старый ключ удалён\n");
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_MTU:
      if (!rdConnected()) connHandle = ev->mtu.conn_handle; // связь есть, даже если CONNECT пришёл с ошибкой
      logEvent("MTU %d, handle %d\n", ev->mtu.value, ev->mtu.conn_handle);
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      if (!rdConnected()) startAdv();
      break;
    case BLE_GAP_EVENT_NOTIFY_TX:
      break;
    default:
      logEvent("Событие BLE %d\n", ev->type);
  }
  return 0;
}

// Как у настоящего пода: в рекламе только manufacturer data, без флагов, имени и UUID сервиса, ответа на скан нет.
void startAdv() {
  if (ble_gap_adv_active()) return;
  std::vector<uint8_t> raw = {(uint8_t)(mfr.size() + 1), 0xff};
  raw.insert(raw.end(), mfr.begin(), mfr.end());
  ble_gap_adv_set_data(raw.data(), raw.size());
  ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  int rc = ble_gap_adv_start(ownAddrType, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
  if (rc) Serial.printf("Реклама не запустилась: %d\n", rc);
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

  memset(gapChrs, 0, sizeof gapChrs);
  gapChrs[0] = {.uuid = &uuidName.u, .access_cb = gapAccess, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE, .val_handle = &hName};
  gapChrs[1] = {.uuid = &uuidAppearance.u, .access_cb = gapAccess, .flags = BLE_GATT_CHR_F_READ, .val_handle = &hAppearance};
  gapChrs[2] = {.uuid = &uuidPpcp.u, .access_cb = gapAccess, .flags = BLE_GATT_CHR_F_READ, .val_handle = &hPpcp};
  gapChrs[3] = {.uuid = &uuidCar.u, .access_cb = gapAccess, .flags = BLE_GATT_CHR_F_READ, .val_handle = &hCar};

  memset(nusChrs, 0, sizeof nusChrs);
  nusChrs[0] = {.uuid = &uuidRdWrite.u, .access_cb = nusAccess, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP, .val_handle = &hRdWrite};
  nusChrs[1] = {.uuid = &uuidButtons.u, .access_cb = nusAccess, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &hButtons};

  memset(podSvcs, 0, sizeof podSvcs);
  podSvcs[0] = {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &uuidGap.u, .characteristics = gapChrs};
  podSvcs[1] = {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &uuidGatt.u}; // пустой, как у пода
  podSvcs[2] = {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &uuidNus.u, .characteristics = nusChrs};

  ble_gatts_reset();
  int rc = ble_gatts_count_cfg(podSvcs);
  if (!rc) rc = ble_gatts_add_svcs(podSvcs);
  if (!rc) rc = ble_gatts_start();
  if (rc) Serial.printf("GATT не собрался: %d\n", rc);
}

// ---------- кадры ----------

void sendFrame(const uint8_t *d, size_t n, bool quiet = false) {
  if (!quiet) Serial.printf("-> %s%s\n", hex(d, n).c_str(), rdConnected() ? "" : "  (RD не подключён, кадр в никуда)");
  if (!rdConnected()) return;
  ble_gattc_notify_custom(connHandle, hButtons, ble_hs_mbuf_from_flat(d, n)); // без проверки подписки
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

// Повторяет кадры настоящего пода: press, один hold после ~540 мс (так в записи 2026-10-23), release.
void pollButton(Button &b) {
  uint32_t now = millis();
  bool down = digitalRead(b.pin) == LOW;
  if (down != b.pressed && now - b.changedAt > DEBOUNCE_MS) {
    b.changedAt = now;
    b.pressed = down;
    if (down) {
      b.pressedAt = now;
      b.holdSent = false;
      Serial.printf("Кнопка %s%s\n", b.up ? "ВВЕРХ" : "ВНИЗ", fineTune ? " (fine-tune)" : "");
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

// Меняет режим только когда переключатель сдвинули, чтобы команда f с Мака тоже работала.
void pollFineSwitch() {
  static uint32_t changedAt = 0;
  static bool last = digitalRead(PIN_FINE) == LOW;
  bool t = digitalRead(PIN_FINE) == LOW;
  if (t != last && millis() - changedAt > DEBOUNCE_MS) {
    changedAt = millis();
    last = t;
    fineTune = t;
    Serial.printf("Переключатель: %s\n", fineTune ? "T (fine-tune)" : "R");
  }
}

// Подключён RD: горит. Ждём RD: короткая вспышка раз в секунду.
void updateLed() {
  bool on = rdConnected() ? true : (millis() % 1000) < 60;
  digitalWrite(PIN_LED, on ? LOW : HIGH);
}

void click(bool up) {
  Serial.printf("Передача %s%s\n", up ? "ВВЕРХ" : "ВНИЗ", fineTune ? " (fine-tune)" : "");
  sendCode(pressCode(up));
  delay(120);
  sendCode(up ? UP_RELEASED : DOWN_RELEASED);
}

// ---------- BLE-колбэки ----------

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *d) override {
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
    if (off >= 0 && m.size() > (size_t)off + 7) Serial.printf("    тип 0x%02x %s\n", (uint8_t)m[off + 7], typeName(m[off + 7]));
    if (!m.empty()) Serial.printf("    mfr: %s\n", hex((const uint8_t *)m.data(), m.size()).c_str());
    Serial.printf("    raw: %s\n", hex(d->getPayload().data(), d->getPayload().size()).c_str());
  }
  void onScanEnd(const NimBLEScanResults &r, int reason) override {
    Serial.printf("Скан окончен, найдено: %u\n", (unsigned)found.size());
  }
};

// ---------- разбор GATT ----------

String props(const NimBLERemoteCharacteristic *c) {
  String s;
  if (c->canRead()) s += " read";
  if (c->canWrite()) s += " write";
  if (c->canWriteNoResponse()) s += " write-nr";
  if (c->canNotify()) s += " notify";
  if (c->canIndicate()) s += " indicate";
  return s;
}

// Подключается к устройству как central и печатает все сервисы, характеристики и дескрипторы с хэндлами.
void dumpGatt(const NimBLEAddress &addr) {
  ble_gap_adv_stop();
  NimBLEClient *c = NimBLEDevice::createClient();
  Serial.printf("Подключаюсь к %s…\n", addr.toString().c_str());
  if (!c->connect(addr)) {
    Serial.println("Не подключился");
  } else {
    for (auto *svc : c->getServices(true)) {
      Serial.printf("сервис %s  хэндлы 0x%04x-0x%04x\n", svc->getUUID().toString().c_str(), svc->getStartHandle(), svc->getEndHandle());
      for (auto *chr : svc->getCharacteristics(true)) {
        Serial.printf("  хар-ка %s  значение 0x%04x [%s ]\n", chr->getUUID().toString().c_str(), chr->getHandle(), props(chr).c_str());
        if (chr->canRead()) {
          NimBLEAttValue v = chr->readValue();
          String text;
          for (size_t i = 0; i < v.size(); i++) text += isprint(v[i]) ? (char)v[i] : '.';
          Serial.printf("    читается: %s  \"%s\"\n", hex(v.data(), v.size()).c_str(), text.c_str());
        }
        for (auto *dsc : chr->getDescriptors(true))
          Serial.printf("    дескр %s  0x%04x\n", dsc->getUUID().toString().c_str(), dsc->getHandle());
      }
    }
    c->disconnect();
  }
  NimBLEDevice::deleteClient(c);
  Serial.println("Разбор GATT окончен");
  startAdv();
}

// BLE_UUID16_DECLARE это составной литерал C, в C++ он не компилируется, поэтому UUID держим в NimBLEUUID.
void showChr(const ble_uuid_t *svc, uint16_t chr, const char *name) {
  uint16_t def, val, cccd;
  NimBLEUUID c(chr), cccdUuid((uint16_t)0x2902);
  if (ble_gatts_find_chr(svc, c.getBase(), &def, &val) == 0) {
    Serial.printf("  0x%04x %s: объявление 0x%04x, значение 0x%04x", chr, name, def, val);
    if (ble_gatts_find_dsc(svc, c.getBase(), cccdUuid.getBase(), &cccd) == 0) Serial.printf(", CCCD 0x%04x", cccd);
    Serial.println();
  }
}

// Свой GATT: ищем известные сервисы и характеристики, чтобы сравнить хэндлы с настоящим подом.
void printLayout() {
  uint16_t h;
  NimBLEUUID gapUuid((uint16_t)0x1800), gattUuid((uint16_t)0x1801), cccdUuid((uint16_t)0x2902);
  const ble_uuid_t *gap = gapUuid.getBase(), *gatt = gattUuid.getBase();
  if (ble_gatts_find_svc(gap, &h) == 0) Serial.printf("сервис 1800 (GAP) с 0x%04x\n", h);
  showChr(gap, 0x2a00, "имя");
  showChr(gap, 0x2a01, "внешний вид");
  showChr(gap, 0x2a04, "параметры соединения");
  showChr(gap, 0x2aa6, "разрешение адреса");
  if (ble_gatts_find_svc(gatt, &h) == 0) Serial.printf("сервис 1801 (GATT) с 0x%04x\n", h);
  showChr(gatt, 0x2a05, "service changed");
  showChr(gatt, 0x2b29, "client features");
  showChr(gatt, 0x2b2a, "database hash");
  showChr(gatt, 0x2b3a, "server features");
  NimBLEUUID svc(SERVICE_UUID), rd(RDWRITE_UUID), bt(BUTTONS_UUID);
  if (ble_gatts_find_svc(svc.getBase(), &h) == 0) Serial.printf("сервис NUS с 0x%04x\n", h);
  uint16_t def, val, cccd;
  if (ble_gatts_find_chr(svc.getBase(), rd.getBase(), &def, &val) == 0)
    Serial.printf("  …0002: объявление 0x%04x, значение 0x%04x\n", def, val);
  if (ble_gatts_find_chr(svc.getBase(), bt.getBase(), &def, &val) == 0) {
    Serial.printf("  …0003: объявление 0x%04x, значение 0x%04x", def, val);
    if (ble_gatts_find_dsc(svc.getBase(), bt.getBase(), cccdUuid.getBase(), &cccd) == 0) Serial.printf(", CCCD 0x%04x", cccd);
    Serial.println();
  }
}

// ---------- настройки ----------

void loadPrefs() {
  prefs.begin("wheeltop", false);
  size_t n = prefs.getBytesLength("mfr");
  if (n) {
    mfr.resize(n);
    prefs.getBytes("mfr", mfr.data(), n);
  } else {
    mfr = DEFAULT_MFR;
  }
  macStr = prefs.getString("mac", "");
  macType = prefs.getUChar("atype", BLE_ADDR_PUBLIC);
}

void restart() {
  Serial.println("Перезагрузка…");
  Serial.flush();
  delay(100);
  ESP.restart();
}

void printInfo() {
  Serial.printf("Тип пода: 0x%02x %s\n", podType(), typeName(podType()));
  Serial.printf("Реклама (mfr): %s\n", hex(mfr.data(), mfr.size()).c_str());
  Serial.printf("BLE-адрес: %s%s\n", NimBLEDevice::getAddress().toString().c_str(),
                macStr.length() ? " (подменён)" : " (заводской)");
  Serial.printf("RD подключён: %s, подписан: %s, fine-tune: %s, реклама: %s\n", rdConnected() ? "да" : "нет",
                subscribers ? "да" : "нет", fineTune ? "вкл" : "выкл",
                ble_gap_adv_active() ? "идёт" : "НЕ ИДЁТ");
}

// ---------- команды ----------

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
  else if (cmd == "f") {
    fineTune = !fineTune;
    Serial.printf("Fine-tune: %s\n", fineTune ? "вкл (T)" : "выкл (R)");
  } else if (cmd == "s" || cmd == "a") {
    scanAll = cmd == "a";
    found.clear();
    Serial.printf("Сканирую 5 с (%s)…\n", scanAll ? "все устройства" : "только WHEELTOP");
    NimBLEDevice::getScan()->start(5000, false, true);
  } else if (cmd == "clone") {
    int i = arg.toInt();
    if (i < 1 || i > (int)found.size()) { Serial.println("Нет такого номера, сначала s"); return; }
    Found &f = found[i - 1];
    if (f.mfr.empty()) { Serial.println("У устройства нет manufacturer data"); return; }
    prefs.putBytes("mfr", f.mfr.data(), f.mfr.size());
    prefs.putString("mac", f.addr.toString().c_str());
    prefs.putUChar("atype", f.addr.getType());
    Serial.printf("Скопировал %s. Выключи настоящий под, иначе адреса совпадут.\n", f.addr.toString().c_str());
    restart();
  } else if (cmd == "type") {
    uint8_t t = arg == "ox" ? 0x37 : arg == "tx-front" ? 0x39 : arg == "tx-left" ? 0x36 : arg == "tx-right" ? 0x38 : 0;
    if (!t) { Serial.println("type ox|tx-front|tx-left|tx-right"); return; }
    if (typeIndex() < 0) mfr = DEFAULT_MFR;
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
    if (i < 1 || i > (int)found.size()) { Serial.println("Нет такого номера, сначала s"); return; }
    if (macStr.length()) { Serial.println("Сначала mac off: плата сейчас с адресом пода"); return; }
    dumpGatt(found[i - 1].addr);
  } else if (cmd == "layout") {
    printLayout();
  } else if (cmd == "reset") {
    prefs.clear();
    restart();
  } else if (cmd == "log") {
    Serial.print(eventLog.length() ? eventLog : String("событий нет\n"));
    Serial.printf("Ключей сопряжения в памяти: %d\n", NimBLEDevice::getNumBonds());
  } else if (cmd == "info") printInfo();
  else Serial.printf("Неизвестная команда: %s\n", line.c_str());
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

  // Публичный адрес подменяем до запуска Bluetooth, случайный задаём после init.
  if (macStr.length() && macType == BLE_ADDR_PUBLIC) {
    std::vector<uint8_t> m = parseHex(macStr);
    if (esp_iface_mac_addr_set(m.data(), ESP_MAC_BT) != ESP_OK) Serial.println("Не удалось подменить MAC");
  }

  NimBLEDevice::init("");
  if (macStr.length() && macType == BLE_ADDR_RANDOM) {
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    NimBLEDevice::setOwnAddr(NimBLEAddress(macStr.c_str(), BLE_ADDR_RANDOM));
    ownAddrType = BLE_OWN_ADDR_RANDOM;
  }
  // Разрешаем сопряжение без PIN: если RD его попросит, увидим это в логе.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  registerPodGatt();
  startAdv();

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCB(), false);
  scan->setActiveScan(true);

  Serial.println("WHEELTOP pod emulator готов. Команды: u d f s a clone type mac raw reset info");
  printInfo();
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
  if (ackPending) {
    ackPending = false;
    static const uint8_t none = 0;
    sendFrame(&none, 0, true);
    logEvent("Ответ RD: пустой notify\n");
  }
  // Связь могла появиться и без CONNECT (статус 26), поэтому время подключения ставим здесь
  if (rdConnected() && connectedAt == 0) connectedAt = millis();
  if (rdConnected() && !paramsRequested && millis() - connectedAt > 1700) {
    paramsRequested = true;
    ble_gap_upd_params p = {};
    p.itvl_min = 32;               // 40 мс
    p.itvl_max = 64;               // 80 мс
    p.latency = 4;
    p.supervision_timeout = 200;   // 2 с
    int rc = ble_gap_update_params(connHandle, &p);
    logEvent("Запрос параметров 40–80 мс, latency 4: %d\n", rc);
  }
}
