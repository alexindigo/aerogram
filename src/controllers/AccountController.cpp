#include "controllers/AccountController.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>

#include "core/crypto/AccountStore.h"
#include "core/plugin/BackendRegistry.h"
#include "core/plugin/Capabilities.h"

/// \file AccountController.cpp
/// No concrete backend class is named in this file: construction goes
/// through BackendRegistry, vault key/store flows through the
/// IMasterKeyAware capability, and UI grouping reads plugin->family().
/// See ~/Documents/Aerogram/plans/backend-untangle/plan.md.

AccountController::AccountController(const QList<QPair<QString, QVariantMap>> &accountSpecs,
                                     MasterKeyManager *vault,
                                     QObject *parent)
    : QObject(parent)
    , m_messageModel(new MessageListModel(this))
    , m_conversationModel(new ConversationListModel(this))
    , m_accountsModel(new AccountListModel(this))
    , m_configStatus(QStringLiteral("Not configured"))
    , m_activeView(QStringLiteral("email"))
    , m_vault(vault)
{
    // Construction is registry-driven: specs are (type, credentials);
    // the controller never constructs a concrete backend itself.
    for (const auto &spec : accountSpecs) {
        BackendPlugin *backend = BackendRegistry::create(spec.first, spec.second);
        if (backend)
            registerAccount(spec.first, spec.second, backend);
        else
            qWarning().noquote() << "Unknown backend type in account spec:" << spec.first;
    }

    if (m_vault) {
        connect(m_vault, &MasterKeyManager::isLockedChanged, this, [this] {
            emit isLockedChanged();
            updateLockOverlayVisibility();
        });
        connect(m_vault, &MasterKeyManager::statusTextChanged,
                this, &AccountController::lockStatusTextChanged);
        connect(m_vault, &MasterKeyManager::vaultStateChanged, this, [this] {
            emit vaultStateChanged();
            updateLockOverlayVisibility();
        });
    }
}

// ---------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------

MessageListModel *AccountController::messageListModel() const { return m_messageModel; }
ConversationListModel *AccountController::conversationListModel() const { return m_conversationModel; }
AccountListModel *AccountController::accountsModel() const { return m_accountsModel; }
QString AccountController::iconPackDir() const { return m_iconPackDir; }
void AccountController::setIconPackDir(const QString &dir) { m_iconPackDir = dir; }
QString AccountController::configStatus() const { return m_configStatus; }
QString AccountController::activeView() const { return m_activeView; }
QString AccountController::activeAccountId() const { return m_activeAccountId; }
QString AccountController::activeConversationId() const { return m_activeConversationId; }
QString AccountController::activeMessageId() const { return m_activeMessageId; }
QVariantMap AccountController::activeMessage() const { return m_activeMessage; }
QString AccountController::activeMessageBody() const { return m_activeMessageBody; }
QVariantList AccountController::activeMessageAttachments() const { return m_activeMessageAttachments; }

bool AccountController::isLocked() const { return m_vault ? m_vault->isLocked() : false; }
QString AccountController::lockStatusText() const { return m_vault ? m_vault->statusText() : QString(); }
bool AccountController::vaultExists() const { return m_vault ? m_vault->vaultExists() : false; }
bool AccountController::vaultNeedsRecovery() const
{
    return m_vault ? m_vault->vaultNeedsRecovery() : false;
}

bool AccountController::hasEncryptedBackend() const
{
    for (const Account &a : m_accounts) {
        if (dynamic_cast<IMasterKeyAware *>(a.backend))
            return true;
    }
    return false;
}

bool AccountController::hasNoAccounts() const { return m_accounts.isEmpty(); }

bool AccountController::showLockOverlay() const
{
    return isLocked() && (vaultExists() || hasEncryptedBackend() || hasNoAccounts());
}

// ---------------------------------------------------------------------
// Property setters
// ---------------------------------------------------------------------

void AccountController::setConfigStatus(const QString &status)
{
    if (m_configStatus != status) {
        m_configStatus = status;
        emit configStatusChanged();
    }
}

void AccountController::setActiveView(const QString &view)
{
    if (m_activeView != view) {
        m_activeView = view;
        emit activeViewChanged();
    }
}

