// 「截2」测试辅助工具（仅用于验证，不进产品）
//
// macOS 上把鼠标移动到指定坐标并可选地执行一次拖拽。
// 用途：验证触发角（光标停留）与选择遮罩的窗口吸附等交互路径。
//
// 编译：clang -framework Cocoa scripts/mouse_tool.m -o scripts/mouse_tool
// 用法：
//   ./mouse_tool move <x> <y>
//   ./mouse_tool drag <x1> <y1> <x2> <y2> [steps]

#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>

static void postMouse(CGEventType type, CGPoint point) {
    CGEventRef event = CGEventCreateMouseEvent(NULL, type, point,
                                               kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static void moveTo(CGPoint to) {
    postMouse(kCGEventMouseMoved, to);
}

static void dragFromTo(CGPoint from, CGPoint to, int steps) {
    postMouse(kCGEventMouseMoved, from);
    usleep(80 * 1000);
    postMouse(kCGEventLeftMouseDown, from);
    usleep(80 * 1000);
    for (int i = 1; i <= steps; ++i) {
        const double t = (double)i / steps;
        CGPoint p = CGPointMake(from.x + (to.x - from.x) * t,
                                from.y + (to.y - from.y) * t);
        postMouse(kCGEventLeftMouseDragged, p);
        usleep(30 * 1000);
    }
    usleep(80 * 1000);
    postMouse(kCGEventLeftMouseUp, to);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mouse_tool move x y | drag x1 y1 x2 y2 [steps]\n");
        return 2;
    }
    if (strcmp(argv[1], "move") == 0 && argc >= 4) {
        moveTo(CGPointMake(atof(argv[2]), atof(argv[3])));
        printf("moved to %s,%s\n", argv[2], argv[3]);
        return 0;
    }
    if (strcmp(argv[1], "drag") == 0 && argc >= 6) {
        const int steps = argc >= 7 ? atoi(argv[6]) : 20;
        dragFromTo(CGPointMake(atof(argv[2]), atof(argv[3])),
                   CGPointMake(atof(argv[4]), atof(argv[5])), steps);
        printf("dragged %s,%s -> %s,%s\n", argv[2], argv[3], argv[4], argv[5]);
        return 0;
    }
    if (strcmp(argv[1], "pos") == 0) {
        CGEventRef e = CGEventCreate(NULL);
        CGPoint p = CGEventGetLocation(e);
        CFRelease(e);
        printf("pos: %.0f,%.0f  trusted=%d\n", p.x, p.y,
               AXIsProcessTrusted() ? 1 : 0);
        return 0;
    }
    if (strcmp(argv[1], "wins") == 0) {
        CFArrayRef list = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
            kCGNullWindowID);
        if (list) {
            CFIndex n = CFArrayGetCount(list);
            for (CFIndex i = 0; i < n; ++i) {
                CFDictionaryRef w = CFArrayGetValueAtIndex(list, i);
                int layer = 0;
                CFNumberRef lr = CFDictionaryGetValue(w, kCGWindowLayer);
                if (lr) CFNumberGetValue(lr, kCFNumberIntType, &layer);
                CFStringRef owner = CFDictionaryGetValue(w, kCGWindowOwnerName);
                CFStringRef name = CFDictionaryGetValue(w, kCGWindowName);
                CFDictionaryRef br = CFDictionaryGetValue(w, kCGWindowBounds);
                CGRect r = CGRectZero;
                if (br) CGRectMakeWithDictionaryRepresentation(br, &r);
                char ob[128] = "", nb[256] = "";
                if (owner) CFStringGetCString(owner, ob, sizeof(ob), kCFStringEncodingUTF8);
                if (name) CFStringGetCString(name, nb, sizeof(nb), kCFStringEncodingUTF8);
                printf("L%-3d %-20.20s %6.0fx%-6.0f+%-6.0f+%-6.0f  %s\n", layer, ob,
                       r.size.width, r.size.height, r.origin.x, r.origin.y, nb);
            }
            CFRelease(list);
        }
        return 0;
    }
    if (strcmp(argv[1], "key") == 0 && argc >= 3) {
        CGKeyCode code = 0;
        if (strcmp(argv[2], "return") == 0) {
            code = 36;
        } else if (strcmp(argv[2], "escape") == 0) {
            code = 53;
        } else {
            fprintf(stderr, "unknown key: %s\n", argv[2]);
            return 2;
        }
        CGEventRef down = CGEventCreateKeyboardEvent(NULL, code, true);
        CGEventRef up = CGEventCreateKeyboardEvent(NULL, code, false);
        CGEventPost(kCGHIDEventTap, down);
        usleep(30 * 1000);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(down);
        CFRelease(up);
        printf("key %s\n", argv[2]);
        return 0;
    }
    fprintf(stderr, "bad args\n");
    return 2;
}
