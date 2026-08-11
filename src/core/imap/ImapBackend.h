#ifndef IMAPBACKEND_H
#define IMAPBACKEND_H

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureSynchronizer>
#include <QMetaObject>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>

#include <atomic>

#include "core/Types.h"
#include "core/plugin/BackendPlugin.h"
#include "core/plugin/Capabilities.h"
#include "CurlTransport.h"
#include "MessageStore.h"
#include "MetadataIndex.h"
#include "MimeParser.h"

/// \brief IMAP backend prototype: libcurl transport + minimal MIME
///        parser + hash-sharded .eml storage + SQLite FTS5 index.
///
/// Conversation mapping: one IMAP folder = one Conversation
/// (kind="folder"). All blocking work (network, DB, file I/O) runs in
/// QtConcurrent workers; results are delivered to the UI thread via
/// queued QMetaObject::invokeMethod. Signals are emitted on this
/// (main-thread) object so receivers use standard AutoConnection.
class ImapBackend : public BackendPlugin,
                    public IConversationProvider,
                    public IMessageProvider,
                    public ICredentialsSetup,
                    public IMasterKeyAware,
                    public ISyncable
{
    Q_OBJECT

public:
    explicit ImapBackend(QObject *parent = nullptr)
        : BackendPlugin(parent)
        , m_pollTimer(new QTimer(this))
    {
        m_pollTimer->setInterval(60 * 1000);
        connect(m_pollTimer, &QTimer::timeout, this, &ImapBackend::syncNow);
    }

    ~ImapBackend() override { shutdown(); }

    QString name() const override { return QStringLiteral("imap"); }

    QString family() const override { return QStringLiteral("email"); }

    // -----------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------

    bool initialize(const QVariantMap &params) override
    {
        // shutdown() is not terminal: resetApp() calls shutdown() +
        // initialize() on the same instance. Re-arm the lifecycle flags.
        m_shuttingDown = false;
        m_syncInFlight = false;

        // Storage paths are computed in configure() once the account
        // identity (user@host) is known — per-account separation.
        m_baseRoot = params.value(QStringLiteral("storage_root")).toString();
        if (m_baseRoot.isEmpty()) {
            m_baseRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                       + QStringLiteral("/imap");
        }
        return true;
    }

    void shutdown() override
    {
        // Stop scheduling new work, then wait for in-flight workers so
        // nothing captures a dangling `this` after destruction.
        m_shuttingDown = true;
        if (m_pollTimer->isActive())
            m_pollTimer->stop();
        m_workers.waitForFinished();
    }

    // -----------------------------------------------------------------
    // ICredentialsSetup
    // -----------------------------------------------------------------

    void configure(const QVariantMap &credentials) override
    {
        m_host = credentials.value(QStringLiteral("host")).toString();
        m_port = credentials.value(QStringLiteral("port"), 1143).toInt();
        m_user = credentials.value(QStringLiteral("user")).toString();
        m_pass = credentials.value(QStringLiteral("pass")).toString();
        m_tls = credentials.value(QStringLiteral("tls"), false).toBool();

        // Per-account storage: <base>/<user@host>/{storage,index.db}.
        QString accountKey = m_user + QLatin1Char('@') + m_host;
        accountKey.replace(QLatin1Char('/'), QLatin1Char('_'));
        const QString root = m_baseRoot + QLatin1Char('/') + accountKey;
        QDir().mkpath(root + QStringLiteral("/storage"));
        m_storageRoot = root + QStringLiteral("/storage");
        m_dbPath = root + QStringLiteral("/index.db");
        m_accountLabel = accountKey;

        emit configured(true);
    }

    /// \brief Provide the master key (post-unlock). Storage is
    ///        encrypted at rest; without the key all storage reads
    ///        fail closed. Called by the controller after unlock.
    void setMasterKey(const QByteArray &key)
    {
        m_key = key;
    }

    /// \brief Rotation mode A helper: stop polling and delete this
    ///        account's encrypted store. Resync rebuilds from the
    ///        server. Call with IO stopped; the controller then hands
    ///        the new key via setMasterKey + startIo.
    void wipeLocalStore()
    {
        m_pollTimer->stop();
        if (!m_storageRoot.isEmpty()) {
            // m_storageRoot = <accountDir>/storage; wipe the account dir.
            const QString accountDir = QFileInfo(m_storageRoot).absolutePath();
            QDir(accountDir).removeRecursively();
            QDir().mkpath(m_storageRoot);
        }
    }

    /// Account removal: delete the whole per-account store (encrypted
    /// shards + index.db), no recreate.
    void purgeLocalData() override
    {
        m_pollTimer->stop();
        if (!m_storageRoot.isEmpty()) {
            const QString accountDir = QFileInfo(m_storageRoot).absolutePath();
            QDir(accountDir).removeRecursively();
        }
    }

    // -----------------------------------------------------------------
    // IO control
    // -----------------------------------------------------------------

    void startIo() override
    {
        if (m_shuttingDown) {
            emit ioStarted(false, QStringLiteral("backend is shutting down"));
            return;
        }
        // Verify credentials with a LIST before declaring IO started.
        emit setupProgress(QStringLiteral("Verifying server connection…"));
        const QString host = m_host, user = m_user, pass = m_pass;
        const int port = m_port;
        const bool tls = m_tls;

        track(QtConcurrent::run([this, host, port, user, pass, tls]() {
            if (m_shuttingDown) return;
            CurlTransport t(host, port, user, pass, tls);
            QStringList folders;
            QString err;
            const bool ok = t.listFolders(folders, &err);
            QMetaObject::invokeMethod(this, [this, ok, err]() {
                if (ok) {
                    emit ioStarted(true, QString());
                    m_pollTimer->start();
                    syncNow();
                } else {
                    emit ioStarted(false, err);
                }
            }, Qt::QueuedConnection);
        }));
    }

    void stopIo() override
    {
        m_pollTimer->stop();
        emit ioStopped();
    }

    // -----------------------------------------------------------------
    // IConversationProvider
    // -----------------------------------------------------------------

    void fetchConversations() override
    {
        const QString dbPath = m_dbPath;
        const QByteArray key = m_key;
        track(QtConcurrent::run([this, dbPath, key]() {
            if (m_shuttingDown) return;
            // Fail closed: without the key, never touch the DB — opening
            // it keyless would create a plaintext index.db that later
            // encrypted opens reject (HMAC failure).
            if (key.isEmpty()) {
                QMetaObject::invokeMethod(this, [this]() {
                    emit conversationsReady({});
                }, Qt::QueuedConnection);
                return;
            }
            QVector<Conversation> convs;
            {
                MetadataIndex idx(dbPath, key);
                QString err;
                if (idx.open(&err))
                    convs = idx.conversations();
            }
            QMetaObject::invokeMethod(this, [this, convs]() {
                emit conversationsReady(convs);
            }, Qt::QueuedConnection);
        }));
    }

    // -----------------------------------------------------------------
    // IMessageProvider
    // -----------------------------------------------------------------

    void fetchMessages(const QString &conversationId) override
    {
        const QString dbPath = m_dbPath;
        const QByteArray key = m_key;
        track(QtConcurrent::run([this, dbPath, key, conversationId]() {
            if (m_shuttingDown) return;
            QVector<Message> msgs;
            if (key.isEmpty()) {
                QMetaObject::invokeMethod(this, [this, conversationId]() {
                    emit messagesReady(conversationId, {});
                }, Qt::QueuedConnection);
                return;
            }
            {
                MetadataIndex idx(dbPath, key);
                QString err;
                if (idx.open(&err))
                    msgs = idx.messages(conversationId);
            }
            QMetaObject::invokeMethod(this, [this, conversationId, msgs]() {
                qInfo() << "ImapBackend: messagesReady" << conversationId << msgs.size() << "messages";
                emit messagesReady(conversationId, msgs);
            }, Qt::QueuedConnection);
        }));
    }

    void fetchMessageBody(const QString &conversationId, const QString &messageId) override
    {
        const QString dbPath = m_dbPath;
        const QString storageRoot = m_storageRoot;
        const QByteArray key = m_key;
        track(QtConcurrent::run([this, dbPath, storageRoot, key, conversationId, messageId]() {
            if (m_shuttingDown) return;
            QString body;
            QString rel;
            if (key.isEmpty()) {
                QMetaObject::invokeMethod(this, [this, conversationId, messageId]() {
                    emit messageBodyReady(conversationId, messageId, QString(),
                                          QString(), false);
                }, Qt::QueuedConnection);
                return;
            }
            {
                MetadataIndex idx(dbPath, key);
                QString err;
                if (idx.open(&err))
                    rel = idx.filePathForMessage(messageId);
            }
            if (!rel.isEmpty()) {
                MessageStore store(storageRoot, key);
                const QByteArray raw = store.get(rel);
                qInfo() << "ImapBackend: body fetch" << messageId << "raw" << raw.size() << "bytes";
                if (!raw.isEmpty())
                    body = MimeParser::parse(raw).bodyPlain;
            } else {
                qInfo() << "ImapBackend: body fetch" << messageId << "no file_path in index";
            }
            QMetaObject::invokeMethod(this, [this, conversationId, messageId, body]() {
                qInfo() << "ImapBackend: messageBodyReady" << messageId << body.size() << "chars";
                emit messageBodyReady(conversationId, messageId, body,
                                      QString(), false);
            }, Qt::QueuedConnection);
        }));
    }

    // -----------------------------------------------------------------
    // IMessageProvider — attachments (decode-on-save, no app-side copy)
    // -----------------------------------------------------------------

    void saveAttachment(const QString &messageId, int partIndex,
                        const QString &destinationPath) override
    {
        const QString dbPath = m_dbPath;
        const QString storageRoot = m_storageRoot;
        const QByteArray key = m_key;
        const QString dest = destinationPath;
        track(QtConcurrent::run([this, dbPath, storageRoot, key, messageId, partIndex, dest]() {
            if (m_shuttingDown) return;
            bool ok = false;
            QString rel;
            if (key.isEmpty()) {
                QMetaObject::invokeMethod(this, [this, messageId, dest]() {
                    emit attachmentSaved(false, messageId, dest);
                }, Qt::QueuedConnection);
                return;
            }
            {
                MetadataIndex idx(dbPath, key);
                QString err;
                if (idx.open(&err))
                    rel = idx.filePathForMessage(messageId);
            }
            if (!rel.isEmpty()) {
                MessageStore store(storageRoot, key);
                const QByteArray raw = store.get(rel);
                if (!raw.isEmpty()) {
                    const QByteArray bytes = MimeParser::extractAttachment(raw, partIndex);
                    if (!bytes.isEmpty()) {
                        QFile out(dest);
                        if (out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size())
                            ok = true;
                    }
                }
            }
            QMetaObject::invokeMethod(this, [this, ok, messageId, dest]() {
                qInfo() << "ImapBackend: attachmentSaved" << ok << messageId << dest;
                emit attachmentSaved(ok, messageId, dest);
            }, Qt::QueuedConnection);
        }));
    }

private slots:
    void syncNow() override
    {
        if (m_syncInFlight)
            return;
        if (m_key.isEmpty()) {
            qInfo() << "ImapBackend: sync skipped (locked, no master key)";
            return;
        }
        m_syncInFlight = true;

        const QString host = m_host, user = m_user, pass = m_pass;
        const int port = m_port;
        const bool tls = m_tls;
        const QString storageRoot = m_storageRoot;
        const QString dbPath = m_dbPath;
        const QString accountLabel = m_accountLabel;
        const QByteArray key = m_key;

        track(QtConcurrent::run([this, host, port, user, pass, tls, storageRoot, dbPath,
                           accountLabel, key]() {
            if (m_shuttingDown) return;
            QString err;
            const QVector<Conversation> convs = syncWorker(host, port, user, pass, tls,
                                                           storageRoot, dbPath, accountLabel,
                                                           key, std::cref(m_shuttingDown), &err);
            QMetaObject::invokeMethod(this, [this, convs, err]() {
                m_syncInFlight = false;
                if (!err.isEmpty()) {
                    qWarning() << "ImapBackend: sync failed:" << err;
                    emit errorOccurred(err);
                    return;
                }
                qInfo() << "ImapBackend: sync complete," << convs.size() << "conversations";
                emit conversationsReady(convs);
                // Demo default: surface INBOX messages immediately so
                // the email view is populated without manual selection.
                for (const Conversation &c : convs) {
                    if (c.name.compare(QStringLiteral("INBOX"), Qt::CaseInsensitive) == 0) {
                        fetchMessages(c.id);
                        break;
                    }
                }
            }, Qt::QueuedConnection);
        }));
    }

private:
    /// \brief Blocking sync worker. Runs on a QThreadPool thread.
    static QVector<Conversation> syncWorker(const QString &host, int port,
                                            const QString &user, const QString &pass,
                                            bool tls, const QString &storageRoot,
                                            const QString &dbPath,
                                            const QString &accountLabel,
                                            const QByteArray &key,
                                            const std::atomic<bool> &shuttingDown,
                                            QString *errOut)
    {
        CurlTransport t(host, port, user, pass, tls);

        QStringList folders;
        QString err;
        if (!t.listFolders(folders, &err)) {
            if (errOut) *errOut = err;
            return {};
        }

        MessageStore store(storageRoot, key);

        {
            MetadataIndex idx(dbPath, key);
            if (!idx.open(&err)) {
                if (errOut) *errOut = err;
                return {};
            }

            for (const QString &folder : folders) {
                if (shuttingDown) break;          // checkpoint between folders
                QList<int> uids;
                if (!t.uidSearchAll(folder, uids, &err))
                    continue;

                QVector<Message> msgs;
                QVector<QString> bodies;
                QVector<QString> paths;
                QVector<QVector<AttachmentMeta>> atts;
                msgs.reserve(uids.size());
                bodies.reserve(uids.size());
                paths.reserve(uids.size());
                atts.reserve(uids.size());

                qint64 newest = 0;
                int unseen = 0;
                QString newestSubject;
                bool folderComplete = true;   // no shutdown break mid-folder
                bool fetchFailed = false;     // no transient fetch errors

                QSet<QString> presentIds;
                for (const int uid : uids) {
                    if (shuttingDown) { folderComplete = false; break; }
                    QByteArray raw;
                    bool seen = false;
                    if (!t.fetchMessage(folder, uid, raw, seen, &err) || raw.isEmpty()) {
                        fetchFailed = true;   // transient: don't reconcile
                        continue;
                    }

                    const MimeParser::ParsedMessage parsed = MimeParser::parse(raw);

                    Message m;
                    m.messageId = !parsed.messageId.isEmpty()
                                ? parsed.messageId
                                : folder + QStringLiteral(":") + QString::number(uid);
                    m.conversationId = folder;
                    m.subject = parsed.subject;
                    m.sender = parsed.sender;
                    m.date = parsed.date;
                    m.snippet = MimeParser::snippetFrom(parsed.bodyPlain);
                    m.isUnread = !seen;

                    msgs.append(m);
                    bodies.append(parsed.bodyPlain);
                    paths.append(store.put(raw, m.messageId));
                    atts.append(MimeParser::listAttachments(raw));
                    presentIds.insert(m.messageId);

                    if (m.isUnread) ++unseen;
                    const qint64 secs = m.date.toSecsSinceEpoch();
                    if (secs > newest) {
                        newest = secs;
                        newestSubject = m.subject;
                    }
                }

                idx.insertMessages(msgs, bodies, paths, atts);

                // Deletion sync: the server's listing is the truth.
                // Only reconcile a folder we listed COMPLETELY with no
                // transient fetch failures — anything less would delete
                // mail we merely failed to read this pass.
                if (folderComplete && !fetchFailed) {
                    const QVector<QString> gone =
                        idx.removeMissingFromConversation(folder, presentIds);
                    for (const QString &rel : gone)
                        store.remove(rel);
                    if (!gone.isEmpty())
                        qInfo() << "ImapBackend: reconciled" << folder
                                << "— removed" << gone.size() << "deleted message(s)";
                }

                Conversation c;
                c.id = folder;
                c.kind = QStringLiteral("folder");
                c.name = folder;
                c.preview = newestSubject;
                c.accountLabel = accountLabel;
                c.unreadCount = unseen;
                c.lastActivity = newest > 0
                               ? QDateTime::fromSecsSinceEpoch(newest)
                               : QDateTime::currentDateTime();
                idx.upsertConversation(c);
            }

            return idx.conversations();
        }
    }

    QString m_host;
    int m_port = 1143;
    QString m_user;
    QString m_pass;
    bool m_tls = false;
    QString m_baseRoot;
    QString m_storageRoot;
    QString m_dbPath;
    QString m_accountLabel;
    QByteArray m_key;
    /// \brief Register a worker future. QFutureSynchronizer has no
    ///        per-future removal, so clear the list wholesale whenever
    ///        nothing is in flight — otherwise it grows without bound
    ///        (~1.4k futures/day idle at 60s polling).
    void track(QFuture<void> f)
    {
        const auto fs = m_workers.futures();
        if (std::all_of(fs.cbegin(), fs.cend(),
                        [](const QFuture<void> &w) { return w.isFinished(); }))
            m_workers.clearFutures();
        m_workers.addFuture(f);
    }

    QTimer *m_pollTimer;
    QFutureSynchronizer<void> m_workers;
    std::atomic<bool> m_shuttingDown{false};
    bool m_syncInFlight = false;
};

#endif
