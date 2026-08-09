#ifndef ACCOUNT_H
#define ACCOUNT_H

#include <QString>
#include <QVariantMap>

class BackendPlugin;

/// \brief A first-class account: the sidebar identity plus its backend
///        instance held through the interface (BackendPlugin*), never
///        the concrete class. See plans/backend-untangle/plan.md.
struct Account {
    QString id;             // alice@gmail.com#imap
    QString type;           // "imap" (registry type string)
    QString label;          // alice@gmail.com (display)
    QString userpic;        // local file / provider URL, may be empty
    QString color;          // #hex, derived deterministically from id at
                            // registration (persisted color comes later)
    int index = 0;          // rail order (reorderable later)
    QVariantMap credentials;  // backend-shaped blob (contains pass etc.)
    BackendPlugin *backend = nullptr;
};

#endif
