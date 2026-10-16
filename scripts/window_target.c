// 「截2」测试辅助工具（仅用于验证，不进产品）
//
// 创建一个**普通顶层窗口**（非 override-redirect）作为窗口吸附的目标。
// 为什么不能用 scripts/color_window.c：那个窗口是 override-redirect 的，
// 会被 windowRects() 的过滤规则正确排除（菜单类窗口不参与吸附）；
// 吸附测试需要一个"看起来像真实应用"的窗口。
//
// 编译：cc scripts/window_target.c -o scripts/window_target -lX11
// 用法：./window_target <x> <y> <w> <h> [seconds]
//   窗口保持 <seconds> 秒后退出（默认 30）。

#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: window_target x y w h [seconds]\n");
        return 2;
    }
    const int x = atoi(argv[1]);
    const int y = atoi(argv[2]);
    const int w = atoi(argv[3]);
    const int h = atoi(argv[4]);
    const int seconds = argc >= 6 ? atoi(argv[5]) : 30;

    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "cannot open display\n");
        return 1;
    }
    const int screen = DefaultScreen(display);
    const Window root = RootWindow(display, screen);
    Window window = XCreateSimpleWindow(display, root, x, y, (unsigned)w,
                                        (unsigned)h, 0,
                                        WhitePixel(display, screen),
                                        BlackPixel(display, screen));
    // 设置一个可读的窗口标题，便于核对。
    XStoreName(display, window, "jie2-window-target");
    XSelectInput(display, window, ExposureMask);
    XMapWindow(display, window);
    XFlush(display);

    // 等窗口真正被映射（IsViewable）再进入等待。
    XWindowAttributes attrs;
    for (int i = 0; i < 100; ++i) {
        XGetWindowAttributes(display, window, &attrs);
        if (attrs.map_state == IsViewable) {
            break;
        }
        usleep(50 * 1000);
    }
    printf("window target at %d,%d %dx%d mapped=%d\n", x, y, w, h,
           attrs.map_state == IsViewable ? 1 : 0);
    fflush(stdout);

    sleep((unsigned)seconds);

    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
