#ifndef IPCSERVER_H
#define IPCSERVER_H

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGenericArgument>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QVariant>

#include "controllers/AccountController.h"

/// \brief JSON-RPC 2.0 server exposing AccountController over a Unix
///        domain socket.
///
/// Incoming requests are dispatched to AccountController public slots
/// via QMetaObject reflection. No per-method wiring: adding a controller
/// slot immediately makes it callable over IPC.
///
/// Outgoing signals from AccountController are broadcast as JSON-RPC
/// notifications (no "id"). Per-signal lambda connections wire each
/// controller signal to the shared broadcast helper. Adding a new
/// controller signal requires one connect() line here.
class IpcServer : public QObject
{
    Q_OBJECT

public:
    explicit IpcServer(AccountController *controller, QObject *parent = nullptr)
        : QObject(parent)
        , m_controller(controller)
        , m_server(new QLocalServer(this))
    {
        const QString socketPath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                 + QStringLiteral("/aerogram.ipc");
        // Fresh machines may not have the cache dir yet (listen fails
        // with "Name error" if the parent doesn't exist).
        QDir().mkpath(QFileInfo(socketPath).absolutePath());
        QLocalServer::removeServer(socketPath);

        // Same-user only — the socket carries credentials in motion.
        m_server->setSocketOptions(QLocalServer::UserAccessOption);

        connect(m_server, &QLocalServer::newConnection, this, &IpcServer::onNewConnection);

        if (!m_server->listen(socketPath))
            qWarning() << "IpcServer: failed to listen at" << socketPath << m_server->errorString();
        else
            qInfo().noquote() << "IpcServer: listening at" << socketPath;

        subscribeToControllerSignals();
    }

private slots:
    void onNewConnection()
    {
        while (auto *client = m_server->nextPendingConnection()) {
            m_clients.insert(client);
            connect(client, &QLocalSocket::readyRead, this, &IpcServer::onReadyRead);
            connect(client, &QLocalSocket::disconnected, this, &IpcServer::onDisconnected);
        }
    }

    void onReadyRead()
    {
        auto *client = qobject_cast<QLocalSocket *>(sender());
        if (!client) return;

        QByteArray &buf = m_bufs[client];
        buf.append(client->readAll());

        while (true) {
            const int idx = buf.indexOf('\n');
            if (idx < 0) break;

            const QByteArray line = buf.left(idx).trimmed();
            buf.remove(0, idx + 1);

            if (line.isEmpty()) continue;

            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
            if (err.error != QJsonParseError::NoError) {
                sendError(client, QJsonValue::Null, -32700,
                          QStringLiteral("Parse error: ") + err.errorString());
                continue;
            }

            const QJsonObject req = doc.object();
            const QJsonValue idVal = req.value("id");
            const QString method = req.value("method").toString();
            const QJsonArray params = req.value("params").toArray();

            handleRequest(client, idVal, method, params);
        }
    }

    void onDisconnected()
    {
        auto *client = qobject_cast<QLocalSocket *>(sender());
        if (!client) return;
        m_clients.remove(client);
        m_bufs.remove(client);
        client->deleteLater();
    }

private:
    // -----------------------------------------------------------------
    // Reflective incoming dispatch
    // -----------------------------------------------------------------

