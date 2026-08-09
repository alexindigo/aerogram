import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: inboxView
    title: "Inbox"
    padding: 0

    signal messageSelected(string messageId)

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---------------------------------------------------------
        // Left: message list
        // ---------------------------------------------------------
        ListView {
            id: messageList
            Layout.preferredWidth: 380
            Layout.fillHeight: true
            model: accountController.messageListModel
            spacing: 4
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

                        Kirigami.Icon {
                            source: "mail-attachment"
                            implicitWidth: 16
                            implicitHeight: 16
                            visible: model.hasAttachments
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
        // Right: reading pane
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

                Label {
                    text: accountController.activeMessage.subject || ""
                    font.bold: true
                    font.pixelSize: 16
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.fillWidth: true
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

                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumHeight: 1
                    color: Kirigami.Theme.disabledTextColor
                    opacity: 0.3
                }

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

                // Attachments
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: accountController.activeMessageAttachments.length > 0
                    spacing: 4

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.minimumHeight: 1
                        color: Kirigami.Theme.disabledTextColor
                        opacity: 0.3
                    }

                    Label {
                        text: "Attachments"
                        font.bold: true
                        font.pixelSize: 12
                    }

                    Repeater {
                        model: accountController.activeMessageAttachments

                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Kirigami.Icon {
                                source: "mail-attachment"
                                implicitWidth: 16
                                implicitHeight: 16
                            }

                            Label {
                                text: modelData.filename + "  (" +
                                      Math.max(1, Math.round(modelData.size / 1024)) + " KB)"
                                font.pixelSize: 12
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }

                            Button {
                                text: "Save"
                                flat: true
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
                    id: saveStatus
                    text: ""
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
            const path = String(selectedFile).replace(/^file:\/\//, "")
            accountController.saveAttachment(accountController.activeMessageId,
                                             pendingIndex, path)
        }
    }

    Connections {
        target: accountController
        function onAttachmentSaved(ok, messageId, path) {
            saveStatus.text = ok ? "Saved to " + path : "Save failed"
        }
    }
}
