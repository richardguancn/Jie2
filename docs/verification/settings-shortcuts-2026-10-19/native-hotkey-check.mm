#include "platform/platform.h"
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#include <cstdio>
#include <string>

static void activated(const std::string&, void*) {}
int main() {
    [NSApplication sharedApplication];
    auto platform = jie2::platform::createPlatform();
    const char* cases[] = {"f17", "space", "left", "tab", "enter", "escape", "comma", "a", "alt+f19"};
    for (const char* text : cases) {
        jie2::platform::HotkeySpec spec; spec.id = "smoke";
        std::string error;
        if (!jie2::platform::parseHotkeyText(text, &spec) ||
            !platform->grabHotkey(spec, activated, nullptr, &error)) {
            std::printf("FAIL register %s: %s\n", text, error.c_str()); return 1;
        }
        const bool duplicate = platform->grabHotkey(spec, activated, nullptr, &error);
        platform->releaseAllHotkeys();
        std::printf("%s register+duplicate-check %s\n", duplicate ? "FAIL" : "PASS", text);
        if (duplicate) return 2;
    }
}