    void handleRequest(QLocalSocket *client, const QJsonValue &id,
                       const QString &method, const QJsonArray &params)
    {
        // Special-case: ping (health check, no controller involvement).
        if (method == QLatin1String("ping")) {
            sendResponse(client, id, QStringLiteral("pong"));
            return;
        }

        // Special-case: get_state (dump all Q_PROPERTYs as JSON).
        if (method == QLatin1String("get_state")) {
            sendResponse(client, id, controllerStateAsJson());
            return;
        }

        // Special-case: get_property (read a single Q_PROPERTY by name).
        if (method == QLatin1String("get_property")) {
            if (params.size() < 1) {
                sendError(client, id, -32602, QStringLiteral("Missing property name"));
                return;
            }
            const QString propName = params.at(0).toString();
            const QVariant v = m_controller->property(propName.toUtf8().constData());
            if (!v.isValid()) {
                sendError(client, id, -32602, QStringLiteral("No such property: ") + propName);
                return;
            }
            sendResponse(client, id, QJsonValue::fromVariant(v));
            return;
        }

        // Generic dispatch: find a public slot on the controller with
        // matching name and arity, then invoke it.
        const QMetaObject *meta = m_controller->metaObject();
        QMetaMethod matched;
        for (int i = 0; i < meta->methodCount(); ++i) {
            const QMetaMethod m = meta->method(i);
            if (m.access() != QMetaMethod::Public) continue;
            if (m.methodType() != QMetaMethod::Slot && m.methodType() != QMetaMethod::Method) continue;
            if (QString::fromLatin1(m.name()) != method) continue;
            if (m.parameterCount() != params.size()) continue;
            matched = m;
            break;
        }

        if (!matched.isValid()) {
            sendError(client, id, -32601, QStringLiteral("Method not found: ") + method);
            return;
        }

        // Convert JSON params to QVariants of the expected types.
        QVector<QVariant> stored;
        stored.reserve(matched.parameterCount());
        for (int i = 0; i < matched.parameterCount(); ++i) {
            const QMetaType type = matched.parameterMetaType(i);
            QVariant v = params.at(i).toVariant();
            if (!v.convert(type)) {
                sendError(client, id, -32602,
                          QStringLiteral("Parameter %1 type mismatch (expected %2)")
                              .arg(i).arg(QString::fromLatin1(type.name())));
                return;
            }
            stored.append(v);
        }

        // Assemble up to 5 QGenericArgument slots. Fail loudly instead
        // of silently truncating extra parameters.
        if (matched.parameterCount() > 5) {
            sendError(client, id, -32602,
                      QStringLiteral("Method has more than 5 parameters: ") + method);
            return;
        }
        QGenericArgument a[5];
        for (int i = 0; i < stored.size() && i < 5; ++i)
            a[i] = QGenericArgument(stored[i].typeName(), stored[i].constData());

        const bool ok = matched.invoke(m_controller, Qt::DirectConnection,
                                        a[0], a[1], a[2], a[3], a[4]);
        if (!ok) {
            sendError(client, id, -32603, QStringLiteral("Invocation failed"));
            return;
        }

        sendResponse(client, id, QStringLiteral("dispatched"));
    }

    QJsonObject controllerStateAsJson() const
    {
        QJsonObject state;
        const QMetaObject *meta = m_controller->metaObject();
        for (int i = 0; i < meta->propertyCount(); ++i) {
            const QMetaProperty p = meta->property(i);
            if (!p.isReadable()) continue;
            const QString name = QString::fromLatin1(p.name());
            if (name == QLatin1String("objectName")) continue;
            const QVariant v = p.read(m_controller);
            // Skip QObject* properties (models) — they don't serialize.
            if (v.canConvert<QObject *>()) continue;
            state.insert(name, QJsonValue::fromVariant(v));
        }
        return state;
    }

    // -----------------------------------------------------------------
    // Outgoing signal broadcast
    //
    // Per-signal lambda wiring. When adding a new controller signal
    // that IPC clients should observe, add one connect() call here.
    //
    // Truly reflective signal-to-event broadcast (without per-signal
    // wiring) is target state; Qt's public API does not currently
    // support it cleanly.
    // -----------------------------------------------------------------

