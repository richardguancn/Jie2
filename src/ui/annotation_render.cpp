#include "ui/annotation_render.h"

#include <QBrush>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>

#include <cmath>
#include <cstring>

#include "core/image_ops.h"

namespace jie2::ui {

namespace {

QColor toQColor(const core::Rgba& rgba) {
    return QColor(rgba.r, rgba.g, rgba.b, rgba.a);
}

/// 箭头头部的几何：在终点处画一个按线宽缩放的实心三角。
/// 这是"箭头"与"直线"的唯一差别，共用同一条主干线。
QPolygonF arrowHead(const QPointF& from, const QPointF& to, double width) {
    const double dx = to.x() - from.x();
    const double dy = to.y() - from.y();
    const double length = std::hypot(dx, dy);
    if (length < 1e-6) {
        return {};
    }
    const double ux = dx / length;
    const double uy = dy / length;
    const double headLength = std::max(10.0, width * 4.0);
    const double headHalfWidth = std::max(6.0, width * 2.2);

    const QPointF base(to.x() - ux * headLength, to.y() - uy * headLength);
    // 垂直方向
    const double px = -uy;
    const double py = ux;

    QPolygonF head;
    head << to
         << QPointF(base.x() + px * headHalfWidth, base.y() + py * headHalfWidth)
         << QPointF(base.x() - px * headHalfWidth, base.y() - py * headHalfWidth);
    return head;
}

/// 把点列转成 QPolygonF。
QPolygonF toPolygon(const std::vector<core::Point>& points) {
    QPolygonF poly;
    poly.reserve(static_cast<int>(points.size()));
    for (const auto& p : points) {
        poly << QPointF(p.x, p.y);
    }
    return poly;
}

} // namespace

const char* toolDisplayName(core::AnnotationTool tool) {
    switch (tool) {
        case core::AnnotationTool::Rect:    return "矩形";
        case core::AnnotationTool::Ellipse: return "椭圆";
        case core::AnnotationTool::Arrow:   return "箭头";
        case core::AnnotationTool::Pencil:  return "铅笔";
        case core::AnnotationTool::Marker:  return "马克笔";
        case core::AnnotationTool::Mosaic:  return "马赛克";
        case core::AnnotationTool::Blur:    return "模糊";
        case core::AnnotationTool::Eraser:  return "橡皮擦";
        case core::AnnotationTool::Text:    return "文字";
        case core::AnnotationTool::Number:  return "序号";
        case core::AnnotationTool::Magnifier: return "放大镜";
    }
    return "?";
}

void paintAnnotation(QPainter& painter, const core::Annotation& annotation) {
    const QColor color = toQColor(annotation.style.color);
    const double width = std::max(1, annotation.style.width);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);

