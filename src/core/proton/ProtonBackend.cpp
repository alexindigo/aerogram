#include "ProtonBackend.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QPromise>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent>

#include <exception>
#include <memory>
#include <stdexcept>

namespace {

/// Failure carrier for the QFuture chains (mirrors RpcError in the
/// Delta Chat backend).
class ProtonError : public std::runtime_error
{
public:
    explicit ProtonError(const QString &msg) : std::runtime_error(msg.toStdString()) {}
};

QJsonValue parseResult(char *raw)
{
    if (!raw)
        throw ProtonError(QStringLiteral("proton core returned null"));
    char *owned = raw;
    const QByteArray json = QByteArray::fromRawData(owned, static_cast<int>(strlen(owned)));
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    proton_free_string(owned);

    const QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("err")))
        throw ProtonError(obj.value(QStringLiteral("err")).toString());
    return obj.value(QStringLiteral("ok"));
}

} // namespace

ProtonBackend::ProtonBackend(QObject *parent)
    : BackendPlugin(parent)
{
}

ProtonBackend::~ProtonBackend()
{
    shutdown();
}

bool ProtonBackend::initialize(const QVariantMap &params)
{
    m_credentials = params;

    // One core per account data dir; the dir namespaces the SDK's
    // session/user DBs and the file keychain.
    const QString user = params.value(QStringLiteral("user")).toString();
    const QString tag = user.isEmpty()
        ? QStringLiteral("default")
        : QString::fromLatin1(QCryptographicHash::hash(user.toUtf8(),
                                                       QCryptographicHash::Md5).toHex().left(8));
    m_dataDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
              + QStringLiteral("/proton/") + tag;
    QDir().mkpath(m_dataDir);

    m_core = proton_core_new(m_dataDir.toUtf8().constData());
    if (!m_core) {
        emit errorOccurred(QStringLiteral("Proton core failed to start"));
        return false;
    }
    return true;
}

void ProtonBackend::shutdown()
{
    // Stop the event thread BEFORE freeing the core: it calls into the
    // core continuously. The 2s wait_event timeout bounds the join.
    if (m_eventThread.joinable()) {
        m_eventThreadStop = true;
        m_eventThread.join();
    }
    if (m_core) {
        proton_core_free(m_core);
        m_core = nullptr;
    }
}

void ProtonBackend::purgeLocalData()
{
    // After shutdown() (the core must be freed first — it holds the
    // session DBs open).
    if (!m_dataDir.isEmpty())
        QDir(m_dataDir).removeRecursively();
}

void ProtonBackend::startEventLoop()
{
    if (m_eventThread.joinable())
        return;
    m_eventThreadStop = false;
    m_eventThread = std::thread([this] {
        while (!m_eventThreadStop) {
            char *raw = proton_call(m_core, "wait_event",
                                    R"({"timeout_ms": 2000})");
            if (m_eventThreadStop)
                break;
            QJsonValue result;
            try {
                result = parseResult(raw);
            } catch (...) {
                continue;  // transient parse/core hiccup — keep polling
            }
            if (!result.toArray().isEmpty())
                emit storageChanged();
        }
    });
}

void ProtonBackend::startIo()
{
    // The SDK's event loop runs on its own runtime from context
    // creation; nothing to kick. ioStarted reports SESSION readiness —
    // emit it only once configure() has actually logged us in (the
    // controller's provisional add flow gates on this ordering).
    if (m_configured)
        emit ioStarted(true, QString());
    else
        m_ioRequested = true;  // configure() will emit when ready
}

void ProtonBackend::stopIo()
{
    emit ioStopped();
}

QFuture<QJsonValue> ProtonBackend::call(const QString &method, const QJsonObject &params)
{
    auto promise = std::make_shared<QPromise<QJsonValue>>();
    promise->start();
    QFuture<QJsonValue> future = promise->future();

    const QByteArray m = method.toUtf8();
    const QByteArray p = QJsonDocument(params).toJson(QJsonDocument::Compact);
    ProtonCore *core = m_core;

    QtConcurrent::run([core, m, p, promise] {
        try {
            char *raw = proton_call(core, m.constData(), p.constData());
            promise->addResult(parseResult(raw));
        } catch (...) {
            promise->setException(std::current_exception());
        }
        promise->finish();
    });
    return future;
}

