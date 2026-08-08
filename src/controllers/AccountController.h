#ifndef ACCOUNTCONTROLLER_H
#define ACCOUNTCONTROLLER_H

#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include "models/MessageListModel.h"
#include "models/ConversationListModel.h"
#include "core/plugin/BackendPlugin.h"
#include "core/crypto/MasterKeyManager.h"

/// \brief Single source of truth. Holds all backends keyed by
///        accountId and routes compound conversation IDs
///        ("<accountId>/<localId>") to the right backend. Backends
///        stay account-unaware; compounding happens here.
class AccountController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MessageListModel* messageListModel READ messageListModel CONSTANT)
    Q_PROPERTY(ConversationListModel* conversationListModel READ conversationListModel CONSTANT)
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

public:
    explicit AccountController(const QList<QPair<QString, BackendPlugin *>> &backends,
                               MasterKeyManager *vault = nullptr,
                               QObject *parent = nullptr);

    MessageListModel *messageListModel() const;
    ConversationListModel *conversationListModel() const;
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

public slots:
    void fetchConversations();
    void fetchMessages(const QString &conversationId);
    void fetchMessageBody(const QString &conversationId, const QString &messageId);
    void selectMessage(const QString &messageId);
    void saveAttachment(const QString &messageId, int partIndex,
                        const QString &destinationPath);
    void sendMessage(const QString &conversationId, const QString &text);
    void addAccount(const QVariantMap &credentials);
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
    void isLockedChanged();
    void lockStatusTextChanged();
    void vaultStateChanged();
    void messageSent(bool ok, const QString &conversationId);
    void messageBodyReady(const QString &conversationId, const QString &messageId,
                          const QString &body);
    void attachmentSaved(bool ok, const QString &messageId, const QString &path);
    void ioStarted(const QString &accountId, bool ok, const QString &error);
    void ioStopped(const QString &accountId);
    void errorOccurred(const QString &error);

private:
    void setConfigStatus(const QString &status);
    void setActiveAccountId(const QString &accountId);
    void setActiveConversationId(const QString &conversationId);
    void setActiveMessageId(const QString &messageId);
    void setActiveMessage(const QVariantMap &message);
    void setActiveMessageBody(const QString &body);
    void setActiveMessageAttachments(const QVariantList &attachments);

    void connectBackend(const QString &accountId, BackendPlugin *backend);
    BackendPlugin *backendFor(const QString &compoundConversationId,
                              QString *localId = nullptr) const;
    void rebuildMergedConversations();
    void persistAccount(const QVariantMap &credentials);
    void onVaultUnlocked();

    QList<QPair<QString, BackendPlugin *>> m_backends;
    QMap<QString, QVector<Conversation>> m_conversationsByAccount;
    MessageListModel *m_messageModel;
    ConversationListModel *m_conversationModel;
    QVector<Message> m_activeMessages;
    QString m_configStatus;
    QString m_activeView;
    QString m_activeAccountId;
    QString m_activeConversationId;
    QString m_activeMessageId;
    QVariantMap m_activeMessage;
    QString m_activeMessageBody;
    QVariantList m_activeMessageAttachments;
    MasterKeyManager *m_vault;  // may be null (no encrypted backends)
    bool m_autoSelected = false;
};

#endif
