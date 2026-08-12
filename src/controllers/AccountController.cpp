#include "controllers/AccountController.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>

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
    ,     m_activeView(QStringLiteral("email"))
    , m_vault(vault)
{
    // Compute the initial panel layout for the initial view
    // (setActiveView only fires on CHANGE, so the constructor must).
    recomputeLayout();

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
QVariantList AccountController::activeMessages() const { return m_activeMessages; }
QVariantList AccountController::panelLayout() const { return m_panelLayout; }

/// \brief Serialize the registry's picker-visible backend schemas for
///        QML: [{type, displayName, family, description, fields:
///        [{key, label, placeholder, kind, required]}]. Constant per
///        run — registration happens once in main.cpp.
QVariantList AccountController::availableBackends() const
{
    QVariantList out;
    for (const BackendInfo &info : BackendRegistry::backendInfos()) {
        QVariantList fields;
        for (const BackendField &f : info.fields) {
            fields.append(QVariantMap{
                {QStringLiteral("key"), f.key},
                {QStringLiteral("label"), f.label},
                {QStringLiteral("placeholder"), f.placeholder},
                {QStringLiteral("kind"), f.kind},
                {QStringLiteral("required"), f.required},
            });
        }
        out.append(QVariantMap{
            {QStringLiteral("type"), info.type},
            {QStringLiteral("displayName"), info.displayName},
            {QStringLiteral("family"), info.family},
            {QStringLiteral("description"), info.description},
            {QStringLiteral("fields"), fields},
        });
    }
    return out;
}

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

QString AccountController::attachmentSaveStatus() const
{
    return m_attachmentSaveStatus;
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
    // Entering the add-account panel with no attempt in flight starts
    // with a clean status — the previous add's banner ("Connected",
    // "Adding failed: …") must not greet the next attempt.
    if (view == QLatin1String("addAccount") && m_pendingAdds.isEmpty())
        setConfigStatus(QString());
    if (m_activeView != view) {
        m_activeView = view;
        emit activeViewChanged();
        recomputeLayout();
    }
}

/// \brief Controller-owned panel layout: position and size per panel,
///        in window pixels. main.qml pushes window size; the controller
///        owns the geometry. A future second window is another host
///        binding the same model.
void AccountController::setWindowSize(int width, int height)
{
    if (m_windowWidth != width || m_windowHeight != height) {
        m_windowWidth = width;
        m_windowHeight = height;
        recomputeLayout();
    }
}

void AccountController::recomputeLayout()
{
    constexpr int sidebarWidth = 70;
    constexpr int listWidth = 380;
    constexpr int sepWidth = 1;
    // Below this width the email view stacks: conversations on top
    // (35%), messages below (65%), with a horizontal divider.
    constexpr int narrowBelow = 700;

    QVariantList rows;
    int sepN = 0;
    auto add = [&rows](const QString &id, const QString &type,
                       int x, int y, int w, int h) {
        QVariantMap m;
        m[QStringLiteral("id")] = id;
        m[QStringLiteral("type")] = type;
        m[QStringLiteral("x")] = x;
        m[QStringLiteral("y")] = y;
        m[QStringLiteral("width")] = w;
        m[QStringLiteral("height")] = h;
        m[QStringLiteral("visible")] = true;
        rows.append(m);
    };
    auto addVsep = [&](int sx) {
        add(QStringLiteral("sep-%1").arg(sepN++), QStringLiteral("separator"),
            sx, 0, sepWidth, m_windowHeight);
    };
    auto addHsep = [&](int sx, int sy, int sw) {
        add(QStringLiteral("sep-%1").arg(sepN++), QStringLiteral("separator"),
            sx, sy, sw, sepWidth);
    };

    // Sidebar, then thin separators between content panels (Airmail-style
    // dividers). All geometry is controller-owned.
    int x = 0;
    add(QStringLiteral("sidebar"), QStringLiteral("panel"), x, 0, sidebarWidth,
        m_windowHeight);
    x += sidebarWidth;

    const bool narrow = m_windowWidth < narrowBelow;

    if (m_activeView == QLatin1String("chats") ||
        m_activeView == QLatin1String("settings") ||
        m_activeView == QLatin1String("addAccount")) {
        const QString id = m_activeView == QLatin1String("chats")
            ? QStringLiteral("chat-conversations")
            : m_activeView == QLatin1String("settings")
                ? QStringLiteral("settings")
                : QStringLiteral("add-account");
        addVsep(x);
        x += sepWidth;
        add(id, QStringLiteral("panel"), x, 0, m_windowWidth - x, m_windowHeight);
    } else if (narrow) {
        // Stacked: conversations 35% above, messages 65% below.
        addVsep(x);
        x += sepWidth;
        const int contentW = m_windowWidth - x;
        const int topH = m_windowHeight * 35 / 100;
        add(QStringLiteral("email-conversations"), QStringLiteral("panel"),
            x, 0, contentW, topH);
        addHsep(x, topH, contentW);
        add(QStringLiteral("email-messages"), QStringLiteral("panel"),
            x, topH + sepWidth, contentW, m_windowHeight - topH - sepWidth);
    } else {
        addVsep(x);
        x += sepWidth;
        add(QStringLiteral("email-conversations"), QStringLiteral("panel"),
            x, 0, listWidth, m_windowHeight);
        x += listWidth;
        addVsep(x);
        x += sepWidth;
        add(QStringLiteral("email-messages"), QStringLiteral("panel"),
            x, 0, m_windowWidth - x, m_windowHeight);
    }

    if (m_panelLayout != rows) {
        m_panelLayout = rows;
        emit panelLayoutChanged();
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

void AccountController::setActiveMessages(const QVariantList &messages)
{
    m_activeMessages = messages;
    emit activeMessagesChanged();
}

// ---------------------------------------------------------------------
// Account registry. Backends are created via BackendRegistry and held
// through the BackendPlugin interface inside Account entities.
// ---------------------------------------------------------------------

namespace {

/// \brief THE one identity-color derivation: djb2 over the lowercase
///        UTF-16 key, indexed into the shared palette. MUST stay in
///        sync with IdentityBlock.qml's JS implementation — same
///        palette, same algorithm, same key (bare address/label, no
///        backend suffix), so the account rail chip and every sender
///        block for the same address render the same color.
QString identityColor(const QString &key)
{
    static const char *palette[] = {
        "#4c9baf", "#7a5fb5", "#b5546e", "#5f8f4e", "#b58433", "#3f7fa5"
    };
    const QString lower = key.toLower();
    quint32 h = 5381;
    for (const QChar c : lower)
        h = (h << 5) + h + c.unicode();   // quint32 wrap == JS >>> 0
    return QString::fromLatin1(palette[h % (sizeof(palette) / sizeof(palette[0]))]);
}

} // namespace

/// \brief The one place that knows how an account id is derived from
///        credentials: user@host (imap-like), a bare address in "user"
///        (proton), else the type name (deltachat/mock) — plus #type.
static QString computeAccountId(const QString &type, const QVariantMap &credentials,
                                QString *labelOut = nullptr)
{
    const QString user = credentials.value(QStringLiteral("user")).toString();
    const QString host = credentials.value(QStringLiteral("host")).toString();
    QString label;
    if (!user.isEmpty() && !host.isEmpty())
        label = user + QLatin1Char('@') + host;
    else if (user.contains(QLatin1Char('@')))
        label = user;
    else
        label = type;
    if (labelOut)
        *labelOut = label;
    return label + QLatin1Char('#') + type;
}


void AccountController::registerAccount(const QString &type, const QVariantMap &credentials,
                                        BackendPlugin *backend, bool driveConfigure)
{
    Account a;
    a.type = type;
    a.backend = backend;
    a.credentials = credentials;

    // Label/id derivation: prefer the backend-reported identity (real
    // address learned during setup); fall back to the credential-derived
    // rule. One helper keeps the provisional path consistent.
    const QString reported = backend->accountLabel();
    if (!reported.isEmpty()) {
        a.label = reported;
        a.id = reported + QLatin1Char('#') + type;
    } else {
        a.id = computeAccountId(type, credentials, &a.label);
    }

    a.index = m_accounts.size();
    // Color keys on the LABEL (bare address), never the id: the backend
    // suffix must not change an identity's color.
    a.color = identityColor(a.label);

    // The controller owns backend instances (Qt parent-child teardown).
    backend->setParent(this);

    m_accounts.append(a);
    connectBackend(m_accounts.last());
    // configure() emits configured() — must run after connectBackend so
    // the signal isn't lost. Callers that already drove the backend
    // through a provisional attempt (addAccount) pass false here.
    if (driveConfigure) {
        if (auto *c = dynamic_cast<ICredentialsSetup *>(backend))
            c->configure(credentials);
    }
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
                m_currentConversationMessages = fixed;
                m_messageModel->setMessages(fixed);
                // A non-empty landing resets the empty-refetch budget.
                if (!fixed.isEmpty())
                    m_emptyRefetchCount = 0;
                emit messagesChanged(compound);
            });

    connect(backend, &BackendPlugin::messageSent, this,
            [this, accountId](bool ok, const QString &localConvId) {
                emit messageSent(ok, accountId + QStringLiteral("/") + localConvId);
            });

    connect(backend, &BackendPlugin::messageBodyReady, this,
            [this, accountId](const QString &localConvId, const QString &messageId,
                              const QString &body, const QString &bodyHtml,
                              bool remoteContentBlocked) {
                if (messageId == m_activeMessageId) {
                    // Update the entry's body in the active list.
                    QVariantList msgs = m_activeMessages;
                    for (int i = 0; i < msgs.size(); ++i) {
                        QVariantMap e = msgs[i].toMap();
                        if (e.value(QStringLiteral("messageId")).toString() == messageId) {
                            e[QStringLiteral("body")] = body;
                            e[QStringLiteral("bodyHtml")] = bodyHtml;
                            e[QStringLiteral("remoteContentBlocked")] = remoteContentBlocked;
                            msgs[i] = e;
                            setActiveMessages(msgs);
                            break;
                        }
                    }
                }
                emit messageBodyReady(accountId + QStringLiteral("/") + localConvId,
                                      messageId, body, bodyHtml, remoteContentBlocked);
            });

    connect(backend, &BackendPlugin::messageBodyChunkReady, this,
            [this, accountId](const QString &localConvId, const QString &messageId,
                              const QString &htmlChunk, bool lastChunk,
                              bool remoteContentBlocked) {
                // Pure passthrough — do NOT accumulate into the model:
                // a per-chunk model reset would recreate the pane's
                // delegate and wipe its append state. The pane streams
                // imperatively off this signal.
                emit messageBodyChunkReady(accountId + QStringLiteral("/") + localConvId,
                                           messageId, htmlChunk, lastChunk,
                                           remoteContentBlocked);
            });

    connect(backend, &BackendPlugin::attachmentSaved, this,
            [this](bool ok, const QString &messageId, const QString &path) {
                m_attachmentSaveStatus = ok ? QStringLiteral("Saved to ") + path
                                            : QStringLiteral("Save failed");
                emit attachmentSaveStatusChanged();
                emit attachmentSaved(ok, messageId, path);
            });

    // -----------------------------------------------------------------
    // Push events: payload-carrying signals apply immediately (they are
    // already precise); the coarse storageChanged is debounced into a
    // targeted refetch. Everything compounds local ids on the way in.
    // -----------------------------------------------------------------

    connect(backend, &BackendPlugin::messageArrived, this,
            [this, accountId](const QString &localConvId, const Message &msg) {
                if (!accountById(accountId))
                    return;
                const QString compound = accountId + QStringLiteral("/") + localConvId;
                if (compound != m_activeConversationId)
                    return;  // badge arrives via conversationUpserted
                // Dedup: our own sends echo back through the push channel.
                for (const Message &m : m_currentConversationMessages) {
                    if (m.messageId == msg.messageId)
                        return;
                }
                Message fixed = msg;
                fixed.conversationId = compound;
                m_currentConversationMessages.append(fixed);
                m_messageModel->appendMessage(fixed);
                emit messagesChanged(compound);
            });

    connect(backend, &BackendPlugin::conversationUpserted, this,
            [this, accountId](const Conversation &conv) {
                if (!accountById(accountId))
                    return;
                Conversation c = conv;
                c.id = accountId + QStringLiteral("/") + conv.id;
                auto &vec = m_conversationsByAccount[accountId];
                bool found = false;
                for (int i = 0; i < vec.size(); ++i) {
                    if (vec[i].id == c.id) {
                        vec[i] = c;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    vec.append(c);
                rebuildMergedConversations();
            });

    connect(backend, &BackendPlugin::messageRemoved, this,
            [this, accountId](const QString &localConvId, const QString &messageId) {
                if (!accountById(accountId))
                    return;
                const QString compound = accountId + QStringLiteral("/") + localConvId;
                if (compound != m_activeConversationId)
                    return;
                for (int i = 0; i < m_currentConversationMessages.size(); ++i) {
                    if (m_currentConversationMessages.at(i).messageId == messageId) {
                        m_currentConversationMessages.removeAt(i);
                        break;
                    }
                }
                m_messageModel->removeMessageById(messageId);
                emit messagesChanged(compound);
            });

    connect(backend, &BackendPlugin::storageChanged, this,
            [this, accountId]() {
                if (!accountById(accountId))
                    return;
                // Coalesce bursts (e.g. DC IncomingMsgBunch) into one
                // targeted refetch per account.
                QTimer *&timer = m_pushDebounceTimers[accountId];
                if (!timer) {
                    timer = new QTimer(this);
                    timer->setSingleShot(true);
                    timer->setInterval(300);
                    connect(timer, &QTimer::timeout, this, [this, accountId]() {
                        Account *account = accountById(accountId);
                        if (!account)
                            return;
                        if (auto *p = dynamic_cast<IConversationProvider *>(account->backend))
                            p->fetchConversations();
                        if (m_activeConversationId.startsWith(accountId + QStringLiteral("/")))
                            fetchMessages(m_activeConversationId);
                    });
                }
                timer->start();
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
    // sync landed), re-fetch now that the index has content. Capped per
    // conversation — a genuinely empty folder must not refetch on every
    // 60s poll forever.
    if (!m_activeConversationId.isEmpty()
            && m_messageModel->rowCount() == 0) {
        if (m_activeConversationId != m_emptyRefetchId) {
            m_emptyRefetchId = m_activeConversationId;
            m_emptyRefetchCount = 0;
        }
        if (m_emptyRefetchCount < kMaxEmptyRefetches) {
            ++m_emptyRefetchCount;
            fetchMessages(m_activeConversationId);
        }
    }

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

    // Resolve a pending account selection once its conversations land —
    // but ONLY if the user is still on that account. If they clicked
    // away meanwhile, the pending selection is stale and must not yank
    // the conversation list back (the "wrong account" race).
    if (!m_pendingSelectAccount.isEmpty()) {
        if (m_activeAccountId != m_pendingSelectAccount) {
            m_pendingSelectAccount.clear();
        } else if (m_conversationsByAccount.contains(m_pendingSelectAccount)
                   && !m_conversationsByAccount.value(m_pendingSelectAccount).isEmpty()) {
            selectDefaultConversation(accountById(m_pendingSelectAccount));
        }
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

    // The panel contract is a LIST (a thread, later) — v1 carries one.
    QVariantMap entry;
    entry[QStringLiteral("messageId")] = messageId;
    entry[QStringLiteral("body")] = QString();
    entry[QStringLiteral("bodyHtml")] = QString();   // filled by fetch
    entry[QStringLiteral("remoteContentBlocked")] = false;

    QVariantList atts;
    for (const Message &m : m_currentConversationMessages) {
        if (m.messageId != messageId)
            continue;
        entry[QStringLiteral("subject")] = m.subject;
        entry[QStringLiteral("sender")] = m.sender;
        entry[QStringLiteral("date")] = m.date;
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
    entry[QStringLiteral("attachments")] = atts;
    setActiveMessages({entry});

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

/// \brief Add a new account at runtime (panel or IPC).
///
/// PROVISIONAL: the account is NOT registered, shown in the rail, or
/// persisted until the backend proves the credentials work. The proof
/// is ioStarted(true) for every backend (IMAP verifies with LIST
/// there; Proton logs in first; Delta Chat after its QR chain). A
/// failed attempt leaves no pill and no vault row — the backend is
/// discarded and the panel shows the backend's real error text.
void AccountController::addAccount(const QVariantMap &credentials)
{
    const QString type = credentials.value(QStringLiteral("type"),
                                           QStringLiteral("imap")).toString();
    const QString accountId = computeAccountId(type, credentials);

    if (accountById(accountId)) {
        setConfigStatus(QStringLiteral("Account already added: ") + accountId);
        return;
    }

    if (m_vault && m_vault->isLocked()) {
        setConfigStatus(QStringLiteral("Unlock Aerogram first, then add the account"));
        return;
    }

    // Delta Chat credentials carry no address (the invite decides it),
    // and one rpc-server process owns one accounts dir — a second
    // panel-add would collide on the default dir's accounts.lock.
    // Every interactive DC add gets its own store, persisted with the
    // credentials so later launches reuse it. (CLI/dev accounts keep
    // the shared default dir.)
    QVariantMap effectiveCreds = credentials;
    if (type == QLatin1String("deltachat")
            && !effectiveCreds.contains(QStringLiteral("accounts_path"))) {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/delta/add-")
            + QString::fromLatin1(QCryptographicHash::hash(
                  QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8()
                      + QByteArray::number(QRandomGenerator::global()->generate64()),
                  QCryptographicHash::Md5).toHex().left(8));
        effectiveCreds[QStringLiteral("accounts_path")] = dir;
    }

    BackendPlugin *backend = BackendRegistry::create(type, effectiveCreds);
    if (!backend) {
        emit errorOccurred(QStringLiteral("Unsupported account type: ") + type);
        return;
    }
    // Owned by the controller even during the attempt (leak-safe on
    // failure paths before deleteLater gets to run).
    backend->setParent(this);

    m_pendingAdds.insert(accountId);
    emit accountAddInProgressChanged();
    setConfigStatus(QStringLiteral("Adding account ") + accountId
                    + QStringLiteral("…"));

    auto lastError = std::make_shared<QString>();
    auto *attemptTimer = new QTimer(this);
    attemptTimer->setSingleShot(true);
    attemptTimer->setInterval(90'000);

    auto failAttempt = [this, backend, accountId, lastError,
                        attemptTimer](const QString &why) {
        if (accountById(accountId) || !m_pendingAdds.contains(accountId))
            return;  // already resolved
        attemptTimer->stop();
        m_pendingAdds.remove(accountId);
        emit accountAddInProgressChanged();
        QString reason = !lastError->isEmpty() ? *lastError : why;
        if (reason.isEmpty())
            reason = QStringLiteral("the server rejected the account");
        setConfigStatus(QStringLiteral("Adding failed: ") + reason);
        backend->deleteLater();
    };

    // Provisional wiring — resolves the attempt only. Once the account
    // is registered these handlers go silent (the full wiring lives in
    // connectBackend).
    connect(backend, &BackendPlugin::setupProgress, this,
            [this, accountId](const QString &stage) {
                if (!accountById(accountId) && m_pendingAdds.contains(accountId))
                    setConfigStatus(stage);
            });
    connect(backend, &BackendPlugin::errorOccurred, this,
            [lastError](const QString &error) { *lastError = error; });
    connect(backend, &BackendPlugin::configured, this,
            [failAttempt](bool ok) mutable {
                if (!ok)
                    failAttempt(QString());
            });
    connect(backend, &BackendPlugin::ioStarted, this,
            [this, type, effectiveCreds, backend, accountId, attemptTimer, failAttempt]
            (bool ok, const QString &error) mutable {
                if (accountById(accountId) || !m_pendingAdds.contains(accountId))
                    return;
                if (!ok) {
                    failAttempt(error);
                    return;
                }

                // Success. Prefer the backend-reported identity (the
                // real address) over the credential-derived guess —
                // without it every Delta Chat account would register
                // as "deltachat#deltachat" and collide.
                QString realId = accountId;
                const QString reported = backend->accountLabel();
                if (!reported.isEmpty() && reported != accountId) {
                    realId = reported + QLatin1Char('#') + type;
                    // Stamp the identity into the credentials so the
                    // vault reload derives the same id — the backend
                    // learns its address only after configure, too late
                    // for registration at load time.
                    if (effectiveCreds.value(QStringLiteral("user")).toString().isEmpty())
                        effectiveCreds[QStringLiteral("user")] = reported;
                }
                if (accountById(realId)) {
                    failAttempt(QStringLiteral("account already added: ") + realId);
                    return;
                }

                // NOW wire the account through the system.
                attemptTimer->stop();
                registerAccount(type, effectiveCreds, backend,
                                /*driveConfigure=*/false);

                if (m_vault && !m_vault->isLocked()) {
                    AccountStore store(accountsDbPath(), m_vault->key());
                    QString err;
                    if (!store.open(&err) || !store.add(effectiveCreds, &err))
                        emit errorOccurred(QStringLiteral("Persist account failed: ") + err);
                }

                m_pendingAdds.remove(accountId);
                emit accountAddInProgressChanged();
                setConfigStatus(QStringLiteral("Account added: ") + realId);

                if (auto *p = dynamic_cast<IConversationProvider *>(backend))
                    p->fetchConversations();
                ensureActiveAccount();
            });
    connect(attemptTimer, &QTimer::timeout, this,
            [failAttempt]() mutable { failAttempt(QStringLiteral("timed out")); });
    attemptTimer->start();

    // Kick the attempt: configure when the backend has the capability,
    // then start IO. (Delta Chat self-configures from its credentials
    // during initialize; Mock emits immediately.)
    if (auto *c = dynamic_cast<ICredentialsSetup *>(backend))
        c->configure(credentials);
    backend->startIo();
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
    // Delete the account's on-disk data (cached mail, indexes, session
    // stores) — removal means removal, including the cache.
    account.backend->purgeLocalData();
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

    if (m_vault && !m_vault->isLocked()) {
        // The vault row is keyed by the CREDENTIALS (type, user, host) —
        // exactly as add() wrote them. Deriving from the label breaks
        // for backends whose user holds a full address with empty host
        // (Delta Chat, Proton).
        const QString user = account.credentials.value(QStringLiteral("user")).toString();
        const QString host = account.credentials.value(QStringLiteral("host")).toString();
        if (!user.isEmpty() || !host.isEmpty()) {
            AccountStore store(accountsDbPath(), m_vault->key());
            QString err;
            if (store.open(&err)
                    && !store.remove(account.type, user, host, &err))
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
    m_currentConversationMessages.clear();
    m_autoSelected = false;
    setConfigStatus(QStringLiteral("Not configured"));
    setActiveAccountId(QString());
    setActiveConversationId(QString());
    setActiveMessageId(QString());
    setActiveMessages({});
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
        selectDefaultConversation(account);
    }

    // Poll-based backends (IMAP): switching to an account is the moment
    // freshness matters most — sync immediately instead of waiting out
    // the poll interval. Push-capable backends don't implement this.
    if (auto *s = dynamic_cast<ISyncable *>(account ? account->backend : nullptr))
        s->syncNow();
}

/// \brief Pick a sensible conversation for an email-family account:
///        the one named INBOX, else the first. Conversation ids are
///        backend-local (Proton uses numeric label ids, not the string
///        "INBOX") — never hardcode names into ids. If the account has
///        no conversations yet (fresh sync in flight), remember the
///        pending selection; rebuildMergedConversations() resolves it
///        when conversations land.
void AccountController::selectDefaultConversation(Account *account)
{
    if (!account)
        return;
    const auto &convs = m_conversationsByAccount.value(account->id);
    if (convs.isEmpty()) {
        m_pendingSelectAccount = account->id;
        if (auto *p = dynamic_cast<IConversationProvider *>(account->backend))
            p->fetchConversations();
        return;
    }
    QString pick = convs.first().id;
    for (const Conversation &c : convs) {
        if (c.name.compare(QStringLiteral("INBOX"), Qt::CaseInsensitive) == 0) {
            pick = c.id;
            break;
        }
    }
    m_pendingSelectAccount.clear();
    fetchMessages(pick);
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
