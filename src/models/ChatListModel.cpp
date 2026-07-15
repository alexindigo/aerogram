#include "ChatListModel.h"

ChatListModel::ChatListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ChatListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_messages.size();
}

QVariant ChatListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_messages.size())
        return {};

    const auto &msg = m_messages.at(index.row());
    switch (role) {
    case ChatIdRole:      return msg.chatId;
    case SenderNameRole:  return msg.senderName;
    case MessageTextRole: return msg.messageText;
    case TimestampRole:   return msg.timestamp;
    case IsGroupRole:     return msg.isGroup;
    default:              return {};
    }
}

QHash<int, QByteArray> ChatListModel::roleNames() const
{
    return {
        { ChatIdRole,      "chatId" },
        { SenderNameRole,  "senderName" },
        { MessageTextRole, "messageText" },
        { TimestampRole,   "timestamp" },
        { IsGroupRole,     "isGroup" },
    };
}

QString ChatListModel::chatIdAtRow(int row) const
{
    if (row < 0 || row >= m_messages.size())
        return {};
    return m_messages.at(row).chatId;
}

void ChatListModel::setChatMessages(const QVector<ChatMessage> &messages)
{
    beginResetModel();
    m_messages = messages;
    endResetModel();
}
