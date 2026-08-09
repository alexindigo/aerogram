import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import Aerogram

Kirigami.Page {
    id: inboxView
    title: "Inbox"
    padding: 0

    signal messageSelected(string messageId)
    signal attachmentSaveRequested(string messageId, int partIndex, string path)

    // Set from main.qml's wiring of accountController.attachmentSaved.
    property string saveStatusText: ""

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---------------------------------------------------------
        // Left: message list (sender identity block per row)
        // ---------------------------------------------------------
        ListView {
            id: messageList
            Layout.preferredWidth: 380
            Layout.fillHeight: true
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

                onClicked: inboxView.messageSelected(model.messageId)
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                visible: messageList.count === 0
                text: "No messages"
            }
        }

        // Vertical separator
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            color: Kirigami.Theme.disabledTextColor
            opacity: 0.3
        }

        // ---------------------------------------------------------
        // Right: reading pane (sticky header, scrolling body, sticky
        // attachment chips at the bottom)
        // ---------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                visible: accountController.activeMessageId.length === 0
                text: "Inbox zero"
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 8
                visible: accountController.activeMessageId.length > 0

                // -- sticky header --
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    IdentityBlock {
                        sender: accountController.activeMessage.sender || ""
                    }

                    Label {
                        text: accountController.activeMessage.sender || ""
                        font.pixelSize: 13
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    Label {
                        text: accountController.activeMessage.date
                              ? Qt.formatDateTime(accountController.activeMessage.date,
                                                  "MMM d, hh:mm")
                              : ""
                        font.pixelSize: 12
                        color: Kirigami.Theme.disabledTextColor
                    }
                }

                Label {
                    text: accountController.activeMessage.subject || ""
                    font.bold: true
                    font.pixelSize: 16
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumHeight: 1
                    color: Kirigami.Theme.disabledTextColor
                    opacity: 0.3
                }

                // -- scrolling body --
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    Label {
                        text: accountController.activeMessageBody
                        wrapMode: Text.Wrap
                        width: inboxView.width - 380 - 40
                    }
                }

                // -- sticky attachment chips bar (only when present) --
                Rectangle {
                    visible: attachmentBar.visible
                    Layout.fillWidth: true
                    Layout.minimumHeight: 1
                    color: Kirigami.Theme.disabledTextColor
                    opacity: 0.3
                }

                Flow {
                    id: attachmentBar
                    visible: accountController.activeMessageAttachments.length > 0
                    Layout.fillWidth: true
                    spacing: 8

                    Repeater {
                        model: accountController.activeMessageAttachments

                        delegate: Rectangle {
                            height: 30
                            width: chipRow.implicitWidth + 20
                            radius: 15
                            color: Kirigami.Theme.alternateBackgroundColor
                            border.width: 1
                            border.color: Kirigami.Theme.disabledTextColor

                            RowLayout {
                                id: chipRow
                                anchors.centerIn: parent
                                spacing: 6

                                AeroIcon {
                                    name: "paperclip"
                                    implicitWidth: 14
                                    implicitHeight: 14
                                }

                                Label {
                                    text: modelData.filename + "  " +
                                          Math.max(1, Math.round(modelData.size / 1024)) + " KB"
                                    font.pixelSize: 11
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    saveDialog.pendingIndex = modelData.index
                                    saveDialog.currentFile = modelData.filename
                                    saveDialog.open()
                                }
                            }
                        }
                    }
                }

                Label {
                    visible: text.length > 0
                    text: inboxView.saveStatusText
                    font.pixelSize: 11
                    color: Kirigami.Theme.disabledTextColor
                }
            }
        }
    }

    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        property int pendingIndex: -1
        onAccepted: {
            // strip file:// AND decode percent-encoding (%20 etc.)
            const path = decodeURIComponent(String(selectedFile).replace(/^file:\/\//, ""))
            inboxView.attachmentSaveRequested(accountController.activeMessageId,
                                              pendingIndex, path)
        }
    }
}
