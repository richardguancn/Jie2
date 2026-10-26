#include "ui/system_menu.h"

#include <QAction>
#include <QBuffer>
#include <QGuiApplication>
#include <QMenu>
#include <QPointer>
#include <QScopedValueRollback>

#include "platform/platform.h"

namespace jie2::ui {
namespace {
bool tracking = false;

#ifdef Q_OS_MACOS
bool useSystemMenu() {
    return QGuiApplication::platformName() == QStringLiteral("cocoa");
}

std::vector<platform::ContextMenuItem> menuItems(
    QMenu* menu, std::vector<QPointer<QAction>>& actions) {
    std::vector<platform::ContextMenuItem> items;
    for (auto* action : menu->actions()) {
        if (!action->isVisible()) continue;
        platform::ContextMenuItem item;
        item.id = static_cast<int>(actions.size());
        actions.emplace_back(action);
        item.text = action->text().section('\t', 0, 0).toStdString();
        item.shortcut = action->text().section('\t', 1).toStdString();
        item.tooltip = action->toolTip().toStdString();
        item.enabled = action->isEnabled();
        item.checked = action->isCheckable() && action->isChecked();
        item.separator = action->isSeparator();
        if (auto* submenu = action->menu()) item.children = menuItems(submenu, actions);
        if (!action->icon().isNull()) {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            action->icon().pixmap(16, 16).save(&buffer, "PNG");
            item.iconPng.assign(bytes.begin(), bytes.end());
        }
        items.push_back(std::move(item));
    }
    return items;
}

QAction* showSystemMenu(QMenu* menu, const QPoint& position) {
    if (tracking) return nullptr;
    QScopedValueRollback<bool> guard(tracking, true);
    QPointer<QMenu> owner(menu);
    menu->aboutToShow();
    if (!owner) return nullptr;
    std::vector<QPointer<QAction>> actions;
    const auto items = menuItems(menu, actions);
    const int selected = platform::popupMacContextMenu(items, position.x(), position.y());
    if (owner) owner->aboutToHide();
    QPointer<QAction> action = selected >= 0 && selected < static_cast<int>(actions.size())
        ? actions[selected] : nullptr;
    // 系统菜单关闭后才执行，避免回调重建菜单或打开窗口时破坏跟踪状态。
    if (action && action->isEnabled() && action->isVisible()) action->trigger();
    return action.data();
}
#endif
}

QAction* execContextMenu(QMenu* menu, const QPoint& globalPosition) {
#ifdef Q_OS_MACOS
    if (useSystemMenu()) return showSystemMenu(menu, globalPosition);
#endif
    return menu->exec(globalPosition);
}

void popupContextMenu(QMenu* menu, const QPoint& globalPosition) {
#ifdef Q_OS_MACOS
    if (useSystemMenu()) { showSystemMenu(menu, globalPosition); return; }
#endif
    menu->popup(globalPosition);
}

bool systemMenuIsOpen() { return tracking; }
}
