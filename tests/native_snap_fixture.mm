#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include <cmath>
#include <cstdio>

namespace {
void reportStableWindow(NSWindow* window, NSButton* button, int attempt = 0) {
    if (attempt == 0) {
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
    const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    CGRect expected = window.frame;
    expected.origin.y = top - NSMaxY(window.frame);
    CGRect frame = expected;
    int reportedPid = 0;
    CFArrayRef entries = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow,
        static_cast<CGWindowID>(window.windowNumber));
    for (CFIndex i = 0; entries && i < CFArrayGetCount(entries); ++i) {
        auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(entries, i));
        auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kCGWindowNumber));
        int identifier = 0;
        if (number && CFNumberGetValue(number, kCFNumberIntType, &identifier) && identifier == window.windowNumber) {
            auto owner = static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kCGWindowOwnerPID));
            if (owner) CFNumberGetValue(owner, kCFNumberIntType, &reportedPid);
            CGRectMakeWithDictionaryRepresentation(static_cast<CFDictionaryRef>(
                CFDictionaryGetValue(entry, kCGWindowBounds)), &frame);
            break;
        }
    }
    if (entries) CFRelease(entries);
    if (attempt < 40 && (std::abs(frame.origin.x - expected.origin.x) > 1 ||
        std::abs(frame.origin.y - expected.origin.y) > 1 || std::abs(frame.size.width - expected.size.width) > 1 ||
        std::abs(frame.size.height - expected.size.height) > 1)) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            reportStableWindow(window, button, attempt + 1);
        });
        return;
    }
    const NSRect control = button.accessibilityFrame;
    const CGFloat x = std::floor(control.origin.x), y = std::floor(top - NSMaxY(control));
    NSDictionary* info = @{@"pid": @(NSProcessInfo.processInfo.processIdentifier), @"owner": @(reportedPid), @"id": @(window.windowNumber), @"appKit": NSStringFromRect(window.frame), @"screenTop": @(top), @"window": @[@(std::lround(frame.origin.x)), @(std::lround(frame.origin.y)),
        @(std::lround(frame.size.width)), @(std::lround(frame.size.height))],
        @"button": @[@(x), @(y), @(std::ceil(NSMaxX(control)) - x), @(std::ceil(top - control.origin.y) - y)]};
    NSData* json = [NSJSONSerialization dataWithJSONObject:info options:0 error:nil];
    std::fwrite(json.bytes, 1, json.length, stdout); std::puts(""); std::fflush(stdout);
}
}

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 180, 600, 340)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        window.title = @"J2 吸附检查";
        window.animationBehavior = NSWindowAnimationBehaviorNone;
        NSButton* button = [NSButton buttonWithTitle:@"测试按钮" target:nil action:nil];
        button.frame = NSMakeRect(50, 180, 180, 32);
        [window.contentView addSubview:button];
        NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(50, 120, 220, 30)];
        field.stringValue = @"测试输入框";
        [window.contentView addSubview:field];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [window displayIfNeeded];
        // 等系统的窗口出现动画结束，避免把过渡帧作为测试预期。
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            reportStableWindow(window, button);
        });
        [NSApp run];
    }
}
