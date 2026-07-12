#ifndef ACCOUNTCONTROLLER_H
#define ACCOUNTCONTROLLER_H

#include <QObject>
#include <QString>
#include "models/MessageListModel.h"
#include "models/ChatListModel.h"

class AccountController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MessageListModel* messageListModel READ messageListModel CONSTANT)
    Q_PROPERTY(ChatListModel* chatListModel READ chatListModel CONSTANT)

public:
    explicit AccountController(QObject *parent = nullptr);

    MessageListModel *messageListModel() const;
    ChatListModel *chatListModel() const;

public slots:
    void loginProton(const QString &user, const QString &pass);
    void checkAutoconfig(const QString &email);
    void resetApp();
    void fetchMessageDetails(const QString &messageId);
    void openProtonChat(const QString &emailAddress);
    void triggerSync();

private:
    MessageListModel *m_messageModel;
    ChatListModel *m_chatModel;
};

#endif
