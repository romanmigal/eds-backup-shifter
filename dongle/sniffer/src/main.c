// Сниффер одного BLE-соединения для nRF52840 Dongle.
//
// Ждёт рекламу шифтера на рекламных каналах 37/38/39 и идёт за ним по каналам, как nRF Sniffer.
// Когда переключатель шлёт шифтеру CONNECT_IND, берёт оттуда access address, CRC init,
// интервал, карту каналов и hop и дальше сам прыгает по каналам данных (CSA #1).
// Hop = 0 (так делает переключатель Wheeltop) тоже работает: тогда канал всегда один.
//
// Каждый принятый пакет уходит на Мак строкой через USB-порт:
//   P <время мкс> <канал> <rssi> <crc 1/0> <access address> <номер события> <заголовок+payload hex>
// Служебные строки начинаются с "#".

#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "nrf.h"

// За кем следим, в порядке байтов в эфире. По умолчанию шифтер c0:00:00:00:00:01.
// Команда с Мака "t e00000000003" (адрес как пишут обычно) меняет цель, например на переключатель.
static uint8_t TARGET[6] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0xc0 };

#define ADV_AA        0x8E89BED6u
#define ADV_CRCINIT   0x555555u
// От начала пакета до события ADDRESS: 40 мкс на 1M PHY, 24 мкс на 2M
static uint32_t preamble_aa_us = 40;

//--------------------------------------------------------------------+
// Кольцевой буфер пакетов: пишет прерывание радио, читает main
//--------------------------------------------------------------------+
typedef struct {
  uint32_t t;
  uint32_t aa;
  uint16_t event;
  uint8_t  ch;
  uint8_t  rssi;
  uint8_t  crcok;
  uint16_t len;       // заголовок (2) + payload, до 257
  uint8_t  data[258];
} pkt_t;

#define RING 32
static pkt_t ring[RING];
static volatile uint32_t ring_head, ring_tail, ring_drops;

static uint8_t rxbuf[258] __attribute__((aligned(4)));

//--------------------------------------------------------------------+
// Состояние
//--------------------------------------------------------------------+
enum { ST_SCAN, ST_CONN };
static volatile int state = ST_SCAN;

static uint8_t  cur_ch;
static uint32_t cur_aa;

// Скан
static uint8_t  scan_ch = 37;
static uint32_t scan_armed_at;
static volatile bool scan_hop_armed;

// Соединение
static struct {
  uint32_t aa, crcinit;
  uint32_t interval_us;
  uint32_t timeout_us;
  uint8_t  hop;
  bool     csa2;            // ChSel = 1 в CONNECT_IND: каналы по CSA #2 (так делает переключатель)
  uint16_t chan_id;         // для CSA #2: старшая половина AA xor младшая
  uint8_t  used[37];
  uint8_t  nused;
  uint8_t  last_unmapped;
  uint16_t event;
  uint32_t anchor;          // ожидаемое (или пойманное) начало текущего события
  uint32_t last_sync;       // начало последнего пойманного события
  bool     first_in_event;
  // отложенные изменения по instant
  bool     chm_pending;  uint16_t chm_instant;  uint8_t chm_new[5];
  bool     upd_pending;  uint16_t upd_instant;  uint32_t upd_interval_us, upd_offset_us, upd_timeout_us;
  bool     phy_pending;  uint16_t phy_instant;  bool phy_2m;
} conn;

static volatile bool note_conn, note_scan;
static volatile uint32_t note_info[8];

//--------------------------------------------------------------------+
// Таймер: TIMER0 1 МГц. PPI 26/27 (заданы аппаратно) пишут время ADDRESS в CC[1], END в CC[2].
// CC[0] — наше расписание, CC[3] — "сейчас".
//--------------------------------------------------------------------+
static inline uint32_t now_us(void) {
  NRF_TIMER0->TASKS_CAPTURE[3] = 1;
  return NRF_TIMER0->CC[3];
}

static void schedule(uint32_t at) {
  uint32_t n = now_us();
  if ((int32_t) (at - n) < 30) at = n + 30;
  NRF_TIMER0->EVENTS_COMPARE[0] = 0;
  NRF_TIMER0->CC[0] = at;
  NRF_TIMER0->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
}

static void unschedule(void) {
  NRF_TIMER0->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk;
  NRF_TIMER0->EVENTS_COMPARE[0] = 0;
}

