// wheeltop-shifter: the Mac pretends to be a WHEELTOP EDS shifter (pod)
// and sends button frames to the rear derailleur (RD) over BLE.
// Protocol: ../wheeltop_eds_protocol_spec.md

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#pragma mark - Protocol

static NSString *const kService = @"6e400001-b5a3-f393-e0a9-e50e24dcca9e";
// As in standard NUS, checked on a real TX Right 2026-10-23 (the spec said the opposite).
static NSString *const kRDWrite = @"6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // write, write-nr: written by the RD
static NSString *const kButtons = @"6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // notify: buttons

enum {
    kDownPressed = 0x01, kUpPressed = 0x02,
    kDownFinePressed = 0x05, kUpFinePressed = 0x06,
    kDownReleased = 0x09, kUpReleased = 0x0a,
    kStatus = 0x10,
};

// 0 means OX (3-byte frames), otherwise the TX pod type byte
static uint8_t podType = 0;

static NSData *frame(uint8_t code) {
    if (podType == 0) {
        uint8_t b[3] = {0x04, code, (uint8_t)(0x04 ^ code)};
        return [NSData dataWithBytes:b length:3];
    }
    uint8_t b[4] = {0x04, podType, code, (uint8_t)((0x04 + podType + code) & 0xff)};
    return [NSData dataWithBytes:b length:4];
}

static NSString *hex(NSData *d) {
    NSMutableArray *parts = [NSMutableArray array];
    const uint8_t *p = d.bytes;
    for (NSUInteger i = 0; i < d.length; i++) [parts addObject:[NSString stringWithFormat:@"%02x", p[i]]];
    return [parts componentsJoinedByString:@" "];
}

static FILE *logFile; // in --listen mode the log is also written to a file

static void say(NSString *s) {
    static NSDateFormatter *f;
    if (!f) { f = [NSDateFormatter new]; f.dateFormat = @"HH:mm:ss.SSS"; }
    printf("[%s] %s\r\n", [f stringFromDate:[NSDate date]].UTF8String, s.UTF8String);
    fflush(stdout);
    if (logFile) {
        fprintf(logFile, "[%s] %s\n", [f stringFromDate:[NSDate date]].UTF8String, s.UTF8String);
        fflush(logFile);
    }
}

#pragma mark - BLE peripheral

@interface Pod : NSObject <CBPeripheralManagerDelegate>
@property(nonatomic) CBPeripheralManager *pm;
@property(nonatomic) CBMutableCharacteristic *buttons;
@property(nonatomic) NSMutableArray<NSData *> *pending;
@property(nonatomic, copy) NSString *name;
@property(nonatomic) NSInteger subscribers;
@property(nonatomic) BOOL fineTune;
@property(nonatomic) NSTimer *statusTimer;
@end

@implementation Pod

- (instancetype)initWithName:(NSString *)name {
    if ((self = [super init])) {
        _name = name;
        _pending = [NSMutableArray array];
        _pm = [[CBPeripheralManager alloc] initWithDelegate:self queue:dispatch_get_main_queue()];
    }
    return self;
}

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager *)p {
    switch (p.state) {
    case CBManagerStatePoweredOn: [self setup]; break;
    case CBManagerStateUnauthorized:
        say(@"No Bluetooth access. Allow it for Terminal: System Settings → Privacy & Security → Bluetooth.");
        break;
    case CBManagerStatePoweredOff: say(@"Bluetooth is off."); break;
    case CBManagerStateUnsupported: say(@"This Mac cannot act as a BLE peripheral."); break;
    default: say([NSString stringWithFormat:@"Bluetooth state: %ld", (long)p.state]);
    }
}

