#include "ui/settings_dialog.h"
#include <QCheckBox>
#include <QAction>
#include <QFontDatabase>
#include <QListWidget>
#include <QStackedWidget>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QGuiApplication>
#include <QStyleHints>
#include "ui/theme.h"
#include <QColor>
#include "core/shortcut_map.h"
#include "core/key_names.h"
#include <QKeyEvent>
#include <QFocusEvent>
#include "platform/hotkey.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdio>

namespace jie2::ui {
core::Config SettingsDialog::readConfig(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return core::Config::parse(file.readAll().toStdString());
}
bool SettingsDialog::writeConfig(const QString& path, const core::Config& config, QString* error) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("无法创建设置目录");
        return false;
    }
    const auto bytes = QByteArray::fromStdString(config.serialize());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) &&
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (file.write(bytes) == bytes.size() && file.commit()) {
            return true;
        }
        if (error) *error = file.errorString();
        file.cancelWriting();
    } else if (error != nullptr) {
        *error = file.errorString();
    }
    // 回退路径：临时文件 + 替换。
    //
    // QSaveFile 在打开时检查**目标文件本身**是否可写，被系统标为不可写的
    // 既有文件（实测：~/Library/Preferences 下的配置文件被 macOS 保护后
    // isWritable() 返回 false，尽管目录可写）会直接拒绝，而实际上"写临时
    // 文件再替换"只需要目录写权限。设置保存不能因为这种环境状态而失败，
    // 否则用户在设置里改的快捷键等全部存不下来。
    const QString temporary = path + QStringLiteral(".tmp");
    QFile plain(temporary);
    if (!plain.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = plain.errorString();
        return false;
    }
    plain.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const bool written = plain.write(bytes) == bytes.size();
    plain.close();
    if (!written) {
        plain.remove();
        if (error != nullptr) *error = QStringLiteral("写入设置临时文件失败");
        return false;
    }
#if defined(Q_OS_WIN)
    QFile::remove(path);  // Windows 的 rename 不覆盖已存在文件
    const bool replaced = QFile::rename(temporary, path);
#else
    const bool replaced = std::rename(
        QFile::encodeName(temporary).constData(),
        QFile::encodeName(path).constData()) == 0;
#endif
    if (!replaced) {
        QFile::remove(temporary);
        if (error != nullptr) *error = QStringLiteral("无法替换设置文件：%1").arg(path);
        return false;
    }
    return true;
}
namespace {
// 按键录入不经过文本输入法，也不把 Enter/Esc/Tab 交给对话框。
class ShortcutEdit final : public QLineEdit {
public:
    ShortcutEdit(const QString& value, bool global, QWidget* parent = nullptr)
        : QLineEdit(value, parent), global_(global) {
        setReadOnly(true);
        setPlaceholderText("点击后按键");
        setToolTip("点击后直接按下单键或组合键");
        setContextMenuPolicy(Qt::NoContextMenu);
        if (!global) {
            QPixmap icon(32, 32); icon.fill(Qt::transparent);
            { QPainter painter(&icon); painter.setRenderHint(QPainter::Antialiasing);
              painter.setPen(QPen(QColor("#8e8e93"), 2.4, Qt::SolidLine, Qt::RoundCap));
              painter.drawLine(10, 10, 22, 22); painter.drawLine(10, 22, 22, 10); }
            icon.setDevicePixelRatio(2);
            auto* clear = addAction(QIcon(icon), QLineEdit::TrailingPosition);
            clear->setToolTip("禁用此快捷键");
            connect(clear, &QAction::triggered, this, [this] { setText("none"); });
        }
    }
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->isAutoRepeat()) return true;
            int key = keyEvent->key();
            if (key == Qt::Key_Control || key == Qt::Key_Shift ||
                key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr) return true;
            if (key == Qt::Key_Enter) key = Qt::Key_Return;
            auto mods = keyEvent->modifiers();
            // 全局热键绑定物理符号键；Shift 后的字符还原为该键本身。
            if (global_) {
                const QString shifted = "_+{}|:\"<>?~";
                const QString base = "-=[]\\;',./`";
                const int index = shifted.indexOf(QChar(key));
                if (index >= 0) { key = base[index].unicode(); mods |= Qt::ShiftModifier; }
            }
            const auto name = core::keyText(key);
            if (name.empty()) { setToolTip("此按键暂不支持，请选择其他按键"); return true; }
            QString value;
#ifdef Q_OS_MACOS
            const bool ctrl = mods.testFlag(Qt::MetaModifier);
            const bool meta = mods.testFlag(Qt::ControlModifier);
#else
            const bool ctrl = mods.testFlag(Qt::ControlModifier);
            const bool meta = mods.testFlag(Qt::MetaModifier);
#endif
            if (ctrl) value += "ctrl+";
            if (mods.testFlag(Qt::AltModifier)) value += "alt+";
            if (mods.testFlag(Qt::ShiftModifier)) value += "shift+";
            if (meta) value += "cmd+";
            value += QString::fromStdString(name);
            if (global_ && !platform::parseHotkeyText(value.toStdString(), nullptr)) {
                setToolTip("此按键暂不支持全局使用，请选择其他按键"); return true;
            }
            setText(value); setToolTip("点击后直接按下单键或组合键");
            event->accept(); return true;
        }
        return QLineEdit::event(event);
    }
    void focusInEvent(QFocusEvent* event) override {
        QLineEdit::focusInEvent(event); selectAll();
    }
