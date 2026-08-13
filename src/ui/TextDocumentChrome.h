#ifndef TEXTDOCUMENTCHROME_H
#define TEXTDOCUMENTCHROME_H

#include <QFont>
#include <QObject>
#include <QQmlProperty>
#include <QQuickTextDocument>
#include <QTextDocument>

/// \brief Apply Aerogram's reading chrome to a QML TextEdit's document:
///        margins, UI font, and a theme-aware default stylesheet
///        (paragraph spacing, heading hierarchy, themed links, quotes,
///        mono code). Rich body views (Reader / HTML) get this once;
///        Text and Raw modes are plain and style themselves in QML.
///
/// Registered as the `textDocumentChrome` context property in main.cpp.
class TextDocumentChrome : public QObject
{
    Q_OBJECT

public:
    explicit TextDocumentChrome(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE void applyReaderChrome(QObject *edit, const QString &linkColor,
                                       const QString &quoteColor) const
    {
        auto *item = qobject_cast<QQuickItem *>(edit);
        if (!item)
            return;
        QQmlProperty docProp(item, QStringLiteral("textDocument"));
        auto *quickDoc = qvariant_cast<QQuickTextDocument *>(docProp.read());
        if (!quickDoc)
            return;
        QTextDocument *doc = quickDoc->textDocument();
        doc->setDocumentMargin(4);   // the pane already pads the edges
        const QVariant f = item->property("font");
        if (f.isValid())
            doc->setDefaultFont(f.value<QFont>());
        doc->setDefaultStyleSheet(QStringLiteral(
            "a { color: %1; }"
            "p { margin-top: 2px; margin-bottom: 10px; }"
            "ul, ol { margin-top: 2px; margin-bottom: 10px; }"
            "li { margin-bottom: 2px; }"
            "blockquote { margin-left: 20px; margin-right: 8px; color: %2; }"
            "pre, code { font-family: monospace; }"
            "pre { margin: 8px 0; }"
            "h1 { font-size: x-large; margin: 10px 0 6px 0; }"
            "h2 { font-size: large; margin: 10px 0 6px 0; }"
            "h3 { font-weight: bold; margin: 8px 0 4px 0; }"
            "hr { margin: 8px 0; }"
            ).arg(linkColor, quoteColor));
    }
};

#endif
