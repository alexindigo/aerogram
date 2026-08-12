#include "EmailStore.h"

#include "core/content/ContentPipeline.h"

#include <QDateTime>

EmailStore::EmailStore(const QString &indexDbPath, const QString &storeRoot)
    : m_indexDb(indexDbPath), m_storeRoot(storeRoot)
{
}

EmailStore::~EmailStore()
{
    QMutexLocker lock(&m_mutex);
    delete m_index;
}

void EmailStore::setKey(const QByteArray &key)
{
    QMutexLocker lock(&m_mutex);
    m_key = key;
}

void EmailStore::clearKey()
{
    QMutexLocker lock(&m_mutex);
    m_key.clear();
    delete m_index;   // force re-key on next use
    m_index = nullptr;
}

MetadataIndex *EmailStore::index()
{
    if (!m_index && !m_key.isEmpty()) {
        m_index = new MetadataIndex(m_indexDb, m_key);
        QString err;
        if (!m_index->open(&err)) {
            qWarning() << "EmailStore: index open failed:" << err;
            delete m_index;
            m_index = nullptr;
        }
    }
    return m_index;
}

QVector<Conversation> EmailStore::conversations()
{
    QMutexLocker lock(&m_mutex);
    if (auto *idx = index())
        return idx->conversations();
    return {};
}

QVector<Message> EmailStore::messages(const QString &conversationId)
{
    QMutexLocker lock(&m_mutex);
    if (auto *idx = index())
        return idx->messages(conversationId);
    return {};
}

QString EmailStore::filePathForMessage(const QString &messageId)
{
    QMutexLocker lock(&m_mutex);
    if (auto *idx = index())
        return idx->filePathForMessage(messageId);
    return {};
}

QByteArray EmailStore::readShard(const QString &relPath)
{
    QMutexLocker lock(&m_mutex);
    if (m_key.isEmpty() || relPath.isEmpty())
        return {};
    MessageStore store(m_storeRoot, m_key);
    return store.get(relPath);
}

EmailStore::BodyParts EmailStore::readBodyStreamed(const QString &messageId)
{
    BodyParts out;
    const QString rel = filePathForMessage(messageId);   // (locks internally)
    if (rel.isEmpty())
        return out;
    const QByteArray raw = readShard(rel);
    if (raw.isEmpty())
        return out;
    const auto parts = ContentPipeline::parseStreamed(raw);
    out.found = true;
    out.plain = parts.plain;
    out.htmlChunks = parts.htmlChunks;
    out.blockedRemote = parts.blockedRemote;
    return out;
}

void EmailStore::storeMessage(const QString &conversationId, const QByteArray &eml,
                              const QString &plainBody,
                              const QVector<AttachmentMeta> &attachments,
                              const QString &keyOverride)
{
    if (m_key.isEmpty() || eml.isEmpty())
        return;
    const auto parsed = ContentPipeline::parse(eml);
    const QString keyHint = !keyOverride.isEmpty()
        ? keyOverride
        : parsed.messageId.isEmpty()
            ? (conversationId + QLatin1Char(':') + QString::number(parsed.date.toSecsSinceEpoch()))
            : parsed.messageId;

    QMutexLocker lock(&m_mutex);
    if (m_key.isEmpty())
        return;

    // Content upgrade: a shard written before we stored multipart emls
    // (plain-only) gets replaced when the same message now carries an
    // html part. The shard is a cache of the best representation.
    bool overwrite = false;
    const qint64 tPut = QDateTime::currentMSecsSinceEpoch();
    MessageStore store(m_storeRoot, m_key);
    const QString rel = store.put(eml, keyHint);   // dedup-probe: same path
    if (auto *idx = index()) {
        if (idx->filePathForMessage(keyHint) == rel
                && eml.contains("text/html")) {
            const QByteArray old = store.get(rel);
            if (!old.isEmpty() && !old.contains("text/html"))
                overwrite = true;   // plain-only → multipart upgrade
        }
    }
    if (overwrite)
        store.put(eml, keyHint, /*overwrite=*/true);
    qInfo().noquote() << QStringLiteral("PERF store-put msg=%1 dur=%2 bytes=%3 overwrite=%4")
                             .arg(keyHint)
                             .arg(QDateTime::currentMSecsSinceEpoch() - tPut)
                             .arg(eml.size())
                             .arg(overwrite);

    Message m;
    m.messageId = keyHint;
    m.conversationId = conversationId;
    m.subject = parsed.subject;
    m.sender = parsed.sender;
    m.date = parsed.date;
    m.snippet = ContentPipeline::snippetFrom(parsed.bodyPlain);
    m.isUnread = false;

    const qint64 tFts = QDateTime::currentMSecsSinceEpoch();
    if (auto *idx = index())
        idx->insertMessages({m}, {plainBody}, {rel}, {attachments});
    qInfo().noquote() << QStringLiteral("PERF store-fts msg=%1 dur=%2 plain_len=%3")
                             .arg(keyHint)
                             .arg(QDateTime::currentMSecsSinceEpoch() - tFts)
                             .arg(plainBody.size());
}

void EmailStore::writeFolderSync(const QString &folder,
                                 const QVector<Message> &msgs,
                                 const QVector<QString> &bodies,
                                 const QVector<QByteArray> &rawEmls,
                                 const QVector<QVector<AttachmentMeta>> &attachments,
                                 const QSet<QString> &presentIds,
                                 bool reconcile)
{
    if (m_key.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);

    QVector<QString> paths;
    paths.reserve(rawEmls.size());
    {
        MessageStore store(m_storeRoot, m_key);
        for (int i = 0; i < rawEmls.size(); ++i)
            paths.append(store.put(rawEmls.at(i), msgs.at(i).messageId));
    }

    if (auto *idx = index()) {
        idx->insertMessages(msgs, bodies, paths, attachments);
        if (reconcile) {
            const QVector<QString> gone =
                idx->removeMissingFromConversation(folder, presentIds);
            if (!gone.isEmpty()) {
                MessageStore store(m_storeRoot, m_key);
                for (const QString &rel : gone)
                    store.remove(rel);
            }
        }
    }
}

void EmailStore::upsertConversation(const Conversation &c)
{
    QMutexLocker lock(&m_mutex);
    if (auto *idx = index())
        idx->upsertConversation(c);
}

void EmailStore::setPaths(const QString &indexDbPath, const QString &storeRoot)
{
    QMutexLocker lock(&m_mutex);
    if (indexDbPath != m_indexDb || storeRoot != m_storeRoot) {
        delete m_index;         // path change — reopen lazily
        m_index = nullptr;
        m_indexDb = indexDbPath;
        m_storeRoot = storeRoot;
    }
}

QVector<QString> EmailStore::reconcileConversation(const QString &conversationId,
                                                   const QSet<QString> &presentIds)
{
    QMutexLocker lock(&m_mutex);
    QVector<QString> gone;
    if (auto *idx = index())
        gone = idx->removeMissingFromConversation(conversationId, presentIds);
    for (const QString &rel : gone) {
        MessageStore store(m_storeRoot, m_key);
        store.remove(rel);
    }
    return gone;
}
