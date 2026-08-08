#ifndef METADATAINDEX_H
#define METADATAINDEX_H

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVector>

#include "core/Types.h"

/// \brief SQLite metadata + FTS5 index for IMAP messages.
///
/// WAL mode with synchronous=NORMAL: background sync writes never
/// block UI reads. Construct per operation with a unique connection
/// name (SQLite connections are thread-confined); the destructor
/// closes and removes the named connection.
///
/// Schema:
///   conversations(id, kind, name, preview, unread_count, last_activity)
///   messages(id, message_id UNIQUE, conversation_id, sender, subject,
///            date, snippet, is_unread, file_path)
///   messages_fts(subject, sender, body_plaintext)  -- rowid == messages.id
class MetadataIndex
{
public:
    MetadataIndex(QString dbPath, QString connectionName)
        : m_dbPath(std::move(dbPath))
        , m_connName(std::move(connectionName))
    {
    }

    ~MetadataIndex()
    {
        if (m_db.isValid()) {
            m_db.close();
            m_db = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(m_connName);
    }

    bool open(QString *err = nullptr)
    {
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connName);
        m_db.setDatabaseName(m_dbPath);
        if (!m_db.open()) {
            if (err) *err = m_db.lastError().text();
            return false;
        }
        exec(QStringLiteral("PRAGMA journal_mode=WAL;"));
        exec(QStringLiteral("PRAGMA synchronous=NORMAL;"));
        exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS conversations ("
            "  id TEXT PRIMARY KEY,"
            "  kind TEXT NOT NULL,"
            "  name TEXT NOT NULL,"
            "  preview TEXT DEFAULT '',"
            "  unread_count INTEGER DEFAULT 0,"
            "  last_activity INTEGER DEFAULT 0"
            ");"));
        exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS messages ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  message_id TEXT UNIQUE,"
            "  conversation_id TEXT NOT NULL,"
            "  sender TEXT DEFAULT '',"
            "  subject TEXT DEFAULT '',"
            "  date INTEGER DEFAULT 0,"
            "  snippet TEXT DEFAULT '',"
            "  is_unread INTEGER DEFAULT 1,"
            "  file_path TEXT DEFAULT ''"
            ");"));
        exec(QStringLiteral(
            "CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5("
            "  subject, sender, body_plaintext"
            ");"));
        exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS attachments ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
            "  part_index INTEGER NOT NULL,"
            "  filename TEXT DEFAULT '',"
            "  mime_type TEXT DEFAULT '',"
            "  size INTEGER DEFAULT 0"
            ");"));
        return true;
    }

    void upsertConversation(const Conversation &c)
    {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "INSERT INTO conversations (id, kind, name, preview, unread_count, last_activity)"
            " VALUES (:id, :kind, :name, :preview, :unread, :activity)"
            " ON CONFLICT(id) DO UPDATE SET"
            "   preview = excluded.preview,"
            "   unread_count = excluded.unread_count,"
            "   last_activity = excluded.last_activity;"));
        q.bindValue(QStringLiteral(":id"), c.id);
        q.bindValue(QStringLiteral(":kind"), c.kind);
        q.bindValue(QStringLiteral(":name"), c.name);
        q.bindValue(QStringLiteral(":preview"), c.preview);
        q.bindValue(QStringLiteral(":unread"), c.unreadCount);
        q.bindValue(QStringLiteral(":activity"), c.lastActivity.toSecsSinceEpoch());
        q.exec();
    }

    /// \brief Batch insert in a single transaction. Existing
    ///        message_ids are ignored; only newly inserted rows also
    ///        land in the FTS table and attachments table.
    void insertMessages(const QVector<Message> &msgs,
                        const QVector<QString> &bodies,
                        const QVector<QString> &paths,
                        const QVector<QVector<AttachmentMeta>> &attachments)
    {
        m_db.transaction();

        QSqlQuery ins(m_db);
        ins.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO messages"
            " (message_id, conversation_id, sender, subject, date, snippet, is_unread, file_path)"
            " VALUES (:mid, :cid, :sender, :subject, :date, :snippet, :unread, :path);"));

        QSqlQuery sel(m_db);
        sel.prepare(QStringLiteral("SELECT id FROM messages WHERE message_id = :mid;"));

        QSqlQuery fts(m_db);
        fts.prepare(QStringLiteral(
            "INSERT INTO messages_fts (rowid, subject, sender, body_plaintext)"
            " VALUES (:rowid, :subject, :sender, :body);"));

        QSqlQuery att(m_db);
        att.prepare(QStringLiteral(
            "INSERT INTO attachments (message_id, part_index, filename, mime_type, size)"
            " VALUES (:dbid, :pindex, :filename, :mime, :size);"));

        for (int i = 0; i < msgs.size(); ++i) {
            const Message &m = msgs.at(i);
            ins.bindValue(QStringLiteral(":mid"), m.messageId);
            ins.bindValue(QStringLiteral(":cid"), m.conversationId);
            ins.bindValue(QStringLiteral(":sender"), m.sender);
            ins.bindValue(QStringLiteral(":subject"), m.subject);
            ins.bindValue(QStringLiteral(":date"), m.date.toSecsSinceEpoch());
            ins.bindValue(QStringLiteral(":snippet"), m.snippet);
            ins.bindValue(QStringLiteral(":unread"), m.isUnread ? 1 : 0);
            ins.bindValue(QStringLiteral(":path"), paths.value(i));
            if (!ins.exec())
                continue;

            if (ins.numRowsAffected() > 0) {
                sel.bindValue(QStringLiteral(":mid"), m.messageId);
                if (sel.exec() && sel.next()) {
                    const QVariant dbId = sel.value(0);

                    fts.bindValue(QStringLiteral(":rowid"), dbId);
                    fts.bindValue(QStringLiteral(":subject"), m.subject);
                    fts.bindValue(QStringLiteral(":sender"), m.sender);
                    fts.bindValue(QStringLiteral(":body"), bodies.value(i));
                    if (!fts.exec())
                        qWarning() << "MetadataIndex: FTS insert failed:"
                                   << fts.lastError().text();

                    const QVector<AttachmentMeta> meta = attachments.value(i);
                    for (const AttachmentMeta &a : meta) {
                        att.bindValue(QStringLiteral(":dbid"), dbId);
                        att.bindValue(QStringLiteral(":pindex"), a.index);
                        att.bindValue(QStringLiteral(":filename"), a.filename);
                        att.bindValue(QStringLiteral(":mime"), a.mimeType);
                        att.bindValue(QStringLiteral(":size"), a.size);
                        if (!att.exec())
                            qWarning() << "MetadataIndex: attachment insert failed:"
                                       << att.lastError().text();
                        att.finish();
                    }
                } else {
                    qWarning() << "MetadataIndex: id lookup failed for" << m.messageId
                               << sel.lastError().text();
                }
                sel.finish();
            }
            ins.finish();
        }

        m_db.commit();
    }

    QVector<Conversation> conversations()
    {
        QVector<Conversation> out;
        QSqlQuery q(m_db);
        if (!q.exec(QStringLiteral(
                "SELECT id, kind, name, preview, unread_count, last_activity"
                " FROM conversations ORDER BY last_activity DESC;")))
            return out;

        while (q.next()) {
            Conversation c;
            c.id = q.value(0).toString();
            c.kind = q.value(1).toString();
            c.name = q.value(2).toString();
            c.preview = q.value(3).toString();
            c.unreadCount = q.value(4).toInt();
            c.lastActivity = QDateTime::fromSecsSinceEpoch(q.value(5).toLongLong());
            out.append(c);
        }
        return out;
    }

    QVector<Message> messages(const QString &conversationId)
    {
        QVector<Message> out;
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "SELECT id, message_id, conversation_id, sender, subject, date, snippet, is_unread"
            " FROM messages WHERE conversation_id = :cid ORDER BY date DESC;"));
        q.bindValue(QStringLiteral(":cid"), conversationId);
        if (!q.exec())
            return out;

        QSqlQuery att(m_db);
        att.prepare(QStringLiteral(
            "SELECT part_index, filename, mime_type, size FROM attachments"
            " WHERE message_id = :dbid ORDER BY part_index;"));

        while (q.next()) {
            Message m;
            const QVariant dbId = q.value(0);
            m.messageId = q.value(1).toString();
            m.conversationId = q.value(2).toString();
            m.sender = q.value(3).toString();
            m.subject = q.value(4).toString();
            m.date = QDateTime::fromSecsSinceEpoch(q.value(5).toLongLong());
            m.snippet = q.value(6).toString();
            m.isUnread = q.value(7).toInt() != 0;

            att.bindValue(QStringLiteral(":dbid"), dbId);
            if (att.exec()) {
                while (att.next()) {
                    AttachmentMeta a;
                    a.index = att.value(0).toInt();
                    a.filename = att.value(1).toString();
                    a.mimeType = att.value(2).toString();
                    a.size = att.value(3).toLongLong();
                    m.attachments.append(a);
                }
            }
            att.finish();
            out.append(m);
        }
        return out;
    }

    QString filePathForMessage(const QString &messageId)
    {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("SELECT file_path FROM messages WHERE message_id = :mid;"));
        q.bindValue(QStringLiteral(":mid"), messageId);
        if (q.exec() && q.next())
            return q.value(0).toString();
        return {};
    }

private:
    void exec(const QString &sql)
    {
        QSqlQuery q(m_db);
        q.exec(sql);
    }

    QString m_dbPath;
    QString m_connName;
    QSqlDatabase m_db;
};

#endif
