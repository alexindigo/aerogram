#include "controllers/AccountController.h"

AccountController::AccountController(QObject *parent)
    : QObject(parent)
    , m_messageModel(new MessageListModel(this))
    , m_chatModel(new ChatListModel(this))
{
}

MessageListModel *AccountController::messageListModel() const
{
    return m_messageModel;
}

ChatListModel *AccountController::chatListModel() const
{
    return m_chatModel;
}

void AccountController::loginProton(const QString &user, const QString &pass)
{
    Q_UNUSED(user);
    Q_UNUSED(pass);
}

void AccountController::checkAutoconfig(const QString &email)
{
    Q_UNUSED(email);
}

void AccountController::resetApp()
{
}

void AccountController::fetchMessageDetails(const QString &messageId)
{
    Q_UNUSED(messageId);
}

void AccountController::openProtonChat(const QString &emailAddress)
{
    Q_UNUSED(emailAddress);
}

void AccountController::triggerSync()
{
}
