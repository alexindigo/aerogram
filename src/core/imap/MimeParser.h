#ifndef MIMEPARSER_H
#define MIMEPARSER_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QString>

#include "core/Types.h"

/// \brief Minimal hand-rolled MIME parser for the prototype (KMime is
///        not packaged on this system; see
///        plans/imap-backend-prototype/plan.md). Extracts headers
///        (Subject, From, Date, Message-ID) and a best-effort
///        text/plain body. NOT a general MIME implementation — swap
///        for KMime when available.
namespace MimeParser {

struct ParsedMessage
{
    QString messageId;
    QString subject;
    QString sender;
    QDateTime date;
    QString bodyPlain;
};

inline QByteArray qpDecode(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const char c = in.at(i);
        if (c == '=' && i + 1 < in.size()) {
            if (in.at(i + 1) == '\r' && i + 2 < in.size() && in.at(i + 2) == '\n') {
                i += 2;
                continue;
            }
            if (in.at(i + 1) == '\n') {
                i += 1;
                continue;
            }
            if (i + 2 < in.size() && isxdigit(in.at(i + 1)) && isxdigit(in.at(i + 2))) {
                const QByteArray hex = in.mid(i + 1, 2);
                out.append(QByteArray::fromHex(hex).at(0));
                i += 2;
                continue;
            }
        }
        out.append(c);
    }
    return out;
}

/// \brief Decode RFC 2047 encoded words (=?charset?B|Q?text?=).
inline QString decodeEncodedWords(const QString &in)
{
    static const QRegularExpression wordRe(
        QStringLiteral("=\\?([^?]+)\\?([bBqQ])\\?([^?]*)\\?="));

    QString out = in;
    auto it = wordRe.globalMatch(in);
    // Apply replacements from last to first so offsets stay valid.
    QList<std::pair<int, int>> spans;
    QList<QString> decoded;
    while (it.hasNext()) {
        const auto m = it.next();
        const QByteArray text = m.captured(3).toUtf8();
        QByteArray bytes;
        if (m.captured(2).compare(QStringLiteral("B"), Qt::CaseInsensitive) == 0) {
            bytes = QByteArray::fromBase64(text);
        } else {
            QByteArray qp = text;
            qp.replace('_', ' ');
            bytes = qpDecode(qp);
        }
        // Prototype: UTF-8 primary, latin1 fallback.
        QString s = QString::fromUtf8(bytes);
        if (s.contains(QChar::ReplacementCharacter))
            s = QString::fromLatin1(bytes);
        spans.append({m.capturedStart(0), m.capturedEnd(0)});
        decoded.append(s);
    }
    for (int i = spans.size() - 1; i >= 0; --i)
        out.replace(spans[i].first, spans[i].second - spans[i].first, decoded[i]);
    return out;
}

inline QByteArray decodeBody(const QByteArray &body, const QString &cte)
{
    if (cte.compare(QStringLiteral("base64"), Qt::CaseInsensitive) == 0)
        return QByteArray::fromBase64(body);
    if (cte.compare(QStringLiteral("quoted-printable"), Qt::CaseInsensitive) == 0)
        return qpDecode(body);
    return body;
}

inline QString stripHtml(const QString &html)
{
    QString s = html;
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    s.replace(tagRe, QStringLiteral(" "));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"))
     .replace(QStringLiteral("&lt;"), QStringLiteral("<"))
     .replace(QStringLiteral("&gt;"), QStringLiteral(">"))
     .replace(QStringLiteral("&quot;"), QStringLiteral("\""))
     .replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    return s.simplified();
}

struct Part
{
    QHash<QString, QString> headers;
    QByteArray body;
};

inline Part splitMessage(const QByteArray &raw)
{
    Part p;
    int split = raw.indexOf("\r\n\r\n");
    int sepLen = 4;
    if (split < 0) {
        split = raw.indexOf("\n\n");
        sepLen = 2;
    }
    const QByteArray head = split < 0 ? raw : raw.left(split);
    p.body = split < 0 ? QByteArray() : raw.mid(split + sepLen);

    // Unfold + parse headers.
    const auto lines = head.split('\n');
    QString lastKey;
    for (const QByteArray &rawLine : lines) {
        QByteArray line = rawLine;
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) continue;
        if ((line.at(0) == ' ' || line.at(0) == '\t') && !lastKey.isEmpty()) {
            p.headers[lastKey] += QStringLiteral(" ") + QString::fromUtf8(line.trimmed());
            continue;
        }
        const int colon = line.indexOf(':');
        if (colon <= 0) continue;
        lastKey = QString::fromUtf8(line.left(colon)).toLower();
        p.headers[lastKey] = QString::fromUtf8(line.mid(colon + 1).trimmed());
    }
    return p;
}

