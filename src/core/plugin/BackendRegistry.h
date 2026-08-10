#ifndef BACKENDREGISTRY_H
#define BACKENDREGISTRY_H

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class BackendPlugin;

/// \brief One credential field a backend needs from the user
///        (e.g. IMAP host, Delta Chat QR invite). `kind` picks the
///        editor: "text" | "password" | "int" | "bool".
struct BackendField {
    QString key;
    QString label;
    QString placeholder;
    QString kind = QStringLiteral("text");
    bool required = false;
};

/// \brief User-facing description of a backend type: shown in the
///        add-account picker, and `fields` drives the dynamic form.
///        Types registered without info stay invisible in the picker
///        (dev backends like mock).
struct BackendInfo {
    QString type;
    QString displayName;
    QString family;      // "email" | "chat"
    QString description;
    QList<BackendField> fields;
};

/// \brief Type → factory (+ optional UI schema) registry. main.cpp
///        registers the built-in backends once; the controller creates
///        instances through create() and never names a concrete class.
///
///        Adding a backend (e.g. Proton): one registerType() call with
///        its factory and field schema — zero controller edits.
class BackendRegistry
{
public:
    using Factory = std::function<BackendPlugin *(const QVariantMap &credentials)>;

    static void registerType(const QString &type, Factory factory)
    {
        factories().insert(type, std::move(factory));
    }

    static void registerType(const QString &type, Factory factory, BackendInfo info)
    {
        factories().insert(type, std::move(factory));
        info.type = type;
        infos().insert(type, std::move(info));
    }

    static BackendPlugin *create(const QString &type, const QVariantMap &credentials)
    {
        const auto it = factories().constFind(type);
        return it == factories().constEnd() ? nullptr : it.value()(credentials);
    }

    static QStringList types()
    {
        return factories().keys();
    }

    /// Schemas of picker-visible backends, in registration order.
    static QList<BackendInfo> backendInfos()
    {
        return infos().values();
    }

private:
    static QMap<QString, Factory> &factories()
    {
        static QMap<QString, Factory> f;
        return f;
    }

    static QMap<QString, BackendInfo> &infos()
    {
        static QMap<QString, BackendInfo> i;
        return i;
    }
};

#endif
