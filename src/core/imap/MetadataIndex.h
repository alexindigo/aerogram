#ifndef METADATAINDEX_H
#define METADATAINDEX_H

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QSet>
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
        // Refuse structurally: a keyless open would create a plaintext
        // DB that later encrypted opens reject (HMAC failure).
        if (m_key.isEmpty()) {
            if (err) *err = QStringLiteral("no key (locked) — refusing keyless open");
            return false;
        }
        if (sqlite3_open(m_dbPath.toUtf8().constData(), &m_db) != SQLITE_OK) {
            if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
            return false;
        }
        sqlite3_key(m_db, m_key.constData(), static_cast<int>(m_key.size()));

        // Multiple workers (fetchConversations + sync) open the same
        // file concurrently at unlock time; without a busy timeout the
        // loser of a schema-creation race gets SQLITE_BUSY immediately.
        sqlite3_busy_timeout(m_db, 5000);

        // Owner-only, same convention as the vault files.
        QFile::setPermissions(m_dbPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        // Verify the key (or plaintext state) with a probe query before
        // touching the schema.
        if (!exec("SELECT count(*) FROM sqlite_master;", err))
            return false;

        if (!exec("PRAGMA journal_mode=WAL;", err)
                || !exec("PRAGMA synchronous=NORMAL;", err))
            return false;

        if (!exec("CREATE TABLE IF NOT EXISTS conversations ("
                    "  id TEXT PRIMARY KEY,"
                    "  kind TEXT NOT NULL,"
                    "  name TEXT NOT NULL,"
                    "  preview TEXT DEFAULT '',"
                    "  unread_count INTEGER DEFAULT 0,"
                    "  last_activity INTEGER DEFAULT 0"
                    ");", err)
            || !exec("CREATE TABLE IF NOT EXISTS messages ("
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
            || !exec("CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5("
                    "  subject, sender, body_plaintext"
                    ");", err)
            || !exec("CREATE TABLE IF NOT EXISTS attachments ("
                    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "  message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
                    "  part_index INTEGER NOT NULL,"
                    "  filename TEXT DEFAULT '',"
                    "  mime_type TEXT DEFAULT '',"
                    "  size INTEGER DEFAULT 0"
                    ");", err))
            return false;

        // Schema v2: a message is identity+content, stored ONCE in
        // `messages`; which conversations (IMAP folders, Proton/Gmail
        // labels) it appears in is membership in `message_labels`.
        // messages.conversation_id stays as the first-seen label hint.
        qint64 version = 0;
        {
            sqlite3_stmt *v = nullptr;
            if (sqlite3_prepare_v2(m_db, "PRAGMA user_version;", -1, &v, nullptr) == SQLITE_OK
                    && sqlite3_step(v) == SQLITE_ROW)
                version = sqlite3_column_int64(v, 0);
            sqlite3_finalize(v);
        }
        if (version < 2) {
            if (!exec("CREATE TABLE IF NOT EXISTS message_labels ("
                        "  message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
                        "  label TEXT NOT NULL,"
                        "  UNIQUE(message_id, label)"
                        ");", err))
                return false;
            // Backfill: every pre-v2 message belongs to its folder.
            if (!exec("INSERT OR IGNORE INTO message_labels (message_id, label)"
                      " SELECT id, conversation_id FROM messages;", err))
                return false;
            if (!exec("PRAGMA user_version = 2;", err))
                return false;
        }
        return true;
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
        sqlite3_stmt *mem = nullptr;
        sqlite3_prepare_v2(m_db,
            "INSERT OR IGNORE INTO message_labels (message_id, label)"
            " VALUES (?, ?);", -1, &mem, nullptr);

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
            // Capture BEFORE the membership insert, which would
            // overwrite the change count.
            const bool wasNew = sqlite3_changes(m_db) > 0;

            // The message row id is needed for membership whether or not
            // the row was just inserted (a message can gain a new label
            // long after its content landed).
            bindText(sel, 1, m.messageId);
            sqlite3_int64 dbId = -1;
            if (sqlite3_step(sel) == SQLITE_ROW)
                dbId = sqlite3_column_int64(sel, 0);
            sqlite3_reset(sel);

            if (dbId >= 0) {
                sqlite3_bind_int64(mem, 1, dbId);
                bindText(mem, 2, m.conversationId);
                if (sqlite3_step(mem) != SQLITE_DONE)
                    qWarning() << "MetadataIndex: membership insert failed:"
                               << sqlite3_errmsg(m_db);
                sqlite3_reset(mem);
            }

            // FTS + attachments land only on first insert (content is
            // immutable per message).
            if (wasNew && dbId >= 0) {
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
            }
            sqlite3_reset(ins);
        }

        sqlite3_finalize(ins);
        sqlite3_finalize(sel);
        sqlite3_finalize(fts);
        sqlite3_finalize(att);
        sqlite3_finalize(mem);

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
                "SELECT m.id, m.message_id, m.conversation_id, m.sender, m.subject, m.date, m.snippet, m.is_unread"
                " FROM messages m"
                " JOIN message_labels ml ON ml.message_id = m.id"
                " WHERE ml.label = ? ORDER BY m.date DESC;",
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

    /// \brief Reconcile a conversation (folder/label) against the
    ///        server's truth. Membership-scoped: messages absent from
    ///        presentIds lose their membership in THIS conversation;
    ///        the message row (and its shard) is deleted only when no
    ///        labels hold it anymore (multi-label messages survive).
    ///        Returns the fully-orphaned messages' store paths.
    ///
    ///        ONLY call with a COMPLETE present-set for the
    ///        conversation — a partial listing would drop memberships
    ///        for mail we merely failed to list this pass.
    QVector<QString> removeMissingFromConversation(const QString &conversationId,
                                                   const QSet<QString> &presentIds)
    {
        // Messages with membership in this conversation that are absent
        // from the present set.
        QVector<QPair<sqlite3_int64, QString>> stale;  // (dbId, storePath)
        {
            sqlite3_stmt *st = nullptr;
            if (sqlite3_prepare_v2(m_db,
                    "SELECT m.id, m.message_id, m.file_path FROM messages m"
                    " JOIN message_labels ml ON ml.message_id = m.id"
                    " WHERE ml.label = ?;", -1, &st, nullptr) != SQLITE_OK)
                return {};
            bindText(st, 1, conversationId);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const QString mid = columnText(st, 1);
                if (!presentIds.contains(mid))
                    stale.append({sqlite3_column_int64(st, 0), columnText(st, 2)});
            }
            sqlite3_finalize(st);
        }
        if (stale.isEmpty())
            return {};

        QVector<QString> removedPaths;
        exec("BEGIN TRANSACTION;");
        sqlite3_stmt *delMem = nullptr;
        sqlite3_stmt *cntMem = nullptr;
        sqlite3_stmt *delMsg = nullptr;
        sqlite3_stmt *delFts = nullptr;
        sqlite3_stmt *delAtt = nullptr;
        sqlite3_prepare_v2(m_db,
            "DELETE FROM message_labels WHERE message_id = ? AND label = ?;",
            -1, &delMem, nullptr);
        sqlite3_prepare_v2(m_db,
            "SELECT COUNT(*) FROM message_labels WHERE message_id = ?;",
            -1, &cntMem, nullptr);
        sqlite3_prepare_v2(m_db, "DELETE FROM messages WHERE id = ?;", -1, &delMsg, nullptr);
        sqlite3_prepare_v2(m_db, "DELETE FROM messages_fts WHERE rowid = ?;", -1, &delFts, nullptr);
        sqlite3_prepare_v2(m_db, "DELETE FROM attachments WHERE message_id = ?;", -1, &delAtt, nullptr);

        for (const auto &row : stale) {
            const sqlite3_int64 dbId = row.first;
            // Drop this conversation's membership.
            sqlite3_bind_int64(delMem, 1, dbId);
            bindText(delMem, 2, conversationId);
            sqlite3_step(delMem); sqlite3_reset(delMem);

            // Orphaned (no labels left)? Then the message itself goes.
            sqlite3_bind_int64(cntMem, 1, dbId);
            qint64 remaining = -1;
            if (sqlite3_step(cntMem) == SQLITE_ROW)
                remaining = sqlite3_column_int64(cntMem, 0);
            sqlite3_reset(cntMem);

            if (remaining == 0) {
                sqlite3_bind_int64(delMsg, 1, dbId);
                sqlite3_step(delMsg); sqlite3_reset(delMsg);
                sqlite3_bind_int64(delFts, 1, dbId);
                sqlite3_step(delFts); sqlite3_reset(delFts);
                sqlite3_bind_int64(delAtt, 1, dbId);
                sqlite3_step(delAtt); sqlite3_reset(delAtt);
                if (!row.second.isEmpty())
                    removedPaths.append(row.second);
            }
        }
        sqlite3_finalize(delMem);
        sqlite3_finalize(cntMem);
        sqlite3_finalize(delMsg);
        sqlite3_finalize(delFts);
        sqlite3_finalize(delAtt);
        exec("COMMIT;");
        return removedPaths;
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
