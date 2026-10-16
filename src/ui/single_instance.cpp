#include "ui/single_instance.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

namespace jie2::ui {

namespace {

/// 构造一条 JSON 消息（含换行结束符）。
QByteArray encodeCommand(const QStringList& args) {
    QJsonArray array;
    for (const QString& arg : args) {
        array.append(arg);
    }
    QJsonObject object;
    object.insert(QStringLiteral("args"), array);
    QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    payload.append('\n');
    return payload;
}

/// 解析收到的 JSON 消息；失败返回 false（消息被丢弃，不猜）。
bool decodeCommand(const QByteArray& payload, QStringList* args) {
    const QJsonDocument document = QJsonDocument::fromJson(payload.trimmed());
    if (!document.isObject()) {
        return false;
    }
    const QJsonArray array = document.object().value(QStringLiteral("args")).toArray();
    args->clear();
    for (const auto& value : array) {
        args->append(value.toString());
    }
    return true;
}

} // namespace

SingleInstanceChannel::SingleInstanceChannel(QObject* parent) : QObject(parent) {}

bool SingleInstanceChannel::sendToRunning(const QString& appId,
                                          const QStringList& args) {
    QLocalSocket socket;
    socket.connectToServer(appId, QIODevice::WriteOnly);
    if (!socket.waitForConnected(500)) {
        return false;
    }
    socket.write(encodeCommand(args));
    const bool written = socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(200);
    }
    return written;
}

bool SingleInstanceChannel::listen(const QString& appId) {
    lastError_.clear();

    // 第一次直接 listen。
    // 若存在**上次异常退出遗留的 socket 文件**，listen 会失败；此时才清理重试。
    // 顺序刻意如此：先 listen 再清（而不是先清）——避免把一个**活跃**实例的
    // socket 文件误删（那会让它的监听路径失效，此后谁也连不上它）。
    if (server_.listen(appId)) {
        connect(&server_, &QLocalServer::newConnection, this,
                &SingleInstanceChannel::handleNewConnection);
        return true;
    }

    if (server_.serverError() == QAbstractSocket::AddressInUseError) {
        lastError_ = QStringLiteral("地址被占用（可能存在残留 socket）");
    } else {
        lastError_ = QStringLiteral("listen 失败：%1").arg(server_.errorString());
    }

    // 清理残留后重试一次。
    QLocalServer::removeServer(appId);
    if (server_.listen(appId)) {
        connect(&server_, &QLocalServer::newConnection, this,
                &SingleInstanceChannel::handleNewConnection);
        lastError_.clear();
        return true;
    }
    lastError_ = QStringLiteral("listen 重试失败：%1").arg(server_.errorString());
    return false;
}

void SingleInstanceChannel::setCommandHandler(
    std::function<void(const QStringList&)> handler) {
    handler_ = std::move(handler);
}

void SingleInstanceChannel::handleNewConnection() {
    while (server_.hasPendingConnections()) {
        QLocalSocket* socket = server_.nextPendingConnection();
        if (socket == nullptr) {
            continue;
        }
        // 消息很小（<1KB），按"等到断开或读完一行"处理即可。
        // 用 lambda 捕获 socket 自身，读完即删。
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            if (socket->canReadLine()) {
                readCommand(socket);
            }
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            // 有些平台（或发送方立即断开）不会先给 readyRead；
            // 断开时兜底读一次未消费的数据。
            if (socket->bytesAvailable() > 0) {
                readCommand(socket);
            }
            socket->deleteLater();
        });
    }
}

void SingleInstanceChannel::readCommand(QLocalSocket* socket) {
    const QByteArray payload = socket->readAll();
    QStringList args;
    if (!decodeCommand(payload, &args)) {
        qWarning().noquote() << QStringLiteral("[jie2] 单实例通道收到无法解析的消息，已丢弃");
        return;
    }
    qInfo().noquote() << QStringLiteral("[jie2] 单实例通道收到命令：%1")
                             .arg(args.isEmpty() ? QStringLiteral("(唤起)")
                                                 : args.join(QLatin1Char(' ')));
    if (handler_) {
        handler_(args);
    }
}

} // namespace jie2::ui
