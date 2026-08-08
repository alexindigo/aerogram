#ifndef CONVERSATIONLISTMODEL_H
#define CONVERSATIONLISTMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include "core/Types.h"

class ConversationListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        ConversationIdRole = Qt::UserRole + 1,
        KindRole,
        NameRole,
        PreviewRole,
        AccountLabelRole,
        UnreadCountRole,
        LastActivityRole
    };

    explicit ConversationListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString conversationIdAtRow(int row) const;

    void setConversations(const QVector<Conversation> &conversations);

private:
    QVector<Conversation> m_conversations;
};

#endif