//--------------------------------------------------------------------+
// Радио
//--------------------------------------------------------------------+
static uint32_t ch_freq(uint8_t ch) {   // МГц минус 2400
  if (ch == 37) return 2;
  if (ch == 38) return 26;
  if (ch == 39) return 80;
  if (ch <= 10) return 4 + 2u * ch;
  return 28 + 2u * (ch - 11);
}

static void radio_tune(uint8_t ch, uint32_t aa, uint32_t crcinit) {
  NRF_RADIO->SHORTS = 0;
  NRF_RADIO->EVENTS_DISABLED = 0;
  NRF_RADIO->TASKS_DISABLE = 1;
  while (NRF_RADIO->EVENTS_DISABLED == 0) {}
  NRF_RADIO->EVENTS_DISABLED = 0;

  NRF_RADIO->FREQUENCY = ch_freq(ch);
  NRF_RADIO->DATAWHITEIV = ch;
  NRF_RADIO->BASE0 = aa << 8;
  NRF_RADIO->PREFIX0 = (aa >> 24) & 0xFF;
  NRF_RADIO->CRCINIT = crcinit;
  NRF_RADIO->PACKETPTR = (uint32_t) rxbuf;
  cur_ch = ch;
  cur_aa = aa;

  NRF_RADIO->EVENTS_END = 0;
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk |
                      RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
  NRF_RADIO->TASKS_RXEN = 1;
}

static void set_phy(bool two_m) {
  NRF_RADIO->MODE = two_m ? RADIO_MODE_MODE_Ble_2Mbit : RADIO_MODE_MODE_Ble_1Mbit;
  NRF_RADIO->PCNF0 = (8u << RADIO_PCNF0_LFLEN_Pos) | (1u << RADIO_PCNF0_S0LEN_Pos) |
                     ((two_m ? RADIO_PCNF0_PLEN_16bit : RADIO_PCNF0_PLEN_8bit) << RADIO_PCNF0_PLEN_Pos);
  preamble_aa_us = two_m ? 24 : 40;
}

