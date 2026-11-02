// rd-app: Мак подключается к заднему переключателю (RD) так же, как приложение Wheeltop,
// и переключает передачи командами приложения. Шифтер при этом остаётся сопряжённым с RD.
// Протокол записан свистком 2026-11-03 (mac/sniff16-app.*), раздел 19 спецификации.
//
// Кадр: fe <K> <байты…> <CRC-16/MODBUS по всему кадру, старший байт первым>.
// Байты после K перемешаны XOR с ключом (K − 0x32). Мы, как и приложение, шлём K = 0x32, то есть без перемешивания.
// Открытый вид: <сеанс> <команда> <длина> <данные…>
//   fe 32 00 11 08 "yOTmK50z"  — приветствие; RD отвечает <сеанс> 11 01 <сеанс>
//   fe 32 <сеанс> 99 01 02     — передача вверх, 01 — вниз
//
// Клавиши: u — вверх, d — вниз, i — повторить всё, что слало приложение при подключении, q — выход.
// Сборка: ./build.sh

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <termios.h>
#include <unistd.h>

static NSString *const kService = @"6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static NSString *const kWrite = @"6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static NSString *const kNotify = @"6e400003-b5a3-f393-e0a9-e50e24dcca9e";
static const uint8_t RD_ADDR[6] = {0xe0, 0x00, 0x00, 0x00, 0x00, 0x03}; // в manufacturer data RD после ff ff

static uint16_t crc16modbus(const uint8_t *p, size_t n) {
  uint16_t c = 0xffff;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xa001 : c >> 1;
  }
  return c;
}

static NSData *appFrame(uint8_t flag, uint8_t cmd, const uint8_t *data, uint8_t len) {
  NSMutableData *d = [NSMutableData data];
  uint8_t head[5] = {0xfe, 0x32, flag, cmd, len};
  [d appendBytes:head length:5];
  if (len) [d appendBytes:data length:len];
  uint16_t c = crc16modbus(d.bytes, d.length);
  uint8_t tail[2] = {(uint8_t)(c >> 8), (uint8_t)(c & 0xff)};
  [d appendBytes:tail length:2];
  return d;
}

static NSString *hex(NSData *d) {
  NSMutableString *s = [NSMutableString string];
  const uint8_t *p = d.bytes;
  for (NSUInteger i = 0; i < d.length; i++) [s appendFormat:i ? @" %02x" : @"%02x", p[i]];
  return s;
}

static NSString *ascii(NSData *d) {
  NSMutableString *s = [NSMutableString string];
  const uint8_t *p = d.bytes;
  for (NSUInteger i = 0; i < d.length; i++) [s appendFormat:@"%c", (p[i] >= 32 && p[i] < 127) ? p[i] : '.'];
  return s;
}

@interface App : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@end

@implementation App {
  CBCentralManager *_cm;
  CBPeripheral *_rd;
  CBCharacteristic *_w;
  uint8_t _session; // номер сеанса из ответа RD на приветствие
}

- (instancetype)init {
  if ((self = [super init])) _cm = [[CBCentralManager alloc] initWithDelegate:self queue:nil];
  return self;
}

- (void)log:(NSString *)s {
  NSDateFormatter *f = [NSDateFormatter new];
  f.dateFormat = @"HH:mm:ss.SSS";
  printf("%s  %s\r\n", [f stringFromDate:NSDate.date].UTF8String, s.UTF8String);
  fflush(stdout);
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)c {
  if (c.state != CBManagerStatePoweredOn) { [self log:@"Bluetooth выключен или нет доступа"]; return; }
  [self log:@"Ищу переключатель EDS… покачайте велосипед"];
  [c scanForPeripheralsWithServices:nil options:nil];
}

- (void)centralManager:(CBCentralManager *)c didDiscoverPeripheral:(CBPeripheral *)p
     advertisementData:(NSDictionary *)ad RSSI:(NSNumber *)rssi {
  NSData *m = ad[CBAdvertisementDataManufacturerDataKey];
  NSString *name = ad[CBAdvertisementDataLocalNameKey] ?: p.name;
  BOOL ours = (m.length >= 8 && memcmp((const uint8_t *)m.bytes + 2, RD_ADDR, 6) == 0) ||
              [name hasPrefix:@"EDS OX"];
  static NSMutableSet *seen;
  if (!seen) seen = [NSMutableSet set];
  if ((name || m) && ![seen containsObject:p.identifier]) {
    [seen addObject:p.identifier];
    if ([name containsString:@"EDS"] || (m.length >= 2 && ((const uint8_t *)m.bytes)[0] == 0xff))
      [self log:[NSString stringWithFormat:@"вижу %@ mfr %@", name, hex(m)]];
  }
  if (!ours) return;
  [self log:[NSString stringWithFormat:@"Нашёл %@ (%@ дБм), подключаюсь", p.name ?: ad[CBAdvertisementDataLocalNameKey], rssi]];
  [c stopScan];
  _rd = p;
  p.delegate = self;
  [c connectPeripheral:p options:nil];
}