    switch (annotation.tool) {
        case core::AnnotationTool::Rect:
        case core::AnnotationTool::Mosaic:
        case core::AnnotationTool::Blur: {
            // 矩形几何（马赛克/模糊的"矢量外观"在预览提示里单独处理，
            // 合成阶段走像素路径；这里仍支持画矩形边框，供提示层复用）。
            if (annotation.points.size() < 2) {
                break;
            }
            const QRectF rect = QRectF(QPointF(annotation.points.front().x,
                                               annotation.points.front().y),
                                       QPointF(annotation.points.back().x,
                                               annotation.points.back().y))
                                    .normalized();
            // 虚线样式（专业版对齐项）：矩形/折线/箭头/画笔支持。
            const Qt::PenStyle style =
                annotation.style.dashed ? Qt::DashLine : Qt::SolidLine;
            QPen pen(color, width, style, Qt::SquareCap, Qt::MiterJoin);
            painter.setPen(pen);
            // 圆角矩形标注（专业版对齐项 v2.4）。
            if (annotation.style.cornerRadius > 0) {
                const double radius = std::min<double>(
                    annotation.style.cornerRadius,
                    std::min(rect.width(), rect.height()) / 2.0);
                painter.drawRoundedRect(rect, radius, radius);
            } else {
                painter.drawRect(rect);
            }
            break;
        }
        case core::AnnotationTool::Ellipse: {
            if (annotation.points.size() < 2) {
                break;
            }
            const QRectF rect = QRectF(QPointF(annotation.points.front().x,
                                               annotation.points.front().y),
                                       QPointF(annotation.points.back().x,
                                               annotation.points.back().y))
                                    .normalized();
            QPen pen(color, width,
                     annotation.style.dashed ? Qt::DashLine : Qt::SolidLine,
                     Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(pen);
            painter.drawEllipse(rect);
            break;
        }
        case core::AnnotationTool::Arrow: {
            if (annotation.points.size() < 2) {
                break;
            }
            const QPointF from(annotation.points.front().x,
                               annotation.points.front().y);
            const QPointF to(annotation.points.back().x,
                             annotation.points.back().y);
            // 反向箭头（专业版 v2.5）：箭头头画在起点。
            const bool reverse = annotation.style.arrowReverse;
            const QPointF tail = reverse ? to : from;   // 尾部（无头端）
            const QPointF head = reverse ? from : to;   // 头部（有头端）

            QPen pen(color, width,
                     annotation.style.dashed ? Qt::DashLine : Qt::SolidLine,
                     Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(pen);
            // 主线收缩到箭头头部的基部，避免线头穿出三角。
            const double dx = head.x() - tail.x();
            const double dy = head.y() - tail.y();
            const double length = std::hypot(dx, dy);
            if (length > 1e-6) {
                const double headLength = std::max(10.0, width * 4.0);
                const double shrink = std::min(1.0, headLength / length);
                const QPointF mainEnd(tail.x() + dx * (1.0 - shrink),
                                      tail.y() + dy * (1.0 - shrink));
                painter.drawLine(tail, mainEnd);
            }
            // 双向箭头（专业版 v2.5）：尾部也画一个头。
            if (annotation.style.arrowBothEnds) {
                const double tailDx = tail.x() - head.x();
                const double tailDy = tail.y() - head.y();
                const double tailLength = std::hypot(tailDx, tailDy);
                if (tailLength > 1e-6) {
                    const double headLength = std::max(10.0, width * 4.0);
                    const double shrink = std::min(1.0, headLength / tailLength);
                    const QPointF tailMain(head.x() + tailDx * (1.0 - shrink),
                                           head.y() + tailDy * (1.0 - shrink));
                    QPen tailPen(color, width,
                                 annotation.style.dashed ? Qt::DashLine
                                                         : Qt::SolidLine,
                                 Qt::RoundCap, Qt::RoundJoin);
                    painter.setPen(tailPen);
                    painter.drawLine(head, tailMain);
                }
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawPolygon(arrowHead(tail, head, width));
            if (annotation.style.arrowBothEnds) {
                painter.drawPolygon(arrowHead(head, tail, width));
            }
            break;
        }
        case core::AnnotationTool::Pencil: {
            if (annotation.points.size() < 2) {
                break;
            }
            QPen pen(color, width,
                     annotation.style.dashed ? Qt::DashLine : Qt::SolidLine,
                     Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(pen);
            const QPolygonF poly = toPolygon(annotation.points);
            // 用折线绘制；铅笔在密集采样下视觉与曲线等价。
            painter.drawPolyline(poly);
            break;
        }
        case core::AnnotationTool::Marker: {
            if (annotation.points.size() < 2) {
                break;
            }
            // 马克笔语义：半透明粗线（默认黄色系由调用方给色）。
            QColor marker = color;
            marker.setAlpha(110);
            if (annotation.style.ellipseShape) {
                // 椭圆形状的马克笔（专业版 v2.6）：半透明椭圆高亮块，
                // 适合"圈出重点"而不是"画一笔"。
                const QRectF rect =
                    QRectF(QPointF(annotation.points.front().x,
                                   annotation.points.front().y),
                           QPointF(annotation.points.back().x,
                                   annotation.points.back().y))
                        .normalized();
                painter.setPen(Qt::NoPen);
                painter.setBrush(marker);
                painter.drawEllipse(rect);
                break;
            }
            QPen pen(marker, width * 3.0, Qt::SolidLine, Qt::RoundCap,
                     Qt::RoundJoin);
            painter.setPen(pen);
            painter.drawPolyline(toPolygon(annotation.points));
            break;
        }
        case core::AnnotationTool::Text: {
            if (annotation.points.empty() || annotation.text.empty()) {
                break;
            }
            // 文字标注：在点位置绘制（带同色描边保证在任意底色上可读，
            // 对齐专业版的"文字标注支持指定描边颜色及粗细"）。
            QFont font;
            font.setPixelSize(std::max(8, annotation.style.fontSize));
            font.setBold(true);
            painter.setFont(font);
            const QPointF origin(annotation.points.front().x,
                                 annotation.points.front().y);
            const QString text = QString::fromStdString(annotation.text);
            const QFontMetricsF metrics(font);
            const QRectF textRect = metrics.boundingRect(text);
            // 描边：先画 8 个方向的偏移副本（简单而有效的描边近似）。
            QColor outline(20, 20, 20, 200);
            if (color.lightness() < 128) {
                outline = QColor(255, 255, 255, 200);
            }
            painter.setPen(outline);
            const double offset = std::max(1.0, annotation.style.width / 2.0);
            for (int dx = -1; dx <= 1; dx += 1) {
                for (int dy = -1; dy <= 1; dy += 1) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    painter.drawText(
                        QPointF(origin.x() + dx * offset, origin.y() + dy * offset),
                        text);
                }
            }
            painter.setPen(color);
            painter.drawText(origin, text);
            Q_UNUSED(textRect);
            break;
        }
        case core::AnnotationTool::Number: {
            if (annotation.points.empty()) {
                break;
            }
            // 序号标注：实心圆 + 白色数字，圆心在点击点。
            const double radius =
                std::max(11.0, static_cast<double>(annotation.style.width) * 5.0);
            const QPointF center(annotation.points.front().x,
                                 annotation.points.front().y);
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(center, radius, radius);
            // 细白边提升在亮色背景上的辨识度。
            painter.setPen(QPen(QColor(255, 255, 255, 160),
                                std::max(1.0, radius / 8.0)));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, radius, radius);

            QFont font;
            font.setPixelSize(static_cast<int>(radius * 1.25));
            font.setBold(true);
            painter.setFont(font);
            painter.setPen(QColor(255, 255, 255));
            const QRectF textRect(center.x() - radius, center.y() - radius,
                                  radius * 2, radius * 2);
            painter.drawText(textRect, Qt::AlignCenter,
                             QString::fromStdString(annotation.text));
            break;
        }
        case core::AnnotationTool::Eraser:
            // 橡皮擦不产生可绘制的标注（它只删除已有标注），
            // 显式分支以消除 -Wswitch 警告并记录语义。
            break;
        case core::AnnotationTool::Magnifier: {
            // 预览：源区域虚线框 + 中部倍数提示。
            // （真实放大效果由 composeAnnotations 在合成阶段渲染。）
            if (annotation.points.size() < 2) {
                break;
            }
            const core::Rect bounds = annotation.boundingRect();
            if (bounds.isEmpty()) {
                break;
            }
            const QRectF rect(bounds.x, bounds.y, bounds.width, bounds.height);
            QPen pen(color, std::max(1.0, width / 2.0));
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect);

            QFont font;
            font.setPixelSize(14);
            font.setBold(true);
            painter.setFont(font);
            const QString label = QStringLiteral("放大 %1x")
                                      .arg(std::max(2, annotation.style.magnifierZoom));
            const QRectF labelRect(rect.center().x() - 40, rect.center().y() - 12,
                                   80, 24);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 150));
            painter.drawRoundedRect(labelRect, 4, 4);
            painter.setPen(QColor(255, 255, 255));
            painter.drawText(labelRect, Qt::AlignCenter, label);
            break;
        }
    }
    painter.restore();
}

