// wheeltop-shifter: Mac притворяется шифтером (подом) WHEELTOP EDS
// и шлёт заднему переключателю (RD) кадры кнопок по BLE.
// Протокол: ../wheeltop_eds_protocol_spec.md

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#pragma mark - Протокол

static NSString *const kService = @"6e400001-b5a3-f393-e0a9-e50e24dcca9e";
// Как в стандартном NUS, проверено на настоящем TX Right 2026-10-23 (спецификация говорила наоборот).
static NSString *const kRDWrite = @"6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // write, write-nr: запись от RD
static NSString *const kButtons = @"6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // notify: кнопки

enum {
    kDownPressed = 0x01, kUpPressed = 0x02,
    kDownFinePressed = 0x05, kUpFinePressed = 0x06,
    kDownReleased = 0x09, kUpReleased = 0x0a,
    kStatus = 0x10,
};

// 0 означает OX (3-байтовые кадры), иначе байт типа TX-пода
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

static FILE *logFile; // в режиме --listen лог дублируется в файл

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

#pragma mark - BLE-периферия

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
        say(@"Нет доступа к Bluetooth. Разреши его для Терминала: Системные настройки → Конфиденциальность → Bluetooth.");
        break;
    case CBManagerStatePoweredOff: say(@"Bluetooth выключен."); break;
    case CBManagerStateUnsupported: say(@"Этот Мак не умеет быть BLE-периферией."); break;
    default: say([NSString stringWithFormat:@"Состояние Bluetooth: %ld", (long)p.state]);
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
    if (error) { say([NSString stringWithFormat:@"Ошибка добавления сервиса: %@", error]); return; }
    // macOS разрешает рекламировать только имя и UUID сервисов, manufacturer data не уходит.
    [p startAdvertising:@{
        CBAdvertisementDataLocalNameKey : self.name,
        CBAdvertisementDataServiceUUIDsKey : @[ [CBUUID UUIDWithString:kService] ],
    }];
}

- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager *)p error:(NSError *)error {
    if (error) { say([NSString stringWithFormat:@"Реклама не запустилась: %@", error]); return; }
    say([NSString stringWithFormat:@"Реклама идёт: имя \"%@\", %@. Жду подключения RD…", self.name,
                                   podType ? [NSString stringWithFormat:@"TX, тип 0x%02x", podType] : @"OX"]);
}

- (void)peripheralManager:(CBPeripheralManager *)p central:(CBCentral *)central didSubscribeToCharacteristic:(CBCharacteristic *)c {
    say([NSString stringWithFormat:@"Central %@ подписался на %@ (mtu %lu)", central.identifier.UUIDString, c.UUID,
                                   (unsigned long)central.maximumUpdateValueLength]);
    if (![c.UUID isEqual:[CBUUID UUIDWithString:kButtons]]) return;
    self.subscribers++;
    if (podType && !self.statusTimer) {
        // TX-под шлёт статус-кадр раз в секунду, RD должен на него отвечать.
        __weak Pod *weakSelf = self;
        self.statusTimer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *t) {
            [weakSelf send:kStatus quiet:YES];
        }];
    }
}

- (void)peripheralManager:(CBPeripheralManager *)p central:(CBCentral *)central didUnsubscribeFromCharacteristic:(CBCharacteristic *)c {
    say([NSString stringWithFormat:@"Central %@ отписался от %@", central.identifier.UUIDString, c.UUID]);
    if (![c.UUID isEqual:[CBUUID UUIDWithString:kButtons]]) return;
    self.subscribers = MAX(0, self.subscribers - 1);
    if (self.subscribers == 0) { [self.statusTimer invalidate]; self.statusTimer = nil; }
}

- (void)peripheralManager:(CBPeripheralManager *)p didReceiveWriteRequests:(NSArray<CBATTRequest *> *)requests {
    for (CBATTRequest *r in requests)
        say([NSString stringWithFormat:@"ЗАПИСЬ от RD → %@: %@", r.characteristic.UUID, hex(r.value ?: [NSData data])]);
    if (requests.firstObject) [p respondToRequest:requests.firstObject withResult:CBATTErrorSuccess];
}

- (void)peripheralManager:(CBPeripheralManager *)p didReceiveReadRequest:(CBATTRequest *)r {
    say([NSString stringWithFormat:@"ЧТЕНИЕ от RD ← %@", r.characteristic.UUID]);
    r.value = [NSData data];
    [p respondToRequest:r withResult:CBATTErrorSuccess];
}

- (void)peripheralManagerIsReadyToUpdateSubscribers:(CBPeripheralManager *)p {
    while (self.pending.count && [p updateValue:self.pending.firstObject forCharacteristic:self.buttons onSubscribedCentrals:nil])
        [self.pending removeObjectAtIndex:0];
}

