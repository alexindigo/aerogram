#ifndef METADATAINDEX_H
#define METADATAINDEX_H

#include <QDateTime>
#include <QDebug>
#include <QString>
#include <QVector>

#include <sodium.h>
#include <sqlcipher/sqlite3.h>

#include "core/Types.h"

/// \brief SQLCipher-encrypted metadata + FTS5 index for IMAP messages.
///
/// Raw sqlite3 C API (not QtSql) so the database key can be applied via
/// sqlite3_key() directly. A handle is just an object: construct per
/// operation, no connection-name registry. WAL + synchronous=NORMAL.
/// Key material is zeroed on destruction.
///
/// Schema:
///   conversations(id, kind, name, preview, unread_count, last_activity)
///   messages(id, message_id UNIQUE, conversation_id, sender, subject,
///            date, snippet, is_unread, file_path)
///   messages_fts(subject, sender, body_plaintext)  -- rowid == messages.id
///   attachments(id, message_id -> messages.id, part_index, filename,
///               mime_type, size)
class MetadataIndex
{
public:
    MetadataIndex(QString dbPath, QByteArray key)
        : m_dbPath(std::move(dbPath))
        , m_key(std::move(key))
    {
    }

    ~MetadataIndex()
    {
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        if (!m_key.isEmpty()) {
            sodium_memzero(m_key.data(), m_key.size());
            m_key.clear();
        }
    }

    bool open(QString *err = nullptr)
    {
        if (sqlite3_open(m_dbPath.toUtf8().constData(), &m_db) != SQLITE_OK) {
            if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
            return false;
        }
        if (!m_key.isEmpty()) {
            sqlite3_key(m_db, m_key.constData(), static_cast<int>(m_key.size()));
        }

        // Multiple workers (fetchConversations + sync) open the same
        // file concurrently at unlock time; without a busy timeout the
        // loser of a schema-creation race gets SQLITE_BUSY immediately.
        sqlite3_busy_timeout(m_db, 5000);

        // Verify the key (or plaintext state) with a probe query before
        // touching the schema.
        if (!exec("SELECT count(*) FROM sqlite_master;", err))
            return false;

        if (!exec("PRAGMA journal_mode=WAL;", err)
                || !exec("PRAGMA synchronous=NORMAL;", err))
            return false;

        return exec("CREATE TABLE IF NOT EXISTS conversations ("
                    "  id TEXT PRIMARY KEY,"
                    "  kind TEXT NOT NULL,"
                    "  name TEXT NOT NULL,"
                    "  preview TEXT DEFAULT '',"
                    "  unread_count INTEGER DEFAULT 0,"
                    "  last_activity INTEGER DEFAULT 0"
                    ");", err)
            && exec("CREATE TABLE IF NOT EXISTS messages ("
                    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "  message_id TEXT UNIQUE,"
                    "  conversation_id TEXT NOT NULL,"
                    "  sender TEXT DEFAULT '',"
                    "  subject TEXT DEFAULT '',"
                    "  date INTEGER DEFAULT 0,"
                    "  snippet TEXT DEFAULT '',"
                    "  is_unread INTEGER DEFAULT 1,"
                    "  file_path TEXT DEFAULT ''"
                    ");", err)
            && exec("CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5("
                    "  subject, sender, body_plaintext"
                    ");", err)
            && exec("CREATE TABLE IF NOT EXISTS attachments ("
                    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "  message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
                    "  part_index INTEGER NOT NULL,"
                    "  filename TEXT DEFAULT '',"
                    "  mime_type TEXT DEFAULT '',"
                    "  size INTEGER DEFAULT 0"
                    ");", err);
    }

    void upsertConversation(const Conversation &c)
    {
        const char *sql =
            "INSERT INTO conversations (id, kind, name, preview, unread_count, last_activity)"
            " VALUES (?, ?, ?, ?, ?, ?)"
            " ON CONFLICT(id) DO UPDATE SET"
            "   preview = excluded.preview,"
            "   unread_count = excluded.unread_count,"
            "   last_activity = excluded.last_activity;";
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
            return;
        bindText(st, 1, c.id);
        bindText(st, 2, c.kind);
        bindText(st, 3, c.name);
        bindText(st, 4, c.preview);
        sqlite3_bind_int(st, 5, c.unreadCount);
        sqlite3_bind_int64(st, 6, c.lastActivity.toSecsSinceEpoch());
        if (sqlite3_step(st) != SQLITE_DONE)
            qWarning() << "MetadataIndex: upsertConversation failed:"
                       << sqlite3_errmsg(m_db);
        sqlite3_finalize(st);
    }