void AccountController::setActiveAccountId(const QString &accountId)
{
    if (m_activeAccountId != accountId) {
        m_activeAccountId = accountId;
        emit activeAccountIdChanged();
    }
}

void AccountController::setActiveConversationId(const QString &conversationId)
{
    if (m_activeConversationId != conversationId) {
        m_activeConversationId = conversationId;
        emit activeConversationIdChanged();
    }
}

void AccountController::setActiveMessageId(const QString &messageId)
{
    if (m_activeMessageId != messageId) {
        m_activeMessageId = messageId;
        emit activeMessageIdChanged();
    }
}

void AccountController::setActiveMessage(const QVariantMap &message)
{
    m_activeMessage = message;
    emit activeMessageChanged();
}

void AccountController::setActiveMessageBody(const QString &body)
{
    if (m_activeMessageBody != body) {
        m_activeMessageBody = body;
        emit activeMessageBodyChanged();
    }
}

void AccountController::setActiveMessageAttachments(const QVariantList &attachments)
{
    m_activeMessageAttachments = attachments;
    emit activeMessageAttachmentsChanged();
}

// ---------------------------------------------------------------------
// Account registry. Backends are created via BackendRegistry and held
// through the BackendPlugin interface inside Account entities.
// ---------------------------------------------------------------------

void AccountController::registerAccount(const QString &type, const QVariantMap &credentials,
                                        BackendPlugin *backend)
{
    Account a;
    a.type = type;
    a.backend = backend;
    a.credentials = credentials;

    // Label: user@host when the credential shape has both (imap-like);
    // otherwise the type name (deltachat/mock).
    const QString user = credentials.value(QStringLiteral("user")).toString();
    const QString host = credentials.value(QStringLiteral("host")).toString();
    a.label = (!user.isEmpty() && !host.isEmpty()) ? user + QLatin1Char('@') + host : type;

    // Sketch format: <backend_id>#<backend> — e.g. alice@gmail.com#imap
    a.id = a.label + QLatin1Char('#') + type;

    a.index = m_accounts.size();
    // Color: derived deterministically from the account id (md5 —
    // qHash is not stable across runs), so the rail color never drifts.
    static const char *palette[] = {
        "#4c9baf", "#7a5fb5", "#b5546e", "#5f8f4e", "#b58433", "#3f7fa5"
    };
    const QByteArray hash = QCryptographicHash::hash(a.id.toUtf8(), QCryptographicHash::Md5);
    a.color = QString::fromLatin1(
        palette[static_cast<unsigned char>(hash.at(0)) % (sizeof(palette) / sizeof(palette[0]))]);

    // The controller owns backend instances (Qt parent-child teardown).
    backend->setParent(this);

    m_accounts.append(a);
    connectBackend(m_accounts.last());
    // configure() emits configured() — must run after connectBackend so
    // the signal isn't lost.
    if (auto *c = dynamic_cast<ICredentialsSetup *>(backend))
        c->configure(credentials);
    rebuildAccountsModel();
    emit backendsChanged();
    updateLockOverlayVisibility();
}

Account *AccountController::accountById(const QString &accountId)
{
    for (auto &a : m_accounts) {
        if (a.id == accountId)
            return &a;
    }
    return nullptr;
}

/// \brief Resolve a compound conversationId ("<accountId>/<localId>")
///        to its account, optionally returning the backend-local ID.
///        Split at the FIRST '/': account ids never contain '/',
///        folder names may.
Account *AccountController::accountForConversation(const QString &compoundConversationId,
                                                   QString *localId)
{
    const int slash = compoundConversationId.indexOf(QLatin1Char('/'));
    if (slash < 0)
        return nullptr;
    if (localId)
        *localId = compoundConversationId.mid(slash + 1);
    return accountById(compoundConversationId.left(slash));
}