- (void)setup {
    self.buttons = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:kButtons]
                                                      properties:CBCharacteristicPropertyNotify
                                                           value:nil
                                                     permissions:CBAttributePermissionsReadable];
    CBMutableCharacteristic *rdWrite =
        [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:kRDWrite]
                                           properties:CBCharacteristicPropertyWrite | CBCharacteristicPropertyWriteWithoutResponse
                                                value:nil
                                          permissions:CBAttributePermissionsWriteable];
    CBMutableService *service = [[CBMutableService alloc] initWithType:[CBUUID UUIDWithString:kService] primary:YES];
    service.characteristics = @[ rdWrite, self.buttons ];
    [self.pm removeAllServices];
    [self.pm addService:service];
}

- (void)peripheralManager:(CBPeripheralManager *)p didAddService:(CBService *)service error:(NSError *)error {
    if (error) { say([NSString stringWithFormat:@"Adding the service failed: %@", error]); return; }
    // macOS only lets us advertise a name and service UUIDs; manufacturer data is not sent.
    [p startAdvertising:@{
        CBAdvertisementDataLocalNameKey : self.name,
        CBAdvertisementDataServiceUUIDsKey : @[ [CBUUID UUIDWithString:kService] ],
    }];
}

- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager *)p error:(NSError *)error {
    if (error) { say([NSString stringWithFormat:@"Advertising did not start: %@", error]); return; }
    say([NSString stringWithFormat:@"Advertising: name \"%@\", %@. Waiting for the RD to connect…", self.name,
                                   podType ? [NSString stringWithFormat:@"TX, type 0x%02x", podType] : @"OX"]);
}

- (void)peripheralManager:(CBPeripheralManager *)p central:(CBCentral *)central didSubscribeToCharacteristic:(CBCharacteristic *)c {
    say([NSString stringWithFormat:@"Central %@ subscribed to %@ (mtu %lu)", central.identifier.UUIDString, c.UUID,
                                   (unsigned long)central.maximumUpdateValueLength]);
    if (![c.UUID isEqual:[CBUUID UUIDWithString:kButtons]]) return;
    self.subscribers++;
    if (podType && !self.statusTimer) {
        // A TX pod sends a status frame once a second; the RD is expected to answer it.
        __weak Pod *weakSelf = self;
        self.statusTimer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *t) {
            [weakSelf send:kStatus quiet:YES];
        }];
    }
}

- (void)peripheralManager:(CBPeripheralManager *)p central:(CBCentral *)central didUnsubscribeFromCharacteristic:(CBCharacteristic *)c {
    say([NSString stringWithFormat:@"Central %@ unsubscribed from %@", central.identifier.UUIDString, c.UUID]);
    if (![c.UUID isEqual:[CBUUID UUIDWithString:kButtons]]) return;
    self.subscribers = MAX(0, self.subscribers - 1);
    if (self.subscribers == 0) { [self.statusTimer invalidate]; self.statusTimer = nil; }
}

- (void)peripheralManager:(CBPeripheralManager *)p didReceiveWriteRequests:(NSArray<CBATTRequest *> *)requests {
    for (CBATTRequest *r in requests)
        say([NSString stringWithFormat:@"WRITE by RD → %@: %@", r.characteristic.UUID, hex(r.value ?: [NSData data])]);
    if (requests.firstObject) [p respondToRequest:requests.firstObject withResult:CBATTErrorSuccess];
}

- (void)peripheralManager:(CBPeripheralManager *)p didReceiveReadRequest:(CBATTRequest *)r {
    say([NSString stringWithFormat:@"READ by RD ← %@", r.characteristic.UUID]);
    r.value = [NSData data];
    [p respondToRequest:r withResult:CBATTErrorSuccess];
}

- (void)peripheralManagerIsReadyToUpdateSubscribers:(CBPeripheralManager *)p {
    while (self.pending.count && [p updateValue:self.pending.firstObject forCharacteristic:self.buttons onSubscribedCentrals:nil])
        [self.pending removeObjectAtIndex:0];
}

