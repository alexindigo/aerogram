#ifndef DELTACHATBACKEND_H
#define DELTACHATBACKEND_H

#include "BackendPlugin.h"
#include "Capabilities.h"

#include <QDebug>
#include <QDir>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPromise>
#include <QProcess>
#include <QStandardPaths>

#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>

/// \brief Exception carrying a JSON-RPC error message from
///        deltachat-rpc-server. Thrown into QFuture chains via
///        QPromise::setException.
class RpcError : public std::runtime_error
{
public:
    explicit RpcError(const QString &msg)
        : std::runtime_error(msg.toStdString()) {}
};

/// \brief BackendPlugin implementation backed by deltachat-rpc-server
///        over JSON-RPC 2.0 on stdio.
///
/// Uses QFuture-based promises for multi-step async coordination. Each
/// RPC call returns a QFuture that fulfils with the result or fails
/// with an RpcError. Sequential flows compose via .then().unwrap();
/// fan-out uses QtFuture::whenAll.
///
/// See docs/ARCHITECTURE.md, Pattern A, for details on the promise
/// pattern.
class DeltaChatBackend : public BackendPlugin,
                         public IConversationProvider,
                         public IMessageProvider,
                         public IMessageSender,
                         public IQrSetup
{
    Q_OBJECT

public:
    explicit DeltaChatBackend(QObject *parent = nullptr)
        : BackendPlugin(parent)
        , m_process(new QProcess(this))
    {
        connect(m_process, &QProcess::readyReadStandardOutput,
                this, &DeltaChatBackend::onReadyRead);
        connect(m_process, &QProcess::errorOccurred,
                this, &DeltaChatBackend::onProcessError);
    }

    ~DeltaChatBackend() override
    {
        shutdown();
    }

    QString name() const override { return QStringLiteral("deltachat"); }

    QString family() const override { return QStringLiteral("chat"); }

    // -----------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------

    bool initialize(const QVariantMap &params) override
    {
        QString accountsPath = params.value("accounts_path").toString();
        if (accountsPath.isEmpty()) {
            accountsPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                         + QDir::separator() + "delta";
        }
        QDir().mkpath(accountsPath);

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("DC_ACCOUNTS_PATH", accountsPath);
        m_process->setProcessEnvironment(env);

        m_process->start("deltachat-rpc-server", QStringList(), QProcess::ReadWrite);
        if (!m_process->waitForStarted(5000))
            return false;

        // Add + select account (fire and forget; result observable via
        // future errors flowing through emit errorOccurred).
        call(QStringLiteral("add_account"), {})
            .then([this](QJsonValue r) {
                m_accountId = r.toInt();
                return call(QStringLiteral("select_account"), {m_accountId});
            }).unwrap()
            .then([](QJsonValue) {})
            .onFailed([this](const std::exception &e) {
                emit errorOccurred(QString::fromUtf8(e.what()));
            });

        return true;
    }

    void shutdown() override
    {
        if (m_process->state() != QProcess::NotRunning) {
            m_process->terminate();
            m_process->waitForFinished(3000);
            if (m_process->state() != QProcess::NotRunning) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
    }

    // -----------------------------------------------------------------
    // Account setup flows
    // -----------------------------------------------------------------

    void setupFromQr(const QString &qrContent) override
    {
        call(QStringLiteral("set_config_from_qr"), {m_accountId, qrContent})
            .then([this](QJsonValue) {
                return call(QStringLiteral("configure"), {m_accountId});
            }).unwrap()
            .then([this](QJsonValue) {
                return call(QStringLiteral("start_io"), {m_accountId});
            }).unwrap()
            .then([this](QJsonValue) {
                emit configured(true);
                emit ioStarted(true, QString());
            })
            .onFailed([this](const std::exception &e) {
                emit configured(false);
                emit errorOccurred(QString::fromUtf8(e.what()));
            });
    }

    void getBackupFromQr(const QString &qrText) override
    {
        call(QStringLiteral("get_backup"), {m_accountId, qrText})
            .then([this](QJsonValue) {
                return call(QStringLiteral("start_io"), {m_accountId});
            }).unwrap()
            .then([this](QJsonValue) {
                emit configured(true);
                emit ioStarted(true, QString());
            })
            .onFailed([this](const std::exception &e) {
                emit configured(false);
                emit errorOccurred(QString::fromUtf8(e.what()));
            });
    }

    // -----------------------------------------------------------------
    // IO control
    // -----------------------------------------------------------------

    void startIo() override
    {
        call(QStringLiteral("start_io"), {m_accountId})
            .then([this](QJsonValue) {
                emit ioStarted(true, QString());
            })
            .onFailed([this](const std::exception &e) {
                emit ioStarted(false, QString::fromUtf8(e.what()));
            });
    }

    void stopIo() override
    {
        call(QStringLiteral("stop_io"), {m_accountId})
            .then([this](QJsonValue) {
                emit ioStopped();
            })
            .onFailed([this](const std::exception &e) {
                emit errorOccurred(QString::fromUtf8(e.what()));
            });
    }

    // -----------------------------------------------------------------
    // Data queries
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // IConversationProvider
    // -----------------------------------------------------------------

    void fetchConversations() override
    {
        call(QStringLiteral("get_chatlist_entries"),
             {m_accountId, QJsonValue::Null, QJsonValue::Null, QJsonValue::Null})
            .then([this](QJsonValue r) -> QFuture<void> {
                QJsonArray chatIds = r.toArray();
                if (chatIds.isEmpty()) {
                    emit conversationsReady({});
                    return QtFuture::makeReadyVoidFuture();
                }

                // Fan out N parallel detail fetches.
                QList<QFuture<QJsonValue>> details;
                details.reserve(chatIds.size());
                for (const QJsonValue &v : chatIds) {
                    const int chatId = v.toInt();
                    details.append(call(QStringLiteral("get_full_chat_by_id"),
                                        {m_accountId, chatId}));
                }

                // Collect IDs in the same order as the futures.
                auto ids = std::make_shared<QList<int>>();
                for (const QJsonValue &v : chatIds)
                    ids->append(v.toInt());

                return QtFuture::whenAll(details.begin(), details.end())
                    .then([this, ids](QList<QFuture<QJsonValue>> results) {
                        QVector<Conversation> conversations;
                        conversations.reserve(results.size());
                        int i = 0;
                        for (const auto &f : results) {
                            const QJsonObject chatObj = f.result().toObject();
                            Conversation c;
                            c.id = QString::number(ids->value(i));
                            c.kind = QStringLiteral("chat");
                            c.name = chatObj.value("name").toString();
                            c.preview = QString();
                            c.unreadCount = 0;  // TODO: get_fresh_msg_cnt
                            c.lastActivity = QDateTime::currentDateTime();
                            conversations.append(c);
                            ++i;
                        }
                        emit conversationsReady(conversations);
                    });
            }).unwrap()
            .onFailed([this](const std::exception &e) {
                emit errorOccurred(QString::fromUtf8(e.what()));
            });
    }

    // -----------------------------------------------------------------
    // IMessageProvider
    // -----------------------------------------------------------------

    void fetchMessages(const QString &conversationId) override
    {
        // TODO: full message fetch — for now emit an empty list to
        // clear the model. See docs/CLEANUP.md.
        emit messagesReady(conversationId, {});
    }

    void fetchMessageBody(const QString &conversationId, const QString &messageId) override
    {
        // TODO: dc_get_msg body extraction. See docs/CLEANUP.md.
        emit messageBodyReady(conversationId, messageId, QString());
    }

    void saveAttachment(const QString &messageId, int partIndex,
                        const QString &destinationPath) override
    {
        // TODO: dc_get_msg blob save. See docs/CLEANUP.md.
        Q_UNUSED(partIndex);
        Q_UNUSED(destinationPath);
        emit attachmentSaved(false, messageId, QString());
    }

    // -----------------------------------------------------------------
    // IMessageSender
    // -----------------------------------------------------------------

    void sendMessage(const QString &conversationId, const QString &text) override
    {
        call(QStringLiteral("misc_send_text_message"),
             {m_accountId, conversationId.toInt(), text})
            .then([this, conversationId](QJsonValue) {
                emit messageSent(true, conversationId);
            })
            .onFailed([this, conversationId](const std::exception &e) {
                emit errorOccurred(QString::fromUtf8(e.what()));
                emit messageSent(false, conversationId);
            });
    }

private slots:
    void onReadyRead()
    {
        m_buffer.append(m_process->readAllStandardOutput());
        while (true) {
            const int idx = m_buffer.indexOf('\n');
            if (idx < 0) break;

            const QByteArray line = m_buffer.left(idx).trimmed();
            m_buffer.remove(0, idx + 1);
            if (line.isEmpty()) continue;

            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
            if (err.error != QJsonParseError::NoError) {
                qWarning().noquote() << "DeltaChat: JSON parse error:"
                                     << err.errorString() << "line:" << line;
                continue;
            }

            const QJsonObject obj = doc.object();

            // Events (notifications) have no id — ignored for now.
            if (!obj.contains("id") || obj.value("id").isNull())
                continue;

            const int responseId = obj.value("id").toInt();
            auto it = m_pending.find(responseId);
            if (it == m_pending.end()) {
                qWarning().noquote() << "DeltaChat: unexpected response id" << responseId;
                continue;
            }

            auto promise = it.value();
            m_pending.erase(it);

            if (obj.contains("error")) {
                const QString msg = obj.value("error").toObject().value("message").toString();
                try {
                    throw RpcError(msg);
                } catch (...) {
                    promise->setException(std::current_exception());
                }
                promise->finish();
                continue;
            }

            promise->addResult(obj.value("result"));
            promise->finish();
        }
    }

    void onProcessError(QProcess::ProcessError err)
    {
        Q_UNUSED(err);
        emit errorOccurred(m_process->errorString());
    }

private:
    // -----------------------------------------------------------------
    // The Pattern A primitive: single RPC as a QFuture.
    //
    // Every outbound RPC goes through this helper. It allocates a
    // request id, stashes a shared promise in m_pending, writes the
    // request, and returns the promise's future. When onReadyRead()
    // matches a response to the id, it either addResult+finish (on
    // success) or setException+finish with an RpcError (on failure).
    // -----------------------------------------------------------------
    QFuture<QJsonValue> call(const QString &method, const QJsonArray &params)
    {
        const int id = nextId();

        auto promise = std::make_shared<QPromise<QJsonValue>>();
        promise->start();
        QFuture<QJsonValue> future = promise->future();

        m_pending.insert(id, promise);
        writeRequest(id, method, params);

        return future;
    }

    int nextId() { return m_nextId++; }

    void writeRequest(int id, const QString &method, const QJsonArray &params)
    {
        QJsonObject req;
        req["jsonrpc"] = QStringLiteral("2.0");
        req["method"] = method;
        req["params"] = params;
        req["id"] = id;

        const QByteArray data = QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n";
        m_process->write(data);
    }

    QProcess *m_process;
    int m_accountId = 0;
    int m_nextId = 1;
    QHash<int, std::shared_ptr<QPromise<QJsonValue>>> m_pending;
    QByteArray m_buffer;
};

#endif