void AccountController::connectBackend(const Account &account)
{
    BackendPlugin *backend = account.backend;
    const QString accountId = account.id;

    // Per-account lambdas capture the accountId so incoming local IDs
    // get compounded ("<accountId>/<localId>") on the way in.
    connect(backend, &BackendPlugin::conversationsReady, this,
            [this, accountId](const QVector<Conversation> &convs) {
                // Ghost-signal guard: a queued emission can already be
                // in the event queue when the account is removed.
                if (!accountById(accountId))
                    return;
                QVector<Conversation> compounded;
                compounded.reserve(convs.size());
                for (Conversation c : convs) {
                    c.id = accountId + QStringLiteral("/") + c.id;
                    compounded.append(c);
                }
                m_conversationsByAccount[accountId] = compounded;
                rebuildMergedConversations();
            });

    connect(backend, &BackendPlugin::messagesReady, this,
            [this, accountId](const QString &localConvId, const QVector<Message> &msgs) {
                const QString compound = accountId + QStringLiteral("/") + localConvId;
                if (compound != m_activeConversationId)
                    return;
                QVector<Message> fixed = msgs;
                for (Message &m : fixed)
                    m.conversationId = compound;
                m_activeMessages = fixed;
                m_messageModel->setMessages(fixed);
                emit messagesChanged(compound);
            });

    connect(backend, &BackendPlugin::messageSent, this,
            [this, accountId](bool ok, const QString &localConvId) {
                emit messageSent(ok, accountId + QStringLiteral("/") + localConvId);
            });

    connect(backend, &BackendPlugin::messageBodyReady, this,
            [this, accountId](const QString &localConvId, const QString &messageId,
                              const QString &body) {
                if (messageId == m_activeMessageId)
                    setActiveMessageBody(body);
                emit messageBodyReady(accountId + QStringLiteral("/") + localConvId,
                                      messageId, body);
            });

    connect(backend, &BackendPlugin::attachmentSaved, this,
            [this](bool ok, const QString &messageId, const QString &path) {
                emit attachmentSaved(ok, messageId, path);
            });

    connect(backend, &BackendPlugin::configured, this,
            [this](bool success) {
                if (success) {
                    setConfigStatus(QStringLiteral("Connected"));
                    fetchConversations();
                } else {
                    setConfigStatus(QStringLiteral("Setup failed"));
                }
            });

    connect(backend, &BackendPlugin::errorOccurred, this,
            [this, accountId](const QString &error) {
                setConfigStatus(QStringLiteral("Error: ") + error);
                emit errorOccurred(accountId + QStringLiteral(": ") + error);
            });

    connect(backend, &BackendPlugin::ioStarted, this,
            [this, accountId](bool ok, const QString &error) {
                emit ioStarted(accountId, ok, error);
                if (ok) {
                    if (m_pendingAdds.remove(accountId))
                        setConfigStatus(QStringLiteral("Account added: ") + accountId);
                } else {
                    m_pendingAdds.remove(accountId);
                    setConfigStatus(QStringLiteral("Account failed to connect: ")
                                    + accountId + QStringLiteral(" — ") + error);
                }
            });

    connect(backend, &BackendPlugin::ioStopped, this,
            [this, accountId]() {
                emit ioStopped(accountId);
            });
}

/// \brief Rebuild the sidebar account rail from the account registry.
///        Chips derive initials from the account label (user part for
///        email-shaped labels); color comes from the persisted account
///        color (assigned at registration).
void AccountController::rebuildAccountsModel()
{
    QVector<AccountEntry> rows;
    rows.reserve(m_accounts.size());
    for (const Account &a : m_accounts) {
        AccountEntry e;
        e.accountId = a.id;
        e.type = a.backend->family();   // backend-reported, no prefix sniffing
        e.color = a.color;

        QString base = a.label;
        const int at = base.indexOf(QLatin1Char('@'));
        if (at > 0) base = base.left(at);
        base = base.left(2);
        if (!base.isEmpty()) base[0] = base[0].toUpper();
        e.chipText = base;

        rows.append(e);
    }

    std::sort(rows.begin(), rows.end(), [](const AccountEntry &a, const AccountEntry &b) {
        return a.type != b.type && a.type == QLatin1String("email");
    });

    m_accountsModel->setAccounts(rows);
}

