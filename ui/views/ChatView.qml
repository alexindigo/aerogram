import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: chatView
    title: "Chats"

    signal chatSelected(string chatId)
    signal composeMessageRequested()
    signal groupInfoRequested(string chatId)

    ListView {
        id: messageList
        anchors.fill: parent
        model: accountController.chatListModel
        spacing: 6
        clip: true

        delegate: Rectangle {
            id: bubble
            width: messageList.width - 16
            height: bubbleLayout.implicitHeight + 16
            radius: 12
            color: model.isGroup
                ? Kirigami.Theme.highlightColor
                : Kirigami.Theme.alternateBackgroundColor
            x: model.isGroup ? 8 : 24

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
                        text: model.senderName
                        font.bold: true
                        font.pixelSize: 13
                    }

                    Label {
                        text: Qt.formatTime(model.timestamp, "hh:mm")
                        font.pixelSize: 11
                        color: Kirigami.Theme.disabledTextColor
                        Layout.alignment: Qt.AlignRight
                    }
                }

                Label {
                    text: model.messageText
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: chatView.chatSelected(model.chatId)
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            visible: messageList.count === 0
            text: "No chats yet"
        }
    }
}
