#ifndef ACCOUNTSTORE_H
#define ACCOUNTSTORE_H

#include <sodium.h>
#include <sqlcipher/sqlite3.h>

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QVector>

/// \brief Vault-level encrypted accounts table (SQLCipher, opened with
///        the app data key). Replaces accounts.json: credentials no
///        longer sit in plaintext on disk.
///
/// Raw sqlite3 C API, same conventions as MetadataIndex: construct per
/// operation, busy_timeout on open, key zeroed on destruction.
///
/// Schema:
///   accounts(type, host, port, user, pass, tls)
///   PRIMARY KEY (type, user, host)
class AccountStore
{
public:
    AccountStore(QString dbPath, QByteArray key)
        : m_dbPath(std::move(dbPath))
        , m_key(std::move(key))
    {
    }

    ~AccountStore()
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
        // Refuse structurally, same as MetadataIndex.
        if (m_key.isEmpty()) {
            if (err) *err = QStringLiteral("no key (locked) — refusing keyless open");
            return false;
        }
        if (sqlite3_open(m_dbPath.toUtf8().constData(), &m_db) != SQLITE_OK) {
            if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
            return false;
        }
        sqlite3_key(m_db, m_key.constData(), static_cast<int>(m_key.size()));
        sqlite3_busy_timeout(m_db, 5000);

        // Credentials live here — owner-only, like the vault boxes.
        QFile::setPermissions(m_dbPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        if (!exec("SELECT count(*) FROM sqlite_master;", err))
            return false;

        if (!exec("CREATE TABLE IF NOT EXISTS accounts ("
                  "  type TEXT NOT NULL,"
                  "  host TEXT NOT NULL,"
                  "  port INTEGER NOT NULL DEFAULT 993,"
                  "  user TEXT NOT NULL,"
                  "  pass TEXT DEFAULT '',"
                  "  tls INTEGER NOT NULL DEFAULT 1,"
                  "  label TEXT DEFAULT '',"
                  "  color TEXT DEFAULT '',"
                  "  idx INTEGER NOT NULL DEFAULT 0,"
                  "  userpic TEXT DEFAULT '',"
                  "  credentials TEXT DEFAULT '',"
                  "  PRIMARY KEY (type, user, host)"
                  ");", err))
            return false;

        migrateColumns();
        return true;
    }

    QVector<QVariantMap> list()
    {
        QVector<QVariantMap> out;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "SELECT type, host, port, user, pass, tls, label, color, idx, userpic"
                " FROM accounts ORDER BY idx, user, host;",
                -1, &st, nullptr) != SQLITE_OK)
            return out;

        while (sqlite3_step(st) == SQLITE_ROW) {
            QVariantMap c;
            c[QStringLiteral("type")] = columnText(st, 0);
            c[QStringLiteral("host")] = columnText(st, 1);
            c[QStringLiteral("port")] = sqlite3_column_int(st, 2);
            c[QStringLiteral("user")] = columnText(st, 3);
            c[QStringLiteral("pass")] = columnText(st, 4);
            c[QStringLiteral("tls")] = sqlite3_column_int(st, 5) != 0;
            c[QStringLiteral("label")] = columnText(st, 6);
            c[QStringLiteral("color")] = columnText(st, 7);
            c[QStringLiteral("idx")] = sqlite3_column_int(st, 8);
            c[QStringLiteral("userpic")] = columnText(st, 9);
            out.append(c);
        }
        sqlite3_finalize(st);
        return out;
    }

    bool add(const QVariantMap &credentials, QString *err = nullptr)
    {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "INSERT OR REPLACE INTO accounts"
                " (type, host, port, user, pass, tls, label, color, idx, userpic, credentials)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);", -1, &st, nullptr) != SQLITE_OK) {
            if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
            return false;
        }
        bindText(st, 1, credentials.value(QStringLiteral("type")).toString());
        bindText(st, 2, credentials.value(QStringLiteral("host")).toString());
        sqlite3_bind_int(st, 3, credentials.value(QStringLiteral("port"), 993).toInt());
        bindText(st, 4, credentials.value(QStringLiteral("user")).toString());
        bindText(st, 5, credentials.value(QStringLiteral("pass")).toString());
        sqlite3_bind_int(st, 6, credentials.value(QStringLiteral("tls"), true).toBool() ? 1 : 0);
        bindText(st, 7, credentials.value(QStringLiteral("label")).toString());
        bindText(st, 8, credentials.value(QStringLiteral("color")).toString());
        sqlite3_bind_int(st, 9, credentials.value(QStringLiteral("idx"), 0).toInt());
        bindText(st, 10, credentials.value(QStringLiteral("userpic")).toString());
        bindText(st, 11, credentials.value(QStringLiteral("credentials")).toString());

        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok && err)
            *err = QString::fromUtf8(sqlite3_errmsg(m_db));
        sqlite3_finalize(st);
        return ok;
    }

    bool remove(const QString &type, const QString &user, const QString &host,
                QString *err = nullptr)
    {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "DELETE FROM accounts WHERE type = ? AND user = ? AND host = ?;",
                -1, &st, nullptr) != SQLITE_OK) {
            if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
            return false;
        }
        bindText(st, 1, type);
        bindText(st, 2, user);
        bindText(st, 3, host);

        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok && err)
            *err = QString::fromUtf8(sqlite3_errmsg(m_db));
        sqlite3_finalize(st);
        return ok;
    }

    /// \brief Re-key the open database to \p newKey (SQLCipher
    ///        sqlite3_rekey). The handle must have been opened (and
    ///        verified) with the OLD key. Used by vault rotation so the
    ///        accounts table survives a dataKey change.
    bool rekey(const QByteArray &newKey, QString *err = nullptr)
    {
        if (sqlite3_rekey(m_db, newKey.constData(),
                          static_cast<int>(newKey.size())) != SQLITE_OK) {
            if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
            return false;
        }
        return true;
    }

    /// \brief True when the table has no rows (used by the one-time
    ///        legacy accounts.json migration).
    bool isEmpty()
    {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(m_db, "SELECT count(*) FROM accounts;",
                               -1, &st, nullptr) != SQLITE_OK)
            return true;
        bool empty = true;
        if (sqlite3_step(st) == SQLITE_ROW)
            empty = sqlite3_column_int(st, 0) == 0;
        sqlite3_finalize(st);
        return empty;
    }

private:
    /// \brief Schema drift: add any missing columns to a v1 accounts
    ///        table (label/color/idx/userpic/credentials). SQLite has
    ///        no IF NOT EXISTS for ADD COLUMN, so check table_info.
    void migrateColumns()
    {
        QSet<QString> cols;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(m_db, "PRAGMA table_info(accounts);",
                               -1, &st, nullptr) != SQLITE_OK)
            return;
        while (sqlite3_step(st) == SQLITE_ROW)
            cols.insert(columnText(st, 1));
        sqlite3_finalize(st);

        const struct { const char *name; const char *def; } wanted[] = {
            { "label",       "label TEXT DEFAULT ''" },
            { "color",       "color TEXT DEFAULT ''" },
            { "idx",         "idx INTEGER NOT NULL DEFAULT 0" },
            { "userpic",     "userpic TEXT DEFAULT ''" },
            { "credentials", "credentials TEXT DEFAULT ''" },
        };
        for (const auto &w : wanted) {
            if (!cols.contains(QString::fromLatin1(w.name)))
                exec(QStringLiteral("ALTER TABLE accounts ADD COLUMN %1")
                         .arg(QString::fromLatin1(w.def)).toUtf8().constData());
        }
    }

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