inline QString headerParam(const QString &headerValue, const QString &param)
{
    // NOT static: the regex is built from `param` per call.
    const QRegularExpression re(QStringLiteral("%1=\"?([^\";]+)\"?").arg(param));
    const auto m = re.match(headerValue);
    return m.hasMatch() ? m.captured(1) : QString();
}

inline QString extractText(const QByteArray &raw, int depth = 0)
{
    if (depth > 4) return {};

    const Part p = splitMessage(raw);
    const QString ct = p.headers.value(QStringLiteral("content-type"),
                                       QStringLiteral("text/plain"));
    const QString cte = p.headers.value(QStringLiteral("content-transfer-encoding"));

    if (ct.startsWith(QStringLiteral("multipart/"), Qt::CaseInsensitive)) {
        const QString boundary = headerParam(ct, QStringLiteral("boundary"));
        if (boundary.isEmpty()) return {};

        const QByteArray delim = "--" + boundary.toUtf8();
        // Manual multi-byte split (QByteArray::split takes only char).
        QList<QByteArray> parts;
        qsizetype pos = 0;
        while (true) {
            const qsizetype idx = p.body.indexOf(delim, pos);
            if (idx < 0) {
                parts.append(p.body.mid(pos));
                break;
            }
            parts.append(p.body.mid(pos, idx - pos));
            pos = idx + delim.size();
        }
        QString htmlFallback;
        QString plainFallback;
        for (const QByteArray &partRaw : parts) {
            const QByteArray trimmed = partRaw.trimmed();
            if (trimmed.isEmpty() || trimmed == "--") continue;
            const Part sub = splitMessage(trimmed);
            const QString subCt = sub.headers.value(QStringLiteral("content-type"),
                                                    QStringLiteral("text/plain"));
            const QString subCte = sub.headers.value(
                QStringLiteral("content-transfer-encoding"));
            if (subCt.startsWith(QStringLiteral("multipart/"), Qt::CaseInsensitive)) {
                const QString nested = extractText(trimmed, depth + 1);
                if (!nested.isEmpty()) return nested;
            } else if (subCt.startsWith(QStringLiteral("text/plain"), Qt::CaseInsensitive)) {
                const QString plain = QString::fromUtf8(decodeBody(sub.body, subCte));
                // Marketing mail often ships a blank text/plain stub
                // beside the real HTML part; a blank stub must not win.
                if (!plain.trimmed().isEmpty())
                    return plain;
                if (plainFallback.isEmpty())
                    plainFallback = plain;
            } else if (subCt.startsWith(QStringLiteral("text/html"), Qt::CaseInsensitive)) {
                if (htmlFallback.isEmpty())
                    htmlFallback = stripHtml(QString::fromUtf8(decodeBody(sub.body, subCte)));
            }
        }
        if (!plainFallback.isEmpty()) return plainFallback;
        return htmlFallback;
    }

    if (ct.startsWith(QStringLiteral("text/html"), Qt::CaseInsensitive))
        return stripHtml(QString::fromUtf8(decodeBody(p.body, cte)));

    return QString::fromUtf8(decodeBody(p.body, cte));
}

