#include "controllers/AccountController.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include "core/plugin/Capabilities.h"
#include "core/imap/ImapBackend.h"

AccountController::AccountController(const QList<QPair<QString, BackendPlugin *>> &backends,
                                     MasterKeyManager *vault,
                                     QObject *parent)
    : QObject(parent)
    , m_backends(backends)
    , m_messageModel(new MessageListModel(this))
    , m_conversationModel(new ConversationListModel(this))
    , m_configStatus(QStringLiteral("Not configured"))
    , m_activeView(QStringLiteral("settings"))
    , m_vault(vault)
{
    for (const auto &pair : m_backends)
        connectBackend(pair.first, pair.second);

    if (m_vault) {
        connect(m_vault, &MasterKeyManager::isLockedChanged,
                this, &AccountController::isLockedChanged);
        connect(m_vault, &MasterKeyManager::statusTextChanged,
                this, &AccountController::lockStatusTextChanged);
        connect(m_vault, &MasterKeyManager::vaultStateChanged,
                this, &AccountController::vaultStateChanged);
    }
}

MessageListModel *AccountController::messageListModel() const
{
    return m_messageModel;
}

ConversationListModel *AccountController::conversationListModel() const
{
    return m_conversationModel;
}

QString AccountController::configStatus() const
{
    return m_configStatus;
}

QString AccountController::activeView() const
{
    return m_activeView;
}

QString AccountController::activeAccountId() const
{
    return m_activeAccountId;
}

QString AccountController::activeConversationId() const
{
    return m_activeConversationId;
}

QString AccountController::activeMessageId() const
{
    return m_activeMessageId;
}

QVariantMap AccountController::activeMessage() const
{
    return m_activeMessage;
}

QString AccountController::activeMessageBody() const
{
    return m_activeMessageBody;
}

QVariantList AccountController::activeMessageAttachments() const
{
    return m_activeMessageAttachments;
}

bool AccountController::isLocked() const
{
    return m_vault ? m_vault->isLocked() : false;
}

QString AccountController::lockStatusText() const
{
    return m_vault ? m_vault->statusText() : QString();
}

bool AccountController::vaultExists() const
{
    return m_vault ? m_vault->vaultExists() : false;
}

