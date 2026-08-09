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
#include "core/plugin/Capabilities.h"
#include "core/plugin/DeltaChatBackend.h"
#include "core/plugin/MockBackend.h"
#include "core/imap/ImapBackend.h"
#include "controllers/AccountController.h"
#include "core/ipc/IpcServer.h"

static BackendPlugin *makeImapBackend(const QVariantMap &creds)
{
    auto *imap = new ImapBackend();
    imap->initialize({});
    imap->configure(creds);
    return imap;
}

/// \brief Append IMAP accounts from a JSON file. Duplicates (same
///        imap:user@host) are skipped so the default config file and
///        CLI flags can coexist.
static void loadAccountsFile(const QString &path,
                             QList<QPair<QString, BackendPlugin *>> &backends)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("type")).toString() != QLatin1String("imap")) {
            qWarning().noquote() << "Unknown account type in" << path << ":"
                                 << o.value(QStringLiteral("type")).toString();
            continue;
        }
        QVariantMap creds;
        creds[QStringLiteral("host")] = o.value(QStringLiteral("host")).toString();
        creds[QStringLiteral("port")] = o.value(QStringLiteral("port")).toInt(993);
        creds[QStringLiteral("user")] = o.value(QStringLiteral("user")).toString();
        creds[QStringLiteral("pass")] = o.value(QStringLiteral("pass")).toString();
        creds[QStringLiteral("tls")] = o.value(QStringLiteral("tls")).toBool(true);
        const QString accountId = QStringLiteral("imap:")
                                + creds[QStringLiteral("user")].toString()
                                + QStringLiteral("@")
                                + creds[QStringLiteral("host")].toString();

        bool dup = false;
        for (const auto &pair : backends) {
            if (pair.first == accountId) { dup = true; break; }
        }
        if (!dup)
            backends.append({accountId, makeImapBackend(creds)});
    }
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

    QList<QPair<QString, BackendPlugin *>> backends;

    if (parser.isSet(accountsOpt))
        loadAccountsFile(parser.value(accountsOpt), backends);

    if (parser.isSet(backendOpt)) {
        const QString b = parser.value(backendOpt);
        if (b == QLatin1String("imap")) {
            QVariantMap creds;
            creds[QStringLiteral("host")] = parser.value(imapHostOpt);
            creds[QStringLiteral("port")] = parser.value(imapPortOpt).toInt();
            creds[QStringLiteral("user")] = parser.value(imapUserOpt);
            creds[QStringLiteral("pass")] = parser.value(imapPassOpt);
            creds[QStringLiteral("tls")] = parser.isSet(imapTlsOpt);
            const QString accountId = QStringLiteral("imap:")
                                    + creds[QStringLiteral("user")].toString()
                                    + QStringLiteral("@")
                                    + creds[QStringLiteral("host")].toString();
            backends.append({accountId, makeImapBackend(creds)});
        } else if (b == QLatin1String("mock")) {
            auto *mock = new MockBackend();
            mock->initialize({});
            backends.append({QStringLiteral("mock"), mock});
        } else if (b == QLatin1String("deltachat")) {
            auto *dc = new DeltaChatBackend();
            dc->initialize({});
            backends.append({QStringLiteral("deltachat"), dc});
        } else {
            qWarning().noquote() << "Unknown backend:" << b;
        }
    }

    // Dialog-added accounts persist in the encrypted vault DB and load
    // post-unlock (controller-driven). No plaintext accounts file is
    // read at boot; a legacy accounts.json is migrated on first unlock.

    // The vault is always constructed and wired: plain launches with no
    // accounts boot into the first-run vault-creation form, and adding
    // an encrypted account at runtime engages it. Whether the overlay
    // shows is the controller's showLockOverlay property.
    MasterKeyManager vault(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/vault"));

    AccountController controller(backends, &vault);

    IpcServer ipc(&controller);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("accountController", &controller);

    const QUrl url(QStringLiteral("qrc:/Aerogram/ui/main.qml"));
    engine.load(url);
    if (engine.rootObjects().isEmpty())
        qFatal("QML root failed to load");

    // When the lock overlay shows, unlock/create drives startup via the
    // controller. Otherwise fetch immediately.
    if (!controller.showLockOverlay())
        controller.fetchConversations();

    return app.exec();
}