void AccountController::rebuildMergedConversations()
{
    QVector<Conversation> merged;
    for (const auto &convs : m_conversationsByAccount) {
        if (!m_activeAccountId.isEmpty() && !convs.isEmpty()
                && !convs.first().id.startsWith(m_activeAccountId + QStringLiteral("/")))
            continue;
        merged += convs;
    }
    m_conversationModel->setConversations(merged);
    emit conversationsChanged();

    // Data-arrived-after-early-selection: if a conversation is active
    // but its model is empty (the user or a test selected it before the
    // sync landed), re-fetch now that the index has content.
    if (!m_activeConversationId.isEmpty()
            && m_messageModel->rowCount() == 0)
        fetchMessages(m_activeConversationId);

    // One-time default selection: prefer a conversation named INBOX,
    // else the first. Keeps the email view populated without clicks.
    if (!m_autoSelected && m_activeConversationId.isEmpty() && !merged.isEmpty()) {
        m_autoSelected = true;
        QString pick = merged.first().id;
        for (const Conversation &c : merged) {
            if (c.name.compare(QStringLiteral("INBOX"), Qt::CaseInsensitive) == 0) {
                pick = c.id;
                break;
            }
        }
        fetchMessages(pick);
    }
}

// ---------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------

void AccountController::fetchConversations()
{
    for (const Account &a : m_accounts) {
        if (auto *p = dynamic_cast<IConversationProvider *>(a.backend))
            p->fetchConversations();
    }
}

void AccountController::fetchMessages(const QString &conversationId)
{
    setActiveConversationId(conversationId);
    QString localId;
    Account *account = accountForConversation(conversationId, &localId);
    if (auto *p = dynamic_cast<IMessageProvider *>(account ? account->backend : nullptr)) {
        p->fetchMessages(localId);
    } else {
        emit errorOccurred(QStringLiteral("No message provider for %1").arg(conversationId));
    }
}

void AccountController::fetchMessageBody(const QString &conversationId, const QString &messageId)
{
    QString localId;
    Account *account = accountForConversation(conversationId, &localId);
    if (auto *p = dynamic_cast<IMessageProvider *>(account ? account->backend : nullptr)) {
        p->fetchMessageBody(localId, messageId);
    } else {
        emit errorOccurred(QStringLiteral("No message provider for %1").arg(conversationId));
    }
}

void AccountController::selectMessage(const QString &messageId)
{
    setActiveMessageId(messageId);
    setActiveMessageBody(QString());

    QVariantMap meta;
    QVariantList atts;
    for (const Message &m : m_activeMessages) {
        if (m.messageId != messageId)
            continue;
        meta[QStringLiteral("subject")] = m.subject;
        meta[QStringLiteral("sender")] = m.sender;
        meta[QStringLiteral("date")] = m.date;
        for (const AttachmentMeta &a : m.attachments) {
            QVariantMap am;
            am[QStringLiteral("index")] = a.index;
            am[QStringLiteral("filename")] = a.filename;
            am[QStringLiteral("mimeType")] = a.mimeType;
            am[QStringLiteral("size")] = a.size;
            atts.append(am);
        }
        break;
    }
    setActiveMessage(meta);
    setActiveMessageAttachments(atts);

    if (!m_activeConversationId.isEmpty())
        fetchMessageBody(m_activeConversationId, messageId);
}

void AccountController::saveAttachment(const QString &messageId, int partIndex,
                                       const QString &destinationPath)
{
    Account *account = accountForConversation(m_activeConversationId, nullptr);
    if (auto *p = dynamic_cast<IMessageProvider *>(account ? account->backend : nullptr)) {
        p->saveAttachment(messageId, partIndex, destinationPath);
    } else {
        emit attachmentSaved(false, messageId, destinationPath);
    }
}

void AccountController::sendMessage(const QString &conversationId, const QString &text)
{
    QString localId;
    Account *account = accountForConversation(conversationId, &localId);
    if (auto *p = dynamic_cast<IMessageSender *>(account ? account->backend : nullptr)) {
        p->sendMessage(localId, text);
    } else {
        emit errorOccurred(QStringLiteral("Backend for %1 cannot send messages")
                               .arg(conversationId));
    }
}