private:
    bool global_;
};

// 控件保留原有 Qt 语义和键盘操作；绘制只调整开关外观。
class SettingsSwitch final : public QCheckBox {
public:
    explicit SettingsSwitch(QWidget* parent = nullptr) : QCheckBox(parent) {
        setFixedSize(38, 28); setCursor(Qt::PointingHandCursor);
    }
protected:
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        QColor track = isChecked() ? QColor("#007aff") : QColor(dark ? "#626268" : "#d3d3d8");
        if (!isEnabled()) track.setAlpha(100);
        painter.setPen(Qt::NoPen); painter.setBrush(track);
        painter.drawRoundedRect(QRectF(1, 3, 36, 22), 11, 11);
        painter.setBrush(isEnabled() ? QColor(Qt::white) : QColor("#d4d4d8"));
        painter.drawEllipse(QRectF(isChecked() ? 17 : 3, 5, 18, 18));
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor("#78b9ff"), 2));
            painter.drawRoundedRect(QRectF(0, 1, 38, 26), 13, 13);
        }
    }
};

QIcon settingsIcon(int kind) {
    const QColor colors[] = {QColor("#8e8e93"), QColor("#007aff"), QColor("#ff9500"),
                             QColor("#af52de"), QColor("#30a46c"), QColor("#007aff")};
    QPixmap pixmap(56, 56); pixmap.fill(Qt::transparent);
    QPainter p(&pixmap); p.setRenderHint(QPainter::Antialiasing); p.scale(2, 2);
    p.setPen(Qt::NoPen); p.setBrush(colors[kind]); p.drawRoundedRect(QRectF(1, 1, 26, 26), 6, 6);
    p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
    if (kind == 0) {
        for (int i = 0; i < 3; ++i) {
            const qreal y = 8 + i * 6, x = i == 1 ? 17 : 11;
            p.drawLine(QPointF(7, y), QPointF(21, y));
            p.setBrush(colors[kind]); p.drawEllipse(QPointF(x, y), 2, 2); p.setBrush(Qt::NoBrush);
        }
    } else if (kind == 1) {
        p.drawLine(QPointF(9, 6), QPointF(9, 19)); p.drawLine(QPointF(6, 9), QPointF(19, 9));
        p.drawLine(QPointF(19, 9), QPointF(19, 22)); p.drawLine(QPointF(9, 19), QPointF(22, 19));
    } else if (kind == 2) {
        p.drawRoundedRect(QRectF(7, 6, 14, 16), 2, 2);
        p.drawLine(QPointF(10, 11), QPointF(18, 11)); p.drawLine(QPointF(10, 15), QPointF(16, 15));
    } else if (kind == 3) {
        p.drawRoundedRect(QRectF(5, 8, 18, 13), 2, 2);
        for (int x = 8; x <= 20; x += 4) { p.drawPoint(QPointF(x, 12)); p.drawPoint(QPointF(x, 15)); }
        p.drawLine(QPointF(10, 18), QPointF(18, 18));
    } else if (kind == 4) {
        p.drawLine(QPointF(7, 10), QPointF(7, 7)); p.drawLine(QPointF(7, 7), QPointF(10, 7));
        p.drawLine(QPointF(18, 7), QPointF(21, 7)); p.drawLine(QPointF(21, 7), QPointF(21, 10));
        p.drawLine(QPointF(7, 18), QPointF(7, 21)); p.drawLine(QPointF(7, 21), QPointF(10, 21));
        p.drawLine(QPointF(18, 21), QPointF(21, 21)); p.drawLine(QPointF(21, 21), QPointF(21, 18));
        p.drawText(QRectF(8, 7, 12, 15), Qt::AlignCenter, "A");
    } else {
        QFont font = p.font(); font.setPixelSize(13); font.setBold(true); p.setFont(font);
        p.drawText(QRectF(4, 5, 13, 15), Qt::AlignCenter, QStringLiteral("文"));
        p.drawText(QRectF(13, 12, 11, 13), Qt::AlignCenter, "A");
    }
    pixmap.setDevicePixelRatio(2); return QIcon(pixmap);
}

