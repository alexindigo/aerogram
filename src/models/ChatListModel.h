#ifndef CHATLISTMODEL_H
#define CHATLISTMODEL_H

#include <QAbstractListModel>
#include <QDateTime>
#include <QString>
#include <QVector>

struct ChatMessage {
    QString chatId;
    QString senderName;
    QString messageText;
    QDateTime timestamp;
    bool isGroup;
};

class ChatListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        ChatIdRole = Qt::UserRole + 1,
        SenderNameRole,
        MessageTextRole,
        TimestampRole,
        IsGroupRole
    };

    explicit ChatListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString chatIdAtRow(int row) const;

private:
    QVector<ChatMessage> m_messages;
};

#endif
