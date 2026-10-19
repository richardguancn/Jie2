#pragma once

#include <string>

#include "core/config.h"

// 「J2」OCR 类型与配置（M5）
//
// 架构决策（评估报告 4.4）：识别模型可自由配置，不硬编码单一服务。
// 本文件只放**纯类型与配置解析**（不依赖 Qt / 平台 API）：
//   - 后端选择与参数
//   - 结果类型
// 真正的执行在 ui 层的 OcrRunner（需要 QProcess / QNetworkAccessManager）。
//
// 分层说明：macOS 的 Vision 是**系统能力**，因此走平台层
// （Platform::recognizeText）；tesseract / 自定义命令 / OpenAI 兼容端点
// 属于应用层集成，走 ui 层的 OcrRunner。

namespace jie2::core {

/// OCR 后端类型。
enum class OcrBackendKind {
    Auto,              // 兼容旧 auto 名称：使用大模型
    System,            // 强制系统能力
    Tesseract,         // 系统 tesseract 命令（离线）
    CustomCommand,     // 用户自定义命令行（stdin 收图 / stdout 出文本）
    OpenAiCompatible,  // OpenAI 兼容大模型端点（默认，发送前确认）
};

const char* toString(OcrBackendKind kind);
/// 解析后端名；无法识别返回 Auto 并把 ok 置 false（调用方据此提示用户）。
OcrBackendKind parseOcrBackend(const std::string& name, bool* ok = nullptr);

/// OCR 一次识别的结果。
struct OcrResult {
    bool ok = false;
    std::string text;      // 识别出的文本（成功时）
    std::string error;     // 失败原因（失败时必填）
    std::string backend;   // 实际使用的后端标识（用于回退链展示）

    static OcrResult failure(std::string reason, std::string backendId = {}) {
        OcrResult result;
        result.ok = false;
        result.error = std::move(reason);
        result.backend = std::move(backendId);
        return result;
    }
    static OcrResult success(std::string text, std::string backendId = {}) {
        OcrResult result;
        result.ok = true;
        result.text = std::move(text);
        result.backend = std::move(backendId);
        return result;
    }
};

/// OCR 配置（从 jie2.conf 的 "ocr." 前缀键读取）。
struct OcrConfig {
    OcrBackendKind backend = OcrBackendKind::OpenAiCompatible;
    std::string language = "chi_sim+eng";  // tesseract 语言串 / 系统后端的语言提示
    std::string command;                   // 自定义命令行（可执行文件路径或含参数）
    std::string endpoint;                  // OpenAI 兼容 base URL
    std::string model;                     // 模型名
    std::string apiKey;                    // 服务密钥（配置文件仅当前用户可读写）
    int timeoutMs = 60000;

    /// 是否为云端后端（决定是否需要"联网需显式同意"的提示）。
    bool isCloud() const { return backend == OcrBackendKind::OpenAiCompatible || backend == OcrBackendKind::Auto; }
};

/// 从配置读取（键：ocr.backend / ocr.language / ocr.command /
/// ocr.endpoint / ocr.model / ocr.api_key / ocr.timeout_ms）。
/// 未知后端名会被记录到 fallbackError（若非空），并回退为 Auto。
OcrConfig parseOcrConfig(const Config& config, std::string* warning = nullptr);

} // namespace jie2::core
