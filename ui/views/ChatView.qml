import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: chatView
    title: "Chats"

    signal chatSelected(string conversationId)

    ListView {
        id: messageList
        anchors.fill: parent
        model: accountController.conversationListModel
        spacing: 6
        clip: true

        delegate: Rectangle {
            id: bubble
            width: messageList.width - 16
            height: bubbleLayout.implicitHeight + 16
            radius: 12
            color: model.kind === "chat"
                ? Kirigami.Theme.highlightColor
                : Kirigami.Theme.alternateBackgroundColor
            x: model.kind === "chat" ? 8 : 24

            ColumnLayout {
                id: bubbleLayout
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 2

                RowLayout {
                    spacing: 6
                    Layout.fillWidth: true

                    Label {
                        text: model.name + (model.unreadCount > 0
                                            ? " (" + model.unreadCount + ")" : "")
                        font.bold: model.unreadCount > 0
                        font.pixelSize: 13
                    }

                    Label {
                        text: model.accountLabel
                        font.pixelSize: 10
                        color: Kirigami.Theme.disabledTextColor
                        visible: model.accountLabel.length > 0
                    }

                    Label {
                        text: Qt.formatDateTime(model.lastActivity, "MMM d hh:mm")
                        font.pixelSize: 11
                        color: Kirigami.Theme.disabledTextColor
                        Layout.alignment: Qt.AlignRight
                    }
                }

                Label {
                    text: model.preview
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    visible: model.preview.length > 0
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: chatView.chatSelected(model.conversationId)
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            visible: messageList.count === 0
            text: "No conversations yet"
        }
    }
}
