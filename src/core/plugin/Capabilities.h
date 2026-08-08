#ifndef CAPABILITIES_H
#define CAPABILITIES_H

#include <QString>
#include <QVariantMap>

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

#endif
