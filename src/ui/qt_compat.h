#pragma once

#include <QMouseEvent>
#include <QPoint>
#include <QString>
#include <QWheelEvent>
#include <QtGlobal>

namespace jie2::ui {

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
inline constexpr auto kSkipEmptyParts = Qt::SkipEmptyParts;
#else
inline constexpr auto kSkipEmptyParts = QString::SkipEmptyParts;
#endif

// Qt 5.12（麒麟 V10）与 Qt 6 使用不同的鼠标坐标接口。
inline QPoint mouseGlobalPosition(const QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

inline QPoint wheelPosition(const QWheelEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().toPoint();
#else
    return event->pos();
#endif
}

} // namespace jie2::ui
