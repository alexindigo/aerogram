#ifndef ACCOUNTCONTROLLER_H
#define ACCOUNTCONTROLLER_H

#include <QObject>
#include <QString>
#include "models/MessageListModel.h"
#include "models/ChatListModel.h"
#include "core/plugin/BackendPlugin.h"

class AccountController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MessageListModel* messageListModel READ messageListModel CONSTANT)
    Q_PROPERTY(ChatListModel* chatListModel READ chatListModel CONSTANT)

public:
    explicit AccountController(BackendPlugin *plugin, QObject *parent = nullptr);

    MessageListModel *messageListModel() const;
    ChatListModel *chatListModel() const;

public slots:
    void fetchChatList();
    void fetchMessages(const QString &chatId);
    void sendMessage(const QString &chatId, const QString &text);
    void configureAccount(const QString &email, const QString &password);
    void triggerSync();
    void resetApp();

private slots:
    void onChatListReady(const QVector<ChatMessage> &chats);
    void onMessagesReady(const QString &chatId, const QVector<Message> &messages);
    void onMessageSent(bool ok, const QString &chatId);

private:
    BackendPlugin *m_plugin;
    MessageListModel *m_messageModel;
    ChatListModel *m_chatModel;
};

#endif
