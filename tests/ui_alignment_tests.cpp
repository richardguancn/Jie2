#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLineEdit>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QCheckBox>
#include "ui/settings_dialog.h"
#include "ui/ocr_runner.h"
#include <QSignalSpy>
#include <QClipboard>
#include <QTemporaryDir>
#include <QTimer>
#include <QMenu>
#include <QScreen>
#include <QPainter>
#include <QMessageBox>
#include <QMimeData>
#include <QScopeGuard>
#include "platform/hotkey.h"
#include "ui/tray.h"
#include "ui/clothesline.h"
#include "ui/pin_window.h"
#include "ui/selection_overlay.h"

namespace jie2::ui {
class MenuTestPlatform final : public platform::Platform {
public:
    mutable int captures = 0;
    mutable int recognitions = 0;
    std::string name() const override { return "menu-test"; }
    core::PlatformCapabilities probe() const override {
        core::PlatformCapabilities c;
        c.session = core::SessionType::MacOS;
        c.screenCapture.status = c.globalHotkey.status = c.alwaysOnTop.status =
            c.layerShell.status = c.clipboardImage.status = core::CapabilityStatus::Available;
        return c;
    }
    bool grabHotkey(const platform::HotkeySpec&, platform::HotkeyCallback, void*, std::string*) override { return true; }
    void releaseAllHotkeys() override {}
    std::vector<platform::DisplayInfo> displays() const override {
        const auto r = QGuiApplication::primaryScreen()->geometry();
        return {{"test", "test", {0, 0, r.width(), r.height()}, 1, true}};
    }
    platform::CaptureResult captureRegion(const core::Rect&) const override {
        ++captures;
        const auto r = QGuiApplication::primaryScreen()->geometry();
        core::ImageBuffer image(r.width(), r.height(), core::PixelFormat::RGBA8888);
        return platform::CaptureResult::success(std::move(image));
    }
    platform::CaptureResult captureDisplay(const std::string&) const override { return captureRegion({}); }
    platform::CaptureResult captureAll() const override { return captureRegion({}); }
    std::vector<core::Rect> windowRects() const override { return {}; }
    core::OcrResult recognizeText(const core::ImageBuffer&, const std::string&) const override {
        ++recognitions;
        return core::OcrResult::success("菜单识别验证", "system-test");
    }
};
class UiAlignmentTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty()) QVERIFY(QDir().mkpath(folder));
    }
    // 全局快捷键文本解析（设置里可自定义；macOS 用 cmd 代替 ctrl）。
    void globalHotkeyParsing() {
        using namespace jie2::platform;
        HotkeySpec spec;
        QVERIFY(parseHotkeyText("ctrl+alt+t", &spec));
        QCOMPARE(spec.modifiers, unsigned(kModCtrl | kModAlt));
        QCOMPARE(spec.key, Key::T);
        // macOS 习惯写法：cmd 落在 Super 位。
        QVERIFY(parseHotkeyText("cmd+shift+4", &spec));
        QCOMPARE(spec.modifiers, unsigned(kModSuper | kModShift));
        QCOMPARE(spec.key, Key::Num4);
        // 同一修饰键的所有通用叫法都要接受（跨平台配置可复用）。
        for (const char* name : {"cmd", "command", "super", "win", "meta"}) {
            const std::string text = std::string(name) + "+p";
            QVERIFY2(parseHotkeyText(text, &spec), text.c_str());
            QCOMPARE(spec.modifiers, unsigned(kModSuper));
            QCOMPARE(spec.key, Key::P);
        }
        QVERIFY(parseHotkeyText("control+option+c", &spec));
        QCOMPARE(spec.modifiers, unsigned(kModCtrl | kModAlt));
        QVERIFY(parseHotkeyText("  Ctrl + Alt + T ", &spec));
        QCOMPARE(spec.key, Key::T);
        QVERIFY(parseHotkeyText("ctrl+f12", &spec));
        QCOMPARE(spec.key, Key::F12);
        QVERIFY(parseHotkeyText("ctrl+f13", &spec));
        QVERIFY(parseHotkeyText("f20", &spec));
        QVERIFY(parseHotkeyText("space", &spec));
        QVERIFY(parseHotkeyText("alt+left", &spec));
        QVERIFY(!parseHotkeyText("f36", &spec));
        // 非法输入必须失败（不能解析出一个"看似可用"的组合）。
        QVERIFY(!parseHotkeyText("", &spec));
        QVERIFY(parseHotkeyText("t", &spec));
        QCOMPARE(spec.modifiers, 0u);
        QVERIFY(!parseHotkeyText("ctrl", &spec));          // 缺主键
        QVERIFY(!parseHotkeyText("ctrl+alt+t+p", &spec));  // 两个主键
        QVERIFY(!parseHotkeyText("ctrl+ctrl+t", &spec));   // 重复修饰键
        QVERIFY(!parseHotkeyText("hyper+t", &spec));       // 未知修饰键
        // 默认值必须可解析，且 macOS 默认用 Command 而不是 Ctrl。
        QVERIFY(parseHotkeyText(defaultHotkeyText('t'), &spec));
        QCOMPARE(spec.key, Key::T);
#ifdef Q_OS_MACOS
        QCOMPARE(spec.modifiers, unsigned(kModSuper | kModAlt));
        QVERIFY(QString::fromStdString(displayHotkey(spec)).contains("Cmd"));
#else
        QCOMPARE(spec.modifiers, unsigned(kModCtrl | kModAlt));
#endif
    }
    // 设置界面必须能改全局快捷键，且写错格式要有提示、不静默保存。
    void globalHotkeysEditableInSettings() {
        core::Config original;
        SettingsDialog dialog(original);
        auto field = [&](const char* name) { return dialog.findChild<QLineEdit*>(name); };
        QVERIFY(field("hotkey.snip") != nullptr);
        QVERIFY(field("hotkey.pin") != nullptr);
        QVERIFY(field("hotkey.save_clipboard") != nullptr);
        // 默认值按平台给（macOS = cmd+alt+…）。
        QVERIFY(field("hotkey.snip")->text().contains("alt"));
        field("hotkey.snip")->setText("cmd+shift+9");
        QCOMPARE(dialog.configuration().get("hotkey.snip"), std::string("cmd+shift+9"));
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        dialog.show();
        auto* save = buttons->button(QDialogButtonBox::Save);
        // 非法格式：对话框不得接受（停留在对话框并给出错误提示）。
        field("hotkey.snip")->setText("invalid");
        QTest::mouseClick(save, Qt::LeftButton);
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(dialog.findChild<QLabel*>("settingsError")->isVisible());
        // 三个动作用同一组合：也不得接受（系统层面只能注册一次）。
        field("hotkey.snip")->setText("cmd+alt+t");
        field("hotkey.pin")->setText("cmd+alt+t");
        QTest::mouseClick(save, Qt::LeftButton);
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
    void shortcutsRecordSaveAndUse() {
        SettingsDialog dialog({});
        dialog.show();
        dialog.findChild<QListWidget*>("settingsNavigation")->setCurrentRow(3);
        auto* field = dialog.findChild<QLineEdit*>("hotkey.snip");
        QTest::mouseClick(field, Qt::LeftButton);
        QTest::keyClick(field, Qt::Key_F8);
        QCOMPARE(field->text(), QString("f8"));
        for (const auto key : {Qt::Key_Space, Qt::Key_Left, Qt::Key_Tab,
                               Qt::Key_Return, Qt::Key_Escape, Qt::Key_Backspace,
                               Qt::Key_Delete, Qt::Key_Comma, Qt::Key_F20}) {
            QTest::keyClick(field, key);
            platform::HotkeySpec parsed;
            QVERIFY(platform::parseHotkeyText(field->text().toStdString(), &parsed));
            QCOMPARE(parsed.modifiers, 0u);
            QVERIFY(dialog.isVisible());
        }
        QTest::keyClick(field, Qt::Key_F8, Qt::AltModifier);
        QCOMPARE(field->text(), QString("alt+f8"));
        QTest::keyClick(field, Qt::Key_Plus, Qt::ShiftModifier);
        QCOMPARE(field->text(), QString("shift+equal"));
#ifdef Q_OS_MACOS
        QTest::keyClick(field, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(field->text(), QString("cmd+k"));
        QTest::keyClick(field, Qt::Key_K, Qt::MetaModifier);
        QCOMPARE(field->text(), QString("ctrl+k"));
#endif
        QTest::keyClick(field, Qt::Key_F8);
        auto* local = dialog.findChild<QLineEdit*>("shortcut.tool.text");
        QTest::keyClick(local, Qt::Key_Left);
        QCOMPARE(local->text(), QString("left"));
        QTemporaryDir temp;
        QTest::mouseClick(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save), Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QString error;
        QVERIFY(SettingsDialog::writeConfig(temp.filePath("settings.conf"), dialog.configuration(), &error));
        const auto stored = SettingsDialog::readConfig(temp.filePath("settings.conf"));
        SettingsDialog reopened(stored);
        QCOMPARE(reopened.findChild<QLineEdit*>("hotkey.snip")->text(), QString("f8"));
        SelectionOverlay::Options options;
        options.shortcuts = core::ShortcutMap::fromConfig(stored, nullptr);
        QPixmap image(320, 240); image.fill(Qt::white);
        SelectionOverlay overlay(image, 1, nullptr, options);
        overlay.show();
        QTest::keyClick(&overlay, Qt::Key_Left);
        QCOMPARE(overlay.currentTool_, core::AnnotationTool::Text);
        QVERIFY(overlay.isVisible());
        core::ShortcutSpec tab; QVERIFY(core::parseShortcutSpec("tab", &tab));
        overlay.shortcuts_.set("tool.text", tab);
        overlay.currentTool_ = core::AnnotationTool::Arrow;
        QTest::keyClick(&overlay, Qt::Key_Tab);
        QCOMPARE(overlay.currentTool_, core::AnnotationTool::Text);
        local->actions().first()->trigger();
        QCOMPARE(local->text(), QString("none"));
        // 数字不应被旧的固定工具选择提前吃掉。
        core::ShortcutSpec digit; QVERIFY(core::parseShortcutSpec("3", &digit));
        overlay.shortcuts_.set("tool.text", digit);
        QTest::keyClick(&overlay, Qt::Key_3);
        QCOMPARE(overlay.currentTool_, core::AnnotationTool::Text);
    }
    void simplifiedSettings() {
        SettingsDialog dialog({}); dialog.show();
        QVERIFY(!dialog.findChild<QSpinBox*>("ocr.timeout_ms"));
        QVERIFY(!dialog.findChild<QSpinBox*>("translation.timeout_ms"));
        auto* nav = dialog.findChild<QListWidget*>("settingsNavigation");
        nav->setCurrentRow(1);
        auto* radius = dialog.findChild<QSpinBox*>("output.round.radius");
        QVERIFY(!radius->isVisible());
        auto* round = dialog.findChild<QCheckBox*>("output.round.enabled");
        QTest::mouseClick(round, Qt::LeftButton);
        QVERIFY(radius->isVisible());
        radius->setValue(36);
        QTest::mouseClick(round, Qt::LeftButton);
        QVERIFY(!radius->isVisible());
        QCOMPARE(dialog.configuration().getInt("output.round.radius"), 36);
    }

    // 遮罩显示时必须请求平台层提升层级（远程桌面 / 全屏窗口场景）：
    // 不提升的话，用户表现为"按了热键非要先切一下窗口才看到遮罩"。
    void overlayRequestsWindowPromotion() {
        QPixmap image(320, 240); image.fill(Qt::white);
        int calls = 0;
        void* lastHandle = nullptr;
        SelectionOverlay::Options options;
        options.promoteWindow = [&](void* handle) {
            ++calls;
            lastHandle = handle;
            return true;
        };
        SelectionOverlay overlay(image, 1, nullptr, options);
        overlay.show();
        QCoreApplication::processEvents();
        // showEvent 里同步调一次 + 下一轮事件循环再补一次（焦点建立较晚）。
        QTRY_VERIFY_WITH_TIMEOUT(calls >= 2, 2000);
        QVERIFY(lastHandle != nullptr);  // 句柄必须有效，否则平台层无法定位窗口
    }
    // 系统截图（macOS Cmd+Shift+4 等）直接落盘：必须自动挂上衣架。
    void systemCaptureHangsOnClothesline() {
        QTemporaryDir shots;
        QVERIFY(shots.isValid());
        qputenv("JIE2_SYSTEM_CAPTURE_DIRS", shots.path().toLocal8Bit());
        const auto restoreEnv = qScopeGuard([] {
            qunsetenv("JIE2_SYSTEM_CAPTURE_DIRS");
        });
        // 启动前就存在的历史截图（模拟用户桌面上早就有的图）。
        const QString oldShot = shots.filePath("Screenshot 2020-01-01 at 09.00.00.png");
        {
            QImage image(40, 30, QImage::Format_ARGB32);
            image.fill(Qt::darkGreen);
            QVERIFY(image.save(oldShot));
        }
        QTemporaryDir temp;
        auto platform = std::make_unique<MenuTestPlatform>();
        TrayController controller(std::move(platform));
        controller.configPath_ = temp.filePath("config");
        controller.clotheslineEnabled_ = false;
        controller.clipboardWatchEnabled_ = false;
        controller.triggerEnabled_ = false;
        controller.setupTriggerCorner();
        const auto initialHistoryCount = controller.history_.entries().size();
        QVERIFY(controller.start());
        QVERIFY(controller.systemCaptureWatcher_ != nullptr);
        // 配置文件不存在时也必须拿到可用的默认全局快捷键
        // （回归点：默认值曾因 loadConfig 提前 return 而留成空规格）。
        QCOMPARE(controller.snipHotkeySpec_.key, platform::Key::T);
        QVERIFY((controller.snipHotkeySpec_.modifiers & platform::kModAlt) != 0);
#ifdef Q_OS_MACOS
        QVERIFY((controller.snipHotkeySpec_.modifiers & platform::kModSuper) != 0);
#else
        QVERIFY((controller.snipHotkeySpec_.modifiers & platform::kModCtrl) != 0);
#endif
        // 启动时的扫描只登记、不挂载：旧截图不能一次性被搬上衣架。
        QCOMPARE(controller.history_.entries().size(), initialHistoryCount);
        // 非截图命名的普通图片也不该被自动挂载。
        const QString notAShot = shots.filePath("holiday.png");
        {
            QImage image(40, 30, QImage::Format_ARGB32);
            image.fill(Qt::darkBlue);
            QVERIFY(image.save(notAShot));
        }
        const std::size_t before = controller.history_.entries().size();
        // 模拟系统刚截好一张图（macOS 中文名 / 英文名都要认）。
        const QString newShot = shots.filePath("Screen Shot 2026-02-02 at 10.11.12.png");
        {
            QImage image(64, 48, QImage::Format_ARGB32);
            image.fill(Qt::darkRed);
            QVERIFY(image.save(newShot));
        }
        // 目录事件 + 300ms 去抖后由定时器扫描。
        QTRY_VERIFY_WITH_TIMEOUT(controller.history_.entries().size() > before, 5000);
        QCOMPARE(controller.history_.entries().back().path, newShot.toStdString());
        QCOMPARE(controller.history_.entries().back().width, 64);
        // 旧截图与普通图片都没被挂上。
        for (const auto& entry : controller.history_.entries()) {
            QVERIFY(entry.path != oldShot.toStdString());
            QVERIFY(entry.path != notAShot.toStdString());
        }
    }
    // 系统截图文件名识别：本地化名称与英文名称都要认，普通图片不能误认。
    void systemCaptureNameDetection() {
        QVERIFY(TrayController::looksLikeSystemCapture("Screen Shot 2026-02-02 at 10.11.12.png"));
        QVERIFY(TrayController::looksLikeSystemCapture("Screenshot 2026-02-02 101112.png"));
        QVERIFY(TrayController::looksLikeSystemCapture("截屏2026-02-02 10.11.12.png"));
        QVERIFY(TrayController::looksLikeSystemCapture("屏幕快照 2026-02-02 上午10.11.12.png"));
        QVERIFY(TrayController::looksLikeSystemCapture("Screenshot (3).png"));
        QVERIFY(!TrayController::looksLikeSystemCapture("holiday.png"));
        QVERIFY(!TrayController::looksLikeSystemCapture("IMG_0001.png"));
    }
    void settingsSaveReloadAndCancel() {
        QTemporaryDir temp;
        core::Config original;
        original.set("custom.keep", "preserved");
        original.set("shortcut.tool.text", "alt+t");
        SettingsDialog dialog(original);
        QCOMPARE(dialog.findChild<QStackedWidget*>("settingsPages")->count(), 6);
        auto field = [&](const char* name) { return dialog.findChild<QLineEdit*>(name); };
        field("ocr.endpoint")->setText("https://example.invalid/v1");
        field("ocr.model")->setText("vision-model");
        field("ocr.api_key")->setText("test-key-not-real");
        QCOMPARE(field("ocr.api_key")->echoMode(), QLineEdit::Password);
        field("translation.endpoint")->setText("https://translate.invalid/v1/chat/completions");
        field("translation.model")->setText("translate-model");
        field("translation.target_language")->setText("英语");
        dialog.findChild<QSpinBox*>("output.round.radius")->setValue(24);
        dialog.findChild<QComboBox*>("pin.esc_action")->setCurrentIndex(1);
        dialog.show(); QTest::qWait(30);
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        const QString path = temp.filePath("nested/jie2.conf");
        QString error;
        QVERIFY2(SettingsDialog::writeConfig(path, dialog.configuration(), &error), qPrintable(error));
        auto stored = SettingsDialog::readConfig(path);
        QCOMPARE(stored.get("ocr.api_key"), std::string("test-key-not-real"));
        QCOMPARE(stored.get("ocr.backend"), std::string("openai-compatible"));
        QCOMPARE(stored.get("custom.keep"), std::string("preserved"));
        QCOMPARE(stored.get("translation.target_language"), std::string("英语"));
        {
            TrayController controller(std::make_unique<MenuTestPlatform>());
            controller.configPath_ = path;
            controller.loadConfig();
            QCOMPARE(controller.roundRadius_, 24);
            QCOMPARE(controller.pinEscAction_, PinWindow::DismissAction::Hide);
            controller.saveConfig(); // 菜单开关、截图计数与退出不能擦掉服务设置。
        }
        stored = SettingsDialog::readConfig(path);
        QCOMPARE(stored.get("ocr.api_key"), std::string("test-key-not-real"));
        QCOMPARE(stored.get("translation.model"), std::string("translate-model"));
        QCOMPARE(stored.get("shortcut.tool.text"), std::string("alt+t"));
        SettingsDialog reopened(stored);
        QCOMPARE(reopened.findChild<QLineEdit*>("ocr.model")->text(), QString("vision-model"));
        reopened.findChild<QLineEdit*>("ocr.api_key")->clear();
        reopened.reject();
        QCOMPARE(SettingsDialog::readConfig(path).get("ocr.api_key"), std::string("test-key-not-real"));
        QVERIFY(!SettingsDialog::writeConfig(temp.path(), stored, &error));
#ifndef Q_OS_WIN
        const auto permissions = QFileInfo(path).permissions();
        QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
#endif
    }
    void settingsApplyImmediately() {
        QTemporaryDir temp;
        TrayController controller(std::make_unique<MenuTestPlatform>());
        controller.configPath_ = temp.filePath("jie2.conf");
        QTimer::singleShot(50, [&] {
            auto* dialog = dynamic_cast<SettingsDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QLineEdit*>("ocr.endpoint")->setText("https://example.invalid/v1");
            dialog->findChild<QLineEdit*>("ocr.model")->setText("new-model");
            dialog->findChild<QLineEdit*>("ocr.api_key")->setText("fake-secret");
            dialog->findChild<QLineEdit*>("shortcut.tool.text")->setText("alt+t");
            dialog->findChild<QSpinBox*>("output.round.radius")->setValue(32);
            QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save), Qt::LeftButton);
        });
        controller.showSettings();
        QCOMPARE(controller.ocrConfig_.model, std::string("new-model"));
        QCOMPARE(controller.roundRadius_, 32);
        QVERIFY(controller.shortcutMap_.matches("tool.text", 'T', core::kScAlt));
        QTimer::singleShot(50, [&] {
            auto* dialog = dynamic_cast<SettingsDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QLineEdit*>("ocr.model")->setText("cancelled-model");
            dialog->reject();
        });
        controller.showSettings();
        QCOMPARE(controller.ocrConfig_.model, std::string("new-model"));
        QCOMPARE(SettingsDialog::readConfig(controller.configPath_).get("ocr.model"), std::string("new-model"));
    }
    void settingsValidationAndScreenshots() {
        SettingsDialog dialog({});
        auto* pages = dialog.findChild<QStackedWidget*>("settingsPages");
        auto* nav = dialog.findChild<QListWidget*>("settingsNavigation");
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        dialog.findChild<QLineEdit*>("ocr.endpoint")->setText("bad-address");
        dialog.show();
        QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(pages->currentIndex(), 4); // 出错时自动定位到对应分类。
        QVERIFY(dialog.findChild<QLabel*>("settingsError")->isVisible());
        dialog.findChild<QLineEdit*>("ocr.endpoint")->clear();
        QVERIFY(!dialog.findChild<QLabel*>("settingsError")->isVisible());
        QVERIFY(!dialog.findChild<QLineEdit*>("ocr.model")->property("invalid").toBool());
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        for (const QString theme : {QString("light"), QString("dark")}) {
            auto* appearance = dialog.findChild<QComboBox*>("appearance.theme");
            appearance->setCurrentIndex(appearance->findData(theme));
            for (const QSize size : {QSize(920, 700), QSize(780, 580)}) {
                dialog.resize(size); QTest::qWait(30);
                for (int i = 0; i < pages->count(); ++i) {
                    const QRect item = nav->visualItemRect(nav->item(i));
                    QTest::mouseClick(nav->viewport(), Qt::LeftButton, Qt::NoModifier, item.center());
                    QCOMPARE(pages->currentIndex(), i); QTest::qWait(30);
                    QVERIFY(dialog.rect().contains(buttons->mapTo(&dialog, buttons->rect().bottomRight())));
                    auto* scroll = qobject_cast<QScrollArea*>(pages->currentWidget());
                    QVERIFY(scroll);
                    QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                    if (!folder.isEmpty()) QVERIFY(dialog.grab().save(folder + QString("/settings-%1-%2-%3.png").arg(theme).arg(size.width()).arg(i)));
                }
            }
        }
        nav->setCurrentRow(0); nav->setFocus();
        QTest::keyClick(nav, Qt::Key_Down); QCOMPARE(pages->currentIndex(), 1);
        nav->setCurrentRow(4);
        auto* key = dialog.findChild<QLineEdit*>("ocr.api_key");
        key->setText("example-key");
        key->actions().first()->trigger(); QCOMPARE(key->echoMode(), QLineEdit::Normal);
        key->actions().first()->trigger(); QCOMPARE(key->echoMode(), QLineEdit::Password);
        nav->setCurrentRow(0);
        auto* toggle = dialog.findChild<QCheckBox*>("clothesline.enabled");
        const bool old = toggle->isChecked();
        QTest::mouseClick(toggle, Qt::LeftButton); QCOMPARE(toggle->isChecked(), !old);
        toggle->setFocus(); QTest::keyClick(toggle, Qt::Key_Space); QCOMPARE(toggle->isChecked(), old);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
        QVERIFY(!dialog.isVisible());
    }
    void modelServices_data() {
        QTest::addColumn<QString>("task"); QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("response"); QTest::addColumn<bool>("success");
        const QByteArray good = R"({"choices":[{"message":{"content":"HELLO 世界"}}]})";
        QTest::newRow("ocr") << "ocr" << 200 << good << true;
        QTest::newRow("auto-uses-model") << "auto" << 200 << good << true;
        QTest::newRow("translate-text") << "text" << 200 << good << true;
        QTest::newRow("translate-image") << "image" << 200 << good << true;
        QTest::newRow("authentication-failure") << "ocr" << 401 << QByteArray("{}") << false;
        QTest::newRow("malformed-response") << "ocr" << 200 << QByteArray("not-json") << false;
        QTest::newRow("missing-result") << "ocr" << 200 << QByteArray("{}") << false;
        QTest::newRow("empty-result") << "ocr" << 200 << QByteArray(R"({"choices":[{"message":{"content":" "}}]})") << false;
        QTest::newRow("timeout") << "ocr" << 0 << QByteArray() << false;
    }
    void modelServices() {
        QFETCH(QString, task); QFETCH(int, status); QFETCH(QByteArray, response); QFETCH(bool, success);
        QTcpServer server;
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));
        QByteArray request;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            auto data = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, data] {
                data->append(socket->readAll());
                const int split = data->indexOf("\r\n\r\n");
                if (split < 0) return;
                int size = -1;
                for (const auto& header : data->left(split).split('\n'))
                    if (header.toLower().startsWith("content-length:")) size = header.mid(15).trimmed().toInt();
                if (size < 0 || data->size() < split + 4 + size) return;
                request = *data;
                if (status == 0) return;
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
        });
        core::OcrConfig config;
        config.endpoint = QString("http://127.0.0.1:%1/v1%2").arg(server.serverPort()).arg(task == "text" ? "/chat/completions" : "/").toStdString();
        config.model = "test-vision"; config.apiKey = "fake-test-key"; config.timeoutMs = status == 0 ? 80 : 3000;
        MenuTestPlatform platform;
        core::ImageBuffer image(20, 10, core::PixelFormat::RGBA8888);
        QImage picture(20, 10, QImage::Format_RGBA8888); picture.fill(Qt::white);
        core::OcrResult result;
        if (task == "text" || task == "image")
            result = OcrRunner::translate(config, task == "image" ? picture : QImage(), task == "text" ? QString("原文") : QString(), "英语");
        else {
            if (task == "auto") config.backend = core::OcrBackendKind::Auto;
            result = OcrRunner::run(config, image, &platform);
        }
        QCOMPARE(result.ok, success);
        QCOMPARE(platform.recognitions, 0);
        if (!success) QVERIFY(!result.error.empty());
        else QCOMPARE(result.text, std::string("HELLO 世界"));
        QVERIFY(request.startsWith("POST /v1/chat/completions HTTP/1.1"));
        QVERIFY(request.contains("Authorization: Bearer fake-test-key"));
        const auto body = QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(body["model"].toString(), QString("test-vision"));
        const auto parts = body["messages"].toArray().first().toObject()["content"].toArray();
        QCOMPARE(parts.size(), task == "text" ? 1 : 2);
        if (task == "text") QVERIFY(parts.first().toObject()["text"].toString().contains("原文"));
        else QVERIFY(parts.last().toObject()["image_url"].toObject()["url"].toString().startsWith("data:image/png;base64,"));
        if (task == "text" || task == "image") QVERIFY(parts.first().toObject()["text"].toString().contains("英语"));
    }
    void toolbarFits_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<bool>("dark");
        QTest::newRow("desktop-dark") << 1280 << 800 << true;
        QTest::newRow("small-light") << 480 << 320 << false;
        QTest::newRow("narrow-dark") << 320 << 480 << true;
    }
    void toolbarFits() {
        QFETCH(int, width); QFETCH(int, height); QFETCH(bool, dark);
        QPixmap image(width, height); image.fill(QColor("#4f647a"));
        SelectionOverlay::Options options; options.darkTheme = dark;
        SelectionOverlay overlay(image, 1, nullptr, options);
        overlay.resize(width, height);
        overlay.frozenLogicalRect_ = overlay.rect();
        overlay.hasSelection_ = true;
        for (const QRect selection : {QRect(1, 1, 80, 50),
                                      QRect(width - 100, height - 70, 90, 60)}) {
            overlay.selectionRect_ = selection;
            const QRect bar = overlay.toolbarRect(selection);
            QVERIFY(overlay.rect().contains(bar.adjusted(-4, -4, 4, 4)));
            for (int i = 0; i < int(overlay.toolbarItems().size()); ++i) {
                const QRect button = overlay.toolbarButtonRect(selection, i);
                QVERIFY(bar.contains(button));
                int hit = -1;
                overlay.toolbarHitTest(button.center(), &hit);
                QCOMPARE(hit, i);
                for (int j = 0; j < i; ++j)
                    QVERIFY(!button.intersects(overlay.toolbarButtonRect(selection, j)));
            }
        }
        overlay.show();
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty()) {
            QDir().mkpath(folder);
            QVERIFY(overlay.grab().save(folder + QString("/toolbar-%1-%2.png").arg(width).arg(dark)));
        }
    }
    void annotateExistingImage() {
        QPixmap image(240, 160); image.fill(Qt::white);
        SelectionOverlay::Options options; options.selectImage = true;
        SelectionOverlay overlay(image, 1, nullptr, options);
        overlay.show();
        QCOMPARE(overlay.currentSelection().size(), QSize(240, 160));
        const QPoint origin = overlay.currentSelection().topLeft();
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, origin + QPoint(20, 20));
        QTest::mouseMove(&overlay, origin + QPoint(100, 80));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, origin + QPoint(100, 80));
        QCOMPARE(overlay.annotations_.size(), size_t(1));
        overlay.performUndo(); QCOMPARE(overlay.annotations_.size(), size_t(0));
        overlay.performRedo(); QCOMPARE(overlay.annotations_.size(), size_t(1));
        QTest::keyClick(&overlay, Qt::Key_Return);
        QVERIFY(overlay.outcome().accepted);
        QCOMPARE(overlay.outcome().outputImage.size(), QSize(240, 160));
        const QImage result = overlay.outcome().outputImage;
        QVERIFY(result.pixelColor(50, 20) != QColor(Qt::white));
        QCOMPARE(result.pixelColor(150, 100), QColor(Qt::white));
    }
    void toolbarDoubleClickDoesNotSave() {
        QPixmap image(240, 160); image.fill(Qt::white);
        SelectionOverlay::Options options; options.selectImage = true;
        SelectionOverlay overlay(image, 1, nullptr, options);
        overlay.show();
        const QPoint tool = overlay.toolbarButtonRect(overlay.currentSelection(), 1).center();
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, tool);
        QCOMPARE(overlay.currentTool_, core::AnnotationTool::Arrow);
        QTest::mouseDClick(&overlay, Qt::LeftButton, Qt::NoModifier, tool);
        QVERIFY(!overlay.outcome().accepted);
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QVERIFY(!overlay.outcome().accepted);
        QVERIFY(overlay.outcome().outputImage.isNull());
    }
    void largeImageKeepsResolution() {
        QPixmap image(1600, 1200); image.fill(Qt::yellow);
        SelectionOverlay::Options options; options.selectImage = true;
        SelectionOverlay overlay(image, 1, nullptr, options);
        QCOMPARE(overlay.selectionToPixelRegion(overlay.currentSelection()).size(), QSize(1600, 1200));
        overlay.commit(true);
        QCOMPARE(overlay.outcome().outputImage.size(), QSize(1600, 1200));
    }
    void clotheslineGestures() {
        QTemporaryDir temp;
        QImage image(640, 480, QImage::Format_RGB32); image.fill(Qt::cyan);
        const QString path = temp.filePath("sample.png"); QVERIFY(image.save(path));
        Clothesline line;
        line.setEntries({{path.toStdString(), "20261016", 640, 480}});
        line.revealForTest(QRect(0, 0, 800, 600));
        QTest::qWait(600);
        const QPoint center = line.cardRectF(0).center().toPoint();
        QSignalSpy copied(&line, &Clothesline::copyRequested);
        QSignalSpy opened(&line, &Clothesline::openRequested);
        QSignalSpy annotated(&line, &Clothesline::annotateRequested);
        QTest::mouseClick(&line, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(copied.count(), 1);
        QTest::mouseDClick(&line, Qt::LeftButton, Qt::NoModifier, center);
        QTest::mouseRelease(&line, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(opened.count(), 1); QCOMPARE(copied.count(), 1);
        QTest::mousePress(&line, Qt::LeftButton, Qt::NoModifier, center);
        // 长按阈值 450ms：用等待式断言而不是"睡 520ms 再比" ——
        // CI 的 runner 负载高时定时器会晚到，硬计时会把正常行为判成失败
        // （macOS 作业确实因此挂过一次）。
        QTRY_COMPARE_WITH_TIMEOUT(annotated.count(), 1, 3000);
        QTest::mouseRelease(&line, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(copied.count(), 1);
        QCOMPARE(annotated.at(0).at(0).toString(), path);
        // 释放、隐藏、数据更新都必须取消尚未到期的长按。
        QTest::mousePress(&line, Qt::LeftButton, Qt::NoModifier, center);
        line.setEntries({{path.toStdString(), "20261016", 640, 480}});
        QTest::qWait(500); QCOMPARE(annotated.count(), 1);
        QTest::mousePress(&line, Qt::LeftButton, Qt::NoModifier, center);
        line.hideNow(); QTest::qWait(500); QCOMPARE(annotated.count(), 1);
        QVERIFY(QFileInfo::exists(path));
    }
    void cropHandlesResizeAndMove() {
        QPixmap image(640, 480); image.fill(Qt::white);
        SelectionOverlay overlay(image, 1, nullptr);
        overlay.hasSelection_ = true; overlay.selectionRect_ = QRect(100, 100, 200, 160);
        overlay.show();
        // 右边中点扩大宽度，不改变高度。
        QPoint handle = overlay.cropHandles()[3];
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, handle);
        QTest::mouseMove(&overlay, handle + QPoint(40, 0));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, handle + QPoint(40, 0));
        QCOMPARE(overlay.currentSelection(), QRect(100, 100, 240, 160));
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(180, 180));
        QTest::mouseMove(&overlay, QPoint(210, 210));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(210, 210));
        QCOMPARE(overlay.currentSelection(), QRect(130, 130, 240, 160));
        QCOMPARE(overlay.annotations_.size(), size_t(0));
    }
    // 选定标注工具后，八手柄仍然可以调整选区（而不是开始画标注）——
    // 用户反馈"截图时没办法重新调整截图的尺寸"，这里锁定该路径。
    void cropHandlesWorkAfterToolChosen() {
        QPixmap image(640, 480); image.fill(Qt::white);
        SelectionOverlay overlay(image, 1, nullptr);
        overlay.hasSelection_ = true; overlay.selectionRect_ = QRect(100, 100, 200, 160);
        overlay.toolChosen_ = true;  // 已选过标注工具
        overlay.show();
        // 右下角手柄：右下各扩 30/20，左上角不动。
        const QPoint handle = overlay.cropHandles()[4];
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, handle);
        QTest::mouseMove(&overlay, handle + QPoint(30, 20));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, handle + QPoint(30, 20));
        QCOMPARE(overlay.currentSelection(), QRect(100, 100, 230, 180));
        QCOMPARE(overlay.annotations_.size(), size_t(0));  // 没有误生成标注
    }
    // 手柄命中区必须明显大于手柄视觉尺寸（5.5px 半径），否则用户难以发现。
    void cropHandleHitAreaIsGenerous() {
        QPixmap image(640, 480); image.fill(Qt::white);
        SelectionOverlay overlay(image, 1, nullptr);
        overlay.hasSelection_ = true; overlay.selectionRect_ = QRect(100, 100, 200, 160);
        const QPoint handle = overlay.cropHandles()[0];
        // 距手柄中心 12（曼哈顿）仍可命中；30 之外不命中。
        QCOMPARE(overlay.cropHandleAt(handle + QPoint(7, 5)), 0);
        QCOMPARE(overlay.cropHandleAt(handle + QPoint(30, 0)), -1);
    }
    void actionButtonsAreDistinct() {
        for (const auto action : {SelectionOverlay::Action::SaveFile,
                                 SelectionOverlay::Action::Copy, SelectionOverlay::Action::Pin}) {
            QPixmap image(240, 160); image.fill(Qt::white);
            SelectionOverlay::Options options; options.selectImage = true;
            SelectionOverlay overlay(image, 1, nullptr, options);
            overlay.show();
            const int index = action == SelectionOverlay::Action::SaveFile ? 11 :
                              action == SelectionOverlay::Action::Copy ? 12 : 10;
            QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier,
                              overlay.toolbarButtonRect(overlay.currentSelection(), index).center());
            QVERIFY(overlay.outcome().accepted);
            QCOMPARE(overlay.outcome().action, action);
            QCOMPARE(overlay.outcome().outputImage.size(), QSize(240, 160));
        }
    }
    void clotheslineWindowDoesNotDrift() {
        QTemporaryDir temp;
        QImage image(400, 240, QImage::Format_RGB32); image.fill(Qt::darkBlue);
        const QString path = temp.filePath("sample.png"); QVERIFY(image.save(path));
        Clothesline line;
        line.setEntries({{path.toStdString(), "", 400, 240}});
        const QRect screen = QGuiApplication::primaryScreen()->geometry();
        for (int cycle = 0; cycle < 3; ++cycle) {
            line.revealForTest(screen);
            QTest::qWait(80);
            const QPoint position = line.pos();
            const double initialOffset = line.contentOffsetY_;
            bool changed = false;
            for (int frame = 0; frame < 50; ++frame) {
                QTest::qWait(16);
                QCOMPARE(line.pos(), position);
                changed |= qAbs(line.contentOffsetY_ - initialOffset) > 1;
            }
            QVERIFY(changed);
            QTRY_VERIFY(qAbs(line.contentOffsetY_) < 0.5);
            qInfo() << "固定窗口第" << cycle + 1 << "轮：位置" << line.pos() << "内容偏移" << line.contentOffsetY_;
            line.conceal();
            QTRY_VERIFY(!line.isVisible());
            QCOMPARE(line.pos(), position);
        }
    }
    void revealDwellAndLeaveDelay() {
        Clothesline line;
        const QRect area(0, 0, 800, 600);
        line.updateRevealForPointer(QPoint(400, 1), area, 1000);
        line.updateRevealForPointer(QPoint(400, 1), area, 1200);
        QVERIFY(!line.isRevealed());
        line.updateRevealForPointer(QPoint(400, 1), area, 1250);
        QVERIFY(line.isRevealed());
        line.updateRevealForPointer(QPoint(400, 500), area, 1500);
        line.updateRevealForPointer(QPoint(400, 500), area, 1999);
        QVERIFY(line.isRevealed());
        line.updateRevealForPointer(QPoint(400, 1), area, 2000);
        line.updateRevealForPointer(QPoint(400, 500), area, 2100);
        line.updateRevealForPointer(QPoint(400, 500), area, 2600);
        QVERIFY(!line.isRevealed());
    }
    void menuMatchesReference() {
        QTemporaryDir temp;
        const QString path = temp.filePath("sample.png");
        QImage image(240, 160, QImage::Format_RGB32); image.fill(Qt::blue); QVERIFY(image.save(path));
        Clothesline line; line.setManagedDirectory(temp.path());
        auto* menu = line.createContextMenu(path);
        QStringList labels;
        for (const auto* action : menu->actions()) if (!action->isSeparator()) labels << action->text();
        QCOMPARE(labels.size(), 6);
        QCOMPARE(labels.at(0), QStringLiteral("复制"));
        QCOMPARE(labels.at(1), QStringLiteral("打开"));
        QCOMPARE(labels.at(2), QStringLiteral("标注"));
        QVERIFY(labels.at(3).contains(QStringLiteral("显示")));
        QCOMPARE(labels.at(4), QStringLiteral("保存到桌面"));
        QCOMPARE(labels.at(5), QStringLiteral("丢弃"));
        QSignalSpy saved(&line, &Clothesline::saveDesktopRequested);
        menu->actions().at(4)->trigger(); QCOMPARE(saved.count(), 1);
        delete menu;
        line.setManagedDirectory(QString());
        menu = line.createContextMenu(path);
        QCOMPARE(menu->actions().last()->text(), QStringLiteral("从衣架取下（保留文件）"));
        delete menu;
    }
    void clotheslineContextMenuClick_data() {
        QTest::addColumn<int>("row");
        for (int row : {-1, 0, 1, 2, 3, 4, 6})
            QTest::newRow(qPrintable(QString::number(row))) << row;
    }
    void clotheslineContextMenuClick() {
        QFETCH(int, row);
        QTemporaryDir temp;
        const QString path = temp.filePath("menu-sample.png");
        QImage image(320, 180, QImage::Format_RGB32);
        image.fill(Qt::blue); QVERIFY(image.save(path));
        Clothesline line;
        line.setManagedDirectory(temp.path());
        line.setEntries({{path.toStdString(), "", 320, 180}});
        QSignalSpy copied(&line, &Clothesline::copyRequested);
        QSignalSpy opened(&line, &Clothesline::openRequested);
        QSignalSpy annotated(&line, &Clothesline::annotateRequested);
        QSignalSpy revealed(&line, &Clothesline::revealRequested);
        QSignalSpy saved(&line, &Clothesline::saveDesktopRequested);
        QSignalSpy discarded(&line, &Clothesline::discardRequested);
        const QList<QSignalSpy*> spies{&copied, &opened, &annotated, &revealed, &saved, &discarded};
        line.revealForTest(QGuiApplication::primaryScreen()->geometry());
        QTRY_VERIFY(qAbs(line.contentOffsetY_) < 0.5);
        bool clicked = false;
        QTimer::singleShot(100, &line, [&] {
            auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!popup || row >= popup->actions().size()) return;
            clicked = true;
            const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
            if (row == 0 && !folder.isEmpty())
                popup->grab().save(folder + "/clothesline-click-menu.png");
            if (row < 0) { QTest::keyClick(popup, Qt::Key_Escape); return; }
            QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier,
                              popup->actionGeometry(popup->actions().at(row)).center());
        });
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &line, [&] {
            for (auto* menu : line.findChildren<QMenu*>()) menu->close();
        });
        timeout.start(2000);
        QTest::mouseClick(&line, Qt::RightButton, Qt::NoModifier,
                          line.cardRectF(0).center().toPoint());
        QVERIFY(clicked);
        for (int i = 0; i < spies.size(); ++i) {
            QCOMPARE(spies[i]->count(), i == (row == 6 ? 5 : row) ? 1 : 0);
            if (!spies[i]->isEmpty()) QCOMPARE(spies[i]->first().first().toString(), path);
        }
        QVERIFY(!line.menuOpen_);
        QVERIFY(QFileInfo::exists(path));
        line.hideNow();
    }
    void referencePreviews() {
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (folder.isEmpty()) return;
        QDir().mkpath(folder);
        QPixmap wallpaper(598, 366);
        { QPainter painter(&wallpaper); QLinearGradient g(0, 0, 598, 366);
          g.setColorAt(0, QColor("#020536")); g.setColorAt(.6, QColor("#1535aa"));
          g.setColorAt(1, QColor("#040a3f")); painter.fillRect(wallpaper.rect(), g); }
        SelectionOverlay overlay(wallpaper, 1, nullptr);
        overlay.resize(598, 366); overlay.frozenLogicalRect_ = overlay.rect();
        overlay.hasSelection_ = true; overlay.selectionRect_ = QRect(339, 111, 140, 117);
        overlay.show();
        QVERIFY(overlay.grab().save(folder + "/snipaste-aligned.png"));
        QTemporaryDir temp;
        std::vector<core::HistoryEntry> entries;
        const QSize sizes[] = {QSize(600,100),QSize(340,300),QSize(400,260),QSize(600,180),QSize(300,440),QSize(360,400),QSize(320,400)};
        for (int i = 0; i < 7; ++i) {
            QImage image(sizes[i], QImage::Format_RGB32); image.fill(i % 2 ? QColor("#eef1f5") : QColor("#202426"));
            QPainter painter(&image); painter.setPen(i % 2 ? QColor("#63738b") : QColor("#929e95"));
            for (int y = 16; y < image.height() - 10; y += 16) painter.drawLine(14, y, image.width() - 25 - (y % 3) * 10, y);
            painter.end();
            const QString path = temp.filePath(QString::number(i) + ".png"); QVERIFY(image.save(path));
            entries.push_back({path.toStdString(), "", image.width(), image.height()});
        }
        Clothesline line; line.setEntries(entries); line.revealForTest(QRect(0, 0, 1920, 600));
        QTest::qWait(1000);
        QImage preview(1920, 210, QImage::Format_RGB32); preview.fill(QColor("#9bc5dd"));
        QPainter painter(&preview); line.render(&painter); painter.end();
        QVERIFY(preview.save(folder + "/clothesline-aligned.png"));
        line.setManagedDirectory(temp.path());
        auto* menu = line.createContextMenu(QString::fromStdString(entries.front().path));
        menu->ensurePolished(); menu->adjustSize();
        QVERIFY(menu->grab().save(folder + "/clothesline-menu.png"));
        delete menu;

    }
    void copiedBadgeAndRevealBreeze() {
        QTemporaryDir temp;
        QImage image(320, 180, QImage::Format_RGB32); image.fill(QColor("#22252e"));
        const QString path = temp.filePath("sample.png"); QVERIFY(image.save(path));
        Clothesline line;
        line.setEntries({{path.toStdString(), "", 320, 180}, {temp.filePath("other.png").toStdString(), "", 320, 180}});
        line.revealForTest(QRect(0, 0, 800, 600));
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(line.entries_[0].swing.value) > .05, 1400);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(line.entries_[1].swing.value) > .05, 1400);
        line.showCopied(path);
        QCOMPARE(line.copiedPath_, path);
        const qint64 first = line.copiedUntilMs_;
        QTest::qWait(150);
        line.showCopied(path);
        QVERIFY(line.copiedUntilMs_ > first);
        line.setEntries({{path.toStdString(), "", 320, 180}});
        QCOMPARE(line.copiedPath_, path);
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty()) {
            QDir().mkpath(folder);
            QVERIFY(line.grab().save(folder + "/copied-feedback.png"));
        }
        QTest::qWait(1800);
        QVERIFY(line.copiedPath_.isEmpty());
        line.showCopied(path);
        line.setEntries({});
        QVERIFY(line.copiedPath_.isEmpty());
        line.hideNow();
    }
    void pinContextMenu() {
        QImage image(400, 240, QImage::Format_ARGB32); image.fill(Qt::red);
        PinWindow pin(image);
        pin.move(400, 300); pin.show();
        QSignalSpy destroyed(&pin, &PinWindow::closed);
        QSignalSpy pasted(&pin, &PinWindow::pasteRequested);
        QSignalSpy grouped(&pin, &PinWindow::groupChanged);
        auto menu = std::unique_ptr<QMenu>(pin.createContextMenu());
        auto find = [](QMenu* menu, const QString& text) -> QAction* {
            for (auto* action : menu->actions()) if (action->text() == text) return action;
            return nullptr;
        };
        QStringList labels;
        for (auto* action : menu->actions()) if (!action->isSeparator()) labels << action->text();
        QCOMPARE(labels, QStringList({"还原", "复制图像", "图像另存为…", "窗口阴影", "图像处理",
            "粘贴", "替换为文件…", "移动到分组", "在文件夹中查看", "关闭", "销毁", "400 × 240"}));
        QVERIFY(!find(menu.get(), "在文件夹中查看")->isEnabled());
        find(menu.get(), "粘贴")->trigger(); QCOMPARE(pasted.count(), 1);
        if (QGuiApplication::platformName() == "offscreen") {
            find(menu.get(), "复制图像")->trigger();
            QCOMPARE(QApplication::clipboard()->image(), image);
            QTimer::singleShot(10, &pin, [&pin] {
                for (auto* popup : pin.findChildren<QMenu*>()) if (popup->isVisible()) popup->close();
            });
            QTest::mouseClick(&pin, Qt::RightButton, Qt::NoModifier, QPoint(10, 10));
            QVERIFY(pin.isVisible()); QCOMPARE(destroyed.count(), 0);
        }
        auto* processing = find(menu.get(), "图像处理")->menu();
        find(processing, "向右旋转 90°")->trigger();
        QCOMPARE(pin.displayedImage().size(), QSize(240, 400));
        find(processing, "反色")->trigger(); QVERIFY(pin.inverted());
        pin.toggleDetailAt(QPoint(50, 60)); QVERIFY(pin.detailMode_);
        find(menu.get(), "还原")->trigger();
        QVERIFY(!pin.detailMode_); QVERIFY(!pin.inverted()); QCOMPARE(pin.size(), QSize(400, 240));
        find(find(menu.get(), "400 × 240")->menu(), "200%")->trigger();
        QCOMPARE(pin.size(), QSize(800, 480));
        pin.resetTo100(); pin.toggleThumbnail();
        QVERIFY(pin.width() < 400);
        find(find(menu.get(), "400 × 240")->menu(), "100%")->trigger();
        QCOMPARE(pin.size(), QSize(400, 240));
        const QPoint beforeShadow = pin.pos();
        find(menu.get(), "窗口阴影")->trigger();
        QVERIFY(!pin.windowFlags().testFlag(Qt::NoDropShadowWindowHint));
        QCOMPARE(pin.pos(), beforeShadow); QVERIFY(pin.isVisible());
        find(find(menu.get(), "移动到分组")->menu(), "分组 2")->trigger();
        QCOMPARE(pin.group(), 1); QCOMPARE(grouped.count(), 1);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QImage replacement(120, 80, QImage::Format_ARGB32); replacement.fill(Qt::blue);
        const QString path = temporary.filePath("replacement.png"); QVERIFY(replacement.save(path));
        QVERIFY(pin.replaceFromFile(path)); QCOMPARE(pin.size(), QSize(120, 80));
        QVERIFY(!pin.replaceFromFile(temporary.filePath("missing.png")));
        QCOMPARE(pin.sourceImage(), replacement);
        const QString exportPath = temporary.filePath("export.png");
        QVERIFY(pin.saveTo(exportPath)); QCOMPARE(QImage(exportPath), replacement);
        menu.reset(pin.createContextMenu());
        QVERIFY(find(menu.get(), "在文件夹中查看")->isEnabled());
        QVERIFY(find(menu.get(), "120 × 80"));
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty()) {
            menu->ensurePolished(); menu->adjustSize();
            QVERIFY(menu->grab().save(folder + "/pin-context-menu.png"));
        }
        find(menu.get(), "关闭")->trigger();
        QVERIFY(!pin.isVisible()); QCOMPARE(destroyed.count(), 0);
        pin.show(); QVERIFY(pin.isVisible());
        pin.toggleDetailAt(QPoint(40, 30));
        menu.reset(pin.createContextMenu());
        QVERIFY(find(menu.get(), "120 × 80"));
        find(menu.get(), "销毁")->trigger(); QCOMPARE(destroyed.count(), 1);
    }
    void pinDetail_data() {
        QTest::addColumn<QPoint>("clicked");
        QTest::newRow("off-center") << QPoint(180, 110);
        QTest::newRow("top-left") << QPoint(2, 2);
        QTest::newRow("bottom-right") << QPoint(397, 237);
    }
    void pinDetail() {
        QFETCH(QPoint, clicked);
        QImage image(400, 240, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) image.setPixelColor(x, y, QColor(x % 256, y % 256, 70));
        PinWindow pin(image); pin.move(400, 300); pin.show();
        QSignalSpy closed(&pin, &PinWindow::closed);
        const QPoint global = pin.mapToGlobal(clicked);
        QTest::mouseDClick(&pin, Qt::LeftButton, Qt::NoModifier, clicked);
        QVERIFY(pin.detailMode_); QCOMPARE(pin.size(), QSize(54, 54));
        QCOMPARE(pin.detailCenter_, QPointF(clicked));
        QCOMPARE(pin.pos() + QPoint(27, 27), global);
        QCOMPARE(pin.detailSourceRect().center(), QPointF(clicked));
        const QImage captured = pin.grab().toImage();
        const qreal dpr = captured.devicePixelRatio();
        const QColor center = captured.pixelColor(qRound(27*dpr), qRound(27*dpr));
        const QColor expected = image.pixelColor(clicked);
        QVERIFY(qAbs(center.red() - expected.red()) <= 2);
        QVERIFY(qAbs(center.green() - expected.green()) <= 2);
        QCOMPARE(closed.count(), 0);
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty() && clicked == QPoint(180, 110)) {
            QVERIFY(pin.grab().save(folder + "/pin-detail.png"));
        }
        pin.applyScaleFactor(2);
        QCOMPARE(pin.size(), QSize(54, 54));
        QCOMPARE(pin.detailSourceRect().center(), QPointF(clicked));
        QCOMPARE(pin.detailSourceRect().width(), 25.0);
        QTest::mouseDClick(&pin, Qt::LeftButton, Qt::NoModifier, QPoint(27, 27));
        QVERIFY(!pin.detailMode_); QCOMPARE(pin.size(), QSize(800, 480));
        QCOMPARE(pin.pos() + clicked * 2, global);
        QCOMPARE(pin.displayedImage(), image);
        QTest::keyClick(&pin, Qt::Key_Escape);
        QCOMPARE(closed.count(), 1);
    }
    void pinDetailRetinaAndTransforms() {
        QImage image(800, 480, QImage::Format_RGB32); image.fill(Qt::green); image.setDevicePixelRatio(2);
        PinWindow pin(image); pin.show();
        QCOMPARE(pin.size(), QSize(400, 240));
        QTest::mouseDClick(&pin, Qt::LeftButton, Qt::NoModifier, QPoint(100, 80));
        QCOMPARE(pin.detailCenter_, QPointF(200, 160));
        QCOMPARE(pin.detailSourceRect().width(), 100.0);
        pin.rotateClockwise(); QCOMPARE(pin.detailCenter_, QPointF(320, 200));
        pin.rotateCounterClockwise(); QCOMPARE(pin.detailCenter_, QPointF(200, 160));
        pin.flipHorizontal(); QCOMPARE(pin.detailCenter_, QPointF(600, 160));
        pin.flipVertical(); QCOMPARE(pin.detailCenter_, QPointF(600, 320));
        pin.resetTo100(); QVERIFY(!pin.detailMode_);
        QCOMPARE(pin.size(), QSize(400, 240));
        // 原有修饰键缩略图仍可用。
        QTest::mouseDClick(&pin, Qt::LeftButton, Qt::ShiftModifier, QPoint(100, 80));
        QVERIFY(pin.thumbnail_); QVERIFY(!pin.detailMode_);
    }
    void contextMenuKeepsFiles() {
        QTemporaryDir temp;
        QImage image(240, 160, QImage::Format_RGB32); image.fill(Qt::green);
        const QString path = temp.filePath("sample.png"); QVERIFY(image.save(path));
        Clothesline line; line.setEntries({{path.toStdString(), "", 240, 160}});
        line.revealForTest(QRect(0, 0, 800, 600)); QTest::qWait(600);
        QSignalSpy dropped(&line, &Clothesline::entryDropped);
        connect(&line, &Clothesline::discardRequested, &line, &Clothesline::takeDownPath);
        QTimer::singleShot(20, &line, [&] {
            auto* menu = line.findChild<QMenu*>();
            if (menu) {
                menu->setActiveAction(menu->actions().last());
                QTest::keyClick(menu, Qt::Key_Return);
            }
        });
        QTest::mouseClick(&line, Qt::RightButton, Qt::NoModifier, line.cardRectF(0).center().toPoint());
        QTRY_COMPARE(dropped.count(), 1);
        QCOMPARE(line.itemCount(), 0);
        QVERIFY(QFileInfo::exists(path));
    }
    void trayMenuClicks() {
        QTemporaryDir temp;
        auto platform = std::make_unique<MenuTestPlatform>();
        auto* backend = platform.get();
        TrayController controller(std::move(platform));
        controller.configPath_ = temp.filePath("config");
        controller.clotheslineEnabled_ = false;
        controller.clipboardWatchEnabled_ = false;
        controller.triggerEnabled_ = false;
        controller.setupTriggerCorner();
        QVERIFY(controller.start());
        QVERIFY(controller.clothesline_ != nullptr);
        QVERIFY(!controller.clothesline_->watchEnabled_);
        auto* clipboard = QApplication::clipboard();
        auto* backup = new QMimeData;
        if (const auto* mime = clipboard->mimeData()) {
            for (const auto& format : mime->formats())
                backup->setData(format, mime->data(format));
        }
        const auto restore = qScopeGuard([&] { clipboard->setMimeData(backup); });
        auto find = [](QMenu* menu, const QString& text) -> QAction* {
            for (auto* a : menu->actions()) if (a->text().startsWith(text)) return a;
            return nullptr;
        };
        auto open = [&] {
#ifdef Q_OS_MACOS
            controller.tray_->activated(QSystemTrayIcon::Trigger);
#else
            controller.menu_->popup(QPoint(50, 50));
#endif
        };
        auto click = [&](const QString& text) {
            open();
            QTRY_VERIFY(controller.menu_->isVisible());
            auto* action = find(controller.menu_, text);
            QVERIFY(action);
            QTest::mouseClick(controller.menu_, Qt::LeftButton, Qt::NoModifier,
                              controller.menu_->actionGeometry(action).center());
            QTRY_VERIFY(!controller.menu_->isVisible());
            QCoreApplication::processEvents();
        };
        click("衣架（");
        QTRY_VERIFY(controller.clotheslineEnabled_);
        QVERIFY(controller.clothesline_->watchEnabled_);
        controller.rebuildMenu();
        QVERIFY(find(controller.menu_, "衣架（")->isChecked());
        click("衣架（");
        QTRY_VERIFY(!controller.clotheslineEnabled_);
        controller.rebuildMenu();
        QVERIFY(!find(controller.menu_, "衣架（")->isChecked());
        click("复制图像时"); QTRY_VERIFY(controller.clipboardWatchEnabled_);
        click("复制图像时"); QTRY_VERIFY(!controller.clipboardWatchEnabled_);
        click("触发角（"); QTRY_VERIFY(controller.triggerEnabled_);
        QVERIFY(controller.cornerTimer_->isActive());
        click("触发角（"); QTRY_VERIFY(!controller.triggerEnabled_);
        QVERIFY(!controller.cornerTimer_->isActive());
        for (const QString label : {QString("圆角（"), QString("边框（"), QString("阴影")}) {
            open(); QTRY_VERIFY(controller.menu_->isVisible());
            auto* sub = find(controller.menu_, "输出装饰")->menu();
            sub->popup(controller.menu_->mapToGlobal(QPoint(controller.menu_->width(), 40)));
            QTRY_VERIFY(sub->isVisible());
            auto* action = find(sub, label); QVERIFY(action);
            const bool before = action->isChecked();
            QTest::mouseClick(sub, Qt::LeftButton, Qt::NoModifier, sub->actionGeometry(action).center());
            QCoreApplication::processEvents();
            QCOMPARE(label.startsWith("圆角") ? controller.roundEnabled_ :
                     label.startsWith("边框") ? controller.borderEnabled_ : controller.shadowEnabled_, !before);
            controller.menu_->hide();
        }
        clipboard->setText("菜单测试贴图");
        click("贴图（"); QTRY_COMPARE(controller.pinCount(), 1);
        QVERIFY(controller.pins_.front()->isVisible());
        controller.closeAllPins(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QImage image(160, 80, QImage::Format_RGB32); image.fill(Qt::white);
        clipboard->setImage(image);
        click("贴图（"); QTRY_COMPARE(controller.pinCount(), 1);
        controller.closeAllPins(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        controller.ocrConfig_.backend = core::OcrBackendKind::System;
        click("文字识别（"); QTRY_COMPARE(backend->recognitions, 1);
        QCOMPARE(clipboard->text(), QString("菜单识别验证"));
        clipboard->clear();
        for (const QString label : {QString("贴图（"), QString("文字识别（"), QString("识别二维码")}) {
            click(label);
            QTRY_VERIFY(qobject_cast<QMessageBox*>(QApplication::activeModalWidget()));
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(!box->text().isEmpty());
            QTest::keyClick(box, Qt::Key_Return);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        int overlays = 0;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, [&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (auto* overlay = qobject_cast<SelectionOverlay*>(widget); overlay && overlay->isVisible()) {
                    ++overlays;
                    dismiss.stop();
                    QTimer::singleShot(100, overlay, [overlay] {
                        QTest::keyClick(overlay, Qt::Key_Escape);
                    });
                }
                if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible())
                    QTest::keyClick(box, Qt::Key_Return);
            }
        });
        dismiss.start(20);
        if (QGuiApplication::platformName() == "cocoa") {
            // macOS 的嵌套窗口需要应用主循环；离屏运行保持 QtTest 自身循环，
            // 避免退出应用主循环后影响同进程中的其他测试。
            QTimer::singleShot(0, &controller, [&] {
                click("截图（");
                click("退出J2");
            });
            QCOMPARE(QCoreApplication::exec(), 0);
        } else {
            click("截图（");
        }
        QTRY_COMPARE(backend->captures, 1);
        QCOMPARE(overlays, 1);
        dismiss.stop();
        click("平台能力");
        QTRY_VERIFY(qobject_cast<QMessageBox*>(QApplication::activeModalWidget()));
        auto* capabilityBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(capabilityBox->text().contains("menu-test"));
        QCOMPARE(capabilityBox->icon(), QMessageBox::Information);
        QVERIFY(capabilityBox->text().contains("屏幕截图：可用"));
        QVERIFY(!capabilityBox->text().contains("**"));
        const QString capabilityFolder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!capabilityFolder.isEmpty()) QVERIFY(capabilityBox->grab().save(capabilityFolder + "/capabilities.png"));
        QTest::keyClick(capabilityBox, Qt::Key_Return);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        open(); QTRY_VERIFY(controller.menu_->isVisible());
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (!folder.isEmpty()) QVERIFY(controller.menu_->grab().save(folder + "/tray-menu.png"));
        QTest::keyClick(controller.menu_, Qt::Key_Escape);
        QTRY_VERIFY(!controller.menu_->isVisible());
    }

    /// README 配图：用真实控件离屏渲染演示图（只在设了 JIE2_UI_ARTIFACTS 时产出）。
    /// 背景是代码画出来的"桌面"，不截取任何真实屏幕内容 —— 配图要进公开仓库。
    void readmeFigures() {
        const QString folder = qEnvironmentVariable("JIE2_UI_ARTIFACTS");
        if (folder.isEmpty()) return;
        QDir().mkpath(folder);

        // ---- 造一个像桌面的背景：渐变 + 光斑 + 两个"应用窗口" + 一块图片区 ----
        const QSize logical(960, 600);
        constexpr double kScale = 2.0;   // 按 Retina 渲染，文字与线条才清晰
        QImage desktop(int(logical.width() * kScale), int(logical.height() * kScale),
                       QImage::Format_RGB32);
        {
            QPainter p(&desktop);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(kScale, kScale);
            QLinearGradient sky(0, 0, 0, logical.height());
            sky.setColorAt(0.0, QColor("#16233a"));
            sky.setColorAt(0.55, QColor("#25405c"));
            sky.setColorAt(1.0, QColor("#0f1722"));
            p.fillRect(QRectF(QPointF(0, 0), QSizeF(logical)), sky);
            for (const auto& blob : {std::tuple<qreal, qreal, qreal, const char*>{
                                         700, 120, 260, "#2f6f8f"},
                                     std::tuple<qreal, qreal, qreal, const char*>{140, 520, 300, "#1f4a63"}}) {
                QRadialGradient g(QPointF(std::get<0>(blob), std::get<1>(blob)), std::get<2>(blob));
                QColor c(std::get<3>(blob)); c.setAlpha(150);
                g.setColorAt(0.0, c); g.setColorAt(1.0, Qt::transparent);
                p.fillRect(QRectF(QPointF(0, 0), QSizeF(logical)), g);
            }
            // 浅色"文档"窗口
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 236));
            p.drawRoundedRect(QRectF(96, 74, 486, 356), 10, 10);
            p.setBrush(QColor("#e6eaf1"));
            p.drawRoundedRect(QRectF(96, 74, 486, 26), 10, 10);
            p.drawRect(QRectF(96, 88, 486, 12));
            for (int i = 0; i < 3; ++i) {
                p.setBrush(QColor(i == 0 ? "#ff5f57" : i == 1 ? "#febc2e" : "#28c840"));
                p.drawEllipse(QPointF(112 + i * 16, 87), 5, 5);
            }
            p.setBrush(QColor("#dbe1ea"));
            for (int line = 0; line < 9; ++line) {
                const qreal w = (line % 4 == 3) ? 210 : 400 - (line % 3) * 40;
                p.drawRoundedRect(QRectF(120, 122 + line * 32, w, 10), 5, 5);
            }
            // 深色"代码"窗口
            p.setBrush(QColor(18, 22, 28, 242));
            p.drawRoundedRect(QRectF(618, 96, 286, 300), 10, 10);
            p.setBrush(QColor("#242a33"));
            p.drawRoundedRect(QRectF(618, 96, 286, 24), 10, 10);
            p.drawRect(QRectF(618, 110, 286, 10));
            const char* palette[] = {"#7dd3fc", "#fca5a5", "#fcd34d", "#a7f3d0", "#c4b5fd"};
            for (int line = 0; line < 10; ++line) {
                p.setBrush(QColor(palette[line % 5]));
                const qreal w = 60 + ((line * 37) % 150);
                p.drawRoundedRect(QRectF(640 + (line % 3) * 12, 140 + line * 24, w, 8), 4, 4);
            }
            // 一块"照片"区
            QLinearGradient photo(0, 0, 1, 1);
            photo.setColorAt(0.0, QColor("#f59e0b"));
            photo.setColorAt(1.0, QColor("#ef4444"));
            p.setBrush(photo);
            p.drawRoundedRect(QRectF(120, 462, 250, 108), 8, 8);
            p.end();
        }

        // 控件截图统一按"原始像素"处理：grab() 在 Retina 上是 2 倍像素，
        // 显式把 DPR 置 1 后，尺寸即像素尺寸，合成时不会再被二次缩放
        // （踩过：保留 DPR=2 直接画到 1 倍画布上，图会被放大并溢出边界）。
        const auto grabPixels = [](QWidget* widget) {
            QPixmap shot = widget->grab();
            shot.setDevicePixelRatio(1.0);
            return shot;
        };

        // ---- 图 1：标注遮罩（选中一块区域 + 若干标注 + 工具条）----
        QImage annotatedResult;
        {
            SelectionOverlayOptions options;
            options.darkTheme = false;
            SelectionOverlay overlay(QPixmap::fromImage(desktop), kScale, nullptr, options);
            overlay.resize(logical);
            overlay.frozenLogicalRect_ = overlay.rect();
            overlay.hasSelection_ = true;
            overlay.selectionRect_ = QRect(150, 100, 520, 300);
            const auto px = [&overlay](int x, int y) {
                return core::Point{int(x * kScale), int(y * kScale)};
            };
            core::Annotation box;
            box.tool = core::AnnotationTool::Rect;
            box.style.color = {239, 68, 68, 255};
            box.style.width = 3;
            box.style.cornerRadius = 10;
            box.points = {px(196, 148), px(372, 236)};
            overlay.annotations_.add(box);
            core::Annotation arrow;
            arrow.tool = core::AnnotationTool::Arrow;
            arrow.style.color = {250, 204, 21, 255};
            arrow.style.width = 4;
            arrow.points = {px(392, 200), px(486, 250)};
            overlay.annotations_.add(arrow);
            core::Annotation text;
            text.tool = core::AnnotationTool::Text;
            text.style.color = {250, 204, 21, 255};
            text.style.fontSize = 30;
            text.points = {px(392, 138)};
            text.text = "看这里";
            overlay.annotations_.add(text);
            core::Annotation number;
            number.tool = core::AnnotationTool::Number;
            number.style.color = {239, 68, 68, 255};
            number.style.width = 3;
            number.points = {px(500, 300)};
            number.text = "1";
            overlay.annotations_.add(number);
            overlay.composedDirty_ = true;
            overlay.show();
            QVERIFY(grabPixels(&overlay).save(folder + "/selection-overlay.png"));

            // ---- 图 2：标注结果（选中区域按像素合成后的成品）----
            annotatedResult = overlay.buildFinalImage();
            QVERIFY(!annotatedResult.isNull()
                    && annotatedResult.save(folder + "/annotated-result.png"));
            overlay.hide();
        }

        // ---- 图 4：贴图窗口（把上面标注好的截图贴到桌面上，带右键菜单）----
        {
            // 贴的就是上面那张标注成品 —— 贴图的典型用法：截完标注、贴到屏幕上对照。
            QVERIFY(!annotatedResult.isNull());
            PinWindow pin(annotatedResult);
            pin.applyScaleFactor(0.4);   // 缩小，场景里窗口与菜单并排都完整可见
            pin.show();
            QTest::qWait(150);
            std::unique_ptr<QMenu> menu(pin.createContextMenu());
            menu->ensurePolished();
            menu->adjustSize();
            const QPixmap pinShot = grabPixels(&pin);
            const QPixmap menuShot = grabPixels(menu.get());
            // 全部用像素坐标：scene 是 kScale 倍画布，贴图与菜单按各自的
            // 真实像素尺寸落位，逻辑坐标乘 kScale 换算。
            const QPoint pinAt(70 * int(kScale), 110 * int(kScale));
            // 菜单放到贴图右侧留白，不压住贴图内容（真实右键也是这个位置关系）。
            const QPoint menuAt(pinAt.x() + pinShot.width() + 16 * int(kScale),
                                pinAt.y() + 30 * int(kScale));
            const int sceneHeight = std::max(460 * int(kScale),
                                             std::max(pinAt.y() + pinShot.height(),
                                                      menuAt.y() + menuShot.height()) + 24);
            // 合成场景：代码画的"桌面" + 真实贴图窗口 + 真实右键菜单。
            QImage scene(logical.width() * int(kScale), sceneHeight, QImage::Format_RGB32);
            { QPainter p(&scene);
              p.drawImage(QRect(0, 0, scene.width(), sceneHeight),
                          desktop, QRect(0, 0, desktop.width(),
                                         std::min(desktop.height(), sceneHeight)));
              p.drawPixmap(pinAt, pinShot);
              p.drawPixmap(menuAt, menuShot); }
            QVERIFY(scene.save(folder + "/pin-window.png"));
            menu.reset();
            pin.close();
        }

        // ---- 图 3：衣架（卡片取自上面这张"桌面"的局部，像真实截图缩略图）----
        {
            QTemporaryDir temp;
            std::vector<core::HistoryEntry> entries;
            const QRect crops[] = {QRect(96, 74, 486, 356), QRect(618, 96, 286, 300),
                                   QRect(120, 462, 250, 108), QRect(300, 120, 280, 200),
                                   QRect(700, 140, 220, 240)};
            int index = 0;
            for (const QRect& crop : crops) {
                const QImage thumb = desktop.copy(QRect(crop.topLeft() * kScale,
                                                        crop.size() * kScale));
                const QString path = temp.filePath(QString::number(index++) + ".png");
                QVERIFY(thumb.save(path));
                entries.push_back({path.toStdString(), "", thumb.width(), thumb.height()});
            }
            Clothesline line;
            line.setEntries(entries);
            line.setDarkTheme(false);
            line.revealForTest(QRect(0, 0, logical.width(), logical.height()));
            QTest::qWait(1200);
            const int stripHeight = 190;
            QImage canvas(logical.width() * int(kScale), stripHeight * int(kScale),
                          QImage::Format_RGB32);
            { QPainter p(&canvas);
              p.drawImage(QRect(0, 0, canvas.width(), canvas.height()),
                          desktop, QRect(0, 0, desktop.width(),
                                         stripHeight * int(kScale))); }
            { QPainter p(&canvas);
              p.scale(kScale, kScale);      // 按逻辑坐标渲染控件，输出 2 倍像素
              line.render(&p); }
            QVERIFY(canvas.save(folder + "/clothesline.png"));
            line.hideNow();
        }
    }

};
}
QTEST_MAIN(jie2::ui::UiAlignmentTest)
#include "ui_alignment_tests.moc"
