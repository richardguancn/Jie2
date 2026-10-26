#pragma once

#include <QPoint>

class QAction;
class QMenu;

namespace jie2::ui {
// macOS 使用系统菜单；其他平台保留 Qt 菜单。
QAction* execContextMenu(QMenu* menu, const QPoint& globalPosition);
void popupContextMenu(QMenu* menu, const QPoint& globalPosition);
bool systemMenuIsOpen();
}
