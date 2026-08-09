#include "AccountListModel.h"

AccountListModel::AccountListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int AccountListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_accounts.size();
}

QVariant AccountListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_accounts.size())
        return {};

    const auto &a = m_accounts.at(index.row());
    switch (role) {
    case AccountIdRole: return a.accountId;
    case ChipTextRole:  return a.chipText;
    case ColorRole:     return a.color;
    case TypeRole:      return a.type;
    default:            return {};
    }
}

QHash<int, QByteArray> AccountListModel::roleNames() const
{
    return {
        { AccountIdRole, "accountId" },
        { ChipTextRole,  "chipText" },
        { ColorRole,     "color" },
        { TypeRole,      "type" },
    };
}

void AccountListModel::setAccounts(const QVector<AccountEntry> &accounts)
{
    beginResetModel();
    m_accounts = accounts;
    endResetModel();
}
