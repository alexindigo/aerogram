#include "MessageListModel.h"

MessageListModel::MessageListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int MessageListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_messages.size();
}

QVariant MessageListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_messages.size())
        return {};

    const auto &msg = m_messages.at(index.row());
    switch (role) {
    case MessageIdRole: return msg.messageId;
    case SubjectRole:   return msg.subject;
    case SenderRole:    return msg.sender;
    case DateRole:      return msg.date;
    case SnippetRole:   return msg.snippet;
    case IsUnreadRole:  return msg.isUnread;
    case HasAttachmentsRole: return !msg.attachments.isEmpty();
    default:            return {};
    }
}

QHash<int, QByteArray> MessageListModel::roleNames() const
{
    return {
        { MessageIdRole, "messageId" },
        { SubjectRole,   "subject" },
        { SenderRole,    "sender" },
        { DateRole,      "date" },
        { SnippetRole,   "snippet" },
        { IsUnreadRole,  "isUnread" },
        { HasAttachmentsRole, "hasAttachments" },
    };
}

QString MessageListModel::messageIdAtRow(int row) const
{
    if (row < 0 || row >= m_messages.size())
        return {};
    return m_messages.at(row).messageId;
}

void MessageListModel::setMessages(const QVector<Message> &messages)
{
    beginResetModel();
    m_messages = messages;
    endResetModel();
}

void MessageListModel::appendMessage(const Message &message)
{
    // Push arrivals are newest; the list is time-ordered ascending, so
    // they land at the end.
    beginInsertRows(QModelIndex(), m_messages.size(), m_messages.size());
    m_messages.append(message);
    endInsertRows();
}

void MessageListModel::removeMessageById(const QString &messageId)
{
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages.at(i).messageId == messageId) {
            beginRemoveRows(QModelIndex(), i, i);
            m_messages.removeAt(i);
            endRemoveRows();
            return;
        }
    }
}
