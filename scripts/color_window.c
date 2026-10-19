/* 「J2」测试辅助程序：创建一个已知颜色的全屏窗口并保持。
 *
 * 为什么需要它：在 Xvfb 等虚拟 X 环境里，`xsetroot -solid` 对 root window
 * 的着色不一定能被 XGetImage 读到（实测 Xvfb 21.x 下读到的仍是黑色）。
 * 直接映射一个真实的、有确定背景色的窗口，是验证抓屏像素更可靠的靶子。
 *
 * 用法：color_window <RRGGBB> [WxH] [秒数]
 *   例：color_window FF0000 640x480 5
 *
 * 编译：gcc -O2 -o color_window color_window.c -lX11
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    unsigned long color = 0xFF0000;
    int width = 640;
    int height = 480;
    int seconds = 5;

    if (argc > 1) {
        color = strtoul(argv[1], NULL, 16);
    }
    if (argc > 2) {
        if (sscanf(argv[2], "%dx%d", &width, &height) != 2) {
            fprintf(stderr, "尺寸格式应为 WxH\n");
            return 2;
        }
    }
    if (argc > 3) {
        seconds = atoi(argv[3]);
    }

    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "无法连接 X 服务器\n");
        return 1;
    }
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    Visual* visual = DefaultVisual(display, screen);
    int depth = DefaultDepth(display, screen);

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixel = color;
    attrs.override_redirect = True; /* 不受窗口管理器影响，保证全屏可见 */

    Window window = XCreateWindow(display, root, 0, 0,
                                  (unsigned)width, (unsigned)height, 0, depth,
                                  InputOutput, visual,
                                  CWBackPixel | CWOverrideRedirect, &attrs);
    XStoreName(display, window, "jie2-color-window");
    XMapWindow(display, window);
    XClearWindow(display, window);
    XFlush(display);

    printf("color-window: #%06lX %dx%d %ds\n", color & 0xFFFFFFu, width,
           height, seconds);
    fflush(stdout);

    sleep((unsigned)seconds);

    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