    void subscribeToControllerSignals()
    {
        connect(m_controller, &AccountController::configStatusChanged, this, [this] {
            QJsonObject p;
            p["configStatus"] = m_controller->configStatus();
            broadcastEvent(QStringLiteral("configStatusChanged"), p);
        });

        connect(m_controller, &AccountController::activeViewChanged, this, [this] {
            QJsonObject p;
            p["activeView"] = m_controller->activeView();
            broadcastEvent(QStringLiteral("activeViewChanged"), p);
        });

        connect(m_controller, &AccountController::activeAccountIdChanged, this, [this] {
            QJsonObject p;
            p["activeAccountId"] = m_controller->activeAccountId();
            broadcastEvent(QStringLiteral("activeAccountIdChanged"), p);
        });

        connect(m_controller, &AccountController::conversationsChanged, this, [this] {
            broadcastEvent(QStringLiteral("conversationsChanged"), {});
        });

        connect(m_controller, &AccountController::messagesChanged, this,
                [this](const QString &conversationId) {
                    QJsonObject p;
                    p["conversationId"] = conversationId;
                    p["count"] = m_controller->messageListModel()->rowCount();
                    broadcastEvent(QStringLiteral("messagesChanged"), p);
                });

        connect(m_controller, &AccountController::messageSent, this,
                [this](bool ok, const QString &conversationId) {
                    QJsonObject p;
                    p["ok"] = ok;
                    p["conversationId"] = conversationId;
                    broadcastEvent(QStringLiteral("messageSent"), p);
                });

        connect(m_controller, &AccountController::messageBodyReady, this,
                [this](const QString &conversationId, const QString &messageId,
                       const QString &body, const QString &bodyHtml,
                       bool remoteContentBlocked, bool hasHtml) {
                    QJsonObject p;
                    p["conversationId"] = conversationId;
                    p["messageId"] = messageId;
                    p["body"] = body;
                    p["bodyHtml"] = bodyHtml;
                    p["remoteContentBlocked"] = remoteContentBlocked;
                    p["hasHtml"] = hasHtml;
                    broadcastEvent(QStringLiteral("messageBodyReady"), p);
                });

        connect(m_controller, &AccountController::attachmentSaved, this,
                [this](bool ok, const QString &messageId, const QString &path) {
                    QJsonObject p;
                    p["ok"] = ok;
                    p["messageId"] = messageId;
                    p["path"] = path;
                    broadcastEvent(QStringLiteral("attachmentSaved"), p);
                });

        connect(m_controller, &AccountController::ioStarted, this,
                [this](const QString &accountId, bool ok, const QString &error) {
                    QJsonObject p;
                    p["accountId"] = accountId;
                    p["ok"] = ok;
                    p["error"] = error;
                    broadcastEvent(QStringLiteral("ioStarted"), p);
                });

        connect(m_controller, &AccountController::ioStopped, this,
                [this](const QString &accountId) {
                    QJsonObject p;
                    p["accountId"] = accountId;
                    broadcastEvent(QStringLiteral("ioStopped"), p);
                });

        connect(m_controller, &AccountController::errorOccurred, this,
                [this](const QString &error) {
                    QJsonObject p;
                    p["error"] = error;
                    broadcastEvent(QStringLiteral("errorOccurred"), p);
                });
    }

    void broadcastEvent(const QString &name, const QJsonObject &params)
    {
        QJsonObject notif;
        notif["jsonrpc"] = QStringLiteral("2.0");
        notif["method"] = name;
        notif["params"] = params;

        const QByteArray data = QJsonDocument(notif).toJson(QJsonDocument::Compact) + "\n";
        for (auto *client : m_clients) {
            if (client && client->state() == QLocalSocket::ConnectedState) {
                client->write(data);
                client->flush();
            }
        }
    }

    // -----------------------------------------------------------------
    // JSON-RPC response helpers
    // -----------------------------------------------------------------

    void sendResponse(QLocalSocket *client, const QJsonValue &id, const QJsonValue &result)
    {
        QJsonObject resp;
        resp["jsonrpc"] = QStringLiteral("2.0");
        resp["id"] = id;
        resp["result"] = result;
        writeJson(client, resp);
    }

    void sendError(QLocalSocket *client, const QJsonValue &id, int code, const QString &message)
    {
        QJsonObject err;
        err["code"] = code;
        err["message"] = message;

        QJsonObject resp;
        resp["jsonrpc"] = QStringLiteral("2.0");
        resp["id"] = id;
        resp["error"] = err;
        writeJson(client, resp);
    }

    void writeJson(QLocalSocket *client, const QJsonObject &obj)
    {
        const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
        client->write(data);
        client->flush();
    }

    AccountController *m_controller;
    QLocalServer *m_server;
    QSet<QLocalSocket *> m_clients;
    QHash<QLocalSocket *, QByteArray> m_bufs;
};

#endif
