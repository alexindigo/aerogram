#ifndef TYPES_H
#define TYPES_H

#include <QString>
#include <QDateTime>

struct Message {
    QString messageId;
    QString subject;
    QString sender;
    QDateTime date;
    QString snippet;
    bool isUnread = false;
};

struct ChatMessage {
    QString chatId;
    QString senderName;
    QString messageText;
    QDateTime timestamp;
    bool isGroup = false;
};

#endif
