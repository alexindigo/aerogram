#ifndef EMAILSTORE_H
#define EMAILSTORE_H

#include <QDateTime>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include "MetadataIndex.h"
#include "MessageStore.h"
#include "core/Types.h"

/// \brief The per-account email store facade — the ONE place mail is
///        read from / written to, identical for every email backend
///        (IMAP, Proton, future Gmail…). Owns the persistent index
///        connection (one KDF per session, not per op) and the shard
///        store; serializes access on an internal mutex.
///
/// This is the store-as-firewall made concrete: backends never parse
/// or read shards themselves — they call EmailStore.
class EmailStore
{
public:
    /// Paths may be unknown at construction (backends learn them at
    /// configure() time) — setPaths() before use.
    EmailStore(const QString &indexDbPath = {}, const QString &storeRoot = {});
    ~EmailStore();

    /// Hand in the vault master key (post-unlock). Reads/writes fail
    /// closed before that.
    void setKey(const QByteArray &key);
    void clearKey();  // on lock/rotation
    /// Paths are known at configure() time for most backends.
    void setPaths(const QString &indexDbPath, const QString &storeRoot);
    /// Drop memberships absent from presentIds; orphans (no labels
    /// left) lose their rows + shards. Returns removed shard paths.
    QVector<QString> reconcileConversation(const QString &conversationId,
                                           const QSet<QString> &presentIds);

    // ---- reads ----
    QVector<Conversation> conversations();
    QVector<Message> messages(const QString &conversationId);
    QString filePathForMessage(const QString &messageId);
    QByteArray readShard(const QString &relPath);

    struct BodyParts {
        bool found = false;
        QString plain;
        QStringList htmlChunks;   // sanitized, display order
        bool blockedRemote = false;
    };
    /// Store-first body read: index lookup → shard → pipeline
    /// (parse + streamed sanitize). found=false on miss.
    BodyParts readBodyStreamed(const QString &messageId);
    /// Raw .eml bytes for a message (empty on miss). Used by the
    /// message-pane Raw view — never parsed/sanitized.
    QByteArray readRawEml(const QString &messageId);

    /// Universal content views of a message — the ONLY way UI gets body
    /// content (store-as-firewall). One shard read + one KMime parse
    /// produces all four presentations + the headers bag.
    struct BodyViews {
        bool found = false;
        QString textOnly;       // plain text (MIME plain or Reader→text)
        QString readerHtml;     // calm Reader document (default view)
        QString sanitizedHtml;  // full Qt-safe HTML for the HTML view
        bool blockedRemote = false;
        QVariantMap headers;    // name → list of facet maps
        QString subject, sender;      // envelope projections
        QDateTime date;
        QVector<AttachmentMeta> attachments;
    };
    BodyViews readBodyViews(const QString &messageId);

    // ---- writes ----
    /// Persist one message as an .eml (put shard + index row).
    /// keyOverride: backends whose model ids are NOT the RFC822
    /// Message-Id (Proton: remote ids) MUST pass it — reads, reconciles
    /// and listings all key off the model id, never the header id.
    void storeMessage(const QString &conversationId, const QByteArray &eml,
                      const QString &plainBody,
                      const QVector<AttachmentMeta> &attachments = {},
                      const QString &keyOverride = {});
    /// Batch write + reconcile a folder listing (IMAP sync).
    void writeFolderSync(const QString &folder,
                         const QVector<Message> &msgs,
                         const QVector<QString> &bodies,
                         const QVector<QByteArray> &rawEmls,
                         const QVector<QVector<AttachmentMeta>> &attachments,
                         const QSet<QString> &presentIds,
                         bool reconcile);
    void upsertConversation(const Conversation &c);

private:
    MetadataIndex *index();  // lazy open; caller holds m_mutex

    QMutex m_mutex;
    QString m_indexDb;
    QString m_storeRoot;
    QByteArray m_key;
    MetadataIndex *m_index = nullptr;
};

#endif
