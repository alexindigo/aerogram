#include "ContentPipeline.h"

#include "HtmlSanitizer.h"

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

    if (out.bodyPlain.trimmed().isEmpty() && !out.bodyHtmlRaw.isEmpty())
        out.bodyPlain = HtmlSanitizer::toPlainText(out.bodyHtmlRaw);

    if (!out.bodyHtmlRaw.isEmpty()) {
        const SanitizedBody s = HtmlSanitizer::sanitize(out.bodyHtmlRaw);
        out.bodyHtmlSafe = s.html;
        out.remoteContentBlocked = s.blockedRemote;
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
