#include "BackendPlugin.h"
#include "Capabilities.h"
#include "core/Types.h"

class MockBackend : public BackendPlugin,
                    public IConversationProvider,
                    public IMessageProvider,
                    public IMessageSender,
                    public IQrSetup
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

    // IQrSetup
    void setupFromQr(const QString &qrContent) override
    {
        Q_UNUSED(qrContent);
        emit configured(true);
    }

    void getBackupFromQr(const QString &qrText) override
    {
        Q_UNUSED(qrText);
        emit configured(true);
    }

    void startIo() override { emit ioStarted(true, QString()); }
    void stopIo() override { emit ioStopped(); }

    // IConversationProvider
    void fetchConversations() override
    {
        const QDateTime now = QDateTime::currentDateTime();
        QVector<Conversation> conversations = {
            { "chat-1", "chat",   "Diana",          "Did you see the new Kirigami components?", QString(), 0, now.addSecs(-1800)  },
            { "chat-2", "chat",   "Release Group",  "Meeting at 3pm tomorrow",                  QString(), 2, now.addSecs(-3600)  },
            { "chat-3", "chat",   "Eve",            "Lets grab lunch?",                         QString(), 0, now.addSecs(-14400) },
            { "chat-4", "folder", "INBOX",          "Weekly team standup notes",                QString(), 3, now.addSecs(-7200)  },
        };
        emit conversationsReady(conversations);
    }

    // IMessageProvider
    void fetchMessages(const QString &conversationId) override
    {
        const QDateTime now = QDateTime::currentDateTime();
        QVector<Message> msgs = {
            { "msg-1", conversationId, "Weekly team standup notes",  "Alice Chen",    now.addSecs(-3600),   "Hey team, here are the action items from today's standup...", QString(), true  },
            { "msg-2", conversationId, "Your invoice is ready",      "Billing Team",  now.addSecs(-7200),   "Please find attached the invoice for last month...",        QString(), true  },
            { "msg-3", conversationId, "Re: Project Delta proposal", "Bob Martinez",  now.addSecs(-86400),  "I think we should go ahead with the phased rollout...",     QString(), false },
            { "msg-4", conversationId, "Welcome to Aerogram",        "Aerogram Team", now.addSecs(-172800), "Thanks for signing up! Here's how to get started...",       QString(), false },
        };
        emit messagesReady(conversationId, msgs);
    }

    void fetchMessageBody(const QString &conversationId, const QString &messageId) override
    {
        emit messageBodyReady(conversationId, messageId,
                              QStringLiteral("Mock body for %1").arg(messageId));
    }

    void saveAttachment(const QString &messageId, int partIndex,
                        const QString &destinationPath) override
    {
        Q_UNUSED(partIndex);
        Q_UNUSED(destinationPath);
        emit attachmentSaved(false, messageId, QString());
    }

    // IMessageSender
    void sendMessage(const QString &conversationId, const QString &text) override
    {
        Q_UNUSED(text);
        emit messageSent(true, conversationId);
    }
};