/// \brief Add a new account at runtime (dialog or IPC). The backend is
///        created through BackendRegistry and persisted into the
///        encrypted vault DB. Success is claimed only after IO
///        verifies (see the ioStarted lambda in connectBackend).
void AccountController::addAccount(const QVariantMap &credentials)
{
    const QString type = credentials.value(QStringLiteral("type"),
                                           QStringLiteral("imap")).toString();

    const QString user = credentials.value(QStringLiteral("user")).toString();
    const QString host = credentials.value(QStringLiteral("host")).toString();
    const QString accountId = (!user.isEmpty() && !host.isEmpty())
                            ? user + QLatin1Char('@') + host + QLatin1Char('#') + type
                            : type;

    if (accountById(accountId)) {
        setConfigStatus(QStringLiteral("Account already added: ") + accountId);
        return;
    }

    if (m_vault && m_vault->isLocked()) {
        setConfigStatus(QStringLiteral("Unlock Aerogram first, then add the account"));
        return;
    }

    BackendPlugin *backend = BackendRegistry::create(type, credentials);
    if (!backend) {
        emit errorOccurred(QStringLiteral("Unsupported account type: ") + type);
        return;
    }

    registerAccount(type, credentials, backend);

    // Persist into the encrypted vault DB.
    if (m_vault && !m_vault->isLocked()) {
        AccountStore store(accountsDbPath(), m_vault->key());
        QString err;
        if (!store.open(&err) || !store.add(credentials, &err))
            emit errorOccurred(QStringLiteral("Persist account failed: ") + err);
    }

    backend->startIo();

    m_pendingAdds.insert(accountId);
    setConfigStatus(QStringLiteral("Adding account ") + accountId
                    + QStringLiteral("…"));
    ensureActiveAccount();
}

/// \brief Remove an account: stop IO, unregister, drop the persisted
///        row. The on-disk store is kept (re-adding reuses it).
///        CLI-provided accounts are only unregistered at runtime.
void AccountController::removeAccount(const QString &accountId)
{
    Account *found = accountById(accountId);
    if (!found) {
        emit errorOccurred(QStringLiteral("No such account: ") + accountId);
        return;
    }

    // Value copy up front: removeAt() dangles pointers into the list.
    const Account account = *found;

    account.backend->stopIo();
    account.backend->shutdown();
    // No ghost signals from the removed backend, and no leak: drop our
    // connect()s, then schedule deletion on the event loop.
    disconnect(account.backend, nullptr, this, nullptr);
    account.backend->deleteLater();

    for (int i = 0; i < m_accounts.size(); ++i) {
        if (m_accounts[i].id == accountId) {
            m_accounts.removeAt(i);
            break;
        }
    }
    m_conversationsByAccount.remove(accountId);
    rebuildMergedConversations();
    rebuildAccountsModel();
    if (m_activeAccountId == accountId)
        setActiveAccountId(QString());

    if (m_vault && !m_vault->isLocked() && !account.label.isEmpty()
            && account.label != account.type) {
        // label is user@host; split on LAST '@' (user may be an email
        // address containing '@').
        const int at = account.label.lastIndexOf(QLatin1Char('@'));
        if (at > 0) {
            AccountStore store(accountsDbPath(), m_vault->key());
            QString err;
            if (store.open(&err)
                    && !store.remove(account.type, account.label.left(at),
                                     account.label.mid(at + 1), &err))
                emit errorOccurred(QStringLiteral("Remove persisted account failed: ") + err);
        }
    }

    emit backendsChanged();
    updateLockOverlayVisibility();
    setConfigStatus(QStringLiteral("Account removed: ") + accountId);
}

// ---------------------------------------------------------------------
// Vault flows
// ---------------------------------------------------------------------

void AccountController::onVaultUnlocked()
{
    // Migrate first so imported rows are loaded in the same pass.
    migrateLegacyAccountsJson();
    loadPersistedAccounts();

    const QByteArray key = m_vault->key();
    for (const Account &a : m_accounts) {
        if (auto *aware = dynamic_cast<IMasterKeyAware *>(a.backend)) {
            aware->setMasterKey(key);
            a.backend->startIo();
        }
    }
    fetchConversations();
    ensureActiveAccount();
}

/// \brief Construct + register backends from the vault's encrypted
///        accounts table (post-unlock). Construction goes through the
///        registry like everything else.
void AccountController::loadPersistedAccounts()
{
    if (!m_vault)
        return;

    AccountStore store(accountsDbPath(), m_vault->key());
    QString err;
    if (!store.open(&err)) {
        emit errorOccurred(QStringLiteral("Accounts DB: ") + err);
        return;
    }

    const auto rows = store.list();
    for (const QVariantMap &creds : rows) {
        const QString type = creds.value(QStringLiteral("type")).toString();
        const QString user = creds.value(QStringLiteral("user")).toString();
        const QString host = creds.value(QStringLiteral("host")).toString();
        const QString accountId = (!user.isEmpty() && !host.isEmpty())
                                ? user + QLatin1Char('@') + host + QLatin1Char('#') + type
                                : type;

        if (accountById(accountId))
            continue;

        BackendPlugin *backend = BackendRegistry::create(type, creds);
        if (backend)
            registerAccount(type, creds, backend);
        else
            qWarning().noquote() << "Unknown backend type in vault accounts table:" << type;
    }
}

