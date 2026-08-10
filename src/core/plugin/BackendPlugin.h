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

    /// \brief UI grouping family reported by the backend ("email" or
    ///        "chat"). The sidebar rail groups accounts by this; no
    ///        prefix-sniffing of account ids anywhere.
    virtual QString family() const { return QStringLiteral("chat"); }

    virtual bool initialize(const QVariantMap &params = {}) = 0;
    virtual void shutdown() = 0;
    virtual void startIo() = 0;
    virtual void stopIo() = 0;

    /// \brief Delete the backend's on-disk data for this account
    ///        (cached mail, indexes, session stores). Called by the
    ///        controller on account REMOVAL, after shutdown(). Default
    ///        no-op for backends with no local store (mock).
    virtual void purgeLocalData() {}

    /// \brief The account's backend-reported identity (the real
    ///        address once the backend knows it — Delta Chat learns it
    ///        only after configure; Proton echoes the login). Empty
    ///        until known; the controller prefers this over
    ///        credential-derived labels so two accounts of the same
    ///        type never collide.
    virtual QString accountLabel() const { return {}; }

signals:
    // Lifecycle
    void configured(bool success);
    void errorOccurred(const QString &error);
    void ioStarted(bool ok, const QString &error);
    void ioStopped();

    /// \brief Setup progress narration ("Authenticating…", "Syncing
    ///        folders…") while a first-time account setup is in flight.
    ///        Human-readable, current-step-only; the controller shows
    ///        it verbatim in the add-account panel.
    void setupProgress(const QString &stage);

    // Content (emitted by capability implementations)
    void conversationsReady(const QVector<Conversation> &conversations);
    void messagesReady(const QString &conversationId, const QVector<Message> &messages);
    void messageSent(bool ok, const QString &conversationId);
    void messageBodyReady(const QString &conversationId, const QString &messageId,
                          const QString &body);
    void attachmentSaved(bool ok, const QString &messageId, const QString &path);

    // -----------------------------------------------------------------
    // Push events (unsolicited — the backend's store changed outside
    // any request). Payload-carrying where the backend knows the data
    // cheaply; storageChanged() is the coarse fallback for bursts and
    // ambiguous changes. All ids are backend-local; the controller
    // compounds them on the way in.
    // -----------------------------------------------------------------

    /// A single new message landed (known id and content).
    void messageArrived(const QString &conversationId, const Message &message);
    /// A conversation's row data changed (preview, unread, activity).
    void conversationUpserted(const Conversation &conversation);
    /// A message vanished (deleted/expired).
    void messageRemoved(const QString &conversationId, const QString &messageId);
    /// Store changed in some bulk/unspecified way — controller refetches.
    void storageChanged();
};

#endif
