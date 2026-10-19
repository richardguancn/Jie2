#pragma once
#include <QDialog>
#include <map>
#include "core/config.h"

class QFormLayout;
class QLabel;
class QListWidget;
class QStackedWidget;
namespace jie2::ui {
// 设置窗口只编辑草稿，调用方在接受后统一保存并更新运行中的功能。
class SettingsDialog : public QDialog {
public:
    explicit SettingsDialog(const core::Config& config, QWidget* parent = nullptr);
    core::Config configuration() const;
    static core::Config readConfig(const QString& path);
    static bool writeConfig(const QString& path, const core::Config& config, QString* error);
private:
    core::Config original_;
    std::map<std::string, QWidget*> fields_;
    QLabel* error_ = nullptr;
    QListWidget* navigation_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    void applyAppearance();
    void showFieldError(const std::string& key, const QString& message);
    void addText(QFormLayout* form, const char* key, const QString& label,
                 const QString& fallback = {}, bool secret = false);
    void addBool(QFormLayout* form, const char* key, const QString& label, bool fallback);
    void addNumber(QFormLayout* form, const char* key, const QString& label,
                   int fallback, int minimum, int maximum);
    void addChoice(QFormLayout* form, const char* key, const QString& label,
                   const QStringList& labels, const QStringList& values, const QString& fallback);
    void validateAndAccept();
};
}
