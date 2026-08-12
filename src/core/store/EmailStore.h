#ifndef EMAILSTORE_H
#define EMAILSTORE_H

#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>
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
