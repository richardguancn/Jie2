#pragma once

#include <QImage>
#include <QPainter>

#include <vector>

#include "core/annotation.h"

// 「J2」UI 层 — 标注渲染（M2）
//
// 分工：
//   - 矢量标注（矩形/椭圆/箭头/铅笔/马克笔）用 QPainter 画，
//     坐标一律按**源图物理像素**给入，由调用方设置好变换。
//   - 像素类（马赛克/模糊）作用于图像缓冲，由 composeAnnotations 统一处理。
//
// 预览与最终合成共用同一套绘制函数 —— 保证"看到的"与"保存的"一致，
// 这是截图工具最容易被用户发现的错位来源。

namespace jie2::ui {

/// 绘制一条矢量标注。painter 的坐标系需为源图像素。
/// 马赛克/模糊在此处不绘制（它们是像素操作，由合成器处理）；
/// 但为了预览反馈，调用方可先用 paintPixelOpHint 画区域提示。
void paintAnnotation(QPainter& painter, const core::Annotation& annotation);

/// 马赛克/模糊在"拖拽预览"阶段的区域提示（半透明覆盖）。
void paintPixelOpHint(QPainter& painter, const core::Annotation& annotation);

/// 把全部标注合成到 base 上（矢量绘制 + 像素效果），返回新图像。
/// 不修改 base。items 为空时返回 base 的拷贝。
QImage composeAnnotations(const QImage& base,
                          const std::vector<core::Annotation>& items);

/// 只应用像素类标注（马赛克/模糊），不画矢量标注。
/// 用于预览底图：矢量标注是轻量对象、实时重绘；像素效果需要烘焙。
QImage composedAnnotationsBase(const QImage& base,
                               const std::vector<core::Annotation>& items);

/// 供工具条与状态栏用的工具显示名（"矩形" / "马赛克"…）。
const char* toolDisplayName(core::AnnotationTool tool);

} // namespace jie2::ui