/// \brief One-time migration: a legacy plaintext accounts.json (from
///        before the vault DB) is imported into the encrypted store,
///        then renamed out of the way.
void AccountController::migrateLegacyAccountsJson()
{
    if (!m_vault)
        return;

    const QString legacy = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                         + QStringLiteral("/accounts.json");
    if (!QFile::exists(legacy))
        return;

    AccountStore store(accountsDbPath(), m_vault->key());
    QString err;
    if (!store.open(&err) || !store.isEmpty())
        return;

    QFile in(legacy);
    if (!in.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll());
    in.close();

    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        QVariantMap creds;
        creds[QStringLiteral("type")] = o.value(QStringLiteral("type")).toString();
        creds[QStringLiteral("host")] = o.value(QStringLiteral("host")).toString();
        creds[QStringLiteral("port")] = o.value(QStringLiteral("port")).toInt(993);
        creds[QStringLiteral("user")] = o.value(QStringLiteral("user")).toString();
        creds[QStringLiteral("pass")] = o.value(QStringLiteral("pass")).toString();
        creds[QStringLiteral("tls")] = o.value(QStringLiteral("tls")).toBool(true);
        store.add(creds);
    }

    QFile::rename(legacy, legacy + QStringLiteral(".migrated"));
    qInfo() << "Migrated legacy accounts.json into the encrypted vault DB";
}

QString AccountController::accountsDbPath() const
{
    return m_vault ? m_vault->vaultDir() + QStringLiteral("/accounts.db") : QString();
}

void AccountController::unlockWithPassphrase(const QString &passphrase)
{
    if (!m_vault)
        return;
    if (m_vault->unlock(passphrase))
        onVaultUnlocked();
}

void AccountController::createVault(const QString &password, const QString &phrase)
{
    if (!m_vault)
        return;
    if (phrase.trimmed().isEmpty()) {
        emit errorOccurred(QStringLiteral("Secret Key phrase must not be empty"));
        return;
    }
    if (m_vault->create(password, phrase))
        onVaultUnlocked();
}

void AccountController::recoverVault(const QString &password, const QString &phrase)
{
    if (!m_vault)
        return;
    if (phrase.trimmed().isEmpty()) {
        emit errorOccurred(QStringLiteral("Secret Key phrase must not be empty"));
        return;
    }
    if (m_vault->recover(password, phrase))
        onVaultUnlocked();
}

/// \brief Rotation. Mode A ("wipe-resync"): the vault rewrites itself
///        with keys from the new password/phrase, then each encrypted
///        backend's store is wiped and resynced. Mode B (re-encrypt in
///        place) is deferred to the vault-UX-polish phase.
void AccountController::rotateVault(const QString &newPassword, const QString &newPhrase,
                                    const QString &mode)
{
    if (!m_vault)
        return;
    if (m_vault->isLocked()) {
        emit errorOccurred(QStringLiteral("Unlock first"));
        return;
    }
    if (mode != QLatin1String("wipe-resync")) {
        emit errorOccurred(QStringLiteral("Rotation mode not implemented: ") + mode);
        return;
    }

    // Capture the old key before rotation — accounts.db is still keyed
    // with it, and sqlite3_rekey needs a handle opened with the old key.
    // detach(): QByteArray is implicitly shared, and the vault's rotate
    // zeroizes its own buffer — a shared copy would be zeroed with it.
    QByteArray oldKey = m_vault->key();
    oldKey.detach();

    // Stop IO first so no in-flight worker writes with the old key
    // into the soon-to-be-wiped store.
    for (const Account &a : m_accounts) {
        if (dynamic_cast<IMasterKeyAware *>(a.backend))
            a.backend->stopIo();
    }

    if (!m_vault->rotate(newPassword, newPhrase, mode)) {
        sodium_memzero(oldKey.data(), oldKey.size());
        emit errorOccurred(QStringLiteral("Rotation failed"));
        return;
    }

    // Re-key the accounts DB in place — it was written with the old
    // dataKey and must follow the vault to the new one, or the next
    // open fails its sqlite_master probe ("file is not a database").
    {
        AccountStore store(accountsDbPath(), oldKey);
        QString err;
        if (!store.open(&err) || !store.rekey(m_vault->key(), &err))
            emit errorOccurred(QStringLiteral("Accounts DB re-key failed: ") + err);
    }
    sodium_memzero(oldKey.data(), oldKey.size());

    const QByteArray key = m_vault->key();
    for (const Account &a : m_accounts) {
        if (auto *aware = dynamic_cast<IMasterKeyAware *>(a.backend)) {
            aware->wipeLocalStore();
            aware->setMasterKey(key);
            a.backend->startIo();
        }
    }
}

