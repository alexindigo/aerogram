#ifndef MASTERKEYMANAGER_H
#define MASTERKEYMANAGER_H

#include <sodium.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QObject>
#include <QString>

#include <filesystem>

/// \brief Vault crypto manager. Holds the data key (in RAM while
///        unlocked) derived deterministically from master password +
///        user-chosen Secret Key phrase:
///
///            salt16  = SHA-256(NFC(phrase).trim())[0:16]
///            dataKey = Argon2id(password, salt16)
///
///        Because dataKey is derivable, master password + phrase alone
///        open any recovered .enc file — no vault artifact required.
///
///        Daily convenience files (all optional for recovery):
///          wrap-salt.bin   random 16B plaintext wrap salt (per-vault)
///          secret-key.enc  secretbox(phrase, wrapKey)  — password-only unlock
///          keycheck.enc    secretbox(known-plain, dataKey) — input check
///        where wrapKey = Argon2id(password, wrap-salt.bin).
///
///        Crypto only — no QML/IPC knowledge. The controller calls
///        create/unlock/recover/rotate and drives post-unlock work.
///
///        API verified against /usr/include/sodium.h (1.0.22).
class MasterKeyManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isLocked READ isLocked NOTIFY isLockedChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool vaultExists READ vaultExists NOTIFY vaultStateChanged)
    Q_PROPERTY(bool vaultNeedsRecovery READ vaultNeedsRecovery NOTIFY vaultStateChanged)