bool AccountController::vaultNeedsRecovery() const
{
    return m_vault ? m_vault->vaultNeedsRecovery() : false;
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
// Backend wiring. Per-backend lambdas capture the accountId so incoming
// local IDs get compounded ("<accountId>/<localId>") on the way in.
// ---------------------------------------------------------------------

void AccountController::connectBackend(const QString &accountId, BackendPlugin *backend)
{
    connect(backend, &BackendPlugin::conversationsReady, this,
            [this, accountId](const QVector<Conversation> &convs) {
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
                if (!ok)
                    setConfigStatus(QStringLiteral("IO start failed: ") + error);
            });

    connect(backend, &BackendPlugin::ioStopped, this,
            [this, accountId]() {
                emit ioStopped(accountId);
            });
}

/// \brief Resolve a compound conversationId to its backend, optionally
///        returning the backend-local ID. Split at the FIRST '/':
///        accountIds never contain '/', folder names may.
BackendPlugin *AccountController::backendFor(const QString &compoundConversationId,
                                             QString *localId) const
{
    const int slash = compoundConversationId.indexOf(QLatin1Char('/'));
    if (slash < 0)
        return nullptr;
    const QString accountId = compoundConversationId.left(slash);
    if (localId)
        *localId = compoundConversationId.mid(slash + 1);
    for (const auto &pair : m_backends) {
        if (pair.first == accountId)
            return pair.second;
    }
    return nullptr;
}

void AccountController::rebuildMergedConversations()
{
    QVector<Conversation> merged;
    for (const auto &convs : m_conversationsByAccount)
        merged += convs;
    m_conversationModel->setConversations(merged);
    emit conversationsChanged();

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
    for (const auto &pair : m_backends) {
        if (auto *p = dynamic_cast<IConversationProvider *>(pair.second))
            p->fetchConversations();
    }
}

void AccountController::fetchMessages(const QString &conversationId)
{
    setActiveConversationId(conversationId);
    QString localId;
    BackendPlugin *backend = backendFor(conversationId, &localId);
    if (auto *p = dynamic_cast<IMessageProvider *>(backend)) {
        p->fetchMessages(localId);
    } else {
        emit errorOccurred(QStringLiteral("No message provider for %1").arg(conversationId));
    }
}

void AccountController::fetchMessageBody(const QString &conversationId, const QString &messageId)
{
    QString localId;
    BackendPlugin *backend = backendFor(conversationId, &localId);
    if (auto *p = dynamic_cast<IMessageProvider *>(backend)) {
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
    BackendPlugin *backend = backendFor(m_activeConversationId, nullptr);
    if (auto *p = dynamic_cast<IMessageProvider *>(backend)) {
        p->saveAttachment(messageId, partIndex, destinationPath);
    } else {
        emit attachmentSaved(false, messageId, destinationPath);
    }
}

void AccountController::sendMessage(const QString &conversationId, const QString &text)
{
    QString localId;
    BackendPlugin *backend = backendFor(conversationId, &localId);
    if (auto *p = dynamic_cast<IMessageSender *>(backend)) {
        p->sendMessage(localId, text);
    } else {
        emit errorOccurred(QStringLiteral("Backend for %1 cannot send messages")
                               .arg(conversationId));
    }
}

void AccountController::setupFromQr(const QString &qrContent)
{
    setConfigStatus(QStringLiteral("Setting up account from QR..."));
    for (const auto &pair : m_backends) {
        if (auto *p = dynamic_cast<IQrSetup *>(pair.second)) {
            p->setupFromQr(qrContent);
            return;
        }
    }
    emit errorOccurred(QStringLiteral("No backend supports QR setup"));
}

void AccountController::getBackupFromQr(const QString &qrText)
{
    setConfigStatus(QStringLiteral("Receiving backup from first device..."));
    for (const auto &pair : m_backends) {
        if (auto *p = dynamic_cast<IQrSetup *>(pair.second)) {
            p->getBackupFromQr(qrText);
            return;
        }
    }
    emit errorOccurred(QStringLiteral("No backend supports QR backup"));
}

void AccountController::configureAccount(const QVariantMap &credentials)
{
    setConfigStatus(QStringLiteral("Configuring account..."));
    for (const auto &pair : m_backends) {
        if (auto *p = dynamic_cast<ICredentialsSetup *>(pair.second)) {
            p->configure(credentials);
            return;
        }
    }
    emit errorOccurred(QStringLiteral("No backend supports credentials setup"));
}

/// \brief Add a new account at runtime (from the add-account dialog or
///        IPC). Constructs, configures, registers, and starts the
///        backend, then persists credentials to
///        <AppConfigLocation>/accounts.json (0600) so the account
///        survives relaunch without CLI flags.
void AccountController::addAccount(const QVariantMap &credentials)
{
    const QString type = credentials.value(QStringLiteral("type"),
                                           QStringLiteral("imap")).toString();
    if (type != QLatin1String("imap")) {
        emit errorOccurred(QStringLiteral("Unsupported account type: ") + type);
        return;
    }

    const QString accountId = QStringLiteral("imap:")
                            + credentials.value(QStringLiteral("user")).toString()
                            + QStringLiteral("@")
                            + credentials.value(QStringLiteral("host")).toString();

    for (const auto &pair : m_backends) {
        if (pair.first == accountId) {
            setConfigStatus(QStringLiteral("Account already added: ") + accountId);
            return;
        }
    }

    if (m_vault && m_vault->isLocked()) {
        setConfigStatus(QStringLiteral("Unlock Aerogram first, then add the account"));
        return;
    }

    // Prototype note: the controller constructing a concrete backend is
    // a deliberate shortcut — a factory plugin registry is the eventual
    // home for this (see docs/CLEANUP.md).
    auto *backend = new ImapBackend(this);
    backend->initialize({});
    backend->configure(credentials);
    if (m_vault && !m_vault->isLocked())
        backend->setMasterKey(m_vault->key());

    m_backends.append({accountId, backend});
    connectBackend(accountId, backend);
    persistAccount(credentials);
    backend->startIo();

    setConfigStatus(QStringLiteral("Account added: ") + accountId);
}

void AccountController::persistAccount(const QVariantMap &credentials)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/accounts.json");

    QJsonArray arr;
    QFile in(path);
    if (in.open(QIODevice::ReadOnly)) {
        arr = QJsonDocument::fromJson(in.readAll()).array();
        in.close();
    }

    QVariantMap toStore = credentials;
    toStore.insert(QStringLiteral("type"), QStringLiteral("imap"));
    arr.append(QJsonObject::fromVariantMap(toStore));

    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        out.close();
        // Credentials on disk: owner-only.
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
}

/// \brief Shared post-unlock path — every successful vault entry point
///        (create/unlock/recover) lands here: hand the data key to
///        encrypted backends, start their IO, fetch conversations.
void AccountController::onVaultUnlocked()
{
    const QByteArray key = m_vault->key();
    for (const auto &pair : m_backends) {
        if (auto *imap = qobject_cast<ImapBackend *>(pair.second)) {
            imap->setMasterKey(key);
            imap->startIo();
        }
    }
    fetchConversations();
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

    // Stop IO first so no in-flight worker writes with the old key
    // into the soon-to-be-wiped store.
    for (const auto &pair : m_backends) {
        if (auto *imap = qobject_cast<ImapBackend *>(pair.second))
            imap->stopIo();
    }

    if (!m_vault->rotate(newPassword, newPhrase, mode)) {
        emit errorOccurred(QStringLiteral("Rotation failed"));
        return;
    }

    const QByteArray key = m_vault->key();
    for (const auto &pair : m_backends) {
        if (auto *imap = qobject_cast<ImapBackend *>(pair.second)) {
            imap->wipeLocalStore();
            imap->setMasterKey(key);
            imap->startIo();
        }
    }
}

void AccountController::triggerSync()
{
    fetchConversations();
}

void AccountController::resetApp()
{
    // NOTE: IMAP backends lose their configured credentials here;
    // restart is required for them. DeltaChat re-initializes fine.
    for (const auto &pair : m_backends) {
        pair.second->shutdown();
        pair.second->initialize({});
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

void AccountController::selectAccount(const QString &accountId)
{
    setActiveAccountId(accountId);
}
