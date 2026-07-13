#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "controllers/AccountController.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;

    AccountController controller;
    engine.rootContext()->setContextProperty("accountController", &controller);

    const QUrl url(QStringLiteral("qrc:/Aerogram/ui/main.qml"));
    engine.load(url);

    return app.exec();
}
