// Две кнопки шифтера на экране Мака: шлют кадры через плату ESP32 (команда raw) так же, как настоящий шифтер.
// Нажатие — код press, через 540 мс удержания — код hold, отпускание — код release.
// Клавиши ↑ и ↓ работают так же, их можно зажать обе сразу. Галочка «T» — режим fine-tune.
//
//   ./board-buttons [/dev/cu.usbmodemXXX]
// Сборка: clang -O2 -fobjc-arc -framework Cocoa board_buttons.m -o board-buttons

#import <Cocoa/Cocoa.h>
#include <fcntl.h>
#include <glob.h>
#include <termios.h>
#include <unistd.h>

static const NSTimeInterval HOLD_AFTER = 0.54; // как у настоящего шифтера

static int openPort(const char *path) {
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return -1;
  struct termios t;
  tcgetattr(fd, &t);
  cfmakeraw(&t);
  t.c_cflag |= CLOCAL | CREAD;
  tcsetattr(fd, TCSANOW, &t);
  return fd;
}

// Ищет плату: шлёт info и ждёт строку «Тип пода»
static NSString *findBoard(int *outFd, NSString **outInfo) {
  glob_t g;
  if (glob("/dev/cu.usbmodem*", 0, NULL, &g) != 0) return nil;
  for (size_t i = 0; i < g.gl_pathc; i++) {
    int fd = openPort(g.gl_pathv[i]);
    if (fd < 0) continue;
    write(fd, "info\n", 5);
    NSMutableString *got = [NSMutableString string];
    for (int n = 0; n < 15; n++) {
      usleep(100000);
      char buf[1024];
      ssize_t r = read(fd, buf, sizeof buf);
      if (r > 0) [got appendString:[[NSString alloc] initWithBytes:buf length:r encoding:NSUTF8StringEncoding] ?: @""];
      if ([got containsString:@"Тип пода"]) break;
    }
    if ([got containsString:@"Тип пода"]) {
      NSString *p = @(g.gl_pathv[i]);
      globfree(&g);
      *outFd = fd;
      *outInfo = got;
      return p;
    }
    close(fd);
  }
  globfree(&g);
  return nil;
}

@class AppDelegate;

// Большая кнопка: события нажатия и отпускания мыши приходят отдельно, в отличие от NSButton
@interface PadView : NSView
@property (copy) NSString *title;
@property BOOL down;
@property (copy) void (^onChange)(BOOL down);
@end

@implementation PadView
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (void)mouseDown:(NSEvent *)e { if (self.onChange) self.onChange(YES); }
- (void)mouseUp:(NSEvent *)e { if (self.onChange) self.onChange(NO); }
- (void)drawRect:(NSRect)r {
  NSRect b = NSInsetRect(self.bounds, 4, 4);
  NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:b xRadius:14 yRadius:14];
  [(self.down ? [NSColor systemBlueColor] : [NSColor controlBackgroundColor]) setFill];
  [p fill];
  [[NSColor separatorColor] setStroke];
  p.lineWidth = 2;
  [p stroke];
  NSDictionary *a = @{NSFontAttributeName: [NSFont systemFontOfSize:30 weight:NSFontWeightSemibold],
                      NSForegroundColorAttributeName: self.down ? NSColor.whiteColor : NSColor.labelColor};
  NSSize s = [self.title sizeWithAttributes:a];
  [self.title drawAtPoint:NSMakePoint(NSMidX(b) - s.width / 2, NSMidY(b) - s.height / 2) withAttributes:a];
}
@end

// Окно, которое ловит ↑ и ↓
@interface KeyView : NSView
@property (copy) void (^onKey)(BOOL up, BOOL down);
@end

