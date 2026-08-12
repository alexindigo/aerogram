#ifndef HTMLSANITIZER_H
#define HTMLSANITIZER_H

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

// C ABI of aerogram-html-sanitize (Rust staticlib) — the ONE email-HTML
// sanitizer, shared by every backend. No account code, no Proton
// coupling; backends must not call each other, they call THIS.
extern "C" {
char *sanitize_html(const char *input);
char *html_to_plain(const char *input);
void sanitize_free_string(char *s);
void *sanitize_stream_new();
char *sanitize_stream_write(void *s, const char *chunk);
char *sanitize_stream_finish(void *s);  // frees the stream
void sanitize_stream_free(void *s);
}

struct SanitizedBody {
    QString html;          // sanitized, safe for QTextDocument rich text
    QString plain;         // plain-text transform
    bool blockedRemote = false;  // remote/embedded content was stripped
};

/// \brief Sanitize an email HTML document for display. Synchronous and
///        fast (a few ms; ~100ms for newsletter-size); call from worker
///        threads for bulk work.
class HtmlSanitizer
{
public:
    static SanitizedBody sanitize(const QString &html)
    {
        char *raw = sanitize_html(html.toUtf8().constData());
        const QString json = QString::fromUtf8(raw ? raw : "");
        if (raw)
            sanitize_free_string(raw);
        const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
        SanitizedBody out;
        out.html = o.value(QStringLiteral("html")).toString();
        out.plain = o.value(QStringLiteral("plain")).toString();
        out.blockedRemote = o.value(QStringLiteral("blocked_remote")).toBool();
        return out;
    }

    static QString toPlainText(const QString &html)
    {
        char *raw = html_to_plain(html.toUtf8().constData());
        const QString json = QString::fromUtf8(raw ? raw : "");
        if (raw)
            sanitize_free_string(raw);
        return QJsonDocument::fromJson(json.toUtf8()).object()
            .value(QStringLiteral("plain")).toString();
    }
};

#endif
