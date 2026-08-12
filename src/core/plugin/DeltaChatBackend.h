#ifndef DELTACHATBACKEND_H
#define DELTACHATBACKEND_H

#include "BackendPlugin.h"
#include "Capabilities.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPromise>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
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

    /// The real account address, learned via get_account_info after
    /// configure/restore — the identity the controller registers.
    QString accountLabel() const override { return m_addr; }

    /// Account removal: delete the per-account rpc store (chats,
    /// messages, keys). The server process is already dead at this
    /// point (shutdown() runs first in removeAccount).
    void purgeLocalData() override
    {
        if (!m_accountsPath.isEmpty())
            QDir(m_accountsPath).removeRecursively();
    }

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
        m_accountsPath = accountsPath;
        m_pendingQr = params.value(QStringLiteral("qr")).toString();

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("DC_ACCOUNTS_PATH", accountsPath);
        m_process->setProcessEnvironment(env);

        m_process->start("deltachat-rpc-server", QStringList(), QProcess::ReadWrite);
        if (!m_process->waitForStarted(5000))
            return false;

        // Reuse an existing configured account across launches (the RPC
        // server persists accounts under DC_ACCOUNTS_PATH); only create
        // a fresh one when the store is empty.
        qWarning().noquote() << "DeltaChat: initialize chain start";
        call(QStringLiteral("get_all_account_ids"), {})
            .then([this](QJsonValue r) {
                const QJsonArray ids = r.toArray();
                qWarning().noquote() << "DeltaChat: existing accounts:" << ids;
                if (ids.isEmpty())
                    return call(QStringLiteral("add_account"), {});
                return QtFuture::makeReadyValueFuture(ids.first());
            }).unwrap()
            .then([this](QJsonValue r) {
                m_accountId = r.toInt();
                qWarning().noquote() << "DeltaChat: selected account" << m_accountId;
                return call(QStringLiteral("select_account"), {m_accountId});
            }).unwrap()
            .then([this](QJsonValue r) {
                // A startIo() that arrived before the account id existed
                // fires now that it does. (The QR setup chain starts IO
                // on its own — this covers the already-configured path.)
                if (m_ioRequested && m_pendingQr.isEmpty()) {
                    m_ioRequested = false;
                    QTimer::singleShot(0, this, [this] { startIo(); });
                }
                return r;  // pass through to the is_configured step
            })
            .then([this](QJsonValue) {
                return call(QStringLiteral("is_configured"), {m_accountId});
            }).unwrap()
            .then([this](QJsonValue r) {
                qWarning().noquote() << "DeltaChat: is_configured:" << r.toBool()
                                     << "qr pending:" << !m_pendingQr.isEmpty();
                startEventPump();
                if (r.toBool()) {
                    // Already configured: learn the identity, then go
                    // online and report ready.
                    fetchIdentity()
                        .then([this](QJsonValue) {
                            emit configured(true);
                            startIo();
                        })
                        .onFailed([this](const std::exception &e) {
                            emit errorOccurred(QString::fromUtf8(e.what()));
                        });
                } else if (!m_pendingQr.isEmpty()) {
                    // Fresh account with QR credentials (chatmail invite
                    // or classic dcaccount: URL) — run the setup chain.
                    setupFromQr(m_pendingQr);
                } else {
                    emit errorOccurred(QStringLiteral(
                        "Delta Chat account is not configured and no "
                        "'qr' credential was provided"));
                }
            })
            .onFailed([this](const std::exception &e) {
                emit errorOccurred(QString::fromUtf8(e.what()));
            });

        return true;
    }

    void shutdown() override
    {
        m_pumpRunning = false;
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
        qWarning().noquote() << "DeltaChat: setupFromQr start";
        emit setupProgress(QStringLiteral("Applying the invite…"));
        call(QStringLiteral("set_config_from_qr"), {m_accountId, qrContent})
            .then([this](QJsonValue) {
                qWarning().noquote() << "DeltaChat: qr applied, configuring...";
                emit setupProgress(QStringLiteral("Configuring the account…"));
                return call(QStringLiteral("configure"), {m_accountId});
            }).unwrap()
            .then([this](QJsonValue) {
                qWarning().noquote() << "DeltaChat: configured, learning identity";
                return fetchIdentity();
            }).unwrap()
            .then([this](QJsonValue) {
                qWarning().noquote() << "DeltaChat: configured, starting io";
                emit setupProgress(QStringLiteral("Starting synchronization…"));
                return call(QStringLiteral("start_io"), {m_accountId});
            }).unwrap()
            .then([this](QJsonValue) {
                qWarning().noquote() << "DeltaChat: io started as" << m_addr;
                emit configured(true);
                emit ioStarted(true, QString());
            })
            .onFailed([this](const std::exception &e) {
                qWarning().noquote() << "DeltaChat: setupFromQr FAILED:" << e.what();
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
        // The controller calls startIo immediately after creation, but
        // the account id is assigned asynchronously by the initialize
        // chain — record the intent and let the chain kick IO once the
        // account exists. (Calling start_io with id 0 fails with
        // "account with id 0 not found".)
        if (m_accountId <= 0) {
            m_ioRequested = true;
            return;
        }
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
                            conversations.append(
                                conversationFromJson(ids->value(i),
                                                     f.result().toObject()));
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
        const int chatId = conversationId.toInt();
        call(QStringLiteral("get_message_ids"), {m_accountId, chatId, false, false})
            .then([this](QJsonValue r) {
                const QJsonArray ids = r.toArray();
                if (ids.isEmpty())
                    return QtFuture::makeReadyValueFuture(QJsonValue(QJsonObject{}));
                return call(QStringLiteral("get_messages"), {m_accountId, ids});
            }).unwrap()
            .then([this, conversationId](QJsonValue r) {
                const QJsonObject byId = r.toObject();
                QVector<Message> messages;
                QVector<int> seenIds;
                messages.reserve(byId.size());
                // Object keys are stringified ids; sort numerically so
                // the list lands oldest → newest.
                QStringList keys = byId.keys();
                std::sort(keys.begin(), keys.end(),
                          [](const QString &a, const QString &b) {
                              return a.toInt() < b.toInt();
                          });
                for (const QString &key : keys) {
                    const QJsonObject m = byId.value(key).toObject();
                    if (m.isEmpty())
                        continue;
                    messages.append(messageFromJson(key, m, conversationId));
                    seenIds.append(key.toInt());
                }
                emit messagesReady(conversationId, messages);

                // DC semantics: viewing a chat marks it seen. Refresh
                // the chat list afterwards so unread badges clear.
                if (!seenIds.isEmpty()) {
                    call(QStringLiteral("markseen_msgs"),
                         {m_accountId, QJsonArray::fromVariantList(
                             QVariantList(seenIds.begin(), seenIds.end()))})
                        .then([this](QJsonValue) {
                            fetchConversations();
                        })
                        .onFailed([this](const std::exception &e) {
                            emit errorOccurred(QString::fromUtf8(e.what()));
                        });
                }
            })
            .onFailed([this](const std::exception &e) {
                emit errorOccurred(QString::fromUtf8(e.what()));
            });
    }

    void fetchMessageBody(const QString &conversationId, const QString &messageId) override
    {
        // Text rides along in the message object — a single-id
        // get_messages is the whole body fetch.
        call(QStringLiteral("get_messages"),
             {m_accountId, QJsonArray{messageId.toInt()}})
            .then([this, conversationId, messageId](QJsonValue r) {
                const QJsonObject m = r.toObject().value(messageId).toObject();
                emit messageBodyReady(conversationId, messageId,
                                      m.value(QStringLiteral("text")).toString(),
                                      QString(), false, false);
            })
            .onFailed([this](const std::exception &e) {
                emit errorOccurred(QString::fromUtf8(e.what()));
            });
    }

    void saveAttachment(const QString &messageId, int partIndex,
                        const QString &destinationPath) override
    {
        Q_UNUSED(partIndex);  // DC: one file per message
        call(QStringLiteral("get_messages"),
             {m_accountId, QJsonArray{messageId.toInt()}})
            .then([this, messageId, destinationPath](QJsonValue r) {
                const QJsonObject m = r.toObject().value(messageId).toObject();
                const QString file = m.value(QStringLiteral("file")).toString();
                if (file.isEmpty()) {
                    emit attachmentSaved(false, messageId, QString());
                    return;
                }
                QFile::remove(destinationPath);  // copy() refuses to overwrite
                const bool ok = QFile::copy(file, destinationPath);
                emit attachmentSaved(ok, messageId, ok ? destinationPath : QString());
            })
            .onFailed([this, messageId](const std::exception &e) {
                emit errorOccurred(QString::fromUtf8(e.what()));
                emit attachmentSaved(false, messageId, QString());
            });
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
                // The sent message exists immediately — refetch so the
                // open thread shows it without waiting for IO events.
                fetchMessages(conversationId);
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

            // Responses carry a truthy id; anything else is noise on
            // this server (events do NOT stream — they are polled via
            // get_next_event_batch, see pumpEvents).
            if (!obj.value("id").isDouble())
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

    /// Learn the account's real address (post-configure/restore) so the
    /// controller can register it under its true identity.
    QFuture<QJsonValue> fetchIdentity()
    {
        return call(QStringLiteral("get_account_info"), {m_accountId})
            .then([this](QJsonValue r) {
                m_addr = r.toObject()
                             .value(QStringLiteral("addr")).toString();
                qWarning().noquote() << "DeltaChat: identity:" << m_addr;
                return r;
            });
    }

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

    // -----------------------------------------------------------------
    // Shared JSON → DTO mappers (used by the fetch paths AND the push
    // path, so the two can never drift apart).
    // -----------------------------------------------------------------

    static Conversation conversationFromJson(int chatId, const QJsonObject &chatObj)
    {
        Conversation c;
        c.id = QString::number(chatId);
        c.kind = QStringLiteral("chat");
        c.name = chatObj.value(QStringLiteral("name")).toString();
        c.preview = QString();
        // freshMessageCounter rides along with the full chat fetch —
        // no extra RPC needed.
        c.unreadCount = chatObj.value(QStringLiteral("freshMessageCounter")).toInt();
        c.lastActivity = QDateTime::currentDateTime();
        return c;
    }

    static Message messageFromJson(const QString &key, const QJsonObject &m,
                                   const QString &conversationId)
    {
        Message msg;
        msg.messageId = key;
        msg.conversationId = conversationId;
        const QJsonObject sender = m.value(QStringLiteral("sender")).toObject();
        QString name = sender.value(QStringLiteral("displayName")).toString();
        if (name.isEmpty())
            name = sender.value(QStringLiteral("address")).toString();
        msg.sender = name;
        msg.date = QDateTime::fromSecsSinceEpoch(
            m.value(QStringLiteral("sortTimestamp")).toInteger());
        msg.body = m.value(QStringLiteral("text")).toString();
        msg.snippet = msg.body.left(120);
        // DC states: 10 = incoming fresh, 13 = incoming noticed.
        const int state = m.value(QStringLiteral("state")).toInt();
        msg.isUnread = (state == 10 || state == 13);
        const QString file = m.value(QStringLiteral("file")).toString();
        if (!file.isEmpty()) {
            AttachmentMeta a;
            a.index = 0;  // DC: one file per message
            a.filename = m.value(QStringLiteral("fileName")).toString();
            a.mimeType = m.value(QStringLiteral("fileMime")).toString();
            a.size = m.value(QStringLiteral("fileBytes")).toInteger();
            msg.attachments.append(a);
        }
        return msg;
    }

    // -----------------------------------------------------------------
    // Event pump. deltachat-rpc-server does NOT push notifications:
    // events are polled — get_next_event_batch blocks server-side until
    // events exist, then resolves. We re-arm after every batch: an
    // always-pending future, driven by readyRead, no threads, no UI
    // blocking. One consumer per server process (that's us).
    // -----------------------------------------------------------------

    void startEventPump()
    {
        if (m_pumpRunning)
            return;
        m_pumpRunning = true;
        pumpEvents();
    }

    void pumpEvents()
    {
        call(QStringLiteral("get_next_event_batch"), {})
            .then([this](QJsonValue batch) {
                handleEvents(batch.toArray());
                pumpEvents();  // re-arm
            })
            .onFailed([this](const std::exception &e) {
                qWarning().noquote() << "DeltaChat: event pump:" << e.what();
                // Back off briefly, then re-arm (process death surfaces
                // separately via onProcessError).
                QTimer::singleShot(1000, this, [this] {
                    if (m_pumpRunning)
                        pumpEvents();
                });
            });
    }

    void handleEvents(const QJsonArray &batch)
    {
        for (const QJsonValue &v : batch) {
            const QJsonObject evt = v.toObject()
                                        .value(QStringLiteral("event")).toObject();
            const QString kind = evt.value(QStringLiteral("kind")).toString();

            if (kind == QLatin1String("IncomingMsg")) {
                const int chatId = evt.value(QStringLiteral("chatId")).toInt();
                const int msgId = evt.value(QStringLiteral("msgId")).toInt();
                if (chatId <= 0 || msgId <= 0)
                    continue;
                // Payload push: fetch the arrived message and emit it
                // plus a fresh conversation row (unread count).
                call(QStringLiteral("get_messages"), {m_accountId, QJsonArray{msgId}})
                    .then([this, chatId, msgId](QJsonValue r) {
                        const QJsonObject m =
                            r.toObject().value(QString::number(msgId)).toObject();
                        if (m.isEmpty())
                            return;
                        emit messageArrived(QString::number(chatId),
                                            messageFromJson(QString::number(msgId), m,
                                                            QString::number(chatId)));
                        refreshConversationRow(chatId);
                    })
                    .onFailed([this](const std::exception &e) {
                        emit errorOccurred(QString::fromUtf8(e.what()));
                    });
            } else if (kind == QLatin1String("ChatModified")) {
                const int chatId = evt.value(QStringLiteral("chatId")).toInt();
                if (chatId > 0)
                    refreshConversationRow(chatId);
            } else if (kind == QLatin1String("IncomingMsgBunch")
                       || kind == QLatin1String("MsgsChanged")) {
                // Bursts/bulk changes: coarse invalidation, the
                // controller debounces these into one refetch.
                emit storageChanged();
            }
            // Info/ConnectivityChanged/etc.: not user-visible state.
        }
    }

    void refreshConversationRow(int chatId)
    {
        call(QStringLiteral("get_full_chat_by_id"), {m_accountId, chatId})
            .then([this, chatId](QJsonValue r) {
                emit conversationUpserted(conversationFromJson(chatId, r.toObject()));
            })
            .onFailed([this](const std::exception &e) {
                emit errorOccurred(QString::fromUtf8(e.what()));
            });
    }

    QProcess *m_process;
    int m_accountId = 0;
    int m_nextId = 1;
    QString m_pendingQr;
    QString m_addr;
    QString m_accountsPath;
    bool m_ioRequested = false;  // startIo arrived before the id existed
    bool m_pumpRunning = false;
    QHash<int, std::shared_ptr<QPromise<QJsonValue>>> m_pending;
    QByteArray m_buffer;
};

#endif
