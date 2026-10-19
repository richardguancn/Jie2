#include "core/ocr.h"

namespace jie2::core {

const char* toString(OcrBackendKind kind) {
    switch (kind) {
        case OcrBackendKind::Auto:             return "auto";
        case OcrBackendKind::System:           return "system";
        case OcrBackendKind::Tesseract:        return "tesseract";
        case OcrBackendKind::CustomCommand:    return "custom-command";
        case OcrBackendKind::OpenAiCompatible: return "openai-compatible";
    }
    return "unknown";
}

OcrBackendKind parseOcrBackend(const std::string& name, bool* ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    if (name.empty() || name == "auto") {
        return OcrBackendKind::Auto;
    }
    if (name == "system") {
        return OcrBackendKind::System;
    }
    if (name == "tesseract") {
        return OcrBackendKind::Tesseract;
    }
    if (name == "custom-command" || name == "custom") {
        return OcrBackendKind::CustomCommand;
    }
    if (name == "openai-compatible" || name == "openai") {
        return OcrBackendKind::OpenAiCompatible;
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return OcrBackendKind::Auto;
}

OcrConfig parseOcrConfig(const Config& config, std::string* warning) {
    OcrConfig ocr;

    const std::string backendName = config.get("ocr.backend", "openai-compatible");
    bool known = true;
    ocr.backend = parseOcrBackend(backendName, &known);
    if (!known && warning != nullptr) {
        *warning = "未知的 OCR 后端名「" + backendName + "」，已回退为 auto";
    }

    ocr.language = config.get("ocr.language", "chi_sim+eng");
    ocr.command = config.get("ocr.command", "");
    ocr.endpoint = config.get("ocr.endpoint", "");
    ocr.model = config.get("ocr.model", "");
    ocr.apiKey = config.get("ocr.api_key", "");
    ocr.timeoutMs = config.getInt("ocr.timeout_ms", 60000);
    if (ocr.timeoutMs < 1000) {
        ocr.timeoutMs = 1000;  // 下限：过小的超时会让识别必然失败
    }
    return ocr;
}

} // namespace jie2::core
