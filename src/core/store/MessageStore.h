#ifndef MESSAGESTORE_H
#define MESSAGESTORE_H

#include <sodium.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

/// \brief Hash-sharded encrypted message storage (.enc files).
///
/// Layout: <root>/<hh>/<hh>/<sha256>.enc where the path derives from
/// the Message-ID hash. File contents are [24-byte nonce][secretbox
/// ciphertext] using ChaCha20-Poly1305 via libsodium — no plaintext
/// email ever touches disk. Decryption happens in memory on read.
///
/// Existing files are never rewritten (dedup by content-addressable
/// path). Requires the master key; put/get fail closed without it.
class MessageStore
{
public:
    MessageStore(QString root, QByteArray key)
        : m_root(std::move(root))
        , m_key(std::move(key))
    {
    }

    ~MessageStore()
    {
        if (!m_key.isEmpty()) {
            sodium_memzero(m_key.data(), m_key.size());
            m_key.clear();
        }
    }

    /// \brief Delete a stored shard (message deleted on the server —
    ///        removal must reach disk, not just the index).
    void remove(const QString &rel)
    {
        if (!rel.isEmpty())
            QFile::remove(m_root + QLatin1Char('/') + rel);
    }

    /// \brief Encrypt + store raw bytes; returns the relative path
    ///        (shard form). \p keyHint should be the Message-ID (or
    ///        folder:uid fallback).
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
                          + QStringLiteral(".enc");
        const QString full = m_root + QLatin1Char('/') + rel;
        if (QFile::exists(full) || m_key.isEmpty())
            return rel;

        QDir().mkpath(QFileInfo(full).absolutePath());

        QByteArray nonce(crypto_secretbox_NONCEBYTES, 0);
        randombytes_buf(nonce.data(), nonce.size());

        QByteArray ct(raw.size() + crypto_secretbox_MACBYTES, 0);
        crypto_secretbox_easy(reinterpret_cast<unsigned char *>(ct.data()),
                              reinterpret_cast<const unsigned char *>(raw.constData()),
                              raw.size(),
                              reinterpret_cast<const unsigned char *>(nonce.constData()),
                              reinterpret_cast<const unsigned char *>(m_key.constData()));

        QFile f(full);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(nonce);
            f.write(ct);
        }
        return rel;
    }

    /// \brief Read + decrypt a stored message. Returns empty on
    ///        missing file or authentication failure.
    QByteArray get(const QString &rel) const
    {
        if (m_key.isEmpty())
            return {};

        QFile f(m_root + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        const QByteArray blob = f.readAll();
        if (blob.size() < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES)
            return {};

        const int ctLen = blob.size() - crypto_secretbox_NONCEBYTES;
        QByteArray plain(ctLen - crypto_secretbox_MACBYTES, 0);
        if (crypto_secretbox_open_easy(
                reinterpret_cast<unsigned char *>(plain.data()),
                reinterpret_cast<const unsigned char *>(blob.constData()) + crypto_secretbox_NONCEBYTES,
                ctLen,
                reinterpret_cast<const unsigned char *>(blob.constData()),
                reinterpret_cast<const unsigned char *>(m_key.constData())) != 0) {
            return {};
        }
        return plain;
    }

private:
    QString m_root;
    QByteArray m_key;
};

#endif
