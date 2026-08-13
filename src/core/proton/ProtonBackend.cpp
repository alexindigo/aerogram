#include "ProtonBackend.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QPromise>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent>

#include "../store/EmailStore.h"
#include "core/content/ContentPipeline.h"
#include "core/content/HtmlSanitizer.h"

#include <KMime/Headers>
#include <KMime/Message>
#include <KMime/Util>

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
    m_store.setPaths(m_indexDb, m_storeRoot);

    m_core = proton_core_new(m_dataDir.toUtf8().constData());
    if (!m_core) {
        emit errorOccurred(QStringLiteral("Proton core failed to start"));
        return false;
    }
    return true;
}

void ProtonBackend::shutdown()
{
    // Stop scheduling new work first; drain in-flight workers so no
    // continuation touches a dead backend/core (the prefetch crash).
    m_shuttingDown = true;
    if (m_eventThread.joinable()) {
        m_eventThreadStop = true;
        m_eventThread.join();
    }
    m_workers.waitForFinished();
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
                // arrival point for incoming mail. QUEUED onto the UI
                // thread: fetchMessages touches QFutureSynchronizer and
                // other non-thread-safe members — never call it from the
                // raw event thread.
                for (const QJsonValue &t : tags) {
                    if (t.toString() == QLatin1String("messages")
                            && !m_inboxLocalId.isEmpty() && !m_key.isEmpty()) {
                        QMetaObject::invokeMethod(this, [this]() {
                            if (!m_shuttingDown)
                                fetchMessages(m_inboxLocalId);
                        }, Qt::QueuedConnection);
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

    if (m_shuttingDown) {
        // Fail fast — no work starts during/after teardown.
        promise->setException(std::make_exception_ptr(
            ProtonError(QStringLiteral("backend is shutting down"))));
        promise->finish();
        return future;
    }

    const QByteArray m = method.toUtf8();
    const QByteArray p = QJsonDocument(params).toJson(QJsonDocument::Compact);
    ProtonCore *core = m_core;

    m_workers.addFuture(QtConcurrent::run([this, core, m, p, promise] {
        if (m_shuttingDown) {
            promise->setException(std::make_exception_ptr(
                ProtonError(QStringLiteral("backend is shutting down"))));
            promise->finish();
            return;
        }
        try {
            char *raw = proton_call(core, m.constData(), p.constData());
            promise->addResult(parseResult(raw));
        } catch (...) {
            promise->setException(std::current_exception());
        }
        promise->finish();
    }));
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
                c.id = o.value(QStringLiteral("id")).toString();
                c.kind = QStringLiteral("folder");
                c.name = o.value(QStringLiteral("name")).toString();
                c.unreadCount = static_cast<int>(o.value(QStringLiteral("unread")).toInteger());
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
         {{QStringLiteral("label_id"), conversationId},
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
                m.messageId = o.value(QStringLiteral("id")).toString();
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
            if (!m_key.isEmpty() && !m_shuttingDown
                    && qint64(messages.size()) == total) {
                const QVector<QString> gone =
                    m_store.reconcileConversation(conversationId, presentIds);
                if (!gone.isEmpty())
                    qInfo() << "ProtonBackend: reconciled" << conversationId
                            << "— removed" << gone.size() << "deleted message(s)";
            }

            // Prefetch bodies into the local store (first N) so opens
            // are instant and the FTS index builds a search corpus.
            // Quiet: persistMessage only — no messageBodyReady (must not
            // clobber whatever message is open).
            if (!m_key.isEmpty() && !m_shuttingDown) {
                const int prefetchCount = qMin(20, messages.size());
                for (int i = 0; i < prefetchCount; ++i) {
                    const QString mid = messages.at(i).messageId;
                    // Skip bodies the store already has — without this,
                    // every listing (and every push event) re-downloads
                    // the same 20 bodies forever.
                    if (!m_store.filePathForMessage(mid).isEmpty())
                        continue;
                    call(QStringLiteral("message_body"),
                         {{QStringLiteral("id"), mid}})
                        .then([this, conversationId, mid](QJsonValue rb) {
                            if (m_shuttingDown)
                                return;
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
    // Policy: the store holds RAW truth; sanitization happens at read
    // time via the shared pipeline (defense in depth).
    if (!m_key.isEmpty()) {
        QString cachedText;
        QStringList cachedChunks;
        bool cachedBlocked = false;
        {
            // Legacy shards (raw plain text from before the .eml
            // synthesis) can never stream HTML — treat as a miss so the
            // live fetch below re-persists in multipart (self-healing
            // upgrade). Detected via the missing Content-Type header.
            const QString rel = m_store.filePathForMessage(messageId);
            const QByteArray raw = rel.isEmpty() ? QByteArray()
                                                 : m_store.readShard(rel);
            if (!raw.isEmpty() && raw.contains("Content-Type:")) {
                const auto parts = m_store.readBodyStreamed(messageId);
                if (parts.found) {
                    cachedText = parts.plain;
                    cachedChunks = parts.htmlChunks;
                    cachedBlocked = parts.blockedRemote;
                }
            }
        }
        if (!cachedText.isEmpty()) {
            // Plain text paints instantly…
            emit messageBodyReady(conversationId, messageId, cachedText,
                                  QString(), cachedBlocked,
                                  /*hasHtml=*/!cachedChunks.isEmpty());
            // …then sanitized HTML chunks stream in for progressive
            // render (first paint never waits for the whole doc).
            // Each chunk is QUEUED: a synchronous burst lands in one
            // frame and looks like an all-at-once render; queued
            // delivery lets the event loop paint between chunks.
            for (int i = 0; i < cachedChunks.size(); ++i) {
                const QString chunk = cachedChunks.at(i);
                const bool last = i == cachedChunks.size() - 1;
                QMetaObject::invokeMethod(this,
                    [this, conversationId, messageId, chunk, last, cachedBlocked]() {
                        emit messageBodyChunkReady(conversationId, messageId,
                                                   chunk, last, cachedBlocked);
                    }, Qt::QueuedConnection);
            }
            return;
        }
    }

    call(QStringLiteral("message_body"),
         {{QStringLiteral("id"), messageId}})
        .then([this, conversationId, messageId](QJsonValue r) {
            const QJsonObject o = r.toObject();
            // The core returns RAW truth; we persist it raw and sanitize
            // for display via the shared streaming pipeline.
            const QString text = o.value(QStringLiteral("text")).toString();
            const QString rawHtml = o.value(QStringLiteral("html")).toString();
            persistMessage(conversationId, messageId, o, text);

            // Plain first…
            emit messageBodyReady(conversationId, messageId, text,
                                  QString(), false,
                                  /*hasHtml=*/!rawHtml.isEmpty());
            // …then the sanitized HTML streams in chunks.
            if (!rawHtml.isEmpty()) {
                const QStringList chunks =
                    ContentPipeline::sanitizeStreamed(rawHtml);
                // blocked flag rides the last chunk (we don't know it
                // until the stream finishes). Chunks are QUEUED (see
                // the cache-hit path) so frames render between them.
                const bool blocked = !chunks.isEmpty();
                for (int i = 0; i < chunks.size(); ++i) {
                    const QString chunk = chunks.at(i);
                    const bool last = i == chunks.size() - 1;
                    QMetaObject::invokeMethod(this,
                        [this, conversationId, messageId, chunk, last, blocked]() {
                            emit messageBodyChunkReady(conversationId, messageId,
                                                       chunk, last, blocked);
                        }, Qt::QueuedConnection);
                }
            }
        })
        .onFailed([this, messageId](const std::exception &e) {
            qWarning().noquote() << QStringLiteral("ProtonBackend: body fetch failed for %1: %2")
                                        .arg(messageId, e.what());
            emit errorOccurred(QString::fromUtf8(e.what()));
        });
}

/// \brief Persist a fetched message as a faithful .eml into the shared
///        store (encrypted shard) + FTS index — the same format IMAP
///        uses, so Proton mail is searchable and offline-readable.
///        Runs on the calling (FFI worker) thread; store/index are
///        self-contained per-account.
/// \brief Assemble the COMPLETE faithful .eml: original header block
///        (real Content-Type provenance kept; Content-* regenerated for
///        the container) + verbatim body + each attachment embedded as
///        a MIME part (bridge parity — what Bridge serves over IMAP).
static QByteArray assembleCompleteEml(const QString &rawHeader,
                                      const QString &plainBody,
                                      const QString &htmlBody,
                                      const QVector<AttachmentMeta> &atts,
                                      const QVector<QByteArray> &blobs)
{
    QByteArray out = rawHeader.toUtf8();
    if (!out.endsWith("\n"))
        out += "\r\n";

    const QByteArray boundary = "aerogram-" + QByteArray::number(
        qHash(rawHeader + plainBody + htmlBody), 36);
    const QString bodyCt = htmlBody.isEmpty()
        ? QStringLiteral("text/plain") : QStringLiteral("text/html");
    const QByteArray bodyBytes =
        (htmlBody.isEmpty() ? plainBody : htmlBody).toUtf8();

    if (atts.isEmpty()) {
        // No attachments: the body rides the original headers as-is.
        out += "\r\n";
        out += bodyBytes;
        out += "\r\n";
        return out;
    }

    // Replace the content description with multipart/mixed and embed.
    out += "\r\nMIME-Version: 1.0\r\n";
    out += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n";

    out += "--" + boundary + "\r\n";
    out += "Content-Type: " + bodyCt.toUtf8() + "; charset=utf-8\r\n";
    out += "Content-Transfer-Encoding: base64\r\n\r\n";
    out += bodyBytes.toBase64() + "\r\n";

    for (int i = 0; i < atts.size(); ++i) {
        const AttachmentMeta &a = atts.at(i);
        out += "--" + boundary + "\r\n";
        out += "Content-Type: " + a.mimeType.toUtf8()
             + "; name=\"" + a.filename.toUtf8() + "\"\r\n";
        out += "Content-Disposition: attachment; filename=\""
             + a.filename.toUtf8() + "\"\r\n";
        out += "Content-Transfer-Encoding: base64\r\n\r\n";
        out += blobs.value(i).toBase64() + "\r\n";
    }
    out += "--" + boundary + "--\r\n";
    return out;
}

void ProtonBackend::persistMessage(const QString &conversationId,
                                   const QString &messageId,
                                   const QJsonObject &apiMsg,
                                   const QString &plainBody)
{
    if (m_shuttingDown || m_key.isEmpty() || plainBody.isEmpty())
        return;

    const QJsonArray atts = apiMsg.value(QStringLiteral("attachments")).toArray();
    const QString html = apiMsg.value(QStringLiteral("html")).toString();

    if (atts.isEmpty()) {
        const QByteArray eml = assembleCompleteEml(
            apiMsg.value(QStringLiteral("header")).toString(), plainBody, html, {}, {});
        m_store.storeMessage(conversationId, eml, plainBody, {}, messageId);
        // The lean core has no SDK body cache to prune — ours is the
        // only copy.
        return;
    }

    // Attachments: fetch+decrypt each (bounded), then store complete.
    QList<QFuture<QJsonValue>> fetches;
    QVector<AttachmentMeta> metas;
    for (const auto &v : atts) {
        const QJsonObject o = v.toObject();
        AttachmentMeta meta;
        meta.index = metas.size();
        meta.filename = o.value(QStringLiteral("name")).toString();
        meta.mimeType = o.value(QStringLiteral("mime")).toString();
        meta.size = o.value(QStringLiteral("size")).toInteger();
        metas.append(meta);
        fetches.append(call(QStringLiteral("get_attachment"),
                            {{QStringLiteral("id"), o.value(QStringLiteral("id")).toString()}}));
    }

    QtFuture::whenAll(fetches.begin(), fetches.end())
        .then([this, conversationId, messageId, apiMsg, plainBody, html, metas](
                  QList<QFuture<QJsonValue>> results) {
            if (m_shuttingDown)
                return;
            QVector<QByteArray> blobs;
            blobs.reserve(results.size());
            for (auto &f : results) {
                const QString b64 =
                    f.result().toObject().value(QStringLiteral("bytes_base64")).toString();
                blobs.append(QByteArray::fromBase64(b64.toLatin1()));
            }
            const QByteArray eml = assembleCompleteEml(
                apiMsg.value(QStringLiteral("header")).toString(), plainBody, html,
                metas, blobs);
            m_store.storeMessage(conversationId, eml, plainBody, metas, messageId);
        })
        .onFailed([this, messageId](const std::exception &e) {
            // LOUD: a persist failure means this message NEVER reaches
            // the store — every future open pays a full network fetch.
            qWarning().noquote() << QStringLiteral("ProtonBackend: persist failed for %1: %2 (message will re-fetch on every open)")
                                        .arg(messageId, e.what());
            emit errorOccurred(QString::fromUtf8(e.what()));
        });
}

void ProtonBackend::wipeLocalStore()
{
    // Drop the live index connection BEFORE removing the files
    // (SQLite would keep writing to an unlinked inode otherwise).
    m_store.clearKey();
    if (!m_storeRoot.isEmpty())
        QDir(m_storeRoot).removeRecursively();
    const QString storeDir = QFileInfo(m_indexDb).absolutePath();
    if (!storeDir.isEmpty())
        QDir(storeDir).removeRecursively();
    QDir().mkpath(m_storeRoot);
    m_store.setKey(m_key);
}

void ProtonBackend::saveAttachment(const QString &messageId, int partIndex,
                                   const QString &destinationPath)
{
    // Attachments are embedded in the stored .eml (complete message) —
    // decode-on-save from the store, same as IMAP. Falls back to the
    // SDK only when the message isn't stored yet.
    const QString rel = m_store.filePathForMessage(messageId);
    const QByteArray raw = rel.isEmpty() ? QByteArray()
                                         : m_store.readShard(rel);
    if (!raw.isEmpty()) {
        const QByteArray bytes = ContentPipeline::extractAttachment(raw, partIndex);
        if (!bytes.isEmpty()) {
            QFile out(destinationPath);
            const bool ok = out.open(QIODevice::WriteOnly)
                         && out.write(bytes) == bytes.size();
            emit attachmentSaved(ok, messageId, ok ? destinationPath : QString());
            return;
        }
    }
    emit attachmentSaved(false, messageId, QString());
}

QString ProtonBackend::rawMessageSource(const QString &messageId)
{
    return QString::fromUtf8(m_store.readRawEml(messageId));
}