QWidget* settingRow(const QString& label, QWidget* control, const QString& description = {}) {
    auto* row = new QWidget; row->setObjectName("settingRow");
    auto* layout = new QHBoxLayout(row); layout->setContentsMargins(16, 11, 16, 11); layout->setSpacing(24);
    auto* copy = new QVBoxLayout; copy->setSpacing(3);
    auto* title = new QLabel(label); title->setObjectName("rowLabel"); title->setBuddy(control);
    copy->addWidget(title);
    if (!description.isEmpty()) {
        auto* help = new QLabel(description); help->setObjectName("rowDescription"); help->setWordWrap(true);
        copy->addWidget(help);
    }
    layout->addLayout(copy, 1); layout->addWidget(control, qobject_cast<QLineEdit*>(control) ? 2 : 0, Qt::AlignVCenter);
    control->setAccessibleName(label);
    return row;
}

void appendRow(QFormLayout* form, QWidget* row) {
    if (form->rowCount()) {
        auto* line = new QFrame; line->setObjectName("rowSeparator"); line->setFixedHeight(1);
        form->addRow(line);
    }
    form->addRow(row);
}
}

void SettingsDialog::addText(QFormLayout* form, const char* key, const QString& label,
                             const QString& fallback, bool secret) {
    const QString name = QString::fromUtf8(key);
    const QString value = QString::fromStdString(original_.get(key, fallback.toStdString()));
    QLineEdit* field = name.startsWith("shortcut.") || name.startsWith("hotkey.")
        ? new ShortcutEdit(value, name.startsWith("hotkey.")) : new QLineEdit(value);
    field->setObjectName(key); field->setMinimumWidth(180); field->setMaximumWidth(330);
    field->setMinimumHeight(30); field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(field, &QLineEdit::textChanged, this, [this] {
        if (error_) { error_->clear(); error_->hide(); }
        for (const auto& [key, control] : fields_) {
            if (control->property("invalid").toBool()) {
                control->setProperty("invalid", false);
                control->style()->unpolish(control); control->style()->polish(control);
            }
        }
    });
    if (secret) {
        field->setEchoMode(QLineEdit::Password);
        // 在字段内提供明确的显示/隐藏操作，不改变保存的密钥。
        QPixmap eye(36, 36); eye.fill(Qt::transparent);
        { QPainter painter(&eye); painter.setRenderHint(QPainter::Antialiasing); painter.scale(2, 2);
          painter.setPen(QPen(QColor("#8e8e93"), 1.3)); painter.setBrush(Qt::NoBrush);
          QPainterPath shape; shape.moveTo(2, 9); shape.quadTo(9, 1, 16, 9); shape.quadTo(9, 17, 2, 9);
          painter.drawPath(shape); painter.drawEllipse(QPointF(9, 9), 2.3, 2.3); }
        eye.setDevicePixelRatio(2);
        auto* reveal = field->addAction(QIcon(eye), QLineEdit::TrailingPosition);
        reveal->setToolTip("显示密钥"); reveal->setCheckable(true);
        connect(reveal, &QAction::toggled, field, [field, reveal](bool on) {
            field->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
            reveal->setToolTip(on ? "隐藏密钥" : "显示密钥");
        });
    }
    if (name.endsWith(".endpoint")) field->setPlaceholderText("https://…/v1");
    else if (name.endsWith(".model")) field->setPlaceholderText("填写模型名称");
    else if (secret) field->setPlaceholderText("填写服务密钥");
    else if (name == "output.border.color") { field->setMinimumWidth(120); field->setMaximumWidth(150); }
    else if (name.startsWith("shortcut.") || name.startsWith("hotkey.")) {
        field->setMaximumWidth(name.startsWith("hotkey.") ? 210 : 150); field->setMinimumWidth(120);

    }
    fields_[key] = field; appendRow(form, settingRow(label, field));
}
void SettingsDialog::addBool(QFormLayout* form, const char* key, const QString& label, bool fallback) {
    auto* field = new SettingsSwitch; field->setChecked(original_.getBool(key, fallback));
    field->setObjectName(key); fields_[key] = field; appendRow(form, settingRow(label, field));
}
void SettingsDialog::addNumber(QFormLayout* form, const char* key, const QString& label,
                               int fallback, int minimum, int maximum) {
    auto* field = new QSpinBox; field->setRange(minimum, maximum);
    field->setValue(original_.getInt(key, fallback)); field->setObjectName(key);
    field->setFixedWidth(130); field->setMinimumHeight(30); field->setAlignment(Qt::AlignRight);
    if (QString::fromUtf8(key).endsWith("_ms")) field->setSuffix(" 毫秒");
    else if (QString::fromUtf8(key).startsWith("output.")) field->setSuffix(" px");
    fields_[key] = field; appendRow(form, settingRow(label, field));
}
void SettingsDialog::addChoice(QFormLayout* form, const char* key, const QString& label,
                               const QStringList& labels, const QStringList& values, const QString& fallback) {
    auto* field = new QComboBox;
    for (int i = 0; i < labels.size(); ++i) field->addItem(labels[i], values[i]);
    field->setCurrentIndex(qMax(0, values.indexOf(QString::fromStdString(original_.get(key, fallback.toStdString())))));
    field->setObjectName(key); field->setMinimumHeight(30); field->setMinimumWidth(148);
    fields_[key] = field; appendRow(form, settingRow(label, field));
}
SettingsDialog::SettingsDialog(const core::Config& config, QWidget* parent)
    : QDialog(parent), original_(config) {
    setWindowTitle(QStringLiteral("J2 设置")); setObjectName("settingsDialog");
    resize(920, 700); setMinimumSize(780, 580);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont); font.setPointSizeF(13); setFont(font);
    auto* root = new QHBoxLayout(this); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(0);
    auto* sidebar = new QFrame; sidebar->setObjectName("settingsSidebar"); sidebar->setFixedWidth(202);
    auto* side = new QVBoxLayout(sidebar); side->setContentsMargins(12, 26, 12, 18); side->setSpacing(10);
    auto* brand = new QWidget; auto* brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(10, 0, 6, 18); brandLayout->setSpacing(10);
    auto* logo = new QLabel; logo->setFixedSize(36, 36); logo->setPixmap(QIcon(":/brand/app/icon-256.png").pixmap(36, 36));
    auto* brandCopy = new QVBoxLayout; brandCopy->setSpacing(2);
    auto* appName = new QLabel("J2"); appName->setObjectName("appName");
    auto* appDetail = new QLabel("让截图更顺手"); appDetail->setObjectName("mutedLabel");
    brandCopy->addWidget(appName); brandCopy->addWidget(appDetail); brandLayout->addWidget(logo); brandLayout->addLayout(brandCopy, 1);
    side->addWidget(brand);
    auto* navTitle = new QLabel("偏好设置"); navTitle->setObjectName("sidebarCaption"); side->addWidget(navTitle);
    navigation_ = new QListWidget; navigation_->setObjectName("settingsNavigation");
    navigation_->setAccessibleName("设置分类"); navigation_->setIconSize(QSize(28, 28));
    navigation_->setSpacing(3); navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    side->addWidget(navigation_, 1);
    auto* version = new QLabel(QStringLiteral("J2  %1").arg(JIE2_VERSION)); version->setObjectName("sidebarCaption"); side->addWidget(version);
    root->addWidget(sidebar);
    auto* content = new QWidget; content->setObjectName("settingsContent");
    auto* contentLayout = new QVBoxLayout(content); contentLayout->setContentsMargins(0, 0, 0, 0); contentLayout->setSpacing(0);
    pages_ = new QStackedWidget; pages_->setObjectName("settingsPages"); contentLayout->addWidget(pages_, 1);
    root->addWidget(content, 1);
    auto page = [this](const QString& name, const QString& description, int icon) {
        auto* nav = new QListWidgetItem(settingsIcon(icon), name, navigation_); nav->setSizeHint(QSize(174, 42));
        auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* body = new QWidget; body->setObjectName("settingsPage");
        auto* layout = new QVBoxLayout(body); layout->setContentsMargins(30, 30, 30, 26); layout->setSpacing(0);
        auto* title = new QLabel(name); title->setObjectName("pageTitle"); layout->addWidget(title);
        layout->addSpacing(8);
        auto* subtitle = new QLabel(description); subtitle->setObjectName("pageDescription"); subtitle->setWordWrap(true); layout->addWidget(subtitle);
        layout->addSpacing(26); layout->addStretch();
        scroll->setWidget(body); pages_->addWidget(scroll); return layout;
    };
    auto group = [](QVBoxLayout* pageLayout, const QString& heading) {
        // 组末总是保留弹性空白，短页面不会把各行拉开。
        const int end = pageLayout->count() - 1;
        auto* caption = new QLabel(heading); caption->setObjectName("sectionCaption");
        pageLayout->insertWidget(end, caption);
        auto* card = new QFrame; card->setObjectName("settingsGroup");
        auto* form = new QFormLayout(card); form->setContentsMargins(0, 0, 0, 0); form->setSpacing(0);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        pageLayout->insertWidget(end + 1, card); pageLayout->insertSpacing(end + 2, 22); return form;
    };
    auto help = [](QVBoxLayout* pageLayout, const QString& text) {
        if (pageLayout->count() > 1) {
            if (auto* gap = pageLayout->itemAt(pageLayout->count() - 2)->spacerItem()) gap->changeSize(0, 8);
        }
        auto* label = new QLabel(text); label->setObjectName("sectionHelp"); label->setWordWrap(true);
        pageLayout->insertWidget(pageLayout->count() - 1, label);
        pageLayout->insertSpacing(pageLayout->count() - 1, 22);
    };
    auto* general = page("常规", "按你的习惯，安排截图与衣架。", 0);
    addChoice(group(general, "外观"), "appearance.theme", "颜色模式", {"跟随系统", "浅色", "深色"}, {"system", "light", "dark"}, "system");
    auto* hanger = group(general, "衣架");
    addBool(hanger, "clothesline.enabled", "在屏幕顶部显示衣架", true);
    addBool(hanger, "clothesline.hang_copied", "自动收集复制的图片", true);
    addBool(hanger, "clothesline.hang_system_capture",
            "自动收集系统截图（如 ⌘⇧4、Win+Shift+S）", true);
    auto* corner = group(general, "触发角");
    addBool(corner, "trigger.enabled", "停留在屏幕角落时截图", false);
    addChoice(corner, "trigger.corners", "触发位置", {"所有角落", "左上角", "右上角", "左下角", "右下角"}, {"all", "tl", "tr", "bl", "br"}, "all");
    addNumber(corner, "trigger.dwell_ms", "停留时间", 350, 100, 5000);
    auto* output = page("截图与标注", "调整截图的输出效果和标注习惯。", 1);
    auto* effects = group(output, "输出效果");
    addBool(effects, "output.round.enabled", "圆角", false);
    addNumber(effects, "output.round.radius", "圆角半径", 8, 0, 500);
    addBool(effects, "output.border.enabled", "边框", false);
    addNumber(effects, "output.border.width", "边框宽度", 1, 1, 50);
    addText(effects, "output.border.color", "边框颜色", "#d2d2d2");
    addBool(effects, "output.shadow.enabled", "阴影", false);
    auto* annotation = group(output, "标注");
    addNumber(annotation, "annotation.palette_size", "调色板颜色数量", 6, 2, 10);
    addBool(annotation, "annotation.eraser_disable_reedit", "橡皮擦删除后不可撤销", false);
    auto* pin = page("贴图", "选择关闭、隐藏与查看缩略图的方式。", 2);
    auto* dismiss = group(pin, "键盘操作");
    addChoice(dismiss, "pin.esc_action", "Esc", {"关闭贴图", "隐藏贴图"}, {"close", "hide"}, "close");
    addChoice(dismiss, "pin.shift_esc_action", "Shift + Esc", {"隐藏贴图", "关闭贴图"}, {"hide", "close"}, "hide");
    addChoice(group(pin, "缩略图"), "pin.thumbnail_modifier", "按住并双击贴图", {"Shift", "Ctrl", "Alt"}, {"shift", "ctrl", "alt"}, "shift");
    help(pin, "隐藏的贴图可以从托盘菜单中重新显示。关闭会移除当前贴图窗口。");
    auto* shortcuts = page("快捷键", "点击输入框，直接按下单键或组合键。", 3);
    // 全局快捷键：在任何应用里都能触发（走系统级热键注册）。
    // 修饰键名称跨平台通用 —— macOS 的 Command、Windows 的 Win、
    // Linux 的 Super 都写作 cmd/win/super 之一，用户按本机叫法填写即可。
    auto* globalKeys = group(shortcuts, "全局快捷键");
    const auto hotkeyDefault = [](char key) {
        return QString::fromStdString(platform::defaultHotkeyText(key));
    };
    addText(globalKeys, "hotkey.snip", "截图", hotkeyDefault('t'));
    addText(globalKeys, "hotkey.pin", "贴图", hotkeyDefault('p'));
    addText(globalKeys, "hotkey.save_clipboard", "保存剪贴板图像",
            hotkeyDefault('c'));
    auto* keys = group(shortcuts, "标注工具");
    const QStringList names = {"文字", "序号", "橡皮擦", "放大镜", "虚线", "圆角", "椭圆", "双向箭头", "反向箭头"};
    const QStringList defaults = {"t", "n", "e", "m", "d", "r", "o", "b", "shift+b"};
    int index = 0;
    for (const auto& id : core::ShortcutMap::actionIds()) {
        addText(keys, ("shortcut." + id).c_str(), names[index], defaults[index]); ++index;
    }
    help(shortcuts, "点击输入框后直接按下所需按键，支持单键或组合键，无需按住 Ctrl 或 Command。标注工具可点击输入框右侧按钮禁用。全局快捷键会在其他应用中生效。");
    auto service = [&](const char* prefix, const QString& title, const QString& description, int icon) {
        auto* layout = page(title, description, icon);
        auto* form = group(layout, "模型服务"); const std::string base(prefix);
        addText(form, (base + ".endpoint").c_str(), "服务地址");
        addText(form, (base + ".model").c_str(), "模型名称");
        addText(form, (base + ".api_key").c_str(), "服务密钥", {}, true);
        help(layout, "使用兼容 OpenAI 的服务。地址可填写到 /v1，或完整的 /chat/completions。");
        return layout;
    };
    auto* ocr = service("ocr", "文字识别", "用大模型提取图片中的文字，识别结果会复制到剪贴板。", 4);
    help(ocr, "请选择支持图片输入的模型。发送图片前，J2 会先询问是否继续。");
    auto* translation = service("translation", "翻译", "翻译剪贴板中的文字或图片，可以单独选择服务。", 5);
    auto* language = group(translation, "翻译偏好");
    addText(language, "translation.target_language", "目标语言", "简体中文");
    help(translation, "翻译图片需要模型支持图片输入。发送文字或图片前，会先询问是否继续。");
    for (auto* layout : {ocr, translation}) help(layout, "密钥保存在当前用户的本机设置文件中，未加密。请勿分享设置文件。");

    const auto dependentRows = [this](const char* toggle, std::initializer_list<const char*> keys) {
        auto* control = qobject_cast<QCheckBox*>(fields_.at(toggle));
        for (const char* key : keys) {
            auto* row = fields_.at(key)->parentWidget();
            auto* form = qobject_cast<QFormLayout*>(row->parentWidget()->layout());
            const auto update = [form, row](bool on) {
                int index; QFormLayout::ItemRole role;
                form->getWidgetPosition(row, &index, &role);
                // 每项前面的分隔线也一起收起。
                if (index > 0) form->setRowVisible(index - 1, on);
                form->setRowVisible(row, on);
            };
            update(control->isChecked());
            connect(control, &QCheckBox::toggled, row, update);
        }
    };
    dependentRows("output.round.enabled", {"output.round.radius"});
    dependentRows("output.border.enabled", {"output.border.width", "output.border.color"});
    dependentRows("trigger.enabled", {"trigger.corners", "trigger.dwell_ms"});

    auto* footer = new QWidget; footer->setObjectName("settingsFooter");
    auto* footerLayout = new QVBoxLayout(footer); footerLayout->setContentsMargins(30, 14, 30, 18); footerLayout->setSpacing(10);
    error_ = new QLabel; error_->setObjectName("settingsError"); error_->setWordWrap(true); error_->hide(); footerLayout->addWidget(error_);
    auto* actionRow = new QHBoxLayout;
    auto* saveHint = new QLabel("保存后生效"); saveHint->setObjectName("mutedLabel"); actionRow->addWidget(saveHint); actionRow->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto* save = buttons->button(QDialogButtonBox::Save); save->setText("保存更改"); save->setObjectName("primaryButton"); save->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    actionRow->addWidget(buttons); footerLayout->addLayout(actionRow); contentLayout->addWidget(footer);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(navigation_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    navigation_->setCurrentRow(0);
    connect(qobject_cast<QComboBox*>(fields_["appearance.theme"]), &QComboBox::currentIndexChanged, this, [this] { applyAppearance(); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        const auto* theme = qobject_cast<QComboBox*>(fields_["appearance.theme"]);
        if (theme->currentData() == "system") applyAppearance();
    });
#endif
    applyAppearance();
}

