#include "MessageListModel.h"

MessageListModel::MessageListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_messages = {
        { "msg-1", "Weekly team standup notes",  "Alice Chen",     QDateTime::currentDateTime().addSecs(-3600),   "Hey team, here are the action items from today's standup...", true },
        { "msg-2", "Your invoice is ready",      "Billing Team",   QDateTime::currentDateTime().addSecs(-7200),   "Please find attached the invoice for last month...",         true },
        { "msg-3", "Re: Project Delta proposal", "Bob Martinez",   QDateTime::currentDateTime().addSecs(-86400),  "I think we should go ahead with the phased rollout...",      false },
        { "msg-4", "Welcome to Atmogram",        "Atmogram Team",  QDateTime::currentDateTime().addSecs(-172800), "Thanks for signing up! Here's how to get started...",        false },
    };
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
    default:            return {};
    }
}

QString MessageListModel::messageIdAtRow(int row) const
{
    if (row < 0 || row >= m_messages.size())
        return {};
    return m_messages.at(row).messageId;
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
    };
}
