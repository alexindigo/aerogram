#include "ChatListModel.h"

ChatListModel::ChatListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_messages = {
        { "chat-1", "Diana",   "Did you see the new Kirigami components?", QDateTime::currentDateTime().addSecs(-1800),  false },
        { "chat-2", "Group",   "Meeting at 3pm tomorrow",                  QDateTime::currentDateTime().addSecs(-3600),  true  },
        { "chat-3", "Eve",     "Lets grab lunch?",                         QDateTime::currentDateTime().addSecs(-14400), false },
        { "chat-4", "Group",   "PR is ready for review",                   QDateTime::currentDateTime().addSecs(-43200), true  },
    };
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

QString ChatListModel::chatIdAtRow(int row) const
{
    if (row < 0 || row >= m_messages.size())
        return {};
    return m_messages.at(row).chatId;
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
