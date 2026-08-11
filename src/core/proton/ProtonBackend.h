#ifndef PROTONBACKEND_H
#define PROTONBACKEND_H

#include "../plugin/BackendPlugin.h"
#include "../plugin/Capabilities.h"

#include <QFuture>
#include <QJsonObject>

// C ABI of aerogram-proton-core (Rust staticlib). All payloads are
// JSON strings; results are {"ok": …} / {"err": …}.
struct ProtonCore;
extern "C" {
ProtonCore *proton_core_new(const char *dataDir);
void proton_core_free(ProtonCore *core);
char *proton_call(ProtonCore *core, const char *method, const char *paramsJson);
void proton_free_string(char *s);
}

/// \brief Proton Mail backend — direct API, no Bridge. All Proton
///        protocol/auth/crypto work lives in the aerogram-proton-core
///        Rust staticlib (Proton's own mail stack); this class is a
///        thin async adapter: every FFI call runs on a QtConcurrent
///        worker (never the UI thread) and results arrive as typed
///        signals.
///
/// Credentials: {"user": <proton address>, "pass": <password>,
///               "totp": <optional 2FA code>}
class ProtonBackend : public BackendPlugin,
                      public IConversationProvider,
                      public IMessageProvider,
                      public ICredentialsSetup
{
    Q_OBJECT

public:
    explicit ProtonBackend(QObject *parent = nullptr);
    ~ProtonBackend() override;

    QString name() const override { return QStringLiteral("proton"); }
    QString family() const override { return QStringLiteral("email"); }

    bool initialize(const QVariantMap &params = {}) override;
    void shutdown() override;
    void startIo() override;
    void stopIo() override;

    // ICredentialsSetup
    void configure(const QVariantMap &credentials) override;

    // IConversationProvider
    void fetchConversations() override;

    // IMessageProvider
    void fetchMessages(const QString &conversationId) override;
    void fetchMessageBody(const QString &conversationId,
                          const QString &messageId) override;
    void saveAttachment(const QString &messageId, int partIndex,
                        const QString &destinationPath) override;

private:
    /// Run one core call on a worker thread; the promise fulfils with
    /// the "ok" payload or fails with the "err" message. Returns the
    /// future — callers attach .then/.onFailed per Pattern A.
    QFuture<QJsonValue> call(const QString &method, const QJsonObject &params = {});

    ProtonCore *m_core = nullptr;  // owned; freed in shutdown()
    bool m_configured = false;     // login completed at least once
    bool m_ioRequested = false;    // controller asked for IO pre-login
    int m_labelRetries = 0;        // post-login label sync patience
    QVariantMap m_credentials;
    QString m_dataDir;
};

#endif
