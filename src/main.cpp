#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "core/plugin/MockBackend.h"
#include "controllers/AccountController.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    auto *backend = new MockBackend();
    backend->initialize({});

    AccountController controller(backend);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("accountController", &controller);

    const QUrl url(QStringLiteral("qrc:/Aerogram/ui/main.qml"));
    engine.load(url);

    controller.fetchChatList();

    return app.exec();
}