- (void)centralManager:(CBCentralManager *)c didConnectPeripheral:(CBPeripheral *)p {
  [self log:@"Подключился, ищу сервис"];
  [p discoverServices:@[[CBUUID UUIDWithString:kService]]];
}

- (void)centralManager:(CBCentralManager *)c didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)e {
  [self log:[NSString stringWithFormat:@"Отключился: %@. Ищу снова", e.localizedDescription ?: @"-"]];
  _w = nil;
  [c scanForPeripheralsWithServices:nil options:nil];
}

- (void)centralManager:(CBCentralManager *)c didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)e {
  [self log:[NSString stringWithFormat:@"Не подключился: %@", e.localizedDescription]];
  [c scanForPeripheralsWithServices:nil options:nil];
}

- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)e {
  for (CBService *s in p.services) [p discoverCharacteristics:nil forService:s];
}

- (void)peripheral:(CBPeripheral *)p didDiscoverCharacteristicsForService:(CBService *)s error:(NSError *)e {
  for (CBCharacteristic *ch in s.characteristics) {
    if ([ch.UUID isEqual:[CBUUID UUIDWithString:kWrite]]) _w = ch;
    if ([ch.UUID isEqual:[CBUUID UUIDWithString:kNotify]]) [p setNotifyValue:YES forCharacteristic:ch];
  }
}

- (void)peripheral:(CBPeripheral *)p didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)ch error:(NSError *)e {
  if (e) { [self log:[NSString stringWithFormat:@"Подписка не удалась: %@", e.localizedDescription]]; return; }
  [self log:@"Подписался. Шлю приветствие приложения. Клавиши: u — вверх, d — вниз, i — инициализация, q — выход"];
  [self hello];
}

- (void)peripheral:(CBPeripheral *)p didUpdateValueForCharacteristic:(CBCharacteristic *)ch error:(NSError *)e {
  NSData *v = ch.value;
  const uint8_t *b = v.bytes;
  if (v.length >= 5 && b[0] == 0xfe) {
    uint8_t key = (uint8_t)(b[1] - 0x32);
    NSMutableData *plain = [NSMutableData dataWithLength:v.length - 4];
    uint8_t *q = plain.mutableBytes;
    for (NSUInteger i = 0; i < plain.length; i++) q[i] = b[2 + i] ^ key;
    if (plain.length >= 2 && q[1] == 0x11) {
      _session = q[0];
      [self log:[NSString stringWithFormat:@"<- %@   ответ на приветствие, сеанс %02x", hex(v), _session]];
      return;
    }
    [self log:[NSString stringWithFormat:@"<- %@   открыто: %@", hex(v), hex(plain)]];
    return;
  }
  [self log:[NSString stringWithFormat:@"<- %@   %@", hex(v), ascii(v)]];
}

- (void)send:(NSData *)d {
  if (!_w) { [self log:@"Ещё не подключён"]; return; }
  [self log:[NSString stringWithFormat:@"-> %@", hex(d)]];
  [_rd writeValue:d forCharacteristic:_w type:CBCharacteristicWriteWithoutResponse];
}

- (void)hello {
  [self send:appFrame(0x00, 0x11, (const uint8_t *)"yOTmK50z", 8)];
}

// Всё, что приложение слало после приветствия (sniff16-app), по порядку
- (void)initSequence {
  uint8_t a[] = {0x80};
  [self send:appFrame(_session, 0xfa, a, 1)];
  for (uint8_t page = 0; page < 4; page++) {
    uint8_t b[] = {0x00, page, 0x51};
    [self send:appFrame(_session, 0xfb, b, 3)];
  }
  uint8_t c[] = {0x38};
  [self send:appFrame(_session, 0x64, c, 1)];
  [self send:appFrame(_session, 0x97, NULL, 0)];
}

- (void)shift:(BOOL)up {
  uint8_t v = up ? 0x02 : 0x01;
  [self send:appFrame(_session, 0x99, &v, 1)];
}

- (void)key:(char)k {
  if (k == 'u') [self shift:YES];
  else if (k == 'd') [self shift:NO];
  else if (k == 'i') [self initSequence];
  else if (k == 'q') {
    if (_rd) [_cm cancelPeripheralConnection:_rd];
    exit(0);
  }
}
@end

int main(void) {
  @autoreleasepool {
    struct termios t;
    tcgetattr(0, &t);
    struct termios raw = t;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &raw);

    App *app = [App new];
    dispatch_source_t in = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(in, ^{
      char k;
      if (read(0, &k, 1) == 1) {
        if (k == 'q') tcsetattr(0, TCSANOW, &t);
        [app key:k];
      }
    });
    dispatch_resume(in);
    [[NSRunLoop mainRunLoop] run];
  }
  return 0;
}
