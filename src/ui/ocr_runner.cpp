#include "ui/ocr_runner.h"

#include <cstring>

#include <QBuffer>
#include <QDebug>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

namespace jie2::ui {

namespace {

/// 把 ImageBuffer 转成 QImage（QProcess 与网络都需要）。
/// 按缓冲声明的格式选择 QImage 格式（与 tray.cpp 的约定一致）。
QImage toQImage(const core::ImageBuffer& buffer) {
    if (buffer.isEmpty()) {
        return {};
    }
    const QImage::Format format =
        buffer.format() == core::PixelFormat::RGBA8888 ? QImage::Format_RGBA8888
                                                       : QImage::Format_ARGB32;
    const QImage wrapped(buffer.data(), buffer.width(), buffer.height(),
                         static_cast<qsizetype>(buffer.stride()), format);
    return wrapped.copy();
}

/// PNG 编码（tesseract 与自定义命令都收 PNG）。
QByteArray encodePng(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

/// 解析自定义命令：支持引号包裹的路径与参数。
/// 例：`"/opt/my ocr" --lang chi` → ["/opt/my ocr", "--lang", "chi"]
QStringList splitCommand(const QString& command) {
    QStringList parts;
    QString current;
    bool inQuotes = false;
    for (const QChar ch : command) {
        if (ch == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            continue;
        }
        if (ch.isSpace() && !inQuotes) {
            if (!current.isEmpty()) {
                parts << current;
                current.clear();
            }
            continue;
        }
        current.append(ch);
    }
    if (!current.isEmpty()) {
        parts << current;
    }
    return parts;
}

} // namespace

QString OcrRunner::probeCommandBackend(const core::OcrConfig& config) {
    switch (config.backend) {
        case core::OcrBackendKind::Tesseract:
        case core::OcrBackendKind::Auto: {
            if (QStandardPaths::findExecutable(QStringLiteral("tesseract"))
                    .isEmpty()) {
                return QStringLiteral("找不到 tesseract 命令（请安装 tesseract-ocr）");
            }
            return {};
        }
        case core::OcrBackendKind::CustomCommand: {
            const QString command = QString::fromStdString(config.command);
            if (command.trimmed().isEmpty()) {
                return QStringLiteral("未配置 ocr.command（自定义命令行后端）");
            }
            const QStringList parts = splitCommand(command);
            if (parts.isEmpty()) {
                return QStringLiteral("ocr.command 无法解析");
            }
            if (QStandardPaths::findExecutable(parts.first()).isEmpty() &&
                !QFileInfo::exists(parts.first())) {
                return QStringLiteral("找不到命令：%1").arg(parts.first());
            }
            return {};
        }
        case core::OcrBackendKind::OpenAiCompatible: {
            if (QString::fromStdString(config.endpoint).trimmed().isEmpty()) {
                return QStringLiteral("未配置 ocr.endpoint（OpenAI 兼容端点）");
            }
            return {};
        }
        case core::OcrBackendKind::System: {
            return QStringLiteral("系统后端由平台层提供，无需命令行探测");
        }
    }
    return QStringLiteral("未知后端");
}

core::OcrResult OcrRunner::run(const core::OcrConfig& config,
                               const core::ImageBuffer& image,
                               const platform::Platform* platform) {
    if (image.isEmpty()) {
        return core::OcrResult::failure("图像为空");
    }

    switch (config.backend) {
        case core::OcrBackendKind::System:
            return runSystem(config, image, platform);

        case core::OcrBackendKind::Tesseract:
            return runTesseract(config, toQImage(image));

        case core::OcrBackendKind::CustomCommand:
            return runCustomCommand(config, toQImage(image));

        case core::OcrBackendKind::OpenAiCompatible:
            return runOpenAiCompatible(config, toQImage(image));

        case core::OcrBackendKind::Auto:
            break;
    }

    // auto：先系统能力，失败再试 tesseract；两条都失败时把两条原因都带上，
    // 让用户知道"试过什么、为什么都没成"。
    core::OcrResult systemResult;
    if (platform != nullptr) {
        systemResult = runSystem(config, image, platform);
        if (systemResult.ok) {
            return systemResult;
        }
    } else {
        systemResult = core::OcrResult::failure("平台层不可用", "system");
    }

    const core::OcrResult tesseractResult =
        runTesseract(config, toQImage(image));
    if (tesseractResult.ok) {
        return tesseractResult;
    }
    return core::OcrResult::failure(
        QStringLiteral("auto 回退链均失败：系统后端（%1）；tesseract（%2）")
            .arg(QString::fromStdString(systemResult.error),
                 QString::fromStdString(tesseractResult.error))
            .toStdString(),
        "auto");
}

core::OcrResult OcrRunner::scanBarcode(const core::ImageBuffer& image) {
    if (image.isEmpty()) {
        return core::OcrResult::failure("图像为空", "zbar");
    }
    const QString executable =
        QStandardPaths::findExecutable(QStringLiteral("zbarimg"));
    if (executable.isEmpty()) {
        return core::OcrResult::failure(
            "找不到 zbarimg 命令。安装方式：\n"
            "  Debian/Ubuntu（含麒麟/统信）：sudo apt install zbar-tools\n"
            "  Fedora/RHEL：sudo dnf install zbar\n"
            "  macOS（Homebrew）：brew install zbar",
            "zbar");
    }

    // zbarimg 只接受文件输入；--raw 输出纯文本（不带 "QR-Code:" 前缀）。
    QTemporaryDir dir;
    if (!dir.isValid()) {
        return core::OcrResult::failure("无法创建临时目录", "zbar");
    }
    const QString inputPath = dir.filePath(QStringLiteral("barcode.png"));
    {
        const QImage qimage = toQImage(image);
        if (qimage.isNull() || !qimage.save(inputPath, "PNG")) {
            return core::OcrResult::failure("无法写入临时图像", "zbar");
        }
    }

    QProcess process;
    process.setProgram(executable);
    // --raw：只输出解码文本；-q：抑制进度噪声。
    process.setArguments({QStringLiteral("--raw"), QStringLiteral("-q"), inputPath});
    process.start();
    if (!process.waitForStarted(10000)) {
        return core::OcrResult::failure("zbarimg 启动超时", "zbar");
    }
    if (!process.waitForFinished(20000)) {
        process.kill();
        return core::OcrResult::failure("zbarimg 超时", "zbar");
    }
    // zbarimg 的退出码约定：0 = 找到并解码；4 = **没有找到码**（不是错误）。
    const int code = process.exitCode();
    const QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit) {
        return core::OcrResult::failure("zbarimg 异常退出", "zbar");
    }
    if (code != 0 && code != 4) {
        const QString stderrText =
            QString::fromUtf8(process.readAllStandardError()).trimmed();
        return core::OcrResult::failure(
            QStringLiteral("zbarimg 退出码 %1：%2").arg(code).arg(stderrText.left(200))
                .toStdString(),
            "zbar");
    }
    if (output.isEmpty()) {
        // 码未识别到：如实返回空文本（与 OCR 的"没识别到文字"同一语义）。
        return core::OcrResult::success("", "zbar");
    }
    return core::OcrResult::success(output.toStdString(), "zbar");
}

core::OcrResult OcrRunner::runSystem(const core::OcrConfig& config,
                                     const core::ImageBuffer& image,
                                     const platform::Platform* platform) {
    if (platform == nullptr) {
        return core::OcrResult::failure("平台层不可用", "system");
    }
    // core 的语言串（"chi_sim+eng"）→ BCP-47 提示（"zh-Hans"）。
    std::string hint;
    if (config.language.find("chi") != std::string::npos) {
        hint = "zh-Hans";
    } else if (config.language.find("eng") != std::string::npos) {
        hint = "en-US";
    }
    return platform->recognizeText(image, hint);
}

core::OcrResult OcrRunner::runTesseract(const core::OcrConfig& config,
                                        const QImage& image) {
    const QString executable =
        QStandardPaths::findExecutable(QStringLiteral("tesseract"));
    if (executable.isEmpty()) {
        return core::OcrResult::failure(
            "找不到 tesseract 命令（请安装 tesseract-ocr 与相应语言包）",
            "tesseract");
    }
    if (image.isNull()) {
        return core::OcrResult::failure("图像为空", "tesseract");
    }

    // tesseract 只接受文件输入：写入临时目录，输出到 stdout。
    QTemporaryDir dir;
    if (!dir.isValid()) {
        return core::OcrResult::failure("无法创建临时目录", "tesseract");
    }
    const QString inputPath = dir.filePath(QStringLiteral("input.png"));
    if (!image.save(inputPath, "PNG")) {
        return core::OcrResult::failure("无法写入临时图像", "tesseract");
    }

    QProcess process;
    process.setProgram(executable);
    QStringList args;
    args << inputPath << QStringLiteral("stdout");
    if (!config.language.empty()) {
        args << QStringLiteral("-l") << QString::fromStdString(config.language);
    }
    process.setArguments(args);
    process.start();
    if (!process.waitForStarted(config.timeoutMs)) {
        return core::OcrResult::failure("tesseract 启动超时", "tesseract");
    }
    if (!process.waitForFinished(config.timeoutMs)) {
        process.kill();
        return core::OcrResult::failure(
            QStringLiteral("tesseract 超时（%1ms）").arg(config.timeoutMs)
                .toStdString(),
            "tesseract");
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString stderrText =
            QString::fromUtf8(process.readAllStandardError()).trimmed();
        return core::OcrResult::failure(
            QStringLiteral("tesseract 退出码 %1：%2")
                .arg(process.exitCode())
                .arg(stderrText.left(200))
                .toStdString(),
            "tesseract");
    }
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    return core::OcrResult::success(output.trimmed().toStdString(), "tesseract");
}

core::OcrResult OcrRunner::runCustomCommand(const core::OcrConfig& config,
                                            const QImage& image) {
    const QString command = QString::fromStdString(config.command);
    if (command.trimmed().isEmpty()) {
        return core::OcrResult::failure(
            "未配置 ocr.command；自定义命令行后端需要提供可执行文件路径",
            "custom-command");
    }
    if (image.isNull()) {
        return core::OcrResult::failure("图像为空", "custom-command");
    }

    const QStringList parts = splitCommand(command);
    if (parts.isEmpty()) {
        return core::OcrResult::failure("ocr.command 无法解析", "custom-command");
    }

    QProcess process;
    process.setProgram(parts.first());
    if (parts.size() > 1) {
        process.setArguments(parts.mid(1));
    }
    process.start();
    if (!process.waitForStarted(config.timeoutMs)) {
        return core::OcrResult::failure(
            QStringLiteral("命令启动失败：%1").arg(parts.first()).toStdString(),
            "custom-command");
    }
    // 契约：stdin 收 PNG 字节流，stdout 出 UTF-8 文本。
    process.write(encodePng(image));
    process.closeWriteChannel();
    if (!process.waitForFinished(config.timeoutMs)) {
        process.kill();
        return core::OcrResult::failure(
            QStringLiteral("命令超时（%1ms）").arg(config.timeoutMs).toStdString(),
            "custom-command");
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString stderrText =
            QString::fromUtf8(process.readAllStandardError()).trimmed();
        return core::OcrResult::failure(
            QStringLiteral("命令退出码 %1：%2")
                .arg(process.exitCode())
                .arg(stderrText.left(200))
                .toStdString(),
            "custom-command");
    }
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    return core::OcrResult::success(output.trimmed().toStdString(),
                                    "custom-command");
}

core::OcrResult OcrRunner::runOpenAiCompatible(const core::OcrConfig& config,
                                               const QImage& image) {
    const QString endpoint = QString::fromStdString(config.endpoint);
    const QString model = QString::fromStdString(config.model);
    if (endpoint.trimmed().isEmpty()) {
        return core::OcrResult::failure("未配置 ocr.endpoint", "openai-compatible");
    }
    if (model.trimmed().isEmpty()) {
        return core::OcrResult::failure("未配置 ocr.model", "openai-compatible");
    }
    if (image.isNull()) {
        return core::OcrResult::failure("图像为空", "openai-compatible");
    }

    // base64 PNG → data URL
    const QByteArray png = encodePng(image);
    if (png.isEmpty()) {
        return core::OcrResult::failure("PNG 编码失败", "openai-compatible");
    }
    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(png.toBase64());

    QJsonObject textPart;
    textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
    textPart.insert(QStringLiteral("text"),
                    QStringLiteral("识别这张图片中的文字，只输出文字内容本身，"
                                   "不要添加解释。"));
    QJsonObject imageUrl;
    imageUrl.insert(QStringLiteral("url"), dataUrl);
    QJsonObject imagePart;
    imagePart.insert(QStringLiteral("type"), QStringLiteral("image_url"));
    imagePart.insert(QStringLiteral("image_url"), imageUrl);
    QJsonArray content;
    content.append(textPart);
    content.append(imagePart);
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), content);
    QJsonArray messages;
    messages.append(message);
    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("messages"), messages);

    QString url = endpoint;
    if (!url.endsWith(QLatin1Char('/'))) {
        url += QLatin1Char('/');
    }
    url += QStringLiteral("chat/completions");

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    if (!config.apiKey.empty()) {
        request.setRawHeader("Authorization",
                             QByteArray("Bearer ") +
                                 QByteArray::fromStdString(config.apiKey));
    }

    QNetworkAccessManager manager;
    QNetworkReply* reply =
        manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

    // 同步等待（带超时）：桌面工具的"点一下等结果"模型。
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(config.timeoutMs);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        return core::OcrResult::failure(
            QStringLiteral("端点超时（%1ms）").arg(config.timeoutMs).toStdString(),
            "openai-compatible");
    }
    const QByteArray payload = reply->readAll();
    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString networkError = reply->errorString();
    const bool hadError = reply->error() != QNetworkReply::NoError;
    reply->deleteLater();

    if (hadError) {
        return core::OcrResult::failure(
            QStringLiteral("请求失败（HTTP %1）：%2")
                .arg(statusCode)
                .arg(networkError)
                .toStdString(),
            "openai-compatible");
    }

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        return core::OcrResult::failure("响应不是 JSON 对象", "openai-compatible");
    }
    const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return core::OcrResult::failure(
            "响应缺少 choices（检查 endpoint 是否为 OpenAI 兼容接口）",
            "openai-compatible");
    }
    const QString text =
        choices.first()
            .toObject()
            .value(QStringLiteral("message"))
            .toObject()
            .value(QStringLiteral("content"))
            .toString();
    return core::OcrResult::success(text.trimmed().toStdString(),
                                    "openai-compatible");
}

} // namespace jie2::ui
