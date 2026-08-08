#include "ConversationListModel.h"

ConversationListModel::ConversationListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ConversationListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_conversations.size();
}

QVariant ConversationListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_conversations.size())
        return {};

    const auto &c = m_conversations.at(index.row());
    switch (role) {
    case ConversationIdRole: return c.id;
    case KindRole:           return c.kind;
    case NameRole:           return c.name;
    case PreviewRole:        return c.preview;
    case AccountLabelRole:   return c.accountLabel;
    case UnreadCountRole:    return c.unreadCount;
    case LastActivityRole:   return c.lastActivity;
    default:                 return {};
    }
}

QHash<int, QByteArray> ConversationListModel::roleNames() const
{
    return {
        { ConversationIdRole, "conversationId" },
        { KindRole,           "kind" },
        { NameRole,           "name" },
        { PreviewRole,        "preview" },
        { AccountLabelRole,   "accountLabel" },
        { UnreadCountRole,    "unreadCount" },
        { LastActivityRole,   "lastActivity" },
    };
}

QString ConversationListModel::conversationIdAtRow(int row) const
{
    if (row < 0 || row >= m_conversations.size())
        return {};
    return m_conversations.at(row).id;
}

void ConversationListModel::setConversations(const QVector<Conversation> &conversations)
{
    beginResetModel();
    m_conversations = conversations;
    endResetModel();
}
