#ifndef BACKENDREGISTRY_H
#define BACKENDREGISTRY_H

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class BackendPlugin;

/// \brief Type → factory registry. main.cpp registers the built-in
///        backends once; the controller creates instances through
///        create() and never names a concrete backend class.
///
///        Adding a backend (e.g. Proton): one registerType() call with
///        its factory — zero controller edits.
class BackendRegistry
{
public:
    using Factory = std::function<BackendPlugin *(const QVariantMap &credentials)>;

    static void registerType(const QString &type, Factory factory)
    {
        factories().insert(type, std::move(factory));
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

private:
    static QMap<QString, Factory> &factories()
    {
        static QMap<QString, Factory> f;
        return f;
    }
};

#endif
