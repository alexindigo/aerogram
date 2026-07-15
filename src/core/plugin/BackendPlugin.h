#ifndef BACKENDPLUGIN_H
#define BACKENDPLUGIN_H

#include <QObject>
#include <QVariantMap>
#include <QVector>
#include "core/Types.h"

class BackendPlugin : public QObject
{
    Q_OBJECT

public:
    explicit BackendPlugin(QObject *parent = nullptr) : QObject(parent) {}
    ~BackendPlugin() override = default;

    virtual QString name() const = 0;

    virtual bool initialize(const QVariantMap &params = {}) = 0;
    virtual void shutdown() = 0;

    virtual void configureAccount(const QString &email, const QString &password) = 0;
    virtual void startIo() = 0;
    virtual void stopIo() = 0;

    virtual void fetchChatList() = 0;
    virtual void fetchMessages(const QString &chatId) = 0;
    virtual void sendMessage(const QString &chatId, const QString &text) = 0;

signals:
    void chatListReady(const QVector<ChatMessage> &chats);
    void messagesReady(const QString &chatId, const QVector<Message> &messages);
    void messageSent(bool ok, const QString &chatId);
    void configured(bool success);
    void errorOccurred(const QString &error);
};

#endif