public:
    explicit MasterKeyManager(QString vaultDir, QObject *parent = nullptr)
        : QObject(parent)
        , m_vaultDir(std::move(vaultDir))
    {
        if (sodium_init() < 0)
            qFatal("libsodium initialization failed");
        QDir().mkpath(m_vaultDir);

        // Leftover round-3 layout (salt.bin, no keycheck.enc): refuse,
        // never silently create on top.
        if (!QFile::exists(keycheckPath()) && QFile::exists(m_vaultDir + QStringLiteral("/salt.bin"))) {
            setStatusText(QStringLiteral("Old vault format — wipe ")
                          + m_vaultDir + QStringLiteral(" to re-init"));
            return;
        }

        if (!vaultExists())
            setStatusText(QStringLiteral("Create master password and Secret Key"));
        else if (vaultNeedsRecovery())
            setStatusText(QStringLiteral("Vault damaged — recover with master password and Secret Key"));
        else
            setStatusText(QStringLiteral("Enter master password"));
    }

    ~MasterKeyManager() override { lock(); }

    bool isLocked() const { return m_locked; }
    QString statusText() const { return m_statusText; }
    QString vaultDir() const { return m_vaultDir; }

    /// \brief keycheck.enc exists → a vault exists (even if damaged).
    bool vaultExists() const { return QFile::exists(keycheckPath()); }

    /// \brief Vault exists but the phrase box is missing/corrupt.
    bool vaultNeedsRecovery() const
    {
        return vaultExists() && !QFile::exists(secretKeyPath());
    }

    /// \brief The data key; empty while locked.
    QByteArray key() const { return m_locked ? QByteArray() : m_dataKey; }

    // -----------------------------------------------------------------
    // Entry points (state contract enforced here and in the controller):
    //   uninitialized                 -> create() only
    //   initialized + secret-key.enc  -> unlock()
    //   initialized - secret-key.enc  -> recover()
    // -----------------------------------------------------------------

    /// \brief First run: create the vault from password + phrase.
    ///        Refuses if a vault already exists.
    bool create(const QString &password, const QString &phrase, QString *err = nullptr)
    {
        if (vaultExists()) {
            setErr(err, QStringLiteral("Vault already exists"));
            return false;
        }
        const QByteArray phraseBytes = normalizePhrase(phrase);
        if (phraseBytes.isEmpty()) {
            setErr(err, QStringLiteral("Secret Key phrase must not be empty"));
            return false;
        }

        const QByteArray pass = password.toUtf8();
        const QByteArray wrapSalt = randomBytes(crypto_pwhash_argon2id_SALTBYTES);
        const QByteArray salt16 = saltFromPhrase(phraseBytes);
        const QByteArray dataKey = argon(pass, salt16);
        const QByteArray wrapKey = argon(pass, wrapSalt);
        if (dataKey.isEmpty() || wrapKey.isEmpty()) {
            setErr(err, QStringLiteral("Key derivation failed"));
            return false;
        }

        // Atomic-ish writes: tmp + rename, 0600. If any write fails,
        // fail closed — next boot sees an incomplete layout.
        if (!writeVault(wrapSalt, phraseBytes, wrapKey, dataKey)) {
            setErr(err, QStringLiteral("Vault write failed"));
            return false;
        }

        adopt(password, phraseBytes, dataKey);
        setStatusText(QString());
        return true;
    }

    /// \brief Daily unlock: password only (phrase read from
    ///        secret-key.enc). Never creates a vault.
    bool unlock(const QString &password, QString *err = nullptr)
    {
        if (!vaultExists()) {
            setErr(err, QStringLiteral("No vault — create one first"));
            return false;
        }
        if (vaultNeedsRecovery()) {
            setErr(err, QStringLiteral("Vault damaged — recover with master password and Secret Key"));
            setStatusText(QStringLiteral("Vault damaged — recover with master password and Secret Key"));
            return false;
        }

        const QByteArray wrapSalt = readFile(wrapSaltPath());
        if (wrapSalt.size() != crypto_pwhash_argon2id_SALTBYTES) {
            setErr(err, QStringLiteral("Vault damaged — recover with master password and Secret Key"));
            setStatusText(QStringLiteral("Vault damaged — recover with master password and Secret Key"));
            return false;
        }

        const QByteArray pass = password.toUtf8();
        const QByteArray wrapKey = argon(pass, wrapSalt);
        if (wrapKey.isEmpty()) {
            setErr(err, QStringLiteral("Key derivation failed"));
            return false;
        }

        // Authenticated open: a wrong password fails here, after ONE
        // Argon2id run.
        const QByteArray phraseBytes = openBox(readFile(secretKeyPath()), wrapKey);
        if (phraseBytes.isEmpty()) {
            setErr(err, QStringLiteral("Wrong password"));
            setStatusText(QStringLiteral("Wrong password"));
            sodium_memzero(const_cast<char *>(pass.constData()), pass.size());
            return false;
        }

        const QByteArray salt16 = saltFromPhrase(phraseBytes);
        const QByteArray dataKey = argon(pass, salt16);
        const bool ok = !dataKey.isEmpty()
                     && openBox(readFile(keycheckPath()), dataKey) == QByteArray(kKeyCheckPlain);
        sodium_memzero(const_cast<char *>(pass.constData()), pass.size());
        if (!ok) {
            setErr(err, QStringLiteral("Vault damaged — recover with master password and Secret Key"));
            setStatusText(QStringLiteral("Vault damaged — recover with master password and Secret Key"));
            return false;
        }

        adopt(password, phraseBytes, dataKey);
        setStatusText(QString());
        return true;
    }

    /// \brief Recovery: password + phrase recomputes dataKey directly;
    ///        verifies against keycheck.enc, then re-writes the daily
    ///        convenience files so password-only unlock works again.
    bool recover(const QString &password, const QString &phrase, QString *err = nullptr)
    {
        if (!vaultExists()) {
            setErr(err, QStringLiteral("No vault to recover — wipe the vault directory to re-init"));
            return false;
        }
        const QByteArray phraseBytes = normalizePhrase(phrase);
        if (phraseBytes.isEmpty()) {
            setErr(err, QStringLiteral("Secret Key phrase must not be empty"));
            return false;
        }

        const QByteArray pass = password.toUtf8();
        const QByteArray salt16 = saltFromPhrase(phraseBytes);
        const QByteArray dataKey = argon(pass, salt16);
        const bool ok = !dataKey.isEmpty()
                     && openBox(readFile(keycheckPath()), dataKey) == QByteArray(kKeyCheckPlain);
        if (!ok) {
            setErr(err, QStringLiteral("Password or Secret Key doesn't match"));
            setStatusText(QStringLiteral("Password or Secret Key doesn't match"));
            sodium_memzero(const_cast<char *>(pass.constData()), pass.size());
            return false;
        }

        // Re-write daily convenience files (fresh wrap salt).
        const QByteArray wrapSalt = randomBytes(crypto_pwhash_argon2id_SALTBYTES);
        const QByteArray wrapKey = argon(pass, wrapSalt);
        sodium_memzero(const_cast<char *>(pass.constData()), pass.size());
        if (wrapKey.isEmpty()
                || !writeFileAtomic(wrapSaltPath(), wrapSalt)
                || !writeFileAtomic(secretKeyPath(), sealBox(phraseBytes, wrapKey))) {
            setErr(err, QStringLiteral("Vault write failed"));
            return false;
        }

        adopt(password, phraseBytes, dataKey);
        setStatusText(QString());
        return true;
    }

    /// \brief Rotation. Mode A ("wipe-resync") only: vault files are
    ///        rewritten with keys derived from the new password/phrase;
    ///        the local store wipe + resync is orchestrated by the
    ///        controller. Mode B returns not-implemented.
    bool rotate(const QString &newPassword, const QString &newPhrase,
                const QString &mode, QString *err = nullptr)
    {
        if (m_locked) {
            setErr(err, QStringLiteral("Unlock first"));
            return false;
        }
        if (mode != QLatin1String("wipe-resync")) {
            setErr(err, QStringLiteral("Rotation mode not implemented: ") + mode);
            return false;
        }

        const QString pass = newPassword.isEmpty() ? m_password : newPassword;
        const QByteArray phraseBytes = newPhrase.isEmpty()
                                     ? m_phrase
                                     : normalizePhrase(newPhrase);
        if (phraseBytes.isEmpty()) {
            setErr(err, QStringLiteral("Secret Key phrase must not be empty"));
            return false;
        }

        const QByteArray passBytes = pass.toUtf8();
        const QByteArray wrapSalt = randomBytes(crypto_pwhash_argon2id_SALTBYTES);
        const QByteArray salt16 = saltFromPhrase(phraseBytes);
        const QByteArray dataKey = argon(passBytes, salt16);
        const QByteArray wrapKey = argon(passBytes, wrapSalt);
        if (dataKey.isEmpty() || wrapKey.isEmpty()) {
            setErr(err, QStringLiteral("Key derivation failed"));
            return false;
        }

        if (!writeVault(wrapSalt, phraseBytes, wrapKey, dataKey)) {
            setErr(err, QStringLiteral("Vault write failed"));
            return false;
        }

        adopt(pass, phraseBytes, dataKey);
        setStatusText(QString());
        return true;
    }

    /// \brief Purge all key material from RAM.
    void lock()
    {
        zeroize(m_password);
        zeroize(m_phrase);
        zeroize(m_dataKey);
        m_locked = true;
        emit isLockedChanged();
        if (vaultExists())
            setStatusText(QStringLiteral("Enter master password"));
    }

