#ifndef CONTENTPIPELINE_H
#define CONTENTPIPELINE_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

#include "../Types.h"

/// \file ContentPipeline.h
/// \brief The ONE message-content pipeline: parse (KMime) → sanitize
///        (HtmlSanitizer) → safe parts. Backend-agnostic: every backend
///        funnels .eml bytes through here; no backend owns parsing or
///        rendering policy.
///
/// Policy: store raw truth (the .eml), sanitize at READ time — a
/// renderer-side change can never re-expose stored poison.

struct ParsedContent
{
    QString messageId;
    QString subject;
    QString sender;
    QDateTime date;
    QString bodyPlain;        // decoded text/plain (or html→text fallback)
    QString bodyHtmlRaw;      // raw text/html part (UNSANITIZED)
    QString bodyHtmlSafe;     // sanitized for QTextDocument rich text
    bool remoteContentBlocked = false;
    QVector<AttachmentMeta> attachments;
};

class ContentPipeline
{
public:
    /// Parse a raw RFC822 message into content parts. HTML (when
    /// present) is sanitized for display; remote/embedded content is
    /// stripped and reported via remoteContentBlocked.
    static ParsedContent parse(const QByteArray &eml);

    /// Short plain-text snippet for list views.
    static QString snippetFrom(const QString &plain);

    /// Decode attachment `index`'s bytes (decode-on-save).
    static QByteArray extractAttachment(const QByteArray &eml, int index);

    /// Progressive render support: sanitized HTML split into chunks in
    /// document order (the pane appends them as they arrive — first
    /// paint never waits for the whole doc). Chunk boundaries fall on
    /// tag ends. Plain text is always returned whole.
    struct StreamedParts {
        QString plain;
        QStringList htmlChunks;   // sanitized, in order
        bool blockedRemote = false;
    };
    static StreamedParts parseStreamed(const QByteArray &eml, int chunkCount = 4);
};

#endif
