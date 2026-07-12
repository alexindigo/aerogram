#ifndef MESSAGELISTMODEL_H
#define MESSAGELISTMODEL_H

#include <QAbstractListModel>
#include <QDateTime>
#include <QString>
#include <QVector>

struct Message {
    QString messageId;
    QString subject;
    QString sender;
    QDateTime date;
    QString snippet;
    bool isUnread;
};

class MessageListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        MessageIdRole = Qt::UserRole + 1,
        SubjectRole,
        SenderRole,
        DateRole,
        SnippetRole,
        IsUnreadRole
    };

    explicit MessageListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString messageIdAtRow(int row) const;

private:
    QVector<Message> m_messages;
};

#endif
