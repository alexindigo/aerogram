#ifndef ACCOUNTLISTMODEL_H
#define ACCOUNTLISTMODEL_H

#include <QAbstractListModel>
#include <QString>
#include <QVector>

/// \brief One account row for the sidebar rail.
struct AccountEntry {
    QString accountId;   // e.g. "imap:alice@gmail.com"
    QString chipText;    // initials, e.g. "Al"
    QString color;       // deterministic per accountId
    QString type;        // "email" | "chat" (drives section headers)
};

class AccountListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        AccountIdRole = Qt::UserRole + 1,
        ChipTextRole,
        ColorRole,
        TypeRole
    };

    explicit AccountListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setAccounts(const QVector<AccountEntry> &accounts);

private:
    QVector<AccountEntry> m_accounts;
};

#endif
