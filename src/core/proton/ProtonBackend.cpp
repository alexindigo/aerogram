#include "ProtonBackend.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QPromise>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent>

#include "../imap/MessageStore.h"
#include "../imap/MetadataIndex.h"
#include "../imap/MimeParser.h"

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

    // Local store lives beside the SDK's session data; purgeLocalData
    // removes m_dataDir wholesale, covering both.
    m_storeRoot = m_dataDir + QStringLiteral("/store/storage");
    m_indexDb = m_dataDir + QStringLiteral("/store/index.db");
    QDir().mkpath(m_storeRoot);

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
            if (!result.toArray().isEmpty()) {
                const auto tags = result.toArray();
                emit storageChanged();
                // Message-table change: persist new arrivals' bodies into
                // the local store right away (search + offline), not just
                // when someone happens to open the folder. INBOX is the
                // arrival point for incoming mail.
                for (const QJsonValue &t : tags) {
                    if (t.toString() == QLatin1String("messages")
                            && !m_inboxLocalId.isEmpty() && !m_key.isEmpty()) {
                        fetchMessages(m_inboxLocalId);  // emits + prefetches/persists
                        break;
                    }
                }
            }
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
            // Capture INBOX's local label id (used for arrival-time
            // body persistence).
            for (const Conversation &c : convs) {
                if (c.name.compare(QStringLiteral("INBOX"), Qt::CaseInsensitive) == 0)
                    m_inboxLocalId = c.id;
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
                // First time labels resolve after login: persist the
                // inbox page so existing mail is in the store (search +
                // offline) without waiting for the user to open it.
                if (!m_didInitialInboxStore && !m_inboxLocalId.isEmpty()
                        && !m_key.isEmpty()) {
                    m_didInitialInboxStore = true;
                    fetchMessages(m_inboxLocalId);
                }
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
            // Shape: {"messages": [...], "total": N}. `total` is the
            // pre-limit count — when we hold all N, the listing is
            // complete and deletions can be reconciled safely.
            const QJsonObject payload = r.toObject();
            const QJsonArray arr = payload.value(QStringLiteral("messages")).toArray();
            const qint64 total = payload.value(QStringLiteral("total")).toInteger();
            QVector<Message> messages;
            QSet<QString> presentIds;
            for (const QJsonValue &v : arr) {
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
                presentIds.insert(m.messageId);
            }
            emit messagesReady(conversationId, messages);

            // Deletion sync: when the listing is COMPLETE (we hold all
            // `total` rows), anything in our index but absent here was
            // deleted server-side — remove it (index rows + store
            // shards). Skipped for truncated listings.
            if (!m_key.isEmpty() && qint64(messages.size()) == total) {
                MetadataIndex idx(m_indexDb, m_key);
                QString err;
                if (idx.open(&err)) {
                    const QVector<QString> gone =
                        idx.removeMissingFromConversation(conversationId, presentIds);
                    if (!gone.isEmpty()) {
                        MessageStore store(m_storeRoot, m_key);
                        for (const QString &rel : gone)
                            store.remove(rel);
                        qInfo() << "ProtonBackend: reconciled" << conversationId
                                << "— removed" << gone.size() << "deleted message(s)";
                    }
                }
            }

            // Prefetch bodies into the local store (first N) so opens
            // are instant and the FTS index builds a search corpus.
            // Quiet: persistMessage only — no messageBodyReady (must not
            // clobber whatever message is open).
            if (!m_key.isEmpty()) {
                const int prefetchCount = qMin(20, messages.size());
                for (int i = 0; i < prefetchCount; ++i) {
                    const QString mid = messages.at(i).messageId;
                    call(QStringLiteral("message_body"),
                         {{QStringLiteral("id"), mid.toLongLong()}})
                        .then([this, conversationId, mid](QJsonValue rb) {
                            const QJsonObject o = rb.toObject();
                            persistMessage(conversationId, mid, o,
                                           o.value(QStringLiteral("text")).toString());
                        })
                        .onFailed([](const std::exception &) {
                            // prefetch is best-effort; ignore
                        });
                }
            }
        })
        .onFailed([this](const std::exception &e) {
            emit errorOccurred(QString::fromUtf8(e.what()));
        });
}

void ProtonBackend::fetchMessageBody(const QString &conversationId, const QString &messageId)
{
    // Store-first: a previously fetched message reads from the local
    // encrypted store (instant, offline). Only a miss hits the SDK.
    if (!m_key.isEmpty()) {
        QString cachedText;
        QString cachedHtml;
        {
            MetadataIndex idx(m_indexDb, m_key);
            QString err;
            if (idx.open(&err)) {
                const QString rel = idx.filePathForMessage(messageId);
                if (!rel.isEmpty()) {
                    MessageStore store(m_storeRoot, m_key);
                    const QByteArray raw = store.get(rel);
                    if (!raw.isEmpty()) {
                        // Stored as a faithful .eml — parse like IMAP.
                        const auto parsed = MimeParser::parse(raw);
                        cachedText = parsed.bodyPlain;
                        cachedHtml = parsed.bodyHtml;  // already sanitized
                    }
                }
            }
        }
        if (!cachedText.isEmpty()) {
            // Instant + offline. bodyHtml came from the store (already
            // sanitized at fetch time); remote content stays blocked.
            emit messageBodyReady(conversationId, messageId, cachedText,
                                  cachedHtml, !cachedHtml.isEmpty());
            return;
        }
    }

    call(QStringLiteral("message_body"),
         {{QStringLiteral("id"), messageId.toLongLong()}})
        .then([this, conversationId, messageId](QJsonValue r) {
            const QJsonObject o = r.toObject();
            // text = plain (always safe), html = SDK-sanitized display
            // html (remote content stripped), blocked flag drives the
            // "remote content blocked" note.
            const QString text = o.value(QStringLiteral("text")).toString();
            persistMessage(conversationId, messageId, o, text);
            emit messageBodyReady(
                conversationId, messageId, text,
                o.value(QStringLiteral("html")).toString(),
                o.value(QStringLiteral("blocked_remote")).toBool());
        })
        .onFailed([this](const std::exception &e) {
            emit errorOccurred(QString::fromUtf8(e.what()));
        });
}

