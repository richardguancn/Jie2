#pragma once

#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <QStringList>

#include <functional>

// 「J2」单实例通道（M6 增补）
//
// 存在的理由（两件事，互相依赖）：
//   1. **同一时间只有一个常驻实例** —— 二次启动不应再起一个托盘，
//      而是唤起已有实例（对齐桌面应用的普遍行为）。
//   2. **让命令作用于运行中的实例** —— `--empty-group`、
//      `--show-group-manager` 这类操作的对象是**内存里的贴图状态**，
//      只有运行中的主实例掌握它。独立进程无从下手，必须走 IPC。
//
// 协议：QLocalSocket（Unix domain socket / Windows 命名管道），
// 一条连接一条 JSON 消息、换行为结束符：
//   {"args": ["--empty-group"]}
// 空 args 表示"唤起"（二次启动的语义）。
//
// 分层说明：依赖 QtNetwork，放 UI 层（core/platform 不依赖 Qt）。

namespace jie2::ui {

class SingleInstanceChannel : public QObject {
    Q_OBJECT

public:
    explicit SingleInstanceChannel(QObject* parent = nullptr);

    /// 把一条命令发送给运行中的实例。
    /// 返回 true = 已送达（调用方应退出）；false = 没有实例在监听。
    ///
    /// 注意：本方法**不需要图形会话**（内部只用 QLocalSocket 的同步 API），
    /// 因此 SSH 到无 display 的机器上也能把命令送达本机 X/Wayland 会话里的实例。
    static bool sendToRunning(const QString& appId, const QStringList& args);

    /// 成为主实例并开始监听。失败返回 false（并由 lastError() 给出原因）。
    /// 失败后调用方可再尝试 sendToRunning —— 覆盖"两个进程同时启动"的竞争。
    bool listen(const QString& appId);

    /// 设置命令处理器（在 Qt 主线程调用）。args 为发送方给的参数列表。
    void setCommandHandler(std::function<void(const QStringList& args)> handler);

    /// 监听的完整 socket 路径（诊断用）。
    QString fullServerName() const { return server_.fullServerName(); }

    /// 最近一次 listen 失败的原因。
    QString lastError() const { return lastError_; }

private:
    void handleNewConnection();
    void readCommand(QLocalSocket* socket);

    QLocalServer server_;
    std::function<void(const QStringList&)> handler_;
    QString lastError_;
};

} // namespace jie2::ui