- (void)send:(uint8_t)code quiet:(BOOL)quiet {
    NSData *d = frame(code);
    if (!quiet) say([NSString stringWithFormat:@"→ %@%@", hex(d), self.subscribers ? @"" : @"  (RD not connected, frame dropped)"]);
    if (!self.subscribers) return;
    if (self.pending.count || ![self.pm updateValue:d forCharacteristic:self.buttons onSubscribedCentrals:nil])
        [self.pending addObject:d];
}

// One click: press, then release after 120 ms (like a short click on the pod's button).
- (void)click:(BOOL)up {
    uint8_t press = up ? (self.fineTune ? kUpFinePressed : kUpPressed) : (self.fineTune ? kDownFinePressed : kDownPressed);
    uint8_t release = up ? kUpReleased : kDownReleased;
    say([NSString stringWithFormat:@"Shift %@%@", up ? @"UP" : @"DOWN", self.fineTune ? @" (fine-tune)" : @""]);
    [self send:press quiet:NO];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [self send:release quiet:NO];
    });
}

@end

#pragma mark - Listening to a real pod

// The Mac connects to the pod as a central (instead of the RD) and records everything the pod sends.
// The pod only accepts a connection while the RD isn't connected, so the RD must be asleep or off.

static const uint8_t kPrefix[] = {0x07, 0x0f, 0x00, 0x14, 0x55, 0x6a, 0x84};

static NSString *typeName(uint8_t t) {
    switch (t) {
    case 0x37: return @"OX";
    case 0x39: return @"TX Front";
    case 0x36: return @"TX Left";
    case 0x38: return @"TX Right";
    default: return [NSString stringWithFormat:@"unknown 0x%02x", t];
    }
}

static NSString *codeName(uint8_t c) {
    switch (c) {
    case 0x01: return @"lower pressed";
    case 0x02: return @"upper pressed";
    case 0x03: return @"lower held";
    case 0x04: return @"upper held";
    case 0x05: return @"lower pressed (fine-tune)";
    case 0x06: return @"upper pressed (fine-tune)";
    case 0x07: return @"lower held (fine-tune)";
    case 0x08: return @"upper held (fine-tune)";
    case 0x09: return @"lower released";
    case 0x0a: return @"upper released";
    case 0x10: return @"status frame";
    default: return @"UNKNOWN CODE";
    }
}

static NSString *decode(NSData *d) {
    const uint8_t *p = d.bytes;
    if (d.length == 3 && p[0] == 0x04)
        return [NSString stringWithFormat:@"OX, code 0x%02x %@%@", p[1], codeName(p[1]),
                                          p[2] == (0x04 ^ p[1]) ? @"" : @", CHECKSUM MISMATCH"];
    if (d.length == 4 && p[0] == 0x04)
        return [NSString stringWithFormat:@"%@, code 0x%02x %@%@", typeName(p[1]), p[2], codeName(p[2]),
                                          p[3] == ((0x04 + p[1] + p[2]) & 0xff) ? @"" : @", CHECKSUM MISMATCH"];
    return @"format not as in the spec";
}

// Offset of the prefix in manufacturer data, or -1. A real TX Right has company id ff ff before the prefix.
static NSInteger prefixOffset(NSData *mfr) {
    for (NSInteger off = 0; off <= 2; off += 2)
        if ((NSInteger)mfr.length >= off + (NSInteger)sizeof kPrefix && memcmp((const uint8_t *)mfr.bytes + off, kPrefix, sizeof kPrefix) == 0)
            return off;
    return -1;
}

static BOOL isWheeltop(NSData *mfr) { return prefixOffset(mfr) >= 0; }

@interface Listener : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic) CBCentralManager *cm;
@property(nonatomic) CBPeripheral *pod;
@property(nonatomic) NSMutableDictionary<NSUUID *, NSData *> *seen; // last advertisement of each pod
@property(nonatomic) NSDate *connectedAt;
@property(nonatomic) BOOL all; // --scan: show all devices and connect to nothing
@end

@implementation Listener

- (instancetype)init {
    if ((self = [super init])) {
        _seen = [NSMutableDictionary dictionary];
        _cm = [[CBCentralManager alloc] initWithDelegate:self queue:dispatch_get_main_queue()];
    }
    return self;
}

