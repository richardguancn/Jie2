// 「J2」测试用色块窗口（macOS）
//
// 用途：为截图验证提供一个**已知颜色的靶子**。
// 动机：抓屏验证不能依赖"桌面上恰好有什么"，否则两次抓屏之间桌面内容变化
// 会让比对失效。这个工具在指定位置显示一块确定颜色的无边框窗口。
//
// 用法：
//   ./color_window_macos <hex> <x> <y> <w> <h> [seconds]
//   例：./color_window_macos FF0000 100 200 400 300 30
//
// 与 scripts/color_window.c（X11 版）对应，两个平台的验证方法保持一致。

#import <Cocoa/Cocoa.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static unsigned parseHexColor(const char* text) {
    unsigned value = 0;
    if (std::sscanf(text, "%x", &value) != 1) {
        return 0;
    }
    return value & 0xFFFFFFu;
}

@interface ColorWindowDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic) NSWindow* window;
@property(nonatomic) int lifetimeSeconds;
@end

@implementation ColorWindowDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [self.window makeKeyAndOrderFront:nil];
    // 应用到所有 Space，并在全屏应用之上也可见。
    self.window.collectionBehavior =
        NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary |
        NSWindowCollectionBehaviorStationary;
    self.window.level = NSFloatingWindowLevel;

    if (self.lifetimeSeconds > 0) {
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW,
                          (int64_t)self.lifetimeSeconds * NSEC_PER_SEC),
            dispatch_get_main_queue(), ^{
              [NSApp terminate:nil];
            });
    }
}

@end

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr,
                     "usage: %s <hex-color> <x> <y> <w> <h> <seconds>\n"
                     "  example: %s FF0000 100 200 400 300 30\n",
                     argv[0], argv[0]);
        return 2;
    }
    const unsigned rgb = parseHexColor(argv[1]);
    const double x = std::atof(argv[2]);
    const double y = std::atof(argv[3]);
    const double w = std::atof(argv[4]);
    const double h = std::atof(argv[5]);
    const int seconds = std::atoi(argv[6]);

    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        ColorWindowDelegate* delegate = [[ColorWindowDelegate alloc] init];
        delegate.lifetimeSeconds = seconds;

        // 注意 macOS 窗口原点在屏幕左下角：Cocoa 坐标需从屏幕高度换算。
        NSRect frame = NSMakeRect(x, y, w, h);
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.backgroundColor =
            [NSColor colorWithCalibratedRed:((rgb >> 16) & 0xFF) / 255.0
                                      green:((rgb >> 8) & 0xFF) / 255.0
                                       blue:(rgb & 0xFF) / 255.0
                                      alpha:1.0];
        window.opaque = YES;
        window.hasShadow = NO;
        delegate.window = window;
        app.delegate = delegate;

        std::printf("color_window: #%06X at (%.0f,%.0f) %.0fx%.0f for %ds\n",
                    rgb, x, y, w, h, seconds);
        std::fflush(stdout);
        [app run];
    }
    return 0;
}