- (void)send:(uint8_t)code quiet:(BOOL)quiet {
    NSData *d = frame(code);
    if (!quiet) say([NSString stringWithFormat:@"→ %@%@", hex(d), self.subscribers ? @"" : @"  (RD не подключён, кадр в никуда)"]);
    if (!self.subscribers) return;
    if (self.pending.count || ![self.pm updateValue:d forCharacteristic:self.buttons onSubscribedCentrals:nil])
        [self.pending addObject:d];
}

// Одно нажатие: press, через 120 мс release (как короткий клик по кнопке пода).
- (void)click:(BOOL)up {
    uint8_t press = up ? (self.fineTune ? kUpFinePressed : kUpPressed) : (self.fineTune ? kDownFinePressed : kDownPressed);
    uint8_t release = up ? kUpReleased : kDownReleased;
    say([NSString stringWithFormat:@"Передача %@%@", up ? @"ВВЕРХ" : @"ВНИЗ", self.fineTune ? @" (fine-tune)" : @""]);
    [self send:press quiet:NO];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [self send:release quiet:NO];
    });
}

@end

#pragma mark - Подслушивание настоящего пода

// Мак подключается к поду как central (вместо RD) и записывает всё, что под шлёт.
// Под принимает подключение, только пока RD его не занял, поэтому RD должен спать или быть выключен.

static const uint8_t kPrefix[] = {0x07, 0x0f, 0x00, 0x14, 0x55, 0x6a, 0x84};

static NSString *typeName(uint8_t t) {
    switch (t) {
    case 0x37: return @"OX";
    case 0x39: return @"TX Front";
    case 0x36: return @"TX Left";
    case 0x38: return @"TX Right";
    default: return [NSString stringWithFormat:@"неизвестный 0x%02x", t];
    }
}

static NSString *codeName(uint8_t c) {
    switch (c) {
    case 0x01: return @"нижняя нажата";
    case 0x02: return @"верхняя нажата";
    case 0x03: return @"нижняя удерживается";
    case 0x04: return @"верхняя удерживается";
    case 0x05: return @"нижняя нажата (fine-tune)";
    case 0x06: return @"верхняя нажата (fine-tune)";
    case 0x07: return @"нижняя удерживается (fine-tune)";
    case 0x08: return @"верхняя удерживается (fine-tune)";
    case 0x09: return @"нижняя отпущена";
    case 0x0a: return @"верхняя отпущена";
    case 0x10: return @"статус-кадр";
    default: return @"НЕИЗВЕСТНЫЙ КОД";
    }
}

static NSString *decode(NSData *d) {
    const uint8_t *p = d.bytes;
    if (d.length == 3 && p[0] == 0x04)
        return [NSString stringWithFormat:@"OX, код 0x%02x %@%@", p[1], codeName(p[1]),
                                          p[2] == (0x04 ^ p[1]) ? @"" : @", КОНТРОЛЬНАЯ СУММА НЕ СОШЛАСЬ"];
    if (d.length == 4 && p[0] == 0x04)
        return [NSString stringWithFormat:@"%@, код 0x%02x %@%@", typeName(p[1]), p[2], codeName(p[2]),
                                          p[3] == ((0x04 + p[1] + p[2]) & 0xff) ? @"" : @", КОНТРОЛЬНАЯ СУММА НЕ СОШЛАСЬ"];
    return @"формат не по спецификации";
}

// Смещение префикса в manufacturer data или -1. У настоящего TX Right перед префиксом стоит company id ff ff.
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
@property(nonatomic) NSMutableDictionary<NSUUID *, NSData *> *seen; // последняя реклама каждого пода
@property(nonatomic) NSDate *connectedAt;
@property(nonatomic) BOOL all; // --scan: показывать все устройства и ни к чему не подключаться
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
    say(@"Ищу под WHEELTOP… Нажми любую кнопку на поде, чтобы он проснулся.");
    [self.cm scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @YES}];
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)cm {
    switch (cm.state) {
    case CBManagerStatePoweredOn: [self scan]; break;
    case CBManagerStateUnauthorized:
        say(@"Нет доступа к Bluetooth. Разреши его для Терминала: Системные настройки → Конфиденциальность → Bluetooth.");
        break;
    case CBManagerStatePoweredOff: say(@"Bluetooth выключен."); break;
    default: say([NSString stringWithFormat:@"Состояние Bluetooth: %ld", (long)cm.state]);
    }
}