void paintPixelOpHint(QPainter& painter, const core::Annotation& annotation) {
    const core::Rect bounds = annotation.boundingRect();
    if (bounds.isEmpty()) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, !annotation.style.ellipseShape);
    // 半透明白色覆盖 + 虚线框：表达"这块区域将被处理"，
    // 但不像真实效果那样让拖拽中的每一帧都做一次像素运算。
    const QRect rect(bounds.x, bounds.y, bounds.width, bounds.height);
    if (annotation.style.ellipseShape) {
        // 椭圆形状：提示层也用椭圆，保证"看到的形状 = 最终效果"。
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 70));
        painter.drawEllipse(rect);
        QPen pen(QColor(255, 255, 255, 200),
                 std::max(1, annotation.style.width / 2));
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(rect);
    } else {
        painter.fillRect(rect, QColor(255, 255, 255, 70));
        QPen pen(QColor(255, 255, 255, 200),
                 std::max(1, annotation.style.width / 2));
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect);
    }
    painter.restore();
}

QImage qImageFromBuffer(const core::ImageBuffer& buffer) {
    return QImage(buffer.data(), buffer.width(), buffer.height(),
                  static_cast<qsizetype>(buffer.stride()),
                  QImage::Format_RGBA8888)
        .copy();
}

core::ImageBuffer bufferFromQImage(const QImage& image) {
    // 前提：image 已是 Format_RGBA8888。
    core::ImageBuffer buffer(image.width(), image.height(),
                             core::PixelFormat::RGBA8888);
    const std::size_t rowBytes = static_cast<std::size_t>(image.width()) * 4;
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(buffer.scanline(y), image.constScanLine(y), rowBytes);
    }
    return buffer;
}

