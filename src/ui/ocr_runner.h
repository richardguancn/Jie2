#pragma once

#include <QImage>
#include <QString>

#include "core/ocr.h"
#include "platform/platform.h"

// 「J2」OCR 执行器（M5）
//
// 职责：按配置调用具体后端并处理回退。位于 ui 层是因为执行需要
// QProcess（命令行后端）与 QNetworkAccessManager（云端后端）；
// 类型与配置解析在 core::ocr（纯 C++，可单测）。
//
// 常规界面统一使用大模型；旧 auto 配置也走大模型，不隐式回退本地识别。
// 显式 system / tesseract / custom-command 只保留兼容命令行与旧的验证脚本。

namespace jie2::ui {

class OcrRunner {
public:
    /// 执行识别。platform 用于"系统能力"后端（macOS Vision）。
    /// 同步执行（内部起事件循环等待进程/网络），适合桌面工具的
    /// "点一下等结果"的交互模型。
    static core::OcrResult run(const core::OcrConfig& config,
                               const core::ImageBuffer& image,
                               const platform::Platform* platform);

    /// 探测某个命令行后端是否可用（用于配置界面与诊断输出）。
    /// 返回空字符串表示可用；否则返回不可用原因。
    static QString probeCommandBackend(const core::OcrConfig& config);

    /// 二维码/条形码识别（专业版对齐项：`barcode-scan` / `barcode-decode`）。
    ///
    /// 用 zbarimg（Debian/Ubuntu 包 zbar-tools；macOS 走 Homebrew 的 zbar）。
    /// 一段可含多个码：每行一条结果（zbarimg 的原生输出格式已按行分隔）。
    /// 未安装 zbarimg 时返回带安装指引的失败，不静默。
    static core::OcrResult scanBarcode(const core::ImageBuffer& image);

    static core::OcrResult translate(const core::OcrConfig& config,
                                     const QImage& image, const QString& text,
                                     const QString& targetLanguage);

private:
    static core::OcrResult runSystem(const core::OcrConfig& config,
                                     const core::ImageBuffer& image,
                                     const platform::Platform* platform);
    static core::OcrResult runTesseract(const core::OcrConfig& config,
                                        const QImage& image);
    static core::OcrResult runCustomCommand(const core::OcrConfig& config,
                                            const QImage& image);
    static core::OcrResult runOpenAiCompatible(const core::OcrConfig& config,
                                               const QImage& image,
                                               const QString& prompt = {},
                                               const QString& inputText = {});
};

} // namespace jie2::ui
