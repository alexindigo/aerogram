#ifndef IMAPBACKEND_H
#define IMAPBACKEND_H

#include <QDateTime>
#include <QDir>
#include <QFile>
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
                    public ICredentialsSetup
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

    // -----------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------

    bool initialize(const QVariantMap &params) override
    {
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
        if (m_pollTimer->isActive())
            m_pollTimer->stop();
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

    // -----------------------------------------------------------------
    // IO control
    // -----------------------------------------------------------------

    void startIo() override
    {
        // Verify credentials with a LIST before declaring IO started.
        const QString host = m_host, user = m_user, pass = m_pass;
        const int port = m_port;
        const bool tls = m_tls;

        QtConcurrent::run([this, host, port, user, pass, tls]() {
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
        });
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
        QtConcurrent::run([this, dbPath]() {
            QVector<Conversation> convs;
            {
                MetadataIndex idx(dbPath, connectionName());
                QString err;
                if (idx.open(&err))
                    convs = idx.conversations();
            }
            QMetaObject::invokeMethod(this, [this, convs]() {
                emit conversationsReady(convs);
            }, Qt::QueuedConnection);
        });
    }

    // -----------------------------------------------------------------
    // IMessageProvider
    // -----------------------------------------------------------------

    void fetchMessages(const QString &conversationId) override
    {
        const QString dbPath = m_dbPath;
        QtConcurrent::run([this, dbPath, conversationId]() {
            QVector<Message> msgs;
            {
                MetadataIndex idx(dbPath, connectionName());
                QString err;
                if (idx.open(&err))
                    msgs = idx.messages(conversationId);
            }
            QMetaObject::invokeMethod(this, [this, conversationId, msgs]() {
                qInfo() << "ImapBackend: messagesReady" << conversationId << msgs.size() << "messages";
                emit messagesReady(conversationId, msgs);
            }, Qt::QueuedConnection);
        });
    }

    void fetchMessageBody(const QString &conversationId, const QString &messageId) override
    {
        const QString dbPath = m_dbPath;
        const QString storageRoot = m_storageRoot;
        QtConcurrent::run([this, dbPath, storageRoot, conversationId, messageId]() {
            QString body;
            QString rel;
            {
                MetadataIndex idx(dbPath, connectionName());
                QString err;
                if (idx.open(&err))
                    rel = idx.filePathForMessage(messageId);
            }
            if (!rel.isEmpty()) {
                MessageStore store(storageRoot);
                const QByteArray raw = store.get(rel);
                if (!raw.isEmpty())
                    body = MimeParser::parse(raw).bodyPlain;
            }
            QMetaObject::invokeMethod(this, [this, conversationId, messageId, body]() {
                qInfo() << "ImapBackend: messageBodyReady" << messageId << body.size() << "chars";
                emit messageBodyReady(conversationId, messageId, body);
            }, Qt::QueuedConnection);
        });
    }

    // -----------------------------------------------------------------
    // IMessageProvider — attachments (decode-on-save, no app-side copy)
    // -----------------------------------------------------------------

    void saveAttachment(const QString &messageId, int partIndex,
                        const QString &destinationPath) override
    {
        const QString dbPath = m_dbPath;
        const QString storageRoot = m_storageRoot;
        const QString dest = destinationPath;
        QtConcurrent::run([this, dbPath, storageRoot, messageId, partIndex, dest]() {
            bool ok = false;
            QString rel;
            {
                MetadataIndex idx(dbPath, connectionName());
                QString err;
                if (idx.open(&err))
                    rel = idx.filePathForMessage(messageId);
            }
            if (!rel.isEmpty()) {
                MessageStore store(storageRoot);
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
        });
    }

private slots:
    void syncNow()
    {
        if (m_syncInFlight)
            return;
        m_syncInFlight = true;

        const QString host = m_host, user = m_user, pass = m_pass;
        const int port = m_port;
        const bool tls = m_tls;
        const QString storageRoot = m_storageRoot;
        const QString dbPath = m_dbPath;
        const QString accountLabel = m_accountLabel;

        QtConcurrent::run([this, host, port, user, pass, tls, storageRoot, dbPath,
                           accountLabel]() {
            QString err;
            const QVector<Conversation> convs = syncWorker(host, port, user, pass, tls,
                                                           storageRoot, dbPath, accountLabel,
                                                           &err);
            QMetaObject::invokeMethod(this, [this, convs, err]() {
                m_syncInFlight = false;
                if (!err.isEmpty()) {
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
        });
    }

private:
    static QString connectionName()
    {
        static std::atomic<int> counter{0};
        return QStringLiteral("imap-%1").arg(++counter);
    }

    /// \brief Blocking sync worker. Runs on a QThreadPool thread.
    static QVector<Conversation> syncWorker(const QString &host, int port,
                                            const QString &user, const QString &pass,
                                            bool tls, const QString &storageRoot,
                                            const QString &dbPath,
                                            const QString &accountLabel,
                                            QString *errOut)
    {
        CurlTransport t(host, port, user, pass, tls);

        QStringList folders;
        QString err;
        if (!t.listFolders(folders, &err)) {
            if (errOut) *errOut = err;
            return {};
        }

        MessageStore store(storageRoot);

        {
            MetadataIndex idx(dbPath, connectionName());
            if (!idx.open(&err)) {
                if (errOut) *errOut = err;
                return {};
            }

            for (const QString &folder : folders) {
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

                for (const int uid : uids) {
                    QByteArray raw;
                    bool seen = false;
                    if (!t.fetchMessage(folder, uid, raw, seen, &err) || raw.isEmpty())
                        continue;

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

                    if (m.isUnread) ++unseen;
                    const qint64 secs = m.date.toSecsSinceEpoch();
                    if (secs > newest) {
                        newest = secs;
                        newestSubject = m.subject;
                    }
                }

                idx.insertMessages(msgs, bodies, paths, atts);

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
    QTimer *m_pollTimer;
    bool m_syncInFlight = false;
};

#endif