void applyPixelOpInPlace(QImage& image, const core::Annotation& item) {
    const core::Rect bounds = item.boundingRect();
    if (bounds.isEmpty()) {
        return;
    }
    core::ImageBuffer buffer = bufferFromQImage(image);
    // 椭圆形状（专业版 v2.6）：马赛克/模糊支持椭圆区域。
    const bool ellipse = item.style.ellipseShape;
    if (item.tool == core::AnnotationTool::Mosaic) {
        buffer = core::pixelate(buffer, bounds, item.style.blockSize, ellipse);
    } else {
        buffer = core::boxBlur(buffer, bounds, item.style.blurRadius, ellipse);
    }
    image = qImageFromBuffer(buffer);
}

/// 放大镜标注（专业版 v2.6）：把源区域的内容放大后画在中心对齐的位置。
/// 放在 composeAnnotations 内联处理 —— 它需要读取"已渲染的当前画面"，
/// 而不是像矢量标注那样只依赖自身参数。
void drawMagnifierInPlace(QImage& image, const core::Annotation& item) {
    const core::Rect bounds = item.boundingRect();
    if (bounds.isEmpty()) {
        return;
    }
    const QRect srcRect = QRect(bounds.x, bounds.y, bounds.width, bounds.height)
                              .intersected(QRect(0, 0, image.width(), image.height()));
    if (srcRect.isEmpty()) {
        return;
    }
    const int zoom = std::max(2, item.style.magnifierZoom);
    const QImage patch = image.copy(srcRect).scaled(
        srcRect.width() * zoom, srcRect.height() * zoom, Qt::KeepAspectRatio,
        Qt::SmoothTransformation);

    // 目标位置：以源区域中心为中心画出放大块（并保证落在画布内）。
    int dx = srcRect.center().x() - patch.width() / 2;
    int dy = srcRect.center().y() - patch.height() / 2;
    dx = std::clamp(dx, 0, std::max(0, image.width() - patch.width()));
    dy = std::clamp(dy, 0, std::max(0, image.height() - patch.height()));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // 放大内容 + 描边（与源区域用同色框相连，方便看出"从哪放大"）。
    painter.drawImage(QPoint(dx, dy), patch);
    painter.setPen(QPen(toQColor(item.style.color), 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRect(dx, dy, patch.width() - 1, patch.height() - 1));
    painter.drawRect(srcRect.adjusted(0, 0, -1, -1));
    painter.end();
}

QImage composedAnnotationsBase(const QImage& base,
                               const std::vector<core::Annotation>& items) {
    if (base.isNull()) {
        return base;
    }
    QImage result = base.convertToFormat(QImage::Format_RGBA8888);
    for (const core::Annotation& item : items) {
        if (item.tool == core::AnnotationTool::Mosaic ||
            item.tool == core::AnnotationTool::Blur) {
            applyPixelOpInPlace(result, item);
        }
    }
    return result;
}

QImage composeAnnotations(const QImage& base,
                          const std::vector<core::Annotation>& items) {
    if (base.isNull()) {
        return base;
    }
    // 统一到 RGBA8888：QPainter 可直接绘制，且像素效果在统一布局上做。
    QImage result = base.convertToFormat(QImage::Format_RGBA8888);

    for (const core::Annotation& item : items) {
        if (item.tool == core::AnnotationTool::Mosaic ||
            item.tool == core::AnnotationTool::Blur) {
            applyPixelOpInPlace(result, item);
            continue;
        }
        if (item.tool == core::AnnotationTool::Magnifier) {
            drawMagnifierInPlace(result, item);
            continue;
        }

        QPainter painter(&result);
        paintAnnotation(painter, item);
        painter.end();
    }
    return result;
}

} // namespace jie2::ui
