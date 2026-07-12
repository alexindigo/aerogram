import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: inboxView
    title: "Inbox"

    signal messageDetailsRequested(string messageId)
    signal chatThreadRequested(string emailAddress)
    signal accountSyncRequested()

    ListView {
        id: messageList
        anchors.fill: parent
        model: accountController.messageListModel
        spacing: 8
        clip: true

        delegate: Kirigami.AbstractCard {
            contentItem: ColumnLayout {
                spacing: 4

                RowLayout {
                    spacing: 8
                    Layout.fillWidth: true

                    Kirigami.Icon {
                        source: model.isUnread ? "mail-mark-unread" : "mail-read"
                        implicitWidth: 20
                        implicitHeight: 20
                    }

                    Label {
                        text: model.sender
                        font.bold: model.isUnread
                        font.pixelSize: 14
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    Label {
                        text: Qt.formatDateTime(model.date, "MMM d")
                        font.pixelSize: 12
                        color: Kirigami.Theme.disabledTextColor
                    }
                }

                Label {
                    text: model.subject
                    font.bold: model.isUnread
                    font.pixelSize: 13
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Label {
                    text: model.snippet
                    font.pixelSize: 12
                    color: Kirigami.Theme.disabledTextColor
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    maximumLineCount: 1
                }
            }

            onClicked: inboxView.messageDetailsRequested(model.messageId)
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            visible: messageList.count === 0
            text: "No emails yet"
        }
    }
}
