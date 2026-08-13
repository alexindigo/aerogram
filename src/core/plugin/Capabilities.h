#ifndef CAPABILITIES_H
#define CAPABILITIES_H

#include <QString>
#include <QVariantMap>

#include "../store/EmailStore.h"

/// \file Capabilities.h
/// \brief Pure-C++ capability contracts a BackendPlugin may implement.
///
/// Capabilities are deliberately NOT QObjects: all signals live on the
/// BackendPlugin base class. The controller probes a plugin with
/// dynamic_cast<IConversationProvider*>(plugin) and calls methods
/// through the interface when present. This keeps the moc surface
/// minimal and lets backends opt into exactly the capabilities they
/// support (e.g. IMAP v1 has no IMessageSender).

class IConversationProvider
{
public:
    virtual ~IConversationProvider() = default;
    virtual void fetchConversations() = 0;
};

class IMessageProvider
{
public:
    virtual ~IMessageProvider() = default;
    virtual void fetchMessages(const QString &conversationId) = 0;
    virtual void fetchMessageBody(const QString &conversationId,
                                  const QString &messageId) = 0;
    /// \brief Decode attachment part \p partIndex of \p messageId and
    ///        write it to \p destinationPath (user-chosen via Save
    ///        dialog). Emits attachmentSaved on completion. Decode-on-
    ///        save: no app-side copy is kept.
    virtual void saveAttachment(const QString &messageId, int partIndex,
                                const QString &destinationPath) = 0;
    /// \brief Raw message source (.eml) for the pane's Raw view.
    ///        Empty when the backend has no local store (or miss).
    virtual QString rawMessageSource(const QString &messageId)
    {
        Q_UNUSED(messageId);
        return {};
    }
    /// \brief All presentations of a message from the local store
    ///        (store-as-firewall: the UI reads these, never backend
    ///        payloads). found=false when not stored.
    virtual EmailStore::BodyViews readBodyViews(const QString &messageId)
    {
        Q_UNUSED(messageId);
        return {};
    }
};

class IMessageSender
{
public:
    virtual ~IMessageSender() = default;
    virtual void sendMessage(const QString &conversationId,
                             const QString &text) = 0;
};

/// \brief QR-based account setup (Delta Chat).
class IQrSetup
{
public:
    virtual ~IQrSetup() = default;
    virtual void setupFromQr(const QString &qrContent) = 0;
    virtual void getBackupFromQr(const QString &qrText) = 0;
};

/// \brief Host/credentials account setup (IMAP, Proton, ...).
class ICredentialsSetup
{
public:
    virtual ~ICredentialsSetup() = default;
    virtual void configure(const QVariantMap &credentials) = 0;
};

/// \brief Backends with encrypted-at-rest storage participate in the
///        vault lifecycle through this capability: the controller hands
///        the master key on unlock and drives store wipes on rotation
///        — without knowing the concrete backend type.
class IMasterKeyAware
{
public:
    virtual ~IMasterKeyAware() = default;
    virtual void setMasterKey(const QByteArray &key) = 0;
    virtual void wipeLocalStore() = 0;
};

/// \brief Backends that poll (IMAP's 60s timer) can be poked for an
///        immediate sync — e.g. when the user activates the account.
///        Push-capable backends (Delta Chat, Proton) don't need this.
class ISyncable
{
public:
    virtual ~ISyncable() = default;
    virtual void syncNow() = 0;
};

#endif