- (void)scan {
    say(@"Looking for a WHEELTOP pod… Press any button on the pod to wake it.");
    [self.cm scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @YES}];
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)cm {
    switch (cm.state) {
    case CBManagerStatePoweredOn: [self scan]; break;
    case CBManagerStateUnauthorized:
        say(@"No Bluetooth access. Allow it for Terminal: System Settings → Privacy & Security → Bluetooth.");
        break;
    case CBManagerStatePoweredOff: say(@"Bluetooth is off."); break;
    default: say([NSString stringWithFormat:@"Bluetooth state: %ld", (long)cm.state]);
    }
}

- (void)centralManager:(CBCentralManager *)cm didDiscoverPeripheral:(CBPeripheral *)p
     advertisementData:(NSDictionary *)ad RSSI:(NSNumber *)rssi {
    NSData *mfr = ad[CBAdvertisementDataManufacturerDataKey];
    if (self.all && !isWheeltop(mfr)) {
        // New device or new advertisement; a noticeable RSSI jump is printed too, to find the pod by proximity.
        NSString *key = [NSString stringWithFormat:@"%@|%@|%@", hex(mfr ?: [NSData data]), ad[CBAdvertisementDataLocalNameKey] ?: @"",
                                                   [ad[CBAdvertisementDataServiceUUIDsKey] componentsJoinedByString:@","] ?: @""];
        NSData *keyData = [key dataUsingEncoding:NSUTF8StringEncoding];
        if ([self.seen[p.identifier] isEqualToData:keyData]) return;
        self.seen[p.identifier] = keyData;
        NSMutableString *s = [NSMutableString stringWithFormat:@"%@ rssi %@", p.identifier.UUIDString, rssi];
        if (ad[CBAdvertisementDataLocalNameKey] || p.name) [s appendFormat:@"  \"%@\"", ad[CBAdvertisementDataLocalNameKey] ?: p.name];
        if (mfr) [s appendFormat:@"\r\n    mfr: %@", hex(mfr)];
        if (ad[CBAdvertisementDataServiceUUIDsKey]) [s appendFormat:@"\r\n    services: %@", [ad[CBAdvertisementDataServiceUUIDsKey] componentsJoinedByString:@", "]];
        if (ad[CBAdvertisementDataServiceDataKey]) [s appendFormat:@"\r\n    service data: %@", ad[CBAdvertisementDataServiceDataKey]];
        say(s);
        return;
    }
    if (!isWheeltop(mfr)) return;
    if ([self.seen[p.identifier] isEqualToData:mfr]) return;
    self.seen[p.identifier] = mfr;

    NSInteger off = prefixOffset(mfr);
    const uint8_t *m = (const uint8_t *)mfr.bytes + off; // m[0] is the start of the prefix 07 0f …
    NSInteger n = mfr.length - off;
    NSMutableString *s = [NSMutableString stringWithFormat:@"POD ADVERTISEMENT %@ rssi %@\r\n    mfr: %@\r\n    type: %@",
                                                           p.identifier.UUIDString, rssi, hex(mfr), n > 7 ? typeName(m[7]) : @"?"];
    if (n > 11)
        [s appendFormat:@", firmware %d.%d, battery %.2f V", m[8], m[9], ((m[10] << 8) | m[11]) / 100.0];
    if (n > 12)
        [s appendFormat:@"\r\n    %ld more bytes after the battery: %@", (long)(n - 12), hex([NSData dataWithBytes:m + 12 length:n - 12])];
    if (ad[CBAdvertisementDataLocalNameKey]) [s appendFormat:@"\r\n    name: %@", ad[CBAdvertisementDataLocalNameKey]];
    if (ad[CBAdvertisementDataServiceUUIDsKey]) [s appendFormat:@"\r\n    services: %@", [ad[CBAdvertisementDataServiceUUIDsKey] componentsJoinedByString:@", "]];
    if (ad[CBAdvertisementDataIsConnectable]) [s appendFormat:@"\r\n    connectable: %@", ad[CBAdvertisementDataIsConnectable]];
    say(s);

    if (!self.pod && !self.all) {
        self.pod = p;
        p.delegate = self;
        say(@"Connecting…");
        [cm connectPeripheral:p options:nil];
    }
}