static void radio_init(void) {
  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_HFCLKSTART = 1;
  while (NRF_CLOCK->EVENTS_HFCLKSTARTED == 0) {}

  NRF_TIMER0->TASKS_STOP = 1;
  NRF_TIMER0->MODE = TIMER_MODE_MODE_Timer;
  NRF_TIMER0->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
  NRF_TIMER0->PRESCALER = 4;
  NRF_TIMER0->TASKS_CLEAR = 1;
  NRF_TIMER0->TASKS_START = 1;
  NRF_PPI->CHENSET = (1u << 26) | (1u << 27);

  NRF_RADIO->POWER = 1;
  NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast;
  set_phy(false);
  NRF_RADIO->PCNF1 = (255u << RADIO_PCNF1_MAXLEN_Pos) | (3u << RADIO_PCNF1_BALEN_Pos) |
                     RADIO_PCNF1_WHITEEN_Msk;
  NRF_RADIO->CRCCNF = 3u | (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
  NRF_RADIO->CRCPOLY = 0x00065B;
  NRF_RADIO->RXADDRESSES = 1;
  NRF_RADIO->INTENSET = RADIO_INTENSET_END_Msk;

  NVIC_SetPriority(RADIO_IRQn, 0);
  NVIC_SetPriority(TIMER0_IRQn, 0);   // тот же приоритет: обработчики не прерывают друг друга
  NVIC_EnableIRQ(RADIO_IRQn);
  NVIC_EnableIRQ(TIMER0_IRQn);
}

//--------------------------------------------------------------------+
// Скан и вход в соединение
//--------------------------------------------------------------------+
static void start_scan(void) {
  unschedule();
  state = ST_SCAN;
  set_phy(false);
  scan_hop_armed = false;
  scan_ch = 37;
  radio_tune(scan_ch, ADV_AA, ADV_CRCINIT);
  note_scan = true;
}

static uint16_t csa2_perm(uint16_t v) {   // разворот битов в каждом байте
  v = (uint16_t) (((v & 0xF0F0) >> 4) | ((v & 0x0F0F) << 4));
  v = (uint16_t) (((v & 0xCCCC) >> 2) | ((v & 0x3333) << 2));
  v = (uint16_t) (((v & 0xAAAA) >> 1) | ((v & 0x5555) << 1));
  return v;
}

static uint8_t next_data_channel(void) {
  if (conn.csa2) {
    uint16_t prn = conn.event ^ conn.chan_id;
    for (int i = 0; i < 3; i++) prn = (uint16_t) (17u * csa2_perm(prn) + conn.chan_id);
    prn ^= conn.chan_id;
    uint8_t unmapped = (uint8_t) (prn % 37);
    for (int i = 0; i < conn.nused; i++) {
      if (conn.used[i] == unmapped) return unmapped;
    }
    return conn.used[((uint32_t) conn.nused * prn) >> 16];
  }
  conn.last_unmapped = (uint8_t) ((conn.last_unmapped + conn.hop) % 37);
  for (int i = 0; i < conn.nused; i++) {
    if (conn.used[i] == conn.last_unmapped) return conn.last_unmapped;
  }
  return conn.used[conn.last_unmapped % conn.nused];
}

static void set_chm(const uint8_t *chm) {
  conn.nused = 0;
  for (int c = 0; c < 37; c++) {
    if (chm[c / 8] & (1u << (c % 8))) conn.used[conn.nused++] = (uint8_t) c;
  }
  if (conn.nused == 0) { conn.used[0] = 0; conn.nused = 1; }
}

static uint32_t margin_us(void) {
  // Запас: 1 мс (на случай, если синхронизировались по ответу шифтера, а не по пакету RD)
  // плюс 1000 ppm от времени без синхронизации
  return 1000 + (conn.anchor - conn.last_sync) / 1000;
}

static void enter_conn(uint8_t hdr, const uint8_t *p, uint32_t t_end) {
  // p — payload CONNECT_IND: InitA[6] AdvA[6] AA[4] CRCInit[3] WinSize WinOffset[2] Interval[2]
  //                          Latency[2] Timeout[2] ChM[5] Hop|SCA
  const uint8_t *ll = p + 12;
  conn.aa = ll[0] | (ll[1] << 8) | (ll[2] << 16) | ((uint32_t) ll[3] << 24);
  conn.crcinit = ll[4] | (ll[5] << 8) | (ll[6] << 16);
  uint8_t  win_size = ll[7];
  uint16_t win_off = ll[8] | (ll[9] << 8);
  uint16_t interval = ll[10] | (ll[11] << 8);
  uint16_t timeout = ll[14] | (ll[15] << 8);
  conn.interval_us = interval * 1250u;
  conn.timeout_us = timeout * 10000u;
  set_chm(ll + 16);
  conn.hop = ll[21] & 0x1F;
  conn.csa2 = (hdr & 0x20) != 0;
  conn.chan_id = (uint16_t) ((conn.aa >> 16) ^ (conn.aa & 0xFFFF));
  conn.last_unmapped = 0;
  conn.event = 0;
  conn.chm_pending = conn.upd_pending = conn.phy_pending = false;

  conn.anchor = t_end + 1250 + win_off * 1250u;
  conn.last_sync = conn.anchor;
  conn.first_in_event = true;

  state = ST_CONN;
  radio_tune(next_data_channel(), conn.aa, conn.crcinit);
  // Следующее событие: после конца окна передачи + интервал
  schedule(conn.anchor + win_size * 1250u + conn.interval_us - margin_us());

  note_info[0] = conn.aa; note_info[1] = conn.crcinit; note_info[2] = interval;
  note_info[3] = conn.csa2 ? 100 + conn.hop : conn.hop; note_info[4] = conn.nused; note_info[5] = win_size;
  note_info[6] = win_off; note_info[7] = timeout;
  note_conn = true;
}

static void conn_next_event(void) {
  conn.event++;
  conn.anchor += conn.interval_us;

  if (conn.upd_pending && conn.event == conn.upd_instant) {
    conn.upd_pending = false;
    conn.anchor += conn.upd_offset_us;
    conn.interval_us = conn.upd_interval_us;
    conn.timeout_us = conn.upd_timeout_us;
  }
  if (conn.phy_pending && conn.event == conn.phy_instant) {
    conn.phy_pending = false;
    set_phy(conn.phy_2m);
  }
  if (conn.chm_pending && conn.event == conn.chm_instant) {
    conn.chm_pending = false;
    set_chm(conn.chm_new);
  }

  if ((int32_t) (now_us() - conn.last_sync) > (int32_t) conn.timeout_us) {
    start_scan();
    return;
  }

  conn.first_in_event = true;
  radio_tune(next_data_channel(), conn.aa, conn.crcinit);
  schedule(conn.anchor + conn.interval_us - margin_us());
}

// Управляющие PDU, которые меняют расписание
static void conn_ll_control(const uint8_t *pdu) {
  if ((pdu[0] & 0x03) != 0x03 || pdu[1] < 1) return;
  const uint8_t *c = pdu + 2;
  switch (c[0]) {
    case 0x00:   // LL_CONNECTION_UPDATE_IND
      if (pdu[1] < 12) return;
      conn.upd_offset_us = (c[2] | (c[3] << 8)) * 1250u;
      conn.upd_interval_us = (c[4] | (c[5] << 8)) * 1250u;
      conn.upd_timeout_us = (c[8] | (c[9] << 8)) * 10000u;
      conn.upd_instant = c[10] | (c[11] << 8);
      conn.upd_pending = true;
      break;
    case 0x01:   // LL_CHANNEL_MAP_IND
      if (pdu[1] < 8) return;
      memcpy(conn.chm_new, c + 1, 5);
      conn.chm_instant = c[6] | (c[7] << 8);
      conn.chm_pending = true;
      break;
    case 0x18:   // LL_PHY_UPDATE_IND: PHY M->S, PHY S->M, instant. Coded PHY не поддерживаем.
      if (pdu[1] < 5 || (c[1] == 0 && c[2] == 0)) return;
      conn.phy_2m = ((c[1] | c[2]) & 0x02) != 0;
      conn.phy_instant = c[3] | (c[4] << 8);
      conn.phy_pending = true;
      break;
    case 0x02:   // LL_TERMINATE_IND: дослушаем до таймаута, это безопасно
      break;
  }
}

//--------------------------------------------------------------------+
// Прерывания
//--------------------------------------------------------------------+
void RADIO_IRQHandler(void) {
  if (!NRF_RADIO->EVENTS_END) return;
  NRF_RADIO->EVENTS_END = 0;

  uint32_t t_addr = NRF_TIMER0->CC[1];
  uint32_t t_end = NRF_TIMER0->CC[2];
  uint8_t crcok = (uint8_t) NRF_RADIO->CRCSTATUS;
  uint8_t len = rxbuf[1];
  uint16_t total = (uint16_t) (2 + len);

  // Копируем сразу: END_START уже снова включил приём в тот же буфер
  uint32_t h = ring_head;
  pkt_t *pk = NULL;
  if (h - ring_tail < RING) {
    pk = &ring[h % RING];
    memcpy(pk->data, rxbuf, total);
    pk->len = total;
    pk->t = t_addr - preamble_aa_us;
    pk->ch = cur_ch;
    pk->aa = cur_aa;
    pk->rssi = (uint8_t) NRF_RADIO->RSSISAMPLE;
    pk->crcok = crcok;
    pk->event = (state == ST_CONN) ? conn.event : 0;
  } else {
    ring_drops++;
  }

  if (state == ST_SCAN) {
    uint8_t type = rxbuf[0] & 0x0F;
    const uint8_t *p = rxbuf + 2;
    if (crcok && type == 0x05 && len >= 34 && memcmp(p + 6, TARGET, 6) == 0) {
      if (pk) ring_head = h + 1;
      enter_conn(rxbuf[0], p, t_end);
      return;
    }
    // ADV_IND / ADV_NONCONN_IND / ADV_SCAN_IND / SCAN_RSP от шифтера: через 250 мкс на следующий канал,
    // если за это время не начался ответ (CONNECT_IND или SCAN_REQ)
    if (crcok && (type == 0x00 || type == 0x02 || type == 0x04 || type == 0x06) && len >= 6 &&
        memcmp(p, TARGET, 6) == 0) {
      scan_armed_at = t_end;
      scan_hop_armed = true;
      schedule(t_end + 250);
    }
  } else if (crcok) {
    if (conn.first_in_event) {
      conn.first_in_event = false;
      conn.anchor = t_addr - preamble_aa_us;
      conn.last_sync = conn.anchor;
      schedule(conn.anchor + conn.interval_us - margin_us());
    }
    conn_ll_control(rxbuf);
  }

  if (pk) ring_head = h + 1;
}

void TIMER0_IRQHandler(void) {
  if (!NRF_TIMER0->EVENTS_COMPARE[0]) return;
  NRF_TIMER0->EVENTS_COMPARE[0] = 0;

  if (state == ST_CONN) {
    conn_next_event();
    return;
  }

  unschedule();
  if (!scan_hop_armed) return;
  // Если после рекламы уже пошёл новый пакет, дождёмся его конца
  if ((int32_t) (NRF_TIMER0->CC[1] - scan_armed_at) > 0) {
    scan_armed_at = NRF_TIMER0->CC[1];
    schedule(now_us() + 400);
    return;
  }
  scan_hop_armed = false;
  scan_ch = (uint8_t) (scan_ch == 39 ? 37 : scan_ch + 1);
  radio_tune(scan_ch, ADV_AA, ADV_CRCINIT);
}

//--------------------------------------------------------------------+
// USB
//--------------------------------------------------------------------+
static void cdc_puts(const char *s) {
  if (!tud_cdc_connected()) return;
  tud_cdc_write_str(s);
}

static bool send_packet(const pkt_t *pk) {
  static char line[600];
  int n = snprintf(line, sizeof line, "P %lu %u %u %u %08lx %u ", (unsigned long) pk->t, pk->ch, pk->rssi,
                   pk->crcok, (unsigned long) pk->aa, pk->event);
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < pk->len; i++) {
    line[n++] = hex[pk->data[i] >> 4];
    line[n++] = hex[pk->data[i] & 15];
  }
  line[n++] = '\n';
  if (tud_cdc_write_available() < (uint32_t) n) return false;
  tud_cdc_write(line, (uint32_t) n);
  return true;
}

