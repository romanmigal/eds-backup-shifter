// rd-app: the Mac connects to the rear derailleur (RD) the way the Wheeltop app does and shifts gears
// with the app's commands. The shifter stays paired with the RD. Protocol: section 19 of the spec.
//
// Frame: fe <K> <bytes…> <CRC-16/MODBUS over the whole frame, high byte first>.
// The bytes after K are XORed with the key (K − 0x32). Like the app, we send K = 0x32, i.e. unscrambled.
// Plain form: <session> <command> <length> <data…>
//   fe 32 00 11 08 "yOTmK50z"  hello; the RD replies <session> 11 01 <session>
//   fe 32 <session> 99 01 02   shift up, 01 is down
//
// Connects to the first RD whose name starts with "EDS OX".
// Keys: u up, d down, i repeat everything the app sends after connecting, q quit.
// Build: ./build.sh

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <termios.h>
#include <unistd.h>

static NSString *const kService = @"6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static NSString *const kWrite = @"6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static NSString *const kNotify = @"6e400003-b5a3-f393-e0a9-e50e24dcca9e";

static uint16_t crc16modbus(const uint8_t *p, size_t n) {
  uint16_t c = 0xffff;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xa001 : c >> 1;
  }
  return c;
}

static NSData *appFrame(uint8_t session, uint8_t cmd, const uint8_t *data, uint8_t len) {
  NSMutableData *d = [NSMutableData data];
  uint8_t head[5] = {0xfe, 0x32, session, cmd, len};
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
  uint8_t _session; // session number from the RD's hello reply
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
  if (c.state != CBManagerStatePoweredOn) { [self log:@"Bluetooth is off or access was denied"]; return; }
  [self log:@"Looking for the EDS derailleur… move the bike to wake it"];
  [c scanForPeripheralsWithServices:nil options:nil];
}

- (void)centralManager:(CBCentralManager *)c didDiscoverPeripheral:(CBPeripheral *)p
     advertisementData:(NSDictionary *)ad RSSI:(NSNumber *)rssi {
  NSString *name = ad[CBAdvertisementDataLocalNameKey] ?: p.name;
  if (![name hasPrefix:@"EDS OX"]) return;
  [self log:[NSString stringWithFormat:@"Found %@ (%@ dBm), connecting", name, rssi]];
  [c stopScan];
  _rd = p;
  p.delegate = self;
  [c connectPeripheral:p options:nil];
}

- (void)centralManager:(CBCentralManager *)c didConnectPeripheral:(CBPeripheral *)p {
  [self log:@"Connected, looking for the service"];
  [p discoverServices:@[[CBUUID UUIDWithString:kService]]];
}

- (void)centralManager:(CBCentralManager *)c didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)e {
  [self log:[NSString stringWithFormat:@"Disconnected: %@. Searching again", e.localizedDescription ?: @"-"]];
  _w = nil;
  [c scanForPeripheralsWithServices:nil options:nil];
}

- (void)centralManager:(CBCentralManager *)c didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)e {
  [self log:[NSString stringWithFormat:@"Connection failed: %@", e.localizedDescription]];
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
  if (e) { [self log:[NSString stringWithFormat:@"Subscription failed: %@", e.localizedDescription]]; return; }
  [self log:@"Subscribed. Sending the app's hello. Keys: u up, d down, i init sequence, q quit"];
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
      [self log:[NSString stringWithFormat:@"<- %@   hello reply, session %02x", hex(v), _session]];
      return;
    }
    [self log:[NSString stringWithFormat:@"<- %@   plain: %@", hex(v), hex(plain)]];
    return;
  }
  [self log:[NSString stringWithFormat:@"<- %@   %@", hex(v), ascii(v)]];
}

- (void)send:(NSData *)d {
  if (!_w) { [self log:@"Not connected yet"]; return; }
  [self log:[NSString stringWithFormat:@"-> %@", hex(d)]];
  [_rd writeValue:d forCharacteristic:_w type:CBCharacteristicWriteWithoutResponse];
}

- (void)hello {
  [self send:appFrame(0x00, 0x11, (const uint8_t *)"yOTmK50z", 8)];
}

// Everything the app sent after the hello, in order
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