- (void)centralManager:(CBCentralManager *)cm didDiscoverPeripheral:(CBPeripheral *)p
     advertisementData:(NSDictionary *)ad RSSI:(NSNumber *)rssi {
    NSData *mfr = ad[CBAdvertisementDataManufacturerDataKey];
    if (self.all && !isWheeltop(mfr)) {
        // Новое устройство или новая реклама; заметный скачок RSSI тоже печатаем, чтобы найти под по близости.
        NSString *key = [NSString stringWithFormat:@"%@|%@|%@", hex(mfr ?: [NSData data]), ad[CBAdvertisementDataLocalNameKey] ?: @"",
                                                   [ad[CBAdvertisementDataServiceUUIDsKey] componentsJoinedByString:@","] ?: @""];
        NSData *keyData = [key dataUsingEncoding:NSUTF8StringEncoding];
        if ([self.seen[p.identifier] isEqualToData:keyData]) return;
        self.seen[p.identifier] = keyData;
        NSMutableString *s = [NSMutableString stringWithFormat:@"%@ rssi %@", p.identifier.UUIDString, rssi];
        if (ad[CBAdvertisementDataLocalNameKey] || p.name) [s appendFormat:@"  \"%@\"", ad[CBAdvertisementDataLocalNameKey] ?: p.name];
        if (mfr) [s appendFormat:@"\r\n    mfr: %@", hex(mfr)];
        if (ad[CBAdvertisementDataServiceUUIDsKey]) [s appendFormat:@"\r\n    сервисы: %@", [ad[CBAdvertisementDataServiceUUIDsKey] componentsJoinedByString:@", "]];
        if (ad[CBAdvertisementDataServiceDataKey]) [s appendFormat:@"\r\n    service data: %@", ad[CBAdvertisementDataServiceDataKey]];
        say(s);
        return;
    }
    if (!isWheeltop(mfr)) return;
    if ([self.seen[p.identifier] isEqualToData:mfr]) return;
    self.seen[p.identifier] = mfr;

    NSInteger off = prefixOffset(mfr);
    const uint8_t *m = (const uint8_t *)mfr.bytes + off; // m[0] это начало префикса 07 0f …
    NSInteger n = mfr.length - off;
    NSMutableString *s = [NSMutableString stringWithFormat:@"РЕКЛАМА пода %@ rssi %@\r\n    mfr: %@\r\n    тип: %@",
                                                           p.identifier.UUIDString, rssi, hex(mfr), n > 7 ? typeName(m[7]) : @"?"];
    if (n > 11)
        [s appendFormat:@", прошивка %d.%d, батарея %.2f В", m[8], m[9], ((m[10] << 8) | m[11]) / 100.0];
    if (n > 12)
        [s appendFormat:@"\r\n    ещё %ld байт после батареи: %@", (long)(n - 12), hex([NSData dataWithBytes:m + 12 length:n - 12])];
    if (ad[CBAdvertisementDataLocalNameKey]) [s appendFormat:@"\r\n    имя: %@", ad[CBAdvertisementDataLocalNameKey]];
    if (ad[CBAdvertisementDataServiceUUIDsKey]) [s appendFormat:@"\r\n    сервисы: %@", [ad[CBAdvertisementDataServiceUUIDsKey] componentsJoinedByString:@", "]];
    if (ad[CBAdvertisementDataIsConnectable]) [s appendFormat:@"\r\n    connectable: %@", ad[CBAdvertisementDataIsConnectable]];
    say(s);

    if (!self.pod && !self.all) {
        self.pod = p;
        p.delegate = self;
        say(@"Подключаюсь…");
        [cm connectPeripheral:p options:nil];
    }
}

- (void)centralManager:(CBCentralManager *)cm didConnectPeripheral:(CBPeripheral *)p {
    self.connectedAt = [NSDate date];
    say(@"Подключился. Читаю сервисы…");
    [cm stopScan];
    [p discoverServices:nil];
}

- (void)centralManager:(CBCentralManager *)cm didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)error {
    say([NSString stringWithFormat:@"Не подключился: %@. Возможно, под занят переключателем, выключи RD.",
                                   error.localizedDescription]);
    self.pod = nil;
}

- (void)centralManager:(CBCentralManager *)cm didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)error {
    say([NSString stringWithFormat:@"Под отключился через %.1f с%@", -[self.connectedAt timeIntervalSinceNow],
                                   error ? [@": " stringByAppendingString:error.localizedDescription] : @""]);
    self.pod = nil;
    [self.seen removeAllObjects];
    [self scan];
}

- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)error {
    for (CBService *s in p.services) {
        say([NSString stringWithFormat:@"Сервис %@", s.UUID.UUIDString]);
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
        say([NSString stringWithFormat:@"  характеристика %@ [%@]", c.UUID.UUIDString, [props componentsJoinedByString:@" "]]);
        if (pr & CBCharacteristicPropertyRead) [p readValueForCharacteristic:c];
        // Как BikeControl: подписываемся на всё, потому что неизвестно, где под шлёт данные.
        if (pr & (CBCharacteristicPropertyNotify | CBCharacteristicPropertyIndicate)) [p setNotifyValue:YES forCharacteristic:c];
    }
}