@implementation KeyView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e {
  if (e.isARepeat) return;
  if (e.keyCode == 126 && self.onKey) self.onKey(YES, YES);
  else if (e.keyCode == 125 && self.onKey) self.onKey(NO, YES);
  else [super keyDown:e];
}
- (void)keyUp:(NSEvent *)e {
  if (e.keyCode == 126 && self.onKey) self.onKey(YES, NO);
  else if (e.keyCode == 125 && self.onKey) self.onKey(NO, NO);
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate {
  NSWindow *_win;
  PadView *_up, *_down;
  NSButton *_fine;
  NSTextField *_status;
  NSTextView *_log;
  int _fd;
  NSString *_port;
  uint8_t _type;           // 0x38 TX Right и т. п.; 0x37 — OX (3-байтовые кадры)
  NSTimer *_holdTimer[2];  // 0 — вниз, 1 — вверх
  NSMutableString *_rx;
  dispatch_source_t _src;
  BOOL _rdConnected;
}

- (instancetype)initWithPort:(NSString *)port fd:(int)fd info:(NSString *)info {
  if ((self = [super init])) {
    _port = port;
    _fd = fd;
    _type = 0x38;
    _rx = [NSMutableString string];
    [self parseLine:info];
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
  _win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 520)
                                     styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskMiniaturizable
                                       backing:NSBackingStoreBuffered
                                         defer:NO];
  _win.title = @"Кнопки шифтера через плату";
  KeyView *root = [[KeyView alloc] initWithFrame:_win.contentView.bounds];
  _win.contentView = root;
  __weak AppDelegate *weakSelf = self;
  root.onKey = ^(BOOL up, BOOL down) { [weakSelf button:up down:down]; };

  _up = [[PadView alloc] initWithFrame:NSMakeRect(20, 390, 420, 110)];
  _up.title = @"▲  Вверх  (↑)";
  _up.onChange = ^(BOOL d) { [weakSelf button:YES down:d]; };
  _down = [[PadView alloc] initWithFrame:NSMakeRect(20, 270, 420, 110)];
  _down.title = @"▼  Вниз  (↓)";
  _down.onChange = ^(BOOL d) { [weakSelf button:NO down:d]; };
  [root addSubview:_up];
  [root addSubview:_down];

  _fine = [NSButton checkboxWithTitle:@"Положение T (fine-tune)" target:nil action:nil];
  _fine.frame = NSMakeRect(24, 236, 300, 24);
  [root addSubview:_fine];

  _status = [NSTextField labelWithString:@""];
  _status.frame = NSMakeRect(24, 210, 420, 22);
  [root addSubview:_status];

  NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 16, 420, 188)];
  sv.hasVerticalScroller = YES;
  sv.borderType = NSBezelBorder;
  _log = [[NSTextView alloc] initWithFrame:sv.contentView.bounds];
  _log.editable = NO;
  _log.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
  _log.autoresizingMask = NSViewWidthSizable;
  sv.documentView = _log;
  [root addSubview:sv];

  [self updateStatus];
  [_win center];
  [_win makeKeyAndOrderFront:nil];
  [_win makeFirstResponder:root];
  [NSApp activateIgnoringOtherApps:YES];

  _src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, _fd, 0, dispatch_get_main_queue());
  dispatch_source_set_event_handler(_src, ^{ [weakSelf readPort]; });
  dispatch_resume(_src);
  [self appendLog:[NSString stringWithFormat:@"Плата на %@", _port]];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }

- (void)updateStatus {
  _status.stringValue = [NSString stringWithFormat:@"Плата: %@   Тип: 0x%02x   Переключатель: %@", _port.lastPathComponent,
                                                   _type, _rdConnected ? @"подключён" : @"не подключён"];
  _status.textColor = _rdConnected ? NSColor.systemGreenColor : NSColor.secondaryLabelColor;
}

- (void)appendLog:(NSString *)s {
  NSDateFormatter *f = [NSDateFormatter new];
  f.dateFormat = @"HH:mm:ss";
  NSString *line = [NSString stringWithFormat:@"%@  %@\n", [f stringFromDate:NSDate.date], s];
  [_log.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:line
      attributes:@{NSFontAttributeName: _log.font, NSForegroundColorAttributeName: NSColor.labelColor}]];
  [_log scrollToEndOfDocument:nil];
}