void ProtonBackend::configure(const QVariantMap &credentials)
{
    const QString user = credentials.value(QStringLiteral("user")).toString();
    const QString pass = credentials.value(QStringLiteral("pass")).toString();
    const QString totp = credentials.value(QStringLiteral("totp")).toString();

    // Restore a persisted session first (no password needed); only when
    // there is none do we burn a fresh login.
    emit setupProgress(QStringLiteral("Restoring saved session…"));
    call(QStringLiteral("restore_session"))
        .then([this, user, pass, totp](QJsonValue r) -> QFuture<QJsonValue> {
            if (r.toObject().value(QStringLiteral("state")).toString() == QLatin1String("ok"))
                return QtFuture::makeReadyValueFuture(r);

            emit setupProgress(QStringLiteral("Signing in to Proton…"));
            return call(QStringLiteral("login"),
                        {{QStringLiteral("user"), user}, {QStringLiteral("password"), pass}})
                .then([this, totp](QJsonValue lr) -> QFuture<QJsonValue> {
                    const QString state =
                        lr.toObject().value(QStringLiteral("state")).toString();
                    if (state == QLatin1String("totp") && !totp.isEmpty()) {
                        emit setupProgress(QStringLiteral("Checking the 2FA code…"));
                        return call(QStringLiteral("submit_totp"),
                                    {{QStringLiteral("code"), totp}});
                    }
                    return QtFuture::makeReadyValueFuture(lr);
                })
                .unwrap();
        })
        .unwrap()
        .then([this](QJsonValue r) {
            const QString state =
                r.toObject().value(QStringLiteral("state")).toString();
            if (state == QLatin1String("ok")) {
                m_configured = true;
                emit setupProgress(QStringLiteral("Signed in — syncing folders…"));
                emit configured(true);
                if (m_ioRequested) {
                    m_ioRequested = false;
                    emit ioStarted(true, QString());
                }
                startEventLoop();
                fetchConversations();
            } else if (state == QLatin1String("totp")) {
                emit errorOccurred(QStringLiteral(
                    "Proton: two-factor authentication enabled — re-add "
                    "the account with a fresh 6-digit code in the TOTP field"));
                emit configured(false);
            } else if (state == QLatin1String("mailbox_password")) {
                emit errorOccurred(QStringLiteral(
                    "Proton: two-password mode is not supported by the "
                    "prototype yet"));
                emit configured(false);
            } else {
                emit configured(false);
            }
        })
        .onFailed([this](const std::exception &e) {
            emit errorOccurred(QString::fromUtf8(e.what()));
            emit configured(false);
        });
}

void ProtonBackend::fetchConversations()
{
    call(QStringLiteral("list_labels"))
        .then([this](QJsonValue r) {
            QVector<Conversation> convs;
            for (const QJsonValue &v : r.toArray()) {
                const QJsonObject o = v.toObject();
                Conversation c;
                c.id = QString::number(o.value(QStringLiteral("id")).toInteger());
                c.kind = QStringLiteral("folder");
                c.name = o.value(QStringLiteral("name")).toString();
                c.unreadCount = 0;  // TODO: label counters (LabelWithCounters)
                c.lastActivity = QDateTime::currentDateTime();
                convs.append(c);
            }
            emit conversationsReady(convs);

            // Right after login the SDK's initial sync may not have
            // written the label rows yet — retry briefly instead of
            // leaving the account empty until restart.
            if (convs.isEmpty() && m_configured && m_labelRetries < 5) {
                ++m_labelRetries;
                emit setupProgress(QStringLiteral("Syncing folders…"));
                QTimer::singleShot(2000, this, [this] { fetchConversations(); });
            } else if (!convs.isEmpty()) {
                m_labelRetries = 0;
            }
        })
        .onFailed([this](const std::exception &e) {
            emit errorOccurred(QString::fromUtf8(e.what()));
        });
}

void ProtonBackend::fetchMessages(const QString &conversationId)
{
    call(QStringLiteral("list_messages"),
         {{QStringLiteral("label_id"), conversationId.toLongLong()},
          {QStringLiteral("limit"), 100}})
        .then([this, conversationId](QJsonValue r) {
            QVector<Message> messages;
            for (const QJsonValue &v : r.toArray()) {
                const QJsonObject o = v.toObject();
                Message m;
                m.messageId = QString::number(o.value(QStringLiteral("id")).toInteger());
                m.conversationId = conversationId;
                m.subject = o.value(QStringLiteral("subject")).toString();
                QString sender = o.value(QStringLiteral("sender_name")).toString();
                if (sender.isEmpty())
                    sender = o.value(QStringLiteral("sender_addr")).toString();
                m.sender = sender;
                m.date = QDateTime::fromSecsSinceEpoch(
                    o.value(QStringLiteral("time")).toInteger());
                m.isUnread = o.value(QStringLiteral("unread")).toBool();
                // Attachment chips need per-part metadata; that rides
                // with the body fetch in v1.
                messages.append(m);
            }
            emit messagesReady(conversationId, messages);
        })
        .onFailed([this](const std::exception &e) {
            emit errorOccurred(QString::fromUtf8(e.what()));
        });
}

void ProtonBackend::fetchMessageBody(const QString &conversationId, const QString &messageId)
{
    call(QStringLiteral("message_body"),
         {{QStringLiteral("id"), messageId.toLongLong()}})
        .then([this, conversationId, messageId](QJsonValue r) {
            const QJsonObject o = r.toObject();
            // NOTE: body is the decrypted raw body — HTML mail arrives
            // as HTML here (mime_type tells). Plain-text transform is
            // prototype follow-up.
            emit messageBodyReady(conversationId, messageId,
                                  o.value(QStringLiteral("text")).toString());
        })
        .onFailed([this](const std::exception &e) {
            emit errorOccurred(QString::fromUtf8(e.what()));
        });
}

void ProtonBackend::saveAttachment(const QString &messageId, int partIndex,
                                   const QString &destinationPath)
{
    // Attachment blobs are a separate SDK call (get_attachment); not
    // wired in the prototype round. Fail honestly.
    Q_UNUSED(partIndex);
    Q_UNUSED(destinationPath);
    emit attachmentSaved(false, messageId, QString());
}
