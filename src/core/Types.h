#ifndef TYPES_H
#define TYPES_H

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVector>

/// \brief A conversation container. For chat backends this is a chat;
///        for email backends this is a folder (v1) or a derived thread
///        (later). The `kind` discriminator carries the distinction so
///        the UI can render accordingly.
///
/// In multi-account setups `id` is compound (`<accountId>/<localId>`,
/// e.g. "imap:alice@gmail.com/INBOX"); `accountLabel` is the short
/// human-readable account tag (e.g. "alice@gmail.com") for display.
struct Conversation {
    QString id;
    QString kind;        // "chat" | "folder"
    QString name;
    QString preview;     // last-message snippet (best effort)
    QString accountLabel;
    int unreadCount = 0;
    QDateTime lastActivity;
};

/// \brief Attachment metadata. Bytes are NOT stored separately — the
///        .eml is the source of truth; `index` is the part position
///        used by extractAttachment/saveAttachment.
struct AttachmentMeta {
    int index = 0;
    QString filename;
    QString mimeType;
    qint64 size = 0;     // decoded size, bytes
};

/// \brief A single message within a conversation. Email-flavored
///        fields (subject) are empty for chat backends; `body` is
///        empty in list views and populated on open via
///        fetchMessageBody.
struct Message {
    QString messageId;
    QString conversationId;   // compound in multi-account setups
    QString subject;
    QString sender;
    QDateTime date;
    QString snippet;
    QString body;
    bool isUnread = false;
    QVector<AttachmentMeta> attachments;
};

Q_DECLARE_METATYPE(Conversation)
Q_DECLARE_METATYPE(QVector<Conversation>)
Q_DECLARE_METATYPE(AttachmentMeta)
Q_DECLARE_METATYPE(QVector<AttachmentMeta>)
Q_DECLARE_METATYPE(Message)
Q_DECLARE_METATYPE(QVector<Message>)

#endif
