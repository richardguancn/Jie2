#!/usr/bin/env bash
# 「J2」自定义命令行 OCR 后端示例脚本
#
# 契约（评估报告 4.4 的"自定义命令行后端"）：
#   stdin  ← PNG 图像字节流
#   stdout → 识别出的 UTF-8 文本
#   退出码 0 = 成功，非 0 = 失败
#
# 这个示例用系统 tesseract 转一手；用户可把 tesseract 换成任意
# 自训模型 / 本地推理器（如 onnxruntime、llama.cpp、自建服务客户端）。
set -eu

tmp="$(mktemp --suffix=.png)"
trap 'rm -f "$tmp"' EXIT

cat > "$tmp"
tesseract "$tmp" stdout -l "${JIE2_OCR_LANG:-eng}" 2>/dev/null