    /// \brief Batch insert in a single transaction. Existing
    ///        message_ids are ignored; only newly inserted rows also
    ///        land in the FTS and attachments tables.
    void insertMessages(const QVector<Message> &msgs,
                        const QVector<QString> &bodies,
                        const QVector<QString> &paths,
                        const QVector<QVector<AttachmentMeta>> &attachments)
    {
        exec("BEGIN TRANSACTION;");

        sqlite3_stmt *ins = nullptr;
        sqlite3_stmt *sel = nullptr;
        sqlite3_stmt *fts = nullptr;
        sqlite3_stmt *att = nullptr;

        sqlite3_prepare_v2(m_db,
            "INSERT OR IGNORE INTO messages"
            " (message_id, conversation_id, sender, subject, date, snippet, is_unread, file_path)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?);", -1, &ins, nullptr);
        sqlite3_prepare_v2(m_db,
            "SELECT id FROM messages WHERE message_id = ?;", -1, &sel, nullptr);
        sqlite3_prepare_v2(m_db,
            "INSERT INTO messages_fts (rowid, subject, sender, body_plaintext)"
            " VALUES (?, ?, ?, ?);", -1, &fts, nullptr);
        sqlite3_prepare_v2(m_db,
            "INSERT INTO attachments (message_id, part_index, filename, mime_type, size)"
            " VALUES (?, ?, ?, ?, ?);", -1, &att, nullptr);

        for (int i = 0; i < msgs.size(); ++i) {
            const Message &m = msgs.at(i);
            bindText(ins, 1, m.messageId);
            bindText(ins, 2, m.conversationId);
            bindText(ins, 3, m.sender);
            bindText(ins, 4, m.subject);
            sqlite3_bind_int64(ins, 5, m.date.toSecsSinceEpoch());
            bindText(ins, 6, m.snippet);
            sqlite3_bind_int(ins, 7, m.isUnread ? 1 : 0);
            bindText(ins, 8, paths.value(i));

            if (sqlite3_step(ins) != SQLITE_DONE) {
                qWarning() << "MetadataIndex: message insert failed:"
                           << sqlite3_errmsg(m_db);
                sqlite3_reset(ins);
                continue;
            }

            if (sqlite3_changes(m_db) > 0) {
                bindText(sel, 1, m.messageId);
                if (sqlite3_step(sel) == SQLITE_ROW) {
                    const sqlite3_int64 dbId = sqlite3_column_int64(sel, 0);

                    sqlite3_bind_int64(fts, 1, dbId);
                    bindText(fts, 2, m.subject);
                    bindText(fts, 3, m.sender);
                    bindText(fts, 4, bodies.value(i));
                    if (sqlite3_step(fts) != SQLITE_DONE)
                        qWarning() << "MetadataIndex: FTS insert failed:"
                                   << sqlite3_errmsg(m_db);
                    sqlite3_reset(fts);

                    const QVector<AttachmentMeta> meta = attachments.value(i);
                    for (const AttachmentMeta &a : meta) {
                        sqlite3_bind_int64(att, 1, dbId);
                        sqlite3_bind_int(att, 2, a.index);
                        bindText(att, 3, a.filename);
                        bindText(att, 4, a.mimeType);
                        sqlite3_bind_int64(att, 5, a.size);
                        if (sqlite3_step(att) != SQLITE_DONE)
                            qWarning() << "MetadataIndex: attachment insert failed:"
                                       << sqlite3_errmsg(m_db);
                        sqlite3_reset(att);
                    }
                } else {
                    qWarning() << "MetadataIndex: id lookup failed for" << m.messageId
                               << sqlite3_errmsg(m_db);
                }
                sqlite3_reset(sel);
            }
            sqlite3_reset(ins);
        }

        sqlite3_finalize(ins);
        sqlite3_finalize(sel);
        sqlite3_finalize(fts);
        sqlite3_finalize(att);

        exec("COMMIT;");
    }

