#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/Types.h"
#include "core/crypto/MasterKeyManager.h"
#include "core/plugin/BackendRegistry.h"
#include "core/plugin/Capabilities.h"
#include "core/plugin/DeltaChatBackend.h"
#include "core/plugin/MockBackend.h"
#include "core/imap/ImapBackend.h"
#include "controllers/AccountController.h"
#include "core/ipc/IpcServer.h"

/// \brief Extract the bundled default icon pack (Tabler, MIT) into the
///        user's data dir on first run. The UI reads icons from disk so
///        the pack is user-replaceable; later phases add user packs and
///        a switcher.
static QString installIconPack()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                      + QStringLiteral("/icons/default");
    QDir().mkpath(dir);
    const QStringList names = {
        QStringLiteral("plus"), QStringLiteral("settings"),
        QStringLiteral("lock"), QStringLiteral("paperclip"),
    };
    for (const QString &n : names) {
        const QString dst = dir + QLatin1Char('/') + n + QStringLiteral(".svg");
        if (QFile::exists(dst))
            continue;
        QFile in(QStringLiteral(":/icons/tabler/") + n + QStringLiteral(".svg"));
        if (!in.open(QIODevice::ReadOnly))
            continue;
        QFile out(dst);
        if (out.open(QIODevice::WriteOnly))
            out.write(in.readAll());
    }
    return dir;
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Aerogram"));

    // Queued cross-thread signal delivery (IMAP sync workers) needs
    // these registered.
    qRegisterMetaType<Conversation>("Conversation");
    qRegisterMetaType<QVector<Conversation>>("QVector<Conversation>");
    qRegisterMetaType<AttachmentMeta>("AttachmentMeta");
    qRegisterMetaType<QVector<AttachmentMeta>>("QVector<AttachmentMeta>");
    qRegisterMetaType<Message>("Message");
    qRegisterMetaType<QVector<Message>>("QVector<Message>");

    // Backend class registry: type → factory. The controller creates
    // instances through this and never names a concrete class. Adding a
    // backend (e.g. Proton) = one registerType call here.
    // Factories construct + initialize only; configure() emits
    // configured(), so it must run after signal wiring — the controller
    // calls it inside registerAccount (after connectBackend).
    BackendRegistry::registerType(QStringLiteral("imap"),
        [](const QVariantMap &) -> BackendPlugin * {
            auto *backend = new ImapBackend();
            backend->initialize({});
            return backend;
        });
    BackendRegistry::registerType(QStringLiteral("mock"),
        [](const QVariantMap &) -> BackendPlugin * {
            auto *backend = new MockBackend();
            backend->initialize({});
            return backend;
        });
    BackendRegistry::registerType(QStringLiteral("deltachat"),
        [](const QVariantMap &credentials) -> BackendPlugin * {
            auto *backend = new DeltaChatBackend();
            // Credentials may carry "qr" (dcaccount:/backup invite) and
            // "accounts_path" overrides.
            backend->initialize(credentials);
            return backend;
        });

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Aerogram"));
    parser.addHelpOption();
    const QCommandLineOption backendOpt(
        QStringLiteral("backend"),
        QStringLiteral("Explicitly start a backend: deltachat | imap | mock. "
                       "With no flags and no saved accounts, the app boots "
                       "empty into the first-run vault flow."),
        QStringLiteral("name"));
    const QCommandLineOption accountsOpt(
        QStringLiteral("accounts"),
        QStringLiteral("JSON file with an array of accounts "
                       "([{\"type\":\"imap\",\"host\":...,\"port\":993,\"user\":...,\"pass\":...,\"tls\":true}])"),
        QStringLiteral("path"));
    const QCommandLineOption imapHostOpt(QStringLiteral("imap-host"),
        QStringLiteral("IMAP server host"), QStringLiteral("host"),
        QStringLiteral("localhost"));
    const QCommandLineOption imapPortOpt(QStringLiteral("imap-port"),
        QStringLiteral("IMAP server port"), QStringLiteral("port"),
        QStringLiteral("1143"));
    const QCommandLineOption imapUserOpt(QStringLiteral("imap-user"),
        QStringLiteral("IMAP username"), QStringLiteral("user"));
    const QCommandLineOption imapPassOpt(QStringLiteral("imap-pass"),
        QStringLiteral("IMAP password"), QStringLiteral("pass"));
    const QCommandLineOption imapTlsOpt(QStringLiteral("imap-tls"),
        QStringLiteral("Use IMAPS (TLS)"));
    parser.addOptions({backendOpt, accountsOpt, imapHostOpt, imapPortOpt,
                       imapUserOpt, imapPassOpt, imapTlsOpt});
    parser.process(app);

    // Account specs are (type, credentials) pairs; the controller
    // constructs backends from them via the registry.
    QList<QPair<QString, QVariantMap>> accountSpecs;

    if (parser.isSet(accountsOpt)) {
        QFile f(parser.value(accountsOpt));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            for (const QJsonValue &v : doc.array()) {
                // Generic pass-through: credential shapes are per
                // backend type (imap: host/user/pass; deltachat: qr).
                // main.cpp must not know them — only imap defaults are
                // applied for backward compatibility.
                QVariantMap creds = v.toObject().toVariantMap();
                if (creds.value(QStringLiteral("type")).toString() == QLatin1String("imap")) {
                    if (!creds.contains(QStringLiteral("port")))
                        creds[QStringLiteral("port")] = 993;
                    if (!creds.contains(QStringLiteral("tls")))
                        creds[QStringLiteral("tls")] = true;
                }
                accountSpecs.append({creds.value(QStringLiteral("type")).toString(), creds});
            }
        } else {
            qWarning().noquote() << "Cannot open accounts file:" << parser.value(accountsOpt);
        }
    }

    if (parser.isSet(backendOpt)) {
        const QString b = parser.value(backendOpt);
        if (b == QLatin1String("imap")) {
            QVariantMap creds;
            creds[QStringLiteral("host")] = parser.value(imapHostOpt);
            creds[QStringLiteral("port")] = parser.value(imapPortOpt).toInt();
            creds[QStringLiteral("user")] = parser.value(imapUserOpt);
            creds[QStringLiteral("pass")] = parser.value(imapPassOpt);
            creds[QStringLiteral("tls")] = parser.isSet(imapTlsOpt);
            accountSpecs.append({QStringLiteral("imap"), creds});
        } else if (b == QLatin1String("mock") || b == QLatin1String("deltachat")) {
            accountSpecs.append({b, {}});
        } else {
            qWarning().noquote() << "Unknown backend:" << b;
        }
    }

    // The vault is always constructed and wired: plain launches with no
    // accounts boot into the first-run vault-creation form, and adding
    // an encrypted account at runtime engages it. Whether the overlay
    // shows is the controller's showLockOverlay property.
    MasterKeyManager vault(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/vault"));

    AccountController controller(accountSpecs, &vault);
    controller.setIconPackDir(installIconPack());

    IpcServer ipc(&controller);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("accountController", &controller);

    const QUrl url(QStringLiteral("qrc:/Aerogram/ui/main.qml"));
    engine.load(url);
    if (engine.rootObjects().isEmpty())
        qFatal("QML root failed to load");

    // When the lock overlay shows, unlock/create drives startup via the
    // controller. Otherwise fetch immediately.
    if (!controller.showLockOverlay()) {
        controller.fetchConversations();
        controller.ensureActiveAccount();
    }

    return app.exec();
}