/// \brief Synthesize a faithful RFC822 message (.eml) from the original
///        header block + the decrypted/sanitized bodies, in the same
///        format IMAP stores. Kept headers (From/To/Subject/Date/
///        Message-ID/Cc…); Content-* and MIME-Version are regenerated
///        to describe the new multipart/alternative body.
static QByteArray buildEml(const QString &rawHeader, const QString &plainBody,
                           const QString &htmlBody)
{
    // Parse the original header block (append CRLF CRLF so splitMessage
    // sees a complete message with an empty body).
    const auto orig = MimeParser::splitMessage(
        rawHeader.toUtf8() + "\r\n\r\n");

    QByteArray out;
    auto putLine = [&out](const QString &name, const QString &value) {
        if (!value.isEmpty())
            out += name.toUtf8() + ": " + value.toUtf8() + "\r\n";
    };
    for (auto it = orig.headers.begin(); it != orig.headers.end(); ++it) {
        const QString k = it.key().toLower();
        if (k.startsWith(QStringLiteral("content-"))
            || k == QLatin1String("mime-version"))
            continue;  // regenerated below for the new body structure
        out += it.key().toUtf8() + ": " + it.value().toUtf8() + "\r\n";
    }

    if (htmlBody.isEmpty()) {
        putLine(QStringLiteral("MIME-Version"), QStringLiteral("1.0"));
        putLine(QStringLiteral("Content-Type"),
                QStringLiteral("text/plain; charset=utf-8"));
        putLine(QStringLiteral("Content-Transfer-Encoding"),
                QStringLiteral("base64"));
        out += "\r\n";
        out += plainBody.toUtf8().toBase64();
        out += "\r\n";
        return out;
    }

    const QByteArray boundary = "aerogram-" + QByteArray::number(
        qHash(plainBody + htmlBody), 36);
    putLine(QStringLiteral("MIME-Version"), QStringLiteral("1.0"));
    putLine(QStringLiteral("Content-Type"),
            QStringLiteral("multipart/alternative; boundary=\"")
            + QString::fromLatin1(boundary) + QStringLiteral("\""));
    out += "\r\n";

    out += "--" + boundary + "\r\n"
           "Content-Type: text/plain; charset=utf-8\r\n"
           "Content-Transfer-Encoding: base64\r\n\r\n"
        + plainBody.toUtf8().toBase64() + "\r\n";

    out += "--" + boundary + "\r\n"
           "Content-Type: text/html; charset=utf-8\r\n"
           "Content-Transfer-Encoding: base64\r\n\r\n"
        + htmlBody.toUtf8().toBase64() + "\r\n";

    out += "--" + boundary + "--\r\n";
    return out;
}

/// \brief Persist a fetched message as a faithful .eml into the shared
///        store (encrypted shard) + FTS index — the same format IMAP
///        uses, so Proton mail is searchable and offline-readable.
///        Runs on the calling (FFI worker) thread; store/index are
///        self-contained per-account.
void ProtonBackend::persistMessage(const QString &conversationId,
                                   const QString &messageId,
                                   const QJsonObject &apiMsg,
                                   const QString &plainBody)
{
    if (m_key.isEmpty() || plainBody.isEmpty())
        return;

    const QByteArray eml = buildEml(apiMsg.value(QStringLiteral("header")).toString(),
                                    plainBody,
                                    apiMsg.value(QStringLiteral("html")).toString());
    const auto parsed = MimeParser::parse(eml);

    MessageStore store(m_storeRoot, m_key);
    const QString rel = store.put(eml, messageId);

    Message m;
    m.messageId = messageId;
    m.conversationId = conversationId;
    m.subject = parsed.subject;
    m.sender = parsed.sender;
    m.date = parsed.date;
    m.isUnread = false;

    MetadataIndex idx(m_indexDb, m_key);
    QString err;
    if (!idx.open(&err))
        return;
    idx.insertMessages({m}, {parsed.bodyPlain}, {rel}, {{}});
}

void ProtonBackend::wipeLocalStore()
{
    if (!m_storeRoot.isEmpty())
        QDir(m_storeRoot).removeRecursively();
    const QString storeDir = QFileInfo(m_indexDb).absolutePath();
    if (!storeDir.isEmpty())
        QDir(storeDir).removeRecursively();
    QDir().mkpath(m_storeRoot);
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
