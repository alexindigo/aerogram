import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Aerogram

// Email conversations panel — the middle panel. For email accounts the
// "conversation" is the folder (INBOX for now); this lists its messages.
// Independent component: reads the controller model, emits semantic
// signals, knows nothing about the messages panel or the sidebar.
Item {
    id: conversationsPanel

    signal messageSelected(string messageId)

    ListView {
        id: messageList
        anchors.fill: parent
        model: accountController.messageListModel
        spacing: 4
        clip: true

        delegate: Kirigami.AbstractCard {
            contentItem: RowLayout {
                spacing: 10

                IdentityBlock {
                    sender: model.sender
                    Layout.alignment: Qt.AlignTop
                }

                ColumnLayout {
                    spacing: 3
                    Layout.fillWidth: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            text: model.sender
                            font.bold: model.isUnread
                            font.pixelSize: 13
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Label {
                            text: Qt.formatDateTime(model.date, "MMM d")
                            font.pixelSize: 11
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

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Rectangle {
                            width: 7
                            height: 7
                            radius: 4
                            color: Kirigami.Theme.highlightColor
                            visible: model.isUnread
                        }

                        Label {
                            text: model.snippet
                            font.pixelSize: 12
                            color: Kirigami.Theme.disabledTextColor
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            maximumLineCount: 1
                        }

                        AeroIcon {
                            name: "paperclip"
                            implicitWidth: 14
                            implicitHeight: 14
                            visible: model.hasAttachments
                        }
                    }
                }
            }

            onClicked: conversationsPanel.messageSelected(model.messageId)
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            visible: messageList.count === 0
            text: "No messages"
        }
    }
}