- (void)parseLine:(NSString *)l {
  NSRange r = [l rangeOfString:@"Тип пода: 0x"];
  if (r.location != NSNotFound && l.length >= NSMaxRange(r) + 2)
    _type = (uint8_t)strtoul([[l substringWithRange:NSMakeRange(NSMaxRange(r), 2)] UTF8String], NULL, 16);
  if ([l containsString:@"RD подключён: да"] || [l containsString:@"Подключился central"] || [l containsString:@"MTU "])
    _rdConnected = YES;
  if ([l containsString:@"RD подключён: нет"] || [l containsString:@"Отключился"]) _rdConnected = NO;
}

- (void)readPort {
  char buf[2048];
  ssize_t n = read(_fd, buf, sizeof buf);
  if (n <= 0) {
    if (n == 0) {
      [self appendLog:@"Плата отключилась"];
      dispatch_source_cancel(_src);
    }
    return;
  }
  [_rx appendString:[[NSString alloc] initWithBytes:buf length:n encoding:NSUTF8StringEncoding] ?: @""];
  NSRange nl;
  while ((nl = [_rx rangeOfString:@"\n"]).location != NSNotFound) {
    NSString *line = [[_rx substringToIndex:nl.location]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    [_rx deleteCharactersInRange:NSMakeRange(0, nl.location + 1)];
    if (!line.length) continue;
    [self parseLine:line];
    [self appendLog:line];
  }
  [self updateStatus];
}

- (void)sendCode:(uint8_t)code {
  NSString *cmd;
  if (_type == 0x37)
    cmd = [NSString stringWithFormat:@"raw 04 %02x %02x\n", code, 0x04 ^ code];
  else
    cmd = [NSString stringWithFormat:@"raw 04 %02x %02x %02x\n", _type, code, (0x04 + _type + code) & 0xff];
  write(_fd, cmd.UTF8String, strlen(cmd.UTF8String));
}

// Коды как в прошивке платы: press 02/01 (fine 06/05), hold 04/03 (fine 08/07), release 0a/09
- (void)button:(BOOL)up down:(BOOL)down {
  PadView *v = up ? _up : _down;
  if (v.down == down) return;
  v.down = down;
  [v setNeedsDisplay:YES];
  int i = up ? 1 : 0;
  BOOL fine = _fine.state == NSControlStateValueOn;
  [_holdTimer[i] invalidate];
  _holdTimer[i] = nil;
  if (down) {
    [self sendCode:up ? (fine ? 0x06 : 0x02) : (fine ? 0x05 : 0x01)];
    uint8_t hold = up ? (fine ? 0x08 : 0x04) : (fine ? 0x07 : 0x03);
    __weak AppDelegate *weakSelf = self;
    _holdTimer[i] = [NSTimer scheduledTimerWithTimeInterval:HOLD_AFTER repeats:NO block:^(NSTimer *t) {
      [weakSelf sendCode:hold];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_holdTimer[i] forMode:NSRunLoopCommonModes];
  } else {
    [self sendCode:up ? 0x0a : 0x09];
  }
}
@end

int main(int argc, const char **argv) {
  @autoreleasepool {
    int fd = -1;
    NSString *info = @"";
    NSString *port;
    if (argc > 1) {
      port = @(argv[1]);
      fd = openPort(argv[1]);
      if (fd < 0) { fprintf(stderr, "Не открыть %s\n", argv[1]); return 1; }
      write(fd, "info\n", 5);
    } else {
      port = findBoard(&fd, &info);
      if (!port) { fprintf(stderr, "Плата не найдена. Подключите её к Маку.\n"); return 1; }
    }
    NSApplication *app = NSApplication.sharedApplication;
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    AppDelegate *d = [[AppDelegate alloc] initWithPort:port fd:fd info:info];
    app.delegate = d;
    [app run];
  }
  return 0;
}