- (void)centralManager:(CBCentralManager *)cm didConnectPeripheral:(CBPeripheral *)p {
    self.connectedAt = [NSDate date];
    say(@"Connected. Reading services…");
    [cm stopScan];
    [p discoverServices:nil];
}

- (void)centralManager:(CBCentralManager *)cm didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)error {
    say([NSString stringWithFormat:@"Connection failed: %@. The pod may be busy with the RD; turn the RD off.",
                                   error.localizedDescription]);
    self.pod = nil;
}

- (void)centralManager:(CBCentralManager *)cm didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)error {
    say([NSString stringWithFormat:@"Pod disconnected after %.1f s%@", -[self.connectedAt timeIntervalSinceNow],
                                   error ? [@": " stringByAppendingString:error.localizedDescription] : @""]);
    self.pod = nil;
    [self.seen removeAllObjects];
    [self scan];
}

- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)error {
    for (CBService *s in p.services) {
        say([NSString stringWithFormat:@"Service %@", s.UUID.UUIDString]);
        [p discoverCharacteristics:nil forService:s];
    }
}

- (void)peripheral:(CBPeripheral *)p didDiscoverCharacteristicsForService:(CBService *)s error:(NSError *)error {
    for (CBCharacteristic *c in s.characteristics) {
        CBCharacteristicProperties pr = c.properties;
        NSMutableArray *props = [NSMutableArray array];
        if (pr & CBCharacteristicPropertyRead) [props addObject:@"read"];
        if (pr & CBCharacteristicPropertyWrite) [props addObject:@"write"];
        if (pr & CBCharacteristicPropertyWriteWithoutResponse) [props addObject:@"write-nr"];
        if (pr & CBCharacteristicPropertyNotify) [props addObject:@"notify"];
        if (pr & CBCharacteristicPropertyIndicate) [props addObject:@"indicate"];
        say([NSString stringWithFormat:@"  characteristic %@ [%@]", c.UUID.UUIDString, [props componentsJoinedByString:@" "]]);
        if (pr & CBCharacteristicPropertyRead) [p readValueForCharacteristic:c];
        // Like BikeControl: subscribe to everything, since we don't know where the pod sends data.
        if (pr & (CBCharacteristicPropertyNotify | CBCharacteristicPropertyIndicate)) [p setNotifyValue:YES forCharacteristic:c];
    }
}

- (void)peripheral:(CBPeripheral *)p didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    say([NSString stringWithFormat:@"  subscription to %@: %@", c.UUID.UUIDString, error ? error.localizedDescription : @"ok"]);
    if (!error && [c.UUID isEqual:[CBUUID UUIDWithString:kButtons]])
        say(@"READY. Press the pod buttons: short, long, in position R and in position T.");
}

- (void)peripheral:(CBPeripheral *)p didUpdateValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    NSData *v = c.value ?: [NSData data];
    if (!c.isNotifying) {
        NSString *text = [[NSString alloc] initWithData:v encoding:NSUTF8StringEncoding];
        say([NSString stringWithFormat:@"  READ %@: %@%@", c.UUID.UUIDString, hex(v),
                                       text.length ? [NSString stringWithFormat:@"  \"%@\"", text] : @""]);
        return;
    }
    say([NSString stringWithFormat:@"FRAME %@ ← %@   (%@)", c.UUID.UUIDString, hex(v), decode(v)]);
}

@end

#pragma mark - Keyboard

static struct termios original;
static void restoreTerminal(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &original); }
static void rawMode(void) {
    tcgetattr(STDIN_FILENO, &original);
    struct termios raw = original;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    atexit(restoreTerminal);
}