int main(void) {
  board_init();
  tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
  board_init_after_tusb();

  radio_init();
  start_scan();

  uint32_t last_beat = 0;
  while (1) {
    tud_task();

    // Пока Мак не открыл порт, пакеты не копим
    if (!tud_cdc_connected()) ring_tail = ring_head;

    while (ring_tail != ring_head) {
      if (!send_packet(&ring[ring_tail % RING])) break;
      ring_tail++;
    }

    char msg[160];
    if (note_conn) {
      note_conn = false;
      snprintf(msg, sizeof msg, "# CONN aa=%08lx crcinit=%06lx interval=%lu hop=%lu (100+ = CSA#2) channels=%lu win=%lu off=%lu timeout=%lu\n",
               note_info[0], note_info[1], note_info[2], note_info[3], note_info[4], note_info[5], note_info[6],
               note_info[7]);
      cdc_puts(msg);
      board_led_write(true);
    }
    if (note_scan) {
      note_scan = false;
      cdc_puts("# SCAN\n");
      board_led_write(false);
    }
    if (tusb_time_millis_api() - last_beat > 2000) {
      last_beat = tusb_time_millis_api();
      snprintf(msg, sizeof msg, "# alive state=%s drops=%lu\n", state == ST_CONN ? "conn" : "scan",
               (unsigned long) ring_drops);
      cdc_puts(msg);
    }
    tud_cdc_write_flush();

    // Команды с Мака: "r" — бросить соединение и снова искать цель; "t aabbccddeeff" — новая цель
    static char cmd[32];
    static int cmd_len;
    while (tud_cdc_available()) {
      char ch = (char) tud_cdc_read_char();
      if (ch != '\n' && ch != '\r') {
        if (cmd_len < (int) sizeof cmd - 1) cmd[cmd_len++] = ch;
        continue;
      }
      cmd[cmd_len] = 0;
      bool rescan = false;
      if (cmd[0] == 'r') rescan = true;
      if (cmd[0] == 't') {
        uint8_t a[6];
        int n = 0;
        for (const char *q = cmd + 1; *q && n < 12; q++) {
          int v = (*q >= '0' && *q <= '9') ? *q - '0' : (*q | 0x20) >= 'a' && (*q | 0x20) <= 'f' ? (*q | 0x20) - 'a' + 10 : -1;
          if (v < 0) continue;
          if (n % 2 == 0) a[n / 2] = (uint8_t) (v << 4); else a[n / 2] |= (uint8_t) v;
          n++;
        }
        if (n == 12) {
          for (int i = 0; i < 6; i++) TARGET[i] = a[5 - i];
          snprintf(msg, sizeof msg, "# TARGET %02x:%02x:%02x:%02x:%02x:%02x\n", a[0], a[1], a[2], a[3], a[4], a[5]);
          cdc_puts(msg);
          rescan = true;
        }
      }
      cmd_len = 0;
      if (rescan) {
        NVIC_DisableIRQ(RADIO_IRQn);
        NVIC_DisableIRQ(TIMER0_IRQn);
        start_scan();
        NVIC_EnableIRQ(RADIO_IRQn);
        NVIC_EnableIRQ(TIMER0_IRQn);
      }
    }
  }
}