signals:
    void isLockedChanged();
    void statusTextChanged();
    void vaultStateChanged();

private:
    static constexpr char kKeyCheckPlain[] = "aerogram-vault-v1";

    QString wrapSaltPath() const { return m_vaultDir + QStringLiteral("/wrap-salt.bin"); }
    QString secretKeyPath() const { return m_vaultDir + QStringLiteral("/secret-key.enc"); }
    QString keycheckPath() const { return m_vaultDir + QStringLiteral("/keycheck.enc"); }

    void adopt(const QString &password, const QByteArray &phrase, const QByteArray &dataKey)
    {
        zeroize(m_password);
        zeroize(m_phrase);
        zeroize(m_dataKey);
        m_password = password;
        m_phrase = phrase;
        m_dataKey = dataKey;
        m_locked = false;
        emit isLockedChanged();
        emit vaultStateChanged();
    }

    void setErr(QString *err, const QString &msg)
    {
        if (err) *err = msg;
    }

    void setStatusText(const QString &text)
    {
        if (m_statusText != text) {
            m_statusText = text;
            emit statusTextChanged();
        }
    }

    static void zeroize(QString &s)
    {
        if (!s.isEmpty()) {
            // QString is UTF-16 and reallocates; scrub the current
            // buffer, then clear. (Phrase lives in QByteArray where
            // scrubbing is reliable; the password copy is best-effort.)
            sodium_memzero(s.data(), s.size() * sizeof(QChar));
            s.clear();
        }
    }

    static void zeroize(QByteArray &b)
    {
        if (!b.isEmpty()) {
            sodium_memzero(b.data(), b.size());
            b.clear();
        }
    }

    static QByteArray normalizePhrase(const QString &phrase)
    {
        return phrase.normalized(QString::NormalizationForm_C).trimmed().toUtf8();
    }

    static QByteArray saltFromPhrase(const QByteArray &phraseBytes)
    {
        const QByteArray hash = QCryptographicHash::hash(phraseBytes,
                                                         QCryptographicHash::Sha256);
        return hash.left(crypto_pwhash_argon2id_SALTBYTES);
    }

    static QByteArray argon(const QByteArray &pass, const QByteArray &salt)
    {
        if (salt.size() != crypto_pwhash_argon2id_SALTBYTES)
            return {};
        QByteArray out(crypto_secretbox_KEYBYTES, 0);
        if (crypto_pwhash_argon2id(
                reinterpret_cast<unsigned char *>(out.data()), out.size(),
                pass.constData(), pass.size(),
                reinterpret_cast<const unsigned char *>(salt.constData()),
                crypto_pwhash_argon2id_OPSLIMIT_INTERACTIVE,
                crypto_pwhash_argon2id_MEMLIMIT_INTERACTIVE,
                crypto_pwhash_argon2id_ALG_ARGON2ID13) != 0) {
            return {};
        }
        return out;
    }

    static QByteArray randomBytes(int n)
    {
        QByteArray out(n, 0);
        randombytes_buf(out.data(), out.size());
        return out;
    }

    static QByteArray sealBox(const QByteArray &plain, const QByteArray &key)
    {
        const QByteArray nonce = randomBytes(crypto_secretbox_NONCEBYTES);
        QByteArray ct(plain.size() + crypto_secretbox_MACBYTES, 0);
        crypto_secretbox_easy(reinterpret_cast<unsigned char *>(ct.data()),
                              reinterpret_cast<const unsigned char *>(plain.constData()),
                              plain.size(),
                              reinterpret_cast<const unsigned char *>(nonce.constData()),
                              reinterpret_cast<const unsigned char *>(key.constData()));
        return nonce + ct;
    }

    static QByteArray openBox(const QByteArray &box, const QByteArray &key)
    {
        if (box.size() < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES)
            return {};
        const int ctLen = box.size() - crypto_secretbox_NONCEBYTES;
        QByteArray plain(ctLen - crypto_secretbox_MACBYTES, 0);
        if (crypto_secretbox_open_easy(
                reinterpret_cast<unsigned char *>(plain.data()),
                reinterpret_cast<const unsigned char *>(box.constData()) + crypto_secretbox_NONCEBYTES,
                ctLen,
                reinterpret_cast<const unsigned char *>(box.constData()),
                reinterpret_cast<const unsigned char *>(key.constData())) != 0) {
            return {};
        }
        return plain;
    }

    bool writeVault(const QByteArray &wrapSalt, const QByteArray &phraseBytes,
                    const QByteArray &wrapKey, const QByteArray &dataKey)
    {
        return writeFileAtomic(wrapSaltPath(), wrapSalt)
            && writeFileAtomic(secretKeyPath(), sealBox(phraseBytes, wrapKey))
            && writeFileAtomic(keycheckPath(), sealBox(QByteArray(kKeyCheckPlain), dataKey));
    }

    static bool writeFileAtomic(const QString &path, const QByteArray &data)
    {
        const QString tmp = path + QStringLiteral(".tmp");
        {
            QFile f(tmp);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            if (f.write(data) != data.size())
                return false;
            f.flush();
            f.close();
        }
        QFile::setPermissions(tmp, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile::remove(path);
        std::error_code ec;
        std::filesystem::rename(tmp.toStdString(), path.toStdString(), ec);
        return !ec;
    }

    static QByteArray readFile(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return f.readAll();
    }

    QString m_vaultDir;
    QString m_password;      // held while unlocked for rotate(); zeroed on lock
    QByteArray m_phrase;     // QByteArray: reliable memzero
    QByteArray m_dataKey;
    bool m_locked = true;
    QString m_statusText;
};

#endif