void AccountController::setupFromQr(const QString &qrContent)
{
    setConfigStatus(QStringLiteral("Setting up account from QR..."));
    for (const Account &a : m_accounts) {
        if (auto *p = dynamic_cast<IQrSetup *>(a.backend)) {
            p->setupFromQr(qrContent);
            return;
        }
    }
    emit errorOccurred(QStringLiteral("No backend supports QR setup"));
}

void AccountController::getBackupFromQr(const QString &qrText)
{
    setConfigStatus(QStringLiteral("Receiving backup from first device..."));
    for (const Account &a : m_accounts) {
        if (auto *p = dynamic_cast<IQrSetup *>(a.backend)) {
            p->getBackupFromQr(qrText);
            return;
        }
    }
    emit errorOccurred(QStringLiteral("No backend supports QR backup"));
}

void AccountController::configureAccount(const QVariantMap &credentials)
{
    setConfigStatus(QStringLiteral("Configuring account..."));
    for (const Account &a : m_accounts) {
        if (auto *p = dynamic_cast<ICredentialsSetup *>(a.backend)) {
            p->configure(credentials);
            return;
        }
    }
    emit errorOccurred(QStringLiteral("No backend supports credentials setup"));
}

void AccountController::triggerSync()
{
    fetchConversations();
}

void AccountController::resetApp()
{
    // NOTE: encrypted backends lose their configured credentials here;
    // restart is required for them. DeltaChat re-initializes fine.
    for (const Account &a : m_accounts) {
        a.backend->shutdown();
        a.backend->initialize({});
    }
    m_messageModel->setMessages({});
    m_conversationModel->setConversations({});
    m_conversationsByAccount.clear();
    m_activeMessages.clear();
    m_autoSelected = false;
    setConfigStatus(QStringLiteral("Not configured"));
    setActiveAccountId(QString());
    setActiveConversationId(QString());
    setActiveMessageId(QString());
    setActiveMessage({});
    setActiveMessageBody(QString());
    setActiveMessageAttachments({});
    fetchConversations();
}

/// \brief Select an account from the sidebar rail. Email accounts jump
///        straight to their INBOX (no folder picker this phase); chat
///        accounts switch to the chats view filtered to that backend.
///        The account's family comes from plugin->family() — no
///        prefix sniffing.
void AccountController::selectAccount(const QString &accountId)
{
    setActiveAccountId(accountId);
    rebuildMergedConversations();

    Account *account = accountById(accountId);
    const bool isChat = account ? account->backend->family() == QLatin1String("chat")
                                : false;
    setActiveView(isChat ? QStringLiteral("chats") : QStringLiteral("email"));

    if (isChat) {
        if (auto *p = dynamic_cast<IConversationProvider *>(account ? account->backend : nullptr))
            p->fetchConversations();
    } else {
        fetchMessages(accountId + QStringLiteral("/INBOX"));
    }
}

/// \brief Auto-select the first account when nothing is selected.
///        Called after accounts load (unlock path, addAccount) and from
///        main() on non-locked startups.
void AccountController::ensureActiveAccount()
{
    if (!m_activeAccountId.isEmpty() || m_accounts.isEmpty())
        return;
    selectAccount(m_accounts.first().id);
}

void AccountController::updateLockOverlayVisibility()
{
    emit lockOverlayVisibilityChanged();
}
