#include <csignal>
#include <cstdio>
#include <cstring>
#include <utility>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include "core/annotation.h"
#include "core/ocr.h"
#include "platform/platform.h"
#include "ui/annotation_render.h"
#include "ui/app_paths.h"
#include "ui/ocr_runner.h"
#include <QSslSocket>
#include <QBuffer>
#include "ui/pin_window.h"
#include "ui/single_instance.h"
#include "ui/tray.h"

namespace {

/// 自动化验证入口（M4）：构造四象限测试图，创建贴图窗口并依次导出
/// 原始 / 旋转 / 翻转 / 反色四种状态，供逐像素检查。
/// 用法：jie2 --test-pin <out-prefix>（生成 <prefix>-orig.png / -rot.png / -flip.png / -inv.png）
int runTestPin(const QString& prefix) {
    // 四象限：左上红、右上绿、左下蓝、右下黄（各 50x30）。
    QImage image(100, 60, QImage::Format_ARGB32);
    image.fill(Qt::black);
    for (int y = 0; y < 60; ++y) {
        for (int x = 0; x < 100; ++x) {
            QColor c;
            if (y < 30) {
                c = x < 50 ? QColor(255, 0, 0) : QColor(0, 255, 0);
            } else {
                c = x < 50 ? QColor(0, 0, 255) : QColor(255, 255, 0);
            }
            image.setPixelColor(x, y, c);
        }
    }

    auto* pin = new jie2::ui::PinWindow(image);
    pin->show();

    const auto save = [&prefix](const QImage& img, const char* suffix) {
        const QString path = prefix + QStringLiteral("-") +
                             QString::fromLatin1(suffix) + QStringLiteral(".png");
        if (img.isNull() || !img.save(path, "PNG")) {
            std::fprintf(stderr, "test-pin failed: cannot save %s\n",
                         path.toUtf8().constData());
            return false;
        }
        std::printf("saved %s (%dx%d)\n", path.toUtf8().constData(),
                    img.width(), img.height());
        return true;
    };

    bool ok = save(pin->displayedImage(), "orig");

    // 单张保存（批量保存走同一 saveTo 路径）。
    const bool savedFile =
        pin->saveTo(prefix + QStringLiteral("-saved.png"));
    std::printf("saveTo: %s\n", savedFile ? "ok" : "failed");
    ok = savedFile && ok;

    pin->rotateClockwise();
    ok = save(pin->displayedImage(), "rot") && ok;

    pin->rotateCounterClockwise();  // 转回
    pin->flipHorizontal();
    ok = save(pin->displayedImage(), "flip") && ok;

    pin->flipHorizontal();  // 翻回
    pin->toggleInvert();
    ok = save(pin->displayedImage(), "inv") && ok;

    // 缩放与透明度状态检查（不改变导出内容，只验证数值）。
    pin->resetTo100();
    std::printf("scale=%.2f opacity=%.0f%% inverted=%d\n", pin->scale(),
                pin->opacityPercent(), pin->inverted() ? 1 : 0);

    delete pin;
    return ok ? 0 : 1;
}

/// 自动化验证入口（M4 增补）：贴图分组。
/// 流程：组 0 建 2 张 → 切到组 1 → 组 1 建 1 张 → 切回组 0。
/// 断言每一阶段的可见数量。需要托盘控制器实例，因此由调用方注入。
/// 用法：jie2 --test-pin-groups
int runTestPinGroups(jie2::ui::TrayController& controller) {
    int failures = 0;
    const auto expect = [&failures](int actual, int wanted, const char* label) {
        if (actual == wanted) {
            std::printf("  ok  %s: %d\n", label, actual);
        } else {
            std::printf("  FAIL %s: got %d, want %d\n", label, actual, wanted);
            ++failures;
        }
    };

    std::printf("== 贴图分组验证 ==\n");
    std::printf("-- 组 0 建 2 张 --\n");
    controller.createTestPins(2);
    expect(controller.pinCount(), 2, "总贴图数");
    expect(controller.visiblePinCount(), 2, "组 0 可见数");

    std::printf("-- 切到组 1（组 0 应隐藏）--\n");
    controller.switchPinGroup(1);
    expect(controller.currentPinGroup(), 1, "当前组");
    expect(controller.visiblePinCount(), 0, "组 1 暂无 → 可见 0");

    std::printf("-- 组 1 建 1 张 --\n");
    controller.createTestPins(1);
    expect(controller.pinCount(), 3, "总贴图数");
    expect(controller.visiblePinCount(), 1, "组 1 可见数");

    std::printf("-- 切回组 0（组 1 应隐藏）--\n");
    controller.switchPinGroup(0);
    expect(controller.currentPinGroup(), 0, "当前组");
    expect(controller.visiblePinCount(), 2, "组 0 可见数");

    std::printf("-- 下一组（-1 应循环到组 1）--\n");
    controller.switchPinGroup(-1);
    expect(controller.currentPinGroup(), 1, "当前组");
    expect(controller.visiblePinCount(), 1, "组 1 可见数");

    std::printf("== %s ==\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}

/// 自动化验证入口（M6）：用 qrencode（若可用）或内置矩阵生成一张
/// 二维码图片，跑通 zbar 识别并校验结果。
/// 用法：jie2 --test-barcode
int runTestBarcode() {
    namespace core = jie2::core;
    // 内置二维码生成：内容固定为 "JIE2-BARCODE-TEST"。
    // 用 Python（若可用）或 qrencode 生成都可以，但为了自包含，
    // 这里直接内置一个小型 QR 生成器（版本 2、纠错 L、字节模式）。
    //
    // 说明：手写完整 QR 编码器不现实，这里改用更稳的做法：
    //   1) 优先调用 qrencode（Debian: qrencode 包）生成标准二维码；
    //   2) 没有 qrencode 时，退回用 zbarimg 自带的测试能力不可行 ——
    //      于是明确报告"缺少生成器"，而不是伪造一张假码自欺。
    const QString qrencode =
        QStandardPaths::findExecutable(QStringLiteral("qrencode"));
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::fprintf(stderr, "test-barcode: cannot create temp dir\n");
        return 1;
    }
    const QString pngPath = dir.filePath(QStringLiteral("qr.png"));
    const QString content = QStringLiteral("JIE2-BARCODE-TEST");

    if (qrencode.isEmpty()) {
        std::printf("skip: 未安装 qrencode（生成测试二维码用），"
                    "无法做端到端扫码验证；zbar 后端本身可正常调用。\n");
        // 退而验证"没码的图不会误报"：全白图应返回空文本。
        QImage blank(120, 120, QImage::Format_RGB888);
        blank.fill(Qt::white);
        const QImage rgba = blank.convertToFormat(QImage::Format_RGBA8888);
        core::ImageBuffer buffer(rgba.width(), rgba.height(),
                                 core::PixelFormat::RGBA8888);
        for (int y = 0; y < rgba.height(); ++y) {
            std::memcpy(buffer.scanline(y), rgba.constScanLine(y),
                        static_cast<std::size_t>(rgba.width()) * 4);
        }
        const core::OcrResult blankResult = jie2::ui::OcrRunner::scanBarcode(buffer);
        if (!blankResult.ok) {
            std::printf("blank test failed: %s\n", blankResult.error.c_str());
            return 1;
        }
        if (!blankResult.text.empty()) {
            std::printf("blank test failed: expected empty, got %s\n",
                        blankResult.text.c_str());
            return 1;
        }
        std::printf("blank-image test passed（无码图返回空文本，未误报）\n");
        return 0;
    }

    {
        QProcess process;
        process.setProgram(qrencode);
        process.setArguments({QStringLiteral("-o"), pngPath, content});
        process.start();
        if (!process.waitForFinished(10000) || process.exitCode() != 0) {
            std::fprintf(stderr, "qrencode failed: %s\n",
                         QString::fromUtf8(process.readAllStandardError())
                             .toUtf8()
                             .constData());
            return 1;
        }
    }

    const QImage qr(pngPath);
    if (qr.isNull()) {
        std::fprintf(stderr, "qrencode produced no readable image\n");
        return 1;
    }
    const QImage rgba = qr.convertToFormat(QImage::Format_RGBA8888);
    core::ImageBuffer buffer(rgba.width(), rgba.height(),
                             core::PixelFormat::RGBA8888);
    for (int y = 0; y < rgba.height(); ++y) {
        std::memcpy(buffer.scanline(y), rgba.constScanLine(y),
                    static_cast<std::size_t>(rgba.width()) * 4);
    }

    const core::OcrResult result = jie2::ui::OcrRunner::scanBarcode(buffer);
    if (!result.ok) {
        std::printf("barcode failed: [%s] %s\n", result.backend.c_str(),
                    result.error.c_str());
        return 1;
    }
    std::printf("barcode ok: backend=%s text=%s\n", result.backend.c_str(),
                result.text.c_str());
    if (QString::fromStdString(result.text).trimmed() != content) {
        std::printf("barcode verification failed: expected %s\n",
                    content.toUtf8().constData());
        return 1;
    }

    // 深色背景增强（对齐 Snipaste v2.10.6"改善深色背景下的二维码识别"）：
    // 把二维码反色（浅码深底）后提交给 scanBarcode —— 直接失败时它会在
    // 内部走"反色重试"路径；无论哪条路径命中，结果必须一致。
    {
        QImage dark = qr.convertToFormat(QImage::Format_ARGB32);
        dark.invertPixels(QImage::InvertRgb);
        const QImage darkRgba = dark.convertToFormat(QImage::Format_RGBA8888);
        core::ImageBuffer darkBuffer(darkRgba.width(), darkRgba.height(),
                                     core::PixelFormat::RGBA8888);
        for (int y = 0; y < darkRgba.height(); ++y) {
            std::memcpy(darkBuffer.scanline(y), darkRgba.constScanLine(y),
                        static_cast<std::size_t>(darkRgba.width()) * 4);
        }
        const core::OcrResult darkResult =
            jie2::ui::OcrRunner::scanBarcode(darkBuffer);
        if (!darkResult.ok) {
            std::printf("dark-barcode failed: %s\n", darkResult.error.c_str());
            return 1;
        }
        if (QString::fromStdString(darkResult.text).trimmed() != content) {
            std::printf("dark-barcode verification failed: got %s\n",
                        darkResult.text.c_str());
            return 1;
        }
        std::printf("dark-barcode ok（浅码深底识别成功）\n");
    }
    std::printf("barcode verification passed\n");
    return 0;
}

/// 自动化验证入口（M5）：渲染一张含文字的图片，跑通 OCR 后端，
/// 并校验识别结果。用于 CI 与后端可用性诊断。
/// 用法：jie2 --test-ocr [backend]（backend 缺省为 tesseract）
int runTestOcr(const QString& backendName) {
    namespace core = jie2::core;
    Q_UNUSED(backendName);
    // 造图：白底黑字，四周留足留白。
    //
    // 两个实测教训（都曾让"看起来像 OCR 出错"，实为测试图的问题）：
    //   1. 画布或绘制区过窄会裁掉首尾字符（早期 520px 宽时丢了 H 与 3）；
    //   2. **不要用粗体**：tesseract 对粗体的大写 H 常误读为 I、尾字符
    //      偶发丢失（在容器里对同一文案做过 bold/regular 对照测试，
    //      只有 regular 稳定给出 "HELLO WORLD 123"）。
    QImage image(760, 200, QImage::Format_RGB888);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        painter.setPen(Qt::black);
        QFont font;
        font.setPixelSize(72);
        painter.setFont(font);
        painter.drawText(QRect(40, 40, 680, 120), Qt::AlignCenter,
                         QStringLiteral("HELLO WORLD 123"));
        painter.end();
    }

    // 先读标准配置文件里的 ocr.* 设置（与托盘运行时读的是同一份），
    // 再用命令行参数覆盖后端 —— 这样 --test-ocr 测的是"用户真实配置"，
    // 自定义命令 / 端点等参数都不必在命令行重复传。
    core::OcrConfig config;
    const QString configPath = jie2::ui::appConfigFile();
    {
        QFile file(configPath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray raw = file.readAll();
            const core::Config cfg = core::Config::parse(
                std::string(raw.constData(), raw.size()));
            std::string warning;
            config = core::parseOcrConfig(cfg, &warning);
        }
    }
    if (!backendName.isEmpty()) {
        bool known = true;
        config.backend = core::parseOcrBackend(backendName.toStdString(), &known);
        if (!known) {
            std::fprintf(stderr, "unknown backend: %s\n",
                         backendName.toUtf8().constData());
            return 2;
        }
    }
    // 测试图是英文：未在配置里指定语言时用 eng，避免依赖中文语言包。
    if (config.language.empty() || config.backend == core::OcrBackendKind::Tesseract) {
        config.language = "eng";
    }

    auto platform = jie2::platform::createPlatform();
    const core::ImageBuffer buffer = [&image]() {
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
        core::ImageBuffer out(rgba.width(), rgba.height(),
                              core::PixelFormat::RGBA8888);
        for (int y = 0; y < rgba.height(); ++y) {
            std::memcpy(out.scanline(y), rgba.constScanLine(y),
                        static_cast<std::size_t>(rgba.width()) * 4);
        }
        return out;
    }();

    const core::OcrResult result =
        jie2::ui::OcrRunner::run(config, buffer, platform.get());
    if (!result.ok) {
        std::printf("ocr failed: [%s] %s\n", result.backend.c_str(),
                    result.error.c_str());
        return 1;
    }
    std::printf("ocr ok: backend=%s text=%s\n", result.backend.c_str(),
                result.text.c_str());
    // 校验：不区分大小写地找 "HELLO"。识别引擎可能加空格/换行，这里宽松匹配。
    QString text = QString::fromStdString(result.text).toUpper();
    text.remove(QRegularExpression(QStringLiteral("[^A-Z0-9]")));
    if (!text.contains(QStringLiteral("HELLO"))) {
        std::printf("ocr verification failed: expected HELLO in result\n");
        return 1;
    }
    std::printf("ocr verification passed\n");
    return 0;
}

/// 自动化验证入口：程序化构造一组标注并合成到测试底图上，
/// 输出 PNG 供逐像素检查（验证"保存时的合成路径"与渲染一致）。
/// 用法：jie2 --test-annotate <out.png>
int runTestAnnotate(const QString& outPath) {
    namespace core = jie2::core;
    using core::Annotation;
    using core::AnnotationTool;

    // 底图：左半白、右半黑、底带一块噪声（供马赛克/模糊观察）。
    QImage base(400, 200, QImage::Format_RGBA8888);
    for (int y = 0; y < base.height(); ++y) {
        for (int x = 0; x < base.width(); ++x) {
            QColor c;
            if (x < 200) {
                c = QColor(255, 255, 255);
            } else {
                c = QColor(0, 0, 0);
            }
            if (y >= 150 && x >= 50 && x < 350) {
                // 棋盘噪声：马赛克与模糊的输入
                c = ((x / 4 + y / 4) % 2 == 0) ? QColor(255, 0, 0)
                                               : QColor(0, 0, 255);
            }
            base.setPixelColor(x, y, c);
        }
    }

    std::vector<Annotation> items;

    // 1. 红色矩形（白底区域上）
    {
        Annotation a;
        a.tool = AnnotationTool::Rect;
        a.points = {core::Point{20, 20}, core::Point{120, 80}};
        items.push_back(a);
    }
    // 2. 绿色椭圆
    {
        Annotation a;
        a.tool = AnnotationTool::Ellipse;
        a.style.color = {34, 197, 94, 255};
        a.points = {core::Point{140, 20}, core::Point{240, 80}};
        items.push_back(a);
    }
    // 3. 蓝色箭头（黑底区域上）
    {
        Annotation a;
        a.tool = AnnotationTool::Arrow;
        a.style.color = {59, 130, 246, 255};
        a.points = {core::Point{260, 40}, core::Point{360, 120}};
        items.push_back(a);
    }
    // 4. 马赛克覆盖噪声区左段
    {
        Annotation a;
        a.tool = AnnotationTool::Mosaic;
        a.style.blockSize = 16;
        a.points = {core::Point{55, 152}, core::Point{140, 198}};
        items.push_back(a);
    }
    // 5. 模糊覆盖噪声区中段
    {
        Annotation a;
        a.tool = AnnotationTool::Blur;
        a.style.blurRadius = 10;
        a.points = {core::Point{150, 152}, core::Point{235, 198}};
        items.push_back(a);
    }
    // 6. 文字标注（M2 增补）：白底区域上，字号 28
    {
        Annotation a;
        a.tool = AnnotationTool::Text;
        a.style.fontSize = 28;
        a.points = {core::Point{30, 110}};
        a.text = "文字";
        items.push_back(a);
    }
    // 6b. 圆角矩形（M2 增补）：白底区左下，圆角 24 → 四角不画线
    {
        Annotation a;
        a.tool = AnnotationTool::Rect;
        a.style.cornerRadius = 24;
        a.points = {core::Point{20, 150}, core::Point{140, 190}};
        items.push_back(a);
    }
    // 6c. 虚线矩形（M2 增补）：黑底区右下，虚线 → 边上应有间断
    {
        Annotation a;
        a.tool = AnnotationTool::Rect;
        a.style.dashed = true;
        a.style.color = {34, 197, 94, 255};
        a.points = {core::Point{250, 20}, core::Point{390, 90}};
        items.push_back(a);
    }
    // 7. 序号标注（M2 增补）：黑底区域上，编号 "7"（显式给文本，
    //    与 UI 层 nextNumberLabel() 的产出解耦，便于像素断言）
    {
        Annotation a;
        a.tool = AnnotationTool::Number;
        a.style.width = 3;
        a.points = {core::Point{330, 50}};
        a.text = "7";
        items.push_back(a);
    }
    // 8. 双向箭头（M2 增补，专业版 v2.5）：黑底区中部
    {
        Annotation a;
        a.tool = AnnotationTool::Arrow;
        a.style.color = {250, 204, 21, 255};  // 黄
        a.style.arrowBothEnds = true;
        a.points = {core::Point{210, 25}, core::Point{300, 45}};
        items.push_back(a);
    }
    // 9. 椭圆马赛克（M2 增补，专业版 v2.6）：噪声区右段。
    //
    // 布局说明（本图元素较多，区域必须互不重叠，否则后绘制的会盖住先绘制的，
    // 断言就会测到错误的内容 —— 这是实测踩过的坑）：
    //   矩形马赛克 x∈[55,140]，模糊 x∈[150,235]，椭圆马赛克 x∈[270,350]；
    //   放大镜的源在中带 (170,90)-(230,140)，2x 后覆盖 (140,65)-(260,165)，
    //   刻意避开了三个像素效果区的**检查点**（y=175 一线）。
    {
        Annotation a;
        a.tool = AnnotationTool::Mosaic;
        a.style.blockSize = 16;
        a.style.ellipseShape = true;
        a.points = {core::Point{270, 152}, core::Point{350, 198}};
        items.push_back(a);
    }
    // 10. 放大镜（M2 增补，专业版 v2.6）：把**跨越黑白分界**的区域放大 2x。
    //    源选 (170,90)-(230,140)：位于椭圆标注之下、像素效果区之上，
    //    横跨 x=200 的白/黑分界 —— 放大后分界清晰可断言。
    {
        Annotation a;
        a.tool = AnnotationTool::Magnifier;
        a.style.magnifierZoom = 2;
        a.points = {core::Point{170, 90}, core::Point{230, 140}};
        items.push_back(a);
    }

    const QImage composed = jie2::ui::composeAnnotations(base, items);
    if (composed.isNull() || !composed.save(outPath, "PNG")) {
        std::fprintf(stderr, "test-annotate failed: cannot save %s\n",
                     outPath.toUtf8().constData());
        return 1;
    }
    std::printf("test-annotate ok: %s (%dx%d, %zu items)\n",
                outPath.toUtf8().constData(), composed.width(),
                composed.height(), items.size());
    return 0;
}

/// SIGTERM/SIGINT 的优雅退出标志。
///
/// 为什么需要：Qt 默认**不处理** SIGTERM —— `kill` 一个 Qt 应用会直接
/// 终止进程、不跑任何析构。而本项目在析构里做两件必须做的事：
/// 保存配置、终止 jie2-line 子进程（否则它变成孤儿窗口）。
/// 处理方式：信号处理器只记标志（异步信号安全的写法），
/// 由事件循环里的定时器轮询并触发正常退出。
volatile std::sig_atomic_t g_quitSignal = 0;

void handleQuitSignal(int signalNumber) { g_quitSignal = signalNumber; }

/// 单实例通道的标识：固定前缀 + 用户名（同一台机器的不同用户互不干扰）。
QString singleInstanceId() {
    return QStringLiteral("jie2-single-") +
           qEnvironmentVariable("USER", QStringLiteral("user"));
}

/// 命令行用法（--help）。
void printUsage() {
    std::printf(
        "J2 %s —— 跨平台截图 / 标注 / 贴图工具\n"
        "\n"
        "运行实例命令（发送给已在运行的实例；无实例时报错）：\n"
        "  --empty-group            清空当前贴图分组\n"
        "  --show-group-manager     打开贴图分组管理窗口\n"
        "  （不带参数启动        = 唤起已有实例；没有实例时启动常驻模式）\n"
        "\n"
        "一次性截图（独立执行，不影响运行实例）：\n"
        "  --select-rect x,y,w,h    截取指定区域（逻辑坐标，相对目标屏）\n"
        "  --select                 打开交互式截图遮罩\n"
        "  --capture-once           抓主屏一张（自动化验证用）\n"
        "  --whiteboard [color]     白板模式：纯色画布 + 标注\n"
        "  --hold                   截图后先标注再输出（等价于 --select）\n"
        "  --block [SECONDS]        兼容参数（本 CLI 天然同步）\n"
        "\n"
        "输出选项：\n"
        "  -o, --output <path>      显式输出路径（.png / .jpg）\n"
        "  --no-save                只写剪贴板不落盘\n"
        "  --size W H               强制输出尺寸\n"
        "  --shadow / --no-shadow   阴影开关\n"
        "  --border N               边框宽度\n"
        "  --round N                圆角半径\n"
        "  --active-screen          用光标所在屏（默认主屏）\n"
        "  --barcode-decode FILE    对本地图片识别二维码/条形码\n"
        "\n"
        "自检（不需要交互）：\n"
        "  --test-overlay-promote   截图遮罩的窗口层级提升（全屏/远程桌面场景）\n"
        "  --test-annotate <out>    标注合成\n"
        "  --test-pin <prefix>      贴图变换\n"
        "  --test-ocr [backend]     文字识别\n"
        "  --test-barcode           二维码/条形码\n"
        "  --test-pin-groups        贴图分组\n"
        "\n"
        "其它：\n"
        "  --version                版本\n"
        "  --help                   本帮助\n",
        JIE2_VERSION);
}

/// 遮罩层级提升自检（--test-overlay-promote）。
///
/// 建一个与截图遮罩同类的无边框置顶窗口，交给平台层提升，然后读回
/// 真实结果。它不能证明"盖住了远程桌面窗口"（那需要人眼确认），
/// 但能证明：原生窗口句柄可解析、层级设置调用成功、不崩溃。
int runTestOverlayPromote() {
    auto platform = jie2::platform::createPlatform();
    if (!platform) {
        std::fprintf(stderr, "promote: FAIL 平台层不可用\n");
        return 1;
    }
    QWidget window;
    window.setWindowTitle(QStringLiteral("J2 overlay promote probe"));
    window.setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                          Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
    window.setGeometry(120, 120, 320, 200);
    window.show();
    QCoreApplication::processEvents();
    const WId handle = window.winId();
    const bool promoted =
        platform->promoteOverlayWindow(reinterpret_cast<void*>(handle));
    std::printf("promote: %s (handle=%llu, platform=%s)\n",
                promoted ? "ok" : "unsupported",
                static_cast<unsigned long long>(handle),
                platform->name().c_str());
    if (!promoted) {
        std::printf("说明：当前会话不允许客户端自定窗口层级"
                    "（Wayland 合成器或未知平台）；全屏/远程桌面下遮罩可能被盖住。\n");
        return 2;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Qt 初始化前处理不需要 GUI 的参数。
    bool captureOnce = false;
    bool selectMode = false;
    bool hasSelectRect = false;
    bool testPinGroups = false;  // --test-pin-groups
    bool testOverlayPromote = false;  // --test-overlay-promote
    bool activeScreen = false;   // --active-screen：用光标所在屏而非主屏
    bool whiteboard = false;     // --whiteboard [color]
    QString whiteboardColor = QStringLiteral("#ffffff");
    QRect selectRect;
    QString testAnnotatePath;
    QString testPinPrefix;
    bool testOcr = false;
    bool testBarcode = false;
    QString barcodeFile;
    // 缺省为空：此时用配置文件里的 ocr.backend（便于测自定义后端）。
    QString testOcrBackend;
    // 作用于**运行实例**的命令（走单实例通道；已实现的专业版命令行选项）。
    QString notifyCommand;

    // M6 命令行覆盖项（只影响本次调用，不写入配置）。
    jie2::ui::TrayController::CaptureOverrides overrides;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("jie2 %s\n", JIE2_VERSION);
            return 0;
        }
        // --hold：截图后先标注再输出（对齐 Snipaste 的 `snip --hold`）。
        // 本实现里等价于打开交互遮罩（遮罩内可标注，确认后才落盘）。
        if (std::strcmp(argv[i], "--hold") == 0) {
            selectMode = true;
        }
        // --block [SECONDS]：让调用方等到截图完成后再退出。
        // 本 CLI 天然同步（命令返回时截图与保存已完成），此处接受参数
        // 以保持命令行兼容；可选的秒数被忽略（不改变同步语义）。
        if (std::strcmp(argv[i], "--block") == 0) {
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                ++i;
            }
        }
        // --size W H：强制输出尺寸。
        if (std::strcmp(argv[i], "--size") == 0 && i + 2 < argc) {
            overrides.hasSize = true;
            overrides.sizeWidth = std::atoi(argv[++i]);
            overrides.sizeHeight = std::atoi(argv[++i]);
        }
        // --keep-aspect-ratio：配合 --size，等比缩放到 W×H 内（专业版 v2.8.6）。
        if (std::strcmp(argv[i], "--keep-aspect-ratio") == 0) {
            overrides.keepAspectRatio = true;
        }
        // --active-screen：用光标所在屏幕（默认主屏）。
        if (std::strcmp(argv[i], "--active-screen") == 0) {
            activeScreen = true;
        }
        // --whiteboard [color]：纯色画布 + 标注（不抓屏）。
        if (std::strcmp(argv[i], "--whiteboard") == 0) {
            whiteboard = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const QString candidate = QString::fromUtf8(argv[++i]);
                if (QColor(candidate).isValid()) {
                    whiteboardColor = candidate;
                } else {
                    std::fprintf(stderr, "invalid --whiteboard color: %s\n",
                                 candidate.toUtf8().constData());
                    return 2;
                }
            }
        }
        if ((std::strcmp(argv[i], "--output") == 0 ||
             std::strcmp(argv[i], "-o") == 0) &&
            i + 1 < argc) {
            overrides.outputPath = QString::fromUtf8(argv[++i]);
        }
        if (std::strcmp(argv[i], "--no-save") == 0) {
            overrides.noSave = true;
        }
        if (std::strcmp(argv[i], "--shadow") == 0) {
            overrides.hasShadow = true;
            overrides.shadow = true;
        }
        if (std::strcmp(argv[i], "--no-shadow") == 0) {
            overrides.hasShadow = true;
            overrides.shadow = false;
        }
        if (std::strcmp(argv[i], "--border") == 0 && i + 1 < argc) {
            overrides.hasBorder = true;
            overrides.borderWidth = std::atoi(argv[++i]);
        }
        if (std::strcmp(argv[i], "--round") == 0 && i + 1 < argc) {
            overrides.hasRound = true;
            overrides.roundRadius = std::atoi(argv[++i]);
        }
        if (std::strcmp(argv[i], "--test-annotate") == 0 && i + 1 < argc) {
            testAnnotatePath = QString::fromUtf8(argv[++i]);
        }
        if (std::strcmp(argv[i], "--test-pin") == 0 && i + 1 < argc) {
            testPinPrefix = QString::fromUtf8(argv[++i]);
        }
        if (std::strcmp(argv[i], "--test-ocr") == 0) {
            testOcr = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                testOcrBackend = QString::fromUtf8(argv[++i]);
            }
        }
        if (std::strcmp(argv[i], "--test-barcode") == 0) {
            testBarcode = true;
        }
        if (std::strcmp(argv[i], "--test-pin-groups") == 0) {
            testPinGroups = true;
        }
        if (std::strcmp(argv[i], "--test-overlay-promote") == 0) {
            testOverlayPromote = true;
        }
        // barcode-decode FILE：对给定文件扫码（专业版对齐项）。
        if (std::strcmp(argv[i], "--barcode-decode") == 0 && i + 1 < argc) {
            barcodeFile = QString::fromUtf8(argv[++i]);
        }
        // 作用于运行实例的命令（专业版命令行选项；经单实例通道送达）。
        if (std::strcmp(argv[i], "--empty-group") == 0) {
            notifyCommand = QStringLiteral("--empty-group");
        }
        if (std::strcmp(argv[i], "--show-group-manager") == 0) {
            notifyCommand = QStringLiteral("--show-group-manager");
        }
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            printUsage();
            return 0;
        }
        if (std::strcmp(argv[i], "--capture-once") == 0) {
            captureOnce = true;
        }
        if (std::strcmp(argv[i], "--select") == 0) {
            selectMode = true;
        }
        if (std::strcmp(argv[i], "--select-rect") == 0 && i + 1 < argc) {
            // 格式：x,y,w,h（逻辑坐标，相对光标所在屏幕）
            const QString text = QString::fromUtf8(argv[++i]);
            const QStringList parts = text.split(QLatin1Char(','));
            if (parts.size() == 4) {
                selectRect = QRect(parts[0].toInt(), parts[1].toInt(),
                                   parts[2].toInt(), parts[3].toInt());
                hasSelectRect = selectRect.isValid();
            }
            if (!hasSelectRect) {
                std::fprintf(stderr, "invalid --select-rect: %s\n",
                             text.toUtf8().constData());
                return 2;
            }
        }
    }

    // ---- 作用于运行实例的命令（M6 增补）----
    //
    // 这些命令只需要把消息送达运行中的实例，不需要图形会话：
    // 这里用 QCoreApplication（不碰 display），因此 SSH 到无图形环境的
    // 机器上也能把命令发给本机会话里的实例。
    if (!notifyCommand.isEmpty()) {
        QCoreApplication coreApp(argc, argv);
        const QString appId = singleInstanceId();
        if (!jie2::ui::SingleInstanceChannel::sendToRunning(
                appId, QStringList{notifyCommand})) {
            std::fprintf(stderr,
                         "J2 未在运行：没有可接收「%s」的实例。\n"
                         "先启动 J2（直接运行，无参数），再执行本命令。\n",
                         notifyCommand.toUtf8().constData());
            return 1;
        }
        std::printf("已送达：%s\n", notifyCommand.toUtf8().constData());
        return 0;
    }

    QApplication app(argc, argv);
    if (QCoreApplication::arguments().contains(QStringLiteral("--self-check"))) {
        // QApplication 初始化本身会加载平台插件；再验证图像与 HTTPS 能力。
        QImage image(8, 8, QImage::Format_ARGB32); image.fill(Qt::white);
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG") || QImage::fromData(png).isNull() || !QSslSocket::supportsSsl()) {
            std::fprintf(stderr, "self-check failed: PNG or TLS unavailable\n"); return 1;
        }
        std::printf("self-check passed: GUI, PNG, TLS\n"); return 0;
    }

    QApplication::setApplicationName(QStringLiteral("jie2"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/app/icon-256.png")));
    QApplication::setApplicationDisplayName(QStringLiteral("J2"));
    QApplication::setApplicationVersion(QStringLiteral(JIE2_VERSION));
    // 托盘常驻应用：关闭最后一个窗口不等于退出。
    QApplication::setQuitOnLastWindowClosed(false);

    // 旧版本把配置放在 macOS 的 ~/Library/Preferences（受系统保护、
    // 普通程序写不进去）。升级后先把已有配置/历史搬到新位置。
    jie2::ui::migrateLegacyAppData(
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));

    auto platform = jie2::platform::createPlatform();
    if (platform == nullptr) {
        QMessageBox::critical(nullptr, QStringLiteral("J2"),
                              QStringLiteral("平台集成层初始化失败"));
        return 1;
    }

    jie2::ui::TrayController controller(std::move(platform));
    controller.setCaptureOverrides(overrides);

    // 遮罩层级提升验证入口（远程桌面 / 全屏窗口场景）：
    // 建一个与截图遮罩同类型标志的真实窗口，调用平台层的提升接口。
    // 这是该功能唯一能在真机上自证的部分（"确实盖住了某人的全屏窗口"
    // 只能由用户目视确认，代码不可能自证）。
    if (testOverlayPromote) {
        return runTestOverlayPromote();
    }

    // 标注渲染验证入口：不需要抓屏与交互，纯图像合成。
    if (!testAnnotatePath.isEmpty()) {
        return runTestAnnotate(testAnnotatePath);
    }

    // 贴图验证入口（M4）：验证旋转/翻转/反色与状态数值。
    if (!testPinPrefix.isEmpty()) {
        return runTestPin(testPinPrefix);
    }

    // OCR 验证入口（M5）：渲染含文字图片 → 识别 → 校验。
    if (testOcr) {
        return runTestOcr(testOcrBackend);
    }

    // 条码验证入口（M6）。
    if (testBarcode) {
        return runTestBarcode();
    }

    // 贴图分组验证入口（M4 增补）：需要控制器实例。
    if (testPinGroups) {
        return runTestPinGroups(controller);
    }

    // barcode-decode FILE：对本地文件扫码（专业版对齐项）。
    if (!barcodeFile.isEmpty()) {
        namespace core = jie2::core;
        const QImage image(barcodeFile);
        if (image.isNull()) {
            std::fprintf(stderr, "barcode-decode: cannot read %s\n",
                         barcodeFile.toUtf8().constData());
            return 1;
        }
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
        core::ImageBuffer buffer(rgba.width(), rgba.height(),
                                 core::PixelFormat::RGBA8888);
        for (int y = 0; y < rgba.height(); ++y) {
            std::memcpy(buffer.scanline(y), rgba.constScanLine(y),
                        static_cast<std::size_t>(rgba.width()) * 4);
        }
        const core::OcrResult result = jie2::ui::OcrRunner::scanBarcode(buffer);
        if (!result.ok) {
            std::fprintf(stderr, "barcode-decode failed: %s\n",
                         result.error.c_str());
            return 1;
        }
        if (result.text.empty()) {
            std::printf("no barcode found\n");
            return 0;
        }
        std::printf("%s\n", result.text.c_str());
        return 0;
    }

    // 自动化验证入口：抓一次主屏、保存、退出 —— 不需要托盘与交互。
    // 用于 CI / 容器里验证"UI 层的截图链路"（含 QImage 转换与 PNG 保存）。
    if (captureOnce) {
        QString error;
        const QString path = controller.captureToFile(&error);
        if (path.isEmpty()) {
            std::fprintf(stderr, "capture-once failed: %s\n",
                         error.toUtf8().constData());
            return 1;
        }
        std::printf("capture-once ok: %s\n", path.toUtf8().constData());
        return 0;
    }

    // 白板模式（M6）：纯色画布 + 标注遮罩。
    if (whiteboard) {
        QString error;
        const QString path =
            controller.whiteboardWithSelection(QColor(whiteboardColor), &error);
        if (path.isEmpty()) {
            if (error.isEmpty()) {
                std::printf("cancelled\n");
                return 0;
            }
            std::fprintf(stderr, "whiteboard failed: %s\n",
                         error.toUtf8().constData());
            return 1;
        }
        std::printf("saved: %s\n", path.toUtf8().constData());
        return 0;
    }

    // 脚本化截图：给定选区直接截，不弹遮罩。
    // 供自动化验证与 M6 命令行调用使用；与交互路径共用同一条裁剪链路。
    if (hasSelectRect) {
        QString error;
        // --active-screen：用光标所在屏；默认主屏（保持既有行为）。
        QScreen* targetScreen = QGuiApplication::primaryScreen();
        if (activeScreen) {
            if (QScreen* cursorScreen = QGuiApplication::screenAt(QCursor::pos())) {
                targetScreen = cursorScreen;
            }
        }
        const QString path =
            controller.captureRectToFile(selectRect, targetScreen, &error);
        if (path.isEmpty()) {
            std::fprintf(stderr, "select-rect failed: %s\n",
                         error.toUtf8().constData());
            return 1;
        }
        std::printf("saved: %s\n", path.toUtf8().constData());
        return 0;
    }

    // 交互式截图验证入口：直接打开选择遮罩，不进入托盘循环。
    // 用于人工与自动化（xdotool / 本机操作）验证选择 → 裁剪 → 保存链路。
    if (selectMode) {
        QString error;
        const QString path = controller.captureWithSelection(&error);
        if (path.isEmpty()) {
            if (error.isEmpty()) {
                std::printf("cancelled\n");
                return 0;
            }
            std::fprintf(stderr, "select failed: %s\n", error.toUtf8().constData());
            return 1;
        }
        std::printf("saved: %s\n", path.toUtf8().constData());
        return 0;
    }

    // ---- 常驻模式：单实例引导（M6 增补）----
    //
    // 先尝试把"唤起"消息送给已有实例；送不到就自己成为主实例。
    // 这样同一时间只有一个常驻托盘（二次启动只唤起），
    // 而 --empty-group / --show-group-manager 也有一个稳定的投递目标。
    const QString appId = singleInstanceId();
    if (jie2::ui::SingleInstanceChannel::sendToRunning(appId, QStringList{})) {
        std::printf("J2 已在运行（已唤起现有实例）\n");
        return 0;
    }
    jie2::ui::SingleInstanceChannel channel;
    if (!channel.listen(appId)) {
        // 竞争兜底：与另一个"刚刚启动、还没开始监听"的实例撞上了。
        // 给它一小段时间，再试一次送达。
        QThread::msleep(150);
        if (jie2::ui::SingleInstanceChannel::sendToRunning(appId, QStringList{})) {
            std::printf("J2 已在运行（已唤起现有实例）\n");
            return 0;
        }
        std::fprintf(stderr, "J2 启动失败：单实例通道不可用（%s）\n",
                     channel.lastError().toUtf8().constData());
        return 1;
    }
    channel.setCommandHandler([&controller](const QStringList& args) {
        if (args.contains(QStringLiteral("--empty-group"))) {
            controller.emptyCurrentPinGroup();
        } else if (args.contains(QStringLiteral("--show-group-manager"))) {
            controller.showPinGroupManager();
        } else {
            controller.activateExistingInstance();
        }
    });

    if (!controller.start()) {
        return 1;
    }

    // 优雅退出：kill / Ctrl+C 时走正常退出路径（析构 → 停子进程、存配置）。
    std::signal(SIGTERM, handleQuitSignal);
    std::signal(SIGINT, handleQuitSignal);
    QTimer signalWatch;
    QObject::connect(&signalWatch, &QTimer::timeout, []() {
        if (g_quitSignal != 0) {
            qInfo().noquote()
                << QStringLiteral("[jie2] 收到退出信号 %1，正在优雅退出…")
                       .arg(static_cast<int>(g_quitSignal));
            QCoreApplication::quit();
        }
    });
    signalWatch.start(200);

    const int exitCode = QApplication::exec();
    return exitCode;
}