    QVector<Conversation> conversations()
    {
        QVector<Conversation> out;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "SELECT id, kind, name, preview, unread_count, last_activity"
                " FROM conversations ORDER BY last_activity DESC;", -1, &st, nullptr) != SQLITE_OK)
            return out;

        while (sqlite3_step(st) == SQLITE_ROW) {
            Conversation c;
            c.id = columnText(st, 0);
            c.kind = columnText(st, 1);
            c.name = columnText(st, 2);
            c.preview = columnText(st, 3);
            c.unreadCount = sqlite3_column_int(st, 4);
            c.lastActivity = QDateTime::fromSecsSinceEpoch(sqlite3_column_int64(st, 5));
            out.append(c);
        }
        sqlite3_finalize(st);
        return out;
    }

    QVector<Message> messages(const QString &conversationId)
    {
        QVector<Message> out;
        sqlite3_stmt *q = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "SELECT id, message_id, conversation_id, sender, subject, date, snippet, is_unread"
                " FROM messages WHERE conversation_id = ? ORDER BY date DESC;",
                -1, &q, nullptr) != SQLITE_OK)
            return out;
        bindText(q, 1, conversationId);

        sqlite3_stmt *att = nullptr;
        sqlite3_prepare_v2(m_db,
            "SELECT part_index, filename, mime_type, size FROM attachments"
            " WHERE message_id = ? ORDER BY part_index;", -1, &att, nullptr);

        while (sqlite3_step(q) == SQLITE_ROW) {
            Message m;
            const sqlite3_int64 dbId = sqlite3_column_int64(q, 0);
            m.messageId = columnText(q, 1);
            m.conversationId = columnText(q, 2);
            m.sender = columnText(q, 3);
            m.subject = columnText(q, 4);
            m.date = QDateTime::fromSecsSinceEpoch(sqlite3_column_int64(q, 5));
            m.snippet = columnText(q, 6);
            m.isUnread = sqlite3_column_int(q, 7) != 0;

            sqlite3_bind_int64(att, 1, dbId);
            while (sqlite3_step(att) == SQLITE_ROW) {
                AttachmentMeta a;
                a.index = sqlite3_column_int(att, 0);
                a.filename = columnText(att, 1);
                a.mimeType = columnText(att, 2);
                a.size = sqlite3_column_int64(att, 3);
                m.attachments.append(a);
            }
            sqlite3_reset(att);
            out.append(m);
        }
        sqlite3_finalize(att);
        sqlite3_finalize(q);
        return out;
    }

    QString filePathForMessage(const QString &messageId)
    {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "SELECT file_path FROM messages WHERE message_id = ?;",
                -1, &st, nullptr) != SQLITE_OK)
            return {};
        bindText(st, 1, messageId);
        QString out;
        if (sqlite3_step(st) == SQLITE_ROW)
            out = columnText(st, 0);
        sqlite3_finalize(st);
        return out;
    }

private:
    bool exec(const char *sql, QString *err = nullptr)
    {
        char *msg = nullptr;
        if (sqlite3_exec(m_db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
            if (err) *err = QString::fromUtf8(msg ? msg : sqlite3_errmsg(m_db));
            sqlite3_free(msg);
            return false;
        }
        return true;
    }

    static void bindText(sqlite3_stmt *st, int idx, const QString &value)
    {
        const QByteArray utf8 = value.toUtf8();
        sqlite3_bind_text(st, idx, utf8.constData(), utf8.size(), SQLITE_TRANSIENT);
    }

    static QString columnText(sqlite3_stmt *st, int idx)
    {
        const unsigned char *txt = sqlite3_column_text(st, idx);
        if (!txt) return {};
        return QString::fromUtf8(reinterpret_cast<const char *>(txt),
                                 sqlite3_column_bytes(st, idx));
    }

    QString m_dbPath;
    QByteArray m_key;
    sqlite3 *m_db = nullptr;
};

#endif