void SettingsDialog::applyAppearance() {
    const auto* choice = qobject_cast<QComboBox*>(fields_["appearance.theme"]);
    const bool dark = resolveTheme(choice ? choice->currentData().toString() : "system").dark;
    QPalette colors = palette();
    colors.setColor(QPalette::Window, QColor(dark ? "#202022" : "#f7f7f9"));
    colors.setColor(QPalette::Base, QColor(dark ? "#303033" : "#ffffff"));
    colors.setColor(QPalette::Text, QColor(dark ? "#f2f2f7" : "#242426"));
    colors.setColor(QPalette::WindowText, colors.color(QPalette::Text));
    colors.setColor(QPalette::ButtonText, colors.color(QPalette::Text));
    colors.setColor(QPalette::Button, QColor(dark ? "#424246" : "#ffffff"));
    colors.setColor(QPalette::Highlight, QColor("#007aff"));
    colors.setColor(QPalette::HighlightedText, Qt::white); setPalette(colors);
    QString sheet = R"(
        QDialog#settingsDialog { background: @background; color: @text; }
        QWidget { font-size: 13px; }
        QFrame#settingsSidebar { background: @sidebar; border: none; border-right: 1px solid @divider; }
        QWidget#settingsContent, QWidget#settingsPage, QScrollArea, QStackedWidget { background: @background; border: none; }
        QLabel { background: transparent; color: @text; border: none; }
        QLabel#appName { font-size: 17px; font-weight: 600; }
        QLabel#sidebarCaption { color: @secondary; font-size: 11px; padding-left: 10px; }
        QLabel#mutedLabel { color: @secondary; font-size: 11px; }
        QListWidget#settingsNavigation { background: transparent; border: none; outline: none; }
        QListWidget#settingsNavigation::item { color: @text; border-radius: 8px; padding: 3px 8px; }
        QListWidget#settingsNavigation::item:hover { background: @hover; }
        QListWidget#settingsNavigation::item:selected { background: @selected; color: @text; }
        QLabel#pageTitle { font-size: 24px; font-weight: 600; }
        QLabel#pageDescription { color: @secondary; font-size: 13px; }
        QLabel#sectionCaption { font-size: 12px; font-weight: 500; padding: 0px 2px 9px 2px; }
        QFrame#settingsGroup { background: @group; border: 1px solid @border; border-radius: 11px; }
        QWidget#settingRow { background: transparent; border: none; }
        QFrame#rowSeparator { background: @divider; border: none; margin-left: 16px; margin-right: 16px; }
        QLabel#rowDescription, QLabel#sectionHelp { color: @secondary; font-size: 11px; }
        QLabel#sectionHelp { padding-left: 2px; padding-right: 2px; }
        QLineEdit { background: @input; color: @text; border: 1px solid @inputBorder; border-radius: 6px; padding: 0px 9px; selection-background-color: #007aff; }
        QLineEdit:focus { border: 2px solid @focus; padding: 0px 8px; }
        QLineEdit[invalid="true"] { border: 1px solid @error; }
        QSpinBox, QComboBox { background: @input; color: @text; border: 1px solid @inputBorder; border-radius: 6px; padding: 1px 9px; }
        QSpinBox:focus, QComboBox:focus { border: 1px solid @focus; }
        QComboBox QAbstractItemView { background: @group; color: @text; selection-background-color: #007aff; }
        QPushButton { background: @button; color: @text; border: 1px solid @border; border-radius: 15px; min-height: 30px; min-width: 68px; padding: 0px 10px; }
        QPushButton:hover { background: @hover; }
        QPushButton:focus { border: 2px solid @focus; }
        QPushButton#primaryButton { background: #007aff; color: white; border: none; min-width: 80px; }
        QPushButton#primaryButton:hover { background: #0069df; }
        QPushButton#primaryButton:pressed { background: #005bc3; }
        QWidget#settingsFooter { background: @background; border-top: 1px solid @divider; }
        QLabel#settingsError { color: @error; font-size: 12px; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 6px 1px; }
        QScrollBar::handle:vertical { background: @scroll; border-radius: 3px; min-height: 28px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
    )";
    const std::pair<const char*, const char*> light[] = {
        {"@background", "#f7f7f9"}, {"@sidebar", "#ededef"}, {"@text", "#242426"},
        {"@secondary", "#68686e"}, {"@divider", "#e4e4e8"}, {"@group", "#ffffff"},
        {"@border", "#e3e3e8"}, {"@inputBorder", "#dcdce1"}, {"@input", "#fafafb"},
        {"@hover", "#e5e5e9"}, {"@selected", "#dcdce2"}, {"@focus", "#70adf5"},
        {"@error", "#c9342b"}, {"@button", "#ffffff"}, {"@scroll", "#c6c6cc"}};
    const std::pair<const char*, const char*> night[] = {
        {"@background", "#202022"}, {"@sidebar", "#29292c"}, {"@text", "#f2f2f7"},
        {"@secondary", "#a0a0a8"}, {"@divider", "#3b3b40"}, {"@group", "#2c2c2f"},
        {"@border", "#3b3b40"}, {"@inputBorder", "#4a4a50"}, {"@input", "#252527"},
        {"@hover", "#404045"}, {"@selected", "#494950"}, {"@focus", "#5aa5ff"},
        {"@error", "#ff8c82"}, {"@button", "#3b3b40"}, {"@scroll", "#62626a"}};
    for (int i = 0; i < 15; ++i) {
        const auto& entry = dark ? night[i] : light[i]; sheet.replace(entry.first, entry.second);
    }
    setStyleSheet(sheet);
    for (auto* checkbox : findChildren<QCheckBox*>()) checkbox->update();
}

void SettingsDialog::showFieldError(const std::string& key, const QString& message) {
    error_->setText(message); error_->show();
    const auto entry = fields_.find(key);
    if (entry == fields_.end()) return;
    auto* field = entry->second;
    for (int i = 0; i < pages_->count(); ++i) if (pages_->widget(i)->isAncestorOf(field)) {
        navigation_->setCurrentRow(i);
        if (auto* area = qobject_cast<QScrollArea*>(pages_->widget(i))) area->ensureWidgetVisible(field, 24, 24);
        break;
    }
    field->setProperty("invalid", true); field->style()->unpolish(field); field->style()->polish(field); field->setFocus();
}
core::Config SettingsDialog::configuration() const {
    auto result = original_;
    for (const auto& [key, widget] : fields_) {
        if (auto* edit = qobject_cast<QLineEdit*>(widget)) result.set(key, edit->text().trimmed().toStdString());
        else if (auto* check = qobject_cast<QCheckBox*>(widget)) result.set(key, check->isChecked() ? "true" : "false");
        else if (auto* spin = qobject_cast<QSpinBox*>(widget)) result.set(key, std::to_string(spin->value()));
        else if (auto* choice = qobject_cast<QComboBox*>(widget)) result.set(key, choice->currentData().toString().toStdString());
    }
    result.set("ocr.backend", "openai-compatible");
    return result;
}
void SettingsDialog::validateAndAccept() {
    const auto config = configuration();
    for (const std::string prefix : {"ocr", "translation"}) {
        const QString endpoint = QString::fromStdString(config.get(prefix + ".endpoint"));
        const QString model = QString::fromStdString(config.get(prefix + ".model"));
        if (endpoint.isEmpty() && model.isEmpty() && config.get(prefix + ".api_key").empty()) continue;
        const QUrl url(endpoint);
        if (model.isEmpty() || !url.isValid() || url.host().isEmpty() ||
            (url.scheme() != "https" && url.scheme() != "http") || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
            showFieldError(prefix + (model.isEmpty() ? ".model" : ".endpoint"), (prefix == "ocr" ? QStringLiteral("文字识别") : QStringLiteral("翻译")) + "：请填写有效的服务地址和模型名称。"); return;
        }
    }
    // 全局快捷键：格式必须能被平台层解析，且三个动作之间不能重复
    // （同一组合在系统层面只能注册一次，重复会导致其中一个静默失效）。
    {
        const std::pair<const char*, const char*> globals[] = {
            {"hotkey.snip", "截图"},
            {"hotkey.pin", "贴图"},
            {"hotkey.save_clipboard", "保存剪贴板图像"},
        };
        std::vector<platform::HotkeySpec> parsedGlobals;
        std::vector<std::string> parsedNames;
        for (const auto& [key, label] : globals) {
            const std::string value = config.get(key);
            platform::HotkeySpec spec;
            if (!platform::parseHotkeyText(value, &spec)) {
                showFieldError(key, QStringLiteral("%1的全局快捷键格式无效：%2。"
                                                   "请点击输入框，直接按下所需单键或组合键。")
                                        .arg(QString::fromUtf8(label),
                                             QString::fromStdString(value)));
                return;
            }
            for (std::size_t i = 0; i < parsedGlobals.size(); ++i) {
                if (parsedGlobals[i].key == spec.key &&
                    parsedGlobals[i].modifiers == spec.modifiers) {
                    showFieldError(key, QStringLiteral("%1与%2不能使用相同的全局快捷键：%3")
                                            .arg(QString::fromUtf8(label),
                                                 QString::fromStdString(parsedNames[i]),
                                                 QString::fromStdString(value)));
                    return;
                }
            }
            parsedGlobals.push_back(spec);
            parsedNames.emplace_back(label);
        }
    }
    std::vector<std::pair<std::string, core::ShortcutSpec>> assigned;
    for (const auto& id : core::ShortcutMap::actionIds()) {
        const auto value = config.get("shortcut." + id);
        core::ShortcutSpec spec;
        if (!core::parseShortcutSpec(value, &spec)) { showFieldError("shortcut." + id, "快捷键格式无效：" + QString::fromStdString(value)); return; }
        if (!spec.key) continue;
        for (const auto& entry : assigned) {
            if (spec.key == entry.second.key && spec.modifiers == entry.second.modifiers) {
                showFieldError("shortcut." + id, "两个标注动作不能使用相同快捷键：" + QString::fromStdString(value)); return;
            }
        }
        assigned.emplace_back(id, spec);
    }
    if (!QColor(QString::fromStdString(config.get("output.border.color"))).isValid()) {
        showFieldError("output.border.color", "请填写有效的边框颜色，例如 #d2d2d2。"); return;
    }
    if (config.get("translation.target_language").empty()) { showFieldError("translation.target_language", "请填写翻译目标语言。"); return; }
    accept();
}
}