#pragma mark - ESP32 over USB

// In this mode the Mac only passes commands to the esp32/wheeltop_pod board, which does the BLE.
static int serialFd = -1;

static NSString *findPort(void) {
    NSArray *files = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/dev" error:nil];
    for (NSString *f in [files sortedArrayUsingSelector:@selector(compare:)]) {
        if ([f hasPrefix:@"cu.usbserial"] || [f hasPrefix:@"cu.wchusbserial"] || [f hasPrefix:@"cu.SLAB_USBtoUART"] ||
            ([f hasPrefix:@"cu.usbmodem"] && ![f containsString:@"NTBKFX"])) // usbmodem…NTBKFX is a monitor's USB hub, not a board
            return [@"/dev/" stringByAppendingString:f];
    }
    return nil;
}

static BOOL openSerial(NSString *port) {
    serialFd = open(port.UTF8String, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serialFd < 0) return NO;
    struct termios t;
    tcgetattr(serialFd, &t);
    cfmakeraw(&t);
    cfsetspeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | HUPCL); // without hardware flow control, writes to USB CDC hang
    tcsetattr(serialFd, TCSANOW, &t);
    // On an ESP32-C3 with USB-Serial/JTAG a raised RTS holds the chip in reset, and macOS raises DTR and RTS on open.
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(serialFd, TIOCMBIC, &lines);

    // static: otherwise ARC frees the source when the function returns and reading silently stops.
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, serialFd, 0, dispatch_get_main_queue());
    __block NSMutableData *line = [NSMutableData data];
    dispatch_source_set_event_handler(src, ^{
        uint8_t buf[256];
        ssize_t n = read(serialFd, buf, sizeof buf);
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                NSString *s = [[NSString alloc] initWithData:line encoding:NSUTF8StringEncoding];
                say([@"ESP32: " stringByAppendingString:[s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"?"]);
                line = [NSMutableData data];
            } else {
                [line appendBytes:&buf[i] length:1];
            }
        }
        if (n == 0) { say(@"The board disconnected."); exit(1); }
    });
    dispatch_resume(src);
    return YES;
}

static void serialSend(NSString *cmd) {
    NSString *s = [cmd stringByAppendingString:@"\n"];
    write(serialFd, s.UTF8String, strlen(s.UTF8String));
}

#pragma mark - main

