import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: sidebar
    width: 70
    color: Kirigami.Theme.backgroundColor

    signal inboxRequested()
    signal chatsRequested()
    signal settingsRequested()
    signal resetApplicationRequested()
    signal addAccountRequested()

    property string currentSection: "email"

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 20
        spacing: 15

        Button {
            text: "+"
            flat: true
            onClicked: sidebar.addAccountRequested()
        }

        Button {
            text: "\u2709"
            flat: true
            highlighted: sidebar.currentSection === "email"
            onClicked: sidebar.inboxRequested()
        }

        Button {
            text: "\uD83D\uDCAC"
            flat: true
            highlighted: sidebar.currentSection === "chats"
            onClicked: sidebar.chatsRequested()
        }

        Item { Layout.fillHeight: true }

        Button {
            text: "\u2699"
            flat: true
            highlighted: sidebar.currentSection === "settings"
            onClicked: sidebar.settingsRequested()
        }

        Button {
            text: "\u274C"
            flat: true
            onClicked: sidebar.resetApplicationRequested()
        }
    }
}
