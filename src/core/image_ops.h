#pragma once

#include "core/geometry.h"
#include "core/image_buffer.h"

// 「J2」图像内核 — 基础图像操作
// 约束：只依赖 C++ 标准库。所有操作返回新缓冲，不修改输入。

namespace jie2::core {

/// 从源图裁剪矩形区域（源图本地坐标）。
/// 区域会被裁剪到图像范围内；完全越界返回空图。
ImageBuffer crop(const ImageBuffer& source, const Rect& region);

/// 把 src 粘贴到 dst 的指定位置（越界部分自动裁掉）。
/// 用于多显示器截图拼接：各屏图像按虚拟桌面坐标贴进大画布。
void blit(ImageBuffer& dst, const ImageBuffer& src, Point topLeft);

/// 最近邻缩放（M1 用于放大镜；高质量缩放留待标注器阶段按需增强）。
ImageBuffer scaleNearest(const ImageBuffer& source, int width, int height);

/// 从两张图计算逐像素最大绝对差，用于测试中验证"像素级正确"。
/// 返回 {最大差值, 不同像素数}。
struct PixelDiff {
    int maxChannelDelta = 0;
    std::size_t differingPixels = 0;
};
PixelDiff comparePixels(const ImageBuffer& a, const ImageBuffer& b);

// ---------------------------------------------------------------------------
// 输出装饰（M1：专业版对齐项 —— 带边框输出 / 带阴影输出 / 圆角截图）
// ---------------------------------------------------------------------------

/// 在四周加纯色边框。width<=0 时原样返回。
ImageBuffer addBorder(const ImageBuffer& source, int width, Rgba color);

struct ShadowSpec {
    int blurRadius = 12;  // 阴影扩散半径（像素）
    int offsetX = 0;      // 正值向右
    int offsetY = 4;      // 正值向下
    Rgba color{0, 0, 0, 140};
};

/// 合成投影。输出画布会扩大以容纳模糊与偏移（四边各留
/// blurRadius + 偏移量）。阴影只由源图 alpha 决定，与源图颜色无关。
ImageBuffer addShadow(const ImageBuffer& source, const ShadowSpec& spec);

/// 圆角处理：半径之外的区域做抗锯齿透明衰减。
/// radius<=0 时原样返回。非 RGBA 输入会先按 RGBA 解释。
ImageBuffer roundCorners(const ImageBuffer& source, int radius);

// ---------------------------------------------------------------------------
// 标注工具的图像效果（M2 的基础件：马赛克 / 高斯模糊）
// 两者都只作用于 region 内，区域外像素原样保留 —— 标注器需要"只糊选中的块"。
// ---------------------------------------------------------------------------

/// 马赛克：region 内按 blockSize×blockSize 分块，每块用块内平均色填充。
/// blockSize<=1 时不做处理。
/// ellipseMask 为 true 时，region 被解释为**椭圆的外接矩形**，
/// 只处理椭圆内的像素（专业版对齐项："马赛克支持椭圆形状"）。
ImageBuffer pixelate(const ImageBuffer& source, const Rect& region, int blockSize,
                     bool ellipseMask = false);

/// 高斯模糊的 box-blur 近似（三次迭代，误差在实际使用中不可见）。
/// radius 为模糊半径（像素）。
/// ellipseMask 语义同 pixelate。
ImageBuffer boxBlur(const ImageBuffer& source, const Rect& region, int radius,
                    bool ellipseMask = false);

} // namespace jie2::core