/// \brief Collect all leaf (non-multipart) parts of a message,
///        descending into multipart containers. Used by the attachment
///        functions; extractText above has its own walking logic which
///        is intentionally left untouched.
inline void collectLeafParts(const QByteArray &raw, QList<Part> &out, int depth = 0)
{
    if (depth > 4) return;

    const Part p = splitMessage(raw);
    const QString ct = p.headers.value(QStringLiteral("content-type"),
                                       QStringLiteral("text/plain"));

    if (!ct.startsWith(QStringLiteral("multipart/"), Qt::CaseInsensitive)) {
        out.append(p);
        return;
    }

    const QString boundary = headerParam(ct, QStringLiteral("boundary"));
    if (boundary.isEmpty()) {
        out.append(p);
        return;
    }

    const QByteArray delim = "--" + boundary.toUtf8();
    qsizetype pos = 0;
    while (true) {
        const qsizetype idx = p.body.indexOf(delim, pos);
        if (idx < 0) {
            const QByteArray tail = p.body.mid(pos).trimmed();
            if (!tail.isEmpty() && tail != "--")
                collectLeafParts(tail, out, depth + 1);
            break;
        }
        const QByteArray piece = p.body.mid(pos, idx - pos).trimmed();
        if (!piece.isEmpty())
            collectLeafParts(piece, out, depth + 1);
        pos = idx + delim.size();
    }
}

inline bool isAttachmentPart(const Part &p)
{
    const QString ct = p.headers.value(QStringLiteral("content-type"),
                                       QStringLiteral("text/plain"));
    const QString disp = p.headers.value(QStringLiteral("content-disposition"))
                             .toLower();
    if (disp.startsWith(QStringLiteral("attachment")))
        return true;
    if (!headerParam(disp, QStringLiteral("filename")).isEmpty())
        return true;
    if (!headerParam(ct, QStringLiteral("name")).isEmpty())
        return true;
    return false;
}

inline AttachmentMeta attachmentMetaFromPart(const Part &p, int index)
{
    const QString ct = p.headers.value(QStringLiteral("content-type"),
                                       QStringLiteral("text/plain"));
    const QString disp = p.headers.value(QStringLiteral("content-disposition"));
    const QString cte = p.headers.value(QStringLiteral("content-transfer-encoding"));

    AttachmentMeta meta;
    meta.index = index;
    meta.filename = headerParam(disp, QStringLiteral("filename"));
    if (meta.filename.isEmpty())
        meta.filename = headerParam(ct, QStringLiteral("name"));
    meta.filename = decodeEncodedWords(meta.filename);
    meta.mimeType = ct.section(';', 0, 0).trimmed();
    meta.size = decodeBody(p.body, cte).size();
    return meta;
}

/// \brief List attachment parts (index-addressable) without decoding
///        bodies beyond what size reporting needs.
inline QVector<AttachmentMeta> listAttachments(const QByteArray &raw)
{
    QList<Part> leaves;
    collectLeafParts(raw, leaves);

    QVector<AttachmentMeta> out;
    for (const Part &p : leaves) {
        if (!isAttachmentPart(p)) continue;
        out.append(attachmentMetaFromPart(p, out.size()));
    }
    return out;
}

/// \brief Extract decoded bytes of attachment \p index (index into the
///        list returned by listAttachments). The .eml is immutable, so
///        indices are stable.
inline QByteArray extractAttachment(const QByteArray &raw, int index)
{
    QList<Part> leaves;
    collectLeafParts(raw, leaves);

    int found = 0;
    for (const Part &p : leaves) {
        if (!isAttachmentPart(p)) continue;
        if (found == index) {
            const QString cte = p.headers.value(
                QStringLiteral("content-transfer-encoding"));
            return decodeBody(p.body, cte);
        }
        ++found;
    }
    return {};
}

inline ParsedMessage parse(const QByteArray &raw)
{
    const Part p = splitMessage(raw);

    ParsedMessage out;
    out.messageId = p.headers.value(QStringLiteral("message-id"));
    // Strip angle brackets.
    if (out.messageId.startsWith(u'<') && out.messageId.endsWith(u'>'))
        out.messageId = out.messageId.mid(1, out.messageId.size() - 2);

    out.subject = decodeEncodedWords(p.headers.value(QStringLiteral("subject")));
    out.sender = decodeEncodedWords(p.headers.value(QStringLiteral("from")));

    const QString dateStr = p.headers.value(QStringLiteral("date"));
    out.date = QDateTime::fromString(dateStr, Qt::RFC2822Date);
    if (!out.date.isValid())
        out.date = QDateTime::currentDateTime();

    out.bodyPlain = extractText(raw);
    return out;
}

inline QString snippetFrom(const QString &bodyPlain)
{
    return bodyPlain.simplified().left(140);
}

} // namespace MimeParser

#endif
