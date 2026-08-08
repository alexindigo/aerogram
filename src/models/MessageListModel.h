#ifndef MESSAGELISTMODEL_H
#define MESSAGELISTMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include "core/Types.h"

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
        IsUnreadRole,
        HasAttachmentsRole
    };

    explicit MessageListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString messageIdAtRow(int row) const;

    void setMessages(const QVector<Message> &messages);

private:
    QVector<Message> m_messages;
};

#endif
