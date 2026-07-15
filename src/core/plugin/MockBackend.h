#include "BackendPlugin.h"
#include "core/Types.h"

#include <QTimer>

class MockBackend : public BackendPlugin
{
    Q_OBJECT

public:
    explicit MockBackend(QObject *parent = nullptr)
        : BackendPlugin(parent) {}

    QString name() const override { return QStringLiteral("mock"); }

    bool initialize(const QVariantMap &params) override
    {
        Q_UNUSED(params);
        return true;
    }

    void shutdown() override {}

    void configureAccount(const QString &email, const QString &password) override
    {
        Q_UNUSED(email);
        Q_UNUSED(password);
        emit configured(true);
    }

    void startIo() override {}
    void stopIo() override {}

    void fetchChatList() override
    {
        QVector<ChatMessage> chats = {
            { "chat-1", "Diana",   "Did you see the new Kirigami components?", QDateTime::currentDateTime().addSecs(-1800),  false },
            { "chat-2", "Group",   "Meeting at 3pm tomorrow",                  QDateTime::currentDateTime().addSecs(-3600),  true  },
            { "chat-3", "Eve",     "Lets grab lunch?",                         QDateTime::currentDateTime().addSecs(-14400), false },
            { "chat-4", "Group",   "PR is ready for review",                   QDateTime::currentDateTime().addSecs(-43200), true  },
        };
        emit chatListReady(chats);
    }

    void fetchMessages(const QString &chatId) override
    {
        Q_UNUSED(chatId);
        QVector<Message> msgs = {
            { "msg-1", "Weekly team standup notes",  "Alice Chen",     QDateTime::currentDateTime().addSecs(-3600),   "Hey team, here are the action items from today's standup...", true },
            { "msg-2", "Your invoice is ready",      "Billing Team",   QDateTime::currentDateTime().addSecs(-7200),   "Please find attached the invoice for last month...",         true },
            { "msg-3", "Re: Project Delta proposal", "Bob Martinez",   QDateTime::currentDateTime().addSecs(-86400),  "I think we should go ahead with the phased rollout...",      false },
            { "msg-4", "Welcome to Aerogram",        "Aerogram Team",  QDateTime::currentDateTime().addSecs(-172800), "Thanks for signing up! Here's how to get started...",        false },
        };
        emit messagesReady(chatId, msgs);
    }

    void sendMessage(const QString &chatId, const QString &text) override
    {
        Q_UNUSED(chatId);
        Q_UNUSED(text);
        emit messageSent(true, chatId);
    }
};
