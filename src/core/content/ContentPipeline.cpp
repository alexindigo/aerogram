#include "ContentPipeline.h"

#include "HtmlSanitizer.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <KMime/Message>
#include <KMime/Content>
#include <KMime/Headers>
#include <KMime/Util>

ParsedContent ContentPipeline::parse(const QByteArray &eml)
{
    ParsedContent out;

    KMime::Message msg;
    msg.setContent(KMime::CRLFtoLF(eml));  // KMime parses LF; .eml is CRLF
    msg.parse();

    if (const auto *h = msg.messageID(KMime::DontCreate)) {
        QString id = h->asUnicodeString();
        if (id.startsWith(u'<') && id.endsWith(u'>'))
            id = id.mid(1, id.size() - 2);
        out.messageId = id;
    }
    if (const auto *h = msg.subject(KMime::DontCreate))
        out.subject = h->asUnicodeString();
    if (const auto *h = msg.from(KMime::DontCreate)) {
        const auto boxes = h->mailboxes();
        if (!boxes.isEmpty())
            out.sender = boxes.first().prettyAddress();
    }
    if (const auto *h = msg.date(KMime::DontCreate))
        out.date = h->dateTime();
    if (!out.date.isValid())
        out.date = QDateTime::currentDateTime();

    // Bodies. mainBodyPart resolves multipart/alternative correctly; a
    // blank text/plain stub must not win over a real HTML part.
    const KMime::Content *plain = msg.mainBodyPart(QByteArrayLiteral("text/plain"));
    const KMime::Content *html = msg.mainBodyPart(QByteArrayLiteral("text/html"));
    if (plain)
        out.bodyPlain = plain->decodedText();
    if (html)
        out.bodyHtmlRaw = html->decodedText();

    if (!out.bodyHtmlRaw.isEmpty()) {
        const SanitizedBody s = HtmlSanitizer::sanitize(out.bodyHtmlRaw);
        out.bodyHtmlSafe = s.html;
        out.remoteContentBlocked = s.blockedRemote;
        out.bodyHtmlReader = HtmlSanitizer::toReaderHtml(out.bodyHtmlRaw);
        // Plain fallback derives from the READER tree (semantic content,
        // no scripts/tracking), not the raw HTML.
        if (out.bodyPlain.trimmed().isEmpty())
            out.bodyPlain = HtmlSanitizer::toPlainText(out.bodyHtmlReader);
    }

    // Attachments (decode-on-save metadata only).
    int i = 0;
    for (const KMime::Content *c : msg.attachments()) {
        AttachmentMeta a;
        a.index = i++;
        if (const auto *cd = c->contentDisposition())
            a.filename = cd->filename();
        if (const auto *ct = c->contentType())
            a.mimeType = QString::fromLatin1(ct->mimeType());
        a.size = c->decodedBody().size();
        out.attachments.append(a);
    }

    return out;
}

QString ContentPipeline::snippetFrom(const QString &plain)
{
    return plain.simplified().left(140);
}

QByteArray ContentPipeline::extractAttachment(const QByteArray &eml, int index)
{
    KMime::Message msg;
    msg.setContent(KMime::CRLFtoLF(eml));  // KMime parses LF; .eml is CRLF
    msg.parse();
    const auto atts = msg.attachments();
    if (index < 0 || index >= atts.size())
        return {};
    return atts.at(index)->decodedBody();
}

ContentPipeline::StreamedParts ContentPipeline::parseStreamed(const QByteArray &eml,
                                                              int chunkCount)
{
    // Reuse the single-pass parse for metadata + plain.
    StreamedParts out;
    const ParsedContent p = parse(eml);
    out.plain = p.bodyPlain;
    out.blockedRemote = p.remoteContentBlocked;

    if (p.bodyHtmlRaw.isEmpty())
        return out;

    // Stream the raw html through the sanitizer: lol-html flushes clean
    // bytes as it parses, so each write returns a progress chunk.
    // (stream FFI declared in HtmlSanitizer.h)
    const QByteArray rawUtf8 = p.bodyHtmlRaw.toUtf8();
    const int step = qMax(1, rawUtf8.size() / qMax(1, chunkCount));
    void *stream = sanitize_stream_new();

    QStringList flushed;
    for (int i = 0; i < rawUtf8.size(); i += step) {
        const QByteArray part = rawUtf8.mid(i, step);
        char *raw = sanitize_stream_write(stream, part.constData());
        const QJsonObject o =
            QJsonDocument::fromJson(QString::fromUtf8(raw ? raw : "").toUtf8()).object();
        if (raw)
            sanitize_free_string(raw);
        const QString chunk = o.value(QStringLiteral("chunk")).toString();
        if (!chunk.isEmpty())
            flushed.append(chunk);
    }
    char *raw = sanitize_stream_finish(stream);  // frees the stream
    const QJsonObject o =
        QJsonDocument::fromJson(QString::fromUtf8(raw ? raw : "").toUtf8()).object();
    if (raw)
        sanitize_free_string(raw);
    out.blockedRemote = out.blockedRemote
        || o.value(QStringLiteral("blocked_remote")).toBool();
    const QString lastChunk = o.value(QStringLiteral("chunk")).toString();
    if (!lastChunk.isEmpty())
        flushed.append(lastChunk);

    out.htmlChunks = flushed;
    return out;
}

QStringList ContentPipeline::sanitizeStreamed(const QString &rawHtml, int chunkCount)
{
    // Stream through the sanitizer; each flushed chunk is a display
    // fragment in order. (Shared with parseStreamed's html stage.)
    const QByteArray rawUtf8 = rawHtml.toUtf8();
    const int step = qMax(1, rawUtf8.size() / qMax(1, chunkCount));
    void *stream = sanitize_stream_new();

    QStringList flushed;
    for (int i = 0; i < rawUtf8.size(); i += step) {
        const QByteArray part = rawUtf8.mid(i, step);
        char *raw = sanitize_stream_write(stream, part.constData());
        const QJsonObject o =
            QJsonDocument::fromJson(QString::fromUtf8(raw ? raw : "").toUtf8()).object();
        if (raw)
            sanitize_free_string(raw);
        const QString chunk = o.value(QStringLiteral("chunk")).toString();
        if (!chunk.isEmpty())
            flushed.append(chunk);
    }
    char *raw = sanitize_stream_finish(stream);  // frees the stream
    const QJsonObject o =
        QJsonDocument::fromJson(QString::fromUtf8(raw ? raw : "").toUtf8()).object();
    if (raw)
        sanitize_free_string(raw);
    const QString lastChunk = o.value(QStringLiteral("chunk")).toString();
    if (!lastChunk.isEmpty())
        flushed.append(lastChunk);
    return flushed;
}