- (void)peripheral:(CBPeripheral *)p didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    say([NSString stringWithFormat:@"  подписка на %@: %@", c.UUID.UUIDString, error ? error.localizedDescription : @"ок"]);
    if (!error && [c.UUID isEqual:[CBUUID UUIDWithString:kButtons]])
        say(@"ГОТОВО. Нажимай кнопки на поде: коротко, долго, в положении R и в положении T.");
}

- (void)peripheral:(CBPeripheral *)p didUpdateValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    NSData *v = c.value ?: [NSData data];
    if (!c.isNotifying) {
        NSString *text = [[NSString alloc] initWithData:v encoding:NSUTF8StringEncoding];
        say([NSString stringWithFormat:@"  ЧТЕНИЕ %@: %@%@", c.UUID.UUIDString, hex(v),
                                       text.length ? [NSString stringWithFormat:@"  \"%@\"", text] : @""]);
        return;
    }
    say([NSString stringWithFormat:@"КАДР %@ ← %@   (%@)", c.UUID.UUIDString, hex(v), decode(v)]);
}

@end

#pragma mark - Клавиатура

static struct termios original;
static void restoreTerminal(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &original); }
static void rawMode(void) {
    tcgetattr(STDIN_FILENO, &original);
    struct termios raw = original;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    atexit(restoreTerminal);
}

#pragma mark - ESP32 по USB

// В этом режиме Мак только передаёт команды плате esp32/wheeltop_pod, а BLE делает она.
static int serialFd = -1;

static NSString *findPort(void) {
    NSArray *files = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/dev" error:nil];
    for (NSString *f in [files sortedArrayUsingSelector:@selector(compare:)]) {
        if ([f hasPrefix:@"cu.usbserial"] || [f hasPrefix:@"cu.wchusbserial"] || [f hasPrefix:@"cu.SLAB_USBtoUART"] ||
            ([f hasPrefix:@"cu.usbmodem"] && ![f containsString:@"NTBKFX"])) // usbmodem…NTBKFX у этого Мака это монитор LG
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
    t.c_cflag &= ~(CRTSCTS | HUPCL); // без аппаратного контроля потока запись в USB CDC зависает
    tcsetattr(serialFd, TCSANOW, &t);
    // На ESP32-C3 с USB-Serial/JTAG поднятый RTS держит чип в сбросе, а macOS поднимает DTR и RTS при открытии.
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(serialFd, TIOCMBIC, &lines);

    // static: иначе ARC освободит источник при выходе из функции, и чтение молча прекратится.
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
        if (n == 0) { say(@"Плата отключилась."); exit(1); }
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
    printf("Использование: wheeltop-shifter [--type ox|tx-front|tx-left|tx-right] [--name ИМЯ]\n"
           "               wheeltop-shifter --esp32 [ПОРТ]   (управлять платой ESP32 по USB)\n"
           "               wheeltop-shifter --listen         (подключиться к настоящему поду и записать его кадры)\n"
           "               wheeltop-shifter --scan           (показать все BLE-устройства рядом)\n");
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
                printf("%s Лог пишется в %s/%s. Выход: q\n", all ? "Скан всех BLE-устройств." : "Режим подслушивания.",
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
            if (!port) { printf("Плата ESP32 не найдена в /dev/cu.*. Подключи её или укажи порт: --esp32 /dev/cu.…\n"); return 1; }
            if (!openSerial(port)) { printf("Не открыть %s: %s\n", port.UTF8String, strerror(errno)); return 1; }
            printf("Плата: %s\n", port.UTF8String);
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
                say(pod.fineTune ? @"Fine-tune: вкл (T)" : @"Fine-tune: выкл (R)");
            };
        }
        printf("WHEELTOP shifter для Mac\n"
               "  ↑ / k / +   передача вверх (верхняя кнопка)\n"
               "  ↓ / j / -   передача вниз (нижняя кнопка)\n"
               "  t           режим fine-tune вкл/выкл (ползунок R/T)\n");
        if (esp32)
            printf("  s / a       скан: поды WHEELTOP / все устройства\n"
                   "  i           настройки платы\n"
                   "  :           ввести команду платы (clone 1, type tx-right, mac …, raw …, reset)\n");
        printf("  q           выход\n");
        rawMode();
        if (esp32) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ serialSend(@"info"); });

        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, STDIN_FILENO, 0, dispatch_get_main_queue());
        static uint8_t esc[3];
        __block int escLen = 0;
        __block NSMutableString *command = nil; // не nil, пока вводится команда после ':'
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
                        printf(" (отмена)\r\n");
                        command = nil;
                    } else {
                        [command appendFormat:@"%c", b];
                        putchar(b);
                    }
                    fflush(stdout);
                    continue;
                }
                // Стрелки приходят как ESC [ A / ESC [ B
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
