#ifndef HTMLTEXTAPPENDER_H
#define HTMLTEXTAPPENDER_H

#include <QObject>
#include <QQmlProperty>
#include <QQuickTextDocument>
#include <QTextCursor>

/// \brief Append HTML to a QML TextEdit WITHOUT re-parsing the whole
///        document (QML's own insert() is plain-text only, and append()
///        forces a paragraph break — wrong for streaming fragments).
///        Goes through QTextCursor::insertHtml at the document end.
///
/// Registered as the `htmlTextAppender` context property in main.cpp.
/// Used by the progressive-render pane path.
class HtmlTextAppender : public QObject
{
    Q_OBJECT

public:
    explicit HtmlTextAppender(QObject *parent = nullptr) : QObject(parent) {}

    /// Append an HTML fragment at the end of `edit`'s document.
    /// `edit` is a QML TextEdit (reads its textDocument property).
    Q_INVOKABLE void appendHtml(QObject *edit, const QString &htmlFragment) const
    {
        auto *item = qobject_cast<QQuickItem *>(edit);
        if (!item)
            return;
        QQmlProperty docProp(item, QStringLiteral("textDocument"));
        auto *quickDoc = qvariant_cast<QQuickTextDocument *>(docProp.read());
        if (!quickDoc)
            return;
        QTextDocument *doc = quickDoc->textDocument();
        QTextCursor cursor(doc);
        cursor.movePosition(QTextCursor::End);
        cursor.insertHtml(htmlFragment);
    }
};

#endif