static void usage(void) {
    printf("Usage: wheeltop-shifter [--type ox|tx-front|tx-left|tx-right] [--name NAME]\n"
           "       wheeltop-shifter --esp32 [PORT]   (control the ESP32 board over USB)\n"
           "       wheeltop-shifter --listen         (connect to a real pod and record its frames)\n"
           "       wheeltop-shifter --scan           (show all BLE devices nearby)\n");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSString *name = @"WHEELTOP";
        BOOL esp32 = NO;
        NSString *port = nil;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--type") && i + 1 < argc) {
                const char *t = argv[++i];
                if (!strcmp(t, "ox")) podType = 0;
                else if (!strcmp(t, "tx-front")) podType = 0x39;
                else if (!strcmp(t, "tx-left")) podType = 0x36;
                else if (!strcmp(t, "tx-right")) podType = 0x38;
                else { usage(); return 1; }
            } else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
                name = @(argv[++i]);
            } else if (!strcmp(argv[i], "--listen") || !strcmp(argv[i], "--scan")) {
                BOOL all = !strcmp(argv[i], "--scan");
                logFile = fopen(all ? "scan.log" : "capture.log", "a");
                printf("%s Logging to %s/%s. Quit: q\n", all ? "Scanning all BLE devices." : "Listening mode.",
                       [NSFileManager defaultManager].currentDirectoryPath.UTF8String, all ? "scan.log" : "capture.log");
                Listener *listener = [Listener new];
                listener.all = all;
                rawMode();
                dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, STDIN_FILENO, 0, dispatch_get_main_queue());
                dispatch_source_set_event_handler(src, ^{
                    char c;
                    if (read(STDIN_FILENO, &c, 1) == 1 && c == 'q') exit(0);
                });
                dispatch_resume(src);
                [[NSRunLoop mainRunLoop] run];
            } else if (!strcmp(argv[i], "--esp32")) {
                esp32 = YES;
                if (i + 1 < argc && argv[i + 1][0] == '/') port = @(argv[++i]);
            } else {
                usage();
                return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 1 : 0;
            }
        }

        Pod *pod = nil;
        void (^up)(void), (^down)(void), (^fine)(void);
        if (esp32) {
            port = port ?: findPort();
            if (!port) { printf("No ESP32 board found in /dev/cu.*. Connect it or give the port: --esp32 /dev/cu.…\n"); return 1; }
            if (!openSerial(port)) { printf("Cannot open %s: %s\n", port.UTF8String, strerror(errno)); return 1; }
            printf("Board: %s\n", port.UTF8String);
            fflush(stdout);
            up = ^{ serialSend(@"u"); };
            down = ^{ serialSend(@"d"); };
            fine = ^{ serialSend(@"f"); };
        } else {
            pod = [[Pod alloc] initWithName:name];
            up = ^{ [pod click:YES]; };
            down = ^{ [pod click:NO]; };
            fine = ^{
                pod.fineTune = !pod.fineTune;
                say(pod.fineTune ? @"Fine-tune: on (T)" : @"Fine-tune: off (R)");
            };
        }
        printf("WHEELTOP shifter for Mac\n"
               "  ↑ / k / +   shift up (upper button)\n"
               "  ↓ / j / -   shift down (lower button)\n"
               "  t           fine-tune on/off (the R/T slider)\n");
        if (esp32)
            printf("  s / a       scan: WHEELTOP pods / all devices\n"
                   "  i           board settings\n"
                   "  :           type a board command (pair, mode app, clone 1, type tx-right, mac …, raw …, reset)\n");
        printf("  q           quit\n");
        rawMode();
        if (esp32) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ serialSend(@"info"); });

        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, STDIN_FILENO, 0, dispatch_get_main_queue());
        static uint8_t esc[3];
        __block int escLen = 0;
        __block NSMutableString *command = nil; // not nil while a command after ':' is being typed
        dispatch_source_set_event_handler(src, ^{
            uint8_t buf[16];
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            for (ssize_t i = 0; i < n; i++) {
                uint8_t b = buf[i];
                if (command) {
                    if (b == '\n' || b == '\r') {
                        printf("\r\n");
                        serialSend(command);
                        command = nil;
                    } else if (b == 0x7f) {
                        if (command.length) { [command deleteCharactersInRange:NSMakeRange(command.length - 1, 1)]; printf("\b \b"); }
                    } else if (b == 0x1b) {
                        printf(" (cancelled)\r\n");
                        command = nil;
                    } else {
                        [command appendFormat:@"%c", b];
                        putchar(b);
                    }
                    fflush(stdout);
                    continue;
                }
                // Arrows arrive as ESC [ A / ESC [ B
                if (escLen || b == 0x1b) {
                    esc[escLen++] = b;
                    if (escLen == 3) {
                        if (esc[1] == '[' && esc[2] == 'A') up();
                        if (esc[1] == '[' && esc[2] == 'B') down();
                        escLen = 0;
                    }
                    continue;
                }
                switch (b) {
                case 'k': case '+': case '=': up(); break;
                case 'j': case '-': case '_': down(); break;
                case 't': fine(); break;
                case 's': if (esp32) serialSend(@"s"); break;
                case 'a': if (esp32) serialSend(@"a"); break;
                case 'i': if (esp32) serialSend(@"info"); break;
                case ':':
                    if (esp32) { command = [NSMutableString string]; printf(": "); fflush(stdout); }
                    break;
                case 'q': exit(0);
                }
            }
        });
        dispatch_resume(src);
        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}
