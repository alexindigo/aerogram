#ifndef BACKENDPLUGIN_H
#define BACKENDPLUGIN_H

#include <QObject>
#include <QVariantMap>
#include <QVector>
#include "core/Types.h"

/// \brief Lifecycle base for all backends.
///
/// The base owns only lifecycle methods; content operations live in
/// the capability interfaces in Capabilities.h. All signals live here
/// on the base so the controller can connect once against
/// BackendPlugin* regardless of which capabilities the concrete
/// backend implements. Signals a backend never emits simply never
/// fire.
class BackendPlugin : public QObject
{
    Q_OBJECT

public:
    explicit BackendPlugin(QObject *parent = nullptr) : QObject(parent) {}
    ~BackendPlugin() override = default;

    virtual QString name() const = 0;

    virtual bool initialize(const QVariantMap &params = {}) = 0;
    virtual void shutdown() = 0;
    virtual void startIo() = 0;
    virtual void stopIo() = 0;

signals:
    // Lifecycle
    void configured(bool success);
    void errorOccurred(const QString &error);
    void ioStarted(bool ok, const QString &error);
    void ioStopped();

    // Content (emitted by capability implementations)
    void conversationsReady(const QVector<Conversation> &conversations);
    void messagesReady(const QString &conversationId, const QVector<Message> &messages);
    void messageSent(bool ok, const QString &conversationId);
    void messageBodyReady(const QString &conversationId, const QString &messageId,
                          const QString &body);
    void attachmentSaved(bool ok, const QString &messageId, const QString &path);
};

#endif
