#ifndef ACCOUNTCONTROLLER_H
#define ACCOUNTCONTROLLER_H

#include <QMap>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include "models/MessageListModel.h"
#include "models/ConversationListModel.h"
#include "models/AccountListModel.h"
#include "core/Account.h"
#include "core/crypto/MasterKeyManager.h"
#include "core/plugin/BackendPlugin.h"

/// \brief Single source of truth. Owns first-class Account entities
///        (each holding its backend through the BackendPlugin
///        interface) and routes compound conversation IDs
///        ("<accountId>/<localId>") to the right account's backend.
///
///        The controller never names a concrete backend class:
///        construction goes through BackendRegistry, key/store flows
///        through the IMasterKeyAware capability, and UI grouping reads
///        plugin->family().
class AccountController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MessageListModel* messageListModel READ messageListModel CONSTANT)
    Q_PROPERTY(ConversationListModel* conversationListModel READ conversationListModel CONSTANT)
    Q_PROPERTY(AccountListModel* accountsModel READ accountsModel CONSTANT)
    Q_PROPERTY(QString iconPackDir READ iconPackDir CONSTANT)
    Q_PROPERTY(QString configStatus READ configStatus NOTIFY configStatusChanged)
    Q_PROPERTY(QString activeView READ activeView WRITE setActiveView NOTIFY activeViewChanged)
    Q_PROPERTY(QString activeAccountId READ activeAccountId NOTIFY activeAccountIdChanged)
    Q_PROPERTY(QString activeConversationId READ activeConversationId NOTIFY activeConversationIdChanged)
    Q_PROPERTY(QString activeMessageId READ activeMessageId NOTIFY activeMessageIdChanged)
    Q_PROPERTY(QVariantMap activeMessage READ activeMessage NOTIFY activeMessageChanged)
    Q_PROPERTY(QString activeMessageBody READ activeMessageBody NOTIFY activeMessageBodyChanged)
    Q_PROPERTY(QVariantList activeMessageAttachments READ activeMessageAttachments NOTIFY activeMessageAttachmentsChanged)
    Q_PROPERTY(bool isLocked READ isLocked NOTIFY isLockedChanged)
    Q_PROPERTY(QString lockStatusText READ lockStatusText NOTIFY lockStatusTextChanged)
    Q_PROPERTY(bool vaultExists READ vaultExists NOTIFY vaultStateChanged)
    Q_PROPERTY(bool vaultNeedsRecovery READ vaultNeedsRecovery NOTIFY vaultStateChanged)
    Q_PROPERTY(bool hasEncryptedBackend READ hasEncryptedBackend NOTIFY backendsChanged)
    Q_PROPERTY(bool hasNoAccounts READ hasNoAccounts NOTIFY backendsChanged)
    Q_PROPERTY(bool showLockOverlay READ showLockOverlay NOTIFY lockOverlayVisibilityChanged)

public:
    explicit AccountController(const QList<QPair<QString, QVariantMap>> &accountSpecs,
                               MasterKeyManager *vault = nullptr,
                               QObject *parent = nullptr);

    MessageListModel *messageListModel() const;
    ConversationListModel *conversationListModel() const;
    AccountListModel *accountsModel() const;
    QString iconPackDir() const;
    void setIconPackDir(const QString &dir);
    QString configStatus() const;
    QString activeView() const;
    QString activeAccountId() const;
    QString activeConversationId() const;
    QString activeMessageId() const;
    QVariantMap activeMessage() const;
    QString activeMessageBody() const;
    QVariantList activeMessageAttachments() const;
    bool isLocked() const;
    QString lockStatusText() const;
    bool vaultExists() const;
    bool vaultNeedsRecovery() const;
    bool hasEncryptedBackend() const;
    bool hasNoAccounts() const;
    bool showLockOverlay() const;

public slots:
    void fetchConversations();
    void fetchMessages(const QString &conversationId);
    void fetchMessageBody(const QString &conversationId, const QString &messageId);
    void selectMessage(const QString &messageId);
    void saveAttachment(const QString &messageId, int partIndex,
                        const QString &destinationPath);
    void sendMessage(const QString &conversationId, const QString &text);
    void addAccount(const QVariantMap &credentials);
    void removeAccount(const QString &accountId);
    void unlockWithPassphrase(const QString &passphrase);
    void createVault(const QString &password, const QString &phrase);
    void recoverVault(const QString &password, const QString &phrase);
    void rotateVault(const QString &newPassword, const QString &newPhrase, const QString &mode);
    void setupFromQr(const QString &qrContent);
    void getBackupFromQr(const QString &qrText);
    void configureAccount(const QVariantMap &credentials);
    void triggerSync();
    void resetApp();
    void setActiveView(const QString &view);
    void selectAccount(const QString &accountId);
    void ensureActiveAccount();

signals:
    void configStatusChanged();
    void activeViewChanged();
    void activeAccountIdChanged();
    void activeConversationIdChanged();
    void activeMessageIdChanged();
    void activeMessageChanged();
    void activeMessageBodyChanged();
    void activeMessageAttachmentsChanged();
    void conversationsChanged();
    void messagesChanged(const QString &conversationId);
    void messageSent(bool ok, const QString &conversationId);
    void messageBodyReady(const QString &conversationId, const QString &messageId,
                          const QString &body);
    void attachmentSaved(bool ok, const QString &messageId, const QString &path);
    void ioStarted(const QString &accountId, bool ok, const QString &error);
    void ioStopped(const QString &accountId);
    void errorOccurred(const QString &error);
    void isLockedChanged();
    void lockStatusTextChanged();
    void vaultStateChanged();
    void backendsChanged();
    void lockOverlayVisibilityChanged();

private:
    void setConfigStatus(const QString &status);
    void setActiveAccountId(const QString &accountId);
    void setActiveConversationId(const QString &conversationId);
    void setActiveMessageId(const QString &messageId);
    void setActiveMessage(const QVariantMap &message);
    void setActiveMessageBody(const QString &body);
    void setActiveMessageAttachments(const QVariantList &attachments);

    void registerAccount(const QString &type, const QVariantMap &credentials,
                         BackendPlugin *backend);
    void connectBackend(const Account &account);
    Account *accountById(const QString &accountId);
    Account *accountForConversation(const QString &compoundConversationId,
                                    QString *localId = nullptr);
    void rebuildMergedConversations();
    void rebuildAccountsModel();
    void loadPersistedAccounts();
    void migrateLegacyAccountsJson();
    QString accountsDbPath() const;
    void onVaultUnlocked();
    void updateLockOverlayVisibility();

    QList<Account> m_accounts;
    QMap<QString, QVector<Conversation>> m_conversationsByAccount;
    MessageListModel *m_messageModel;
    ConversationListModel *m_conversationModel;
    AccountListModel *m_accountsModel;
    QString m_iconPackDir;
    QVector<Message> m_activeMessages;
    QString m_configStatus;
    QString m_activeView;
    QString m_activeAccountId;
    QString m_activeConversationId;
    QString m_activeMessageId;
    QVariantMap m_activeMessage;
    QString m_activeMessageBody;
    QVariantList m_activeMessageAttachments;
    MasterKeyManager *m_vault;
    QSet<QString> m_pendingAdds;
    bool m_autoSelected = false;
};

#endif
