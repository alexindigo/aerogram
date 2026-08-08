#ifndef MESSAGESTORE_H
#define MESSAGESTORE_H

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

/// \brief Hash-sharded loose .eml storage.
///
/// Layout: <root>/<hh>/<hh>/<sha256>.eml where hh are the first and
/// second byte pairs of the key hash. Two levels keep per-directory
/// file counts near 1-2 even at 100k messages. Existing files are
/// never rewritten (dedup by content-addressable path).
class MessageStore
{
public:
    explicit MessageStore(QString root)
        : m_root(std::move(root))
    {
    }

    /// \brief Store raw bytes; returns the relative path (shard form).
    ///        \p keyHint should be the Message-ID (or folder:uid
    ///        fallback) — the same key always maps to the same path.
    QString put(const QByteArray &raw, const QString &keyHint)
    {
        const QByteArray hash = QCryptographicHash::hash(keyHint.toUtf8(),
                                                         QCryptographicHash::Sha256)
                                    .toHex();
        const QString rel = QString::fromLatin1(hash.left(2))
                          + QLatin1Char('/')
                          + QString::fromLatin1(hash.mid(2, 2))
                          + QLatin1Char('/')
                          + QString::fromLatin1(hash)
                          + QStringLiteral(".eml");
        const QString full = m_root + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(full).absolutePath());
        if (QFile::exists(full))
            return rel;

        QFile f(full);
        if (f.open(QIODevice::WriteOnly))
            f.write(raw);
        return rel;
    }

    QByteArray get(const QString &rel) const
    {
        QFile f(m_root + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return f.readAll();
    }

private:
    QString m_root;
};

#endif
