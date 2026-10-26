#import <Cocoa/Cocoa.h>

#include "native_menu_test_support.h"
#include "ui/system_menu.h"
#include <QApplication>

namespace {
bool hasOnlySystemRows(NSMenu* menu) {
    for (NSMenuItem* item in menu.itemArray) {
        if (item.view != nil || [item.title containsString:@"\t"]) return false;
        if (item.submenu && !hasOnlySystemRows(item.submenu)) return false;
    }
    return true;
}
}

QString selectNativeMenuForTest(const QStringList& path, const std::function<void()>& open) {
    __block bool observed = false;
    __block QString failure;
    id observer = [NSNotificationCenter.defaultCenter
        addObserverForName:NSMenuDidBeginTrackingNotification object:nil queue:nil
        usingBlock:^(NSNotification* notification) {
            if (observed) return;
            observed = true;
            NSMenu* root = notification.object;
            if (![root isKindOfClass:NSMenu.class] || !hasOnlySystemRows(root))
                failure = QStringLiteral("菜单包含非系统绘制的行");
            if (!jie2::ui::systemMenuIsOpen() || QApplication::activePopupWidget())
                failure = QStringLiteral("未进入原生菜单跟踪状态");
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC),
                dispatch_get_main_queue(), ^{
                    NSMenu* current = root;
                    for (int i = 0; i < path.size(); ++i) {
                        NSString* title = [NSString stringWithUTF8String:path[i].toUtf8().constData()];
                        const NSInteger index = [current indexOfItemWithTitle:title];
                        if (index < 0) { failure = QStringLiteral("找不到菜单项：") + path[i]; break; }
                        NSMenuItem* item = [current itemAtIndex:index];
                        if (i + 1 < path.size()) {
                            current = item.submenu;
                            if (!current) { failure = QStringLiteral("缺少子菜单"); break; }
                        } else if (item.enabled) {
                            [current performActionForItemAtIndex:index];
                        }
                    }
                    [root cancelTrackingWithoutAnimation];
                });
        }];
    open();
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    if (!observed) return QStringLiteral("未打开 macOS 系统菜单");
    if (jie2::ui::systemMenuIsOpen()) return QStringLiteral("菜单关闭后仍处于跟踪状态");
    return failure;
}
