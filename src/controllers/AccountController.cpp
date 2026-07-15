#include "controllers/AccountController.h"

#include <QDebug>

AccountController::AccountController(BackendPlugin *plugin, QObject *parent)
    : QObject(parent)
    , m_plugin(plugin)
    , m_messageModel(new MessageListModel(this))
    , m_chatModel(new ChatListModel(this))
{
    connect(m_plugin, &BackendPlugin::chatListReady, this, &AccountController::onChatListReady);
    connect(m_plugin, &BackendPlugin::messagesReady, this, &AccountController::onMessagesReady);
    connect(m_plugin, &BackendPlugin::messageSent, this, &AccountController::onMessageSent);
    connect(m_plugin, &BackendPlugin::errorOccurred, this, [](const QString &err) {
        qWarning() << "Backend error:" << err;
    });
}

MessageListModel *AccountController::messageListModel() const
{
    return m_messageModel;
}

ChatListModel *AccountController::chatListModel() const
{
    return m_chatModel;
}

void AccountController::fetchChatList()
{
    m_plugin->fetchChatList();
}

void AccountController::fetchMessages(const QString &chatId)
{
    m_plugin->fetchMessages(chatId);
}

void AccountController::sendMessage(const QString &chatId, const QString &text)
{
    m_plugin->sendMessage(chatId, text);
}

void AccountController::configureAccount(const QString &email, const QString &password)
{
    m_plugin->configureAccount(email, password);
}

void AccountController::triggerSync()
{
    m_plugin->fetchChatList();
}

void AccountController::resetApp()
{
    m_plugin->shutdown();
    m_messageModel->setMessages({});
    m_chatModel->setChatMessages({});
    m_plugin->initialize();
    m_plugin->fetchChatList();
}

void AccountController::onChatListReady(const QVector<ChatMessage> &chats)
{
    m_chatModel->setChatMessages(chats);
}

void AccountController::onMessagesReady(const QString &chatId, const QVector<Message> &messages)
{
    Q_UNUSED(chatId);
    m_messageModel->setMessages(messages);
}

void AccountController::onMessageSent(bool ok, const QString &chatId)
{
    Q_UNUSED(ok);
    Q_UNUSED(chatId);
}
